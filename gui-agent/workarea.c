/*
 * workarea - guest work-area synchronization. See workarea.h.
 */
#include <windows.h>
#include <stdio.h>

#include "common.h"
#include "main.h"
#include "workarea.h"

#include <log.h>
#include <config.h>
#include <qubesdb-client.h>

#define QDB_WORKAREA_PATH "/qubes-workarea"

static CRITICAL_SECTION g_WaLock;
static BOOL g_WaInitDone = FALSE;

// dom0-provided values (qubesdb watcher or MSG_WORKAREA); dom0 root coordinates
static BOOL g_WaDom0Valid = FALSE;
static RECT g_WaDom0;          // work area
static int  g_WaFrame[4];      // l, r, t, b

// inference: smallest daemon-dictated window origin seen (WM placement reveals
// panel + frame offsets); sanity-capped in WaCompute
static int g_WaInferX = -1;
static int g_WaInferY = -1;

static RECT g_WaLastApplied;   // zero until first successful apply

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

void WorkAreaApply(void)
{
    if (!g_WaInitDone)
        return;

    RECT target;
    EnterCriticalSection(&g_WaLock);
    BOOL have = WaCompute(&target);
    BOOL changed = have && !EqualRect(&target, &g_WaLastApplied);
    if (changed)
        g_WaLastApplied = target;
    LeaveCriticalSection(&g_WaLock);

    if (!have || !changed)
        return;

    RECT current;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0) &&
        EqualRect(&current, &target))
        return; // OS already agrees

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &target,
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
    {
        win_perror("SPI_SETWORKAREA");
        return;
    }
    LogInfo("guest work area set to (%d,%d)-(%d,%d)",
        target.left, target.top, target.right, target.bottom);
    EnumWindows(WaRefitProc, 0);
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

        unsigned len = 0;
        char* v = qdb_read(h, QDB_WORKAREA_PATH, &len);
        if (v)
        {
            if (WaParseDom0Value(v))
                WorkAreaApply();
            free(v);
        }

        if (qdb_watch(h, QDB_WORKAREA_PATH))
        {
            char* ev;
            while ((ev = qdb_read_watch(h)) != NULL)
            {
                free(ev);
                v = qdb_read(h, QDB_WORKAREA_PATH, &len);
                if (v)
                {
                    if (WaParseDom0Value(v))
                        WorkAreaApply();
                    free(v);
                }
            }
        }

        qdb_close(h);
        LogDebug("qubesdb connection lost, retrying");
        Sleep(30000);
    }
}

void WorkAreaInit(void)
{
    if (g_WaInitDone)
        return;
    InitializeCriticalSection(&g_WaLock);
    g_WaInitDone = TRUE;
    HANDLE t = CreateThread(NULL, 0, WaWatchThread, NULL, 0, NULL);
    if (t)
        CloseHandle(t); // fire-and-forget; lives until process exit
    else
        win_perror("CreateThread(WaWatchThread)");
}
