/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>
#include <wtsapi32.h>
#include <sas.h>
#include <shlwapi.h>
#include <strsafe.h>
#include "common.h"

#include <log.h>
#include <config.h>
#include <qubes-io.h>

#define SERVICE_NAME L"QgaWatchdog"

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;

// Set when the SCM tells us the service (or the machine) is going down. See
// AgentRespawnPointless below.
volatile LONG g_ServiceStopping = 0;

// Manual-reset: set by ControlHandlerEx on STOP/SHUTDOWN, ends WatchdogThread. Before this
// existed the handler reported STOPPED while the respawn loop was still live, so Stop-Service
// returned and the loop could relaunch the agent under an installer that had just killed it.
static HANDLE g_StopEvent = NULL;
// Auto-reset: set on SERVICE_CONTROL_SESSIONCHANGE (console connect / logon) so the watchdog
// starts the agent when the console session arrives instead of rediscovering it by polling.
static HANDLE g_SessionEvent = NULL;

// StartTargetProcess: nothing was launched because there is no console session yet. Distinct
// from a launch failure so the caller does not stamp a start it never made.
#define START_SKIPPED_NO_SESSION ERROR_NO_SUCH_LOGON_SESSION

void WINAPI ServiceMain(IN DWORD argc, IN WCHAR *argv[]);
DWORD WINAPI ControlHandlerEx(IN DWORD controlCode, IN DWORD eventType, IN void *eventData, IN void *context);

// Entry point.
int wmain(int argc, WCHAR *argv[])
{
    SERVICE_TABLE_ENTRY	serviceTable[] = {
        { SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    StartServiceCtrlDispatcher(serviceTable);
    return ERROR_SUCCESS;
}

// DETECTION ONLY. Finds a process by exe name so the watchdog can tell that an agent it did not
// start is alive (and say so, QGAWDFOREIGN). The pid it returns is never adopted as our agent and
// never terminated: the owner's rule (2026-10-03) is that a component touches only processes it
// started, by handle - a process found by name is ANY process with that name.
BOOL IsProcessRunning(IN const WCHAR *exeName, OUT DWORD *processId OPTIONAL, OUT DWORD *sessionId OPTIONAL)
{
    WTS_PROCESS_INFO *processInfo = NULL;
    DWORD count = 0, i;
    HANDLE shutdownEvent = NULL;
    BOOL found = FALSE;

    if (!WTSEnumerateProcesses(WTS_CURRENT_SERVER, 0, 1, &processInfo, &count))
    {
        win_perror("WTSEnumerateProcesses");
        goto cleanup;
    }

    for (i = 0; i < count; i++)
    {
        if (0 == _wcsnicmp(exeName, processInfo[i].pProcessName, wcslen(exeName))) // match
        {
            if (processId)
                *processId = processInfo[i].ProcessId;
            if (sessionId)
                *sessionId = processInfo[i].SessionId;
            LogVerbose("%s: PID %d, session %d", processInfo[i].pProcessName, processInfo[i].ProcessId, processInfo[i].SessionId);
            found = TRUE;
            break;
        }
    }

cleanup:
    if (processInfo)
        WTSFreeMemory(processInfo);
    return found;
}

// Starts the process as SYSTEM in currently active console session.
// Returns ERROR_SUCCESS with *processHandle/*processId set when a process was created,
// START_SKIPPED_NO_SESSION when there is no console session to start it in (nothing was
// launched), or the Win32 error of the failing call.
DWORD StartTargetProcess(IN WCHAR *exePath, OUT HANDLE *processHandle, OUT DWORD *processId) // non-const because it can be modified by CreateProcess*
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD currenttSessionId, consoleSessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();
    DWORD status;

    *processHandle = NULL;
    *processId = 0;

    consoleSessionId = WTSGetActiveConsoleSessionId();
    if (consoleSessionId == 0xFFFFFFFF) // disconnected or changing
    {
        // Distinct code, not ERROR_SUCCESS: the caller used to treat this skip as a launch, find
        // no agent a second later, and count it as a crash - backoff 1->2->...->60 s during early
        // boot plus a "grant-table exhaustion, needs a reboot" warning for a transient.
        LogDebug("console session is 0x%x, skipping", consoleSessionId);
        return START_SKIPPED_NO_SESSION;
        // we'll launch gui agent when the console connects to a session again
    }

    // Get access token from ourselves. Both tokens are closed before returning on every path:
    // they used to leak on each launch (audit 2026-09-08) - bounded only by the 60 s backoff,
    // but a service that respawns a crashing agent for days accumulated two handles per attempt.
    // (GetCurrentProcess() is a pseudo-handle; it is not closed.)
    if (!OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken))
    {
        return win_perror("OpenProcessToken");
    }
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &currenttSessionId, sizeof(currenttSessionId), &size);
    LogDebug("current session: %d, console session: %d", currenttSessionId, consoleSessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
    {
        status = win_perror("DuplicateTokenEx");
        CloseHandle(currentToken);
        return status != ERROR_SUCCESS ? status : ERROR_GEN_FAILURE;
    }
    CloseHandle(currentToken);

    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &consoleSessionId, sizeof(consoleSessionId)))
    {
        status = win_perror("SetTokenInformation(TokenSessionId)");
        CloseHandle(newToken);
        return status != ERROR_SUCCESS ? status : ERROR_GEN_FAILURE;
    }

    LogInfo("Running process '%s' in session %d", exePath, consoleSessionId);
    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    // No need to set desktop here, gui agent attaches to the input desktop anyway,
    // and hardcoding this to winlogon is wrong.
    if (!CreateProcessAsUser(newToken, NULL, exePath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        status = win_perror("CreateProcessAsUser");
        CloseHandle(newToken);
        return status != ERROR_SUCCESS ? status : ERROR_GEN_FAILURE; // never report a failed launch as success
    }
    CloseHandle(newToken);   // the new process holds its own reference

    // Keep the process handle: the watchdog waits on it, so the agent's exit is seen the instant
    // it happens instead of on the next 1 s name-match enumeration (or 60 s when backed off).
    *processHandle = pi.hProcess;
    *processId = pi.dwProcessId;
    CloseHandle(pi.hThread);

    return ERROR_SUCCESS;
}

// DO NOT RESPAWN INTO A MACHINE THAT IS SHUTTING DOWN (2026-08-28).
//
// At shutdown the session-1 agent is torn down first, so this loop sees "not running", restarts
// it, that instance dies too (the daemon is gone: "QioReadBuffer ... The pipe has been ended"),
// and the loop restarts it again - all in the last seconds before the SCM stops us. The result is
// a cluster of two dead agents plus "The guest has NO GUI while this lasts" written into the log
// on every single normal shutdown.
//
// That noise is not cosmetic: it is indistinguishable from a real failure, and it sent this
// project chasing a non-existent boot-time double-spawn race. GWeck's field log (posts 96-98)
// shows the identical cluster at uptime 412 s and 414 s followed by a reboot - a shutdown, not a
// boot. Whatever we suppress here, we LOG the signals we looked at, so the next occurrence says
// which of them actually fired instead of leaving the next reader to guess as I did.
//
// Acted on: the SCM control (STOP/SHUTDOWN/PRESHUTDOWN) and SM_SHUTTINGDOWN. The WTS console
// session state is recorded but NOT acted on - at the sign-in screen (pre-logon) the session is
// legitimately not "active" and the agent must still be started there.
static BOOL AgentRespawnPointless(OUT WCHAR *why, IN size_t whyChars)
{
    BOOL shuttingDown = (GetSystemMetrics(SM_SHUTTINGDOWN) != 0);
    BOOL serviceStopping = (InterlockedCompareExchange(&g_ServiceStopping, 0, 0) != 0);

    DWORD sessionId = WTSGetActiveConsoleSessionId();
    int state = -1;
    WTS_CONNECTSTATE_CLASS *sessionState = NULL;
    DWORD size = 0;
    if (sessionId != 0xFFFFFFFF &&
        WTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSConnectState,
            (LPWSTR*)&sessionState, &size) &&
        sessionState && size >= sizeof(*sessionState))
    {
        state = (int)*sessionState;
    }
    if (sessionState)
        WTSFreeMemory(sessionState);

    StringCchPrintf(why, whyChars, L"servicestop=%d sm_shuttingdown=%d console=0x%x wtsstate=%d",
        serviceStopping ? 1 : 0, shuttingDown ? 1 : 0, sessionId, state);

    return serviceStopping || shuttingDown;
}

// How long the agent gets to leave on its own stop request before it is terminated, how long the
// termination is given to take effect, and the STOP_PENDING wait hint the control handler reports
// so the SCM allows for their sum plus the thread join (ServiceMain bounds the join by the same).
#define AGENT_STOP_GRACE_MS 10000
#define AGENT_KILL_WAIT_MS 5000
#define STOP_WAIT_HINT_MS 30000

// THE OWNER STOPS ITS CHILD (owner's rule, 2026-10-03: a component touches only processes it
// started, by handle; nothing is ever killed or adopted by process NAME). Until this existed a
// service stop ended only the respawn loop and LEFT THE AGENT RUNNING - which is why every
// installer path then had to find gui-agent.exe BY NAME and kill it, together with the helpers it
// launches, with its framebuffer grants still held. Now the stop is: ask the agent to exit through
// its own stop event (QGA_SHUTDOWN - its exit path revokes the grants, which process death never
// does, and tells its helpers to leave), wait on the HANDLE we hold from CreateProcessAsUser, and
// only if it is still alive after AGENT_STOP_GRACE_MS end it with TerminateProcess on that handle.
// Every outcome is logged with the pid and the exit code; ServiceMain reports STOPPED only after
// this has returned. Closes the handle.
static void StopOwnAgent(IN HANDLE agentProcess, IN DWORD agentPid, IN const WCHAR *exeName)
{
    HANDLE shutdownEvent;
    DWORD wait;
    DWORD exitCode = 0xFFFFFFFF;

    shutdownEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, QGA_SHUTDOWN_EVENT_NAME);
    if (shutdownEvent)
    {
        if (SetEvent(shutdownEvent))
            LogInfo("service stopping: asked '%s' (PID %u) to exit via %s, waiting up to %u ms on its handle",
                exeName, agentPid, QGA_SHUTDOWN_EVENT_NAME, AGENT_STOP_GRACE_MS);
        else
            win_perror("SetEvent(" QGA_SHUTDOWN_EVENT_NAME L")");
        CloseHandle(shutdownEvent);
    }
    else
    {
        // The agent creates the event early in its init, so this is an agent that has not got that
        // far (or is already gone). There is nothing to ask; the wait below decides.
        win_perror("OpenEvent(" QGA_SHUTDOWN_EVENT_NAME L")");
        LogWarning("service stopping: %s is not open, '%s' (PID %u) cannot be asked to exit - "
            L"waiting on its handle, then terminating it", QGA_SHUTDOWN_EVENT_NAME, exeName, agentPid);
    }

    wait = WaitForSingleObject(agentProcess, AGENT_STOP_GRACE_MS);
    if (wait != WAIT_OBJECT_0)
    {
        LogWarning("service stopping: '%s' (PID %u) is still running %u ms after the exit request (wait 0x%x) - "
            L"terminating it by handle; its framebuffer grants stay held until dom0 drops them",
            exeName, agentPid, AGENT_STOP_GRACE_MS, wait);
        if (!TerminateProcess(agentProcess, 1))
            win_perror("TerminateProcess(agent)");
        wait = WaitForSingleObject(agentProcess, AGENT_KILL_WAIT_MS);
        if (wait != WAIT_OBJECT_0)
            LogError("service stopping: '%s' (PID %u) did not exit within %u ms of TerminateProcess (wait 0x%x) - "
                L"reporting STOPPED with the agent still present", exeName, agentPid, AGENT_KILL_WAIT_MS, wait);
    }
    if (wait == WAIT_OBJECT_0)
    {
        if (!GetExitCodeProcess(agentProcess, &exitCode))
            exitCode = 0xFFFFFFFF;
        LogInfo("service stopping: '%s' (PID %u) is gone, exit code 0x%x", exeName, agentPid, exitCode);
    }
    CloseHandle(agentProcess);
}

// Restarts gui agent in active session if it's dead for too long.
DWORD WINAPI WatchdogThread(void *param)
{
    WCHAR* cmdline = (WCHAR*) param;

    PathUnquoteSpaces(cmdline);
    WCHAR* exeName = PathFindFileName(cmdline);

    LogDebug("cmdline: '%s', exe: '%s'", cmdline, exeName);

    // BACK OFF WHEN THE AGENT DIES IMMEDIATELY. The loop used to respawn once per second for
    // ever, which is right for a crash but wrong for a failure the agent cannot recover from by
    // being run again - measured 2026-08-15: with the Xen grant table exhausted the agent exits
    // during vchan init (0x5aa) and the watchdog respawned it every second indefinitely, each
    // attempt writing a fresh 0-byte log and asking for grants that are not there. A guest in
    // that state answers qrexec and has no GUI at all, and hammering it only makes the table
    // situation worse. Healthy restarts are unaffected: the delay only grows for an agent that
    // dies again within QUICK_DEATH_MS of being started, and resets the moment one survives.
    #define QUICK_DEATH_MS 10000
    #define BACKOFF_MAX_MS 60000
    DWORD backoffMs = 1000;
    DWORD quickDeaths = 0;
    ULONGLONG startedAt = 0;
    // A launch attempt that failed in CreateProcessAsUser is backed off like a quick death but
    // reported as what it is; the old code folded it into the "died within 10 s" grant-table text.
    BOOL lastLaunchFailed = FALSE;
    DWORD lastLaunchError = ERROR_SUCCESS;
    // Handle of the agent WE STARTED - the only process this service owns. While we hold one it is
    // the liveness oracle: the loop sleeps on it and wakes the moment the process exits, and the
    // stop path below ends it (StopOwnAgent). Without it the loop enumerated processes every
    // second by name prefix, which both detected a death late and could adopt a same-named
    // stranger as "running" without a word in the log.
    HANDLE agentProcess = NULL;
    DWORD agentPid = 0;
    // A same-named process this service did NOT start. Never adopted (owner's rule 2026-10-03:
    // nothing is killed or adopted by NAME - until then it was OpenProcess'ed into agentProcess
    // and supervised as ours, which the stop path would now terminate), never stopped; only
    // WAITED ON, through this handle, so that no second agent is started while it lives (two
    // agents fight for the vchan and the loser dies - main.c, the single-instance mutex), and
    // reported once per episode (QGAWDFOREIGN).
    HANDLE foreignProcess = NULL;
    DWORD foreignPid = 0;
    BOOL foreignLogged = FALSE;
    BOOL waitingForSession = FALSE;

    while (TRUE)
    {
        HANDLE waitHandles[3];
        DWORD waitCount = 0;
        DWORD timeoutMs;
        DWORD wait;
        BOOL running;
        BOOL exitedQuickly = FALSE;

        waitHandles[waitCount++] = g_StopEvent;
        waitHandles[waitCount++] = g_SessionEvent;
        if (agentProcess)
            waitHandles[waitCount++] = agentProcess;
        else if (foreignProcess)
            waitHandles[waitCount++] = foreignProcess;   // waited on, never acted on

        // Holding a healthy agent's handle - or a stranger's, which is only waited out - there is
        // nothing to poll for: its exit wakes us. The timeout is only the respawn backoff, plus the
        // QUICK_DEATH_MS survival check while a backoff is in force.
        timeoutMs = ((agentProcess || foreignProcess) && quickDeaths == 0) ? INFINITE : backoffMs;

        wait = WaitForMultipleObjects(waitCount, waitHandles, FALSE, timeoutMs);
        if (wait == WAIT_OBJECT_0) // stop event
        {
            LogInfo("service stop requested, watchdog thread exiting");
            break;
        }
        if (wait == WAIT_FAILED)
        {
            win_perror("WaitForMultipleObjects");
            Sleep(backoffMs); // do not spin
            continue;
        }
        if (wait == WAIT_OBJECT_0 + 1)
            LogInfo("console session changed, checking the agent");
        if (agentProcess && wait == WAIT_OBJECT_0 + 2)
        {
            DWORD exitCode = 0;
            WCHAR why[128] = L"";
            if (!GetExitCodeProcess(agentProcess, &exitCode))
                exitCode = 0xFFFFFFFF;
            // A DYING AGENT IS A MAJOR ERROR, not a warning (owner, 2026-10-03). This service did
            // not ask it to exit: a stop the service asked for takes the g_StopEvent path above and
            // StopOwnAgent, never this line. So an exit seen here is the agent going away under us -
            // logged at ERROR with the exit code, whatever the relaunch below then does. The one
            // exit that is not a death: the machine is going down and the session-1 agent is torn
            // down before the SCM reaches us (the measured shutdown cluster, AgentRespawnPointless
            // above) - that stays INFO, with the signals that said so, as the respawn path does.
            if (AgentRespawnPointless(why, RTL_NUMBER_OF(why)))
                LogInfo("Process '%s' (PID %u) exited with code 0x%x while the system is going down (%s)",
                    exeName, agentPid, exitCode, why);
            else
                LogError("Process '%s' (PID %u) exited with code 0x%x without this service asking it to - "
                    L"the agent DIED (%s)", exeName, agentPid, exitCode, why);
            CloseHandle(agentProcess);
            agentProcess = NULL;
            agentPid = 0;
            // Judge "quick" at the moment of death, not after the delay below (a 16 s+ delay made
            // every death look old and reset the backoff). And KEEP the delay: the handle wakes
            // us the instant the agent exits, so without it a quick death would be respawned
            // immediately and the backoff would never hold - exactly the once-a-second grant-table
            // hammering it was added to stop.
            exitedQuickly = (startedAt != 0 && GetTickCount64() - startedAt < QUICK_DEATH_MS);
            if (exitedQuickly &&
                WaitForSingleObject(g_StopEvent, backoffMs) == WAIT_OBJECT_0)
            {
                LogInfo("service stop requested, watchdog thread exiting");
                break;
            }
        }

        // A stranger we were waiting out has exited: say so and let the loop start our own.
        if (!agentProcess && foreignProcess && wait == WAIT_OBJECT_0 + 2)
        {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(foreignProcess, &exitCode))
                exitCode = 0xFFFFFFFF;
            LogInfo("QGAWDFOREIGN the '%s' this service did not start (PID %u) has exited with code 0x%x - "
                L"starting our own", exeName, foreignPid, exitCode);
            CloseHandle(foreignProcess);
            foreignProcess = NULL;
            foreignPid = 0;
            foreignLogged = FALSE;
        }

        // Is the gui agent running? Our handle is authoritative. Without one (service start, or a
        // restart while an agent we did not launch is alive) a same-named process may exist. It is
        // NOT ours: it is never adopted and never stopped (the owner's rule - until 2026-10-03 this
        // loop OpenProcess'ed it into agentProcess and supervised it as the agent it started). It is
        // reported once per episode as an anomaly, with pid and session, and waited out through a
        // separate handle so that no second agent is started while it lives. If it cannot be opened
        // even for SYNCHRONIZE the loop falls back to the backoff-paced name poll for it.
        running = (agentProcess != NULL);
        if (!running && foreignProcess)
        {
            if (WaitForSingleObject(foreignProcess, 0) == WAIT_OBJECT_0)
            {
                // Exited between the wait above and here; the normal path is the block before.
                CloseHandle(foreignProcess);
                foreignProcess = NULL;
                foreignPid = 0;
                foreignLogged = FALSE;
            }
            else
                running = TRUE;
        }
        if (!running)
        {
            DWORD pid = 0, sid = 0;
            if (IsProcessRunning(exeName, &pid, &sid))
            {
                running = TRUE;
                foreignProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (foreignProcess)
                    foreignPid = pid;
                else
                    win_perror("OpenProcess(foreign agent, SYNCHRONIZE)");
                if (!foreignLogged)
                {
                    LogError("QGAWDFOREIGN '%s' (PID %u, session %u) is running but was NOT started by this service - "
                        L"not adopted, not stopped, and no second agent is started while it lives (%s)",
                        exeName, pid, sid,
                        foreignProcess ? L"waiting on its handle" : L"it cannot be opened, polling for it by name");
                    foreignLogged = TRUE;
                }
            }
            else
            {
                foreignLogged = FALSE;
            }
        }

        if (!running)
        {
            WCHAR why[128] = L"";
            HANDLE newProcess = NULL;
            DWORD newPid = 0;
            DWORD status;

            if (AgentRespawnPointless(why, RTL_NUMBER_OF(why)))
            {
                LogInfo("Process '%s' not running and the system is going down (%s) - "
                    L"not restarting it", exeName, why);
                continue;
            }

            // No console session yet (early boot, or 0xFFFFFFFF while it is changing): nothing
            // can be started, so nothing is recorded - this is not an agent death and must not
            // grow the backoff. g_SessionEvent wakes us when the console connects.
            if (WTSGetActiveConsoleSessionId() == 0xFFFFFFFF)
            {
                if (!waitingForSession)
                    LogInfo("Process '%s' not running and there is no console session yet - "
                        L"will start it when one connects (%s)", exeName, why);
                waitingForSession = TRUE;
                continue;
            }
            waitingForSession = FALSE;

            // Fast failure = the agent we waited on died quickly, or the previous launch itself
            // failed (nothing ran, so "ran long enough to be healthy" cannot apply), or - on the
            // poll-by-name path only - the last start is recent.
            if (exitedQuickly || lastLaunchFailed ||
                (startedAt != 0 && GetTickCount64() - startedAt < QUICK_DEATH_MS))
            {
                quickDeaths++;
                if (backoffMs < BACKOFF_MAX_MS)
                {
                    backoffMs *= 2;
                    if (backoffMs > BACKOFF_MAX_MS)
                        backoffMs = BACKOFF_MAX_MS;
                }
                // ERROR, not warning (owner, 2026-10-03): a launch that fails and an agent that
                // dies on arrival are the guest having no GUI, and the backoff only spaces the
                // attempts out - nothing here is recovering.
                if (lastLaunchFailed)
                    LogError("Starting process '%s' failed (error 0x%x), %u time(s) in a row - "
                        L"backing off to %u ms (%s). The guest has NO GUI while this lasts.",
                        exeName, lastLaunchError, quickDeaths, backoffMs, why);
                else
                    LogError("Process '%s' died within %u ms of starting, %u time(s) in a row - "
                        L"backing off to %u ms (%s). The guest has NO GUI while this lasts; the agent "
                        L"log names the failure (grant-table exhaustion, 0x5aa, needs a reboot).",
                        exeName, QUICK_DEATH_MS, quickDeaths, backoffMs, why);
            }
            else
            {
                if (quickDeaths != 0)
                    LogInfo("Process '%s' had been failing fast; it last ran long enough to count "
                        L"as healthy, restart delay reset to 1000 ms", exeName);
                quickDeaths = 0;
                backoffMs = 1000;
                LogWarning("Process '%s' not running, restarting it (%s)", exeName, why);
            }

            status = StartTargetProcess(cmdline, &newProcess, &newPid);
            if (status == START_SKIPPED_NO_SESSION)
            {
                // Session vanished between our check and the launch: not an attempt, record nothing.
                continue;
            }
            startedAt = GetTickCount64();
            if (status == ERROR_SUCCESS && newProcess)
            {
                agentProcess = newProcess;
                agentPid = newPid;
                lastLaunchFailed = FALSE;
                lastLaunchError = ERROR_SUCCESS;
            }
            else
            {
                // Real launch failure (already logged by win_perror): backed off via startedAt on
                // the next tick, with the actual error in the text.
                lastLaunchFailed = TRUE;
                lastLaunchError = status;
            }
        }
        else if (quickDeaths != 0 && startedAt != 0 &&
                 GetTickCount64() - startedAt >= QUICK_DEATH_MS)
        {
            // Survived the window - stop punishing it.
            LogInfo("Process '%s' has been up for %u ms, restart delay reset", exeName, QUICK_DEATH_MS);
            quickDeaths = 0;
            backoffMs = 1000;
        }
    }

    // THE OWNER STOPS ITS CHILD: the agent this service started goes down with the service, by
    // handle (StopOwnAgent logs every outcome). A stranger that was only waited out is left exactly
    // as it was - it is not ours - and that is said.
    if (agentProcess)
        StopOwnAgent(agentProcess, agentPid, exeName);
    if (foreignProcess)
    {
        LogError("QGAWDFOREIGN service stopping while '%s' (PID %u), which this service did not start, is running - "
            L"left running, not ours", exeName, foreignPid);
        CloseHandle(foreignProcess);
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI EventsThread(void *param)
{
    HANDLE events[1];
    DWORD signaledEvent = 2;

    LogDebug("start");

    // Default security for the SAS event, only SYSTEM processes can signal it.
    events[0] = CreateEvent(NULL, FALSE, FALSE, QGA_SAS_EVENT_NAME);

    while (TRUE)
    {
        signaledEvent = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) - WAIT_OBJECT_0;

        switch (signaledEvent)
        {
        case 0: // SAS event
            LogInfo("SAS event signaled");
            SendSAS(FALSE); // calling as service
            break;

        default:
            LogWarning("Wait failed, result 0x%x", signaledEvent + WAIT_OBJECT_0);
        }
    }

    return ERROR_SUCCESS;
}

void WINAPI ServiceMain(IN DWORD argc, IN WCHAR *argv[])
{
    WCHAR moduleName[CFG_MODULE_MAX];
    HANDLE workerHandle = NULL;
    HANDLE watchdogHandle = NULL;
    DWORD status;
    BOOL cleanStop = FALSE;

    WCHAR* cmdline = malloc(MAX_PATH_LONG_WSIZE);
    if (!cmdline)
        goto cleanup;

    // Read the registry configuration.
    CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));
    status = CfgReadString(moduleName, REG_CONFIG_AGENT_PATH_VALUE, cmdline, MAX_PATH_LONG, NULL);
    if (ERROR_SUCCESS != status)
    {
        win_perror("CfgReadString(" REG_CONFIG_AGENT_PATH_VALUE L")");
        goto cleanup;
    }

    // Created before the control handler is registered so a STOP arriving early cannot be missed.
    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_SessionEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_StopEvent || !g_SessionEvent)
    {
        win_perror("CreateEvent");
        goto cleanup;
    }

    g_Status.dwServiceType = SERVICE_WIN32;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    // PRESHUTDOWN arrives BEFORE the ordinary shutdown notifications, which is the only chance to
    // know the machine is going down early enough to stop respawning the agent into it.
    // SESSIONCHANGE tells the watchdog when the console session arrives (see g_SessionEvent).
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
        SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    g_Status.dwWin32ExitCode = 0;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 0;
    g_StatusHandle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, ControlHandlerEx, NULL);
    if (g_StatusHandle == 0)
    {
        win_perror("RegisterServiceCtrlHandlerEx");
        goto cleanup;
    }

    LogDebug("Starting event thread");
    workerHandle = CreateThread(NULL, 0, EventsThread, NULL, 0, NULL);
    if (!workerHandle)
    {
        win_perror("CreateThread(events)");
        goto cleanup;
    }

    LogDebug("Starting watchdog thread");
    watchdogHandle = CreateThread(NULL, 0, WatchdogThread, cmdline, 0, NULL);
    if (!watchdogHandle)
    {
        win_perror("CreateThread(watchdog)");
        goto cleanup;
    }

    // RUNNING only once both threads exist; a thread-creation failure is then a visible start
    // failure (START_PENDING -> STOPPED with the error) instead of a service that says RUNNING
    // while doing nothing.
    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    // The watchdog thread exits on g_StopEvent (set by ControlHandlerEx on STOP/SHUTDOWN/
    // PRESHUTDOWN, which report STOP_PENDING) - after stopping the agent it started (StopOwnAgent:
    // the exit request, AGENT_STOP_GRACE_MS, then TerminateProcess and AGENT_KILL_WAIT_MS). STOPPED
    // is reported here, after it has actually exited, so a caller whose Stop-Service returned is
    // guaranteed no further agent launch from this service AND no agent of this service's left
    // behind. The join is BOUNDED once the stop is requested, by STOP_WAIT_HINT_MS (what the
    // handler told the SCM to allow): a service that never reaches a terminal state is waited out
    // for the full preshutdown timeout (180 s by default) and logged as Event 7043 - observed on
    // this rig (2026-08-29) and the reason PRESHUTDOWN once reported STOPPED straight from the
    // handler. Every wait in the thread is bounded, so hitting this bound is an anomaly worth the
    // error line; the stop proceeds regardless.
    {
        HANDLE joinHandles[2] = { watchdogHandle, g_StopEvent };
        DWORD join = WaitForMultipleObjects(2, joinHandles, FALSE, INFINITE);
        if (join == WAIT_OBJECT_0 + 1)
        {
            join = WaitForSingleObject(watchdogHandle, STOP_WAIT_HINT_MS);
            if (join != WAIT_OBJECT_0)
                LogError("watchdog thread did not exit within %u ms of the stop request (wait 0x%x) - "
                    L"reporting STOPPED anyway; the respawn loop is disarmed by g_ServiceStopping, "
                    L"and the agent may still be running", STOP_WAIT_HINT_MS, join);
        }
    }
    cleanStop = TRUE;

cleanup:
    // don't free cmdline here, a thread using it may be still running, memory is freed on exit anyway
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = cleanStop ? 0 : GetLastError();
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 0;
    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);

    LogInfo("exiting");
    return;
}

DWORD WINAPI ControlHandlerEx(IN DWORD controlCode, IN DWORD eventType, IN void *eventData, IN void *context)
{
    switch (controlCode)
    {
    case SERVICE_CONTROL_PRESHUTDOWN:
        // Earliest reliable "the machine is going down" signal: latch the flag so the watchdog
        // stops respawning the agent into a dying machine.
        //
        // THEN REPORT STOPPED, IMMEDIATELY. The previous version only latched the flag, on the
        // assumption that "the SCM follows this with SHUTDOWN/STOP, which is where the state
        // transition belongs". That assumption is wrong and it cost ~3 minutes on EVERY clean
        // shutdown: the SCM does not send SHUTDOWN until preshutdown COMPLETES, and a service that
        // never reports a terminal state is waited out for the full preshutdown timeout (180 s by
        // default) before the SCM gives up and logs
        //     Event 7043: "The Qubes GUI agent watchdog service did not shut down properly after
        //                  receiving a preshutdown control."
        // That event was observed on this rig and traced here (2026-08-29). ServiceMain blocks
        // INFINITE on a worker thread that never exits, so no other path can report the state.
        //
        // Reporting STOPPED from this handler while WatchdogThread was still alive was the
        // second half of that problem: the SCM considered us gone while the respawn loop was
        // still live (only g_ServiceStopping kept it from relaunching). Now that the loop waits
        // on g_StopEvent it ends as soon as it has stopped the agent it started (StopOwnAgent,
        // bounded), so PRESHUTDOWN takes the same path as STOP: STOP_PENDING here with a wait hint
        // that covers that stop, STOPPED from ServiceMain once the thread has exited - and
        // ServiceMain bounds that join, so a stuck thread can never bring Event 7043 back.
        InterlockedExchange(&g_ServiceStopping, 1);
        LogInfo("preshutdown - the agent will not be restarted from here on, stopping");
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOP_PENDING;
        g_Status.dwCheckPoint = 0;
        g_Status.dwWaitHint = STOP_WAIT_HINT_MS;
        SetServiceStatus(g_StatusHandle, &g_Status);
        if (g_StopEvent)
            SetEvent(g_StopEvent);
        break;
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        InterlockedExchange(&g_ServiceStopping, 1);
        LogInfo("stopping...");
        // STOP_PENDING here, STOPPED from ServiceMain once WatchdogThread has exited - which, since
        // 2026-10-03, is after it has stopped the agent it started (the wait hint covers that).
        // Reporting STOPPED from this handler let Stop-Service return while the respawn loop was
        // still live; if its tick fell in that window it relaunched the agent the installer had
        // just killed, under the very device surgery the quiesce exists to protect. Leaving the
        // agent running past the stop was what made every installer kill it by name.
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOP_PENDING;
        g_Status.dwCheckPoint = 0;
        g_Status.dwWaitHint = STOP_WAIT_HINT_MS;
        SetServiceStatus(g_StatusHandle, &g_Status);
        if (g_StopEvent)
            SetEvent(g_StopEvent);
        break;
    case SERVICE_CONTROL_SESSIONCHANGE:
        // Console session arrival is what the watchdog waits for when it could not start the
        // agent (no session yet); wake it instead of leaving it to rediscover the session by polling.
        if (eventType == WTS_CONSOLE_CONNECT || eventType == WTS_SESSION_LOGON)
        {
            WTSSESSION_NOTIFICATION *notification = (WTSSESSION_NOTIFICATION *)eventData;
            LogInfo("session change 0x%x, session %u", eventType,
                notification ? notification->dwSessionId : 0xFFFFFFFF);
            if (g_SessionEvent)
                SetEvent(g_SessionEvent);
        }
        else
        {
            LogDebug("session change 0x%x (ignored)", eventType);
        }
        break;
    default:
        LogDebug("code 0x%x, event 0x%x", controlCode, eventType);
        break;
    }

    return ERROR_SUCCESS;
}
