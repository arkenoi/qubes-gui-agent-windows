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

#define DEBUG_DUMP_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <dwmapi.h>
#include <Psapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "capture.h"
#include "common.h"
#include "main.h"
#include "vchan.h"
#include "resolution.h"
#include "send.h"
#include "perwindow.h"
#include "workarea.h"
#include "wincapture.h"
#include "vchan-handlers.h"
#include "util.h"
#include "debug.h"
#include "perf.h"
#include "qubes-io.h"

// windows-utils
#include <log.h>
#include <config.h>
#include <exec.h>
#include <qubesdb-client.h>

#include <strsafe.h>

#define FULLSCREEN_ON_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_OFF"

extern struct libvchan *g_Vchan;

// FIXME: too much global state accessed from everywhere

DWORD g_ScreenHeight;
DWORD g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;
BOOL g_SeamlessMode = TRUE;
LONG g_ScreenWinX = 0;
LONG g_ScreenWinY = 0;

// after we send MSG_DESTROY in fullscreen mode we can get delayed MSG_CONFIGURE,
// we shouldn't reply to that before sending MSG_CREATE
BOOL g_LocalScreenDestroyed = FALSE;

// NEVEREXIT: whether MSG_CREATE for the screen window (id 0) is currently outstanding
// with the connected daemon. Written only by StartFrameProcessing (set after a
// successful CREATE) and StopFrameProcessing (cleared after a successful DESTROY),
// both on the main loop thread. Two consumers:
//   - StartFrameProcessing skips the CREATE when set: the A7 degraded state retries
//     it after PARTIAL failures, and gui-daemon exit(1)s on a duplicate CREATE for an
//     existing window id (xside.c "CREATE for already existing window id");
//   - SetSeamlessMode refuses to send window-0 MAP/UNMAP when clear: any window-0
//     message before CREATE(0) hits gui-daemon's "msg without CREATE" exit(1).
static BOOL g_ScreenAnnounced = FALSE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
// Live desktop image (the granted DDA framebuffer) and its pitch, refreshed every
// frame. Valid for the life of the duplication - the daemon reads it continuously -
// so paths outside the frame loop (composite synthesis activation) can sample it too.
static const BYTE* g_FbBits = NULL;
static int g_FbPitch = 0;
// Dimensions the published framebuffer was actually granted/mapped at. With A6 the
// capture geometry can change in place (RecreateDuplication adopts a new desc), so the
// readers above must clamp against THESE, not g_ScreenWidth/Height: the g_Screen*
// globals are written by the resolution thread and can lag the live surface, and a
// stale-larger value would walk the copy loops past the end of the mapped surface.
static UINT g_FbWidth = 0;
static UINT g_FbHeight = 0;

DWORD g_HostScreenWidth = 0;
DWORD g_HostScreenHeight = 0;

// Registry gate REG_CONFIG_STAGING_VALUE ("StagingGrant", default ON): use the
// persistent staging framebuffer grant (capture.c) instead of granting the mapped
// DXGI desktop surface per capture generation. One registry flip + agent restart
// gives the A/B against the direct-map path.
BOOL g_StagingGrant = TRUE;

// minimal acceptable window dimensions
DWORD g_MinWindowWidth = 0;
DWORD g_MinWindowHeight = 0;

char g_DomainName[256] = "<unknown>";
USHORT g_GuiDomainId = 0;

// TODO: group the list and global window handles in a struct with some accessors
LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;

HWND g_DesktopWindow = NULL;
HWND g_TaskbarWindow = NULL;
BOOL g_ShowTaskbar = FALSE;

// these two are always present and "visible" to the winuser APIs regardless of their true state
// may be cloaked by DWM if hidden
// FIXME: search is sometimes "visible" (for winuser) and not DWM cloaked but it's really invisible, detect this state better
BOOL g_StartVisible = FALSE;
HWND g_StartWindow = NULL;
HWND g_SearchWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

/*
 * Event-driven window tracking.
 *
 * The watched window list used to be rebuilt by an EnumWindows() pass on every
 * captured frame. Measured on a stock Windows 10 guest that pass took 23 ms
 * (median, up to 177 ms) per frame and accounted for 97.5% of all frame
 * processing time: the guest has ~70 top-level windows, only one of which is
 * usually eligible, and every ineligible one was re-interrogated from scratch on
 * every frame (each interrogation is a cross-process WM_GETTEXT plus an RPC to
 * DWM).
 *
 * Instead a dedicated thread owns a set of WinEvent hooks and only records which
 * window handles something happened to; the main loop interrogates just those.
 * See PHASE2A-NOTES.md for the design rationale.
 */

// Interval of the full EnumWindows() resync that backstops the event stream (see
// TakePendingWindows). Two seconds: a resync is cheap now that ineligible windows
// are cached (nothing is interrogated unless it visibly changed), and it bounds
// how long a window could stay missing from the gui daemon to something a user
// would call a glitch rather than a hang.
#define WINDOW_RESYNC_INTERVAL_MS 2000

// Maximum number of distinct window handles queued between two drains. On
// overflow we fall back to a full resync instead of dropping an event.
#define PENDING_WINDOWS_MAX 256

// Maximum number of remembered ineligible windows.
#define REJECTED_WINDOWS_MAX 256

// Handles the hook thread waits on besides its message queue: stop, rearm.
#define WINDOW_EVENT_WAIT_COUNT 2

// Placeholder window of a minimized UAC prompt, see GetWindowData().
#define UAC_DUMMY_WINDOW_CLASS L"$$$Secure UAP Dummy Window Class For Interim Dialog"

// Frame shadow strips Office draws around its own windows, see ShouldAcceptWindow().
#define MSO_BORDER_EFFECT_CLASS L"MSO_BORDEREFFECT_WINDOW_CLASS"

// Written by the hook thread, drained by the main loop.
static CRITICAL_SECTION g_csWindowEvents;
static HWND g_PendingWindows[PENDING_WINDOWS_MAX];
// Event id that queued each pending window, parallel to g_PendingWindows. Needed because
// the admit path must know WHY a window was queued: for events whose effect is visible in
// the cheap signature (LOCATIONCHANGE/STATECHANGE/NAMECHANGE) a cached rejection is still
// valid and the expensive interrogation can be skipped, but CLOAKED/UNCLOAKED change
// eligibility WITHOUT touching style/rect, so those must always force re-examination.
static DWORD g_PendingEvents[PENDING_WINDOWS_MAX];
static UINT g_PendingWindowsCount = 0;
static BOOL g_ResyncRequested = TRUE; // the first pass is always a full enumeration

// Main loop only, guarded by nothing: read and written by the thread that drains events.
static DWORD g_LastResyncTime = 0;

static HANDLE g_WindowEventSignal = NULL; // auto-reset: window events are waiting
static HANDLE g_WindowEventRearm = NULL;  // auto-reset: reattach to the input desktop, rearm hooks
static HANDLE g_WindowEventStop = NULL;   // manual-reset: leave the hook thread
static HANDLE g_WindowEventThread = NULL;

// Set when the hook thread dies. Without hooks the only tracking left is the periodic
// resync, so keep it short: at WINDOW_RESYNC_INTERVAL_MS window moves would reach dom0 at
// 0.5 Hz, which is WORSE than the per-frame enumeration this replaced. Falling back to
// roughly the old cadence is a regression to par, not below it.
static volatile LONG g_WindowEventThreadDead = 0;
#define WINDOW_RESYNC_FALLBACK_MS 150

#ifndef EVENT_OBJECT_CLOAKED // Win8+ SDK
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

static const struct
{
    DWORD Min;
    DWORD Max;
} g_HookedEventRanges[] =
{
    { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND }, // minimize/restore
    { EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH }, // secure desktop etc
    { EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE }, // create/destroy/show/hide
    { EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_NAMECHANGE }, // state/location/name (window moves)
    { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED }, // DWM cloaking = invisible for us
};

// TRUE for events whose effect on eligibility is INVISIBLE to the cheap reject-cache
// signature (pid/tid/style/exstyle/rect), so a cached rejection cannot be trusted and the
// window must be interrogated again.
//   - CLOAKED/UNCLOAKED: DWM cloaking is not a window style; a cloaked window keeps its
//     rect and styles, so nothing in the signature moves.
//   - CREATE/DESTROY/SHOW/HIDE: shell/lifecycle transitions; also update the globals
//     (g_ShowTaskbar, g_StartVisible) that gate other windows.
//   - NAMECHANGE: the caption is part of what is sent to dom0 and is not in the signature.
// LOCATIONCHANGE and STATECHANGE are deliberately NOT here: both move the rect or the
// style, so IsWindowRejected() detects them by itself, and those are exactly the events a
// dragged window emits at input rate. Re-interrogating on them would reinstate the ~340 us
// per-window cost that Phase 2A exists to remove.
static BOOL WindowEventForcesReexamine(IN DWORD event)
{
    switch (event)
    {
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_NAMECHANGE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
        return TRUE;
    default:
        return FALSE;
    }
}

// Record that something happened to a window (or, with resync=TRUE, that the
// whole list needs to be rebuilt). Called from the hook thread.
static void QueueWindowEvent(IN HWND window, IN DWORD event, IN BOOL resync)
{
    BOOL signal = TRUE;

    EnterCriticalSection(&g_csWindowEvents);
    if (resync)
    {
        g_PendingWindowsCount = 0;
        g_ResyncRequested = TRUE;
    }
    // NOTE: there is deliberately no "else if (g_ResyncRequested) signal = FALSE" shortcut.
    // It looks free (a pending resync does cover every window) but it depends on an
    // invariant DiscardWindowEvents() breaks: that function also sets g_ResyncRequested,
    // without signalling, and only TakePendingWindows() clears it. After a
    // seamless-off -> on transition the flag can remain set with no signal outstanding, and
    // from then on EVERY event takes the suppressed path, so window moves silently fall
    // back to capture rate. Signalling an auto-reset event is far cheaper than that.
    else
    {
        UINT i;

        for (i = 0; i < g_PendingWindowsCount; i++)
        {
            if (g_PendingWindows[i] == window)
                break;
        }

        if (i < g_PendingWindowsCount)
        {
            // Already queued. Keep the reason that forces the most work, so a cheap
            // LOCATIONCHANGE cannot mask a CLOAKED that arrived for the same window.
            if (WindowEventForcesReexamine(event))
                g_PendingEvents[i] = event;
            signal = FALSE; // already queued and not drained yet
        }
        else if (g_PendingWindowsCount < PENDING_WINDOWS_MAX)
        {
            g_PendingEvents[g_PendingWindowsCount] = event;
            g_PendingWindows[g_PendingWindowsCount++] = window;
        }
        else
        {
            // We can't keep up with the event rate; don't lose anything, resync.
            g_PendingWindowsCount = 0;
            g_ResyncRequested = TRUE;
        }
    }
    LeaveCriticalSection(&g_csWindowEvents);

    if (signal)
        SetEvent(g_WindowEventSignal);
}

// Hand the queued window handles to the caller and tell it whether a full
// enumeration is due. Called from the main loop only.
static BOOL TakePendingWindows(OUT HWND* buffer, OUT DWORD* events, IN UINT capacity, OUT UINT* count)
{
    BOOL resync;
    DWORD now;

    EnterCriticalSection(&g_csWindowEvents);
    resync = g_ResyncRequested;
    g_ResyncRequested = FALSE;
    *count = 0;
    if (!resync)
    {
        *count = g_PendingWindowsCount < capacity ? g_PendingWindowsCount : capacity;
        memcpy(buffer, g_PendingWindows, (*count) * sizeof(HWND));
        memcpy(events, g_PendingEvents, (*count) * sizeof(DWORD));
    }
    g_PendingWindowsCount = 0;
    LeaveCriticalSection(&g_csWindowEvents);

    // Safety net: an event can be lost (the hooks are down for a moment while we
    // reattach to a new input desktop, the system coalesces or drops events if the
    // hook thread falls behind), and some inputs of ShouldAcceptWindow() change
    // without any event for the affected window at all (g_ShowTaskbar,
    // g_StartVisible). A periodic full enumeration repairs any such desync.
    now = GetTickCount();
    {
        // If the hook thread died there are no events at all, so the resync IS the
        // tracking - run it far more often so we degrade to the old behaviour rather
        // than below it (see g_WindowEventThreadDead).
        DWORD interval = InterlockedCompareExchange(&g_WindowEventThreadDead, 0, 0)
            ? WINDOW_RESYNC_FALLBACK_MS : WINDOW_RESYNC_INTERVAL_MS;

        if (!resync && (now - g_LastResyncTime) >= interval)
            resync = TRUE;
    }

    if (resync)
        g_LastResyncTime = now;

    return resync;
}

// Forget everything that was queued; the caller knows the list is up to date
// (or that it doesn't care, in fullscreen mode).
static void DiscardWindowEvents(void)
{
    EnterCriticalSection(&g_csWindowEvents);
    g_PendingWindowsCount = 0;
    g_ResyncRequested = TRUE; // whatever we just dropped is covered by the next resync
    LeaveCriticalSection(&g_csWindowEvents);
}

// WinEvent hook callback, runs on the hook thread.
//
// This must stay cheap: the same thread has to keep retrieving messages for
// out-of-context hooks to be delivered at all, so anything slow here (a
// cross-process GetWindowText, a DWM query, a vchan write) would stall the event
// stream. It only ever records handles; all interrogation happens in the main loop.
static void CALLBACK WindowEventProc(
    IN HWINEVENTHOOK hook,
    IN DWORD event,
    IN HWND window,
    IN LONG objectId,
    IN LONG childId,
    IN DWORD eventThread,
    IN DWORD eventTime)
{
    UNREFERENCED_PARAMETER(hook);
    UNREFERENCED_PARAMETER(eventThread);
    UNREFERENCED_PARAMETER(eventTime);

    if (!g_SeamlessMode) // the watched window list isn't used in fullscreen mode
        return;

    if (event == EVENT_SYSTEM_DESKTOPSWITCH)
    {
        QueueWindowEvent(NULL, 0, TRUE);
        return;
    }

    // Only whole top-level windows are relevant. This filter is what makes the
    // callback affordable: it drops the flood of EVENT_OBJECT_LOCATIONCHANGE for
    // carets, the cursor and child controls.
    if (objectId != OBJID_WINDOW || childId != CHILDID_SELF || !window)
        return;

    // EnumWindows() only offers windows parented to the desktop, track exactly the
    // same set (this also drops message-only windows). A destroyed window can't be
    // queried anymore, so it has to be let through unfiltered.
    if (event != EVENT_OBJECT_DESTROY && GetAncestor(window, GA_PARENT) != GetDesktopWindow())
        return;

    QueueWindowEvent(window, event, FALSE);
}

// Owns the WinEvent hooks and pumps messages for them.
static DWORD WINAPI WindowEventThreadProc(IN void* param)
{
    HWINEVENTHOOK hooks[RTL_NUMBER_OF(g_HookedEventRanges)];
    HANDLE waitFor[WINDOW_EVENT_WAIT_COUNT];
    BOOL exitThread = FALSE;
    HDESK ownDesktop = NULL; // the desktop THIS thread opened, closed only by this thread

    UNREFERENCED_PARAMETER(param);
    LogDebug("start");

    waitFor[0] = g_WindowEventStop;
    waitFor[1] = g_WindowEventRearm;

    while (!exitThread)
    {
        HDESK previousDesktop = ownDesktop;

        ZeroMemory(hooks, sizeof(hooks));

        // WinEvent hooks only receive events from the desktop that the thread setting
        // them is attached to, so this thread must follow the input desktop.
        //
        // Deliberately NOT AttachToInputDesktop(): that helper also writes the shared
        // globals g_DesktopWindow/g_StartWindow/g_SearchWindow, unsynchronised, and it is
        // already called from two other threads. The reject cache now depends on
        // g_StartWindow/g_SearchWindow (ExamineWindow refuses to cache them because
        // interrogating them updates g_StartVisible/g_ShowTaskbar), so a torn read there
        // would cache the real Start window as ineligible - permanently, since the entry
        // survives every resync. It also CloseDesktop()s the process-default desktop
        // handle, which MSDN forbids and which a third caller would double-close.
        // Only the per-thread part is needed here.
        //
        // DESKTOP_CREATEWINDOW: this thread also owns the work-area broadcast listener
        // (WorkAreaCreateListener below), and CreateWindowEx on a thread whose desktop
        // handle lacks that right fails with ERROR_ACCESS_DENIED - the hooks worked
        // without it, the listener never could. If the wide open is denied (a desktop
        // that refuses window creation), fall back to the narrow mask so the hooks -
        // the critical function of this thread - still come up.
        ownDesktop = OpenInputDesktop(0, FALSE,
            DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL | DESKTOP_CREATEWINDOW);
        if (!ownDesktop)
            ownDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL);
        if (ownDesktop)
        {
            if (!SetThreadDesktop(ownDesktop))
                win_perror("SetThreadDesktop");
        }
        else
        {
            win_perror("OpenInputDesktop");
        }

        if (previousDesktop)
            CloseDesktop(previousDesktop);

        for (UINT i = 0; i < RTL_NUMBER_OF(g_HookedEventRanges); i++)
        {
            hooks[i] = SetWinEventHook(g_HookedEventRanges[i].Min, g_HookedEventRanges[i].Max,
                NULL, WindowEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

            if (!hooks[i])
                LogWarning("SetWinEventHook(0x%x-0x%x) failed", g_HookedEventRanges[i].Min, g_HookedEventRanges[i].Max);
        }

        // Whatever happened while the hooks were down is unknown to us.
        QueueWindowEvent(NULL, 0, TRUE);

        // This thread pumps messages, so it owns the work-area broadcast listener
        // (WM_SETTINGCHANGE/WM_DISPLAYCHANGE re-assert; see workarea.h).
        WorkAreaCreateListener();

        while (TRUE)
        {
            // Out-of-context hook callbacks are delivered by the system while this
            // thread retrieves messages, so this wait must be message-aware.
            DWORD signaled = MsgWaitForMultipleObjects(WINDOW_EVENT_WAIT_COUNT, waitFor, FALSE, INFINITE, QS_ALLINPUT);

            if (signaled == WAIT_OBJECT_0) // stop
            {
                exitThread = TRUE;
                break;
            }

            if (signaled == WAIT_OBJECT_0 + 1) // rearm on a new desktop
                break;

            if (signaled == WAIT_OBJECT_0 + WINDOW_EVENT_WAIT_COUNT) // message(s) queued
            {
                MSG msg;

                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT)
                    {
                        exitThread = TRUE;
                        break;
                    }

                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (exitThread)
                    break;

                continue;
            }

            win_perror("MsgWaitForMultipleObjects");
            exitThread = TRUE;
            break;
        }

        for (UINT i = 0; i < RTL_NUMBER_OF(hooks); i++)
        {
            if (hooks[i])
                UnhookWinEvent(hooks[i]);
        }

        // The listener must go before the next SetThreadDesktop: a thread that owns a
        // window on its current desktop cannot switch desktops, and the window would be
        // stale on the old desktop anyway (broadcasts are per-desktop). Recreated after
        // re-attach at the top of the loop; this bottom-of-loop site also runs on every
        // thread-exit path (exitThread breaks out of the inner loop only).
        WorkAreaDestroyListener();
    }

    if (ownDesktop)
        CloseDesktop(ownDesktop);

    // D7: if this thread ever exits, event-driven tracking stops and window moves fall back
    // to the 2 s resync - i.e. 0.5 Hz, WORSE than the 6.6 Hz the old per-frame enumeration
    // achieved. Make that loud rather than a silent regression, and let the frame path fall
    // back to a short resync so behaviour degrades to roughly the old code instead.
    LogError("window event thread exiting - tracking falls back to periodic resync");
    InterlockedExchange(&g_WindowEventThreadDead, 1);

    LogDebug("end");
    return ERROR_SUCCESS;
}

// Tell the hook thread to reattach to the current input desktop and rearm.
static void RearmWindowEvents(void)
{
    if (g_WindowEventRearm)
        SetEvent(g_WindowEventRearm);
}

static ULONG StartWindowEventThread(void)
{
    InitializeCriticalSection(&g_csWindowEvents);

    g_WindowEventSignal = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_WindowEventRearm = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_WindowEventStop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_WindowEventSignal || !g_WindowEventRearm || !g_WindowEventStop)
        return win_perror("CreateEvent");

    g_WindowEventThread = CreateThread(NULL, 0, WindowEventThreadProc, NULL, 0, NULL);
    if (!g_WindowEventThread)
        return win_perror("CreateThread(window events)");

    return ERROR_SUCCESS;
}

static void StopWindowEventThread(void)
{
    if (!g_WindowEventThread)
        return;

    SetEvent(g_WindowEventStop);
    if (WAIT_OBJECT_0 != WaitForSingleObject(g_WindowEventThread, 2000))
        LogWarning("window event thread didn't exit in time");

    CloseHandle(g_WindowEventThread);
    g_WindowEventThread = NULL;
}

#ifdef DEBUG_DUMP_WINDOWS
// FIXME: this fails for UWP apps
void DumpWindowBitmap(const WINDOW_DATA* data)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    WCHAR path[256];
    StringCchPrintf(path, 256, L"c:\\Qubes Tools\\log\\%x_%04d%02d%02d_%02d%02d%02d_%03d.bmp", data->Handle,
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    DWORD status = 0;
    HDC hdc = NULL;
    HBITMAP bm = NULL;
    HDC win_dc = GetWindowDC(data->Handle);
    if (!win_dc)
    {
        status = win_perror("CreateCompatibleDC(window)");
        goto end;
    }

    hdc = CreateCompatibleDC(win_dc);
    if (!hdc)
    {
        status = win_perror("CreateCompatibleDC(window)");
        goto end;
    }

    RECT rect;
    GetWindowRect(data->Handle, &rect);
    int cx = rect.right - rect.left;
    int cy = rect.bottom - rect.top;
    bm = CreateCompatibleBitmap(win_dc, cx, cy);
    if (!bm)
    {
        status = win_perror("CreateCompatibleBitmap(window)");
        goto end;
    }

    SelectObject(hdc, bm);

    if (!BitBlt(hdc, 0, 0, cx, cy, win_dc, 0, 0, SRCCOPY))
    {
        status = win_perror("BitBlt(window)");
        goto end;
    }

    BITMAP b;
    GetObject(bm, sizeof(BITMAP), &b);

    BITMAPFILEHEADER bmfHeader;
    BITMAPINFOHEADER bi;

    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = b.bmWidth;
    bi.biHeight = b.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    DWORD bsize = ((b.bmWidth * bi.biBitCount + 31) / 32) * 4 * b.bmHeight;
    HANDLE dib = GlobalAlloc(GHND, bsize);
    char* ptr = (char*)GlobalLock(dib);
    GetDIBits(win_dc, bm, 0, (UINT)b.bmHeight, ptr, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    HANDLE file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD dibsize = bsize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);
    bmfHeader.bfSize = dibsize;
    bmfHeader.bfType = 0x4D42; // BM
    DWORD wr;
    WriteFile(file, (LPSTR)&bmfHeader, sizeof(BITMAPFILEHEADER), &wr, NULL);
    WriteFile(file, (LPSTR)&bi, sizeof(BITMAPINFOHEADER), &wr, NULL);
    WriteFile(file, (LPSTR)ptr, bsize, &wr, NULL);
    GlobalUnlock(dib);
    GlobalFree(dib);
    CloseHandle(file);

end:
    if (win_dc)
        ReleaseDC(data->Handle, win_dc);
    if (hdc)
        ReleaseDC(data->Handle, hdc);
    if (bm)
        DeleteObject(bm);
    LogVerbose("end (%x)", status);
}

// diagnostic: dump all watched windows
void DumpWindows(void)
{
    WINDOW_DATA *entry;
    static WCHAR* exePath = NULL;

    EnterCriticalSection(&g_csWatchedWindows);
    if (!exePath)
    {
        exePath = malloc(MAX_PATH_LONG_WSIZE);
        if (!exePath)
            goto end;
    }

    if (IsListEmpty(&g_WatchedWindowsList))
        goto end;

    LogDebug("### Window dump:");

    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        exePath[0] = 0;
        DWORD pid;
        GetWindowThreadProcessId(entry->Handle, &pid);
        if (pid)
        {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process != INVALID_HANDLE_VALUE)
            {
                DWORD size = MAX_PATH_LONG;
                QueryFullProcessImageName(process, 0, exePath, &size);
                CloseHandle(process);
            }
        }

        LogLock();
        LogDebugRaw("0x%x: (%6d,%6d) %4ux%4u ",
            entry->Handle, entry->X, entry->Y, entry->Width, entry->Height);
        LogDebugRaw("%c %c ovr=%d [%s] '%s' {%s} parent=0x%x ",
            entry->IsVisible?'V':'-', entry->IsIconic?'_':' ', entry->IsOverrideRedirect,
            entry->Class, entry->Caption, exePath, GetAncestor(entry->Handle, GA_PARENT));
        if (entry->ModalParent)
            LogDebugRaw(" ModalParent=0x%x ", entry->ModalParent);
        LogStyle(entry->Style);
        LogExStyle(entry->ExStyle);
        LogDebugRaw("\n");
        LogUnlock();

        //DumpWindowBitmap(entry);
        entry = (WINDOW_DATA *)entry->ListEntry.Flink;
    }
end:
    LeaveCriticalSection(&g_csWatchedWindows);
}
#endif // DEBUG_DUMP_WINDOWS

// When DWM compositing is enabled (normally always on), most windows are actually smaller
// than their size reported by winuser functions. This is because their edges contain
// invisible grip handles managed by DWM. This function returns actual visible window size.
// This function also accounts for a clipping region possibly defined for the window
// and returns a minimal bounding rectangle that covers the visible region.
ULONG GetRealWindowRect(IN HWND window, OUT RECT* rect)
{
    RECT dwmRect;
    // get real rect of the window as managed by DWM
    HRESULT hresult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &dwmRect, sizeof(RECT));
    if (hresult != S_OK)
        return hresult;

    // monitor info is needed to adjust for DPI scaling
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX monInfo;
    monInfo.cbSize = sizeof(monInfo);
#pragma warning(push)
#pragma warning(disable:4133) // incompatible types - from 'MONITORINFOEX *' to 'LPMONITORINFO' (the function accepts both)
    if (!GetMonitorInfo(monitor, &monInfo))
        return win_perror("GetMonitorInfo failed");
#pragma warning(pop)

    DEVMODE devMode;
    devMode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(monInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode);

    // adjust for DPI scaling
    double scale = (monInfo.rcMonitor.right - monInfo.rcMonitor.left) / (double)devMode.dmPelsWidth;

    rect->left = (LONG)((dwmRect.left - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->right = (LONG)((dwmRect.right - devMode.dmPosition.x) * scale) + monInfo.rcMonitor.left;
    rect->top = (LONG)((dwmRect.top - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    rect->bottom = (LONG)((dwmRect.bottom - devMode.dmPosition.y) * scale) + monInfo.rcMonitor.top;
    LogDebug("0x%x: rect (%d,%d) %dx%d", window,
        rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);

    // get clipping region
    // TODO: probably need to account for DPI here too
    static HRGN region = NULL;
    if (!region)
        region = CreateRectRgn(0, 0, 0, 0);

    int region_type = GetWindowRgn(window, region);
    if (region_type == SIMPLEREGION || region_type == COMPLEXREGION)
    {
        RECT bounds;
        GetRgnBox(region, &bounds); // bounding box for the region TODO: better clipping for complex ones?
        LogDebug("0x%x: clipping region type %d: bounds (%d,%d) %dx%d", window, region_type,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
        // region coords are relative to the window
        // TODO: should we use DWM's window rect or USER one if they differ?
        bounds.left += rect->left;
        bounds.top += rect->top;
        bounds.right += rect->left;
        bounds.bottom += rect->top;

        RECT clipped;
        IntersectRect(&clipped, rect, &bounds);
        *rect = clipped;
        // clipped is always bounded by rect

        LogDebug("0x%x: clipping region type %d: final rect (%d,%d) %dx%d", window, region_type,
            rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);

        if (window == g_StartWindow)
        {
            // FIXME: make this more reliable
            // start menu still has transparent DWM borders included even after clipping
            // TODO: this will probably break if taskbar is on a different side of the screen
            rect->right -= (bounds.right - bounds.left) - (clipped.right - clipped.left);
            rect->top += (bounds.bottom - bounds.top) - (clipped.bottom - clipped.top);
            LogDebug("0x%x: fixed start rect (%d,%d) %dx%d", window,
                rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top);
        }
    }

    return ERROR_SUCCESS;
}

BOOL HasFlags(DWORD value, DWORD flags)
{
    return (value & flags) == flags;
}

BOOL IsPopup(IN const WINDOW_DATA* entry)
{
    if (!entry->IsVisible)
        return FALSE;

    BOOL isPopup = !(
        // "Normal" popups without caption
        HasFlags(entry->Style, WS_CAPTION) ||
        // Metro apps without WS_CAPTION.
        // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
        // but I guess MS doesn't adhere to its own standards...
        (HasFlags(entry->Style, WS_SYSMENU) && HasFlags(entry->ExStyle, WS_EX_APPWINDOW)));

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    if (isPopup && (entry->Width >= g_HostScreenWidth || entry->Height >= g_HostScreenHeight))
    {
        LogDebug("0x%x: popup too large: %ux%u, host screen %ux%u",
            entry->Handle, entry->Width, entry->Height, g_HostScreenWidth, g_HostScreenHeight);
        isPopup = FALSE;
    }

    if (isPopup && entry->IsVisible)
    {
        LogVerbose("0x%x: popup %ux%u", entry->Handle, entry->Width, entry->Height);
    }

    return isPopup;
}

ULONG ToggleMap(IN const WINDOW_DATA* entry)
{
    if (entry->Synthesized)
        return ERROR_SUCCESS; // never announced; see main.h

    ULONG status = SendWindowUnmap(entry->Handle);
    if (status != ERROR_SUCCESS)
        return status;

    status = SendWindowMap(entry);
    if (status != ERROR_SUCCESS)
        return status;

    // The daemon released its mapping of the per-window buffer on MSG_UNMAP; the
    // grants are still valid, so re-announce them and repaint.
    if (PwIsAttached(entry))
    {
        status = PwRemapWindow(entry);
        if (status == ERROR_SUCCESS)
            status = SendWindowDamageEvent(entry->Handle, 0, 0,
                entry->Width, entry->Height);
    }
    return status;
}

// fills WINDOW_DATA if successful
// if *windowData is NULL, the function allocates a new struct and sets the pointer
// if *windowData is not NULL, the function updates the supplied struct
ULONG GetWindowData(IN HWND window, IN OUT WINDOW_DATA** windowData)
{
    WINDOW_DATA* entry = NULL;
    ULONG status;

    if (windowData == NULL)
        return ERROR_INVALID_PARAMETER;

    RECT rect;
    status = GetRealWindowRect(window, &rect);
    if (!SUCCEEDED(status))
    {
        return win_perror2(status, "GetRealWindowRect");
    }

    if (*windowData != NULL)
    {
        entry = *windowData;
    }
    else
    {
        entry = (WINDOW_DATA*)malloc(sizeof(*entry));
        if (!entry)
        {
            LogError("Failed to malloc entry");
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        *windowData = entry;
    }

    ZeroMemory(entry, sizeof(*entry));

    entry->X = rect.left;
    entry->Y = rect.top;
    // Seed the frame registration so a window seen for the first time converts its damage
    // against a sane origin; ProcessNewFrame re-snapshots it every frame thereafter.
    entry->Width = rect.right - rect.left;
    entry->Height = rect.bottom - rect.top;
    entry->Handle = window;
    entry->Style = GetWindowLong(window, GWL_STYLE);
    entry->ExStyle = GetWindowLong(window, GWL_EXSTYLE);
    // Must be set unconditionally: ZeroMemory() above leaves it at 0, which would read as
    // "fully transparent". No-ops for non-layered windows.
    entry->IsIconic = IsIconic(window);
    entry->DeletePending = FALSE;
    GetWindowText(window, entry->Caption, ARRAYSIZE(entry->Caption)); // don't really care about errors here
    GetClassName(window, entry->Class, ARRAYSIZE(entry->Class));

    entry->IsVisible = IsWindowVisible(window);

    DWORD cloaked;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        // DWM-cloaked windows are the third class of unmappable chrome (CLAUDE.md 2A-chrome):
        // present, styled visible, drawn nowhere - e.g. windows on another virtual desktop or
        // suspended UWP frames. Folding it into IsVisible here is what makes
        // ShouldAcceptWindow() reject them; the hook thread watches
        // EVENT_OBJECT_CLOAKED/UNCLOAKED and forces re-examination, since cloaking changes
        // none of the fields the reject cache signature compares.
        if (cloaked != 0) // hidden by DWM
        {
            LogVerbose("0x%x: DWM-cloaked (0x%x)", window, cloaked);
            entry->IsVisible = FALSE;
        }
    }

    if (window == g_StartWindow)
        g_StartVisible = entry->IsVisible;

    if (entry->IsVisible)
    {
        // Special treatment for minimized UAC prompts:
        // These tend to show up when pressing ctrl-shift-enter in the "Run" dialog to run as admin.
        // They don't show as normal windows, just flash their icon in the taskbar as if minimized.
        // But, for SOME reason, *sometimes* these windows don't have the iconic flag set, and their size is 0...
        // TODO: enumerate taskbar buttons to properly detect such cases,
        // this seems to require using UI Automation interfaces.
        if (wcscmp(entry->Class, UAC_DUMMY_WINDOW_CLASS) == 0)
        {
            // For now we hide these windows and mark the taskbar to be shown
            // because programatically activating them doesn't seem to work.
            entry->IsVisible = FALSE;
            g_ShowTaskbar = TRUE;
            LogVerbose("0x%x: UAC prompt", window);
            return ERROR_SUCCESS;
        }

        // check if we're modal to some other window
        HWND owner = GetWindow(window, GW_OWNER);
        entry->Owner = owner; // also an input of ShouldAcceptWindow(), see WINDOW_DATA
        GetWindowThreadProcessId(window, &entry->ProcessId); // see WINDOW_DATA.ProcessId
        if (owner)
        {
            BOOL ownerDisabled = GetWindowLong(owner, GWL_STYLE) & WS_DISABLED;
            if (ownerDisabled)
                entry->ModalParent = owner;
            else
                entry->ModalParent = NULL;
        }
    }

    // A maximized window's DWM frame rect can exceed the screen (invisible resize borders,
    // or the window having been nudged by a dom0-initiated move). Reporting that oversize
    // rect makes the dom0 WM constrain the daemon's X window, the daemon echo the
    // constrained size back, and HandleConfigure fight the guest - an endless CONFIGURE
    // ping-pong with a full per-window grant rebuild on every flip. Clamp what we report
    // (and grant) to the visible screen; the per-window capture crop already skips the
    // off-screen frame margin.
    if ((entry->Style & WS_MAXIMIZE) && entry->IsVisible &&
        g_HostScreenWidth > 0 && g_HostScreenHeight > 0)
    {
        // DELIBERATELY the SCREEN, not the work area. entry->X/Y/W/H is not merely what
        // we report to dom0: perwindow.c derives the capture crop from it
        // (cropY = entry->Y - wr.top), so shrinking this rect CROPS REAL CONTENT.
        // Clamping to the work area was tried on 2026-08-07 and cut 64 px off the top of
        // a maximized Notepad - its title bar and menu bar vanished in dom0 - because
        // Windows had maximized against the full screen while our work area started at
        // y=56. Against the screen the clamp only ever trims the invisible resize border,
        // which is exactly what the per-window crop is supposed to skip.
        // A maximized window overflowing dom0's workspace is a WORK-AREA problem: fix it
        // by making SPI_SETWORKAREA stick so Windows maximizes into the right rect, never
        // by reporting geometry that does not match the pixels.
        int cx = entry->X < 0 ? 0 : entry->X;
        int cy = entry->Y < 0 ? 0 : entry->Y;
        int x2 = entry->X + (int)entry->Width;
        int y2 = entry->Y + (int)entry->Height;
        if (x2 > (int)g_HostScreenWidth)
            x2 = (int)g_HostScreenWidth;
        if (y2 > (int)g_HostScreenHeight)
            y2 = (int)g_HostScreenHeight;
        // All-or-nothing: a window entirely off-screen keeps its raw geometry rather
        // than getting a half-applied clamp.
        if (x2 > cx && y2 > cy)
        {
            entry->X = cx;
            entry->Y = cy;
            entry->Width = (DWORD)(x2 - cx);
            entry->Height = (DWORD)(y2 - cy);
        }
    }

    entry->IsOverrideRedirect = IsPopup(entry);

    return ERROR_SUCCESS;
}

// watched window critical section must be entered
// also sends creation notifications to gui daemon
// --- composite synthesis (see WINDOW_DATA.Synthesized in main.h) ---------------
// Watched-windows CS must be held for all of these.

// px a popup may stick out of the owner's granted buffer and still be synthesized.
// Win11 XAML popups (menus/flyouts) align to the owner's OUTER window rect while the
// buffer starts at the DWM visual frame: measured 5 px overhang at 96 DPI
// (win11-idd-test, Notepad File menu WR left 258 vs DWM frame 263), and the invisible
// resize border grows with DPI. 12 covers that with headroom; Alt-nav keytip badges
// (~20+ px outside) stay excluded on purpose. Both consumers clip/clamp: the frame
// loop's patch intersects with the buffer rect, the capture mask clamps to channel
// width, so a larger overhang only ever crops shadow-margin pixels.
#define SYNTH_OVERHANG_MAX 12

// Interval of the periodic full re-copy of synthesized children in the frame loop.
// The dirty-rect-driven patch can catch a popup MID-DRAW, and once the popup goes
// quiet no further damage forces a second pass; this bounds how long such a
// half-drawn composite can persist in dom0.
#define SYNTH_FULL_PATCH_MS 200

static void PwPatchSynthRect(IN WINDOW_DATA* owner, IN const WINDOW_DATA* child);

// Owner-candidacy checks shared by the GW_OWNER path and the same-process fallback:
// the owner must be an announceable window with its own PrintWindow-fed buffer (a
// slice-fed owner already carries the composited child pixels, and a legacy owner
// has no buffer to patch into), with mask capacity left, whose GRANTED geometry
// contains the popup (almost - SYNTH_OVERHANG_MAX).
static BOOL SynthOwnerQualifies(IN const WINDOW_DATA* owner, IN const WINDOW_DATA* entry)
{
    if (!owner || owner == entry || owner->Synthesized || owner->DeletePending)
        return FALSE;
    if (!PwIsAttached(owner) || owner->PwSliceFed)
        return FALSE;
    // An entry already accounted on this owner does not consume a NEW mask slot.
    if (owner->SynthChildCount >= WC_MAX_MASK && entry->SynthOwner != owner->Handle)
        return FALSE;

    // containment against the geometry the owner's BUFFER was granted for
    int oL = owner->X, oT = owner->Y;
    int oR = oL + (int)owner->PwWidth, oB = oT + (int)owner->PwHeight;
    int cL = entry->X, cT = entry->Y;
    int cR = cL + (int)entry->Width, cB = cT + (int)entry->Height;
    if (cL < oL - SYNTH_OVERHANG_MAX || cT < oT - SYNTH_OVERHANG_MAX ||
        cR > oR + SYNTH_OVERHANG_MAX || cB > oB + SYNTH_OVERHANG_MAX)
        return FALSE;

    // Proximity is not containment. The overhang allowance was meant for popups that
    // stick out slightly WHILE STILL OVERLAPPING, but on its own it also admits children
    // that lie entirely outside the buffer: Word's Document Recovery dialog owns four
    // MSO_BORDEREFFECT strips that sit flush against its edges with EXACTLY zero
    // intersection, 8 px out - inside the allowance. Composited, they can never be
    // painted (the patch intersects to an empty rect, forever), and the resulting
    // synthesize/materialize flip-flop was what eventually fed gui-daemon a message for
    // a window it had no CREATE for. Require real overlap with the granted buffer.
    if (cR <= oL || cL >= oR || cB <= oT || cT >= oB)
        return FALSE;

    return TRUE;
}

// Does this window qualify to be composited into an owner instead of becoming its
// own dom0 window? Deliberately strict: only override-redirect windows (menus,
// tooltips, bubbles - never real dialogs), and only into a qualifying owner:
//   1. its GW_OWNER, when that is a window we track (the Win10 / Office chrome case);
//   2. otherwise - no GW_OWNER at all, or GW_OWNER pointing at an untracked helper
//      window - the TOPMOST window of the same process whose granted buffer contains
//      the popup. Win11 XAML windowed popups (Xaml_WindowedPopupClass "PopupHost":
//      teaching bubbles, WinUI menus/flyouts/context menus) are exactly this shape;
//      geometric strictness is the guard against over-matching, and a popup that
//      leaves the containment materializes via the normal re-check.
// A window that is ALREADY synthesized re-qualifies only against its recorded owner:
// hopping owners mid-life would desync the mask/child accounting.
static BOOL SynthQualifies(IN const WINDOW_DATA* entry, OUT WINDOW_DATA** ownerOut)
{
    if (!entry->IsOverrideRedirect || entry->IsIconic || !entry->IsVisible)
        return FALSE;
    if (entry->Width == 0 || entry->Height == 0)
        return FALSE;

    WINDOW_DATA* owner = NULL;

    if (entry->Synthesized)
    {
        owner = FindWindowByHandle(entry->SynthOwner);
        if (!SynthOwnerQualifies(owner, entry))
            return FALSE;
    }
    else if (entry->Owner)
    {
        // GW_OWNER set: honor it exclusively. A menu owned by window A must never be
        // synthesized into an overlapping window B of the same app.
        //
        // "Exclusively" has to include the case where the owner is NOT tracked. Testing
        // `FindWindowByHandle(entry->Owner) != NULL` in the condition let an owned popup
        // whose owner the agent does not track fall through to the same-process fallback
        // below, which then adopted it into whatever topmost sibling happened to contain
        // it. Measured: Office's MSO_BORDEREFFECT shadow strips around a sign-in DIALOG
        // were adopted by the maximized OpusApp main window (they do overlap it, so the
        // geometric guard passes), their pixels patched in from the composited desktop,
        // and the owner's capture masked those bands out - leaving a frozen L-shaped
        // shadow ghost burned into the document area that nothing could ever repaint,
        // outliving the dialog itself. An untracked owner means "not ours to composite
        // into", so the popup must be refused, never re-homed.
        owner = FindWindowByHandle(entry->Owner);
        if (!owner || !SynthOwnerQualifies(owner, entry))
            return FALSE;
    }
    else if (entry->ProcessId != 0)
    {
        // Prefer the foreground window: keytips and flyouts belong to the focused
        // window, and with two overlapping windows of the same process the topmost-
        // containing pick below can attach the popup to the wrong sibling.
        HWND fgHandle = GetForegroundWindow();
        if (fgHandle)
        {
            WINDOW_DATA* fg = FindWindowByHandle(fgHandle);
            if (fg && fg->ProcessId == entry->ProcessId && !fg->IsOverrideRedirect &&
                SynthOwnerQualifies(fg, entry))
                owner = fg;
        }

        if (!owner)
        {
            int bestZ = INT_MAX;
            for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
                 e != &g_WatchedWindowsList; e = e->Flink)
            {
                WINDOW_DATA* cand = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
                if (cand->ProcessId != entry->ProcessId || cand->IsOverrideRedirect)
                    continue;
                if (!SynthOwnerQualifies(cand, entry))
                    continue;
                if (cand->ZOrder < bestZ)
                {
                    bestZ = cand->ZOrder;
                    owner = cand;
                }
            }
        }
        if (!owner)
            return FALSE;
    }
    else
        return FALSE;

    if (ownerOut)
        *ownerOut = owner;
    return TRUE;
}

// Push the current synthesized-child rects of `owner` to its capture channel, so the
// owner's PrintWindow pass leaves those pixels alone (the frame loop owns them).
// No-ops when the computed mask is byte-identical to the last one pushed: WcSetMask
// takes the engine lock exclusively AND forces a full recapture (wincapture.cpp), so
// a redundant push is a pure loss. The memo is written before WcSetMask, which
// silently no-ops when no channel exists; that is safe because PwAttachWindow is the
// only channel-creation path and it clears the memo.
C_ASSERT(RTL_NUMBER_OF(((WINDOW_DATA*)0)->SynthMaskLast) == WC_MAX_MASK);
static void SynthUpdateMask(IN OUT WINDOW_DATA* owner)
{
    RECT mask[WC_MAX_MASK];
    int n = 0;
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
         e != &g_WatchedWindowsList && n < WC_MAX_MASK; e = e->Flink)
    {
        const WINDOW_DATA* c = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (!c->Synthesized || c->SynthOwner != owner->Handle)
            continue;
        RECT r = { c->X - owner->X, c->Y - owner->Y,
                   c->X - owner->X + (int)c->Width, c->Y - owner->Y + (int)c->Height };
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (int)owner->PwWidth) r.right = (int)owner->PwWidth;
        if (r.bottom > (int)owner->PwHeight) r.bottom = (int)owner->PwHeight;
        if (r.right > r.left && r.bottom > r.top)
            mask[n++] = r;
    }

    if (n == owner->SynthMaskLastCount &&
        memcmp(mask, owner->SynthMaskLast, (size_t)n * sizeof(RECT)) == 0)
        return; // the channel already has exactly this mask

    owner->SynthMaskLastCount = n;
    memcpy(owner->SynthMaskLast, mask, (size_t)n * sizeof(RECT));
    LogDebug("QGADRAG,ev=maskpush,hwnd=0x%x,n=%d",
        (uint32_t)(ULONG_PTR)owner->Handle, n);
    WcSetMask(owner->Handle, mask, n);
}

// Flush the mask updates the geometry paths deferred (SynthMaskPending, main.h) -
// called by TrackWindows exactly once per tracking pass, AFTER all interrogations of
// the pass completed: every window's X/Y then comes from the same consistent
// snapshot, so a pure joint owner+child move computes a mask identical to the memo
// and pushes nothing. Computing at the call sites instead pushed a mixed-state mask
// plus its restore - two forced recaptures per pass, at input rate during a
// menu-over-drag. Watched windows critical section must be entered.
static void SynthFlushMasks(void)
{
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
         e != &g_WatchedWindowsList; e = e->Flink)
    {
        WINDOW_DATA* w = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (!w->SynthMaskPending)
            continue;
        w->SynthMaskPending = FALSE;
        SynthUpdateMask(w);
    }
}

// Mark a window synthesized (no protocol traffic from here on) and account it on
// the owner. The caller must not have announced it yet.
static void SynthActivate(IN OUT WINDOW_DATA* entry, IN OUT WINDOW_DATA* owner)
{
    entry->Synthesized = TRUE;
    entry->SynthOwner = owner->Handle;
    owner->SynthChildCount++;
    SynthUpdateMask(owner);
    // Paint it NOW: a menu paints once, before it is tracked, and a static screen
    // produces no further frames - waiting for damage would leave it invisible.
    PwPatchSynthRect(owner, entry);
    owner->SynthLastFullPatch = GetTickCount();
    LogInfo("QGAPROTO,msg=SYNTH,hwnd=0x%x,owner=0x%x,x=%d,y=%d,w=%u,h=%u",
        (uint32_t)(ULONG_PTR)entry->Handle, (uint32_t)(ULONG_PTR)owner->Handle,
        entry->X, entry->Y, entry->Width, entry->Height);
}

// Stop synthesizing (window gone, or it no longer qualifies). Does NOT announce the
// window; the caller decides whether to materialize it.
static void SynthDeactivate(IN OUT WINDOW_DATA* entry)
{
    if (!entry->Synthesized)
        return;
    entry->Synthesized = FALSE;
    WINDOW_DATA* owner = FindWindowByHandle(entry->SynthOwner);
    entry->SynthOwner = NULL;
    if (owner && owner->SynthChildCount > 0)
    {
        owner->SynthChildCount--;
        SynthUpdateMask(owner);
        // The owner's capture skipped those pixels while the child lived; force a
        // fresh capture so the owner's own content reappears there.
        if (PwIsAttached(owner) && !owner->PwSliceFed)
        {
            WcMarkDirty(owner->Handle);
            (void)SendWindowDamageEvent(owner->Handle, 0, 0, owner->PwWidth, owner->PwHeight);
        }
    }
}

ULONG AddWindow(IN WINDOW_DATA* entry)
{
    ULONG status = ERROR_SUCCESS;
    LogVerbose("start, handle 0x%x, visible %d, iconic %d", entry->Handle, entry->IsVisible, entry->IsIconic);
    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // Composite synthesis: an owner-contained popup is never announced to dom0 - it
    // is painted into its owner's buffer by the frame loop instead (main.h).
    {
        WINDOW_DATA* synthOwner = NULL;
        if (PwEnabled() && SynthQualifies(entry, &synthOwner))
        {
            SynthActivate(entry, synthOwner);
            return ERROR_SUCCESS;
        }
    }

    // Sub-floor popups (Win11 Alt-nav keytip badges, ~40x46) are acceptable ONLY as
    // synthesized content: announced, each becomes an individually bordered dom0
    // window slice-fed with whatever pixels sit behind it. When synthesis cannot
    // represent one (outside every owner's granted buffer), drop it silently - not
    // appearing beats appearing wrong. Materialization funnels back through this
    // path, so a synthesized badge that drifts out of its owner is dropped here too.
    if (entry->IsOverrideRedirect &&
        (entry->Width < g_MinWindowWidth || entry->Height < g_MinWindowHeight))
    {
        LogDebug("0x%x: sub-floor popup %ux%u with no synth owner, dropping silently",
            entry->Handle, entry->Width, entry->Height);
        entry->DeletePending = TRUE;
        return ERROR_SUCCESS;
    }

    // send window creation info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowCreate");
            goto end;
        }
        entry->CreateSent = TRUE;

        // Per-window framebuffer: announce the window's own buffer BEFORE mapping so
        // the daemon never composites this window from the screen slice. On failure
        // the window simply stays on the legacy path.
        if (PwEnabled() && entry->IsVisible && !entry->IsIconic &&
            entry->Width > 0 && entry->Height > 0)
        {
            (void)PwAttachWindow(entry);
        }

        // map (show) the window if it's visible OR minimized
        if (entry->IsIconic || entry->IsVisible)
        {
            status = SendWindowMap(entry);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SendWindowMap");
                goto end;
            }

            // Force a full repaint of the window we just mapped.
            //
            // Damage is attributed to windows the agent already knows about. A window can
            // easily be painted by Windows BEFORE we learn it exists - the old code hid this
            // because AddAllWindows() enumerated live on the frame path, so anything that
            // existed at that instant was known before damage was attributed; with
            // event-driven tracking the CREATE/SHOW event can still be in flight when the
            // frame carrying that window's paint is processed, and the damage is dropped.
            // A menu is the worst case: it paints once and then generates no further damage,
            // so it stays BLANK in dom0 until something else happens to dirty it (hovering
            // an entry redraws it - which is exactly the reported symptom, and why keyboard
            // navigation does not show it).
            // This also narrows the same race in the shipped agent, where it is rarer but
            // demonstrably present.
            if (entry->IsVisible && entry->Width > 0 && entry->Height > 0)
            {
                ULONG damageStatus = SendWindowDamageEvent(entry->Handle, 0, 0,
                    entry->Width, entry->Height);
                if (ERROR_SUCCESS != damageStatus)
                    win_perror2(damageStatus, "SendWindowDamageEvent(initial)");
            }

            if (entry->IsIconic)
            {
                status = SendWindowFlags(entry->Handle, WINDOW_FLAG_MINIMIZE, 0);
                if (ERROR_SUCCESS != status)
                    goto end;
            }
        }

        status = SendWindowName(entry->Handle, entry->Caption);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowName");
            goto end;
        }
    }

end:
    LogVerbose("end (%x)", status);
    return status;
}

// Send MSG_CONFIGURE for a tracked window with its current geometry, unless it would be
// byte-identical to the last one sent (drag processing produced bursts of 4+ duplicates).
static ULONG SendWindowConfigureIfChanged(IN OUT WINDOW_DATA* entry)
{
    if (entry->Synthesized)
        return ERROR_SUCCESS; // never announced; see main.h
    if (entry->CfgSentValid &&
        entry->LastCfgX == entry->X && entry->LastCfgY == entry->Y &&
        entry->LastCfgW == (int)entry->Width && entry->LastCfgH == (int)entry->Height &&
        entry->LastCfgOvr == entry->IsOverrideRedirect)
        return ERROR_SUCCESS;

    ULONG status = SendWindowConfigure(entry->Handle, entry->X, entry->Y,
        entry->Width, entry->Height, entry->IsOverrideRedirect);
    if (status == ERROR_SUCCESS)
    {
        entry->CfgSentValid = TRUE;
        entry->LastCfgX = entry->X;
        entry->LastCfgY = entry->Y;
        entry->LastCfgW = (int)entry->Width;
        entry->LastCfgH = (int)entry->Height;
        entry->LastCfgOvr = entry->IsOverrideRedirect;
    }
    return status;
}

// Remove window from the list and free memory.
// Watched windows list critical section must be entered.
ULONG RemoveWindow(IN OUT WINDOW_DATA *entry)
{
    // Stop per-window capture and queue its grant for revocation before any protocol
    // messages: the daemon releases its mapping on UNMAP/DESTROY below, after which
    // the queued revoke succeeds on a tick.
    PwDetachWindow(entry);

    ULONG status = ERROR_INVALID_PARAMETER;

    LogVerbose("start");

    if (!entry)
        goto end;

    LogDebug("0x%x", entry->Handle);

    RemoveEntryList(&entry->ListEntry);

    if (entry->Handle == g_StartWindow)
        g_StartVisible = FALSE;

    // Never announced (synthesized, or announce failed): the daemon has no CREATE for
    // this hwnd, and UNMAP/DESTROY naming it is the documented daemon-killer (send.c).
    // This gate must be on CreateSent, not on Synthesized: materialization clears
    // Synthesized and then relies on removal to re-add the window through the normal
    // path, so the removal itself must still be silent.
    if (!entry->CreateSent)
    {
        SynthDeactivate(entry);
        free(entry);
        status = ERROR_SUCCESS;
        goto end;
    }

    // Children composited into this window lose their host; drop their synthesis so
    // the next tracking pass re-examines (and announces) them normally.
    if (entry->SynthChildCount > 0)
    {
        for (LIST_ENTRY* e = g_WatchedWindowsList.Flink; e != &g_WatchedWindowsList; )
        {
            WINDOW_DATA* c = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
            e = e->Flink;
            if (c->Synthesized && c->SynthOwner == entry->Handle)
            {
                c->Synthesized = FALSE;
                c->SynthOwner = NULL;
                c->DeletePending = TRUE; // re-examined from scratch by TrackWindows
            }
        }
        entry->SynthChildCount = 0;
    }

    if (g_VchanClientConnected)
    {
        // The entry is already off the list: even if a send fails (vchan going down),
        // still attempt DESTROY and free the entry. Bailing out here used to leak the
        // entry AND leave the daemon tracking a window we would never message again.
        status = SendWindowUnmap(entry->Handle);
        if (ERROR_SUCCESS != status)
            win_perror2(status, "SendWindowUnmap");

        if (entry->Handle) // never destroy screen "window"
        {
            status = SendWindowDestroy(entry->Handle);
            if (ERROR_SUCCESS != status)
                win_perror2(status, "SendWindowDestroy");
        }
    }

    free(entry);
    status = ERROR_SUCCESS;
end:
    LogVerbose("end (%x)", status);
    return status;
}

/*
 * Cache of windows that were interrogated and found ineligible.
 *
 * Without it every resync would re-interrogate the ~70 windows that a Windows
 * desktop keeps around and never shows. Entries are only trusted while nothing
 * cheaply observable about the window changed, so a window that becomes eligible
 * is re-admitted even if we never saw an event for it.
 *
 * Accessed by the main loop only, with the watched windows critical section held.
 */
typedef struct _REJECTED_WINDOW
{
    HWND  Handle;
    DWORD ProcessId; // creator ids: guard against handle reuse
    DWORD ThreadId;
    DWORD Style;     // cheap change detectors, no cross-process calls involved
    DWORD ExStyle;
    RECT  Rect;
} REJECTED_WINDOW;

static REJECTED_WINDOW g_RejectedWindows[REJECTED_WINDOWS_MAX];
static UINT g_RejectedWindowsCount = 0;

static int FindRejectedWindow(IN HWND window)
{
    for (UINT i = 0; i < g_RejectedWindowsCount; i++)
    {
        if (g_RejectedWindows[i].Handle == window)
            return (int)i;
    }

    return -1;
}

static void EvictRejectedWindow(IN HWND window)
{
    int index = FindRejectedWindow(window);

    if (index < 0)
        return;

    g_RejectedWindowsCount--;
    g_RejectedWindows[index] = g_RejectedWindows[g_RejectedWindowsCount];
    ZeroMemory(&g_RejectedWindows[g_RejectedWindowsCount], sizeof(REJECTED_WINDOW));
}

static void ClearRejectedWindows(void)
{
    ZeroMemory(g_RejectedWindows, sizeof(g_RejectedWindows));
    g_RejectedWindowsCount = 0;
}

// Remember a window as ineligible. The caller must have evicted any previous entry.
// The signature MUST be sampled by the caller BEFORE the window is interrogated, and
// passed in here. Sampling it after the accept/reject decision would bake in any state
// change that happened *during* the interrogation: if the owner calls ShowWindow() in that
// gap we would cache the already-visible signature, and if the resulting EVENT_OBJECT_SHOW
// is then coalesced or dropped, every later resync would compare "unchanged" and the window
// would never be shown in dom0 again. That defeats the 2 s resync in exactly the case it
// exists for.
static void CacheRejectedWindow(IN HWND window, IN const REJECTED_WINDOW* signature)
{
    UINT index;
    REJECTED_WINDOW* rejected;

    if (g_RejectedWindowsCount < REJECTED_WINDOWS_MAX)
    {
        index = g_RejectedWindowsCount++;
    }
    else
    {
        // Full: reuse a slot held by a window that no longer exists. If there is
        // none, just don't cache this one, the only cost is interrogating it again
        // on the next resync.
        for (index = 0; index < REJECTED_WINDOWS_MAX; index++)
        {
            if (!IsWindow(g_RejectedWindows[index].Handle))
                break;
        }

        if (index == REJECTED_WINDOWS_MAX)
        {
            LogWarning("rejected window cache full");
            return;
        }
    }

    rejected = &g_RejectedWindows[index];
    *rejected = *signature;
    rejected->Handle = window;
}

// Sample the cheap signature used by the reject cache. Must be called BEFORE interrogating
// the window (see CacheRejectedWindow).
static void SampleWindowSignature(IN HWND window, OUT REJECTED_WINDOW* signature)
{
    ZeroMemory(signature, sizeof(*signature));
    signature->Handle = window;
    signature->ThreadId = GetWindowThreadProcessId(window, &signature->ProcessId);
    signature->Style = (DWORD)GetWindowLong(window, GWL_STYLE);
    signature->ExStyle = (DWORD)GetWindowLong(window, GWL_EXSTYLE);
    GetWindowRect(window, &signature->Rect);
}

// TRUE if the window is known to be ineligible and nothing we can check cheaply
// changed since it was rejected. Deliberately avoids everything that made the old
// per-frame enumeration expensive: no window text, no DWM calls, no allocation.
// cheapest end of that scale - nothing like the cross-process WM_GETTEXT this exists to
// avoid.)
static BOOL IsWindowRejected(IN HWND window)
{
    int index = FindRejectedWindow(window);
    const REJECTED_WINDOW* rejected;
    DWORD processId = 0;
    DWORD threadId;
    RECT rect;

    if (index < 0)
        return FALSE;

    rejected = &g_RejectedWindows[index];

    // Windows recycles window handles: a handle that now belongs to a different
    // thread or process is a different window, and the entry must not apply to it.
    // (Both ids are 0 if the window is gone.)
    threadId = GetWindowThreadProcessId(window, &processId);
    if (threadId != rejected->ThreadId || processId != rejected->ProcessId)
        return FALSE;

    // Backstop for a lost event: everything that can make a window eligible
    // (WS_VISIBLE, size, extended styles) also changes one of these.
    DWORD exStyle = (DWORD)GetWindowLong(window, GWL_EXSTYLE);

    if ((DWORD)GetWindowLong(window, GWL_STYLE) != rejected->Style || exStyle != rejected->ExStyle)
        return FALSE;

    if (!GetWindowRect(window, &rect) || !EqualRect(&rect, &rejected->Rect))
        return FALSE;

    // ...with one exception: the layered alpha, which an app can change with no style
    // change and no WinEvent at all. Checked last because it is the least likely of these
    // to have moved, and it is the only one that costs a call beyond the ones above.

    return TRUE;
}

// Interrogate a window that isn't tracked yet: add it to the watched list if it
// qualifies, otherwise remember it as ineligible.
// Watched windows critical section must be entered.
static ULONG ExamineWindow(IN HWND window, IN OUT UINT* interrogated)
{
    WINDOW_DATA* data = NULL;
    ULONG status;
    REJECTED_WINDOW signature;

    EvictRejectedWindow(window); // whatever we knew about it is being refreshed now
    // Sample BEFORE interrogating: see CacheRejectedWindow for why sampling afterwards can
    // permanently hide a window that became eligible mid-interrogation.
    SampleWindowSignature(window, &signature);
    (*interrogated)++;

    status = GetWindowData(window, &data);
    if (status != ERROR_SUCCESS)
    {
        // Typically a window DWM knows nothing about, so it can't be mapped anyway.
        LogVerbose("0x%x: GetWindowData failed (0x%x)", window, status);
        if (data)
            free(data);
        CacheRejectedWindow(window, &signature);
        return ERROR_SUCCESS;
    }

    if (!ShouldAcceptWindow(data))
    {
        // Interrogating these has a side effect on global state that AddAllWindows()
        // recomputes from scratch (the UAC placeholder sets g_ShowTaskbar, Start sets
        // g_StartVisible which in turn gates Search), so they must not be skipped.
        if (window != g_StartWindow && window != g_SearchWindow &&
            0 != wcscmp(data->Class, UAC_DUMMY_WINDOW_CLASS))
        {
            CacheRejectedWindow(window, &signature);
        }

        LogVerbose("0x%x: rejected", window);
        free(data);
        return ERROR_SUCCESS;
    }

    status = AddWindow(data); // the list takes ownership of data
    if (ERROR_SUCCESS != status)
        win_perror2(status, "AddWindow");

    return status;
}

typedef struct _ADD_WINDOWS_CONTEXT
{
    UINT Interrogated; // windows whose state was actually queried
    ULONG Status;
} ADD_WINDOWS_CONTEXT;

// EnumWindows callback for adding all eligible top-level windows to the list.
// watched windows critical section must be entered
static BOOL CALLBACK AddWindowsProc(IN HWND window, IN LPARAM lParam)
{
    ADD_WINDOWS_CONTEXT* context = (ADD_WINDOWS_CONTEXT*)lParam;

    if (FindWindowByHandle(window)) // already in the list
        return TRUE; // skip to next window

    if (IsWindowRejected(window)) // known to be ineligible, and unchanged since
        return TRUE;

    context->Status = ExamineWindow(window, &context->Interrogated);
    if (ERROR_SUCCESS != context->Status)
        return FALSE; // stop enumeration, fatal error occurred (should probably exit process at this point)

    return TRUE;
}


// Follow the input desktop if it changes under us.
//
// The agent can start while Winlogon is still the input desktop (the logon screen). It then
// attaches to Winlogon correctly - and stays there after autologon switches the input desktop
// to Default. EnumWindows on Winlogon cannot see the user's windows, so nothing is ever added
// to the watched list and the qube renders nothing in dom0 until the agent is restarted.
//
// Measured on a failing cold boot:
//   QGADESK,from=Default,to=Winlogon,SetThreadDesktop=ok
//   QGADESK,event=enumfail,threadDesktop=Winlogon,inputDesktop=Default   (x12, every resync)
//
// Intermittent because it is a race against autologon, and cleared by an agent restart because
// that re-attaches to whatever is current - which is why every check that restarted the agent
// in a live session missed it.
static void EnsureOnInputDesktop(void)
{
    WCHAR mine[128] = L"", input[128] = L"";
    DWORD needed = 0;

    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    HDESK in = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!in)
        return; // cannot tell; try again on the next resync

    if (cur)
        GetUserObjectInformation(cur, UOI_NAME, mine, sizeof(mine), &needed);
    GetUserObjectInformation(in, UOI_NAME, input, sizeof(input), &needed);
    CloseDesktop(in);

    if (mine[0] && input[0] && 0 == wcscmp(mine, input))
        return; // already there

    LogInfo("input desktop changed: '%s' -> '%s', re-attaching", mine, input);
    AttachToInputDesktop();
    // The window hooks only receive events from the desktop the setting thread is attached to,
    // so they have to follow as well or tracking stays dead on the new desktop.
    RearmWindowEvents();
}

// Adds all top-level windows to the watched list.
// This is the resync path: the watched list is normally maintained from window
// events instead (see TrackWindows).
// watched windows critical section must be entered
static ULONG AddAllWindows(IN OUT UINT* interrogated)
{
    ADD_WINDOWS_CONTEXT context = { 0 };

    LogVerbose("start");

    EnsureOnInputDesktop();

    // Keep dom0's stacking in step with the guest's by re-mapping whatever is foreground.
    //
    // dom0 and the guest are otherwise free to disagree about z-order, and when they do, the
    // window dom0 draws on top receives the pixels of whatever covers it in the guest's
    // composited framebuffer - text sliced away mid-drag, see OVERLAP-IN-MOTION.md. The agent
    // has no stacking message, but if the daemon raises a window on MSG_MAP then re-mapping
    // the foreground window is enough, and costs one message per focus change.
    {
        static HWND lastForeground = NULL;
        HWND fg = GetForegroundWindow();
        if (fg && fg != lastForeground)
        {
            WINDOW_DATA* fgData = FindWindowByHandle(fg);
            if (fgData && fgData->IsVisible && !fgData->IsIconic && !fgData->Synthesized)
            {
                lastForeground = fg;
                LogInfo("foreground -> 0x%x, re-mapping to raise it in dom0", fg);
                SendWindowMap(fgData);
            }
        }
    }

    g_TaskbarWindow = FindWindow(L"Shell_TrayWnd", 0);
    g_ShowTaskbar = FALSE;

    ULONG status = ERROR_SUCCESS;
    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, (LPARAM)&context))
    {
        status = context.Status != ERROR_SUCCESS ? context.Status : win_perror("EnumWindows");
        // Correlate the failure with this thread's desktop (see QGADESK in util.c): the
        // question is whether the desktop we are on is the input desktop, a stale one, or a
        // handle that has been closed underneath us.
        {
            WCHAR name[128] = L"?";
            DWORD needed = 0;
            HDESK cur = GetThreadDesktop(GetCurrentThreadId());
            BOOL got = cur ? GetUserObjectInformation(cur, UOI_NAME, name, sizeof(name), &needed) : FALSE;
            HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
            WCHAR iname[128] = L"?";
            if (input)
            {
                GetUserObjectInformation(input, UOI_NAME, iname, sizeof(iname), &needed);
                CloseDesktop(input);
            }
            LogInfo("QGADESK,event=enumfail,tid=%lu,threadDesktop=%s(ok=%d,h=0x%p),inputDesktop=%s",
                GetCurrentThreadId(), name, got, cur, iname);
        }
    }

    *interrogated += context.Interrogated;

    if (g_TaskbarWindow)
    {
        WINDOW_DATA* taskbarEntry = FindWindowByHandle(g_TaskbarWindow);
        if (g_ShowTaskbar)
        {
            if (taskbarEntry) // taskbar is already tracked
                goto end;

            LogDebug("showing taskbar");
            ShowWindow(g_TaskbarWindow, SW_SHOW);
            taskbarEntry = NULL;
            EvictRejectedWindow(g_TaskbarWindow);
            (*interrogated)++;
            status = GetWindowData(g_TaskbarWindow, &taskbarEntry);
            if (status != ERROR_SUCCESS)
            {
                win_perror2(status, "GetWindowData(taskbar)");
                goto end;
            }
            status = AddWindow(taskbarEntry);
        }
        else
        {
            if (!taskbarEntry) // taskbar is not tracked
                goto end;
            status = RemoveWindow(taskbarEntry);
            ShowWindow(g_TaskbarWindow, SW_HIDE);
        }
    }

end:
    LogVerbose("end (%x)", status);
    return status;
}

// Reinitialize watched windows, called after a seamless/fullscreen switch or resolution change.
static ULONG ResetWatch(BOOL seamlessMode)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    ULONG status;

    LogVerbose("start");

    LogDebug("removing all windows");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA *)entry->ListEntry.Flink;

        status = RemoveWindow(entry);
        if (ERROR_SUCCESS != status)
        {
            LeaveCriticalSection(&g_csWatchedWindows);
            return win_perror2(status, "RemoveWindow");
        }

        entry = nextEntry;
    }

    ClearRejectedWindows(); // window acceptance is being decided from scratch

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopWindow = NULL;
    status = ERROR_SUCCESS;

    // WatchForEvents will map the whole screen as one window.
    if (seamlessMode)
    {
        UINT interrogated = 0;

        LogVerbose("seamless mode, adding all windows");
        // Add all eligible windows to watch list.
        // Since this is a switch from fullscreen, no windows were watched.
        EnterCriticalSection(&g_csWatchedWindows);
        status = AddAllWindows(&interrogated);
        LeaveCriticalSection(&g_csWatchedWindows);

        // The list is authoritative again, restart the resync interval.
        g_LastResyncTime = GetTickCount();

        if (g_TaskbarWindow)
            ShowWindow(g_TaskbarWindow, SW_HIDE);
    }
    else
    {
        g_TaskbarWindow = FindWindow(L"Shell_TrayWnd", 0);
        if (g_TaskbarWindow)
            ShowWindow(g_TaskbarWindow, SW_SHOW);
    }

    LogVerbose("end (%x)", status);
    return status;
}

// set fullscreen/seamless mode
ULONG SetSeamlessMode(IN BOOL seamlessMode, IN BOOL forceUpdate)
{
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start");
    LogDebug("Seamless mode changing to %d", seamlessMode);

    if (g_SeamlessMode == seamlessMode && !forceUpdate)
        goto end; // nothing to do

    status = CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, seamlessMode, NULL);
    if (status != ERROR_SUCCESS)
        LogWarning("Failed to write seamless mode registry value");

    // NEVEREXIT guard: with the screen window not announced (A7 degraded capture
    // before CREATE(0) went out, or between screen DESTROY and the restart) the
    // window-0 MAP/UNMAP below would hit gui-daemon's "msg without CREATE" exit(1)
    // and take down the qube's GUI. Record the mode only; StartFrameProcessing
    // applies it for real via SetSeamlessMode(g_SeamlessMode, TRUE) when capture
    // (re)starts.
    if (g_VchanClientConnected && !g_ScreenAnnounced)
    {
        g_SeamlessMode = seamlessMode;
        LogWarning("NEVEREXIT seamless mode %d recorded only (screen window not announced); applied on capture (re)start",
            seamlessMode);
        status = ERROR_SUCCESS;
        goto end;
    }

    if (!seamlessMode)
    {
        // show the screen window
        status = SendWindowMap(NULL);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowMap(NULL)");
            goto end;
        }
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            LogDebug("Changing resolution to match host's");
            status = RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, L"seamless-force");
            // FIXME: wait until the resolution actually changes?
            if (status != ERROR_SUCCESS)
            {
                win_perror2(status, "RequestResolutionChange");
                goto end;
            }
        }
        // hide the screen window
        status = SendWindowUnmap(NULL);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowUnmap(NULL)");
            goto end;
        }
    }

    // ResetWatch removes all watched windows.
    // If seamless mode is on, top-level windows are added to watch list.
    status = ResetWatch(seamlessMode);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "ResetWatch");
        goto end;
    }

    g_SeamlessMode = seamlessMode;

    // The published IDD mode set is seamless-dependent (the host size is only in
    // it while seamless is active - resolution.c BuildIddModeSet a1), so a
    // transition must republish it. Only here, after the commit above, does
    // BuildIddModeSet read the new mode. Registry only - the entering-seamless
    // direction is made live by the exact-follow obtain that the
    // RequestResolutionChange above ends in (it reloads the driver itself),
    // and the leaving-seamless direction only DROPS an unused entry, which must
    // not cost a topology change.
    ResolutionRecomputeIddModeSet();

    LogInfo("Seamless mode changed to %d", seamlessMode);
    status = ERROR_SUCCESS;

end:
    LogVerbose("end (%x)", status);
    return status;
}

WINDOW_DATA *FindWindowByHandle(IN HWND window)
{
    WINDOW_DATA *entry;

    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        if (window == entry->Handle)
            return entry;

        entry = (WINDOW_DATA *)entry->ListEntry.Flink;
    }

    return NULL;
}

// filters unwanted windows (not visible, too small etc)
// assumes window state is up to date
BOOL ShouldAcceptWindow(IN const WINDOW_DATA *data)
{
    if (!data->IsVisible)
        return FALSE;

    if (!g_ShowTaskbar && data->Handle == g_TaskbarWindow)
        return FALSE;

    if (data->DeletePending)
        return FALSE;

    if (data->Handle == GetShellWindow())
        return FALSE;

    // hide search regardless of its state if start is being shown
    // FIXME: this is a workaround for search being detected as visible and not DWM-cloaked
    // even if it's really invisible/transparent
    if (g_StartVisible && data->Handle == g_SearchWindow)
    {
        LogDebug("rejecting Search because Start is visible");
        return FALSE;
    }

    // too small?
    // The SM_CXMIN/CYMIN floor models a NORMAL window's decoration minimum; popups have
    // no such minimum. Win11 Alt-nav keytip badges (Xaml_WindowedPopupClass, ~40x46) are
    // real UI that the floor would swallow, so popups get a token floor here.
    // KNOWN GAP (see BOOTSTRAP-win11.md): a sub-floor popup that fails synthesis is still
    // announced, and dom0 borders each one - 12 red-bordered badges around an Alt menu.
    // Intended follow-up: announce sub-floor popups ONLY when they synthesize; drop them
    // silently otherwise.
    if (data->IsOverrideRedirect)
    {
        if (data->Width < 4 || data->Height < 4)
            return FALSE;
    }
    else if (data->Width < g_MinWindowWidth || data->Height < g_MinWindowHeight)
        return FALSE;

    // Win11 shell drag/snap overlays: click-through, uncapturable, full-bleed.
    // Measured on win11-idd-test (FINDINGS 2026-08-01): dragging a window by its guest
    // title bar raises XamlExplorerHostIslandWindow (explorer.exe) 2560x360 at the top of
    // the screen with ex-style TOPMOST|TRANSPARENT|TOOLWINDOW|LAYERED|NOREDIRECTIONBITMAP.
    // WS_EX_NOREDIRECTIONBITMAP means it has no redirection surface, so PrintWindow cannot
    // capture it and the agent falls back to slice-feeding the COMPOSITED DESKTOP - which
    // dom0 then renders as a huge phantom window full of wallpaper for as long as the drag
    // lasts. Snap layouts are meaningless in seamless mode anyway (dom0's WM owns
    // placement), and WS_EX_TRANSPARENT means the user could never click it.
    // Narrow on purpose: all three of click-through + uncapturable + toolwindow must hold,
    // so ordinary DirectComposition app windows (NOREDIRECTIONBITMAP alone, e.g. the XAML
    // popups this build synthesizes) are untouched.
    if ((data->ExStyle & WS_EX_TRANSPARENT) &&
        (data->ExStyle & WS_EX_NOREDIRECTIONBITMAP) &&
        (data->ExStyle & WS_EX_TOOLWINDOW))
    {
        LogDebug("0x%x: click-through uncapturable shell overlay (%s), rejecting",
            data->Handle, data->Class);
        return FALSE;
    }

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (data->Style & WS_CHILD)
    {
        LogDebug("ignoring child window"); // this shouldn't happen as we only enumerate top-level windows
        return FALSE;
    }

    // Ignore the "activate windows" banner in the bottom-right TODO: more precise detection
    if ((data->ExStyle & WS_EX_NOACTIVATE) && (data->ExStyle & WS_EX_TOPMOST) && (data->ExStyle & WS_EX_TRANSPARENT))
    {
        if (wcslen(data->Caption) == 0 && !wcscmp(data->Class, L"Worker Window"))
            return FALSE;
    }

    // ---- compound-window chrome (CLAUDE.md 2A-chrome) -------------------------------
    //
    // Some applications - Office 2013+ is the canonical case - draw their frame shadow and
    // glow with extra TOP-LEVEL windows arranged around the real frame instead of letting
    // DWM do it. To the agent each of those looks like an ordinary top-level window, so it
    // gets mapped, and the gui daemon dutifully draws a qube border around every one of
    // them: a single Office window arrives in dom0 as five separate bordered fragments.
    //
    // These are never anything a user can interact with, so dropping them loses nothing.
    // The rules below are deliberately narrow - a false positive means a real window
    // vanishes from dom0, which is much worse than a spurious border - and they are pure
    // reads of state GetWindowData() already collected, so they cost nothing per frame.

    // NOTE: an "alpha == 0 layered window" rule was considered and DELIBERATELY REJECTED.
    // Windows fades menus and tooltips IN from alpha 0, so sampling one at the moment it
    // appears would reject and cache it, and SetLayeredWindowAttributes emits no WinEvent -
    // the window would stay invisible in dom0 until the next periodic resync (up to
    // WINDOW_RESYNC_INTERVAL_MS). Worse, applying it to an already-mapped window turns a
    // transient fade-out into MSG_DESTROY plus a recreate, losing z-order and focus.
    // Its only benefit was cosmetic (suppressing an empty bordered rect), and it does not
    // even match the Office strips, which are VISIBLE shadows with alpha > 0. A missing
    // window is far worse than a spurious border.

    // Rule 2: owned, click-through, undecorated layered chrome - the Office shadow strips.
    // Every clause is required:
    //   WS_EX_LAYERED|WS_EX_TRANSPARENT  the strip is alpha-blended AND hit-test
    //                                    transparent, i.e. it can never receive a click; a
    //                                    dom0 window for it could not be interacted with
    //                                    even in principle.
    //   Owner != NULL                    it decorates another window of the same app. An
    //                                    unowned top-level window is somebody's real UI
    //                                    (splash screens, HUD overlays) and is left alone.
    //   !WS_CAPTION                      anything with a title bar is a window the user is
    //                                    meant to see as a window. Tested with & rather than
    //                                    HasFlags() on purpose: WS_CAPTION is
    //                                    WS_BORDER|WS_DLGFRAME, so this also spares a merely
    //                                    bordered window - erring towards keeping windows.
    //                                    Office's strips are plain WS_POPUP, no border bits.
    //   !WS_EX_APPWINDOW                 the app explicitly asked for a taskbar button, so
    //                                    it considers this a first-class window.
    //   WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW (BOTH)  the app declared it non-activatable
    //                                    AND non-taskbar. Office strips carry both. Requiring
    //                                    both on purpose: a comctl32 tooltip is owned,
    //                                    toolwindow and can be WS_EX_TRANSPARENT, but is
    //                                    generally NOT WS_EX_NOACTIVATE, so demanding both
    //                                    keeps tooltips visible.
    // Note this is strictly narrower than the pre-existing "activate windows" banner rule
    // above, which additionally pins the class name; that one stays as is.
    if (HasFlags(data->ExStyle, WS_EX_LAYERED | WS_EX_TRANSPARENT) &&
        data->Owner != NULL &&
        !(data->Style & WS_CAPTION) &&
        !(data->ExStyle & WS_EX_APPWINDOW) &&
        HasFlags(data->ExStyle, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW))
    {
        // LogVerbose, not LogDebug: during a drag every strip emits
        // EVENT_OBJECT_LOCATIONCHANGE, its cached signature changes (rect moved) and it
        // is re-examined at input rate, so this would be hundreds of formatted lines a
        // second with a cross-process class name on the hot path.
        LogVerbose("0x%x: rejecting compound-window chrome (class '%s', owner 0x%x, style 0x%x, exstyle 0x%x)",
            data->Handle, data->Class, data->Owner, data->Style, data->ExStyle);
        return FALSE;
    }

    // Rule 3: Office's shadow strips, by class. Office gives them a dedicated window class,
    // MSO_BORDEREFFECT_WINDOW_CLASS - measured on a real Microsoft 365 install (FINDINGS
    // 2026-08-02): four 8 px strips of that class ringing Word's first-run sign-in dialog,
    // owned by that DIALOG rather than by the main OpusApp frame. An exact discriminator
    // where rule 2's style heuristic is not: on the build this rule was written against,
    // the strips passed every rule above and were admitted to the watched list.
    //
    // They are pure decoration: Office draws the frame shadow itself instead of letting DWM
    // do it, as described at the top of this section. The gui daemon borders every window
    // the agent announces, so dropping them does not weaken daemon-side bordering
    // (CLAUDE.md 2A-chrome rule 4) - it stops presenting decoration fragments as windows.
    //
    // It also removes the precondition for a crash that is now fully understood (FINDINGS
    // 2026-08-03): admitted, the strips were synthesized, materialized in one burst ("owner
    // geometry changed, materializing child" x4), and the frame loop then sent MSG_CONFIGURE
    // for a window it had never announced - gui-daemon exits on that ("msg 0x86 without
    // CREATE for 0x20340"), costing the qube its GUI. The send-side gate and the overlap test
    // already break that chain; this rule removes its precondition entirely, and also stops
    // the strips being re-homed onto the maximized main frame they happen to overlap, where
    // they paint a shadow band across the document. A window rejected here can take none of
    // those steps: ExamineWindow() drops it before AddWindow(), and AddWindow() is both the only
    // insertion into the watched list and the only caller of SynthActivate(), so it is
    // never announced, never synthesized, and has no synthesis to materialize out of.
    if (0 == wcscmp(data->Class, MSO_BORDER_EFFECT_CLASS))
    {
        // LogVerbose for rule 2's reason: the strips move with the window they decorate,
        // so during a drag this is re-evaluated at input rate.
        LogVerbose("0x%x: rejecting Office frame shadow strip (owner 0x%x, %ux%u)",
            data->Handle, data->Owner, data->Width, data->Height);
        return FALSE;
    }

    // DWM-cloaked windows are the third 2A-chrome case; they are rejected by the
    // !data->IsVisible test at the top of this function, because GetWindowData() folds
    // DWMWA_CLOAKED into IsVisible. No separate rule needed here.

    return TRUE;
}

// Refresh data about a window, send notifications to gui daemon if needed.
// Marks the window for removal from the list if the new state makes it no longer eligible.
// Watched windows critical section must be entered.
static ULONG UpdateWindowData(IN OUT WINDOW_DATA *windowData)
{
    ULONG status = ERROR_SUCCESS;

    LogVerbose("start, 0x%x", windowData->Handle);

    WINDOW_DATA data;
    WINDOW_DATA* ptr = &data;

    if (!IsWindow(windowData->Handle))
    {
        LogDebug("0x%x is destroyed, marking for removal", windowData->Handle);
        windowData->DeletePending = TRUE;
        goto end;
    }

    // get current window state
    status = GetWindowData(windowData->Handle, &ptr);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "GetWindowData");
        goto end;
    }

    // Synthesized windows produce NO protocol traffic. Refresh local geometry (the
    // frame loop paints them from it), keep the owner's capture mask in step, and
    // materialize them as real windows if they stop qualifying (moved out of the
    // owner, owner changed) - materialization happens by dropping synthesis here and
    // letting the standard re-examination path announce them.
    if (windowData->Synthesized)
    {
        BOOL geomChanged = (windowData->X != data.X || windowData->Y != data.Y ||
            windowData->Width != data.Width || windowData->Height != data.Height);
        windowData->X = data.X;
        windowData->Y = data.Y;
        windowData->Width = data.Width;
        windowData->Height = data.Height;
        windowData->Style = data.Style;
        windowData->ExStyle = data.ExStyle;
        windowData->Owner = data.Owner;
        windowData->ProcessId = data.ProcessId;
        windowData->IsIconic = data.IsIconic;
        windowData->IsVisible = data.IsVisible;
        windowData->IsOverrideRedirect = data.IsOverrideRedirect;

        if (!data.IsVisible || !ShouldAcceptWindow(windowData))
        {
            windowData->DeletePending = TRUE; // silent removal via RemoveWindow
            status = ERROR_SUCCESS;
            goto end;
        }

        WINDOW_DATA* owner = NULL;
        if (!SynthQualifies(windowData, &owner))
        {
            LogInfo("0x%x: no longer owner-contained, materializing as a dom0 window",
                windowData->Handle);
            SynthDeactivate(windowData);
            // Re-announce from scratch: drop it from the list and let the next
            // tracking pass add it through the normal path (CREATE/attach/MAP).
            windowData->DeletePending = TRUE;
        }
        else if (geomChanged)
        {
            // Do NOT compute the mask here: the owner may be interrogated later in
            // this same pass (a joint move updates the two positions separately), so
            // a push now would publish a mixed-state mask and force a recapture, at
            // input rate during a drag. TrackWindows flushes once per pass.
            owner->SynthMaskPending = TRUE;
        }
        status = ERROR_SUCCESS;
        goto end;
    }

    // While maximized, cap the reported size at what the dom0 WM last said it can
    // display (recorded by HandleConfigure); the per-window dump then matches the dom0
    // window exactly instead of overflowing it by the height of dom0's decorations.
    if ((data.Style & WS_MAXIMIZE) && windowData->DaemonMaxValid)
    {
        if (data.Width > windowData->DaemonMaxW)
            data.Width = windowData->DaemonMaxW;
        if (data.Height > windowData->DaemonMaxH)
            data.Height = windowData->DaemonMaxH;
    }

    if (windowData->IsVisible != data.IsVisible)
    {
        windowData->IsVisible = data.IsVisible;
        LogDebug("0x%x IsVisible changed to %d", data.Handle, data.IsVisible);
        if (!data.IsVisible && !data.IsIconic)
            goto end; // skip other stuff, this window will be removed
    }

    if (windowData->Style != data.Style)
    {
        windowData->Style = data.Style;
        LogDebug("0x%x style changed to 0x%x", data.Handle, data.Style);
    }

    if (windowData->ExStyle != data.ExStyle)
    {
        windowData->ExStyle = data.ExStyle;
        LogDebug("0x%x exstyle changed to 0x%x", data.Handle, data.ExStyle);

        // Windows can BECOME layered after attach (Edge sets WS_EX_LAYERED +
        // UpdateLayeredWindow on its first-run overlay only after showing it).
        // PrintWindow returns premultiplied source pixels for ULW surfaces - a 30%-alpha
        // dimming backdrop captures as near-black - so rebuild the attachment: the
        // re-attach comes back slice-fed (content copied from the composited screen),
        // which stays window-relative and renders correctly wherever dom0 places the
        // window. PwResizeWindow already implements detach+reattach with the
        // daemon-release fallback.
        if (PwIsAttached(windowData) && !windowData->PwSliceFed &&
            !PwWindowEligible(windowData))
        {
            LogInfo("0x%x: became PrintWindow-ineligible (layered), rebuilding slice-fed",
                windowData->Handle);
            (void)PwResizeWindow(windowData);
        }
    }

    // Not diffed (nothing is sent to dom0 when they change), but they ARE inputs of
    // ShouldAcceptWindow(), which is re-evaluated against windowData at the end of this
    // function. Left stale, a window that turned into chrome - or one whose layered alpha
    // dropped to 0 - would keep being mapped.
    windowData->Owner = data.Owner;

    // caption
    if (0 != wcscmp(windowData->Caption, data.Caption))
    {
        // caption changed
        StringCchCopy(windowData->Caption, ARRAYSIZE(windowData->Caption), data.Caption);
        status = SendWindowName(windowData->Handle, windowData->Caption);
        if (status != ERROR_SUCCESS)
            goto end;
    }

    // minimized state changed
    if (data.IsIconic)
    {
        if (!windowData->IsIconic)
        {
            LogDebug("0x%x became minimized", windowData->Handle);
            windowData->IsIconic = TRUE;
            status = SendWindowFlags(windowData->Handle, WINDOW_FLAG_MINIMIZE, 0);
        }
        // ignore position changes, iconic windows have coords like (-32000,-32000)
        goto end;
    }
    else
    {
        if (windowData->IsIconic)
        {
            LogVerbose("0x%x became restored", windowData->Handle);
            status = SendWindowFlags(windowData->Handle, 0, WINDOW_FLAG_MINIMIZE); // unset minimize
            if (status != ERROR_SUCCESS)
                goto end;
            // A window added while minimized never got a per-window buffer (attach is
            // gated on !IsIconic); attach now that it has real content. Failure just
            // leaves it on the legacy path.
            if (PwEnabled() && !PwIsAttached(windowData) && windowData->IsVisible &&
                windowData->Width > 0 && windowData->Height > 0)
            {
                if (PwAttachWindow(windowData) == ERROR_SUCCESS)
                    (void)SendWindowDamageEvent(windowData->Handle, 0, 0,
                        windowData->Width, windowData->Height);
            }
        }
        windowData->IsIconic = FALSE;
    }

    // coords
    BOOL coordsChanged = (windowData->X != data.X || windowData->Y != data.Y ||
        windowData->Width != data.Width || windowData->Height != data.Height);

    if (coordsChanged)
    {
        LogVerbose("coords changed: 0x%x (%d,%d) %dx%d -> (%d,%d) %dx%d",
            windowData->Handle, windowData->X, windowData->Y, windowData->Width, windowData->Height,
            data.X, data.Y, data.Width, data.Height);

        windowData->X = data.X;
        windowData->Y = data.Y;
        windowData->Width = data.Width;
        windowData->Height = data.Height;

        // A slice-fed window that moved now overlaps a different screen region; its
        // buffer still holds the old region's pixels. Schedule a full re-copy.
        if (windowData->PwSliceFed)
            windowData->PwSliceNeedsFull = TRUE;
    }

    BOOL oldPopupState = windowData->IsOverrideRedirect;
    BOOL popupStateChanged = windowData->IsOverrideRedirect != data.IsOverrideRedirect;
    if (popupStateChanged)
    {
        LogDebug("0x%x: popup state changed from %d to %d", windowData->Handle, windowData->IsOverrideRedirect,
            data.IsOverrideRedirect);

        windowData->IsOverrideRedirect = data.IsOverrideRedirect;
    }

    // Order of sending updates to the gui daemon here is important (window size vs popup state):
    // wrong order can trigger the override-redirect protection for large windows.
    // Case 1: window became large enough to trigger protection, but the popup state changed from true to false.
    // In this case we need to send the popup state first, then the size update.
    // Case 2: window became small (from large), popup state changed from false to true.
    // In this case we need to send the size update first, then the popup state.
    // For other combinations the order doesn't matter, so we use the old popup state to determine the order.
    if (oldPopupState)
    {
        // popup state first, then configure
        if (popupStateChanged)
        {
            status = ToggleMap(windowData);
            if (status != ERROR_SUCCESS)
                goto end;
        }

        if (coordsChanged)
        {
            status = SendWindowConfigureIfChanged(windowData);
            if (status != ERROR_SUCCESS)
                goto end;
        }
    }
    else
    {
        // configure first, then popup state
        if (coordsChanged)
        {
            status = SendWindowConfigureIfChanged(windowData);
            if (status != ERROR_SUCCESS)
                goto end;
        }

        if (popupStateChanged)
        {
            status = ToggleMap(windowData);
            if (status != ERROR_SUCCESS)
                goto end;
        }
    }

    // The per-window buffer is size-bound: whenever the live size diverges from the
    // geometry the buffer was granted for, rebuild (new buffer + grant + WINDOW_DUMP)
    // and repaint. Comparing against PwWidth/PwHeight instead of this pass's delta also
    // catches dom0-initiated resizes, where HandleConfigure pre-writes Width/Height and
    // a plain sizeChanged never fires. Failure inside PwResizeWindow already forces the
    // daemon back to the legacy path via unmap/map.
    if (PwIsAttached(windowData) &&
        (windowData->PwWidth != windowData->Width ||
         windowData->PwHeight != windowData->Height))
    {
        if (PwResizeWindow(windowData) == ERROR_SUCCESS)
        {
            ULONG damageStatus = SendWindowDamageEvent(windowData->Handle, 0, 0,
                windowData->Width, windowData->Height);
            if (damageStatus != ERROR_SUCCESS)
                win_perror2(damageStatus, "SendWindowDamageEvent(resize)");
        }
        else
        {
            LogDebug("0x%x: per-window rebuild failed, window on legacy path",
                windowData->Handle);
        }
    }

    // Owner-side: keep the capture mask aligned after the owner moved/resized, and
    // re-check that its synthesized children are still contained. The mask itself is
    // pushed once per tracking pass (SynthFlushMasks) - the children may not have
    // been interrogated yet in this pass, and a mixed-state push forces a recapture.
    if (windowData->SynthChildCount > 0)
    {
        for (LIST_ENTRY* e = g_WatchedWindowsList.Flink; e != &g_WatchedWindowsList; )
        {
            WINDOW_DATA* c = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
            e = e->Flink;
            if (!c->Synthesized || c->SynthOwner != windowData->Handle)
                continue;
            WINDOW_DATA* stillOwner = NULL;
            if (!SynthQualifies(c, &stillOwner))
            {
                LogInfo("0x%x: owner geometry changed, materializing child", c->Handle);
                SynthDeactivate(c);
                c->DeletePending = TRUE;
            }
        }
        if (windowData->SynthChildCount > 0)
            windowData->SynthMaskPending = TRUE;
    }

    // TODO: should we care about style changes? some of them affect Z-order (topmost etc)

    if (data.ModalParent != windowData->ModalParent)
    {
        LogDebug("0x%x: modal parent changed from 0x%x to 0x%x",
            data.Handle, windowData->ModalParent, data.ModalParent);
        windowData->ModalParent = data.ModalParent;
        // need to toggle map since this is the only way to change modal status for gui daemon
        status = ToggleMap(windowData);
        if (ERROR_SUCCESS != status)
            goto end;
    }

    status = ERROR_SUCCESS;

end:
    if (!windowData->DeletePending && !ShouldAcceptWindow(windowData))
    {
        LogDebug("0x%x no longer eligible, marking for removal", windowData->Handle);
        windowData->DeletePending = TRUE;
    }

    LogVerbose("end (%x)", status);
    return status;
}

// What one TrackWindows() pass did, for the QGAPERF instrumentation.
typedef struct _TRACK_STATS
{
    LONGLONG UpdateTicks;   // refreshing already tracked windows
    LONGLONG EnumTicks;     // admitting new windows
    LONGLONG RemoveTicks;   // dropping windows that became ineligible
    UINT Interrogated;      // windows whose state was actually queried
    UINT Events;            // window events applied
    BOOL Resync;            // this pass was a full EnumWindows() resync
} TRACK_STATS;

// Tracking work accumulated between frames (window events processed while no frame
// was being handled). Folded into the next frame's record so the QGAPERF numbers
// keep accounting for all of the tracking cost. Main loop only.
static LONGLONG g_TrackedUpdateTicks = 0;
static LONGLONG g_TrackedEnumTicks = 0;
static LONGLONG g_TrackedRemoveTicks = 0;
static UINT g_TrackedInterrogated = 0;
static UINT g_TrackedEvents = 0;

// One window tracking pass: apply the events collected by the hook thread (or
// re-enumerate everything if a resync is due) and send the resulting notifications
// to the gui daemon.
// Watched windows critical section must be entered.
static ULONG TrackWindows(OUT TRACK_STATS* stats)
{
    HWND pending[PENDING_WINDOWS_MAX];
    DWORD pendingEvents[PENDING_WINDOWS_MAX]; // why each was queued (see WindowEventForcesReexamine)
    UINT pendingCount = 0;
    WINDOW_DATA* entry;
    WINDOW_DATA* nextEntry;
    ULONG phaseStatus = ERROR_SUCCESS;
    // Like the code this replaces, only a failed removal is reported to the caller:
    // failing to read or add one window must not abort processing of the frame.
    ULONG status = ERROR_SUCCESS;
    LONGLONG perfPhase, perfSendPhase;

    ZeroMemory(stats, sizeof(*stats));
    stats->Resync = TakePendingWindows(pending, pendingEvents, PENDING_WINDOWS_MAX, &pendingCount);
    stats->Events = pendingCount;

    // Refresh the state of tracked windows: all of them on a resync, otherwise only
    // those an event was reported for. If a window is no longer eligible (destroyed,
    // hidden...) then mark it for removal but keep it in the list for now, so that
    // the next phase can skip it.
    perfPhase = PerfNow();
    perfSendPhase = g_PerfSendTicks;

    if (stats->Resync)
    {
        entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
        while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
        {
            entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
            nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;

            stats->Interrogated++;
            phaseStatus = UpdateWindowData(entry);
            if (phaseStatus != ERROR_SUCCESS)
            {
                win_perror2(phaseStatus, "UpdateWindowData");
                entry->DeletePending = TRUE;
                // TODO: exit if there was a vchan failure and we not just failed to get window data
            }

            entry = nextEntry;
        }
    }
    else
    {
        for (UINT i = 0; i < pendingCount; i++)
        {
            entry = FindWindowByHandle(pending[i]);
            if (!entry)
                continue; // not tracked (yet), handled below

            stats->Interrogated++;
            phaseStatus = UpdateWindowData(entry);
            if (phaseStatus != ERROR_SUCCESS)
            {
                win_perror2(phaseStatus, "UpdateWindowData");
                entry->DeletePending = TRUE;
            }
        }
    }

    stats->UpdateTicks = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
    perfPhase = PerfNow();
    perfSendPhase = g_PerfSendTicks;

    // Admit windows that aren't tracked yet.
    if (stats->Resync)
    {
        AddAllWindows(&stats->Interrogated);
    }
    else
    {
        for (UINT i = 0; i < pendingCount; i++)
        {
            if (FindWindowByHandle(pending[i])) // tracked, refreshed above
                continue;

            if (!IsWindow(pending[i])) // gone, nothing to add
            {
                EvictRejectedWindow(pending[i]);
                continue;
            }

            // An event means "look at this one again" - but only actually re-interrogate
            // when the cached rejection can no longer be trusted. Without this check an
            // ineligible but event-noisy window (DWM helpers, tooltips, hidden Electron
            // frames, anything dragged) re-pays the full ~340 us GetWindowText +
            // DwmGetWindowAttribute cost on every event, at input rate, holding
            // g_csWatchedWindows - exactly the cost Phase 2A removes, on an unbounded
            // guest-controlled schedule.
            if (!WindowEventForcesReexamine(pendingEvents[i]) && IsWindowRejected(pending[i]))
                continue;

            phaseStatus = ExamineWindow(pending[i], &stats->Interrogated);
            if (phaseStatus != ERROR_SUCCESS)
            {
                // D8: the rest of this batch was already dequeued and would be lost.
                QueueWindowEvent(NULL, 0, TRUE);
                break;
            }
        }
    }

    stats->EnumTicks = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
    perfPhase = PerfNow();
    perfSendPhase = g_PerfSendTicks;

    // Remove windows marked for deletion.
    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;

        if (entry->DeletePending)
        {
            ULONG removeStatus = RemoveWindow(entry);
            if (removeStatus != ERROR_SUCCESS)
            {
                win_perror2(removeStatus, "RemoveWindow");
                status = removeStatus;
                break;
            }
        }

        entry = nextEntry;
    }

    // Every interrogation of this pass is done (and removals settled): push the
    // capture-mask updates they deferred, at most one per owner (SynthFlushMasks).
    SynthFlushMasks();

    stats->RemoveTicks = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
    return status;
}

// Apply pending window events outside of frame processing. This is what makes
// window moves reach the gui daemon at input rate instead of at capture rate.
static void ProcessWindowEvents(void)
{
    PwRevokeTick();

    // Reap dead capture channels: a WGC session that threw (window gone mid-poll,
    // device loss) stops delivering silently. Detach and force the daemon back to the
    // screen path with an unmap/map so the window updates again instead of freezing.
    if (PwEnabled())
    {
        EnterCriticalSection(&g_csWatchedWindows);
        LIST_ENTRY* pwe = g_WatchedWindowsList.Flink;
        while (pwe != &g_WatchedWindowsList)
        {
            WINDOW_DATA* wd = CONTAINING_RECORD(pwe, WINDOW_DATA, ListEntry);
            pwe = pwe->Flink;
            if (PwIsAttached(wd) && !wd->PwSliceFed && WcIsDead(wd->Handle))
            {
                LogWarning("0x%x: capture channel died, reverting to legacy path", wd->Handle);
                PwDetachWindow(wd);
                if (wd->IsVisible || wd->IsIconic)
                {
                    if (SendWindowUnmap(wd->Handle) == ERROR_SUCCESS)
                        (void)SendWindowMap(wd);
                }
            }
        }
        LeaveCriticalSection(&g_csWatchedWindows);
    }

    TRACK_STATS stats;
    ULONG status;

    EnterCriticalSection(&g_csWatchedWindows);
    status = TrackWindows(&stats);
    LeaveCriticalSection(&g_csWatchedWindows);

    // Work-area drift check (event path; the frame path in WatchForEvents has the
    // matching call). Self-rate-limited inside, and must run outside
    // g_csWatchedWindows: a re-assert does cross-process window calls.
    WorkAreaEnsureApplied();

    if (status != ERROR_SUCCESS)
        win_perror2(status, "TrackWindows");

    g_TrackedUpdateTicks += stats.UpdateTicks;
    g_TrackedEnumTicks += stats.EnumTicks;
    g_TrackedRemoveTicks += stats.RemoveTicks;
    g_TrackedInterrogated += stats.Interrogated;
    g_TrackedEvents += stats.Events;
}

// Called after receiving new frame.

// Assign each watched window its position in the guest z-order (0 = topmost).
//
// The desktop framebuffer holds the COMPOSITED desktop, so a dirty rect in screen coordinates
// covers whatever is visible there - which belongs to the TOPMOST window over that area, not
// to every window whose rectangle contains it. Without an ordering there is no way to tell
// which window a given pixel actually belongs to.
//
// This is a bare EnumWindows: it touches no window properties, so it costs a fraction of the
// per-frame enumeration Phase 2A removed (which called GetWindowLong/GetWindowRect per window).
static BOOL g_ZOrderValid = FALSE;

static BOOL CALLBACK ZOrderProc(HWND window, LPARAM lParam)
{
    int* next = (int*)lParam;
    WINDOW_DATA* entry = FindWindowByHandle(window);
    if (entry)
        entry->ZOrder = (*next)++;
    return TRUE;
}

// Order the watched list topmost-first into `sorted`, returning how many were placed.
static UINT CollectZOrder(WINDOW_DATA** sorted, UINT capacity)
{
    WINDOW_DATA* entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        entry->ZOrder = INT_MAX; // not seen by EnumWindows => treat as bottom
        entry = (WINDOW_DATA*)entry->ListEntry.Flink;
    }

    // Clipping now applies only to override-redirect popups, so the ordering is only needed
    // when one is on screen - which is a second or two at a time. Paying a full EnumWindows
    // on every frame for a case that is almost never active cost roughly 4x the Phase 2A
    // drag figure. Skip it, and report the order as unknown so nothing clips.
    BOOL anyPopup = FALSE;
    WINDOW_DATA* scan = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (scan != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        scan = CONTAINING_RECORD(scan, WINDOW_DATA, ListEntry);
        if (scan->IsOverrideRedirect && scan->IsVisible)
        {
            anyPopup = TRUE;
            break;
        }
        scan = (WINDOW_DATA*)scan->ListEntry.Flink;
    }
    if (!anyPopup)
    {
        // still hand back the window list, just without a trustworthy order
        UINT n = 0;
        WINDOW_DATA* e = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
        while (e != (WINDOW_DATA*)&g_WatchedWindowsList && n < capacity)
        {
            e = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
            sorted[n++] = e;
            e = (WINDOW_DATA*)e->ListEntry.Flink;
        }
        g_ZOrderValid = FALSE;
        return n;
    }

    int next = 0;
    // EnumWindows can fail (observed: ERROR_INVALID_HANDLE). If it does, every ZOrder stays
    // INT_MAX, the sort below is arbitrary, and clipping against an arbitrary order is far
    // worse than not clipping: if the desktop window (which spans the whole screen) sorts
    // first it claims everything as covered and NOTHING else receives damage - the entire
    // qube renders stale. Failure must degrade to "do not clip", never to "clip wrongly".
    g_ZOrderValid = EnumWindows(ZOrderProc, (LPARAM)&next) ? TRUE : FALSE;
    if (!g_ZOrderValid)
    {
        static DWORD lastComplaint = 0;
        DWORD now = GetTickCount();
        if (now - lastComplaint > 5000)
        {
            lastComplaint = now;
            win_perror("EnumWindows (z-order); damage clipping disabled for now");
        }
    }

    UINT count = 0;
    entry = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList && count < capacity)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        sorted[count++] = entry;
        entry = (WINDOW_DATA*)entry->ListEntry.Flink;
    }

    // Any window EnumWindows did not report has an unknown position, so the whole ordering
    // is untrustworthy - treat it the same as an outright failure.
    for (UINT i = 0; i < count; i++)
    {
        if (sorted[i]->ZOrder == INT_MAX)
        {
            g_ZOrderValid = FALSE;
            break;
        }
    }

    // insertion sort: the list is small (single digits) and nearly ordered in practice
    for (UINT i = 1; i < count; i++)
    {
        WINDOW_DATA* key = sorted[i];
        int j = (int)i - 1;
        while (j >= 0 && sorted[j]->ZOrder > key->ZOrder)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return count;
}

static HWND g_LastPopupDamageWindow = NULL;

// While a PrintWindow-fed window is moving, refresh its content at most this often.
// Covers content that genuinely changes mid-drag (video, progress bars); the engine's
// 250 ms round-robin sweep (wincapture.cpp) independently bounds staleness for
// everything else. Do not lower this to "every frame" - that IS the 17 ms/frame drag
// regression (instrumentation/qwtfull-w10/bench-qwtfull-w10.md).
#define PW_MOVE_RECAPTURE_MS 150

// Motion counts as over only after this much quiet. Mid-drag frames occasionally
// apply no LOCATIONCHANGE (~5% of drag frames in the bench data), and treating a
// single still frame as the end of the drag would fire a full-cost settle recapture
// right back into the motion.
#define PW_MOVE_SETTLE_MS 150

// Copy `area` (screen coords) of the mapped desktop frame into a slice-fed window's
// per-window buffer and send the matching window-relative damage. Clips to the screen,
// the window rect, and the granted buffer geometry; silently skips when the frame is
// not mapped (content then arrives with the next mapped frame).
static void PwSliceCopyAndDamage(IN OUT WINDOW_DATA* entry, IN const CAPTURE_FRAME* frame,
                                 IN const BYTE* fb, IN const RECT* area)
{
    // fb is the persistently-granted desktop image (ctx->framebuffer): its address is
    // constant for the life of the duplication and the daemon reads it live, so it is
    // always current here - do NOT gate on frame->mapped, which is only TRUE on the
    // very first frame (MapDesktopSurface runs once, for the pointer to grant).
    if (!fb || frame->rect.Pitch <= 0 || !entry->PwBuffer)
        return;

    RECT screenR = { 0, 0, (LONG)min(g_ScreenWidth, g_FbWidth), (LONG)min(g_ScreenHeight, g_FbHeight) };
    RECT winR = { entry->X, entry->Y,
                  entry->X + (int)entry->PwWidth, entry->Y + (int)entry->PwHeight };
    RECT r;
    if (!IntersectRect(&r, area, &winR))
        return;
    if (!IntersectRect(&r, &r, &screenR))
        return;

    int relX = r.left - entry->X;
    int relY = r.top - entry->Y;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (relX < 0 || relY < 0 || w <= 0 || h <= 0)
        return;
    if ((ULONG)(relX + w) > entry->PwWidth || (ULONG)(relY + h) > entry->PwHeight)
        return; // buffer geometry changed underneath; next full copy repaints

    const BYTE* src = fb +
        (size_t)r.top * frame->rect.Pitch + (size_t)r.left * 4;
    BYTE* dst = (BYTE*)entry->PwBuffer +
        ((size_t)relY * entry->PwWidth + (size_t)relX) * 4;
    for (int row = 0; row < h; row++)
    {
        memcpy(dst, src, (size_t)w * 4);
        src += frame->rect.Pitch;
        dst += (size_t)entry->PwWidth * 4;
    }

    (void)SendWindowDamageEvent(entry->Handle, relX, relY, w, h);
}

// Paint this owner's synthesized children into its buffer from the composited
// desktop image and report the damage. The owner's capture masks these rects (see
// SynthUpdateMask), so nothing overwrites them afterwards. Called per frame for
// owners with children; `area` limits work to the region that actually changed.
// Copy one synthesized child's region (optionally limited to `area`) out of the live
// desktop image into the owner's buffer and report it as owner damage.
static void PwPatchSynthChildClipped(IN WINDOW_DATA* owner, IN const WINDOW_DATA* c,
                                     IN const RECT* area)
{
    if (!g_FbBits || g_FbPitch <= 0 || !owner->PwBuffer)
    {
        LogWarning("synth paint 0x%x: no source (fb=%p pitch=%d buf=%p)",
            c->Handle, g_FbBits, g_FbPitch, owner->PwBuffer);
        return;
    }

    RECT childR = { c->X, c->Y, c->X + (int)c->Width, c->Y + (int)c->Height };
    RECT ownerR = { owner->X, owner->Y,
                    owner->X + (int)owner->PwWidth, owner->Y + (int)owner->PwHeight };
    RECT screenR = { 0, 0, (LONG)min(g_ScreenWidth, g_FbWidth), (LONG)min(g_ScreenHeight, g_FbHeight) };
    RECT r;
    if (!IntersectRect(&r, &childR, &ownerR) || !IntersectRect(&r, &r, &screenR))
    {
        LogWarning("synth paint 0x%x: child (%d,%d)-(%d,%d) outside owner (%d,%d)-(%d,%d)",
            c->Handle, childR.left, childR.top, childR.right, childR.bottom,
            ownerR.left, ownerR.top, ownerR.right, ownerR.bottom);
        return;
    }
    if (area && !IntersectRect(&r, &r, area))
        return;

    int relX = r.left - owner->X, relY = r.top - owner->Y;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (relX < 0 || relY < 0 || w <= 0 || h <= 0)
        return;
    if ((ULONG)(relX + w) > owner->PwWidth || (ULONG)(relY + h) > owner->PwHeight)
    {
        LogWarning("synth paint 0x%x: rel (%d,%d) %dx%d exceeds owner buffer %ux%u",
            c->Handle, relX, relY, w, h, owner->PwWidth, owner->PwHeight);
        return;
    }

    LogInfo("QGAPROTO,msg=SYNTHPAINT,hwnd=0x%x,owner=0x%x,rx=%d,ry=%d,w=%d,h=%d",
        (uint32_t)(ULONG_PTR)c->Handle, (uint32_t)(ULONG_PTR)owner->Handle,
        relX, relY, w, h);

    const BYTE* src = g_FbBits + (size_t)r.top * g_FbPitch + (size_t)r.left * 4;
    BYTE* dst = (BYTE*)owner->PwBuffer +
        ((size_t)relY * owner->PwWidth + (size_t)relX) * 4;
    for (int row = 0; row < h; row++)
    {
        memcpy(dst, src, (size_t)w * 4);
        src += g_FbPitch;
        dst += (size_t)owner->PwWidth * 4;
    }
    (void)SendWindowDamageEvent(owner->Handle, relX, relY, w, h);
}

static void PwPatchSynthRect(IN WINDOW_DATA* owner, IN const WINDOW_DATA* child)
{
    PwPatchSynthChildClipped(owner, child, NULL);
}

// Has the SCREEN content over this window's rect changed since the last recapture trigger?
//
// WHY THIS EXISTS. Windows 11 presents far more frames than Windows 10 for the same input -
// measured at 488 vs 259 frames over an identical 20 s typing workload with the agent,
// display path (Basic Display Adapter) and resolution (3440x1440) all held constant, i.e.
// 1.88x with only the OS differing. Every present whose dirty rect touches a window triggers
// a PrintWindow recapture. The surplus ones are byte-identical, so the row-diff in
// wincapture.cpp sends nothing to dom0 - the wasted work is the capture itself (~15-18 ms on
// a WARP guest). Nothing in the DDA dirty rects distinguishes "DWM re-presented the same
// pixels" from "the user typed a character", so the only cheap discriminator is the screen
// content itself: hashing the window's rect costs memcmp-class time (~0.2 ms for 800x600).
//
// WHY A HASH AND NOT A RETAINED COPY. A copy would cost ~1.7 MB per window; the hash is 8
// bytes. A collision would skip one real update, but wincapture's round-robin sweep refreshes
// every attached window regardless of dirty rects, so a missed update converges instead of
// leaving a permanently stale window. That safety net is what makes hashing acceptable here.
//
// WHY THE OCCLUSION GUARD IS MANDATORY. PrintWindow renders the WINDOW; the screen shows
// whatever is on top of it. If anything overlaps the window, screen pixels are not a proxy
// for window content - an occluded window could change underneath an unchanged screen region
// and we would skip a real update forever (the sweep would still fix it, but only after a
// visible delay). rgnCovered accumulates windows ABOVE this one as the Z-ordered loop walks
// down, so at the call site it is exactly this window's occluders. Any overlap, or a
// Z-order we do not trust, and this returns FALSE = "assume changed".
//
// Returns TRUE only when it is SAFE and CERTAIN that nothing changed. Every uncertainty -
// no framebuffer, unknown Z-order, occlusion, off-screen, zero area - returns FALSE, so the
// worst case is the previous behaviour.
// Recaptures avoided by the screen-content compare are counted through PerfNotePwDecision()
// into the QGAPERF record's pwskip/pwcap pair. This used to be a file-static that nothing
// ever read, under a comment claiming it was "exposed so the effect is measurable" - it was
// not, and the acceptance criterion written against it could never pass.
static BOOL PwScreenUnchanged(IN OUT WINDOW_DATA* entry, IN const BYTE* fb, IN UINT pitch,
                              IN UINT fbWidth, IN UINT fbHeight, IN const RECT* rect,
                              IN HRGN rgnCoveredAbove)
{
    // Each refusal is counted by CAUSE. A bare 0 % hit rate is ambiguous between "a guard
    // always refuses" and "the content genuinely changes every time" - and those have opposite
    // conclusions: the first is a bug in this function, the second falsifies the premise that
    // Windows 11's extra presents are redundant. Measured 0 skips in 5557 decisions, so the
    // distinction has to be made from data, not argued.
    if (!fb || pitch == 0)
    {
        PerfNotePwRefusal(PW_REFUSE_NO_FB);
        return FALSE;
    }
    if (!g_ZOrderValid)
    {
        PerfNotePwRefusal(PW_REFUSE_NO_ZORDER);
        return FALSE;
    }

    // Fully on-screen only: a clipped rect would hash a different area each time the window
    // straddles an edge, which is a false "changed" at best and a false "unchanged" at worst.
    if (rect->left < 0 || rect->top < 0 ||
        rect->right > (LONG)fbWidth || rect->bottom > (LONG)fbHeight ||
        rect->right <= rect->left || rect->bottom <= rect->top)
    {
        PerfNotePwRefusal(PW_REFUSE_OFFSCREEN);
        return FALSE;
    }

    // Anything above this window makes the screen an invalid proxy for its content.
    if (RectInRegion(rgnCoveredAbove, rect))
    {
        PerfNotePwRefusal(PW_REFUSE_OCCLUDED);
        return FALSE;
    }

    // FNV-1a over the rows. Reading 4 bytes at a time keeps this a streaming scan; the
    // window is contiguous per row, so this is memcmp-class work.
    ULONGLONG h = 1469598103934665603ULL;
    const UINT bpp = 4;
    const UINT rowBytes = (UINT)(rect->right - rect->left) * bpp;
    for (LONG y = rect->top; y < rect->bottom; y++)
    {
        const BYTE* row = fb + (SIZE_T)y * pitch + (SIZE_T)rect->left * bpp;
        for (UINT i = 0; i < rowBytes; i += 4)
        {
            h ^= (ULONGLONG)(*(const UINT32*)(row + i));
            h *= 1099511628211ULL;
        }
    }

    if (entry->PwScreenHashValid && entry->PwScreenHash == h)
        return TRUE;                        // identical screen content: capture would be a no-op

    // Reached the hash and it differed: the window's screen pixels really did change. This is
    // the ONLY refusal that supports "the present was not redundant"; every other one above is
    // this function declining to look.
    PerfNotePwRefusal(entry->PwScreenHashValid ? PW_REFUSE_CONTENT_CHANGED : PW_REFUSE_FIRST_SEEN);
    entry->PwScreenHash = h;
    entry->PwScreenHashValid = TRUE;
    return FALSE;
}

static void PwPatchSynthChildren(IN OUT WINDOW_DATA* owner, IN const RECT* area)
{
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink; e != &g_WatchedWindowsList; e = e->Flink)
    {
        WINDOW_DATA* c = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (c->Synthesized && c->SynthOwner == owner->Handle)
            PwPatchSynthChildClipped(owner, c, area);
    }
}

// Drop the published desktop image. The pointer belongs to the mapped desktop surface of a
// duplication object; when that duplication is discarded the mapping goes with it, but
// synthesis reads g_FbBits from OUTSIDE the frame loop - SynthActivate() paints from the
// window-event thread - so nothing else would stop it dereferencing the stale pointer.
// Called by the capture layer under ctx->frame.lock whenever the surface is released.
// PwPatchSynthChildClipped already handles NULL by declining to paint; the child is repainted
// by the post-recovery sweep once a new frame has been published.
void PwInvalidateFramebuffer(void)
{
    g_FbBits = NULL;
    g_FbPitch = 0;
    g_FbWidth = 0;
    g_FbHeight = 0;
}

static ULONG ProcessNewFrame(IN const CAPTURE_FRAME* frame, IN const BYTE* framebuffer,
    IN UINT fbWidth, IN UINT fbHeight)
{
    // Publish the live desktop image for paths outside this loop (synthesis).
    if (framebuffer && frame->rect.Pitch > 0)
    {
        g_FbBits = framebuffer;
        g_FbPitch = frame->rect.Pitch;
        g_FbWidth = fbWidth;
        g_FbHeight = fbHeight;
    }

    // Menus/tooltips are override-redirect windows. They are mapped like any other window,
    // but dom0 screenshot tooling enumerates only managed windows, so whether their repaints
    // (e.g. hover highlight) actually reach the daemon cannot be checked from outside. Count
    // them here instead. Emitted only when non-zero, so it is silent unless a popup is
    // actually being damaged - a menu is open for a second or two at a time.
    // INFO, not DEBUG: LogDebug does not appear at the guest's default LogLevel=3, which is
    // the same trap that made the ACCESS_LOST recovery look like a no-op (see
    // instrumentation/ACCESS-LOST-VERIFIED.md). A diagnostic invisible at the level the
    // guest actually runs at is worse than none: it reads as a confirmed zero.
    ULONG perfPopupDamage = 0;
    HWND  perfPopupWindow = NULL;

    WINDOW_DATA *entry;
    ULONG status = ERROR_SUCCESS;

    // Damage clipping state. Regions are reused across the whole frame rather than created
    // per window, so this costs four GDI objects per frame regardless of window count.
    WINDOW_DATA* zSorted[64];
    UINT zCount = 0;
    HRGN rgnCovered = NULL, rgnWindow = NULL, rgnVisible = NULL, rgnDirty = NULL, rgnDamage = NULL;
    RGNDATA* rgnData = NULL;
    DWORD rgnDataSize = 0;

    // Instrumentation (see perf.h). All of these collapse to zero when disabled;
    // the only cost then is the g_PerfEnabled test inside PerfNow().
    LONGLONG perfFrameStart = PerfNow();
    LONGLONG perfSendBase = g_PerfSendTicks;
    LONG perfSendCountBase = g_PerfSendCount;
    LONGLONG perfPhase = 0, perfSendPhase = 0;
    LONGLONG perfUpdate = 0, perfEnum = 0, perfRemove = 0, perfDamage = 0;
    LONGLONG perfTrackedOutOfFrame = 0; // tracking ticks accumulated between frames
    UINT perfWindows = 0, perfInterrogated = 0, perfEvents = 0;
    TRACK_STATS track;

    LogVerbose("start");
    if (!g_SeamlessMode)
    {
        perfPhase = PerfNow();
        perfSendPhase = g_PerfSendTicks;

        // HELD FRAME: while an exact-obtain/settle is in flight the daemon's window
        // still has the pre-resize geometry (transitional dumps are suppressed);
        // damage computed against the transit geometry paints as sheared garbage
        // there (user-reported brief mangling). Send nothing - the daemon freezes
        // on the last clean frame, and the post-apply A6ACKREPAINT repaints
        // everything at the final size.
        if (!ResolutionShouldAnnounceGeometry(fbWidth, fbHeight))
        {
            LogVerbose("held frame (transitional %ux%u)", fbWidth, fbHeight);
            return ERROR_SUCCESS;
        }

        if (frame->dirty_rects_count == 0)
        {
            // normally we don't get frames with 0 dirty rects unless it's the 1st one
            // then refresh everything (at the size the dump was actually granted at,
            // which after an A6 in-place resize can differ from g_Screen*)
            LogDebug("no dirty rects, updating whole screen");
            SendWindowDamageEvent(NULL, 0, 0, fbWidth, fbHeight);
        }
        else
        {
            for (UINT i = 0; i < frame->dirty_rects_count; i++)
            {
                RECT rect = frame->dirty_rects[i];
                SendWindowDamageEvent(NULL, rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top);
            }
        }

        perfDamage = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
        PerfEmitFrame(FALSE, perfFrameStart, PerfNow() - perfFrameStart,
            0, 0, 0, perfDamage,
            g_PerfSendTicks - perfSendBase, g_PerfSendCount - perfSendCountBase,
            &frame->perf, frame->dirty_rects_count, 1, 0, 0);

        LogVerbose("end (fullscreen)");
        return ERROR_SUCCESS;
    }

    EnterCriticalSection(&g_csWatchedWindows);

    // Created here, not at the top: the fullscreen path returns before ever reaching the
    // damage loop and would leak these every frame.
    rgnCovered = CreateRectRgn(0, 0, 0, 0);
    rgnWindow  = CreateRectRgn(0, 0, 0, 0);
    rgnVisible = CreateRectRgn(0, 0, 0, 0);
    rgnDirty   = CreateRectRgn(0, 0, 0, 0);
    rgnDamage  = CreateRectRgn(0, 0, 0, 0);

    // Bring the watched window list up to date from the events the hook thread
    // collected (or resync it wholesale, periodically). This used to be an
    // EnumWindows() pass over every top-level window on every single frame.
    status = TrackWindows(&track);

    // Tracking done between frames belongs to this frame's accounting too, or the
    // work would simply vanish from the QGAPERF numbers.
    perfUpdate = track.UpdateTicks + g_TrackedUpdateTicks;
    perfEnum = track.EnumTicks + g_TrackedEnumTicks;
    perfRemove = track.RemoveTicks + g_TrackedRemoveTicks;
    perfInterrogated = track.Interrogated + g_TrackedInterrogated;
    perfEvents = track.Events + g_TrackedEvents;
    // ...which means `tot` must include it as well. `tot` is otherwise
    // (PerfNow() - perfFrameStart), i.e. in-frame only, so once most tracking moves
    // BETWEEN frames (which is the whole point of the hook) upd+enu would exceed tot and
    // analyze-perf.py would report shares >100% and a negative "unaccounted". The one
    // number Phase 2A is judged on has to balance.
    perfTrackedOutOfFrame = g_TrackedUpdateTicks + g_TrackedEnumTicks + g_TrackedRemoveTicks;
    g_TrackedUpdateTicks = g_TrackedEnumTicks = g_TrackedRemoveTicks = 0;
    g_TrackedInterrogated = g_TrackedEvents = 0;

    perfPhase = PerfNow();
    perfSendPhase = g_PerfSendTicks;

    if (status != ERROR_SUCCESS)
        goto cleanup;

    // send damage notifications, TOPMOST FIRST so each window can be clipped against the
    // area already claimed by the windows above it
    zCount = CollectZOrder(zSorted, RTL_NUMBER_OF(zSorted));
    for (UINT zi = 0; zi < zCount; zi++)
    {
        entry = zSorted[zi];
        perfWindows++;

        // A window that is minimized or hidden occludes nothing. Letting one contribute to
        // the covered region below would permanently suppress damage for whatever is beneath
        // it - a window going partially blank for no visible reason.
        if (entry->IsIconic || !entry->IsVisible)
            continue;

        // A window awaiting removal is not a window we may talk about. Materialization
        // ("owner geometry changed") clears Synthesized and sets DeletePending on an entry
        // that stays in this list until TrackWindows re-examines it, and CreateSent is
        // FALSE for one that was only ever composited - so without this skip the frame
        // loop treats it as an ordinary legacy window and sends CONFIGURE/damage for a
        // window dom0 has no CREATE for, which makes gui-daemon exit. It claims no area
        // either: it is on its way out, and suppressing damage beneath a window that is
        // about to vanish is the more visible error.
        if (entry->DeletePending)
            continue;

        // Synthesized windows have no dom0 window: never send anything for them. They
        // still claim their area so LEGACY windows below are clipped as before; their
        // pixels reach dom0 through the owner's buffer (PwPatchSynthChildren).
        if (entry->Synthesized)
        {
            SetRectRgn(rgnWindow, entry->X, entry->Y,
                entry->X + (int)entry->Width, entry->Y + (int)entry->Height);
            if (g_ZOrderValid)
                CombineRgn(rgnCovered, rgnCovered, rgnWindow, RGN_OR);
            continue;
        }

        // Windows with their own per-window buffer get content AND damage from the WGC
        // engine; the composited screen carries nothing for them, and slicing it is
        // exactly the artifact source this build removes. They still claim their area
        // (override-redirect only, same rule as below) so LEGACY-path windows beneath
        // them are clipped as before.
        if (PwIsAttached(entry))
        {
            RECT pwRect = { entry->X, entry->Y,
                            entry->X + (int)entry->Width, entry->Y + (int)entry->Height };
            RECT pwHit;
            if (entry->PwSliceFed)
            {
                // Agent-side slice: copy the changed region of the composited screen
                // into the window's own buffer. Content becomes window-relative, so
                // dom0 renders it correctly wherever it places the window - the
                // daemon-side legacy slice misregisters as soon as dom0 repositions
                // the window (force_on_screen on a fullscreen overlay, measured 31px).
                if (entry->PwSliceNeedsFull)
                {
                    entry->PwSliceNeedsFull = FALSE;
                    PwSliceCopyAndDamage(entry, frame, framebuffer, &pwRect);
                }
                else
                {
                    for (UINT pdi = 0; pdi < frame->dirty_rects_count; pdi++)
                        if (IntersectRect(&pwHit, &frame->dirty_rects[pdi], &pwRect))
                            PwSliceCopyAndDamage(entry, frame, framebuffer, &pwHit);
                }
            }
            else
            {
                // Screen dirty rects are the change TRIGGER for the per-window engine:
                // if anything on screen changed where this window is, ask the engine to
                // recapture it (content itself comes from PrintWindow, never the screen).
                //
                // EXCEPT while the window is MOVING. A dragged window dirties its whole
                // screen extent every frame, but a pure position change does not alter
                // the window's OWN content: the PrintWindow buffer is position-
                // invariant, dom0 repositions it from MSG_CONFIGURE alone (and repaints
                // exposures from its stored image), so the row-diff of such a recapture
                // is empty and nothing is sent. The recapture is pure waste - ~15-18 ms
                // of PrintWindow per frame on a WARP guest - and tracking
                // interrogations running concurrently with it stall for ~8 ms each,
                // which is the measured 17 ms/frame drag regression (instrumentation/
                // qwtfull-w10/bench-qwtfull-w10.md). The window counts as moving from
                // the frame its position changed until PW_MOVE_SETTLE_MS pass with no
                // further change - a single frame without movement is NOT the end of a
                // drag (~5% of drag frames apply no LOCATIONCHANGE). While moving, the
                // trigger is skipped and content refreshes at most once per
                // PW_MOVE_RECAPTURE_MS; when motion ends, one recapture fires
                // UNCONDITIONALLY, because the final repaint at the new position may
                // not intersect that frame's dirty rects. A same-frame RESIZE has
                // already rebuilt the channel (PwResizeWindow -> fresh channel starts
                // dirty + synchronous prefill + PwFrameXYValid reset), so nothing is
                // lost there. Slice-fed windows never reach this branch: their content
                // comes from the composited screen and IS position-dependent
                // (PwSliceNeedsFull on move stays required).
                DWORD pwNow = GetTickCount();
                if (entry->PwFrameXYValid &&
                    (entry->X != entry->PwFrameX || entry->Y != entry->PwFrameY))
                {
                    entry->PwLastMoveTick = pwNow;
                    entry->PwSettleDue = TRUE;
                }
                entry->PwFrameX = entry->X;
                entry->PwFrameY = entry->Y;
                entry->PwFrameXYValid = TRUE;

                if (entry->PwSettleDue &&
                    pwNow - entry->PwLastMoveTick < PW_MOVE_SETTLE_MS)
                {
                    // Moving. A stale PwLastMoveCapTick makes the throttled refresh
                    // fire on the FIRST moving frame, so one-shot programmatic moves
                    // still capture immediately, as before.
                    if (pwNow - entry->PwLastMoveCapTick >= PW_MOVE_RECAPTURE_MS)
                    {
                        entry->PwLastMoveCapTick = pwNow;
                        LogDebug("QGADRAG,ev=refresh,hwnd=0x%x",
                            (uint32_t)(ULONG_PTR)entry->Handle);
                        WcMarkDirty(entry->Handle);
                    }
                    else
                    {
                        LogDebug("QGADRAG,ev=suppress,hwnd=0x%x",
                            (uint32_t)(ULONG_PTR)entry->Handle);
                    }
                }
                else if (entry->PwSettleDue)
                {
                    // Quiet for PW_MOVE_SETTLE_MS: motion is over. Recapture once,
                    // regardless of where this frame's damage landed.
                    entry->PwSettleDue = FALSE;
                    LogDebug("QGADRAG,ev=settle,hwnd=0x%x",
                        (uint32_t)(ULONG_PTR)entry->Handle);
                    WcMarkDirty(entry->Handle);
                }
                else
                {
                    BOOL pwDamaged = FALSE;
                    for (UINT pdi = 0; pdi < frame->dirty_rects_count; pdi++)
                    {
                        if (IntersectRect(&pwHit, &frame->dirty_rects[pdi], &pwRect))
                        {
                            pwDamaged = TRUE;
                            break;
                        }
                    }
                    // The dirty rect says the screen was PRESENTED here, not that this
                    // window's content changed. Windows 11 re-presents ~1.9x more often than
                    // Windows 10 for identical input, and those extra recaptures are
                    // byte-identical - they cost a PrintWindow each and send nothing. Compare
                    // the screen bytes first; skip only when they are provably unchanged AND
                    // the window is unoccluded (see PwScreenUnchanged).
                    if (pwDamaged)
                    {
                        BOOL pwSkip = PwScreenUnchanged(entry, framebuffer, frame->rect.Pitch,
                                                        fbWidth, fbHeight, &pwRect, rgnCovered);
                        // Record BOTH outcomes: the claim this fix makes is a rate (captures
                        // avoided over captures considered), and skips alone cannot express
                        // one - they only grow with how long the workload ran.
                        PerfNotePwDecision(pwSkip);
                        if (!pwSkip)
                            WcMarkDirty(entry->Handle);
                    }
                }

                // Composited children live in the masked regions of this buffer and
                // are fed from the screen image (PrintWindow does not render owned
                // popups). Repaint whatever part of them changed this frame.
                if (entry->SynthChildCount > 0)
                {
                    for (UINT pdi = 0; pdi < frame->dirty_rects_count; pdi++)
                        if (IntersectRect(&pwHit, &frame->dirty_rects[pdi], &pwRect))
                            PwPatchSynthChildren(entry, &pwHit);

                    // A patch above (or the one at SynthActivate) can copy a child
                    // MID-DRAW; if no later dirty rect intersects it, the half-drawn
                    // pixels persist in dom0. Re-copy every child's FULL rect
                    // periodically, regardless of where this frame's damage landed.
                    DWORD synthNow = GetTickCount();
                    if (synthNow - entry->SynthLastFullPatch >= SYNTH_FULL_PATCH_MS)
                    {
                        entry->SynthLastFullPatch = synthNow;
                        PwPatchSynthChildren(entry, NULL);
                    }
                }
            }
            SetRectRgn(rgnWindow, entry->X, entry->Y,
                entry->X + (int)entry->Width, entry->Y + (int)entry->Height);
            if (g_ZOrderValid && entry->IsOverrideRedirect)
                CombineRgn(rgnCovered, rgnCovered, rgnWindow, RGN_OR);
            continue;
        }

        // INVARIANT: the origin used to convert damage to window-relative coordinates must
        // be the same origin most recently sent in MSG_CONFIGURE, because that is what the
        // gui-daemon adds back when it copies out of the shared framebuffer. TrackWindows()
        // above has just sent MSG_CONFIGURE with entry->X/Y, so entry->X/Y it must be.
        // Converting against the pre-tracking position instead was tried and is wrong: it
        // mis-registers every dragged window by exactly one frame of movement.
        RECT windowRect = { entry->X, entry->Y,
                            entry->X + (int)entry->Width, entry->Y + (int)entry->Height };
        RECT changedArea; // intersection of damage rect with window rect

        // Re-read the window's position immediately before registering damage against it.
        //
        // dom0 copies out of the LIVE shared framebuffer when it processes the message, not
        // out of a snapshot taken when the frame was captured. So the content it will read
        // sits at the window's CURRENT position, and registering damage against the position
        // TrackWindows() saw earlier in this frame mis-registers it by however far the window
        // has moved since - measured at p95=22px, max=38px during a drag, which is the
        // "contents wobble within the frame" the user reports.
        //
        // The origin must also stay equal to the one last announced in MSG_CONFIGURE, so when
        // the position has moved, announce the new one first and then convert against it.
        {
            // Only worth refreshing if this window is actually damaged this frame: the helper
            // does DwmGetWindowAttribute + GetMonitorInfo + EnumDisplaySettings, and paying it
            // for every watched window every frame is most of the drag regression.
            RECT probe = { entry->X, entry->Y,
                           entry->X + (int)entry->Width, entry->Y + (int)entry->Height };
            RECT hit;
            BOOL damagedNow = FALSE;
            for (UINT di = 0; di < frame->dirty_rects_count; di++)
            {
                if (IntersectRect(&hit, &frame->dirty_rects[di], &probe))
                {
                    damagedNow = TRUE;
                    break;
                }
            }

            RECT fresh;
            // Maximized windows do not drag; skip the refresh so the raw (unclamped)
            // DWM rect cannot leak out here and restart the dom0 CONFIGURE ping-pong.
            if (damagedNow && !(entry->Style & WS_MAXIMIZE) &&
                GetRealWindowRect(entry->Handle, &fresh) == ERROR_SUCCESS)
            {
                int freshW = fresh.right - fresh.left, freshH = fresh.bottom - fresh.top;
                if ((fresh.left != entry->X || fresh.top != entry->Y ||
                     freshW != (int)entry->Width || freshH != (int)entry->Height) &&
                    freshW > 0 && freshH > 0)
                {
                    entry->X = fresh.left;
                    entry->Y = fresh.top;
                    entry->Width = freshW;
                    entry->Height = freshH;
                    SendWindowConfigureIfChanged(entry);
                }
            }
        }

        windowRect.left = entry->X;
        windowRect.top = entry->Y;
        windowRect.right = entry->X + (int)entry->Width;
        windowRect.bottom = entry->Y + (int)entry->Height;

        // Region of this window NOT covered by anything stacked above it. The framebuffer is
        // the composited desktop, so the pixels under a higher window belong to that window,
        // not to this one; sending them here makes the daemon paint the upper window's content
        // into this window's pixmap. That is what corrupts a menu's host window on hover and
        // what leaves debris when one window is dragged across another.
        SetRectRgn(rgnWindow, windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
        CombineRgn(rgnVisible, rgnWindow, rgnCovered, RGN_DIFF);

        // skip windows that aren't in the changed area
        for (UINT i = 0; i < frame->dirty_rects_count; i++)
        {
            if (IntersectRect(&changedArea, &frame->dirty_rects[i], &windowRect))
            {
                LogVerbose("damage for 0x%x: window (%d,%d) %dx%d, damage (%d,%d) %dx%d, intersect (%d,%d) %dx%d",
                    entry->Handle, entry->X, entry->Y, entry->Width, entry->Height,
                    frame->dirty_rects[i].left, frame->dirty_rects[i].top,
                    frame->dirty_rects[i].right - frame->dirty_rects[i].left, frame->dirty_rects[i].bottom - frame->dirty_rects[i].top,
                    changedArea.left, changedArea.top, changedArea.right - changedArea.left, changedArea.bottom - changedArea.top);

                SetRectRgn(rgnDirty, changedArea.left, changedArea.top,
                    changedArea.right, changedArea.bottom);
                if (CombineRgn(rgnDamage, rgnVisible, rgnDirty, RGN_AND) == NULLREGION)
                    continue; // entirely hidden behind a higher window

                // The visible part can be several disjoint rectangles (an L shape when a
                // window is partly overlapped), so send each rather than their bounding box -
                // the bounding box would re-introduce the covered area.
                DWORD needed = GetRegionData(rgnDamage, 0, NULL);
                if (needed == 0)
                    continue;
                if (needed > rgnDataSize)
                {
                    RGNDATA* grown = (RGNDATA*)realloc(rgnData, needed);
                    if (!grown)
                        continue; // out of memory: drop this rect rather than send it wrong
                    rgnData = grown;
                    rgnDataSize = needed;
                }
                if (GetRegionData(rgnDamage, rgnDataSize, rgnData) == 0)
                    continue;

                const RECT* parts = (const RECT*)rgnData->Buffer;
                for (DWORD p = 0; p < rgnData->rdh.nCount; p++)
                {
                    if (entry->IsOverrideRedirect)
                    {
                        perfPopupDamage++;
                        perfPopupWindow = entry->Handle;
                    }

                    status = SendWindowDamageEvent(entry->Handle,
                        parts[p].left - entry->X, // window-relative, same origin as MSG_CONFIGURE
                        parts[p].top - entry->Y,
                        parts[p].right - parts[p].left, // size
                        parts[p].bottom - parts[p].top);

                    if (ERROR_SUCCESS != status)
                    {
                        win_perror2(status, "SendWindowDamageEvent");
                        goto cleanup;
                    }
                }
            }
        }

        // Claim this window's area so lower windows do not receive its pixels - but ONLY for
        // override-redirect popups (menus, tooltips).
        //
        // Clipping against general z-order was tried and is wrong. The agent never tells the
        // daemon about stacking, so dom0's order and the guest's routinely disagree; when they
        // do, the region withheld from a window is exactly the region dom0 has on top, and it
        // renders as a stale band. Measured directly: with a Notepad focused above chromerepro
        // in the guest while dom0 drew chromerepro on top, chromerepro showed a stale vertical
        // band, which disappeared the instant the guest's stacking was made to agree.
        //
        // Popups are the safe case and the one that actually matters: a menu is on top in both
        // the guest and in dom0 by construction, so withholding its area from the window below
        // can never expose a stale region - and that bleed is what corrupts a menu's host
        // window on hover.
        //
        // Fixing the general case needs the daemon to learn z-order: a protocol change,
        // Phase 3, not something to smuggle in here.
        if (g_ZOrderValid && entry->IsOverrideRedirect)
            CombineRgn(rgnCovered, rgnCovered, rgnWindow, RGN_OR);
    }

cleanup:
    perfDamage = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
    LeaveCriticalSection(&g_csWatchedWindows);

    // tot includes the tracking that happened between frames (see perfTrackedOutOfFrame),
    // so that upd+enu+rem+dmg+snd stays <= tot and the shares remain meaningful.
    PerfEmitFrame(TRUE, perfFrameStart, PerfNow() - perfFrameStart + perfTrackedOutOfFrame,
        perfUpdate, perfEnum, perfRemove, perfDamage,
        g_PerfSendTicks - perfSendBase, g_PerfSendCount - perfSendCountBase,
        &frame->perf, frame->dirty_rects_count, perfWindows, perfInterrogated, perfEvents);

    // Only the FIRST damage frame for a given popup is logged. A menu is damaged on every
    // hover repaint, so logging each frame emits ~40 lines/second at capture rate for as long
    // as the menu is open - unacceptable noise in a package meant to be installed as-is. One
    // line per popup still answers the question this counter exists for (do override-redirect
    // windows receive damage at all), and the message says so rather than implying a total.
    if (rgnCovered) DeleteObject(rgnCovered);
    if (rgnWindow)  DeleteObject(rgnWindow);
    if (rgnVisible) DeleteObject(rgnVisible);
    if (rgnDirty)   DeleteObject(rgnDirty);
    if (rgnDamage)  DeleteObject(rgnDamage);
    free(rgnData);

    if (perfPopupDamage > 0 && perfPopupWindow != g_LastPopupDamageWindow)
    {
        g_LastPopupDamageWindow = perfPopupWindow;
        LogInfo("popup damage: override-redirect window 0x%x is receiving damage (%u message(s) this frame; logged once per popup)",
            perfPopupWindow, perfPopupDamage);
    }
    else if (perfPopupDamage == 0 && perfPopupWindow == NULL)
    {
        // no popup damaged this frame; allow the next popup to log again
        g_LastPopupDamageWindow = NULL;
    }

    LogVerbose("end (%x)", status);
    return status;
}

ULONG StartFrameProcessing(IN HANDLE newFrameEvent, IN HANDLE captureErrorEvent, OUT CAPTURE_CONTEXT** capture)
{
    LogVerbose("start");
    AttachToInputDesktop();
    // The hook thread has its own desktop association, it must follow.
    RearmWindowEvents();
    // Initialize capture interfaces, this also initializes framebuffer PFNs
    *capture = CaptureInitialize(newFrameEvent, captureErrorEvent);
    if (!(*capture))
        return win_perror("CaptureInitialize");

    ULONG status;
    // send whole screen window, needed even in seamless mode.
    // NEVEREXIT: exactly once per announced generation - a degraded-state retry
    // re-runs this function after a partial failure, and a duplicate CREATE(0) makes
    // gui-daemon exit(1) (see g_ScreenAnnounced).
    if (!g_ScreenAnnounced)
    {
        status = SendWindowCreate(NULL);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "SendWindowCreate(NULL)");
        g_ScreenAnnounced = TRUE;
    }

    g_LocalScreenDestroyed = FALSE;

    // send the whole screen framebuffer map
    // ctx->width/height are written once in CaptureInitialize, before the capture thread
    // starts, so reading them here is as safe as the grant_refs read below. The page count
    // MUST come from CaptureGrantPageCount, not g_ScreenWidth/Height: it is the size
    // grant_refs was actually allocated for - the exact per-geometry count on the
    // direct-map path, the CONSTANT staging capacity under StagingGrant (the daemon
    // accepts a larger-than-needed count; only a too-small one is exit(1), see capture.h).
    status = SendScreenGrants(CaptureGrantPageCount(*capture), (*capture)->grant_refs,
        (*capture)->width, (*capture)->height);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendScreenGrants");

    // this (re)initializes watched windows list
    status = SetSeamlessMode(g_SeamlessMode, TRUE);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SetSeamlessMode");

    status = CaptureStart(*capture);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "CaptureStart");

    LogVerbose("end");
    return ERROR_SUCCESS;
}

// During a display mode switch or monitor replug DXGI can refuse duplication for several
// seconds (observed: 0x887A0026 "keyed mutex abandoned" formatting of ACCESS_LOST,
// 0x887A0005 device suspended). Treating that as fatal turned every topology change into an
// agent crash + watchdog respawn, which then re-applied a stale cached resolution
// (FINDINGS 2026-08-05). Retry the known-transient errors briefly in-line here; when this
// still fails (or the error is not one of the transients) the CALLERS no longer exit -
// they enter the A7 degraded state and keep retrying at A7_DEGRADED_RETRY_MS (NEVEREXIT).
static ULONG StartFrameProcessingWithRetry(IN HANDLE newFrameEvent, IN HANDLE captureErrorEvent, OUT CAPTURE_CONTEXT** capture)
{
    ULONG status = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 10; attempt++)
    {
        if (attempt > 0)
            Sleep(750);
        status = StartFrameProcessing(newFrameEvent, captureErrorEvent, capture);
        if (ERROR_SUCCESS == status)
        {
            if (attempt > 0)
                LogInfo("A7RETRY capture initialized after %d retries", attempt);
            return ERROR_SUCCESS;
        }
        if (status != 0x887A0026 && status != 0x887A0005)
            break; // not a known-transient DXGI failure
        LogWarning("A7RETRY transient capture init failure 0x%x, attempt %d", status, attempt + 1);
    }
    return status;
}

// CaptureTeardown() must be called separately after gui daemon confirms screen destruction
ULONG StopFrameProcessing(IN OUT CAPTURE_CONTEXT** capture)
{
    LogVerbose("start");
    // NEVEREXIT hardening: also check *capture - the old check only caught a NULL
    // pointer-to-pointer (which no caller ever passes), while CaptureStop(*capture)
    // dereferences the contained pointer, which IS legitimately NULL for long
    // stretches in the A7 degraded state. All call sites guard today; this makes a
    // future unguarded call a no-op instead of a crash (= an exit).
    if (!capture || !*capture)
        return ERROR_SUCCESS;

    CaptureStop(*capture);

    ULONG status = SendWindowUnmap(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowUnmap(screen)");

    status = SendWindowDestroy(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowDestroy(screen)");

    // the daemon forgets window 0 on DESTROY; the next start must re-CREATE it
    g_ScreenAnnounced = FALSE;

    // pause replying to gui daemon's messages for the destroyed screen window
    g_LocalScreenDestroyed = TRUE;

    LogVerbose("end");
    return ERROR_SUCCESS;
}

// A6 exit-path bounds (design 2.2). The drain budget caps how long exit waits for
// dom0 to release its grant mappings; the ring headroom is the minimum free space in
// the write ring below which the teardown notifications are skipped outright -
// VchanSendBuffer blocks forever on a full ring, and a wedged daemon must not be able
// to stall shutdown. The teardown traffic is tiny (two header-only messages per
// window), so 16 KiB of a 64 KiB ring covers hundreds of windows with margin.
#define A6_EXIT_SETTLE_MS 1000
#define A6_EXIT_RING_HEADROOM 16384

// A7/NEVEREXIT degraded no-capture state: when capture init keeps failing after the
// fast in-line retries, the agent does NOT exit (every needless exit risks killing
// gui-daemon via the dom0 EOF-on-write bug). It keeps servicing the vchan - input,
// configure, per-window capture if alive - and re-attempts capture init at this
// cadence, logging at most once per A7_DEGRADED_LOG_MS so soaks can count episodes
// without flooding.
#define A7_DEGRADED_RETRY_MS 5000
#define A7_DEGRADED_LOG_MS   60000

// Enter (or re-arm) the degraded state after a StartFrameProcessing failure.
// A partial failure can leave a capture context that never went live - tear it down
// to a known state first. CaptureTeardown handles half-built contexts (every field
// is guarded); under the default staging grant the screen grant persists by design,
// and on the direct-map path a still-mapped grant is parked/leaked loudly by the A6
// sweep rather than freed under dom0.
static void EnterCaptureDegraded(IN ULONG failStatus, IN OUT CAPTURE_CONTEXT** capture,
    IN OUT BOOL* degraded, IN OUT ULONGLONG* retryDue, IN OUT ULONGLONG* logLast)
{
    if (*capture)
    {
        CaptureTeardown(*capture);
        *capture = NULL;
    }

    *retryDue = GetTickCount64() + A7_DEGRADED_RETRY_MS;
    if (!*degraded)
    {
        *degraded = TRUE;
        *logLast = GetTickCount64();
        LogError("A7DEGRADED entering degraded state: capture init failed (0x%x); "
            "vchan stays serviced, retrying capture init every %u ms",
            failStatus, A7_DEGRADED_RETRY_MS);
    }
}

// main event loop
// TODO: refactor into smaller parts
static ULONG WINAPI WatchForEvents(void)
{
    ULONG eventCount;
    DWORD signaledEvent;
    BOOL vchanIoInProgress;
    ULONG status;
    BOOL exitLoop;
    HANDLE watchedEvents[MAXIMUM_WAIT_OBJECTS];
#ifdef DEBUG_DUMP_WINDOWS
    DWORD dumpLastTime = GetTickCount();
    //UINT64 damageCount = 0, damageCountOld = 0;
#endif

    LogDebug("start");
    HANDLE newFrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    HANDLE captureErrorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    // KEEP-FATAL: no vchan could even be set up, so no daemon is connected - exiting
    // is harmless (nothing to EOF at) and the service respawn retries with backoff.
    if (!VchanInit(g_GuiDomainId, 6000))
    {
        LogError("VchanInit() failed");
        return GetLastError();
    }

    // KEEP-FATAL (both): startup resource failure before any daemon connection;
    // there is no degraded state to fall back to and nothing connected to protect.
    HANDLE fullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!fullScreenOnEvent)
        return GetLastError();
    HANDLE fullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!fullScreenOffEvent)
        return GetLastError();

    g_VchanClientConnected = FALSE;
    vchanIoInProgress = FALSE;
    exitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize(g_Vchan));
    watchedEvents[0] = g_ShutdownEvent;
    watchedEvents[1] = newFrameEvent;
    watchedEvents[2] = fullScreenOnEvent;
    watchedEvents[3] = fullScreenOffEvent;
    watchedEvents[4] = libvchan_fd_for_select(g_Vchan);
    watchedEvents[5] = captureErrorEvent;
    // Last, so that WaitForMultipleObjects prefers frames and vchan traffic to it.
    watchedEvents[6] = g_WindowEventSignal;
    eventCount = 7;

    CAPTURE_CONTEXT* capture = NULL;

    // A7/NEVEREXIT degraded no-capture state (see EnterCaptureDegraded)
    BOOL captureDegraded = FALSE;
    ULONGLONG captureRetryDue = 0;
    ULONGLONG degradedLogLast = 0;

    while (TRUE)
    {
        status = ERROR_SUCCESS;

        vchanIoInProgress = TRUE;

        // A7: while degraded, wake in time for the next capture-init retry; the
        // vchan/input events below are still serviced normally in between.
        DWORD waitTimeout = INFINITE;
        if (captureDegraded)
        {
            ULONGLONG now64 = GetTickCount64();
            waitTimeout = (captureRetryDue > now64) ? (DWORD)(captureRetryDue - now64) : 0;
        }

        // Wait for events.
        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, waitTimeout);
        if (signaledEvent != WAIT_TIMEOUT && signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            // KEEP-FATAL: the wait itself failing means our own handle table is
            // broken - the loop cannot service the vchan at all anymore, and a
            // retry would spin at 100% CPU. Not convertible to a degraded state.
            status = win_perror("WaitForMultipleObjects");
            break;
        }

#ifdef DEBUG_DUMP_WINDOWS
        if (g_SeamlessMode)
        {
            // dump watched windows every second
            // (skip entirely if the output would be discarded: DumpWindows() does
            // OpenProcess+QueryFullProcessImageName per window and takes both the
            // watched-windows and the logger lock, once a second, at any log level)
            if (LogGetLevel() >= LOG_LEVEL_DEBUG && GetTickCount() - dumpLastTime > 1000)
            {
                DumpWindows();
                dumpLastTime = GetTickCount();

                // XXX dump performance counters
                //LogDebug("last second damages: %llu", damageCount - damageCountOld);
                //damageCountOld = damageCount;
            }
        }
#endif

        if (0 == signaledEvent)
        {
            // KEEP-FATAL: QGA_SHUTDOWN - the explicit stop request, case (b).
            LogDebug("Shutdown event signaled");
            exitLoop = TRUE;
            break;
        }

        switch (signaledEvent)
        {
        case 1: // new frame available
            LogVerbose("new frame");
            // NEVEREXIT: capture can legitimately be NULL here - in the A7 degraded
            // state a stale frame event set by the torn-down capture generation can
            // still be signaled once; dereferencing it was a crash (= an exit).
            if (g_VchanClientConnected && capture)
            {
                // The duplication was recreated in place after a loss and the framebuffer
                // was re-granted, so the daemon is still mapping the old pages. Republish
                // the refs BEFORE sending any damage for this frame: damage that refers to
                // pages the daemon has not mapped yet would be painted from stale memory.
                if (capture->grants_changed &&
                    !ResolutionShouldAnnounceGeometry(capture->width, capture->height))
                {
                    // The daemon adopts window-0 size from the DUMP geometry too, not
                    // just from MSG_CONFIGURE - a transitional re-dump resized the
                    // daemon's window to the mid-replug default and it echoed that
                    // back (measured, first scripted resize 2026-08-05). Skip the
                    // whole transitional republish: with staging the refs are
                    // unchanged, frames during the blink are junk anyway, and the
                    // final target geometry sets grants_changed again itself.
                    LogInfo("A6DUMP suppressed (transitional %ux%u during exact-obtain/settle)",
                        capture->width, capture->height);
                    ResolutionNoteTransitSize(capture->width, capture->height);
                }
                else if (capture->grants_changed)
                {
                    capture->grants_changed = FALSE;
                    // STAGING: same refs and constant page count every time - this
                    // re-dump is a pure header (geometry) refresh, no grant traffic.
                    ULONG grantStatus = SendScreenGrants(
                        CaptureGrantPageCount(capture),
                        capture->grant_refs, capture->width, capture->height);
                    if (grantStatus != ERROR_SUCCESS)
                        win_perror2(grantStatus, "SendScreenGrants (after recreate)");
                    else
                    {
                        LogInfo("framebuffer re-granted after duplication recovery, MSG_WINDOW_DUMP re-sent");
                        {
                            // M0BLINK: the dump is on the wire. Everything after this
                            // point until repaint-first is local send cost only.
                            LONG64 m0 = ResolutionM0BlinkObtainStart();
                            if (m0 != 0)
                            {
                                ULONGLONG now = GetTickCount64();
                                LogInfo("M0BLINK dump-sent t=%I64u sinceobtain=%I64u ms",
                                    now, now - (ULONGLONG)m0);
                            }
                        }
                    // externally-driven mode changes (not via SetVideoMode) also
                    // reload the cursor scheme - re-blank here too
                    HideCursors();

                        // A6: the re-grant may carry a new geometry (in-place resize).
                        // Follow the dump with MSG_CONFIGURE for window 0 at the granted
                        // size: the daemon adopts whatever the agent reports for window 0
                        // (it is exempt from configure flow control, xside.c:2063-2069),
                        // and for a same-size recovery the echo is a no-op.
                        // EXCEPT while an exact-obtain is in flight: the desktop transits
                        // through intermediate real modes during the replug, and telling
                        // the daemon about them makes its window twitch and echo the
                        // transit size back as fake dom0 intent (measured revert,
                        // FINDINGS 2026-08-05). The final apply sends the real one.
                        if (!ResolutionShouldAnnounceGeometry(capture->width, capture->height))
                        {
                            LogInfo("A6CONFIGURE suppressed (transitional %ux%u during exact-obtain/settle)",
                                capture->width, capture->height);
                            ResolutionNoteTransitSize(capture->width, capture->height);
                        }
                        else
                        {
                            // Echo dom0's own last-reported window position - never
                            // (0,0), which would MOVE the window and clip its left
                            // border (see HandleConfigure).
                            ULONG cfgStatus = SendWindowConfigure(NULL,
                                g_ScreenWinX, g_ScreenWinY,
                                capture->width, capture->height, FALSE);
                            if (cfgStatus != ERROR_SUCCESS)
                                win_perror2(cfgStatus, "SendWindowConfigure (screen, after recreate)");
                            else
                                LogInfo("A6CONFIGURE window 0 -> %ux%u", capture->width, capture->height);
                        }
                    }

                    // Everything the daemon holds came from the old framebuffer, so repaint
                    // all of it rather than waiting for each window to happen to be damaged
                    // again - an idle window would otherwise stay frozen indefinitely.
                    if (g_SeamlessMode)
                    {
                        EnterCriticalSection(&g_csWatchedWindows);
                        // Advance from the LIST_ENTRY, not from the converted record: the
                        // `continue` below skips the tail of the loop body, so an advance
                        // placed there was skipped for every synthesized window - leaving
                        // `repaint` pointing at the already-converted WINDOW_DATA and
                        // re-applying CONTAINING_RECORD to it on the next pass, which walks
                        // the pointer backwards by offsetof(WINDOW_DATA, ListEntry) (~1 KB)
                        // into unrelated heap and loops there reading garbage, on the main
                        // event-loop thread, holding this critical section.
                        for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
                             e != &g_WatchedWindowsList;
                             e = e->Flink)
                        {
                            WINDOW_DATA* repaint = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
                            if (repaint->Synthesized)
                                continue; // painted into its owner; it has no buffer of its own
                            if (repaint->IsVisible && !repaint->IsIconic &&
                                repaint->Width > 0 && repaint->Height > 0)
                            {
                                SendWindowDamageEvent(repaint->Handle, 0, 0,
                                    repaint->Width, repaint->Height);
                            }
                        }
                        LeaveCriticalSection(&g_csWatchedWindows);
                    }
                    else
                    {
                        // Repaint at the size the new dump was granted at, not g_Screen*
                        // (which can lag an A6 in-place resize).
                        SendWindowDamageEvent(NULL, 0, 0, capture->width, capture->height);
                    }

                    // M0BLINK: THIS is when the user gets pixels back after a novel-size
                    // resize - not A6ACKREPAINT. The daemon processes the vchan strictly
                    // in order (xside.c handle_message) and handle_window_dump releases
                    // the old mapping before mapping the new refs, so damage queued after
                    // the dump above can only be painted against the new mapping. The
                    // ack-gated repaint that follows is a belt-and-braces second pass one
                    // round trip later; timing the blink at it overstates the blink.
                    {
                        LONG64 m0 = InterlockedExchange64(&g_M0BlinkFirstPaintStart, 0);
                        if (m0 != 0)
                        {
                            ULONGLONG now = GetTickCount64();
                            LogInfo("M0BLINK repaint-first t=%I64u sinceobtain=%I64u ms",
                                now, now - (ULONGLONG)m0);
                        }
                    }
                }

                ProcessNewFrame(&capture->frame, (const BYTE*)capture->framebuffer,
                    capture->width, capture->height);

                // Work-area drift check (frame path; ProcessWindowEvents has the
                // matching call). Here, not inside ProcessNewFrame: no locks are held
                // and the check stays out of the frame's perf accounting. Together the
                // two call sites make the check fire as long as frames OR window events
                // flow, whichever drains the tracking tick - including after hook-thread
                // death, when only frames arrive.
                WorkAreaEnsureApplied();
            }

            if (capture)
                SetEvent(capture->ready_event); // frame processed
            break;

        case 2:
            LogVerbose("fullscreen on");
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                // NEVEREXIT (CONVERT, was exitLoop): a failed mode switch leaves us in
                // the previous mode - degraded, not dead. If the cause was a vchan
                // send failure the receive path detects the dead vchan fatally (a).
                win_perror2(status, "SetSeamlessMode(FALSE)");
                LogWarning("NEVEREXIT seamless-off switch failed (0x%x) - staying in current mode", status);
            }
            break;

        case 3:
            LogVerbose("fullscreen off");
            status = SetSeamlessMode(TRUE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                // NEVEREXIT (CONVERT, was exitLoop): see case 2.
                win_perror2(status, "SetSeamlessMode(TRUE)");
                LogWarning("NEVEREXIT seamless-on switch failed (0x%x) - staying in current mode", status);
            }
            break;

        case 4: // vchan receive
            if (!g_VchanClientConnected)
            {
                vchanIoInProgress = FALSE;
                libvchan_cleanup(g_Vchan); // needed to cleanup xenstore entry

                LogInfo("A vchan client has connected");

                // needs to be set before enumerating windows so maps get sent
                // (and before sending anything really)
                g_VchanClientConnected = TRUE;

                // This daemon knows about no windows yet; forget what the previous one
                // was told before any per-window message can be gated against it.
                SendResetCreatedWindows();

                // KEEP-FATAL: SendProtocolVersion only fails when the vchan write
                // fails (daemon dead/EOF) - case (a). If the vchan somehow reports
                // open, the handshake stream position is unknown and no recovery is
                // protocol-safe; log the distinction loudly.
                if (ERROR_SUCCESS != SendProtocolVersion())
                {
                    LogError("SendProtocolVersion failed (vchan open=%d) - "
                        "handshake cannot proceed, exiting", libvchan_is_open(g_Vchan));
                    exitLoop = TRUE;
                    break;
                }

                // KEEP-FATAL: HandleVersion only fails on a vchan receive failure
                // (dead/EOF) - case (a). Continuing without the daemon's version
                // would also leave the version-gated paths (per-window) undefined.
                if (ERROR_SUCCESS != HandleVersion())
                {
                    LogError("HandleVersion failed (vchan open=%d) - "
                        "handshake cannot proceed, exiting", libvchan_is_open(g_Vchan));
                    exitLoop = TRUE;
                    break;
                }

                // This will probably change the current video mode if we don't have one saved in the registry.
                // KEEP-FATAL here, but only the vchan receive failure can still reach
                // this (case (a), msg_xconf possibly part-consumed): a SetVideoMode
                // failure no longer propagates - HandleXconf falls back to the current
                // resolution and returns success (NEVEREXIT conversion there).
                if (ERROR_SUCCESS != HandleXconf())
                {
                    LogError("HandleXconf failed (vchan open=%d) - exiting", libvchan_is_open(g_Vchan));
                    exitLoop = TRUE;
                    break;
                }

                // Screen geometry is final here; sync the guest work area to dom0's
                // usable workspace (config/qubesdb/inference - see workarea.h).
                WorkAreaInit();
                WorkAreaApply();

                // M6: arm the IDD's mode list with the computed set (target +
                // maximize/tile-half) once per process, before capture starts, so
                // the habitual sizes later switch with replug=0. Non-fatal.
                ResolutionPublishBootModeSet();

                status = StartFrameProcessingWithRetry(newFrameEvent, captureErrorEvent, &capture);
                if (ERROR_SUCCESS != status)
                {
                    // NEVEREXIT (CONVERT, was exitLoop): capture down is a degraded
                    // state, not a fatal one - the vchan is alive (we just finished
                    // the handshake on it), and exiting needlessly risks killing
                    // gui-daemon via the dom0 EOF bug. Keep servicing the vchan and
                    // retry capture init periodically.
                    win_perror2(status, "StartFrameProcessing");
                    EnterCaptureDegraded(status, &capture,
                        &captureDegraded, &captureRetryDue, &degradedLogLast);
                }

                break;
            }

            EnterCriticalSection(&g_VchanCriticalSection);
            LogVerbose("vchan receive, %d bytes", VchanGetReadBufferSize(g_Vchan));

            vchanIoInProgress = FALSE;

            if (!libvchan_is_open(g_Vchan))
            {
                // KEEP-FATAL: the vchan is genuinely dead / the daemon disconnected -
                // case (a). Exit is harmless here and the service respawn handles it.
                LogError("vchan disconnected");
                exitLoop = TRUE;
                LeaveCriticalSection(&g_VchanCriticalSection);
                break;
            }

            BOOL screenDestroyed = FALSE;
            while (VchanGetReadBufferSize(g_Vchan) > 0)
            {
                status = HandleServerData(!g_LocalScreenDestroyed, capture, &screenDestroyed);
                if (ERROR_SUCCESS != status)
                {
                    // KEEP-FATAL: after the NEVEREXIT conversions in vchan-handlers.c
                    // every failure that still propagates here is a vchan-level
                    // receive/send failure - the stream is broken or a message body
                    // was left partially consumed. Re-parsing a desynced stream could
                    // interpret arbitrary bytes as messages (including synthesized
                    // input), so this must never be converted. Case (a).
                    exitLoop = TRUE;
                    LogError("HandleServerData failed: 0x%x", status);
                    break;
                }
            }
            LeaveCriticalSection(&g_VchanCriticalSection);

            if (screenDestroyed)
            {
                LogDebug("gui daemon confirms screen destruction");
                // NEVEREXIT: capture is NULL if this confirm arrives while already in
                // the A7 degraded state (CaptureTeardown would crash on NULL).
                if (capture)
                {
                    CaptureTeardown(capture);
                    capture = NULL;
                }
                status = StartFrameProcessingWithRetry(newFrameEvent, captureErrorEvent, &capture);
                if (ERROR_SUCCESS != status)
                {
                    // NEVEREXIT (CONVERT, was exitLoop): same as the connect-time
                    // site - degrade and retry instead of exiting.
                    win_perror2(status, "StartFrameProcessing");
                    EnterCaptureDegraded(status, &capture,
                        &captureDegraded, &captureRetryDue, &degradedLogLast);
                }
                else if (captureDegraded)
                {
                    captureDegraded = FALSE;
                    LogInfo("A7DEGRADED recovered: capture restarted");
                }
            }
            break;

        case 5: // capture error, can be due to a desktop switch or resolution change
            LogDebug("capture error");

            // NEVEREXIT: a stale error event from a torn-down capture generation can
            // fire while degraded (capture == NULL); StopFrameProcessing dereferences
            // *capture. Nothing to stop in that case.
            if (capture)
                StopFrameProcessing(&capture);
            // CaptureTeardown() is delayed until we receive confirming MSG_DESTROY for 0x0 from gui daemon
            // revoking framebuffer access before that is unsafe
            break;

        case 6: // window events collected by the hook thread
            // Applying them here instead of only when the next frame arrives is what
            // gets window moves to the gui daemon at input rate.
            // Nothing is sent while the screen window is destroyed (between a capture
            // error and the restart that follows it); the discarded events are covered
            // by the resync that ResetWatch/DiscardWindowEvents leave pending.
            if (g_VchanClientConnected && g_SeamlessMode && !g_LocalScreenDestroyed)
                ProcessWindowEvents();
            else
                DiscardWindowEvents();
            break;
        }

        // A7/NEVEREXIT: degraded-state capture re-init. Runs on the wait timeout armed
        // above, but also opportunistically after any other event once the retry is
        // due. Single attempt per period (not the 10x fast retry - that would stall
        // vchan servicing for seconds at a time while capture is persistently down).
        if (captureDegraded && !exitLoop && g_VchanClientConnected &&
            GetTickCount64() >= captureRetryDue)
        {
            ULONG retryStatus = StartFrameProcessing(newFrameEvent, captureErrorEvent, &capture);
            if (ERROR_SUCCESS == retryStatus)
            {
                captureDegraded = FALSE;
                LogInfo("A7DEGRADED recovered: capture restarted");
            }
            else
            {
                if (capture)
                {
                    // partial init - back to a known state (see EnterCaptureDegraded)
                    CaptureTeardown(capture);
                    capture = NULL;
                }
                captureRetryDue = GetTickCount64() + A7_DEGRADED_RETRY_MS;
                if (GetTickCount64() - degradedLogLast >= A7_DEGRADED_LOG_MS)
                {
                    degradedLogLast = GetTickCount64();
                    LogWarning("A7DEGRADED capture unavailable, retrying");
                }
            }
        }

        if (exitLoop)
            break;
    }

    LogDebug("main loop finished");

    // --- A6 (approved design, 2.2): bounded, leak-free exit. Previously the exit order
    // was PwShutdown -> libvchan_close -> StopFrameProcessing -> CaptureTeardown: no
    // per-window teardown ever ran (every attached buffer leaked its grant silently) and
    // the screen UNMAP/DESTROY went out after g_VchanClientConnected dropped, i.e. was
    // silently discarded. New order: notify dom0 of everything while the vchan is still
    // open, drain the queued grant revocations with a hard time budget, then close.
    // A dead daemon must not stall exit: VchanSendBuffer blocks FOREVER on a full ring,
    // so when the ring lacks headroom the client is declared gone (every send below is
    // already gated on g_VchanClientConnected) and the exit proceeds notification-less.

    // Join the WGC capture thread first: its damage callback sends on the vchan (so the
    // close below would be a use-after-free with it alive), and stopping it makes this
    // thread the only vchan writer - the ring headroom checked once cannot shrink under
    // the sends that follow. PwShutdown below still runs the rest of the per-window
    // shutdown (WcShutdown is idempotent).
    WcShutdown();

    BOOL closeVchan = g_VchanClientConnected;
    if (g_VchanClientConnected &&
        (!libvchan_is_open(g_Vchan) || VchanGetWriteBufferSize(g_Vchan) < A6_EXIT_RING_HEADROOM))
    {
        LogWarning("A6EXIT vchan dead or ring full (open=%d, space=%d) - exiting without teardown notifications",
            libvchan_is_open(g_Vchan), VchanGetWriteBufferSize(g_Vchan));
        EnterCriticalSection(&g_VchanCriticalSection);
        g_VchanClientConnected = FALSE;
        LeaveCriticalSection(&g_VchanCriticalSection);
    }

    // Detach-all, at last: RemoveWindow detaches the per-window buffer (queueing its
    // grant revocation) and sends MSG_UNMAP/MSG_DESTROY for every tracked window;
    // ResetWatch(FALSE) is the existing machinery for exactly that sweep. Then the
    // screen window gets its UNMAP/DESTROY too, still on the open vchan. Both paths
    // no-op their sends if the client was declared gone above.
    ResetWatch(FALSE);
    if (capture)
        StopFrameProcessing(&capture);

    // SETTLE, then a SINGLE revoke pass - never a retry loop. A revoke racing
    // dom0's unmap can spin unboundedly inside xenbus (NMI-dump-proven, FINDINGS
    // 2026-08-05 cont 9); the retry loop here rolled that race ~20x per exit and
    // wedged the guest during OS shutdown (cont 11). The settle delay gives the
    // daemon time to process the DESTROYs above so the one attempt lands AFTER
    // its unmaps, not concurrently. Anything still busy is leaked loudly; domain
    // teardown reclaims it.
    // ...and even the single pass only when the daemon is GONE (vchan dead or
    // declared so above): a live daemon still maps the pages, so the revoke
    // cannot succeed and can only lose the race (the single attempt wedged one
    // more OS shutdown, 2026-08-06). Daemon alive => leak by design: one staging
    // grant (~7200 pages) per agent exit, reboot-cleared, loudly logged.
    if (!g_VchanClientConnected || !libvchan_is_open(g_Vchan))
    {
        Sleep(A6_EXIT_SETTLE_MS);
        PwRevokeTick();
        if (capture)
            CaptureRevokeStaleGrants(capture, L"exit-single-pass");
        if (PwRevokePending() || (capture && CaptureHasStaleGrants(capture)))
            LogWarning("A6LEAK exit: abandoning busy grants after single pass (per-window=%d, screen=%d)",
                PwRevokePending(), capture ? CaptureHasStaleGrants(capture) : FALSE);
    }
    else
    {
        LogWarning("A6LEAK exit by design: daemon still alive and mapping - skipping all revokes "
            "(xenbus revoke-vs-unmap race; see FINDINGS 2026-08-05 cont 9)");
    }

    PwShutdown();

    EnterCriticalSection(&g_VchanCriticalSection);
    if (closeVchan)
    {
        libvchan_close(g_Vchan);
        g_VchanClientConnected = FALSE;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (capture)
    {
        StopFrameProcessing(&capture); // no-op sends now; kept for the not-connected exit path
        CaptureTeardown(capture);
    }

    // STAGING: the persistent screen grant outlives every capture generation by design
    // (CaptureTeardown must never revoke it). This is its ONE revocation point: after
    // the teardown notifications and the drain above, best effort - a failure is
    // logged and the single buffer leaked, never a stall.
    CaptureStagingRevokeOnExit();

    LogInfo("exiting");
    // all handles will be closed on exit anyway

    return exitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static DWORD GetDomainName(OUT char *nameBuffer, IN DWORD nameLength)
{
    DWORD status = ERROR_SUCCESS;
    qdb_handle_t qdb = NULL;
    char *domainName = NULL;

    qdb = qdb_open(NULL);
    if (!qdb)
        return win_perror("qdb_open");

    domainName = qdb_read(qdb, "/name", NULL);
    if (!domainName)
    {
        LogError("Failed to read domain name");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("%S", domainName);
    status = StringCchCopyA(nameBuffer, nameLength, domainName);
    if (FAILED(status))
        win_perror2(status, "StringCchCopyA");

cleanup:
    qdb_free(domainName);
    if (qdb)
        qdb_close(qdb);

    return status;
}

static DWORD GetGuiDomainId(OUT USHORT* gid)
{
    DWORD status = ERROR_SUCCESS;
    qdb_handle_t qdb = NULL;
    char *string_id = NULL;
    int id = 0;

    qdb = qdb_open(NULL);
    if (!qdb)
        return win_perror("qdb_open");

    string_id = qdb_read(qdb, "/qubes-gui-domain-xid", NULL);
    if (!string_id)
    {
        LogError("Failed to read GUI domain id");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("GUI domain id: %S", string_id);

    id = atoi(string_id);
    if (errno == ERANGE || id < 0 || id > USHRT_MAX)
    {
        LogError("GUI domain id is invalid (%S)", string_id);
        status = ERROR_INVALID_DATA;
        goto cleanup;
    }

    status = ERROR_SUCCESS;
    *gid = (USHORT)id;

cleanup:
    qdb_free(string_id);
    if (qdb)
        qdb_close(qdb);

    return status;
}

static ULONG Init(void)
{
    ULONG status;
    WSADATA wsaData;
    WCHAR moduleName[CFG_MODULE_MAX];

    LogDebug("start");

    // This needs to be done first as a safeguard to not start multiple instances of this process.
    g_ShutdownEvent = CreateNamedEvent(QGA_SHUTDOWN_EVENT_NAME);
    if (!g_ShutdownEvent)
    {
        return GetLastError();
    }

    PerfInit();
    PwInit();

    EnableUIAccess();
    status = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));

    status = CfgReadDword(moduleName, REG_CONFIG_CURSOR_VALUE, &g_DisableCursor, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_CURSOR_VALUE);
        g_DisableCursor = TRUE;
    }

    DWORD seamlessMode;
    status = CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_VALUE, &seamlessMode, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (FALSE)", REG_CONFIG_SEAMLESS_VALUE);
        g_SeamlessMode = FALSE;
    }
    else
    {
        g_SeamlessMode = seamlessMode;
    }

    DWORD stagingGrant;
    status = CfgReadDword(moduleName, REG_CONFIG_STAGING_VALUE, &stagingGrant, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_STAGING_VALUE);
        g_StagingGrant = TRUE;
    }
    else
    {
        g_StagingGrant = (stagingGrant != 0);
    }

    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    // XXX needed?
    status = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != status)
    {
        win_perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    status = GetDomainName(g_DomainName, RTL_NUMBER_OF(g_DomainName));
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read domain name, using host name");

        status = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (status == 0)
        {
            if (0 != gethostname(g_DomainName, sizeof(g_DomainName)))
            {
                LogWarning("gethostname failed: 0x%x", status);
            }
            WSACleanup();
        }
        else
        {
            LogWarning("WSAStartup failed: 0x%x", status);
            // this is not fatal, only used to get host name for full desktop window title
        }
    }

    status = GetGuiDomainId(&g_GuiDomainId);
    if (status != ERROR_SUCCESS)
        return status;

    LogInfo("Fullscreen desktop name: %S", g_DomainName);

    InitializeListHead(&g_WatchedWindowsList);
    InitializeCriticalSection(&g_csWatchedWindows);

    // Attach the Qubes IDD and make it the sole active display BEFORE InitVideoModes() and
    // before the screen is mapped: both read the current topology, so doing this afterwards
    // would enumerate the emulated VGA's mode list and map the wrong screen geometry.
    // A failure here is deliberately NOT fatal - a guest whose IDD did not come up must
    // still get a working agent on the Basic Display Adapter rather than no GUI at all.
    // The failure is loud in the log and the health gate asserts the end state separately.
    if (ERROR_SUCCESS != EnsureQubesIddSolo())
        LogWarning("IDD solo failed - continuing on the current display topology");

    InitVideoModes();

    g_MinWindowWidth = GetSystemMetrics(SM_CXMIN);
    g_MinWindowHeight = GetSystemMetrics(SM_CYMIN);

    // The window-event thread creates the work-area broadcast listener, whose wndproc
    // takes g_WaLock; that lock must exist before the thread does (an early
    // WM_DISPLAYCHANGE or Explorer SPI_SETWORKAREA broadcast would otherwise enter an
    // uninitialized CRITICAL_SECTION). CreateThread orders this write for the new thread.
    WorkAreaLockInit();

    // Must be running before the main loop starts waiting on g_WindowEventSignal.
    status = StartWindowEventThread();
    if (ERROR_SUCCESS != status)
        return status;

    return ERROR_SUCCESS;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (ERROR_SUCCESS != Init())
        return win_perror("Init");

    InitializeCriticalSection(&g_VchanCriticalSection);

    // Call the thread proc directly.
    if (ERROR_SUCCESS != WatchForEvents())
    {
        StopWindowEventThread();
        return win_perror("WatchForEvents");
    }

    StopWindowEventThread();
    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("exiting");
    return ERROR_SUCCESS;
}
