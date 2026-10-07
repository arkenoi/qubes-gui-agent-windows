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

// THE ONE JOB OF THIS SERVICE (docs/ADR-supervision.md sections 4 and 5, main repo): start a SYSTEM-token
// gui-agent.exe inside the user's interactive session - the one thing no Windows mechanism does - ONCE per
// session, and perform ONE relaunch: the agent's defined RECONNECT exit (dom0's gui-daemon went away; a
// fresh agent re-announcing is the reconnect). Everything else about keeping the agent alive is Windows':
// an agent that DIES makes this service end itself with a failure code (QGA_SVC_EXIT_AGENT_DIED), and the
// SCM's recovery actions - armed by the installer: restart after 5 s, 15 s, then 60 s, failures counted for
// error exits too - restart the service, which launches a new agent at its start. There is no relaunch
// loop, no backoff and no poll in this file any more (owner, 2026-10-07: "rely on windows system
// services if we need to keep smth running"; Jev keepalive_owner=scm-recovery 0.55).
//
// LAUNCH TRIGGERS, and only these: the service's start with a console session present; WTS_SESSION_LOGON /
// WTS_CONSOLE_CONNECT for a session other than one that announced its end, when no agent of ours runs; the
// agent's RECONNECT exit. A logoff that is not a shutdown ends the announced session (WTS_SESSION_LOGOFF
// clears the latch), so the next logon into the console session - autologon, seconds later - gets an agent.
//
// THE SESSION END (the measured defect, 2026-10-07): Windows' session teardown terminated the agent with
// 0x40010004 33-826 ms after the shutdown began and this service relaunched it into the same ending session,
// three instances per shutdown. Now the agent tells this service "my session is ending" from its
// WM_QUERYENDSESSION (through the per-pid channel of include/qga-lifecycle.h, created here before the agent
// runs) and the service acknowledges before the end proceeds; from then on nothing is launched into that
// session. Every agent exit is read through ONE pure decision, QgaDecideAgentExit, whose table the offline
// suite holds row by row.

#include <windows.h>
#include <wtsapi32.h>
#include <sas.h>
#include <shlwapi.h>
#include <sddl.h>
#include <strsafe.h>
#include "common.h"

#include <log.h>
#include <config.h>
#include <qubes-io.h>
#include "deathevent.h"
#include "qga-exitcodes.h"
#include "qga-lifecycle.h"

#define SERVICE_NAME L"QgaWatchdog"

SERVICE_STATUS g_Status;
SERVICE_STATUS_HANDLE g_StatusHandle;

// Set when the SCM tells us the service (or the machine) is going down: the watchdog thread leaves
// through its stop path and never launches again.
volatile LONG g_ServiceStopping = 0;

// Manual-reset: set by ControlHandlerEx on STOP/SHUTDOWN/PRESHUTDOWN, ends WatchdogThread. Before this
// existed the handler reported STOPPED while the respawn loop was still live, so Stop-Service
// returned and the loop could relaunch the agent under an installer that had just killed it.
static HANDLE g_StopEvent = NULL;
// AN ANNOUNCED END THAT NEVER HAPPENS IS AN ANOMALY, NOT A STATE TO SIT IN (2026-10-07, from the Jev review of this
// change: "a session latched as ending that then continues" scored 0.53 - the hole was real). The latch is normally
// cleared by the session's logoff or by the agent's own "the end was cancelled" message. Neither can arrive if the end
// is VETOED by another application AND the agent then dies: the dead agent cannot send the cancellation, no logoff
// follows, and the session would run to its end with no GUI and nothing allowed to launch one.
//
// So the latch is also a BOUNDED FAILURE DETECTOR. Measured 2026-10-07: from the session teardown to this service's
// PRESHUTDOWN was 4 s in one shutdown and 32 s in another, so a real shutdown resolves the latch well inside this
// budget - and if it has not resolved, the end did not happen. On expiry the service says so at ERROR and allows
// launches into that session again. It is not a timeout standing in for an observable condition: both observable
// conditions (the logoff, the cancellation) are waited on, and this is the third exit of that wait (rule 6 of the
// experimenter skill), reported loudly rather than silently recovered from.
#define SESSION_END_NEVER_HAPPENED_MS (90u * 1000u)

// Auto-reset, one each: a session ARRIVED (WTS_CONSOLE_CONNECT / WTS_SESSION_LOGON - a launch trigger) and
// a session LEFT (WTS_SESSION_LOGOFF - the end of a session that announced it). The session ids ride in
// the two LONGs; a burst that overwrites one is harmless, the handler re-reads the console session.
static HANDLE g_SessionArriveEvent = NULL;
static HANDLE g_SessionLeaveEvent = NULL;
static volatile LONG g_ArriveSession = -1;
static volatile LONG g_LeaveSession = -1;

// The service-specific code this service ends with when its agent died or could not be launched
// (0 = a clean stop). Set by WatchdogThread, reported by ServiceMain: the SCM then logs 7024 and runs the
// recovery actions.
static volatile LONG g_ServiceFailCode = 0;

#define NO_SESSION 0xFFFFFFFFUL

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

// ---- the lifecycle channel (include/qga-lifecycle.h) --------------------------------------------------
typedef struct _LIFECYCLE_CHANNEL
{
    HANDLE Notice;     // manual-reset, agent -> service: "Windows is ending my session"
    HANDLE Ack;        // manual-reset, service -> agent: acknowledged
    HANDLE Continue;   // auto-reset, agent -> service: the end was cancelled
    HANDLE Done;       // manual-reset, agent -> service: the orderly exit is COMPLETE (see include/qga-lifecycle.h)
} LIFECYCLE_CHANNEL;

static void LifecycleChannelClose(IN OUT LIFECYCLE_CHANNEL *ch)
{
    if (ch->Notice) CloseHandle(ch->Notice);
    if (ch->Ack) CloseHandle(ch->Ack);
    if (ch->Continue) CloseHandle(ch->Continue);
    if (ch->Done) CloseHandle(ch->Done);
    ch->Notice = ch->Ack = ch->Continue = ch->Done = NULL;
}

static HANDLE LifecycleEventCreate(IN SECURITY_ATTRIBUTES *sa, IN DWORD agentPid, IN const WCHAR *which, IN BOOL manualReset)
{
    WCHAR name[96];
    HANDLE h;
    if (FAILED(QgaLifecycleObjectName(name, RTL_NUMBER_OF(name), agentPid, which)))
        return NULL;
    h = CreateEvent(sa, manualReset, FALSE, name);
    if (!h)
    {
        win_perror("CreateEvent(lifecycle channel)");
        return NULL;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Somebody holds a name made for a pid that does not exist yet: a squat. Refused - the agent is ended
        // before it ran (the caller's job) and the launch is reported as failed.
        LogError("QGAWDSQUAT the lifecycle object '%s' already exists before the agent it is named for has run - "
            L"refusing it", name);
        CloseHandle(h);
        return NULL;
    }
    return h;
}

// Created by THIS service, SYSTEM-only, BEFORE the agent runs (it is suspended until this returns); the agent only
// opens them. FALSE = nothing created (the caller ends the suspended agent).
static BOOL LifecycleChannelCreate(IN DWORD agentPid, OUT LIFECYCLE_CHANNEL *ch)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    SECURITY_ATTRIBUTES sa;
    BOOL ok;

    ZeroMemory(ch, sizeof(*ch));
    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(QGA_LIFECYCLE_SDDL, SDDL_REVISION_1, &sd, NULL) || !sd)
    {
        win_perror("ConvertStringSecurityDescriptorToSecurityDescriptor(lifecycle)");
        return FALSE;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    ch->Notice = LifecycleEventCreate(&sa, agentPid, QGA_LIFECYCLE_NOTICE, TRUE);
    ch->Ack = LifecycleEventCreate(&sa, agentPid, QGA_LIFECYCLE_ACK, TRUE);
    ch->Continue = LifecycleEventCreate(&sa, agentPid, QGA_LIFECYCLE_CONTINUE, FALSE);
    ch->Done = LifecycleEventCreate(&sa, agentPid, QGA_LIFECYCLE_DONE, TRUE);
    LocalFree(sd);
    ok = ch->Notice && ch->Ack && ch->Continue && ch->Done;
    if (!ok)
        LifecycleChannelClose(ch);
    return ok;
}

// Starts the process as SYSTEM in the currently active console session, with its lifecycle channel created
// before it runs (CREATE_SUSPENDED, the channel, then ResumeThread).
// Returns ERROR_SUCCESS with *processHandle/*processId/*sessionId/*channel set when a process was created,
// START_SKIPPED_NO_SESSION when there is no console session to start it in (nothing was launched), or the
// Win32 error of the failing call.
DWORD StartTargetProcess(IN WCHAR *exePath, OUT HANDLE *processHandle, OUT DWORD *processId, OUT DWORD *sessionId,
    OUT LIFECYCLE_CHANNEL *channel) // non-const because it can be modified by CreateProcess*
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
    *sessionId = NO_SESSION;
    ZeroMemory(channel, sizeof(*channel));

    consoleSessionId = WTSGetActiveConsoleSessionId();
    if (consoleSessionId == NO_SESSION) // disconnected or changing
    {
        // Distinct code, not ERROR_SUCCESS: the caller used to treat this skip as a launch, find
        // no agent a second later, and count it as a crash.
        LogDebug("console session is 0x%x, skipping", consoleSessionId);
        return START_SKIPPED_NO_SESSION;
        // we'll launch gui agent when the console connects to a session again
    }

    // Get access token from ourselves. Both tokens are closed before returning on every path:
    // they used to leak on each launch (audit 2026-09-08). (GetCurrentProcess() is a pseudo-handle; it is not closed.)
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
    // SUSPENDED: the lifecycle channel is named by the agent's pid and must exist before the agent's first act
    // opens it (include/qga-lifecycle.h). Resumed below once the channel exists; ended by handle if it cannot.
    if (!CreateProcessAsUser(newToken, NULL, exePath, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        status = win_perror("CreateProcessAsUser");
        CloseHandle(newToken);
        return status != ERROR_SUCCESS ? status : ERROR_GEN_FAILURE; // never report a failed launch as success
    }
    CloseHandle(newToken);   // the new process holds its own reference

    if (!LifecycleChannelCreate(pi.dwProcessId, channel))
    {
        // Never ran (still suspended): ended by the handle we hold, which is this service's own child.
        LogError("QGAWDLAUNCH the lifecycle channel for PID %u could not be created - ending the suspended agent "
            L"before it runs; without the channel a session end could not be handshaken", pi.dwProcessId);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return ERROR_GEN_FAILURE;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1)
    {
        status = win_perror("ResumeThread(agent)");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LifecycleChannelClose(channel);
        return status != ERROR_SUCCESS ? status : ERROR_GEN_FAILURE;
    }

    // Keep the process handle: the watchdog waits on it, so the agent's exit is seen the instant it happens.
    *processHandle = pi.hProcess;
    *processId = pi.dwProcessId;
    *sessionId = consoleSessionId;
    CloseHandle(pi.hThread);

    return ERROR_SUCCESS;
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
// While waiting, an end-session notice from the agent is still acknowledged (a shutdown and a service
// stop can overlap; the agent's handshake is bounded, so the ack must not wait for this wait).
// Every outcome is logged with the pid and the exit code; ServiceMain reports STOPPED only after
// this has returned. Closes the handle and the channel.
static void StopOwnAgent(IN HANDLE agentProcess, IN DWORD agentPid, IN const WCHAR *exeName, IN OUT LIFECYCLE_CHANNEL *ch)
{
    HANDLE shutdownEvent;
    DWORD wait;
    DWORD exitCode = 0xFFFFFFFF;
    HANDLE waitHandles[2];
    ULONGLONG deadline = GetTickCount64() + AGENT_STOP_GRACE_MS;

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

    waitHandles[0] = agentProcess;
    waitHandles[1] = ch->Notice;
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        DWORD left = (deadline > now) ? (DWORD)(deadline - now) : 0;
        wait = WaitForMultipleObjects(ch->Notice ? 2 : 1, waitHandles, FALSE, left);
        if (wait == WAIT_OBJECT_0 + 1)
        {
            // The agent's session is ending while we stop: acknowledge, so its WM_QUERYENDSESSION returns at once.
            LogInfo("QGAWDSESSIONEND '%s' (PID %u) reports its session ending during the service stop - acknowledged",
                exeName, agentPid);
            SetEvent(ch->Ack);
            ResetEvent(ch->Notice);
            continue;
        }
        break;
    }
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
    LifecycleChannelClose(ch);
}

// What this service does about an agent exit, decided by include/qga-lifecycle.h's table and said in the log and,
// for a death, in the Event Log. Returns the verdict for the caller's state changes.
static QGA_EXIT_VERDICT JudgeAgentExit(IN const WCHAR *exeName, IN DWORD agentPid, IN DWORD agentSession,
    IN DWORD exitCode, IN BOOL noticeAcked, IN BOOL orderlyDone, IN ULONGLONG startedAt)
{
    const QGA_EXIT_VERDICT v = QgaDecideAgentExit(exitCode, noticeAcked, orderlyDone);
    const ULONGLONG ranMs = startedAt != 0 ? GetTickCount64() - startedAt : DEATHEVENT_RAN_UNKNOWN;

    switch (v.Decision)
    {
    case QGA_DECIDE_NOTHING:
        LogInfo("QGAWDREQUESTED '%s' (PID %u) exited on request (0x%x) - not a death, nothing relaunched; the next "
            L"logon or console connect gets an agent", exeName, agentPid, exitCode);
        break;
    case QGA_DECIDE_SESSION_END:
        LogInfo("QGAWDSESSIONEND '%s' (PID %u) left with its session %u (exit 0x%x, notice acknowledged) - not a death; "
            L"no agent is launched into session %u", exeName, agentPid, agentSession, exitCode, agentSession);
        break;
    case QGA_DECIDE_SESSION_END_UNNOTICED:
        LogError("QGAWDSESSIONEND-UNNOTICED '%s' (PID %u) left with its session %u (exit 0x%x) but this service never "
            L"saw its end-session notice - the lifecycle channel failed; no agent is launched into session %u",
            exeName, agentPid, agentSession, exitCode, agentSession);
        break;
    case QGA_DECIDE_SESSION_END_REAPED:
        // THE CLEAN SHUTDOWN. The system reaped the agent a few milliseconds AFTER its orderly exit
        // completed - acknowledged notice, `done` signalled - which is what every measured clean
        // session end does. Its verdict row already said so (IsError FALSE, no relaunch, no death
        // record, no service failure); this case was simply never added, so the decision fell
        // through `default:` into the QGAWDDEATH branch and a completed exit was reported as a
        // death at ERROR, with text claiming the service was ending for the SCM's recovery when
        // FailService is FALSE and it was not. Measured on win11r-logvol 2026-10-08: the agent
        // logged "QGAENDSESSION orderly exit complete" and the watchdog logged QGAWDDEATH in the
        // same second. This is the row the 2026-10-07 split was added for; only its log line was
        // missing.
        LogInfo("QGAWDSESSIONEND-REAPED '%s' (PID %u) finished its orderly exit and was then reaped by the system "
            L"with its session %u (exit 0x%x, notice acknowledged, completion signalled) - not a death; no agent "
            L"is launched into session %u", exeName, agentPid, agentSession, exitCode, agentSession);
        break;
    case QGA_DECIDE_SESSION_END_FORCED:
        // SAYS ONLY WHAT THE SERVICE KNOWS. It used to assert "its orderly exit did not complete
        // inside the end-session budget" - a cause the service has no way to establish: it never
        // sees the agent's own exit code on a session end (Windows reaps the process before
        // ExitProcess can set it, so this reads 0x40010004 = DBG_TERMINATE_PROCESS), and it cannot
        // tell a budget expiry from a kill mid-teardown. The agent's own QGAENDSESSION lines can.
        LogError("QGAWDSESSIONEND-FORCED '%s' (PID %u) was ended by the system (0x%x) after acknowledging its "
            L"end-session notice and had NOT signalled that its orderly exit was complete - it was ended "
            L"mid-teardown or its exit budget expired, and the agent's own QGAENDSESSION lines say which "
            L"(its vchan announcement may be left behind); no agent is launched into session %u",
            exeName, agentPid, exitCode, agentSession);
        break;
    case QGA_DECIDE_NOTICE_MISSED:
        // THE SYSTEM'S OWN RECORD OF THE END OF THAT SESSION, with no word from the agent: either the agent died
        // before its end-session window was up, or Windows never sent WM_QUERYENDSESSION to it. Not relaunched
        // into that session - the measured defect - and LOUD, so the retest shows which it was.
        LogError("QGAWDNOTICEMISSED end-session notice MISSED: '%s' (PID %u) was ended by the system (0x%x, "
            L"DBG_TERMINATE_PROCESS - what Windows' session teardown leaves) and this service saw no end-session "
            L"notice from it; its vchan announcement is left behind. No agent is launched into session %u. "
            L"Check the agent's log for QGAENDSESSION / QGALIFECYCLE lines", exeName, agentPid, exitCode, agentSession);
        break;
    case QGA_DECIDE_NOGUI_LATCH:
        // A START-TIME CONDITION, NOT A DEATH (include/qga-exitcodes.h; Jev 0.94): no GUI domain for this qube this boot.
        // Logged once, no death event, no relaunch before the next boot - a relaunch would fail the same way every time.
        LogInfo("QGAWDNOGUIDOMAIN '%s' (PID %u) found no GUI domain for this qube (guivm is '') - not relaunching it "
            L"before the next boot; this is a start-time condition, not a death", exeName, agentPid);
        break;
    case QGA_DECIDE_RECONNECT:
        // THE ONE RELAUNCH THIS SERVICE PERFORMS (Jev 1.0): a protocol event, not a death - dom0's gui-daemon went
        // away or never came, and a fresh agent announcing a fresh vchan is the reconnect. The agent bounds this
        // itself (its first-client restart budget), so a daemon that never returns cannot turn it into a loop.
        LogInfo("QGAWDRECONNECT '%s' (PID %u) exited to reconnect (0x%x: dom0's gui-daemon went away or never came) "
            L"after %I64u ms - relaunching at once; not a death", exeName, agentPid, exitCode, ranMs);
        break;
    case QGA_DECIDE_DEATH_IN_ENDING_SESSION:
    case QGA_DECIDE_DEATH:
    default:
        // A DYING AGENT IS A MAJOR ERROR, not a warning (owner, 2026-10-03). This service did not ask it to exit (a
        // requested stop is QGA_EXIT_REQUESTED, or the g_StopEvent path and StopOwnAgent) and it did not leave for a
        // defined reason: it DIED. 0x5aa is ERROR_NO_SYSTEM_RESOURCES, which on this guest has meant an exhausted
        // Xen grant table (measured 2026-08-15) - said only when that is the code.
        LogError("QGAWDDEATH '%s' (PID %u) exited with code 0x%x without this service asking it to - the agent DIED "
            L"after %I64u ms (%s).%s", exeName, agentPid, exitCode, ranMs,
            v.Decision == QGA_DECIDE_DEATH_IN_ENDING_SESSION
                ? L"its session is ending: not relaunched into it, the service stays"
                : L"this service ends with QGA_SVC_EXIT_AGENT_DIED so the SCM's recovery restarts it and a new agent",
            exitCode == 0x5aa
                ? L" Exit code 0x5aa is ERROR_NO_SYSTEM_RESOURCES: on this guest that has meant an exhausted Xen "
                  L"grant table, which only a reboot clears."
                : L"");
        break;
    }
    if (v.WriteDeathRecord)
    {
        // THE ONE RECORD THE SYSTEM CANNOT WRITE ITSELF (docs/ADR-supervision.md 2, main repo):
        // a crash is in Windows Error Reporting and Application event 1000 already, but a clean,
        // unasked exit is visible only here. ONE Event Log entry under our source, with the exit
        // code and how long it ran; the dom0 notification is the event-triggered reporter's job
        // (ADR 3), not this service's - nothing here waits on qrexec or on a session.
        DeathEventReport(DEATHEVENT_ID_GUI_AGENT, exeName, agentPid, exitCode, ranMs,
            v.FailService
                ? L"The QubesGuiWatchdog service ends itself so that Windows' service recovery restarts it (after 5 s, "
                  L"15 s, then 60 s) and the restarted service starts a new GUI agent. Its log and the agent's are in "
                  L"the Qubes Tools log directory; a crash also leaves a Windows Error Reporting record "
                  L"(AppCrash_gui-agent.exe_*)."
                : L"Its session is ending, so nothing is relaunched into it; the next logon gets a new GUI agent. Its "
                  L"log and the watchdog's are in the Qubes Tools log directory; a crash also leaves a Windows Error "
                  L"Reporting record (AppCrash_gui-agent.exe_*).");
    }
    return v;
}

// Launches the agent once per session, relaunches it once on its RECONNECT exit, and otherwise leaves keeping it
// alive to Windows (see the file header).
DWORD WINAPI WatchdogThread(void *param)
{
    WCHAR* cmdline = (WCHAR*) param;

    PathUnquoteSpaces(cmdline);
    WCHAR* exeName = PathFindFileName(cmdline);

    LogDebug("cmdline: '%s', exe: '%s'", cmdline, exeName);

    // Handle of the agent WE STARTED - the only process this service owns. While we hold one it is
    // the liveness oracle: the loop sleeps on it and wakes the moment the process exits, and the
    // stop path below ends it (StopOwnAgent).
    HANDLE agentProcess = NULL;
    DWORD agentPid = 0;
    DWORD agentSession = NO_SESSION;   // the console session it was launched into
    ULONGLONG startedAt = 0;
    LIFECYCLE_CHANNEL channel;
    BOOL noticeAcked = FALSE;          // this instance announced its session's end and we acknowledged
    BOOL orderlyDone = FALSE;          // ...and it signalled that its orderly exit had COMPLETED
    // The session that announced its end: nothing is launched into it until WTS_SESSION_LOGOFF says it has ended.
    DWORD endedSession = NO_SESSION;
    // A launch trigger fired and no agent of ours runs: launch at the next opportunity (console session present,
    // no foreign agent). TRUE at service start - "service start with a console session present" is a trigger.
    BOOL launchWanted = TRUE;
    BOOL waitingForSession = FALSE;
    // A same-named process this service did NOT start. Never adopted (owner's rule 2026-10-03:
    // nothing is killed or adopted by NAME), never stopped; only WAITED ON, through this handle, so that
    // no second agent is started while it lives (two agents fight for the vchan and the loser dies -
    // main.c, the single-instance mutex), and reported once per episode (QGAWDFOREIGN).
    HANDLE foreignProcess = NULL;
    DWORD foreignPid = 0;
    // The agent reported NO GUI DOMAIN this boot (include/qga-exitcodes.h): nothing is relaunched before the next boot.
    BOOL noGuiDomain = FALSE;
    // When the latch above was set, and whether its expiry has already been reported (see
    // SESSION_END_NEVER_HAPPENED_MS). 0 = no latch is being timed.
    ULONGLONG endedSessionAt = 0;

    ZeroMemory(&channel, sizeof(channel));

    while (TRUE)
    {
        HANDLE waitHandles[6];
        DWORD waitCount = 0;
        DWORD idxNotice = 0xFFFFFFFF, idxContinue = 0xFFFFFFFF, idxProcess = 0xFFFFFFFF;
        DWORD wait;

        // ---- a launch, if one is wanted and possible --------------------------------------------------------
        if (launchWanted && !agentProcess && !foreignProcess && !noGuiDomain)   // QGA_NOGUI_NORELAUNCH
        {
            DWORD consoleSession = WTSGetActiveConsoleSessionId();
            DWORD pid = 0, sid = 0;
            if (consoleSession == NO_SESSION)
            {
                // No console session yet (early boot, or changing): nothing can be started. The arrival event wakes us.
                if (!waitingForSession)
                    LogInfo("QGAWDLAUNCH '%s' wanted but there is no console session yet - will start it when one connects",
                        exeName);
                waitingForSession = TRUE;
            }
            else if (consoleSession == endedSession)
            {
                // THE SESSION ANNOUNCED ITS END: never launched into (the measured defect). A LOGOFF of that session
                // clears this; a LOGON that follows is a new trigger.
                LogInfo("QGAWDNOLAUNCH '%s' not launched: console session %u announced its end and has not ended yet "
                    L"(a logon after it ends is the next trigger)", exeName, consoleSession);
                launchWanted = FALSE;
                waitingForSession = FALSE;
            }
            else if (IsProcessRunning(exeName, &pid, &sid))
            {
                // Is the gui agent running already? Our handle is authoritative; without one, a same-named process
                // may exist. It is NOT ours: never adopted, never stopped - reported once per episode as an anomaly,
                // with pid and session, and waited out through a separate handle so that no second agent is started
                // while it lives. The launch stays wanted and happens when it exits.
                foreignProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (foreignProcess)
                    foreignPid = pid;
                else
                    win_perror("OpenProcess(foreign agent, SYNCHRONIZE)");
                LogError("QGAWDFOREIGN '%s' (PID %u, session %u) is running but was NOT started by this service - "
                    L"not adopted, not stopped, and no second agent is started while it lives (%s)",
                    exeName, pid, sid,
                    foreignProcess ? L"waiting on its handle" : L"it cannot be opened - the service ends with a failure "
                                                                   L"code so the SCM's recovery looks again");
                if (!foreignProcess)
                {
                    InterlockedExchange(&g_ServiceFailCode, (LONG)QGA_SVC_EXIT_LAUNCH_FAILED);
                    break;
                }
                waitingForSession = FALSE;
            }
            else
            {
                HANDLE newProcess = NULL;
                DWORD newPid = 0, newSession = NO_SESSION;
                DWORD status;
                waitingForSession = FALSE;
                status = StartTargetProcess(cmdline, &newProcess, &newPid, &newSession, &channel);
                if (status == START_SKIPPED_NO_SESSION)
                {
                    // Session vanished between our check and the launch: not an attempt, the arrival event wakes us.
                    waitingForSession = TRUE;
                }
                else if (status == ERROR_SUCCESS && newProcess)
                {
                    agentProcess = newProcess;
                    agentPid = newPid;
                    agentSession = newSession;
                    startedAt = GetTickCount64();
                    noticeAcked = FALSE;
                    launchWanted = FALSE;
                    LogInfo("QGAWDLAUNCH '%s' started as PID %u in session %u", exeName, agentPid, agentSession);
                }
                else
                {
                    // A LAUNCH THAT FAILS IS A FAILURE OF THIS SERVICE (already logged by win_perror): nothing ran, so
                    // there is no 4001; the service ends with its own code and the SCM's recovery retries by restarting
                    // it - no backoff loop here (owner, 2026-10-07).
                    LogError("QGAWDFAIL starting '%s' failed (error 0x%x) - the guest has NO GUI; this service ends with "
                        L"QGA_SVC_EXIT_LAUNCH_FAILED so the SCM's recovery restarts it and the launch is retried",
                        exeName, status);
                    InterlockedExchange(&g_ServiceFailCode, (LONG)QGA_SVC_EXIT_LAUNCH_FAILED);
                    break;
                }
            }
        }

        // ---- the wait: no timeout - every wake is an event ------------------------------------------------
        waitHandles[waitCount++] = g_StopEvent;
        waitHandles[waitCount++] = g_SessionArriveEvent;
        waitHandles[waitCount++] = g_SessionLeaveEvent;
        if (agentProcess)
        {
            idxProcess = waitCount; waitHandles[waitCount++] = agentProcess;
            idxNotice = waitCount; waitHandles[waitCount++] = channel.Notice;
            idxContinue = waitCount; waitHandles[waitCount++] = channel.Continue;
        }
        else if (foreignProcess)
        {
            idxProcess = waitCount; waitHandles[waitCount++] = foreignProcess;   // waited on, never acted on
        }

        // The wait has a deadline ONLY while a latched session has no agent left in it: that is the one state that
        // nothing observable may ever leave (above). Otherwise the events are the only wake-ups.
        {
            DWORD waitMs = INFINITE;
            if (endedSessionAt != 0 && !agentProcess)
            {
                const ULONGLONG el = GetTickCount64() - endedSessionAt;
                waitMs = (el >= SESSION_END_NEVER_HAPPENED_MS) ? 0 : (DWORD)(SESSION_END_NEVER_HAPPENED_MS - el);
            }
            wait = WaitForMultipleObjects(waitCount, waitHandles, FALSE, waitMs);
        }
        if (wait == WAIT_TIMEOUT)
        {
            // THE ANNOUNCED END NEVER HAPPENED (SESSION_END_NEVER_HAPPENED_MS). The session that said it was ending
            // has neither ended nor cancelled, and the agent that announced it is gone - so the end was vetoed and
            // took the cancellation with it. Say so loudly and allow launches again; the loop's next pass launches.
            const DWORD cs = WTSGetActiveConsoleSessionId();
            LogError("QGAWDSESSIONSTUCK session %u announced its end %u s ago and has neither ended nor cancelled it, "
                L"and no agent of ours is left in it (console session is now %u) - the end was vetoed and the agent "
                L"that announced it died with the cancellation. Allowing launches into it again; the guest had NO GUI "
                L"for that time.", endedSession, (unsigned)(SESSION_END_NEVER_HAPPENED_MS / 1000), cs);
            endedSession = NO_SESSION;
            endedSessionAt = 0;
            launchWanted = TRUE;
            continue;
        }
        if (wait == WAIT_OBJECT_0) // stop event
        {
            LogInfo("service stop requested, watchdog thread exiting");
            break;
        }
        if (wait == WAIT_FAILED)
        {
            // Our own handle table is broken: the loop cannot wait at all. Not a state to spin in - end the
            // service with a failure code; the SCM's recovery restarts it.
            win_perror("WaitForMultipleObjects");
            InterlockedExchange(&g_ServiceFailCode, (LONG)QGA_SVC_EXIT_LAUNCH_FAILED);
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1)
        {
            // A SESSION ARRIVED (logon / console connect): a launch trigger when no agent of ours runs, for any
            // session but one that announced its end.
            DWORD sid = (DWORD)InterlockedCompareExchange(&g_ArriveSession, -1, -1);
            if (agentProcess || foreignProcess)
                LogInfo("QGAWDTRIGGER session %u arrived while an agent runs (PID %u) - nothing to launch", sid,
                    agentProcess ? agentPid : foreignPid);
            else if (sid == endedSession)
                LogInfo("QGAWDTRIGGER session %u arrived but it announced its end and has not ended - not a launch trigger", sid);
            else
            {
                LogInfo("QGAWDTRIGGER session %u arrived and no agent of ours runs - launching", sid);
                launchWanted = TRUE;
            }
            continue;
        }
        if (wait == WAIT_OBJECT_0 + 2)
        {
            // A SESSION LEFT (logoff). The session that announced its end has now ended: a logon that follows -
            // autologon at the sign-in screen, seconds later on these guests - is a new session to launch into.
            DWORD sid = (DWORD)InterlockedCompareExchange(&g_LeaveSession, -1, -1);
            if (sid == endedSession)
            {
                LogInfo("QGAWDSESSIONEND session %u has ended (logoff) - a logon into the console session is the next "
                    L"launch trigger", sid);
                endedSession = NO_SESSION;
                endedSessionAt = 0;
            }
            else
                LogDebug("session %u logged off (not the announced one %u)", sid, endedSession);
            continue;
        }
        if (agentProcess && wait == WAIT_OBJECT_0 + idxNotice)
        {
            // THE AGENT'S SESSION IS ENDING (its WM_QUERYENDSESSION). Acknowledge at once - its handler waits for this,
            // bounded - and latch the session: nothing is launched into it from here on, whatever the exit code says.
            noticeAcked = TRUE;
            endedSession = agentSession;
            endedSessionAt = GetTickCount64();   // the detector above starts once no agent is left in it
            SetEvent(channel.Ack);
            ResetEvent(channel.Notice);
            LogInfo("QGAWDSESSIONEND '%s' (PID %u) reports session %u ending - acknowledged; no agent is launched into "
                L"session %u from here on", exeName, agentPid, agentSession, agentSession);
            continue;
        }
        if (agentProcess && wait == WAIT_OBJECT_0 + idxContinue)
        {
            // The end was cancelled (another application vetoed it): the session continues.
            noticeAcked = FALSE;
            if (endedSession == agentSession)
            {
                endedSession = NO_SESSION;
                endedSessionAt = 0;
            }
            ResetEvent(channel.Ack);
            LogWarning("QGAWDSESSIONCONTINUES '%s' (PID %u): the end of session %u was cancelled - launches into it are "
                L"allowed again", exeName, agentPid, agentSession);
            continue;
        }
        if (agentProcess && wait == WAIT_OBJECT_0 + idxProcess)
        {
            DWORD exitCode = 0;
            QGA_EXIT_VERDICT v;
            if (!GetExitCodeProcess(agentProcess, &exitCode))
                exitCode = 0xFFFFFFFF;
            // Did the agent get to the END of its orderly exit before the system reaped it? Its own signal
            // answers that; the observed process code cannot (both cases arrive as 0x40010004).
            orderlyDone = (channel.Done != NULL && WaitForSingleObject(channel.Done, 0) == WAIT_OBJECT_0);
            v = JudgeAgentExit(exeName, agentPid, agentSession, exitCode, noticeAcked, orderlyDone, startedAt);
            if (v.NoLaunchIntoSession)
                endedSession = agentSession;
            if (v.Decision == QGA_DECIDE_NOGUI_LATCH)
                noGuiDomain = TRUE;   // QGA_NOGUI_LATCH
            CloseHandle(agentProcess);
            agentProcess = NULL;
            agentPid = 0;
            LifecycleChannelClose(&channel);
            noticeAcked = FALSE;
            if (v.RelaunchNow)
                launchWanted = TRUE;
            if (v.FailService)
            {
                // THE SERVICE FAILS ITSELF so that Windows restarts it (and with it the agent): no relaunch loop and no
                // backoff in this file - the SCM's recovery actions are the keep-alive (docs/ADR-supervision.md 4).
                LogError("QGAWDFAIL ending this service with QGA_SVC_EXIT_AGENT_DIED (0x%x) for the SCM's recovery to "
                    L"restart it", QGA_SVC_EXIT_AGENT_DIED);
                InterlockedExchange(&g_ServiceFailCode, (LONG)QGA_SVC_EXIT_AGENT_DIED);
                break;
            }
            continue;
        }
        if (foreignProcess && wait == WAIT_OBJECT_0 + idxProcess)
        {
            // A stranger we were waiting out has exited: say so; the deferred launch happens at the top of the loop.
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(foreignProcess, &exitCode))
                exitCode = 0xFFFFFFFF;
            LogInfo("QGAWDFOREIGN the '%s' this service did not start (PID %u) has exited with code 0x%x - "
                L"the deferred launch proceeds", exeName, foreignPid, exitCode);
            CloseHandle(foreignProcess);
            foreignProcess = NULL;
            foreignPid = 0;
            launchWanted = TRUE;
            continue;
        }
        LogWarning("unexpected wait result 0x%x", wait);
    }

    // THE OWNER STOPS ITS CHILD: the agent this service started goes down with the service, by
    // handle (StopOwnAgent logs every outcome). A stranger that was only waited out is left exactly
    // as it was - it is not ours - and that is said. On a FAILURE exit (the agent died) there is no
    // agent to stop; the service ends and the SCM restarts it.
    if (agentProcess)
        StopOwnAgent(agentProcess, agentPid, exeName, &channel);
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

    UNREFERENCED_PARAMETER(param);
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
    LONG failCode = 0;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

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
    g_SessionArriveEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_SessionLeaveEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_StopEvent || !g_SessionArriveEvent || !g_SessionLeaveEvent)
    {
        win_perror("CreateEvent");
        goto cleanup;
    }

    g_Status.dwServiceType = SERVICE_WIN32;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    // PRESHUTDOWN arrives BEFORE the ordinary shutdown notifications; SESSIONCHANGE tells the watchdog when a
    // console session arrives (the launch trigger) and when one has left (the end of an announced session).
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

    // The watchdog thread exits on g_StopEvent (set by ControlHandlerEx on STOP/SHUTDOWN/PRESHUTDOWN, which
    // report STOP_PENDING) - after stopping the agent it started (StopOwnAgent, bounded) - OR on its own when the
    // agent died or could not be launched (g_ServiceFailCode set): then this service reports STOPPED with that
    // service-specific code, the SCM logs 7024 and its recovery actions restart the service. STOPPED is reported
    // here, after the thread has actually exited, so a caller whose Stop-Service returned is guaranteed no
    // further agent launch from this service AND no agent of this service's left behind. The join is BOUNDED
    // once a stop is requested, by STOP_WAIT_HINT_MS (what the handler told the SCM to allow).
    {
        HANDLE joinHandles[2] = { watchdogHandle, g_StopEvent };
        DWORD join = WaitForMultipleObjects(2, joinHandles, FALSE, INFINITE);
        if (join == WAIT_OBJECT_0 + 1)
        {
            join = WaitForSingleObject(watchdogHandle, STOP_WAIT_HINT_MS);
            if (join != WAIT_OBJECT_0)
                LogError("watchdog thread did not exit within %u ms of the stop request (wait 0x%x) - "
                    L"reporting STOPPED anyway; no launch can follow (g_ServiceStopping is latched and this service "
                    L"has no respawn loop since 2026-10-07), but the agent may still be running",
                    STOP_WAIT_HINT_MS, join);
        }
        else
        {
            failCode = InterlockedCompareExchange(&g_ServiceFailCode, 0, 0);
            if (failCode != 0 && InterlockedCompareExchange(&g_ServiceStopping, 0, 0) == 0)
            {
                // Not asked to stop: the thread ended because the agent died or could not be launched. STOP_PENDING is
                // not needed - nothing is left to stop - and the code below is what the SCM's recovery keys on.
                LogError("QGAWDFAIL the service ends with service-specific code 0x%x (the SCM's recovery restarts it)",
                    (DWORD)failCode);
            }
            else
                failCode = 0;
        }
    }
    cleanStop = TRUE;

cleanup:
    // don't free cmdline here, a thread using it may be still running, memory is freed on exit anyway
    g_Status.dwCurrentState = SERVICE_STOPPED;
    if (failCode != 0)
    {
        g_Status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_Status.dwServiceSpecificExitCode = (DWORD)failCode;
    }
    else
    {
        g_Status.dwWin32ExitCode = cleanStop ? 0 : GetLastError();
        g_Status.dwServiceSpecificExitCode = 0;
    }
    g_Status.dwCheckPoint = 0;
    g_Status.dwWaitHint = 0;
    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);

    LogInfo("exiting");
    return;
}

DWORD WINAPI ControlHandlerEx(IN DWORD controlCode, IN DWORD eventType, IN void *eventData, IN void *context)
{
    UNREFERENCED_PARAMETER(context);
    switch (controlCode)
    {
    case SERVICE_CONTROL_PRESHUTDOWN:
        // The machine is going down. MEASURED 2026-10-07: this arrives 4-32 s after the shutdown began, AFTER
        // session 1's logoff - too late to keep the agent from being relaunched into the ending session, which is
        // why the session end is handshaken with the agent itself (the lifecycle channel). Here it only ends the
        // service cleanly: REPORT STOP_PENDING AND STOP. A service that never reports a terminal state is waited
        // out for the full preshutdown timeout (180 s by default) and logged as Event 7043 - observed on this rig
        // (2026-08-29) and the reason this once reported STOPPED straight from the handler; now the thread stops
        // the agent it started (StopOwnAgent, bounded) and ServiceMain reports STOPPED after it has exited.
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
    {
        // A console session ARRIVING is the launch trigger (service start aside); a session LEAVING is how an
        // announced end becomes final (a logoff without a shutdown, so the next logon can get an agent).
        WTSSESSION_NOTIFICATION *notification = (WTSSESSION_NOTIFICATION *)eventData;
        DWORD sid = notification ? notification->dwSessionId : NO_SESSION;
        if (eventType == WTS_CONSOLE_CONNECT || eventType == WTS_SESSION_LOGON)
        {
            LogInfo("session change 0x%x (arrival), session %u", eventType, sid);
            InterlockedExchange(&g_ArriveSession, (LONG)sid);
            if (g_SessionArriveEvent)
                SetEvent(g_SessionArriveEvent);
        }
        else if (eventType == WTS_SESSION_LOGOFF)
        {
            LogInfo("session change 0x%x (logoff), session %u", eventType, sid);
            InterlockedExchange(&g_LeaveSession, (LONG)sid);
            if (g_SessionLeaveEvent)
                SetEvent(g_SessionLeaveEvent);
        }
        else
        {
            LogDebug("session change 0x%x, session %u (ignored)", eventType, sid);
        }
        break;
    }
    default:
        LogDebug("code 0x%x, event 0x%x", controlCode, eventType);
        break;
    }

    return ERROR_SUCCESS;
}
