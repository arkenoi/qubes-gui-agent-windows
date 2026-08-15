/*
 * workarea - guest work-area synchronization. See workarea.h.
 */
#include <windows.h>
#include <stdio.h>

#include "common.h"
#include "main.h"
#include "workarea.h"
#include "resolution.h"

#include <log.h>
#include <config.h>
#include <qubesdb-client.h>

#define QDB_WORKAREA_PATH "/qubes-workarea"

// Minimum spacing of the WorkAreaEnsureApplied drift compare.
#define WA_DRIFT_CHECK_MS 2000
// Minimum gap between broadcast-driven re-asserts; see WorkAreaReassertThrottled.
#define WA_REASSERT_MIN_MS 1000
// Consecutive failed corrections before the drift check backs off, and the slow rate
// it falls back to; see WorkAreaEnsureApplied.
#define WA_LOST_FIGHT_TRIES 5
#define WA_DRIFT_BACKOFF_MS 30000

static CRITICAL_SECTION g_WaLock;
static BOOL g_WaLockInit = FALSE; // g_WaLock initialized (see WorkAreaLockInit)
static BOOL g_WaInitDone = FALSE; // full init: sources may be read, apply is allowed

// dom0-provided values (qubesdb watcher or MSG_WORKAREA); dom0 root coordinates
static BOOL g_WaDom0Valid = FALSE;
static RECT g_WaDom0;          // work area
static int  g_WaFrame[4];      // l, r, t, b

// inference: smallest daemon-dictated window origin seen (WM placement reveals
// panel + frame offsets); sanity-capped in WaCompute
static int g_WaInferX = -1;
static int g_WaInferY = -1;

static RECT g_WaLastApplied;   // zero until first successful apply

// Broadcast-listener window. Created, destroyed and used ONLY on the window-event
// thread (window handles are thread-affine for DestroyWindow), so no lock.
static HWND g_WaListener = NULL;

static BOOL WaRectSane(const RECT* r)
{
    return r->right - r->left >= 640 && r->bottom - r->top >= 480 &&
           r->left >= 0 && r->top >= 0 &&
           r->right <= (LONG)g_ScreenWidth && r->bottom <= (LONG)g_ScreenHeight;
}

// Compute the target guest work area. Returns FALSE if no source is available.
// g_WaLock must be held.
static BOOL WaCompute(OUT RECT* out)
{
    // 1. registry override: guest-final rect
    WCHAR buf[64];
    if (CfgReadString(NULL, REG_CONFIG_WORKAREA_VALUE, buf, RTL_NUMBER_OF(buf), NULL)
        == ERROR_SUCCESS)
    {
        int x, y, w, h;
        if (swscanf_s(buf, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4)
        {
            RECT r = { x, y, x + w, y + h };
            if (WaRectSane(&r))
            {
                *out = r;
                return TRUE;
            }
            LogWarning("ignoring insane WorkArea config '%s'", buf);
        }
    }

    // 2. dom0-provided work area + frame extents
    if (g_WaDom0Valid)
    {
        RECT usable = g_WaDom0;
        if (usable.left < 0) usable.left = 0;
        if (usable.top < 0) usable.top = 0;
        if (usable.right > (LONG)g_ScreenWidth) usable.right = (LONG)g_ScreenWidth;
        if (usable.bottom > (LONG)g_ScreenHeight) usable.bottom = (LONG)g_ScreenHeight;
        RECT r = { usable.left + g_WaFrame[0], usable.top + g_WaFrame[2],
                   usable.right - g_WaFrame[1], usable.bottom - g_WaFrame[3] };
        if (WaRectSane(&r))
        {
            *out = r;
            return TRUE;
        }
        LogWarning("dom0 workarea (%d,%d)-(%d,%d) f=%d/%d/%d/%d yields insane guest rect",
            g_WaDom0.left, g_WaDom0.top, g_WaDom0.right, g_WaDom0.bottom,
            g_WaFrame[0], g_WaFrame[1], g_WaFrame[2], g_WaFrame[3]);
    }

    // 3. inference from observed daemon window origins: treat the smallest seen
    // origin as the top-left margin, mirror the left margin on the other sides
    if (g_WaInferX >= 0 && g_WaInferY >= 0)
    {
        int mx = g_WaInferX > 16 ? 16 : g_WaInferX;
        int my = g_WaInferY > 64 ? 64 : g_WaInferY;
        RECT r = { mx, my, (LONG)g_ScreenWidth - mx, (LONG)g_ScreenHeight - mx };
        if (WaRectSane(&r))
        {
            *out = r;
            return TRUE;
        }
    }

    return FALSE;
}

// Re-fit maximized windows so they pick up the changed work area. Windows does
// not re-lay-out an already-maximized window on SPI_SETWORKAREA; forcing the
// placement's showCmd through SetWindowPlacement recomputes the maximized rect
// without a visible restore. Enumerates top-level windows directly - no watched
// list access, so no locks are held across cross-process window calls.
static BOOL CALLBACK WaRefitProc(HWND hwnd, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);
    // SetWindowPlacement round-trips synchronously into the target app with no
    // timeout; on the window-event thread (WaWndProc caller) a hung app would stall
    // hook delivery for the duration. Skip hung windows - they re-lay-out from the
    // current work area on their own once they recover.
    if (IsHungAppWindow(hwnd))
        return TRUE;
    if (IsWindowVisible(hwnd) && IsZoomed(hwnd) && !IsIconic(hwnd))
    {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(hwnd, &wp))
        {
            wp.showCmd = SW_MAXIMIZE;
            SetWindowPlacement(hwnd, &wp);
        }
    }
    return TRUE;
}

// Re-apply the current target regardless of the "unchanged" shortcut: used when
// something else overwrote the work area.
void WorkAreaReassert(void)
{
    // Callable from the broadcast listener, which exists before WorkAreaInit runs
    // (the window-event thread starts in Init(), WorkAreaInit only at vchan connect):
    // until then there is no target to re-assert, so this must be a no-op. g_WaLock
    // itself is safe earlier than that (WorkAreaLockInit precedes the thread).
    if (!g_WaInitDone)
        return;

    EnterCriticalSection(&g_WaLock);
    SetRectEmpty(&g_WaLastApplied);
    LeaveCriticalSection(&g_WaLock);
    WorkAreaApply();
}

// Rate-limited entry point for the WM_SETTINGCHANGE(SPI_SETWORKAREA) listener ONLY.
//
// Explorer answers our SPI_SETWORKAREA by recomputing the work area from its own
// taskbar geometry and setting it back, and that set broadcasts SPI_SETWORKAREA to
// us - so re-asserting once per broadcast is a ping-pong. Measured on the clean
// install of a459f0e: 1018 applies in 85 s (~12/s), each running EnumWindows plus
// synchronous cross-process SetWindowPlacement ON THE WINDOW-EVENT THREAD, which is
// also the hook-delivery thread.
//
// The gate leaves the deadline UNTOUCHED when it suppresses: a suppressed call must
// not push the window forward, or a sustained external broadcast train (taskbar
// auto-hide, any app using SPIF_SENDCHANGE) would starve re-asserts indefinitely
// instead of merely rate-limiting them. Only WM_SETTINGCHANGE is gated;
// WM_DISPLAYCHANGE and the drift check call WorkAreaReassert directly, because those
// are already self-limited and are the paths that must not be throttled.
static void WorkAreaReassertThrottled(void)
{
    static volatile LONG nextAllowed; // ticks; 0 = never fired
    if (!g_WaInitDone)
        return;

    DWORD now = GetTickCount();
    LONG seen = InterlockedCompareExchange(&nextAllowed, 0, 0);
    if (seen != 0 && (LONG)(now - (DWORD)seen) < 0)
        return; // still inside the window - deadline deliberately left as is

    LONG deadline = (LONG)(now + WA_REASSERT_MIN_MS);
    if (deadline == 0)
        deadline = 1; // keep 0 meaning "never fired"
    if (InterlockedCompareExchange(&nextAllowed, deadline, seen) != seen)
        return; // another thread just claimed this slot; no retry, no spin

    WorkAreaReassert();
}

void WorkAreaApply(void)
{
    if (!g_WaInitDone)
        return;

    RECT target;
    EnterCriticalSection(&g_WaLock);
    BOOL have = WaCompute(&target);
    BOOL changed = have && !EqualRect(&target, &g_WaLastApplied);
    LeaveCriticalSection(&g_WaLock);

    if (!have || !changed)
        return;

    // CLAMP TO THE DESKTOP WE ACTUALLY HAVE. The dom0 feed describes dom0's window, and the guest
    // desktop can legitimately be smaller - during a mode change it briefly IS smaller. Windows
    // rejects a work area that leaves the screen with ERROR_INVALID_PARAMETER, which was observed
    // repeating every 30 s forever on a guest whose feed still described the previous, larger
    // desktop. A clamped work area is right; an out-of-bounds one is simply refused.
    RECT screen = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    RECT clamped;
    if (IntersectRect(&clamped, &target, &screen) && !IsRectEmpty(&clamped))
    {
        if (!EqualRect(&clamped, &target))
            LogInfo("work area (%d,%d)-(%d,%d) clipped to the %dx%d desktop -> (%d,%d)-(%d,%d)",
                target.left, target.top, target.right, target.bottom,
                screen.right, screen.bottom,
                clamped.left, clamped.top, clamped.right, clamped.bottom);
        target = clamped;
    }

    RECT current;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0) &&
        EqualRect(&current, &target))
        return; // OS already agrees

    // NO SPIF_SENDCHANGE: the broadcast makes Explorer recompute the work area from
    // its own taskbar geometry and immediately overwrite ours (verified in-guest -
    // with flags the value reverts, without them it sticks). Maximized windows are
    // re-fitted explicitly below instead of relying on the WM_SETTINGCHANGE fanout.
    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &target, 0))
    {
        // Say WHAT was refused: "SPI_SETWORKAREA failed with error 0x57" on its own sent us
        // looking for a broken API instead of a bad rectangle.
        LogWarning("SPI_SETWORKAREA refused (%d,%d)-(%d,%d) on a %dx%d desktop, error 0x%x",
            target.left, target.top, target.right, target.bottom,
            screen.right, screen.bottom, GetLastError());
        return;
    }

    // Record the applied rect ONLY once Windows accepted it. It used to be stored before the
    // attempt, so a REFUSED apply was remembered as applied: the next pass computed the same
    // target, saw "unchanged" and returned early, and the guest kept a work area Windows had
    // never accepted, with nothing retrying it.
    EnterCriticalSection(&g_WaLock);
    g_WaLastApplied = target;
    LeaveCriticalSection(&g_WaLock);

    LogInfo("guest work area set to (%d,%d)-(%d,%d)",
        target.left, target.top, target.right, target.bottom);
    EnumWindows(WaRefitProc, 0);
}

BOOL WorkAreaGetApplied(OUT RECT* applied)
{
    EnterCriticalSection(&g_WaLock);
    *applied = g_WaLastApplied;
    LeaveCriticalSection(&g_WaLock);
    return !IsRectEmpty(applied);
}

void WorkAreaSetDom0(int x, int y, int w, int h, int fl, int fr, int ft, int fb)
{
    EnterCriticalSection(&g_WaLock);
    g_WaDom0.left = x;
    g_WaDom0.top = y;
    g_WaDom0.right = x + w;
    g_WaDom0.bottom = y + h;
    g_WaFrame[0] = fl;
    g_WaFrame[1] = fr;
    g_WaFrame[2] = ft;
    g_WaFrame[3] = fb;
    g_WaDom0Valid = TRUE;
    LeaveCriticalSection(&g_WaLock);

    // The feed drives the published IDD mode set (maximize/tile sizes); keep it
    // current. Registry-only on change - no reload, no blink (see resolution.c).
    ResolutionRecomputeIddModeSet();
}

BOOL WorkAreaGetDom0Raw(int* x, int* y, int* w, int* h, int* fl, int* fr, int* ft, int* fb)
{
    if (!g_WaLockInit)
        return FALSE;

    EnterCriticalSection(&g_WaLock);
    BOOL valid = g_WaDom0Valid;
    if (valid)
    {
        *x = g_WaDom0.left;
        *y = g_WaDom0.top;
        *w = g_WaDom0.right - g_WaDom0.left;
        *h = g_WaDom0.bottom - g_WaDom0.top;
        *fl = g_WaFrame[0];
        *fr = g_WaFrame[1];
        *ft = g_WaFrame[2];
        *fb = g_WaFrame[3];
    }
    LeaveCriticalSection(&g_WaLock);
    return valid;
}

void WorkAreaNoteDaemonOrigin(int x, int y)
{
    if (x <= 0 || y <= 0 || x > 100 || y > 200)
        return; // not a plausible top-left placement margin
    BOOL changed = FALSE;
    EnterCriticalSection(&g_WaLock);
    if (g_WaInferX < 0 || x < g_WaInferX) { g_WaInferX = x; changed = TRUE; }
    if (g_WaInferY < 0 || y < g_WaInferY) { g_WaInferY = y; changed = TRUE; }
    LeaveCriticalSection(&g_WaLock);
    if (changed)
        WorkAreaApply();
}

static BOOL WaParseDom0Value(const char* v)
{
    int x, y, w, h, fl, fr, ft, fb;
    if (sscanf_s(v, "%d %d %d %d %d %d %d %d",
                 &x, &y, &w, &h, &fl, &fr, &ft, &fb) != 8)
    {
        LogWarning("unparsable %S value: '%S'", QDB_WORKAREA_PATH, v);
        return FALSE;
    }
    WorkAreaSetDom0(x, y, w, h, fl, fr, ft, fb);
    return TRUE;
}

// One read of /qubes-workarea -> WorkAreaSetDom0. Returns TRUE if a valid value
// was read and stored.
static BOOL WaReadDom0(qdb_handle_t h)
{
    unsigned len = 0;
    char* v = qdb_read(h, QDB_WORKAREA_PATH, &len);
    if (!v)
        return FALSE;
    BOOL ok = WaParseDom0Value(v);
    free(v);
    return ok;
}

BOOL WorkAreaSyncReadDom0(void)
{
    if (!g_WaLockInit)
        return FALSE; // WorkAreaSetDom0 takes g_WaLock; too early to read

    qdb_handle_t h = qdb_open(NULL);
    if (!h)
        return FALSE;
    BOOL ok = WaReadDom0(h);
    qdb_close(h);
    return ok;
}

// Watch thread: deliver /qubes-workarea (written by the dom0 watcher script)
// and its changes. The value survives dom0 panel/monitor hotplug because the
// watcher re-writes on _NET_WORKAREA changes. Reconnects if qubesdb-daemon
// restarts. Runs for the life of the process.
static DWORD WINAPI WaWatchThread(PVOID param)
{
    UNREFERENCED_PARAMETER(param);
    for (;;)
    {
        qdb_handle_t h = qdb_open(NULL);
        if (!h)
        {
            Sleep(30000);
            continue;
        }

        if (WaReadDom0(h))
            WorkAreaApply();

        if (qdb_watch(h, QDB_WORKAREA_PATH))
        {
            char* ev;
            while ((ev = qdb_read_watch(h)) != NULL)
            {
                free(ev);
                if (WaReadDom0(h))
                    WorkAreaApply();
            }
        }

        qdb_close(h);
        LogDebug("qubesdb connection lost, retrying");
        Sleep(30000);
    }
}

// Broadcast listener: Windows notifies work-area/display changes with
// WM_SETTINGCHANGE(SPI_SETWORKAREA) and WM_DISPLAYCHANGE, which are sent to top-level
// windows only (a message-only window would never see them). Explorer recomputes the
// work area from its own taskbar geometry on those events and overwrites ours, so this
// is exactly where to re-assert - no polling, no timer.
static LRESULT CALLBACK WaWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA)
        {
            // Our own SPI_SETWORKAREA does not broadcast (see WorkAreaApply), so any
            // such notification is somebody else changing it: re-assert ours.
            LogDebug("WM_SETTINGCHANGE(SPI_SETWORKAREA): re-asserting");
            WorkAreaReassertThrottled();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        LogDebug("WM_DISPLAYCHANGE %ux%u: re-asserting work area",
            (UINT)LOWORD(lp), (UINT)HIWORD(lp));
        // The monitor rect / display mode GetRealWindowRect caches per HMONITOR
        // ("MonInfoCache") can only change on this event; drop the cache before
        // anything recomputes geometry from it.
        MonitorCacheInvalidate();
        // The mode ALREADY changed (this is a notification, not a request); make the
        // agent's believed size follow reality before the work area is recomputed, or
        // WaCompute clamps against a stale screen and SPI_SETWORKAREA rejects the rect.
        ResolutionAdoptCurrent();
        WorkAreaReassert();
        // A display may have been ATTACHED, not just resized - the IddCx monitor arriving
        // during install is the routine case. Two attached displays grow the desktop past
        // what dom0 sees, so re-assert solo (debounced, on its own thread).
        ResolutionRequestIddSoloReassert();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Drift check for the tracking paths: if the OS work area no longer matches what we
// last applied (Explorer recomputed it and the broadcast listener missed it or is
// down), re-assert. Self-rate-limited to one SPI_GETWORKAREA read per
// WA_DRIFT_CHECK_MS regardless of which caller gets there, so it works no matter
// which path drains the tracking tick. Both call sites (frame path and event path in
// main.c) run on the main-loop thread with no agent locks held; the check only takes
// g_WaLock for a struct copy and releases it before any SPI/window call.
//
// Coverage: fires as long as frames or window events arrive - including after
// hook-thread death (frames still flow). NOT covered: a fully idle desktop (no
// frames, no events); only the broadcast listener catches an overwrite there.
void WorkAreaEnsureApplied(void)
{
    static DWORD lastCheck; // main-loop thread only (both call sites)

    if (!g_WaInitDone)
        return;

    // Backoff for a work area we cannot win. If something (Explorer recomputing from
    // its own taskbar geometry) reverts our value every single time, correcting it
    // every WA_DRIFT_CHECK_MS forever is a permanent low-rate fight: an EnumWindows +
    // cross-process SetWindowPlacement sweep every 2 s for the life of the qube, with
    // maximized windows re-fitted and then abandoned each cycle. After
    // WA_LOST_FIGHT_TRIES consecutive corrections that did not stick, drop to
    // WA_DRIFT_BACKOFF_MS and say so once; any check that finds our value intact
    // resets both. Correctness is unchanged - we still re-assert, just not at a rate
    // that buys nothing.
    static unsigned lostInARow;
    static BOOL backoffLogged;
    DWORD interval = (lostInARow >= WA_LOST_FIGHT_TRIES) ? WA_DRIFT_BACKOFF_MS : WA_DRIFT_CHECK_MS;

    DWORD now = GetTickCount();
    if (now - lastCheck < interval)
        return;
    lastCheck = now;

    // Same cadence, same thread: catch an out-of-band display-mode reversion even if
    // the WM_DISPLAYCHANGE broadcast raced the belief update or the listener is down.
    // One EnumDisplaySettings read per tick; no-op when belief and mode agree.
    ResolutionAdoptCurrent();

    RECT applied;
    EnterCriticalSection(&g_WaLock);
    applied = g_WaLastApplied;
    LeaveCriticalSection(&g_WaLock);

    if (IsRectEmpty(&applied))
        return; // never applied anything - nothing to defend

    RECT current;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0))
        return;

    if (EqualRect(&current, &applied))
    {
        lostInARow = 0; // our value held: the fight, if there was one, is over
        backoffLogged = FALSE;
        return;
    }

    lostInARow++;
    if (lostInARow >= WA_LOST_FIGHT_TRIES && !backoffLogged)
    {
        backoffLogged = TRUE;
        LogWarning("work area reverted %u checks in a row (OS insists on (%d,%d)-(%d,%d)); "
            "backing off re-assert to %u ms",
            lostInARow, current.left, current.top, current.right, current.bottom,
            (unsigned)WA_DRIFT_BACKOFF_MS);
    }
    else if (!backoffLogged)
    {
        LogInfo("work area drifted: OS has (%d,%d)-(%d,%d), ours was (%d,%d)-(%d,%d); re-asserting",
            current.left, current.top, current.right, current.bottom,
            applied.left, applied.top, applied.right, applied.bottom);
    }
    WorkAreaReassert();
}

void WorkAreaCreateListener(void)
{
    static const WCHAR cls[] = L"QubesGuiAgentWorkArea";
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = WaWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = cls;

    // Never let WaWndProc become reachable with g_WaLock uninitialized (its
    // re-assert path enters it). Init() calls WorkAreaLockInit before starting the
    // window-event thread, so this cannot trigger; it guards future reordering.
    if (!g_WaLockInit)
    {
        LogError("workarea lock not initialized - listener not created (call WorkAreaLockInit first)");
        return;
    }

    if (g_WaListener) // rearm without an intervening destroy: replace, don't leak
        WorkAreaDestroyListener();

    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        win_perror("RegisterClassEx(workarea listener)");
        return;
    }
    // Top-level (broadcasts skip HWND_MESSAGE windows) but never shown, so the agent's
    // own tracking rejects it: ShouldAcceptWindow drops invisible windows.
    HWND h = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!h)
    {
        win_perror("CreateWindowEx(workarea listener)");
        return;
    }
    g_WaListener = h;
    LogDebug("work-area listener window %p created", h);
}

void WorkAreaDestroyListener(void)
{
    if (!g_WaListener)
        return;
    if (!DestroyWindow(g_WaListener))
    {
        // Loud on purpose: the thread would still own a window, so every subsequent
        // SetThreadDesktop rearm fails and window tracking silently degrades to the
        // periodic resync. Should be unreachable (same-thread destroy of a valid
        // window); clearing the handle anyway keeps create/destroy re-runnable.
        LogError("DestroyWindow(workarea listener %p) FAILED - window-event thread may no longer be able to switch desktops", g_WaListener);
        win_perror("DestroyWindow(workarea listener)");
    }
    g_WaListener = NULL;
}

void WorkAreaLockInit(void)
{
    if (g_WaLockInit)
        return;
    InitializeCriticalSection(&g_WaLock);
    g_WaLockInit = TRUE;
}

void WorkAreaInit(void)
{
    if (g_WaInitDone)
        return;
    WorkAreaLockInit(); // no-op when Init() already ran it (the normal path)
    g_WaInitDone = TRUE;

    // One synchronous read of the dom0 feed before the watch thread exists:
    // consumers that run right after this in the connect sequence (WorkAreaApply,
    // the M6 boot mode-set publish) must not race the thread's first read.
    // Failure is fine - the thread retries for the life of the process.
    WorkAreaSyncReadDom0(); // apply happens in the connect sequence right after

    HANDLE t = CreateThread(NULL, 0, WaWatchThread, NULL, 0, NULL);
    if (t)
        CloseHandle(t); // fire-and-forget; lives until process exit
    else
        win_perror("CreateThread(WaWatchThread)");
}
