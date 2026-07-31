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

// after we send MSG_DESTROY in fullscreen mode we can get delayed MSG_CONFIGURE,
// we shouldn't reply to that before sending MSG_CREATE
BOOL g_LocalScreenDestroyed = FALSE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
DWORD g_HostScreenWidth = 0;
DWORD g_HostScreenHeight = 0;

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
    ULONG status = SendWindowUnmap(entry->Handle);
    if (status != ERROR_SUCCESS)
        return status;

    return SendWindowMap(entry);
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
        if (owner)
        {
            BOOL ownerDisabled = GetWindowLong(owner, GWL_STYLE) & WS_DISABLED;
            if (ownerDisabled)
                entry->ModalParent = owner;
            else
                entry->ModalParent = NULL;
        }
    }

    entry->IsOverrideRedirect = IsPopup(entry);

    return ERROR_SUCCESS;
}

// watched window critical section must be entered
// also sends creation notifications to gui daemon
ULONG AddWindow(IN WINDOW_DATA* entry)
{
    ULONG status = ERROR_SUCCESS;
    LogVerbose("start, handle 0x%x, visible %d, iconic %d", entry->Handle, entry->IsVisible, entry->IsIconic);
    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // send window creation info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowCreate");
            goto end;
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

// Remove window from the list and free memory.
// Watched windows list critical section must be entered.
ULONG RemoveWindow(IN OUT WINDOW_DATA *entry)
{
    ULONG status = ERROR_INVALID_PARAMETER;

    LogVerbose("start");

    if (!entry)
        goto end;

    LogDebug("0x%x", entry->Handle);

    RemoveEntryList(&entry->ListEntry);

    if (entry->Handle == g_StartWindow)
        g_StartVisible = FALSE;

    if (g_VchanClientConnected)
    {
        status = SendWindowUnmap(entry->Handle);
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowUnmap");
            goto end;
        }

        if (entry->Handle) // never destroy screen "window"
        {
            status = SendWindowDestroy(entry->Handle);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SendWindowDestroy");
                goto end;
            }
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

// Adds all top-level windows to the watched list.
// This is the resync path: the watched list is normally maintained from window
// events instead (see TrackWindows).
// watched windows critical section must be entered
static ULONG AddAllWindows(IN OUT UINT* interrogated)
{
    ADD_WINDOWS_CONTEXT context = { 0 };

    LogVerbose("start");

    g_TaskbarWindow = FindWindow(L"Shell_TrayWnd", 0);
    g_ShowTaskbar = FALSE;

    ULONG status = ERROR_SUCCESS;
    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, (LPARAM)&context))
        status = context.Status != ERROR_SUCCESS ? context.Status : win_perror("EnumWindows");

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
            status = RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight);
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
    if (data->Width < g_MinWindowWidth || data->Height < g_MinWindowHeight)
        return FALSE;

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
            status = SendWindowConfigure(windowData->Handle,
                windowData->X, windowData->Y, windowData->Width, windowData->Height, windowData->IsOverrideRedirect);
            if (status != ERROR_SUCCESS)
                goto end;
        }
    }
    else
    {
        // configure first, then popup state
        if (coordsChanged)
        {
            status = SendWindowConfigure(windowData->Handle,
                windowData->X, windowData->Y, windowData->Width, windowData->Height, windowData->IsOverrideRedirect);
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

    stats->RemoveTicks = PerfNow() - perfPhase - (g_PerfSendTicks - perfSendPhase);
    return status;
}

// Apply pending window events outside of frame processing. This is what makes
// window moves reach the gui daemon at input rate instead of at capture rate.
static void ProcessWindowEvents(void)
{
    TRACK_STATS stats;
    ULONG status;

    EnterCriticalSection(&g_csWatchedWindows);
    status = TrackWindows(&stats);
    LeaveCriticalSection(&g_csWatchedWindows);

    if (status != ERROR_SUCCESS)
        win_perror2(status, "TrackWindows");

    g_TrackedUpdateTicks += stats.UpdateTicks;
    g_TrackedEnumTicks += stats.EnumTicks;
    g_TrackedRemoveTicks += stats.RemoveTicks;
    g_TrackedInterrogated += stats.Interrogated;
    g_TrackedEvents += stats.Events;
}

// Called after receiving new frame.
static ULONG ProcessNewFrame(IN const CAPTURE_FRAME* frame)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    ULONG status = ERROR_SUCCESS;

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

        if (frame->dirty_rects_count == 0)
        {
            // normally we don't get frames with 0 dirty rects unless it's the 1st one
            // then refresh everything
            LogDebug("no dirty rects, updating whole screen");
            SendWindowDamageEvent(NULL, 0, 0, g_ScreenWidth, g_ScreenHeight);
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

    // send damage notifications
    entry = (WINDOW_DATA *)g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA*)entry->ListEntry.Flink;
        perfWindows++;

        if (entry->IsIconic) // minimized, don't care
            goto skip;

        // INVARIANT: the origin used to convert damage to window-relative coordinates must
        // be the same origin most recently sent in MSG_CONFIGURE, because that is what the
        // gui-daemon adds back when it copies out of the shared framebuffer. TrackWindows()
        // above has just sent MSG_CONFIGURE with entry->X/Y, so entry->X/Y it must be.
        // Converting against the pre-tracking position instead was tried and is wrong: it
        // mis-registers every dragged window by exactly one frame of movement.
        RECT windowRect = { entry->X, entry->Y,
                            entry->X + (int)entry->Width, entry->Y + (int)entry->Height };
        RECT changedArea; // intersection of damage rect with window rect

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

                status = SendWindowDamageEvent(entry->Handle,
                    changedArea.left - entry->X, // window-relative, same origin as MSG_CONFIGURE
                    changedArea.top - entry->Y,
                    changedArea.right - changedArea.left, // size
                    changedArea.bottom - changedArea.top);

                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "SendWindowDamageEvent");
                    goto cleanup;
                }
            }
        }
skip:
        entry = nextEntry;
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
    // send whole screen window, needed even in seamless mode
    status = SendWindowCreate(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowCreate(NULL)");

    g_LocalScreenDestroyed = FALSE;

    // send the whole screen framebuffer map
    status = SendScreenGrants(FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight), (*capture)->grant_refs);
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

// CaptureTeardown() must be called separately after gui daemon confirms screen destruction
ULONG StopFrameProcessing(IN OUT CAPTURE_CONTEXT** capture)
{
    LogVerbose("start");
    if (!capture)
        return ERROR_SUCCESS;

    CaptureStop(*capture);

    ULONG status = SendWindowUnmap(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowUnmap(screen)");

    status = SendWindowDestroy(NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "SendWindowDestroy(screen)");

    // pause replying to gui daemon's messages for the destroyed screen window
    g_LocalScreenDestroyed = TRUE;

    LogVerbose("end");
    return ERROR_SUCCESS;
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
    if (!VchanInit(g_GuiDomainId, 6000))
    {
        LogError("VchanInit() failed");
        return GetLastError();
    }

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

    while (TRUE)
    {
        status = ERROR_SUCCESS;

        vchanIoInProgress = TRUE;

        // Wait for events.
        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
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
            // shutdown event
            LogDebug("Shutdown event signaled");
            exitLoop = TRUE;
            break;
        }

        switch (signaledEvent)
        {
        case 1: // new frame available
            LogVerbose("new frame");
            if (g_VchanClientConnected)
            {
                assert(capture);
                ProcessNewFrame(&capture->frame);
            }

            if (capture)
                SetEvent(capture->ready_event); // frame processed
            break;

        case 2:
            LogVerbose("fullscreen on");
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SetSeamlessMode(FALSE)");
                exitLoop = TRUE;
            }
            break;

        case 3:
            LogVerbose("fullscreen off");
            status = SetSeamlessMode(TRUE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "SetSeamlessMode(TRUE)");
                exitLoop = TRUE;
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

                if (ERROR_SUCCESS != SendProtocolVersion())
                {
                    LogError("SendProtocolVersion failed");
                    exitLoop = TRUE;
                    break;
                }

                if (ERROR_SUCCESS != HandleVersion())
                {
                    LogError("HandleVersion failed");
                    exitLoop = TRUE;
                    break;
                }

                // This will probably change the current video mode if we don't have one saved in the registry.
                if (ERROR_SUCCESS != HandleXconf())
                {
                    LogError("HandleXconf failed");
                    exitLoop = TRUE;
                    break;
                }

                status = StartFrameProcessing(newFrameEvent, captureErrorEvent, &capture);
                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "StartFrameProcessing");
                    exitLoop = TRUE;
                    break;
                }

                break;
            }

            EnterCriticalSection(&g_VchanCriticalSection);
            LogVerbose("vchan receive, %d bytes", VchanGetReadBufferSize(g_Vchan));

            vchanIoInProgress = FALSE;

            if (!libvchan_is_open(g_Vchan))
            {
                LogError("vchan disconnected");
                exitLoop = TRUE;
                LeaveCriticalSection(&g_VchanCriticalSection);
                break;
            }

            BOOL screenDestroyed = FALSE;
            while (VchanGetReadBufferSize(g_Vchan) > 0)
            {
                status = HandleServerData(!g_LocalScreenDestroyed, &screenDestroyed);
                if (ERROR_SUCCESS != status)
                {
                    exitLoop = TRUE;
                    LogError("HandleServerData failed: 0x%x", status);
                    break;
                }
            }
            LeaveCriticalSection(&g_VchanCriticalSection);

            if (screenDestroyed)
            {
                LogDebug("gui daemon confirms screen destruction");
                CaptureTeardown(capture);
                capture = NULL;
                status = StartFrameProcessing(newFrameEvent, captureErrorEvent, &capture);
                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "StartFrameProcessing");
                    exitLoop = TRUE;
                    break;
                }
            }
            break;

        case 5: // capture error, can be due to a desktop switch or resolution change
            LogDebug("capture error");

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

        if (exitLoop)
            break;
    }

    LogDebug("main loop finished");

    EnterCriticalSection(&g_VchanCriticalSection);
    if (g_VchanClientConnected)
    {
        libvchan_close(g_Vchan);
        g_VchanClientConnected = FALSE;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (capture)
    {
        StopFrameProcessing(&capture);
        CaptureTeardown(capture);
    }

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

    InitVideoModes();

    g_MinWindowWidth = GetSystemMetrics(SM_CXMIN);
    g_MinWindowHeight = GetSystemMetrics(SM_CYMIN);

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
