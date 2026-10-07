/*
 * lifecycle.c - the agent's end-of-life order. See lifecycle.h; the service side is watchdog/watchdog.c and the
 * contract both sides share is include/qga-lifecycle.h.
 *
 * MEASURED (2026-10-07, QWT 4.3.35): at every guest shutdown Windows' session teardown terminated the agent with
 * 0x40010004 (DBG_TERMINATE_PROCESS) 33-826 ms after the shutdown began - before the agent had any say - and the
 * service relaunched it into the same ending session. The agent's only top-level window (the work-area listener)
 * is created by the hooks thread AFTER Init and handles neither WM_QUERYENDSESSION nor WM_ENDSESSION; one instance
 * was killed while still in Init. So the window here comes FIRST, on its own thread, and the thread does nothing
 * else: it must answer Windows while the main thread is wherever it is (Init, the wait, a vchan send).
 */
#include <windows.h>
#include <strsafe.h>

#include <log.h>

#include "common.h"
#include "qga-exitcodes.h"
#include "qga-lifecycle.h"
#include "lifecycle.h"

// main.c: the main loop's stop event (Global\QGA_SHUTDOWN), created in Init. NULL until then - the main thread is
// in Init at that point and WinMain checks the latch when Init returns, so the loop is never entered.
extern HANDLE g_ShutdownEvent;

static volatile LONG g_ExitCode = 0;          // the latched QGA_EXIT_* code; 0 = none yet
static volatile LONG g_SessionEnding = 0;
static HANDLE g_ExitDone = NULL;              // manual-reset: the orderly exit has run (LifecycleExitDone)
static HANDLE g_Notice = NULL;                // the service's channel, opened by our pid (NULL = started outside the service)
static HANDLE g_Ack = NULL;
static HANDLE g_Continue = NULL;
static HANDLE g_WindowReady = NULL;           // manual-reset: the window thread has its window (or gave up)
static HWND g_Window = NULL;

DWORD LifecycleLatchExit(IN DWORD code)
{
    LONG prev = InterlockedCompareExchange(&g_ExitCode, (LONG)code, 0);
    if (prev != 0 && (DWORD)prev != code)
        LogInfo("QGAEXITCODE 0x%x requested, 0x%x already latched - the first reason stands", code, (DWORD)prev);
    return prev != 0 ? (DWORD)prev : code;
}

DWORD LifecycleExitCode(void)
{
    return (DWORD)InterlockedCompareExchange(&g_ExitCode, 0, 0);
}

BOOL LifecycleSessionEnding(void)
{
    return InterlockedCompareExchange(&g_SessionEnding, 0, 0) != 0;
}

void LifecycleExitDone(void)
{
    if (g_ExitDone)
        SetEvent(g_ExitDone);
}

static LRESULT CALLBACK LifecycleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_QUERYENDSESSION:
    {
        // ORDER (R1): the helpers' relaunch is disarmed BEFORE anything is told to leave or can be ended, then the
        // service is told so it cannot launch another agent into this session, then the end is allowed. Nothing is
        // vetoed here - a shutdown is not ours to refuse.
        const BOOL logoff = (lp & ENDSESSION_LOGOFF) != 0;
        InterlockedExchange(&g_SessionEnding, 1);
        LogInfo("QGAENDSESSION Windows is ending this session (%s, lParam 0x%Ix) - helper launches disarmed first, "
            L"then the service is told", logoff ? L"logoff" : L"shutdown", (ULONG_PTR)lp);
        HelpersDisarm(L"the session is ending");
        if (g_Notice)
        {
            if (!SetEvent(g_Notice))
                win_perror("SetEvent(lifecycle notice)");
            else
            {
                // BOUNDED: a failure detector, not a fix. The service answers from its wait loop within milliseconds;
                // expiry means the service is stuck or gone, and the end is allowed anyway so a stuck service can
                // never stall the shutdown (the handshake-deadlock risk). The service then decides on the exit code
                // alone, with the notice it did see (it is manual-reset: it stays signalled for the service's wait).
                const DWORD w = g_Ack ? WaitForSingleObject(g_Ack, QGA_ENDSESSION_ACK_WAIT_MS) : WAIT_FAILED;
                if (w == WAIT_OBJECT_0)
                    LogInfo("QGAENDSESSION the service acknowledged the notice: no agent is launched into this session again");
                else
                    LogError("QGAENDSESSION the service did NOT acknowledge the end-session notice within %u ms (wait 0x%x) - "
                        L"the end is allowed anyway; the service decides on the exit code and the notice it holds",
                        QGA_ENDSESSION_ACK_WAIT_MS, w);
            }
        }
        else
        {
            LogError("QGAENDSESSION no lifecycle channel to the QubesGuiWatchdog service (this agent was not started by "
                L"it, or the channel could not be opened) - the end is allowed without a notice; the service, if any, "
                L"will see this instance's exit without one");
        }
        return TRUE;
    }
    case WM_ENDSESSION:
        if (wp)
        {
            // THE ORDERLY EXIT, not a termination: the main loop leaves through its existing exit path (helpers told
            // to leave - their relaunch is already disarmed - the vchan closed and its announcement withdrawn, grants
            // revoked as that path decides), and this process ends with QGA_EXIT_SESSION_END. Windows ends the process
            // once every window has returned from this message, so the handler waits for the exit to be done and
            // ends the process ITSELF - returning first would hand Windows the exit (0x40010004, as measured).
            DWORD w;
            LifecycleLatchExit(QGA_EXIT_SESSION_END);
            LogInfo("QGAENDSESSION the session is ending: leaving through the orderly exit with QGA_EXIT_SESSION_END");
            if (g_ShutdownEvent)
                SetEvent(g_ShutdownEvent);
            w = g_ExitDone ? WaitForSingleObject(g_ExitDone, QGA_ENDSESSION_EXIT_WAIT_MS) : WAIT_FAILED;
            if (w == WAIT_OBJECT_0)
            {
                LogInfo("QGAENDSESSION orderly exit complete - exit code QGA_EXIT_SESSION_END");
                LogFlush();
                ExitProcess(QGA_EXIT_SESSION_END);
            }
            // BOUNDED: expiry is a loud ERROR, and the end is allowed. The service will see 0x40010004 after an
            // acknowledged notice (QGA_DECIDE_SESSION_END_FORCED) and launch nothing into this session.
            LogError("QGAENDSESSION the orderly exit did not complete within %u ms (wait 0x%x) - Windows ends this "
                L"process; the service sees the system's termination status after the acknowledged notice",
                QGA_ENDSESSION_EXIT_WAIT_MS, w);
            LogFlush();
            return 0;
        }
        // The end was cancelled by another application's veto: the session continues, with everything re-armed.
        InterlockedExchange(&g_SessionEnding, 0);
        LogWarning("QGAENDSESSION the end of the session was CANCELLED - helper launches re-armed; telling the service "
            L"the session continues");
        HelpersRearm();
        if (g_Continue && !SetEvent(g_Continue))
            win_perror("SetEvent(lifecycle continue)");
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static DWORD WINAPI LifecycleWindowThread(void *param)
{
    static const WCHAR cls[] = L"QubesGuiAgentLifecycle";
    WNDCLASSEX wc;
    MSG msg;

    UNREFERENCED_PARAMETER(param);
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LifecycleWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = cls;
    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        win_perror("RegisterClassEx(lifecycle window)");
        SetEvent(g_WindowReady);
        return ERROR_SUCCESS;
    }
    // Top-level (end-session messages skip HWND_MESSAGE windows) but never shown, so the agent's own tracking
    // rejects it: ShouldAcceptWindow drops invisible windows. Same shape as the work-area listener.
    g_Window = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"", WS_POPUP,
                              0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!g_Window)
        win_perror("CreateWindowEx(lifecycle window)");
    SetEvent(g_WindowReady);
    if (!g_Window)
        return ERROR_SUCCESS;

    // This thread pumps for the life of the process; the process exit ends it.
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return ERROR_SUCCESS;
}

void LifecycleStart(void)
{
    WCHAR name[96];
    const DWORD pid = GetCurrentProcessId();
    HANDLE thread;

    g_ExitDone = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_WindowReady = CreateEvent(NULL, TRUE, FALSE, NULL);

    // The service created the channel before it resumed this process (watchdog.c StartTargetProcess); an agent
    // started by hand, by a dev script or by an older service has none - said once, and the handshake is then a
    // local one (disarm, orderly exit, the defined exit code) with no notice to anybody.
    if (SUCCEEDED(QgaLifecycleObjectName(name, RTL_NUMBER_OF(name), pid, QGA_LIFECYCLE_NOTICE)))
        g_Notice = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (SUCCEEDED(QgaLifecycleObjectName(name, RTL_NUMBER_OF(name), pid, QGA_LIFECYCLE_ACK)))
        g_Ack = OpenEvent(SYNCHRONIZE, FALSE, name);
    if (SUCCEEDED(QgaLifecycleObjectName(name, RTL_NUMBER_OF(name), pid, QGA_LIFECYCLE_CONTINUE)))
        g_Continue = OpenEvent(EVENT_MODIFY_STATE, FALSE, name);
    if (g_Notice && g_Ack && g_Continue)
        LogInfo("QGALIFECYCLE channel to the QubesGuiWatchdog service open (" QGA_LIFECYCLE_NAME_FMT L")", pid, L"*");
    else
    {
        LogWarning("QGALIFECYCLE no channel to the QubesGuiWatchdog service (notice=%d ack=%d continue=%d, error 0x%x) - "
            L"this agent was not started by the service, or an older service started it; a session end is handled "
            L"locally (helpers disarmed, orderly exit, QGA_EXIT_SESSION_END) with no notice to a service",
            g_Notice != NULL, g_Ack != NULL, g_Continue != NULL, GetLastError());
        if (g_Notice) { CloseHandle(g_Notice); g_Notice = NULL; }
        if (g_Ack) { CloseHandle(g_Ack); g_Ack = NULL; }
        if (g_Continue) { CloseHandle(g_Continue); g_Continue = NULL; }
    }

    if (!g_ExitDone || !g_WindowReady)
    {
        win_perror("CreateEvent(lifecycle)");
        LogError("QGALIFECYCLE the end-session window is NOT created - a session end will terminate this agent without "
            L"its orderly exit (the service sees 0x40010004 without a notice)");
        return;
    }
    thread = CreateThread(NULL, 0, LifecycleWindowThread, NULL, 0, NULL);
    if (!thread)
    {
        win_perror("CreateThread(lifecycle window)");
        LogError("QGALIFECYCLE the end-session window thread did not start - a session end will terminate this agent "
            L"without its orderly exit");
        return;
    }
    CloseHandle(thread);   // runs for the life of the process
    // BOUNDED: the window must exist before Init starts opening the vchan, or the first-act promise is empty;
    // expiry is an ERROR and Init proceeds (the agent is then no worse than before this change).
    if (WaitForSingleObject(g_WindowReady, QGA_LIFECYCLE_WINDOW_WAIT_MS) != WAIT_OBJECT_0 || !g_Window)
        LogError("QGALIFECYCLE the end-session window is not up after %u ms (window %p) - a session end may terminate "
            L"this agent without its orderly exit", QGA_LIFECYCLE_WINDOW_WAIT_MS, g_Window);
    else
        LogInfo("QGALIFECYCLE end-session window %p up before Init - a session end runs the orderly exit", g_Window);
}
