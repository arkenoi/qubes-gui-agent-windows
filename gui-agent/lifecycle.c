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
static HANDLE g_Done = NULL;                  // agent -> service: the orderly exit COMPLETED (qga-lifecycle.h)
static HANDLE g_WindowReady = NULL;           // manual-reset: the window thread has its window (or gave up)
static HWND g_Window = NULL;
// The session THIS agent published as going down (QgaGoingDownSet), or QGA_NO_SESSION when nothing was
// published - a failed publish must not be followed by releasing a latch this agent does not own.
static DWORD g_GoingDownSession = QGA_NO_SESSION;

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

// THE ORDERLY CONTRACT IS SATISFIED. Separate from LifecycleExitDone because the two meanings were
// one call, and that is what left the signal too late: `done` was set only once the main thread was
// actually leaving - after the window-event thread's 2 s join - while the work the service cares
// about (the vchan withdrawal, the window-0 unmap sweep, the staging grant) had finished long
// before. Windows reaps the process during session teardown, so a shutdown could land in that gap
// and the service would read 0x40010004 with no `done` set and call a COMPLETED exit forced.
// Measured on a clean shutdown: the agent logged "QGAENDSESSION orderly exit complete" at
// 153432.110 and the watchdog logged QGAWDSESSIONEND-FORCED at 153432.214 - 104 ms later, with the
// 10 s budget never approached. This is "if something runs too early, run it in time" read the
// other way round: the signal ran too LATE.
//
// Manual-reset, so calling it twice is idempotent and LifecycleExitDone may still set it.
void LifecycleOrderlyComplete(void)
{
    if (g_Done)
        SetEvent(g_Done);
}

void LifecycleExitDone(void)
{
    // THE SERVICE IS TOLD HERE, on the thread that finished the work and in the same breath - not by whoever
    // wakes on g_ExitDone. The gap between "the orderly exit is complete" and "the service knows it" is what
    // makes a completed exit read as a forced one when the system's kill lands inside it, and a handoff to
    // another thread is a much wider gap than this. What remains is the few instructions between the last
    // orderly step and this SetEvent, which no signal can remove.
    // ONE setter, called from here too: the Init-failure paths and the ordinary leave both still
    // need `done` set, and on those paths no vchan was ever opened so there is nothing to withdraw.
    // The event is manual-reset, so a second call after LifecycleOrderlyComplete() is a no-op.
    LifecycleOrderlyComplete();
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
        // THE DURABLE FORM OF THE SAME FACT, published before anything is told and before any wait, so that it
        // holds even if this process is terminated inside the wait below - which is what Windows measurably does
        // (33-826 ms after the shutdown begins, per the header of this file). HelpersDisarm covers launches in
        // THIS process from here on; the latch covers every other launcher, and this service after a restart,
        // neither of which can have received the message. Our own session, not the console session: the session
        // that is ending is mine, and the console session may already be moving.
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &g_GoingDownSession))
        {
            g_GoingDownSession = QGA_NO_SESSION;
            win_perror("ProcessIdToSessionId(self, going-down latch)");
        }
        else
        {
            LONG st = 0;
            if (QgaGoingDownSet(g_GoingDownSession, &st))
                LogInfo("QGAGOINGDOWN session %u published as going down - no launch into it from any process "
                    L"until it ends", g_GoingDownSession);
            else
            {
                // A launcher that cannot read the latch falls back to what it had before it existed; say which
                // call failed so the status is interpretable (0xC0000061 = the privilege is not held).
                if (st != 0)
                    LogError("QGAGOINGDOWN could not publish session %u as going down: NT status 0x%08x; launchers "
                        L"outside this process are not told", g_GoingDownSession, (DWORD)st);
                else
                    win_perror("QgaGoingDownSet");
                g_GoingDownSession = QGA_NO_SESSION;
            }
        }
        if (g_Notice)
        {
            if (!SetEvent(g_Notice))
                win_perror("SetEvent(lifecycle notice)");
            else
            {
                // BOUNDED: a failure detector, not a fix. The service answers from its wait loop within milliseconds;
                // expiry means the service is stuck or gone, and the end is allowed anyway so a stuck service can
                // never stall the shutdown (the handshake-deadlock risk). The service then decides on the exit code
                // alone, with the end-session notice it did see and holds (it is manual-reset: it stays signalled
                // for the service's wait).
                const DWORD w = g_Ack ? WaitForSingleObject(g_Ack, QGA_ENDSESSION_ACK_WAIT_MS) : WAIT_FAILED;
                if (w == WAIT_OBJECT_0)
                    LogInfo("QGAENDSESSION session end acknowledged, no agent is launched into it again");
                else
                    LogError("QGAENDSESSION the service did not acknowledge the session end within %u ms (wait 0x%x); "
                        L"the session ends anyway", QGA_ENDSESSION_ACK_WAIT_MS, w);
            }
        }
        else
        {
            // No lifecycle channel to the QubesGuiWatchdog service: either the service did not start this agent,
            // or the channel could not be opened (LifecycleStart said which, at QGALIFECYCLE). The end is allowed
            // without a notice; a service, if one exists, sees this instance's exit with no notice ahead of it.
            // The code here establishes neither cause, so the line names none.
            LogError("QGAENDSESSION session ending; no channel to the watchdog service, it is not told");
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
            LogInfo("QGAENDSESSION the session is ending, taking the orderly exit (QGA_EXIT_SESSION_END)");
            if (g_ShutdownEvent)
                SetEvent(g_ShutdownEvent);
            w = g_ExitDone ? WaitForSingleObject(g_ExitDone, QGA_ENDSESSION_EXIT_WAIT_MS) : WAIT_FAILED;
            if (w == WAIT_OBJECT_0)
            {
                // The service already knows: LifecycleExitDone signalled `done` on the thread that finished
                // the work. Measured 2026-10-07: this line went out at 153432.110 and the watchdog recorded
                // the forced end at 153432.214 - one ERROR per shutdown for an exit that had completed.
                LogInfo("QGAENDSESSION orderly exit complete - exit code QGA_EXIT_SESSION_END");
                LogFlush();
                ExitProcess(QGA_EXIT_SESSION_END);
            }
            // BOUNDED: expiry is a loud ERROR, and the end is allowed although the orderly exit did not complete:
            // Windows ends this process, and the service sees the system's termination status 0x40010004 after an
            // acknowledged notice (QGA_DECIDE_SESSION_END_FORCED) and launches nothing into this session.
            LogError("QGAENDSESSION the orderly exit did not finish within %u ms (wait 0x%x); Windows ends the agent now",
                QGA_ENDSESSION_EXIT_WAIT_MS, w);
            LogFlush();
            return 0;
        }
        // The end was cancelled by another application's veto: the session continues, with everything re-armed
        // (the helper launches), telling the service so through the continue event.
        InterlockedExchange(&g_SessionEnding, 0);
        LogWarning("QGAENDSESSION session end CANCELLED, the session continues; helpers re-armed");
        HelpersRearm();
        // WITHDRAW THE DURABLE FACT, because it has stopped being true and this session keeps its id: a latch
        // left behind here would refuse every launch into a live session until it logs off. This process is the
        // one that learns of the veto, straight from Windows, with no service in the path - which is why the
        // release is here and not only in the service. If this agent dies inside the veto instead, the service's
        // SESSION_END_NEVER_HAPPENED_MS detector is what clears it (watchdog.c).
        if (g_GoingDownSession != QGA_NO_SESSION)
        {
            const int rel = QgaGoingDownRelease(g_GoingDownSession);
            if (rel == 1)
                LogInfo("QGAGOINGDOWN session %u withdrawn - launches into it are allowed again",
                    g_GoingDownSession);
            else if (rel == 0)
                // The end state is the one we wanted, by another route: no latch, so nothing refuses. Said at
                // WARNING and not ERROR because nothing is broken for the user - but this agent published it
                // and something else removed it, which is not a path this code knows about.
                LogWarning("QGAGOINGDOWN session %u was already withdrawn by the time this agent released it - "
                    L"launches into it are allowed", g_GoingDownSession);
            else
                LogError("QGAGOINGDOWN could not withdraw session %u: launches into this LIVE session stay "
                    L"refused until it logs off", g_GoingDownSession);
            g_GoingDownSession = QGA_NO_SESSION;
        }
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
    if (SUCCEEDED(QgaLifecycleObjectName(name, RTL_NUMBER_OF(name), pid, QGA_LIFECYCLE_DONE)))
        g_Done = OpenEvent(EVENT_MODIFY_STATE, FALSE, name);
    if (g_Notice && g_Ack && g_Continue && g_Done)
        LogInfo("QGALIFECYCLE channel to the QubesGuiWatchdog service open (" QGA_LIFECYCLE_NAME_FMT L")", pid, L"*");
    else
    {
        // No channel: this agent was not started by the QubesGuiWatchdog service, or an older service started it.
        // A session end is then handled locally - helpers disarmed, the orderly exit, QGA_EXIT_SESSION_END - with
        // no notice to any service.
        LogWarning("QGALIFECYCLE no channel to the watchdog service (notice=%d ack=%d continue=%d, error 0x%x); "
            L"a session end is handled locally, no notice",
            g_Notice != NULL, g_Ack != NULL, g_Continue != NULL, GetLastError());
        if (g_Notice) { CloseHandle(g_Notice); g_Notice = NULL; }
        if (g_Ack) { CloseHandle(g_Ack); g_Ack = NULL; }
        if (g_Continue) { CloseHandle(g_Continue); g_Continue = NULL; }
    }

    if (!g_ExitDone || !g_WindowReady)
    {
        win_perror("CreateEvent(lifecycle)");
        // Without the window, a session end terminates the agent outright (Windows will terminate every process in
        // the session): the service sees 0x40010004 and no notice.
        LogError("QGALIFECYCLE end-session window not created; a session end kills this agent without its orderly exit");
        return;
    }
    thread = CreateThread(NULL, 0, LifecycleWindowThread, NULL, 0, NULL);
    if (!thread)
    {
        win_perror("CreateThread(lifecycle window)");
        LogError("QGALIFECYCLE end-session window thread did not start; a session end skips the orderly exit");
        return;
    }
    CloseHandle(thread);   // runs for the life of the process
    // BOUNDED: the window must exist before Init starts opening the vchan, or the first-act promise is empty
    // (with it up a session end runs the orderly exit; without it a session end may terminate this agent); expiry
    // is an ERROR and Init proceeds (the agent is then no worse than before this change).
    if (WaitForSingleObject(g_WindowReady, QGA_LIFECYCLE_WINDOW_WAIT_MS) != WAIT_OBJECT_0 || !g_Window)
        LogError("QGALIFECYCLE the end-session window is not up after %u ms (window %p); a session end may skip the orderly exit",
            QGA_LIFECYCLE_WINDOW_WAIT_MS, g_Window);
    else
        LogInfo("QGALIFECYCLE end-session window %p up before Init; a session end takes the orderly exit", g_Window);
}
