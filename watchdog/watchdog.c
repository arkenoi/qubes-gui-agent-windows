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
DWORD StartTargetProcess(IN WCHAR *exePath) // non-const because it can be modified by CreateProcess*
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    HANDLE newToken;
    DWORD currenttSessionId, consoleSessionId;
    DWORD size;
    HANDLE currentToken;
    HANDLE currentProcess = GetCurrentProcess();

    consoleSessionId = WTSGetActiveConsoleSessionId();
    if (consoleSessionId == 0xFFFFFFFF) // disconnected or changing
    {
        LogDebug("console session is 0x%x, skipping", consoleSessionId);
        return ERROR_SUCCESS;
        // we'll launch gui agent when the console connects to a session again
    }

    // Get access token from ourselves.
    OpenProcessToken(currentProcess, TOKEN_ALL_ACCESS, &currentToken);
    // Session ID is stored in the access token. For services it's normally 0.
    GetTokenInformation(currentToken, TokenSessionId, &currenttSessionId, sizeof(currenttSessionId), &size);
    LogDebug("current session: %d, console session: %d", currenttSessionId, consoleSessionId);

    // We need to create a primary token for CreateProcessAsUser.
    if (!DuplicateTokenEx(currentToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
    {
        return win_perror("DuplicateTokenEx");
    }
    CloseHandle(currentProcess);

    // Change the session ID in the new access token to the target session ID.
    // This requires SeTcbPrivilege, but we're running as SYSTEM and have it.
    if (!SetTokenInformation(newToken, TokenSessionId, &consoleSessionId, sizeof(consoleSessionId)))
    {
        return win_perror("SetTokenInformation(TokenSessionId)");
    }

    LogInfo("Running process '%s' in session %d", exePath, consoleSessionId);
    // Create process with the new token.
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    // No need to set desktop here, gui agent attaches to the input desktop anyway,
    // and hardcoding this to winlogon is wrong.
    if (!CreateProcessAsUser(newToken, NULL, exePath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        return win_perror("CreateProcessAsUser");
    }

    CloseHandle(pi.hProcess);
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

    while (TRUE)
    {
        Sleep(backoffMs);

        // Check if the gui agent is running.
        if (!IsProcessRunning(exeName, NULL, NULL))
        {
            WCHAR why[128] = L"";
            if (AgentRespawnPointless(why, RTL_NUMBER_OF(why)))
            {
                LogInfo("Process '%s' not running and the system is going down (%s) - "
                    L"not restarting it", exeName, why);
                continue;
            }

            if (startedAt != 0 && GetTickCount64() - startedAt < QUICK_DEATH_MS)
            {
                quickDeaths++;
                if (backoffMs < BACKOFF_MAX_MS)
                {
                    backoffMs *= 2;
                    if (backoffMs > BACKOFF_MAX_MS)
                        backoffMs = BACKOFF_MAX_MS;
                }
                LogWarning("Process '%s' died within %u ms of starting, %u time(s) in a row - "
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

            StartTargetProcess(cmdline);
            startedAt = GetTickCount64();
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

    g_Status.dwServiceType = SERVICE_WIN32;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    // PRESHUTDOWN arrives BEFORE the ordinary shutdown notifications, which is the only chance to
    // know the machine is going down early enough to stop respawning the agent into it.
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
        SERVICE_ACCEPT_PRESHUTDOWN;
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

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

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

    // FIXME that thread never exits
    WaitForSingleObject(workerHandle, INFINITE);

cleanup:
    // don't free cmdline here, a thread using it may be still running, memory is freed on exit anyway
    g_Status.dwCurrentState = SERVICE_STOPPED;
    g_Status.dwWin32ExitCode = GetLastError();
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
        // Earliest reliable "the machine is going down" signal. Only latch the flag: the SCM
        // follows this with SHUTDOWN/STOP, which is where the state transition belongs.
        InterlockedExchange(&g_ServiceStopping, 1);
        LogInfo("preshutdown - the agent will not be restarted from here on");
        break;
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        InterlockedExchange(&g_ServiceStopping, 1);
        g_Status.dwWin32ExitCode = 0;
        g_Status.dwCurrentState = SERVICE_STOPPED;
        LogInfo("stopping...");
        SetServiceStatus(g_StatusHandle, &g_Status);
        break;
    default:
        LogDebug("code 0x%x, event 0x%x", controlCode, eventType);
        break;
    }

    return ERROR_SUCCESS;
}
