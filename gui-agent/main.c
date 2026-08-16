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
#include "toastcrop.h"
#include "faultinject.h"
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

// Last window the foreground re-raise in AddAllWindows acted on. Cleared by ResetWatch so
// the corrective re-fires on the first pass after every mass re-announce.
static HWND g_LastForeground = NULL;

// Window the user is currently dragging with a held mouse button, as seen on the INPUT path
// (set by HandleButton on press, cleared on release). The frame path's move detection keys
// off LOCATIONCHANGE events, which lapse on ~5% of drag frames and after PW_MOVE_SETTLE_MS
// of a slow drag - and every lapsed frame pays a full PrintWindow recapture (15-18 ms on
// this GPU-less guest), which is both the measured drag-time cost and pure waste, since a
// moving window's CONTENT does not change. The latch closes those gaps: while it is set the
// window counts as moving regardless of event timing.
volatile HWND g_InputDragWindow = NULL;

// Tick of the last input event seen for the latched window. A Button1 release can be lost
// (agent restart mid-click, secure desktop, a click that lands on another qube), and with
// the drag freeze in force a stuck latch would hold that window's position announces - and
// its legacy-path content - frozen until the next Button1 event anywhere. The pump's
// existing WAIT_TIMEOUT sweep disarms it after this long without input (review finding).
DWORD g_InputDragLastEventTick = 0;
#define INPUT_DRAG_STUCK_MS 10000

// Translation origin FROZEN at the Button1 press that armed the latch (D1 drag
// wobble). dom0 sends WINDOW-RELATIVE input coordinates; the agent reconstructs an
// absolute by adding a guest-side origin, but the true addend is dom0's APPLIED
// window origin, which lags our announces and is unobservable during a guest-native
// drag (measured: ZERO inbound MSG_CONFIGURE inside a 5.85 s drag). With the LIVE
// tracked origin the guest's modal move loop closes a feedback loop that is
// divergent for the measured announce-apply lag (66-250 ms against a ~10 ms event
// rate), saturating at cursor-re-anchor amplitude v*lag = 40-168 px - the measured
// 40-163 px back-and-forth with 16-19% of announces reversing direction. Injection
// is EXACT if and only if dom0's origin is held constant for the whole drag, so:
// translate against the origin captured at press, and withhold position-only
// announces for the latched window until release (SendWindowConfigureIfChanged).
// All writers/readers run on the WatchForEvents pump thread (HandleServerData is
// dispatched from the same loop), so no synchronization is needed.
int g_InputDragOriginX = 0;
int g_InputDragOriginY = 0;
BOOL g_InputDragOriginValid = FALSE;

// LIVE-FEEDBACK drag servo (D1 drag wobble, second iteration; default path - the freeze
// above remains as the InputDragFreeze=1 fallback). The freeze is exact but was rejected:
// the dom0 window must keep FOLLOWING the cursor during the drag. So announces stay live
// and the loop stays closed - but the control law is restructured. With the LIVE origin
// the injection is A = r + W_live and the app's modal move loop applies W' = A - g: a
// gain-1 servo through the announce->apply->next-motion transport lag (measured 66-250 ms
// against a ~46 ms move-loop cadence, i.e. 1-5 samples of delay). Its characteristic
// roots are |z| = 1.00 at one sample and 1.15-1.19 at 2-5 - structurally oscillatory
// across the WHOLE measured lag range, which is the observed 40-163 px forth-and-back
// with 16-19% of announces reversing direction.
//
// The fix removes the delay from the loop equation instead of detuning around it
// (a plain gain reduction stable to 250 ms needs beta <= 0.285 and then trails the
// cursor by v*T/beta ~ 250 px - rejected): dom0's applied origin D is RECONSTRUCTED
// from the agent's own timestamped announce history. D can only ever take values this
// agent announced, in order (measured drift announced-vs-applied: 0,0 in 240/240
// samples), so estimating it is a pure timing problem, and the announce send times are
// known exactly - only the small transit+apply time tau must be assumed. The injection
// then servos the window toward the reconstructed cursor C_hat = r + D_hat at a
// fractional gain: with the history exact the closed loop is z^m * (z - (1-beta)) - a
// single real pole INDEPENDENT of the transport lag m, so it cannot oscillate for any
// measured lag; a deliberate +/-46 ms timing mismatch (twice the plausible tau error)
// keeps the worst root at 0.881 for beta = 0.6, while beta = 0.7 breaks at 1.060 (do
// not raise the default). Full-jitter simulation against the measured distributions:
// 1.6% residual reversals (at announce-quantization amplitude) vs 43% for the live
// origin, 11 px stop-overshoot, ~150 ms step settling.
//
// All state below is pump-thread-only, the same invariant as the frozen-origin globals
// above: HandleButton/HandleMotion (HandleServerData), the frame walk and the settle
// sweep all run on WatchForEvents' thread.

// Grab offset: dom0's window-relative cursor position at the Button1 press that armed
// the latch. The press injection lands the guest cursor at (origin + grab), so the
// modal move loop's own grab offset equals this by construction.
// The cursor position the servo last injected for the latched window. The drag law walks
// THIS toward the reconstructed dom0 cursor, so the modal loop's grab offset never enters
// the loop and cannot poison it when Windows re-anchors it mid-drag (review finding).
// Relative coordinates of the previous drag motion, used to bound how far one injected
// step may move (see the clamp in InjectMotion).
int g_DragLastRelX = 0;
int g_DragLastRelY = 0;
int g_DragLastInjectedX = 0;
int g_DragLastInjectedY = 0;
int g_InputDragGrabX = 0;
int g_InputDragGrabY = 0;

// Ring of position announces SENT for the latched window: what dom0 was told, and when.
// 16 entries x the 66 ms measured announce gap covers >1 s of history against the
// 250 ms max observed apply lag - 4x headroom.
#define DRAG_ANNOUNCE_RING 16
static struct
{
    DWORD Tick;
    int X;
    int Y;
} g_DragAnnounces[DRAG_ANNOUNCE_RING];
static UINT g_DragAnnounceHead = 0;  // next write slot
static UINT g_DragAnnounceCount = 0; // 0 = no drag armed, ring must not be read

// Seed the ring at the Button1 press with the origin the press translated against:
// until the first mid-drag announce is applied dom0's origin CANNOT differ from it, so
// the first ~66 ms of every drag reconstruct exactly by construction.
void DragAnnounceReset(IN int x, IN int y)
{
    g_DragAnnounces[0].Tick = GetTickCount();
    g_DragAnnounces[0].X = x;
    g_DragAnnounces[0].Y = y;
    g_DragAnnounceHead = 1;
    g_DragAnnounceCount = 1;
}

void DragAnnounceClear(void)
{
    g_DragAnnounceHead = 0;
    g_DragAnnounceCount = 0;
}

// Record one sent position announce for the latched window. Callers gate on the latch;
// the count gate here additionally refuses to grow a ring no press has seeded.
void DragAnnounceRecord(IN int x, IN int y)
{
    if (g_DragAnnounceCount == 0)
        return;
    // A same-position announce (size-only change) does not move dom0's origin: skip it,
    // so the ring stays a strict position history and DragAnnounceMoved() below keeps
    // meaning "this window has MOVED during this drag" - the discriminator that keeps
    // client-area drags out of the damped law.
    {
        UINT newest = (g_DragAnnounceHead + DRAG_ANNOUNCE_RING - 1) % DRAG_ANNOUNCE_RING;
        if (g_DragAnnounces[newest].X == x && g_DragAnnounces[newest].Y == y)
            return;
    }
    g_DragAnnounces[g_DragAnnounceHead].Tick = GetTickCount();
    g_DragAnnounces[g_DragAnnounceHead].X = x;
    g_DragAnnounces[g_DragAnnounceHead].Y = y;
    g_DragAnnounceHead = (g_DragAnnounceHead + 1) % DRAG_ANNOUNCE_RING;
    if (g_DragAnnounceCount < DRAG_ANNOUNCE_RING)
        g_DragAnnounceCount++;
}

// TRUE once at least one position announce for the latched window has gone out since
// the press: the window demonstrably moves with this drag, so the feedback loop
// (announce -> dom0 apply -> window-relative coordinates) is closed and the damped
// law applies. Until then - which covers EVERY client-area drag (text selection,
// scrollbars, sliders: the window never moves, so this never trips) and the first
// ~66 ms of every title-bar drag - dom0's applied origin cannot differ from the press
// origin, the reconstruction is exact, and damping would be pure harm: it would make
// a selection drag's cursor undershoot by (1-beta) of its pull from the grab point.
BOOL DragAnnounceMoved(void)
{
    return g_DragAnnounceCount > 1;
}

// QUANTISED ORIGIN (drag wobble, exact variant). The servo below interpolates because it is
// GUESSING when each announce landed. That guess is the servo's weak point - and it is avoidable,
// because dom0's origin is not an unknown quantity at all: it is a value WE chose and sent. The only
// unknown is WHEN it took effect.
//
// So do not estimate the value; wait out the timing. Return the newest announce that is older than
// adoptMs, i.e. one dom0 has certainly applied by now. Between announces the origin is then exactly
// right rather than approximately right, the reconstruction r + origin is exact, and the gain-1
// feedback loop that produces the 40-163 px oscillation never closes: the origin this event uses
// cannot be moved by the announce this event causes.
//
// The cost is deliberate and bounded: while an announce is still within adoptMs we keep using the
// PREVIOUS one, so a fast hand runs ahead of dom0 by up to one announce interval - a constant
// offset, not an oscillation. Pair it with InputDragAnnounceMs to make that interval a choice.
BOOL DragAnnounceAppliedOrigin(IN DWORD adoptMs, OUT int* x, OUT int* y)
{
    if (g_DragAnnounceCount == 0)
        return FALSE;

    const DWORD now = GetTickCount();
    const UINT oldest = (g_DragAnnounceHead + DRAG_ANNOUNCE_RING - g_DragAnnounceCount) % DRAG_ANNOUNCE_RING;

    // Walk newest -> oldest for the first entry old enough to be applied. The press seed is the
    // floor: dom0's origin cannot differ from it before the first mid-drag announce.
    for (UINT i = 0; i < g_DragAnnounceCount; i++)
    {
        UINT idx = (g_DragAnnounceHead + DRAG_ANNOUNCE_RING - 1 - i) % DRAG_ANNOUNCE_RING;
        if ((DWORD)(now - g_DragAnnounces[idx].Tick) >= adoptMs)
        {
            *x = g_DragAnnounces[idx].X;
            *y = g_DragAnnounces[idx].Y;
            return TRUE;
        }
    }

    *x = g_DragAnnounces[oldest].X;
    *y = g_DragAnnounces[oldest].Y;
    return TRUE;
}

// dom0's origin at absolute tick atTick, linearly interpolated between the two
// bracketing announces. D really moves stepwise at unobservable apply times;
// interpolation smooths the announce quantization instead of guessing each step edge,
// and the servo's stability margin (worst root 0.881 under a +/-46 ms mismatch, a full
// announce interval) covers the difference. Clamped to the oldest entry before the
// recorded range (the exact press origin) and to the newest after it.
BOOL DragAnnounceOriginAt(IN DWORD atTick, OUT int* x, OUT int* y)
{
    if (g_DragAnnounceCount == 0)
        return FALSE;

    UINT oldest = (g_DragAnnounceHead + DRAG_ANNOUNCE_RING - g_DragAnnounceCount) % DRAG_ANNOUNCE_RING;
    BOOL haveBelow = FALSE;
    UINT below = 0; // logical index (0..count-1) of the newest entry with Tick <= atTick
    for (UINT i = 0; i < g_DragAnnounceCount; i++)
    {
        UINT idx = (oldest + i) % DRAG_ANNOUNCE_RING;
        // Signed wraparound-safe compare: entry tick <= atTick?
        if ((LONG)(g_DragAnnounces[idx].Tick - atTick) <= 0)
        {
            haveBelow = TRUE;
            below = i;
        }
        else
            break; // ticks are monotone; the first future entry ends the scan
    }

    if (!haveBelow) // atTick predates the whole ring: the seed (press origin) is exact
    {
        *x = g_DragAnnounces[oldest].X;
        *y = g_DragAnnounces[oldest].Y;
        return TRUE;
    }

    UINT prevIdx = (oldest + below) % DRAG_ANNOUNCE_RING;
    if (below + 1 >= g_DragAnnounceCount) // nothing newer recorded: clamp to the last announce
    {
        *x = g_DragAnnounces[prevIdx].X;
        *y = g_DragAnnounces[prevIdx].Y;
        return TRUE;
    }

    UINT nextIdx = (oldest + below + 1) % DRAG_ANNOUNCE_RING;
    DWORD span = g_DragAnnounces[nextIdx].Tick - g_DragAnnounces[prevIdx].Tick;
    DWORD frac = atTick - g_DragAnnounces[prevIdx].Tick;
    if (span == 0)
    {
        *x = g_DragAnnounces[nextIdx].X;
        *y = g_DragAnnounces[nextIdx].Y;
        return TRUE;
    }
    *x = g_DragAnnounces[prevIdx].X + (int)(((LONGLONG)(g_DragAnnounces[nextIdx].X -
        g_DragAnnounces[prevIdx].X) * (LONGLONG)frac) / (LONGLONG)span);
    *y = g_DragAnnounces[prevIdx].Y + (int)(((LONGLONG)(g_DragAnnounces[nextIdx].Y -
        g_DragAnnounces[prevIdx].Y) * (LONGLONG)frac) / (LONGLONG)span);
    return TRUE;
}

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

// Wake the main loop's window-tracking pass without queuing a specific window event.
// Used by the toastcrop worker when an async measurement resolves, so the re-announce
// with the cropped rect happens within one pass instead of waiting for the surface's
// next natural event. Safe from any thread; a spurious signal costs one empty pass.
void PokeWindowTracking(void)
{
    if (g_WindowEventSignal)
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
        {
            // R2 ("ResyncDragDefer", default off): the resync interrogates EVERY tracked
            // window in one burst, and concurrent with a drag's throttled PrintWindow
            // recapture each interrogation stalls ~8 ms - the measured 24-53 ms upd
            // spikes land squarely inside the p95=140 ms announce-gap tail. Defer the
            // burst while an input drag is latched, capped at 3x the interval so a lost
            // release cannot starve the safety net. Never deferred in fallback mode:
            // with the hook thread dead the resync IS the tracking.
            BOOL deferred = g_ResyncDragDefer &&
                interval == WINDOW_RESYNC_INTERVAL_MS &&
                g_InputDragWindow != NULL &&
                (now - g_LastResyncTime) < 3 * WINDOW_RESYNC_INTERVAL_MS;

            if (!deferred)
                resync = TRUE;
        }
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

// R1 ("MonInfoCache", default off): generation counter for the per-HMONITOR cache
// below, bumped by the WM_DISPLAYCHANGE listener - the only event on which the cached
// monitor rect / display mode can change. Interlocked because the listener runs on the
// work-area broadcast thread while the cache itself lives on the pump thread.
static volatile LONG g_MonCacheGen = 1;

// Belt-and-braces bound on cache staleness if the broadcast listener is down: worst
// case degrades to today's per-call cost for one interval, never to wrong-forever.
#define MON_CACHE_TTL_MS 2000

void MonitorCacheInvalidate(void)
{
    InterlockedIncrement(&g_MonCacheGen);
}

// The GetMonitorInfo + EnumDisplaySettings pair costs ~340 us per interrogation in the
// best case, and EnumDisplaySettings serializes into the display path the IDD present
// loop saturates during a drag (42 ms p50 present cadence) - it is the one call here
// that can independently block for tens of ms. The inputs change only on a mode
// change, which WM_DISPLAYCHANGE observes, so a cached copy returns byte-identical
// values. The cache needs its own lock: besides the pump thread, GetRealWindowRect
// runs on the toastcrop worker (measurement recheck) and the capture thread (the
// ProtoTraceWobble probe in send.c). The lock is only ever held for struct copies -
// the system calls stay outside it.
static SRWLOCK g_MonCacheLock = SRWLOCK_INIT;

static BOOL GetMonitorSettings(IN HMONITOR monitor, OUT MONITORINFOEX* monInfo, OUT DEVMODE* devMode)
{
    static HMONITOR cachedMonitor = NULL; // all statics guarded by g_MonCacheLock
    static LONG cachedGen = 0;
    static DWORD cachedTick = 0;
    static MONITORINFOEX cachedInfo;
    static DEVMODE cachedMode;

    LONG gen = InterlockedCompareExchange(&g_MonCacheGen, 0, 0);
    if (g_MonInfoCache)
    {
        BOOL hit = FALSE;
        AcquireSRWLockShared(&g_MonCacheLock);
        if (monitor == cachedMonitor && gen == cachedGen &&
            GetTickCount() - cachedTick < MON_CACHE_TTL_MS)
        {
            *monInfo = cachedInfo;
            *devMode = cachedMode;
            hit = TRUE;
        }
        ReleaseSRWLockShared(&g_MonCacheLock);
        if (hit)
            return TRUE;
    }

    monInfo->cbSize = sizeof(*monInfo);
    if (!GetMonitorInfo(monitor, (LPMONITORINFO)monInfo))
        return FALSE;

    devMode->dmSize = sizeof(DEVMODE);
    devMode->dmDriverExtra = 0;
    // Return value deliberately unchecked, as it always was on this path - but a
    // failed read must not be CACHED, or one glitch would be replayed for a TTL.
    if (EnumDisplaySettings(monInfo->szDevice, ENUM_CURRENT_SETTINGS, devMode) && g_MonInfoCache)
    {
        // Only WRITE the cache when caching is enabled: with the knob off (the shipped
        // default) this path must stay byte-identical to the old code, and in particular
        // must not take a new cross-thread exclusive lock on the per-window hot path that
        // the pump, capture and toastcrop threads all walk (review finding).
        AcquireSRWLockExclusive(&g_MonCacheLock);
        cachedMonitor = monitor;
        cachedGen = gen;
        cachedTick = GetTickCount();
        cachedInfo = *monInfo;
        cachedMode = *devMode;
        ReleaseSRWLockExclusive(&g_MonCacheLock);
    }
    return TRUE;
}

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

    // DWM hands back GARBAGE for a cloaked / mid-transition shell surface: measured -666 x
    // -750 for a StartMenuExperienceHost CoreWindow (2026-08-12), which then poisoned
    // entry->X/Y, the toast-crop query key (logged as 4294966630x4294966546) and the slice
    // rect. S_OK is not a validity check - verify the rect is a rect.
    if (dwmRect.right <= dwmRect.left || dwmRect.bottom <= dwmRect.top)
    {
        RECT fallback;
        if (GetWindowRect(window, &fallback) &&
            fallback.right > fallback.left && fallback.bottom > fallback.top)
        {
            LogDebug("0x%x: inverted DWM bounds (%d,%d)-(%d,%d), using GetWindowRect",
                window, dwmRect.left, dwmRect.top, dwmRect.right, dwmRect.bottom);
            dwmRect = fallback;
        }
        else
        {
            LogWarning("0x%x: inverted DWM bounds (%d,%d)-(%d,%d) and no usable GetWindowRect - rejecting",
                window, dwmRect.left, dwmRect.top, dwmRect.right, dwmRect.bottom);
            return ERROR_INVALID_DATA;
        }
    }

    // monitor info is needed to adjust for DPI scaling
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX monInfo;
    DEVMODE devMode;
    if (!GetMonitorSettings(monitor, &monInfo, &devMode))
        return win_perror("GetMonitorInfo failed");

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

    // Mirror of the daemon's MAX_OVERRIDE_REDIRECT_PERCENTAGE (xside.h, 90%): a rect
    // between 90% and 100% passes the guard above, but the daemon strips its
    // override_redirect while mapping it, and no later MSG_CONFIGURE can turn the flag back
    // on. Without this the strip is invisible to the agent - it keeps believing the window
    // is a popup, so the popup-state-flip path never fires and the window stays WM-placed
    // forever. Agreeing with the daemon here is what lets the EXISTING flip machinery
    // (UpdateWindowData -> ToggleMap, i.e. UNMAP+MAP) re-latch override_redirect once the
    // window shrinks back under the threshold.
    // The daemon measures against dom0's WORK area, which the guest cannot observe; the
    // guest screen (driven to dom0's viewport) is the closest thing it has, so this is the
    // same rule one panel's worth of area too generous, never too strict - it can only
    // fail to demote, exactly as today, never demote a window dom0 would have accepted.
    if (isPopup && g_HostScreenWidth > 0 && g_HostScreenHeight > 0 &&
        (ULONGLONG)entry->Width * (ULONGLONG)entry->Height * 100ULL >
        (ULONGLONG)g_HostScreenWidth * (ULONGLONG)g_HostScreenHeight * 90ULL)
    {
        LogDebug("0x%x: popup over 90%% of the screen: %ux%u, host screen %ux%u",
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
    // GetRealWindowRect returns a WIN32 code, not an HRESULT, and two of its failure returns are
    // POSITIVE - ERROR_INVALID_DATA and win_perror's return - for which SUCCEEDED() is TRUE. The
    // old !SUCCEEDED test therefore SKIPPED this block for them, fell through with `rect` still
    // UNINITIALIZED, and returned ERROR_SUCCESS: stack garbage announced to dom0 as window
    // geometry. That is a protocol violation the daemon is entitled to kill the qube over - it is
    // the "invalid or suspicious GUI request" dialog, whose source was never identified.
    if (status != ERROR_SUCCESS)
    {
        // A window that died between being enumerated and being measured is the normal case,
        // not a fault: DwmGetWindowAttribute answers E_HANDLE for a handle that is already gone.
        // It was logged at ERROR and dominated the log - dozens of lines a minute on an idle
        // guest, which is how a real failure gets missed. Only this one status is demoted;
        // anything else still shouts.
        // status is ULONG here while HRESULT_FROM_WIN32 yields a signed HRESULT, so compare as
        // ULONG - /W4 with warnings-as-errors rejects the mixed-sign compare.
        // Neither of these is a fault, and both are already explained one frame down:
        // E_HANDLE is a window that died between enumeration and measurement, and
        // ERROR_INVALID_DATA was just logged BY GetRealWindowRect with the offending rectangle.
        // Measured: fixing the swallow above turned 64 silent garbage announcements into 65 ERROR
        // lines per 40 s, which would simply move the noise rather than remove it.
        if (status == (ULONG)HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
            status == ERROR_INVALID_DATA)
        {
            LogDebug("0x%x: not measurable this pass (0x%x)", window, status);
            return (ULONG)status;
        }
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
    entry->SizeLockW = -1; // never sent; distinguishes "locked to 0" impossible-state
    entry->SizeLockH = -1;

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

    // A shell toast draws its drop shadow INSIDE its own window rect (probed on the guest:
    // GetWindowRect == DWMWA_EXTENDED_FRAME_BOUNDS, delta zero), so the rect Windows
    // reports is larger than the visible card - measured live 2026-08-11 at 396x332
    // announced for a 377x287 card. Announced as-is, dom0 borders that margin and the
    // slice copy fills it with composited desktop pixels.
    //
    // This is writer #1 of the canonical rect, so cropping here (and nowhere else) is what
    // makes MSG_CREATE, MSG_CONFIGURE, the per-window grant and its slice copy, damage
    // conversion and occlusion agree by construction - the same reason the WS_MAXIMIZE
    // clamp below lives here. It runs BEFORE IsOverrideRedirect is evaluated on purpose:
    // the cropped size is the one that has to clear the agent's own popup guard and the
    // daemon's 90% strip.
    //
    // Toasts are REQUIRED-kept (CLAUDE.md 2A-chrome 3c): every failure inside toastcrop
    // yields zero insets, which leaves the rect exactly as it is today.
    if (IsShellToastWindow(entry))
    {
        RECT insets;
        if (ToastCropLookup(entry, &insets))
        {
            entry->X += (int)insets.left;
            entry->Y += (int)insets.top;
            entry->Width -= (DWORD)(insets.left + insets.right);
            entry->Height -= (DWORD)(insets.top + insets.bottom);
            entry->CropLeft = (int)insets.left;
            entry->CropTop = (int)insets.top;
            entry->CropRight = (int)insets.right;
            entry->CropBottom = (int)insets.bottom;
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

    // WM-managed shell surfaces (user requirement 2026-08-11: Start and toasts must be
    // movable). An override-redirect window is invisible to the dom0 WM - no frame, no
    // drag, guest-authoritative placement. Announcing the classified shell surfaces as
    // NORMAL windows gives them a dom0 frame and a WM drag; the dom0-initiated moves come
    // back as MSG_CONFIGURE and HandleConfigure applies them crop-compensated. This
    // deliberately deviates from Linux-agent parity (menus are OR there) - it is what the
    // user asked for, and gui-agent\ShellManaged=0 restores parity.
    // ...but ONLY once the toast crop has resolved (insets are set just above). Before that
    // the surface's announced size is the full/host rect - making THAT a WM-managed window
    // would flash a giant resizable frame until the crop lands (measured 2026-08-12: a shell
    // CoreWindow announced or=0 at 5120x1336). Staying an override-redirect popup until the
    // card is known avoids that; the crop-resolve re-announce then flips it to or=0 via the
    // usual UpdateWindowData->ToggleMap path, and the size-lock hint goes out with it.
    if (entry->IsOverrideRedirect &&
        (entry->CropLeft || entry->CropTop || entry->CropRight || entry->CropBottom))
    {
        SHELL_SURFACE_KIND kind = ShellSurfaceKind(entry);
        if (kind != ShellSurfaceNone &&
            (g_ShellManaged == SHELL_MANAGED_ALL ||
             (g_ShellManaged == SHELL_MANAGED_START && kind == ShellSurfaceStart)))
        {
            LogDebug("0x%x: cropped shell surface kind=%d announced WM-managed (policy %u)",
                entry->Handle, kind, g_ShellManaged);
            entry->IsOverrideRedirect = FALSE;
        }
    }

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
        if (ERROR_INVALID_DATA == status)
        {
            // The wire sanitizer refused this window's rect (send.c), so dom0 has no CREATE
            // for it. CreateSent must stay FALSE: that is what makes the created-window gate
            // quarantine every dependent message for this hwnd - MAP, WMNAME, CONFIGURE,
            // damage - each of which the daemon exit(1)s on without a CREATE. DeletePending
            // makes the next tracking pass re-examine the window from scratch, so a
            // transient bad rect self-heals into a normal announcement.
            // Reported as success on purpose: one bad window must not abort the EnumWindows
            // pass that is announcing all the others (AddWindowsProc stops on any failure).
            LogWarning("0x%x: geometry refused at the wire, leaving the window unannounced "
                "(it stays invisible in dom0 until a later pass re-creates it)", entry->Handle);
            entry->DeletePending = TRUE;
            status = ERROR_SUCCESS;
            goto end;
        }
        if (ERROR_SUCCESS != status)
        {
            win_perror2(status, "SendWindowCreate");
            goto end;
        }
        entry->CreateSent = TRUE;

        // FAULT INJECTION (test builds only, rank 1). Reproduces the 13 duplicate CREATEs
        // observed for live HWNDs during a Start-menu flip storm. It has to sit AFTER
        // CreateSent, because the defect is precisely a second CREATE for a window the
        // agent already believes it announced. gui-daemon is documented to exit(1) on a
        // CREATE for an id it already tracks (xside.c:3943-3948) yet demonstrably did not,
        // and that contradiction is an open question rank 3 depends on
        // (docs/PLAN-stability-overhaul.md) - this is how it gets answered on demand
        // instead of by waiting for a storm to cooperate.
        // It must be the FORCED variant: rank 3's create-once rule now answers an ordinary
        // repeat CREATE with MSG_CONFIGURE, so calling SendWindowCreate here would exercise
        // the fix instead of re-introducing the defect it removes.
        if (FiShouldDupCreate(entry->Handle))
            (void)SendWindowCreateForced(entry);

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
// Minimum spacing between position-only MSG_CONFIGUREs for one window (~60/s). Fast enough
// that a drag looks continuous in dom0, slow enough that we cannot outrun the daemon's
// consumption and build a backlog that replays after the drag.
#define CFG_POS_MIN_INTERVAL_MS 16

// How long a posted async SetWindowPos is presumed in flight before a newer daemon
// geometry may be posted anyway (the window may legitimately never reach the target,
// e.g. a guest-side snap; without the timeout one lost move would wedge the drive).
#define DAEMON_MOVE_INFLIGHT_MS 200

// Minimum spacing between refreshes of the announce-space/SetWindowPos-space delta
// (one DwmGetWindowAttribute + GetMonitorInfo + EnumDisplaySettings per refresh).
#define DAEMON_OFF_TTL_MS 500

// Send a position the rate limiter withheld, once the window has stopped moving. Called from
// the tracking pass for every window; cheap no-op when nothing is pending.
static void CfgFlushPendingMove(IN OUT WINDOW_DATA* entry)
{
    if (!entry->CfgPendingPos)
        return;
    if (GetTickCount() - entry->CfgLastSentTick < CFG_POS_MIN_INTERVAL_MS)
        return; // still inside the window; the next pass will flush it
    // While the daemon is dictating this window's geometry, dom0 already knows where its
    // window is - hold the flush until the drive ends, then announce the true resting
    // place below (or nothing, if the window landed exactly where the daemon put it).
    if (entry->DaemonStreamTick != 0 &&
        GetTickCount() - entry->DaemonStreamTick < DAEMON_DRIVE_ACTIVE_MS)
        return;
    // Mirror hold for a guest-native drag (D1 drag wobble): a withheld position that
    // leaks mid-drag moves dom0's applied origin and re-opens the feedback loop the
    // frozen translation origin closed. On release the latch clears and the next pass
    // (frame loop or settle sweep) flushes exactly one final announce.
    if (g_InputDragFreeze && g_InputDragOriginValid && entry->Handle == g_InputDragWindow)
        return;

    // Flush the CURRENT canonical position, not the coordinates that were withheld: the
    // window may have moved again since the limiter (or the daemon-drive hold) recorded
    // them, and announcing a stale intermediate is exactly the replay this exists to stop.
    entry->CfgPendingPos = FALSE;
    if (entry->CfgSentValid &&
        entry->LastCfgX == entry->X && entry->LastCfgY == entry->Y &&
        entry->LastCfgW == (int)entry->Width && entry->LastCfgH == (int)entry->Height &&
        entry->LastCfgOvr == entry->IsOverrideRedirect)
        return; // already exactly what the daemon knows - announcing it again would echo

    entry->CfgLastSentTick = GetTickCount();
    if (SendWindowConfigure(entry->Handle, entry->X, entry->Y,
            entry->Width, entry->Height, entry->IsOverrideRedirect) == ERROR_SUCCESS)
    {
        entry->CfgSentValid = TRUE;
        entry->LastCfgX = entry->X;
        entry->LastCfgY = entry->Y;
        entry->LastCfgW = (int)entry->Width;
        entry->LastCfgH = (int)entry->Height;
        entry->LastCfgOvr = entry->IsOverrideRedirect;
        // Live drag servo: the reconstruction of dom0's applied origin is only as
        // complete as this history - every position the daemon will apply for the
        // latched window must be recorded at its send tick.
        if (entry->Handle == g_InputDragWindow && g_InputDragOriginValid)
            DragAnnounceRecord(entry->X, entry->Y);
    }
}

// Pace announces for the dragged window when InputDragAnnounceMs is set. A larger interval means
// dom0's window steps instead of gliding, but the origin the quantised reconstruction uses is
// settled for a larger fraction of the drag - the whole dial between "dom0 follows smoothly" and
// "input maps exactly". Zero keeps the natural rate.
static BOOL DragAnnouncePaced(IN const WINDOW_DATA* entry)
{
    static DWORD lastTick = 0;
    if (!g_InputDragAnnounceMs || entry->Handle != g_InputDragWindow || !g_InputDragOriginValid)
        return TRUE;
    const DWORD now = GetTickCount();
    if (lastTick != 0 && (DWORD)(now - lastTick) < g_InputDragAnnounceMs)
        return FALSE;
    lastTick = now;
    return TRUE;
}

static ULONG SendWindowConfigureIfChanged(IN OUT WINDOW_DATA* entry)
{
    if (!DragAnnouncePaced(entry))
        return ERROR_SUCCESS;

    if (entry->Synthesized)
        return ERROR_SUCCESS; // never announced; see main.h
    if (entry->CfgSentValid &&
        entry->LastCfgX == entry->X && entry->LastCfgY == entry->Y &&
        entry->LastCfgW == (int)entry->Width && entry->LastCfgH == (int)entry->Height &&
        entry->LastCfgOvr == entry->IsOverrideRedirect)
        return ERROR_SUCCESS;

    // OUTGOING RATE LIMIT for position-only changes.
    //
    // A guest-native drag moves the window at input rate, and every step used to be
    // announced. If the daemon drains MSG_CONFIGURE slower than we produce them, the ring
    // becomes a QUEUE OF STALE POSITIONS: the guest window is already at rest while dom0 is
    // still working through the backlog, so the dom0 window walks the whole drag path after
    // the user let go - the "jump back and replay the trajectory" (user-reported 2026-08-12;
    // the guest window was sampled STATIC while the agent was still announcing motion, which
    // is exactly this producer/consumer mismatch, and it is why nothing on the INBOUND path
    // explained it - the daemon sends no configures during such a drag at all).
    //
    // Latest-wins: at most one position-only announce per window per CFG_POS_MIN_INTERVAL_MS,
    // with the withheld coordinates remembered so the FINAL resting position is always sent
    // (below, and by the next non-position change which is never rate-limited). Size /
    // override-redirect changes are never delayed - only pure moves.
    {
        BOOL posOnly = entry->CfgSentValid &&
            entry->LastCfgW == (int)entry->Width && entry->LastCfgH == (int)entry->Height &&
            entry->LastCfgOvr == entry->IsOverrideRedirect;
        DWORD now = GetTickCount();
        // FROZEN ANCHOR: dom0 owns this shell surface's placement (see DaemonOwnsPos).
        // Re-announcing the guest anchor would make the daemon snap its window back from
        // wherever the user dragged it. Size / override-redirect changes still go out -
        // only the position is dom0's to keep.
        if (posOnly && entry->DaemonOwnsPos)
        {
            entry->CfgPendingPos = FALSE;
            return ERROR_SUCCESS;
        }

        // INPUT-DRAG SUPPRESSION (D1 drag wobble, mirror image of the daemon-drive
        // branch below). During a guest-native drag the input handlers translate
        // against the origin frozen at button-down; that is EXACT precisely as long
        // as dom0's applied origin does not move, i.e. as long as we announce no
        // positions. Announcing mid-drag would re-open the divergent feedback loop
        // (measured saturation 40-168 px, 16-19% of announces reversing direction).
        // The withheld-coords slot keeps the flush path armed: on release the latch
        // clears and exactly one final announce carries the resting position. Size /
        // override-redirect changes still go out (a mid-drag snap escapes here), and
        // the dom0 window's content freeze is the same trade the daemon-drive hold
        // makes for dom0 WM drags.
        if (posOnly && g_InputDragFreeze && g_InputDragOriginValid && entry->Handle == g_InputDragWindow)
        {
            entry->CfgPendingPos = TRUE;
            entry->CfgPendingX = entry->X;
            entry->CfgPendingY = entry->Y;
            return ERROR_SUCCESS;
        }

        // DAEMON-DRIVE SUPPRESSION. While the daemon streams MSG_CONFIGURE for this window
        // (a dom0 WM title-bar drag), the guest window chases the dictated positions with a
        // lag of one-to-many frames. Announcing each lagging step back made the daemon move
        // its window there - fighting the WM during the drag and, after release, walking the
        // dom0 window through every queued stale position (THE drag replay, user-reproduced
        // 2026-08-12 with a 1:1 trace). dom0 knows where its own window is; say nothing
        // about position until the drive ends. The withheld-coords slot keeps the flush
        // path armed so the resting position is verified (and only announced if it differs
        // from what the daemon itself dictated). Size/override changes still go through.
        if (posOnly && entry->DaemonStreamTick != 0 &&
            (now - entry->DaemonStreamTick) < DAEMON_DRIVE_ACTIVE_MS)
        {
            entry->CfgPendingPos = TRUE;
            entry->CfgPendingX = entry->X;
            entry->CfgPendingY = entry->Y;
            return ERROR_SUCCESS;
        }
        if (posOnly && entry->CfgLastSentTick != 0 &&
            (now - entry->CfgLastSentTick) < CFG_POS_MIN_INTERVAL_MS)
        {
            entry->CfgPendingPos = TRUE;
            entry->CfgPendingX = entry->X;
            entry->CfgPendingY = entry->Y;
            return ERROR_SUCCESS; // withheld; flushed by CfgFlushPendingMove or the next change
        }
        entry->CfgLastSentTick = now;
        entry->CfgPendingPos = FALSE;
    }

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

        // Live drag servo: mirror of the record in CfgFlushPendingMove - this is the
        // path a mid-drag position announce normally takes (through the 16 ms limiter).
        if (entry->Handle == g_InputDragWindow && g_InputDragOriginValid)
            DragAnnounceRecord(entry->X, entry->Y);

        // A WM-managed shell surface (or=0 with a crop) must be DRAGGABLE but not
        // RESIZEABLE. The size-lock hint (PMinSize==PMaxSize) is sent HERE, on the
        // announce path, because the crop that identifies these as cards resolves
        // asynchronously AFTER the window is created - sending it at CREATE (as first
        // tried) always ran with zero insets and never fired. Re-sent only when the
        // announced card size actually changes.
        if (!entry->IsOverrideRedirect &&
            (entry->CropLeft || entry->CropTop || entry->CropRight || entry->CropBottom) &&
            (entry->SizeLockW != (int)entry->Width || entry->SizeLockH != (int)entry->Height))
        {
            if (SendWindowSizeLock(entry->Handle, entry->Width, entry->Height) == ERROR_SUCCESS)
            {
                entry->SizeLockW = (int)entry->Width;
                entry->SizeLockH = (int)entry->Height;
            }
        }
    }
    return status;
}

// Apply the newest daemon-dictated geometry (HandleConfigure stashes it; see DaemonMove*
// in main.h). Latest-wins with at most ONE async SetWindowPos in flight per window: a
// daemon configure flood (dom0 WM drag at input rate) used to queue dozens of async moves
// the guest window played back over seconds after release - the drag replay. Call with
// g_csWatchedWindows held.
void ApplyPendingDaemonMove(IN OUT WINDOW_DATA* entry)
{
    if (!entry->DaemonMovePending)
        return;

    // NEVER move a classified shell surface (Start/toast/search). Its content is slice-fed
    // from the composited desktop at the window's rect, and DirectComposition keeps
    // painting the card at the surface's NATURAL anchor - so a moved HWND is announced at a
    // rect containing bare wallpaper (user-reported 2026-08-12: "a peek into the underlying
    // desktop"). Slice-feed is only correct at the anchor; refusing the move keeps the
    // announced rect and the painted pixels the same region.
    if (entry->CropLeft || entry->CropTop || entry->CropRight || entry->CropBottom ||
        IsShellToastWindow(entry))
    {
        entry->DaemonMovePending = FALSE;
        LogDebug("0x%x: daemon move refused for a shell surface (slice anchor)", entry->Handle);
        return;
    }

    DWORD now = GetTickCount();

    // Is the previously posted async move still in flight? Compare the window's actual
    // GetWindowRect origin (SetWindowPos space) to the last posted target. A move-less
    // post (size-only) never gates. Timeout so a target the window can never reach (guest
    // WM snap, clamped coordinates) cannot wedge the drive.
    if (entry->DaemonPostedValid &&
        (now - entry->DaemonPostedTick) < DAEMON_MOVE_INFLIGHT_MS)
    {
        RECT wr;
        if (GetWindowRect(entry->Handle, &wr) &&
            (wr.left != entry->DaemonPostedX || wr.top != entry->DaemonPostedY))
            return; // still traveling; keep only the newest pending geometry
    }

    // Refresh the announce-space -> SetWindowPos-space delta if stale. GetRealWindowRect
    // is the expensive trio (DWM + monitor + display settings); the TTL keeps it off the
    // per-configure path, which is what killed the reverted 95492ed.
    if (!entry->DaemonOffValid || (now - entry->DaemonOffTick) >= DAEMON_OFF_TTL_MS)
    {
        RECT wr, real;
        if (GetWindowRect(entry->Handle, &wr) &&
            GetRealWindowRect(entry->Handle, &real) == ERROR_SUCCESS)
        {
            entry->DaemonOffX = real.left - wr.left;
            entry->DaemonOffY = real.top - wr.top;
            entry->DaemonOffValid = TRUE;
        }
        // On failure keep whatever we had (0/0 initially = the old behavior).
        entry->DaemonOffTick = now;
    }

    // SWP_NOACTIVATE: a dom0-driven move is geometry only - it must never change guest
    // activation. Guest foreground is driven exclusively by MSG_FOCUS (HandleFocus);
    // letting a move activate would, for a WM-managed Start, be a foreground change that
    // dismisses the menu the instant the user grabs its frame to drag it (the protocol
    // has no focus-LOSS message, so nothing else here can dismiss Start - a move is the
    // only guest-foreground-adjacent action a dom0 drag produces).
    UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE;
    if (entry->DaemonMoveNoMove)
        flags |= SWP_NOMOVE;
    if (entry->DaemonMoveNoSize)
        flags |= SWP_NOSIZE;

    // The daemon dictates in announce space (what MSG_CONFIGURE carries); SetWindowPos
    // takes GetWindowRect space. Convert, or the window lands off by the invisible-border
    // delta, the frame path announces the shifted position, and the daemon applies it as
    // a real move - the guaranteed post-drop hop. For cropped shell surfaces the announce
    // space is the visible CARD (raw origin = card origin - insets, toastcrop.c); the
    // insets are zero for every other window, so this is a no-op on the normal path.
    int tx = entry->DaemonMoveX - entry->CropLeft - entry->DaemonOffX;
    int ty = entry->DaemonMoveY - entry->CropTop - entry->DaemonOffY;

    entry->DaemonMovePending = FALSE;
    if (SetWindowPos(entry->Handle, NULL, tx, ty,
            entry->DaemonMoveW, entry->DaemonMoveH, flags))
    {
        if (!(flags & SWP_NOMOVE))
        {
            entry->DaemonPostedValid = TRUE;
            entry->DaemonPostedX = tx;
            entry->DaemonPostedY = ty;
            entry->DaemonPostedTick = now;
        }
    }
    else
    {
        win_perror("SetWindowPos(daemon move)");
    }
}

void ApplyAllPendingDaemonMoves(void)
{
    EnterCriticalSection(&g_csWatchedWindows);
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
         e != &g_WatchedWindowsList; e = e->Flink)
    {
        WINDOW_DATA* entry = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        ApplyPendingDaemonMove(entry);
    }
    LeaveCriticalSection(&g_csWatchedWindows);
}

// Does any window still owe daemon-settle work (a withheld announce, an unapplied daemon
// move, or held damage)? Decides whether the pump may sleep forever or must wake to sweep.
BOOL DaemonSettleWorkPending(void)
{
    BOOL pending = FALSE;
    EnterCriticalSection(&g_csWatchedWindows);
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
         e != &g_WatchedWindowsList && !pending; e = e->Flink)
    {
        WINDOW_DATA* entry = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        pending = entry->DaemonMovePending || entry->CfgPendingPos || entry->DaemonDamageHeld;
    }
    LeaveCriticalSection(&g_csWatchedWindows);
    return pending;
}

// Timer-driven settle: everything the frame loop would do for a window once a daemon drive
// ends, for the case where NO frame ever arrives to do it (a fully static desktop after a
// drag - review finding: the settle was frame-gated, so held damage and the resting-place
// announce could starve indefinitely). The held-damage release here sends the FULL window
// without occlusion clipping - off-frame there are no regions to clip with, and a one-shot
// overdraw beats indefinite staleness in this rare no-frames path.
void DaemonSettleSweep(void)
{
    if (g_InputDragWindow && g_InputDragLastEventTick != 0 &&
        GetTickCount() - g_InputDragLastEventTick > INPUT_DRAG_STUCK_MS)
    {
        LogWarning("input drag latch stuck on 0x%x for >%u ms with no input - disarming",
            (uint32_t)(ULONG_PTR)g_InputDragWindow, INPUT_DRAG_STUCK_MS);
        g_InputDragWindow = NULL;
        g_InputDragOriginValid = FALSE;
        DragAnnounceClear(); // the servo's history dies with the latch it belongs to
    }

    EnterCriticalSection(&g_csWatchedWindows);
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink;
         e != &g_WatchedWindowsList; e = e->Flink)
    {
        WINDOW_DATA* entry = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (entry->Synthesized || entry->DeletePending)
            continue;
        CfgFlushPendingMove(entry);
        ApplyPendingDaemonMove(entry);
        // The latched window's hold (D1) must survive this off-frame sweep too: its
        // announced origin is frozen, so a damage release here would paint the
        // window's OLD screen region - the exact mis-paint the hold prevents.
        if (g_InputDragFreeze && g_InputDragOriginValid && entry->Handle == g_InputDragWindow)
            continue;
        if (entry->DaemonDamageHeld &&
            (entry->DaemonStreamTick == 0 ||
             GetTickCount() - entry->DaemonStreamTick >= DAEMON_DRIVE_ACTIVE_MS))
        {
            entry->DaemonDamageHeld = FALSE;
            SendWindowDamageEvent(entry->Handle, 0, 0,
                (int)entry->Width, (int)entry->Height);
        }
    }
    LeaveCriticalSection(&g_csWatchedWindows);
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

    // Windows recycles HWND values, so a slot left behind here would hand this window's
    // crop to whatever unrelated popup gets the handle next.
    ToastCropEvict(entry->Handle);

    RemoveEntryList(&entry->ListEntry);

    if (entry->Handle == g_StartWindow)
        g_StartVisible = FALSE;

    // Destroyed mid-drag: Windows recycles HWND values, so a latch left behind would
    // freeze the translation origin and suppress announces for whatever unrelated
    // window gets the handle next (same hazard as the toast-crop slot above).
    if (entry->Handle == g_InputDragWindow)
    {
        g_InputDragWindow = NULL;
        g_InputDragOriginValid = FALSE;
        DragAnnounceClear(); // recycled HWND must not inherit the dead drag's history
    }

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

// Enough for every top-level window a real session has (tens); overflow degrades to
// announcing the tail in enumeration order, logged, never dropped.
#define ADD_WINDOWS_PENDING_MAX 256

typedef struct _ADD_WINDOWS_CONTEXT
{
    UINT Interrogated; // windows whose state was actually queried
    ULONG Status;
    HWND Pending[ADD_WINDOWS_PENDING_MAX]; // eligible windows in EnumWindows (top-first) order
    UINT PendingCount;
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

    // COLLECT ONLY - announced after enumeration, in REVERSE (bottom-first) order.
    //
    // EnumWindows walks top-first, and each MSG_CREATE makes X place the new window ON TOP,
    // so announcing in enumeration order hands dom0 the guest's stacking INVERTED - after any
    // mass re-announce (capture reset, resync) every window pair that overlaps is stacked
    // wrongly and the composited-framebuffer slice bleeds the wrong window's pixels
    // ("windows overlapping wrongly", adversarially verified 2026-08-12). Bottom-first
    // announce reconstructs the guest's order with X's create-on-top semantics, and owners
    // get announced before the popups they own for free (owners sit below in z-order),
    // which is also what xside.c's transient_for lookup wants.
    if (context->PendingCount < ADD_WINDOWS_PENDING_MAX)
    {
        context->Pending[context->PendingCount++] = window;
        return TRUE;
    }

    // Overflow: announce inline (top-first) rather than drop; a session with >256 eligible
    // top-level windows has bigger problems than stacking.
    LogWarning("pending-window list full (%u), announcing 0x%x in enumeration order",
        context->PendingCount, (unsigned)(ULONG_PTR)window);
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

    // Announce the collected windows BOTTOM-FIRST (see AddWindowsProc for why). A failure
    // stops the pass exactly as the inline version did.
    for (UINT i = context.PendingCount; i > 0 && status == ERROR_SUCCESS; i--)
    {
        HWND w = context.Pending[i - 1];
        if (FindWindowByHandle(w)) // examined meanwhile (taskbar path, event races)
            continue;
        status = ExamineWindow(w, &context.Interrogated);
    }

    // Keep dom0's stacking in step with the guest's by re-mapping whatever is foreground.
    //
    // dom0 and the guest are otherwise free to disagree about z-order, and when they do, the
    // window dom0 draws on top receives the pixels of whatever covers it in the guest's
    // composited framebuffer - text sliced away mid-drag, see OVERLAP-IN-MOTION.md. The agent
    // has no stacking message, but if the daemon raises a window on MSG_MAP then re-mapping
    // the foreground window is enough, and costs one message per focus change.
    //
    // Runs AFTER the announce pass on purpose: it used to run before, where on the first
    // pass after a reset the watched list was still empty, FindWindowByHandle(fg) missed,
    // and the corrective provably no-oped exactly when a whole session's stacking had just
    // been rebuilt. g_LastForeground (cleared by ResetWatch) re-arms it after every reset.
    {
        HWND fg = GetForegroundWindow();
        if (fg && fg != g_LastForeground)
        {
            WINDOW_DATA* fgData = FindWindowByHandle(fg);
            if (fgData && fgData->CreateSent && fgData->IsVisible && !fgData->IsIconic &&
                !fgData->Synthesized)
            {
                g_LastForeground = fg;
                LogInfo("foreground -> 0x%x, re-mapping to raise it in dom0", fg);
                SendWindowMap(fgData);
            }
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
    // Re-arm the foreground re-raise: everything was just destroyed, so whatever is
    // foreground must be re-raised on the first announce pass after this reset.
    g_LastForeground = NULL;
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

    // START IS DISABLED IN SEAMLESS MODE (user decision 2026-08-13, shipped state).
    // On 25H2 the Start surface has never rendered acceptably through the seamless path:
    // it parks off-screen while closed, morphs between a card-sized and a workarea-sized
    // window, and its content is a DirectComposition surface that neither PrintWindow nor
    // a framebuffer slice reproduces correctly once it is moved. Rather than ship a menu
    // that shows wallpaper or a phantom at a random position, do not present it at all -
    // the Super key is already dropped in seamless (BlockMenuKey), so the usual way to
    // summon it is closed too. Toasts are NOT affected: they render correctly and stay.
    // Re-enable with SeamlessStart=1 to work on it; see docs/PLAN-start-menu.md.
    if (g_SeamlessMode && !g_SeamlessStart && ShellSurfaceKind(data) == ShellSurfaceStart)
    {
        LogDebug("0x%x: Start surface not presented in seamless mode (SeamlessStart=0)",
            data->Handle);
        return FALSE;
    }

    // GENUINE-OPEN GATE. A shell host (StartMenuExperienceHost / ShellExperienceHost /
    // SearchHost) keeps a top-level surface alive while its menu is CLOSED. Mapping that
    // phantom announces a window with no menu inside it - dom0 then shows a slice of bare
    // desktop, at whatever rect the surface happens to report (measured: a 1201x919 window
    // full of wallpaper, and one at x=6063 on a 5120-wide screen). The card measurement is
    // the only reliable "is it actually presenting something" signal we have, so a shell
    // surface whose measurement FINISHED and found no card is not a window. In-flight
    // measurements are never rejected, so an opening menu still maps as soon as its card
    // resolves (toastcrop pokes the tracking pass when it does).
    if (ShellSurfaceCardless(data))
    {
        LogDebug("0x%x: shell surface with no card - not presenting a menu, rejecting",
            data->Handle);
        return FALSE;
    }

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
        // The IsWindow() check above does NOT close the race - DWM can drop the window between
        // it and the measurement - so demoting inside GetWindowData only moved this ERROR line
        // one frame up, at the same rate, under a different name. ERROR_INVALID_DATA belongs
        // here too: GetRealWindowRect already logged that case WITH the offending rectangle, and
        // before the fix above it never reached this line at all; it must not arrive as a flood
        // now that it does.
        if (status == (ULONG)HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
            status == ERROR_INVALID_DATA)
            LogDebug("0x%x: not measurable this pass (0x%x)", windowData->Handle, status);
        else
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

    // Insets the interrogation above already subtracted from data.X/Y/Width/Height
    // (toastcrop.c). Copied UNCONDITIONALLY, unlike the coords below: those are gated on
    // coordsChanged, while the frame-loop refresh re-applies these insets to a freshly
    // sampled RAW rect on every damaged frame. Left at zero here, the live entry would
    // un-crop itself the moment the toast is damaged, and the size flip would rebuild its
    // grant on every pass.
    windowData->CropLeft = data.CropLeft;
    windowData->CropTop = data.CropTop;
    windowData->CropRight = data.CropRight;
    windowData->CropBottom = data.CropBottom;

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
// A window whose area must be withheld from the windows STACKED BELOW it: every
// override-redirect popup (menus, tooltips, or=1 shell surfaces - on top in both the
// guest and dom0 by construction), and equally a WM-MANAGED cropped shell surface
// (TOPMOST in the guest; without this the ShellManaged flip would bleed Start's
// composited pixels into the slice-fed windows beneath it - review finding). Arbitrary
// dom0 restacking above a managed Start can still show bleed; full correctness needs the
// Phase 3 daemon-learns-z-order protocol change.
static BOOL ClaimsOcclusionArea(IN const WINDOW_DATA* entry)
{
    if (entry->IsOverrideRedirect)
        return TRUE;
    return (entry->CropLeft || entry->CropTop || entry->CropRight || entry->CropBottom) &&
        IsShellToastWindow(entry);
}

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
        if (scan->IsVisible && ClaimsOcclusionArea(scan))
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
static BOOL PwSliceCopyAndDamage(IN OUT WINDOW_DATA* entry, IN const CAPTURE_FRAME* frame,
                                 IN const BYTE* fb, IN const RECT* area)
{
    // fb is the persistently-granted desktop image (ctx->framebuffer): its address is
    // constant for the life of the duplication and the daemon reads it live, so it is
    // always current here - do NOT gate on frame->mapped, which is only TRUE on the
    // very first frame (MapDesktopSurface runs once, for the pointer to grant).
    if (!fb || frame->rect.Pitch <= 0 || !entry->PwBuffer)
        return FALSE;

    RECT screenR = { 0, 0, (LONG)min(g_ScreenWidth, g_FbWidth), (LONG)min(g_ScreenHeight, g_FbHeight) };
    RECT winR = { entry->X, entry->Y,
                  entry->X + (int)entry->PwWidth, entry->Y + (int)entry->PwHeight };
    RECT r;
    if (!IntersectRect(&r, area, &winR))
        return FALSE;
    if (!IntersectRect(&r, &r, &screenR))
        return FALSE;

    int relX = r.left - entry->X;
    int relY = r.top - entry->Y;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (relX < 0 || relY < 0 || w <= 0 || h <= 0)
        return FALSE;
    if ((ULONG)(relX + w) > entry->PwWidth || (ULONG)(relY + h) > entry->PwHeight)
        return FALSE; // buffer geometry changed underneath; next full copy repaints

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
    return TRUE;
}

// DRAG-SLICE refresh (InputDragSlice): full-rect, ROW-DIFFED copy of the dragged
// window's on-screen region out of the composited desktop into its per-window buffer.
//
// Same clipping and buffer-geometry contract as PwSliceCopyAndDamage above, with two
// deliberate differences:
//  - every row is memcmp'd before it is copied, and damage covers only the changed row
//    band. During a title-bar drag the window's OWN (window-relative) pixels are
//    identical frame to frame, so the steady-state cost is a read-only scan and ~zero
//    vchan/dom0 traffic - without the diff, a per-frame full-extent copy is the
//    measured +68% drag CPU (11.1 -> 18.6) plus a 10 MB dom0 blit at 45 Hz for the
//    2573x1013 target window.
//  - it is called EVERY processed frame while the drag-slice is engaged (no throttle).
//    Registration skew - entry->X/Y one motion step stale/fresh against the
//    framebuffer's pixels (captured live 2026-08-12: desktop sampled at (953,541) for a
//    window announced at (979,545)) - then self-corrects within one frame instead of
//    persisting: the skewed rows differ, are re-copied, and converge as soon as
//    announce and screen agree.
//
// Off-screen bands are clipped and simply keep their previous content (repaired by the
// settle recapture). Returns FALSE only when no copy could run at all (no framebuffer,
// fully off-screen, geometry changed underneath); the caller holds the last content in
// that case - it must NOT fall back to PrintWindow mid-drag, which is the app-thread
// stall this mode exists to remove.
static BOOL PwDragSliceRefresh(IN OUT WINDOW_DATA* entry, IN const CAPTURE_FRAME* frame,
                               IN const BYTE* fb)
{
    if (!fb || frame->rect.Pitch <= 0 || !entry->PwBuffer)
        return FALSE;

    RECT screenR = { 0, 0, (LONG)min(g_ScreenWidth, g_FbWidth), (LONG)min(g_ScreenHeight, g_FbHeight) };
    RECT winR = { entry->X, entry->Y,
                  entry->X + (int)entry->PwWidth, entry->Y + (int)entry->PwHeight };
    RECT r;
    if (!IntersectRect(&r, &winR, &screenR))
        return FALSE;

    int relX = r.left - entry->X;
    int relY = r.top - entry->Y;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (relX < 0 || relY < 0 || w <= 0 || h <= 0)
        return FALSE;
    if ((ULONG)(relX + w) > entry->PwWidth || (ULONG)(relY + h) > entry->PwHeight)
        return FALSE; // buffer geometry changed underneath; settle repaints

    const BYTE* src = fb + (size_t)r.top * frame->rect.Pitch + (size_t)r.left * 4;
    BYTE* dst = (BYTE*)entry->PwBuffer +
        ((size_t)relY * entry->PwWidth + (size_t)relX) * 4;
    int y0 = -1, y1 = -1;
    for (int row = 0; row < h; row++)
    {
        if (memcmp(dst, src, (size_t)w * 4) != 0)
        {
            memcpy(dst, src, (size_t)w * 4);
            if (y0 < 0)
                y0 = row;
            y1 = row;
        }
        src += frame->rect.Pitch;
        dst += (size_t)entry->PwWidth * 4;
    }

    if (y0 >= 0)
        (void)SendWindowDamageEvent(entry->Handle, relX, relY + y0, w, y1 - y0 + 1);
    return TRUE;
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
// Occlusion WITHOUT a Z-order. CollectZOrder deliberately skips its EnumWindows pass unless
// an override-redirect popup is on screen (main.c: "roughly 4x the Phase 2A drag figure"), so
// g_ZOrderValid is FALSE during any ordinary workload. The first version of this check simply
// required g_ZOrderValid and therefore refused 100% of the time: 0 skips in 5557 decisions,
// which read as "the premise is wrong" when it actually meant "the code never ran".
//
// A cheap, order-free substitute: if NO other visible window's rectangle intersects this one,
// nothing can be covering it whatever the order is. That alone is too conservative - a
// full-screen window BELOW (the shell desktop) would veto everything - so it is paired with
// "this is the foreground window", which by definition has nothing above it but topmost
// windows. Together they are sound without paying for an ordering, and they cover exactly the
// case that matters for typing and scrolling: the window the user is working in.
static BOOL PwAnyVisibleOverlap(IN const WINDOW_DATA* self, IN const RECT* rect)
{
    RECT hit;
    WINDOW_DATA* e = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (e != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        e = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (e != self && e->IsVisible && !e->IsIconic && !e->DeletePending &&
            e->Width > 0 && e->Height > 0)
        {
            RECT other = { e->X, e->Y, e->X + (int)e->Width, e->Y + (int)e->Height };
            if (IntersectRect(&hit, &other, rect))
                return TRUE;
        }
        e = (WINDOW_DATA*)e->ListEntry.Flink;
    }
    return FALSE;
}

// Occlusion gate for the DRAG-SLICE (InputDragSlice): can anything be composited ON TOP
// of the dragged window inside `rect`, making the screen an invalid source for it?
//
// PwAnyVisibleOverlap above tests ALL visible windows, including ones stacked BELOW -
// on any busy desktop that refuses constantly and would starve the drag-slice on the
// exact workload it exists for. But the dragged window is a special case: an input drag
// means the user pressed and holds Button1 on it, which activates and raises it, so the
// only surfaces that can sit ABOVE it are TOPMOST ones (taskbar, toasts, Start/shell
// popups) - normal windows it is dragged across are BELOW it and their pixels are not
// in its screen region. The z-order-based rgnCovered machinery cannot answer this
// (CollectZOrder skips its EnumWindows pass unless an override-redirect popup is on
// screen, so g_ZOrderValid is FALSE in ordinary workloads), hence this order-free test:
// only override-redirect popups (topmost by construction) and WS_EX_TOPMOST windows
// count as occluders. The dragged window's OWN synthesized children are excluded: their
// pixels belong in its buffer (the same screen source PwPatchSynthChildren uses).
static BOOL PwTopmostOverlap(IN const WINDOW_DATA* self, IN const RECT* rect)
{
    RECT hit;
    WINDOW_DATA* e = (WINDOW_DATA*)g_WatchedWindowsList.Flink;
    while (e != (WINDOW_DATA*)&g_WatchedWindowsList)
    {
        e = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (e != self && e->IsVisible && !e->IsIconic && !e->DeletePending &&
            e->Width > 0 && e->Height > 0 &&
            !(e->Synthesized && e->SynthOwner == self->Handle) &&
            (e->IsOverrideRedirect || (e->ExStyle & WS_EX_TOPMOST)))
        {
            RECT other = { e->X, e->Y, e->X + (int)e->Width, e->Y + (int)e->Height };
            if (IntersectRect(&hit, &other, rect))
                return TRUE;
        }
        e = (WINDOW_DATA*)e->ListEntry.Flink;
    }
    return FALSE;
}

// DDA-SOURCED CAPTURE (hybrid-capture-design.md S1.1, predicates E1-E6).
//
// PrintWindow re-renders the ENTIRE window - 15-18 ms on a WARP guest - even when a keystroke
// dirtied a 560x48 line. Measured against stock QWT on Windows 11 that is where the gap lives:
// typing 3.00x and scroll 2.11x worse, while drag is at parity and 98.1% of per-window
// refusals are genuine content change, so no amount of skipping can help. Stock pays a cheap
// screen crop for the same damage.
//
// When a window is unoccluded, the composited desktop already CONTAINS its pixels, so the
// damaged sub-rect can be memcpy'd out of the granted framebuffer instead - which is exactly
// what PwSliceCopyAndDamage already does for slice-fed windows. This is a routing decision,
// not new machinery.
//
// E6 ("fully unoccluded") was the design's one unanswered predicate. It is answered here by
// the same order-free test the screen-hash fast path uses, so it costs nothing extra.
// Runtime feature switches for ATTRIBUTION, checkable without elevation.
//
// The typing improvement (9.371 -> ~5.3) was measured on a build carrying BOTH frame-level
// redundant-frame dropping AND DDA-sourced capture, with frdrop firing 565-882 times per rep
// and ddacap at 94.5%. Either could have produced it, so the result could not be assigned -
// which is exactly what the project's own rule about one change per measurement exists to
// prevent.
//
// A registry DWORD alone is not enough: qrexec runs UNELEVATED on clean-room guests, and that
// is what reduced the FocusRaise A/B to zero valid points. So a marker file under
// C:\Users\Public (writable by any user) overrides the registry default at runtime, letting
// both halves be measured on ONE binary with no reinstall.
//
// The check is throttled to once a second: a GetFileAttributes per frame would be a cost of
// its own inside the very path being measured.
static BOOL MarkerPresent(IN const WCHAR* path, IN OUT DWORD* lastTick, IN OUT BOOL* cached)
{
    DWORD now = GetTickCount();
    if (*lastTick == 0 || now - *lastTick >= 1000)
    {
        *lastTick = now ? now : 1;
        *cached = (GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES);
    }
    return *cached;
}

static BOOL DdaCaptureEnabled(void)
{
    static DWORD tick = 0; static BOOL off = FALSE;
    if (!g_DdaCapture)
        return FALSE;
    return !MarkerPresent(L"C:\\Users\\Public\\qga-dda-off", &tick, &off);
}

static BOOL FrameDropEnabled(void)
{
    static DWORD tick = 0; static BOOL off = FALSE;
    if (!g_FrameDrop)
        return FALSE;
    return !MarkerPresent(L"C:\\Users\\Public\\qga-frdrop-off", &tick, &off);
}

// See REG_CONFIG_SWEEP_EXEMPT_VALUE (perf.h): keep the engine's periodic sweep off the
// window the DDA path is actively serving. Marker-file override so the exemption can be
// A/B'd on one binary, like the other attribution switches.
static BOOL SweepExemptEnabled(void)
{
    static DWORD tick = 0; static BOOL off = FALSE;
    if (!g_SweepDdaExempt)
        return FALSE;
    return !MarkerPresent(L"C:\\Users\\Public\\qga-sweepdda-off", &tick, &off);
}

// How long a window must be STILL before the composited desktop is used as its source.
// Measured: with no such guard, drag CPU went 11.106 -> 18.622 (+68%) while typing improved
// 43%. The reason is that the legacy path deliberately does almost NOTHING while a window
// moves - a drag dirties the window's whole screen extent every frame, but a pure position
// change does not alter the window's own content, so recapture is pure waste and the
// move-settle logic suppresses it. Copying that full extent out of the screen every frame
// reintroduces exactly the work that optimisation removed. Stay out of the way while moving.
#define PW_DDA_MOVE_QUIET_MS 300

static BOOL PwDdaEligible(IN const WINDOW_DATA* entry, IN const RECT* rect,
                          IN UINT fbWidth, IN UINT fbHeight, IN HWND foreground)
{
    // E3, explicit: not moving, and not just-moved. The caller's branch already excludes the
    // frames where movement is in progress, but PwSettleDue clears between LOCATIONCHANGE
    // events (about 5% of drag frames apply none), so without a quiet period the DDA path
    // still runs during a drag on those frames and pays the full-window copy.
    if (entry->PwSettleDue ||
        entry->Handle == g_InputDragWindow || // held-button drag: see g_InputDragWindow
        GetTickCount() - entry->PwLastMoveTick < PW_DDA_MOVE_QUIET_MS)
    {
        PerfNotePwRefusal(PW_REFUSE_DDA_MOVING);
        return FALSE;
    }

    // E2: the granted buffer must match the window's current size, or a dump claiming more
    // pixels than were granted makes gui-daemon exit(1).
    if (entry->PwWidth != entry->Width || entry->PwHeight != entry->Height)
    {
        PerfNotePwRefusal(PW_REFUSE_DDA_GEOMETRY);
        return FALSE;
    }

    // E4: DDA holds no pixels off-screen; an off-screen band would freeze.
    if (rect->left < 0 || rect->top < 0 ||
        rect->right > (LONG)fbWidth || rect->bottom > (LONG)fbHeight ||
        rect->right <= rect->left || rect->bottom <= rect->top)
    {
        PerfNotePwRefusal(PW_REFUSE_DDA_OFFSCREEN);
        return FALSE;
    }

    // E5: a layered window is COMPOSITED into the desktop, so the screen shows the blended
    // result while PrintWindow shows unblended content - different pixels, not a shortcut.
    if (entry->ExStyle & WS_EX_LAYERED)
    {
        PerfNotePwRefusal(PW_REFUSE_DDA_LAYERED);
        return FALSE;
    }

    // E6: unoccluded.
    //
    // NOTE ON FOREGROUND, and why this counter matters more than it looks. The guest's
    // foreground window follows DOM0's focus (MSG_FOCUS -> SetForegroundWindow). So if the
    // operator clicks another qube while a benchmark runs, the guest window stops being
    // foreground and this path refuses for the whole run. Measured: reps where DDA engaged
    // typed at 5.2-6.2 %CPU, reps where it did not at 20.4 - and the cause was invisible
    // because these refusals were not counted. Same reasoning as PwScreenUnchanged: with a valid Z-order use it,
    // otherwise "is the foreground window" plus "no other visible window overlaps".
    if (!g_ZOrderValid)
    {
        if (entry->Handle != foreground)
        {
            PerfNotePwRefusal(PW_REFUSE_DDA_NOTFG);
            return FALSE;
        }
        if (PwAnyVisibleOverlap(entry, rect))
        {
            PerfNotePwRefusal(PW_REFUSE_DDA_OVERLAP);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL PwScreenUnchanged(IN OUT WINDOW_DATA* entry, IN const BYTE* fb, IN UINT pitch,
                              IN UINT fbWidth, IN UINT fbHeight, IN const RECT* rect,
                              IN HRGN rgnCoveredAbove, IN HWND foreground)
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
    if (g_ZOrderValid)
    {
        if (RectInRegion(rgnCoveredAbove, rect))
        {
            PerfNotePwRefusal(PW_REFUSE_OCCLUDED);
            return FALSE;
        }
    }
    else
    {
        // No ordering available - use the order-free pair described above.
        if (entry->Handle != foreground)
        {
            PerfNotePwRefusal(PW_REFUSE_NOT_FOREGROUND);
            return FALSE;
        }
        if (PwAnyVisibleOverlap(entry, rect))
        {
            PerfNotePwRefusal(PW_REFUSE_OVERLAP);
            return FALSE;
        }
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

static void FrameSigInvalidate(void);   // defined with the frame-coalescing helpers below

// Drop the published desktop image. The pointer belongs to the mapped desktop surface of a
// duplication object; when that duplication is discarded the mapping goes with it, but
// synthesis reads g_FbBits from OUTSIDE the frame loop - SynthActivate() paints from the
// window-event thread - so nothing else would stop it dereferencing the stale pointer.
// Called by the capture layer under ctx->frame.lock whenever the surface is released.
// PwPatchSynthChildClipped already handles NULL by declining to paint; the child is repainted
// by the post-recovery sweep once a new frame has been published.
void PwInvalidateFramebuffer(void)
{
    // The frame signature describes pixels in THIS buffer. Once the surface is released the
    // next buffer may hold identical bytes at the same coordinates for an unrelated reason,
    // and a stale signature would authorise skipping the first real frame after recovery -
    // exactly the frame that repaints everything.
    FrameSigInvalidate();
    g_FbBits = NULL;
    g_FbPitch = 0;
    g_FbWidth = 0;
    g_FbHeight = 0;
}


// ---------------------------------------------------------------- frame-level coalescing --
// MEASURED: on a settled Windows 11 guest with a verified session, the desktop presents ~5.2
// frames/s while ABSOLUTELY IDLE, each carrying real dirty rects (empty=0, ~350k px). An
// in-guest probe sampling at 250 ms with jitter found actual pixel change in only 6 of 39
// intervals, and every changed region lay inside the one open application window. So roughly
// nine out of ten idle presents carry no pixel change at all: DWM reports composition damage
// for regions whose contents are identical.
//
// Those frames cost the whole per-frame pipeline - window walk, region arithmetic, and a
// PrintWindow per intersecting window - to deliver nothing. Hashing the damaged pixels and
// dropping the frame when they are unchanged removes that entirely.
//
// WHY THIS IS SAFE. DDA reports ALL damage, so if the union of dirty rects is byte-identical
// to the previous frame's, nothing on screen moved. Comparing CONSECUTIVE frames is what makes
// it sound: whatever content this hash describes was already delivered when the previous frame
// was processed, so there is nothing left to send.
//
// COST CONTROL. Hashing is linear in damaged area, so it is skipped above a threshold: a frame
// dirtying most of the screen is doing real work and would only be slowed by the check. Idle
// and typing damage sit far below it.
#define FRAME_HASH_MAX_AREA (1500u * 1000u)   // px; above this, do not hash - just process

static ULONGLONG g_LastFrameSig = 0;
static BOOL      g_LastFrameSigValid = FALSE;

// Called whenever the framebuffer identity changes (resolution change, re-grant, duplication
// recreated). Without this the agent could compare a hash taken from a buffer that no longer
// exists and wrongly skip the first real frame after the change.
static void FrameSigInvalidate(void)
{
    g_LastFrameSigValid = FALSE;
}

// FNV-1a over the dirty rects: their geometry AND their pixels. Geometry is included so a
// frame damaging a DIFFERENT region with coincidentally equal bytes cannot collide with it.
// Returns FALSE when the frame must not be hashed (no buffer, or too much damage).
// TRUE when this frame's damaged pixels are byte-identical to the previous frame's, so no
// window needs recapturing. Updates the stored signature as a side effect.
static BOOL FrameRedundant(IN const CAPTURE_FRAME* frame, IN const BYTE* fb, IN UINT pitch,
                           IN UINT fbWidth, IN UINT fbHeight);

static BOOL FrameSignature(IN const CAPTURE_FRAME* frame, IN const BYTE* fb, IN UINT pitch,
                           IN UINT fbWidth, IN UINT fbHeight, OUT ULONGLONG* outSig)
{
    if (!fb || pitch == 0 || frame->dirty_rects_count == 0)
        return FALSE;

    UINT64 area = 0;
    for (UINT i = 0; i < frame->dirty_rects_count; i++)
    {
        const RECT* r = &frame->dirty_rects[i];
        if (r->left < 0 || r->top < 0 || r->right > (LONG)fbWidth || r->bottom > (LONG)fbHeight ||
            r->right <= r->left || r->bottom <= r->top)
            return FALSE;                       // clipped or bogus rect: do not reason about it
        area += (UINT64)(r->right - r->left) * (UINT64)(r->bottom - r->top);
        if (area > FRAME_HASH_MAX_AREA)
            return FALSE;                       // too much damage to be worth hashing
    }

    ULONGLONG h = 1469598103934665603ULL;
    for (UINT i = 0; i < frame->dirty_rects_count; i++)
    {
        const RECT* r = &frame->dirty_rects[i];
        h ^= (ULONGLONG)(UINT)r->left;  h *= 1099511628211ULL;
        h ^= (ULONGLONG)(UINT)r->top;   h *= 1099511628211ULL;
        h ^= (ULONGLONG)(UINT)r->right; h *= 1099511628211ULL;
        h ^= (ULONGLONG)(UINT)r->bottom;h *= 1099511628211ULL;
        const UINT rowBytes = (UINT)(r->right - r->left) * 4;
        for (LONG y = r->top; y < r->bottom; y++)
        {
            const BYTE* row = fb + (SIZE_T)y * pitch + (SIZE_T)r->left * 4;
            for (UINT b = 0; b < rowBytes; b += 4)
            {
                h ^= (ULONGLONG)(*(const UINT32*)(row + b));
                h *= 1099511628211ULL;
            }
        }
    }
    *outSig = h;
    return TRUE;
}

static BOOL FrameRedundant(IN const CAPTURE_FRAME* frame, IN const BYTE* fb, IN UINT pitch,
                           IN UINT fbWidth, IN UINT fbHeight)
{
    ULONGLONG sig;
    if (!FrameSignature(frame, fb, pitch, fbWidth, fbHeight, &sig))
    {
        // Not hashable (no buffer, or too much damage): do not let a stale signature
        // authorise a later skip.
        g_LastFrameSigValid = FALSE;
        return FALSE;
    }
    if (g_LastFrameSigValid && sig == g_LastFrameSig)
        return TRUE;
    g_LastFrameSig = sig;
    g_LastFrameSigValid = TRUE;
    return FALSE;
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

    // The redundant-frame check does NOT belong here, and putting it here produced a BLACK
    // guest window. Returning early from the top of this function skips:
    //   - TrackWindows() below, which discovers windows and sends CREATE/CONFIGURE/MAP, so
    //     window management stops entirely;
    //   - in FULLSCREEN mode, the SendWindowDamageEvent(NULL, 0, 0, fbWidth, fbHeight) further
    //     down, which is the ONLY thing that tells dom0 to repaint. A static boot screen then
    //     yields identical frame after identical frame, every one skipped, and dom0 never
    //     paints anything at all.
    // It now lives in the SEAMLESS branch only, after tracking - see the FrameRedundant() call
    // there. Fullscreen is deliberately excluded: its per-frame cost is one damage message,
    // so there is nothing worth saving and everything to lose.

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
    // Once per frame, not once per window: the per-window fast path needs it for every
    // candidate and a syscall per window would eat the saving it is trying to make.
    HWND pwForeground = GetForegroundWindow();
    zCount = CollectZOrder(zSorted, RTL_NUMBER_OF(zSorted));

    // Redundant-frame drop, deliberately HERE and not at the top of the function:
    // TrackWindows() above has already run, so window discovery and CREATE/CONFIGURE/MAP are
    // unaffected, and the fullscreen path returned long before this point so its one damage
    // message per frame is untouched. Only the per-window damage/capture walk is skipped -
    // which is the expensive part and the only part that has nothing to do when no pixel
    // changed. Emptying the list rather than returning keeps the GDI regions freed and the
    // frame accounted for in QGAPERF exactly as before.
    if (FrameDropEnabled() && zCount > 0 &&
        FrameRedundant(frame, framebuffer, frame->rect.Pitch, fbWidth, fbHeight))
    {
        PerfNoteRedundantFrame();
        zCount = 0;
    }
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

                // DRAG-SLICE ELIGIBILITY (InputDragSlice), decided BEFORE the D2 block
                // below so ownership can be TRANSFERRED instead of dropped when the
                // slice takes over. While an input drag is latched on this window (or a
                // previous frame already engaged the slice), its content is served by a
                // row-diffed copy from the composited desktop (PwDragSliceRefresh)
                // instead of PrintWindow.
                //
                // WHY (measured on win11-fresh, 25H2, 5120x1440, in-guest 10 ms
                // sampler): PrintWindow(PW_RENDERFULLCONTENT) executes synchronously on
                // the dragged APP'S UI thread - the thread running the modal move loop.
                // p50 49.4 ms per call at 2.6 Mpx idle, 150-250 ms under drag-time DWM
                // contention. The press-frame recapture + the 150 ms throttled refresh
                // produced 193/211 ms COLD drag-start dead time (the cursor travelled
                // 221 px before the window first moved), 30-40 ms warm, and metronomic
                // 193-277 ms stair-steps for the whole drag - one window step per
                // PrintWindow block. The desktop framebuffer already contains the
                // dragged window's live pixels (it is foreground and on-screen by
                // definition of an input drag), so the slice keeps content live with
                // ZERO cross-process calls.
                //
                // Eligibility mirrors the DDA predicates, minus the moving/foreground
                // refusals (the drag IS the moving case; the press that latched the
                // drag also activates/raises the window):
                //  - buffer geometry must match (E2: a dump claiming more pixels than
                //    granted makes gui-daemon exit(1); a mid-drag resize rebuilds the
                //    channel and re-engages next frame);
                //  - not WS_EX_LAYERED (E5: the screen shows the blended result, not
                //    the window's own content) - those keep the throttled PrintWindow
                //    path unchanged;
                //  - no TOPMOST surface overlaps it (order-free PwTopmostOverlap; the
                //    full PwAnyVisibleOverlap would refuse on any busy desktop since
                //    windows BELOW the dragged one overlap it constantly).
                // When eligibility fails MID-drag (dragged under the taskbar/a toast),
                // the refresh is SKIPPED and the last content is held - never a
                // PrintWindow, which would reintroduce the stall - and the settle
                // recapture repairs any staleness when the drag ends.
                BOOL dragSliceReady =
                    g_InputDragSlice &&
                    (entry->PwDragSlice || entry->Handle == g_InputDragWindow) &&
                    framebuffer != NULL && frame->rect.Pitch > 0 &&
                    entry->PwWidth == entry->Width && entry->PwHeight == entry->Height &&
                    !(entry->ExStyle & WS_EX_LAYERED) &&
                    !PwTopmostOverlap(entry, &pwRect);

                if (entry->PwFrameXYValid &&
                    (entry->X != entry->PwFrameX || entry->Y != entry->PwFrameY))
                {
                    entry->PwLastMoveTick = pwNow;
                    entry->PwSettleDue = TRUE;

                    // D2 MIS-RENDER FIX. A DDA-active window that moves can get one
                    // slice copy with a one-move-step-stale entry->X/Y against the
                    // post-move screen (captured live 2026-08-12: dom0 showed the
                    // desktop sampled at (953,541) for a window announced at
                    // (979,545) - wallpaper strips left/top, right/bottom clipped).
                    // That would be a one-frame glitch, except the ddaOwned latch
                    // swallows every recapture this settle machinery fires: WcMarkDirty
                    // only stores the dirty bit, the engine defers it while ddaOwned,
                    // and ownership is dropped only inside the pwDamaged branch an
                    // idle window never re-enters - so the stale copy was the FINAL
                    // content dom0 ever received. Dropping ownership on the first
                    // observed move lets the throttled refresh and the settle
                    // recapture reach the engine; DDA mode re-establishes through
                    // WcPrefill (authoritative source) once the window is still and
                    // eligible again. Deliberately keyed on an actual position change,
                    // never on the g_InputDragWindow latch alone: a plain click on a
                    // DDA-active window must not pay a 15-65 ms re-establish.
                    //
                    // EXCEPT when the drag-slice is about to take over this frame
                    // (dragSliceReady): then ownership is TRANSFERRED, not dropped.
                    // Dropping it here releases the deferred press-frame dirty to the
                    // engine, which lands one PrintWindow on the app's UI thread at the
                    // exact moment the drag starts - a direct contributor to the
                    // measured 193-211 ms cold drag-start dead time.
                    if (g_DdaMoveInvalidate && entry->PwDdaActive && !dragSliceReady)
                    {
                        LogInfo("QGADDAMOVE hwnd=0x%x (%d,%d)->(%d,%d): dropping DDA ownership",
                            (uint32_t)(ULONG_PTR)entry->Handle,
                            entry->PwFrameX, entry->PwFrameY, entry->X, entry->Y);
                        entry->PwDdaActive = FALSE;
                        WcSetDdaOwned(entry->Handle, FALSE);
                    }
                }
                entry->PwFrameX = entry->X;
                entry->PwFrameY = entry->Y;
                entry->PwFrameXYValid = TRUE;

                // The latch keeps the suppression alive across frames where no
                // LOCATIONCHANGE landed and across a drag slower than the settle window;
                // without it those frames each pay a full PrintWindow.
                if (entry->Handle == g_InputDragWindow)
                {
                    entry->PwSettleDue = TRUE;
                    entry->PwLastMoveTick = pwNow;
                }

                if (entry->PwSettleDue &&
                    pwNow - entry->PwLastMoveTick < PW_MOVE_SETTLE_MS)
                {
                    if (g_InputDragFreezeContent && entry->Handle == g_InputDragWindow)
                    {
                        // FREEZE CONTENT WHILE THE USER DRAGS (user request 2026-08-13).
                        // Neither capture path is acceptable during a drag: PrintWindow is
                        // a synchronous cross-process render that blocks the dragged app's
                        // own message loop (measured 193/211 ms before the window even
                        // starts moving), and copying from the desktop framebuffer bakes a
                        // one-motion-step-stale edge strip into the window every frame
                        // (the moving artifacts). Sending NOTHING has neither cost: dom0
                        // keeps showing the last good bitmap - which is exactly what a
                        // remote desktop does while a window is dragged - and the settle
                        // branch below repaints once, authoritatively, when motion stops.
                        //
                        // This is only safe because the POSITION path is now clean: the
                        // same suppression shipped earlier today while the announce loop
                        // was still oscillating, and a frozen bitmap flung around by that
                        // oscillation is what the user saw as 'wobble with rendering
                        // artifacts'. The servo fixed the position side first.
                        if (entry->PwDragSlice)
                        {
                            // Was slice-feeding (knob flipped mid-drag): keep ownership,
                            // just stop refreshing - the settle below hands it back.
                            entry->PwDragSlice = FALSE;
                        }
                        if (!entry->PwDragFrozen)
                        {
                            // CLAIM THE CHANNEL. Suppressing our own recapture is not
                            // enough: the capture engine sweeps live channels on its own
                            // timer and services pending dirty marks, and each of those is
                            // a PrintWindow into the DRAGGED APP'S thread - the very stall
                            // this freeze exists to remove (measured 156-259 ms on a cold
                            // first drag, 0 ms once warm). Owning the channel for the
                            // duration is what makes the freeze actually quiet.
                            WcSetDdaOwned(entry->Handle, TRUE);
                            entry->PwDdaActive = FALSE;
                        }
                        entry->PwDragFrozen = TRUE;
                        LogDebug("QGADRAGFREEZE,ev=hold,hwnd=0x%x",
                            (uint32_t)(ULONG_PTR)entry->Handle);
                    }
                    else if (dragSliceReady)
                    {
                        // DRAG-SLICE (see the eligibility comment above). Engages on
                        // the PRESS frame itself - the g_InputDragWindow latch forces
                        // this branch from button-down on - which is exactly when the
                        // old path fired its first PrintWindow into the app thread.
                        if (!entry->PwDragSlice)
                        {
                            // Claim the buffer BEFORE the first write: from here on
                            // the engine neither sweeps nor async-captures this
                            // channel, and a pending dirty (e.g. the press-frame mark
                            // of an earlier ineligible frame) stays pending until the
                            // settle branch drops ownership. If the window was
                            // DDA-active, ownership is already held and simply carries
                            // over (the D2 drop above is skipped when dragSliceReady).
                            WcSetDdaOwned(entry->Handle, TRUE);
                            entry->PwDdaActive = FALSE; // drag-slice, not DDA steady state
                            entry->PwDragSlice = TRUE;
                        entry->PwDragHoldFrames = 0;
                            // INFO so the engage tick is visible at the guest's
                            // default LogLevel=3 - this is the timestamp the
                            // drag-start acceptance measurement correlates with the
                            // in-guest sampler.
                            LogInfo("QGADRAGSLICE,ev=engage,hwnd=0x%x",
                                (uint32_t)(ULONG_PTR)entry->Handle);
                        }
                        // Row-diffed full-rect refresh, EVERY processed frame: content
                        // stays live (changed rows are copied and damaged within one
                        // frame) and registration skew self-corrects instead of
                        // persisting. A FALSE return (fully off-screen, geometry
                        // changed underneath) holds the last content; the settle
                        // recapture repairs it.
                        (void)PwDragSliceRefresh(entry, frame, framebuffer);
                    }
                    else if (entry->PwDragSlice)
                    {
                        // Engaged, but this frame is ineligible (TOPMOST overlap, or
                        // the framebuffer went away). Hold the last content briefly -
                        // falling back to PrintWindow immediately would reintroduce the
                        // 49-250 ms app-thread stall mid-drag.
                        //
                        // BUT THE HOLD IS BOUNDED. An unbounded hold is the exact defect
                        // that shipped and was reverted today: a window whose content
                        // changes while it is dragged under a toast or Start would show
                        // dom0 a frozen bitmap for the whole overlap (review finding).
                        // Once the hold reaches the historic refresh interval, fall back
                        // to the throttled PrintWindow for this frame - byte-identical
                        // to the behaviour of the build the user has been running, so
                        // this case cannot be worse than today, only better.
                        entry->PwDragHoldFrames++;
                        if (pwNow - entry->PwLastMoveCapTick >= PW_MOVE_RECAPTURE_MS)
                        {
                            entry->PwLastMoveCapTick = pwNow;
                            entry->PwDragSlice = FALSE;   // re-engages when eligible again
                            if (entry->PwDdaActive)
                            {
                                WcSetDdaOwned(entry->Handle, FALSE);
                                entry->PwDdaActive = FALSE;
                            }
                            LogDebug("QGADRAGSLICE,ev=hold-expired,hwnd=0x%x",
                                (uint32_t)(ULONG_PTR)entry->Handle);
                            WcMarkDirty(entry->Handle);
                        }
                        else
                        {
                            LogDebug("QGADRAGSLICE,ev=hold,hwnd=0x%x",
                                (uint32_t)(ULONG_PTR)entry->Handle);
                        }
                    }
                    // Moving, no drag-slice (programmatic move, layered window,
                    // geometry mismatch, or InputDragSlice=0): historic behaviour.
                    // A stale PwLastMoveCapTick makes the throttled refresh
                    // fire on the FIRST moving frame, so one-shot programmatic moves
                    // still capture immediately, as before.
                    else if (pwNow - entry->PwLastMoveCapTick >= PW_MOVE_RECAPTURE_MS)
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
                    if (entry->PwDragFrozen)
                    {
                        // The drag froze this window's content: dom0 has been showing
                        // the pre-drag bitmap. This settle is therefore not an
                        // optimisation, it is the ONLY thing that makes the window
                        // correct again - it must fire, and it must send the whole
                        // window rather than a diff against a buffer that never moved.
                        entry->PwDragFrozen = FALSE;
                        entry->PwSliceNeedsFull = TRUE;
                        // Hand the channel back BEFORE the settle mark, or the ownership
                        // latch swallows it and dom0 keeps the pre-drag bitmap forever.
                        WcSetDdaOwned(entry->Handle, FALSE);
                        LogInfo("QGADRAGFREEZE,ev=settle,hwnd=0x%x",
                            (uint32_t)(ULONG_PTR)entry->Handle);
                    }
                    if (entry->PwDragSlice)
                    {
                        // Release the drag-slice BEFORE the settle mark, or the
                        // ddaOwned latch swallows it and the last slice copy becomes
                        // the FINAL content dom0 ever receives - the exact D2 failure
                        // mode. The one authoritative off-drag PrintWindow below also
                        // absorbs any slice-vs-PrintWindow source difference (alpha
                        // byte, Win11 rounded corners) as a one-time transition, and
                        // its row-diff sends only what actually differs. DDA steady
                        // state then re-establishes through its own WcPrefill path
                        // once the window is quiet and eligible again.
                        entry->PwDragSlice = FALSE;
                        entry->PwDdaActive = FALSE;
                        WcSetDdaOwned(entry->Handle, FALSE);
                        LogInfo("QGADRAGSLICE,ev=settle,hwnd=0x%x,holds=%u",
                            (uint32_t)(ULONG_PTR)entry->Handle, entry->PwDragHoldFrames);
                    }
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
                        // DDA source when the screen provably holds this window's pixels:
                        // copy only the damaged sub-rects instead of re-rendering the whole
                        // window. Falls back to PrintWindow the moment any predicate fails,
                        // and a periodic forced recapture bounds any pixel-equality
                        // difference between the two sources to at most PW_DDA_VERIFY_MS.
                        //
                        // NOTE: this must NOT `continue`. The rest of this loop body paints
                        // synthesized children and, critically, ORs this window into
                        // rgnCovered. Skipping that would make every window BELOW this one
                        // look unoccluded, and they would then be served screen content
                        // containing THIS window's pixels - exactly the corruption the
                        // occlusion logic exists to prevent.
                        // ESTABLISH ONCE, THEN STAY. The previous version re-captured with
                        // PrintWindow every PW_DDA_VERIFY_MS "to correct any difference"
                        // between the two pixel sources. If the sources differ at all - the
                        // design names alpha byte, Win11 rounded corners and DWM per-window
                        // effects as likely (hybrid-capture-design.md S2.4) - that turns a
                        // static difference into a periodic CONTENT SWAP. Observed: the window
                        // cycling normal -> wrong -> normal, three times, at roughly that
                        // period. A visible 0.5 Hz strobe is worse than the mismatch it was
                        // meant to hide.
                        //
                        // Now: one PrintWindow on ENTERING DDA mode establishes the buffer from
                        // the authoritative source, then screen copies until an eligibility
                        // predicate fails. Any residual source difference is a one-time
                        // transition, never a repeating flicker. Whether the sources actually
                        // differ is a separate question that must now be MEASURED, not assumed.
                        BOOL ddaHandled = FALSE;
                        if (DdaCaptureEnabled() &&
                            (g_ZOrderValid ? !RectInRegion(rgnCovered, &pwRect) : TRUE) &&
                            PwDdaEligible(entry, &pwRect, fbWidth, fbHeight, pwForeground))
                        {
                            if (!entry->PwDdaActive)
                            {
                                // Entering DDA mode: establish the buffer from the
                                // authoritative source SYNCHRONOUSLY, on this thread.
                                //
                                // WcPrefill, not WcMarkDirty. WcMarkDirty queues an ASYNC
                                // capture on the engine thread, which would then be writing
                                // entry->PwBuffer while this thread starts memcpying screen
                                // pixels into it - the buffer-ownership race the design calls
                                // "the one genuinely new race" (section 4.3). WcPrefill runs
                                // CaptureAndDiff inline, so the establish completes before any
                                // DDA copy begins, and while DDA-active nothing ever marks the
                                // window dirty - so the engine never touches the buffer at all.
                                // The race closes by construction rather than by locking.
                                //
                                // WcPrefill deliberately does not fire the damage callback, so
                                // the full-window damage is sent here.
                                entry->PwDdaActive = TRUE;
                                // Claim the buffer BEFORE the establish: from here on the
                                // engine must neither sweep nor async-capture this channel
                                // (WcPrefill below is a direct call and unaffected).
                                WcSetDdaOwned(entry->Handle, SweepExemptEnabled());
                                PerfNotePwDecision(FALSE);
                                if (WcPrefill(entry->Handle) == ERROR_SUCCESS)
                                {
                                    (void)SendWindowDamageEvent(entry->Handle, 0, 0,
                                                                entry->PwWidth, entry->PwHeight);
                                    ddaHandled = TRUE;
                                }
                                else
                                {
                                    // Could not establish - do not start copying into a buffer
                                    // whose contents are unknown; fall back to the normal path.
                                    entry->PwDdaActive = FALSE;
                                    WcSetDdaOwned(entry->Handle, FALSE);
                                }
                            }
                            else
                            {
                                BOOL copied = FALSE;
                                for (UINT ddi = 0; ddi < frame->dirty_rects_count; ddi++)
                                    if (IntersectRect(&pwHit, &frame->dirty_rects[ddi], &pwRect))
                                        if (PwSliceCopyAndDamage(entry, frame, framebuffer, &pwHit))
                                            copied = TRUE;
                                // Only "handled" if a copy actually happened. The copy declines
                                // silently on a null buffer, a geometry mismatch or an empty
                                // intersection, and treating an attempt as success would skip
                                // PrintWindow too - sending nothing at all for this window.
                                if (copied)
                                {
                                    PerfNoteDdaCapture();
                                    ddaHandled = TRUE;
                                    // Re-asserted per steady-state frame (sub-us: one SRW
                                    // shared acquire + a walk of <= a handful of channels)
                                    // so a marker-file toggle applies within a second even
                                    // with no eligibility transition, and so a channel
                                    // re-created behind our back (detach/re-attach) does
                                    // not linger sweepable while DDA-active.
                                    WcSetDdaOwned(entry->Handle, SweepExemptEnabled());
                                }
                                else
                                {
                                    entry->PwDdaActive = FALSE;   // re-establish next time
                                    WcSetDdaOwned(entry->Handle, FALSE);
                                }
                            }
                        }
                        else
                        {
                            if (entry->PwDdaActive)
                                WcSetDdaOwned(entry->Handle, FALSE);
                            entry->PwDdaActive = FALSE;   // left DDA mode; re-establish on return
                        }

                        if (!ddaHandled)
                        {
                            BOOL pwSkip = PwScreenUnchanged(entry, framebuffer, frame->rect.Pitch,
                                                            fbWidth, fbHeight, &pwRect, rgnCovered,
                                                            pwForeground);
                            // Record BOTH outcomes: the claim is a rate - captures avoided over
                            // captures considered - and skips alone cannot express one.
                            PerfNotePwDecision(pwSkip);
                            if (!pwSkip)
                                WcMarkDirty(entry->Handle);
                        }
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
            // Per-window-path windows leave the loop here, so they need the same two
            // settle steps the legacy path runs below: flush a withheld position once
            // the window is quiet, and apply the newest daemon-dictated geometry if one
            // is still waiting (see the legacy-path call sites for the full rationale).
            CfgFlushPendingMove(entry);
            ApplyPendingDaemonMove(entry);

            SetRectRgn(rgnWindow, entry->X, entry->Y,
                entry->X + (int)entry->Width, entry->Y + (int)entry->Height);
            if (g_ZOrderValid && ClaimsOcclusionArea(entry))
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
                // Writer #2 of the canonical rect, and GetRealWindowRect hands back the RAW
                // rect. A cropped toast would therefore be un-cropped here on every damaged
                // frame, oscillating against GetWindowData's cropped value: an oversized
                // MSG_CONFIGURE and a full per-window grant rebuild every pass. Re-apply the
                // insets the tracking pass already measured - entry fields only, no UIA and
                // no classification on the frame path. Zero for every window but a toast.
                fresh.left += entry->CropLeft;
                fresh.top += entry->CropTop;
                fresh.right -= entry->CropRight;
                fresh.bottom -= entry->CropBottom;

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

        // If the rate limiter withheld this window's last move, send it now that the
        // window has gone quiet - otherwise dom0 would be left one step behind the
        // window's resting place.
        CfgFlushPendingMove(entry);

        // And the mirror image: if the DAEMON's newest dictated geometry is still waiting
        // (a prior async move was in flight when it arrived), apply it now - this is the
        // guaranteed per-frame progress point that lands the window on the final dictated
        // position once the flood stops, even if the vchan goes quiet.
        ApplyPendingDaemonMove(entry);

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

        // DAEMON-DRIVE DAMAGE HOLD. While the daemon dictates this window's geometry (dom0
        // WM drag), position announces are suppressed (SendWindowConfigureIfChanged), so the
        // daemon's framebuffer read origin for this slice-fed window is frozen - damage sent
        // now would make dom0 paint pixels from the window's OLD screen region into it.
        // Hold the window's damage instead: its dom0 content freezes for the duration of the
        // drag (the same trade the g_InputDragWindow latch makes for guest-native drags, and
        // what most WMs degrade to under load), and the drive-end settle below repaints the
        // whole window once, right after CfgFlushPendingMove has announced the true resting
        // origin. Occlusion claims are unaffected - only the sends are held.
        BOOL daemonHoldDamage = FALSE;
        if (entry->DaemonStreamTick != 0 &&
            (GetTickCount() - entry->DaemonStreamTick) < DAEMON_DRIVE_ACTIVE_MS)
        {
            daemonHoldDamage = TRUE;
            entry->DaemonDamageHeld = TRUE;
        }
        else if (g_InputDragFreeze && g_InputDragOriginValid && entry->Handle == g_InputDragWindow &&
                 entry->CfgPendingPos)
        {
            // Same hold for a guest-native drag (D1): position announces for the
            // latched window are withheld, so the daemon's framebuffer read origin is
            // frozen at the pre-drag position - damage sent now would paint the
            // window's OLD screen region. Gated on CfgPendingPos: until the first
            // withheld announce the frozen origin still matches the window, and a
            // click that never moves anything must not hold damage at all.
            daemonHoldDamage = TRUE;
            entry->DaemonDamageHeld = TRUE;
        }
        else if (entry->DaemonDamageHeld)
        {
            // Drive ended: repaint what was withheld - but only the VISIBLE part.
            // A full-window send here would hand this window the pixels of anything
            // stacked above it (the occlusion bleed the region arithmetic below
            // exists to prevent; review finding). rgnVisible is already this window
            // minus the area claimed by higher windows.
            entry->DaemonDamageHeld = FALSE;
            DWORD needed = GetRegionData(rgnVisible, 0, NULL);
            if (needed > rgnDataSize)
            {
                RGNDATA* grown = (RGNDATA*)realloc(rgnData, needed);
                if (grown) { rgnData = grown; rgnDataSize = needed; }
            }
            if (needed != 0 && needed <= rgnDataSize &&
                GetRegionData(rgnVisible, rgnDataSize, rgnData) != 0)
            {
                const RECT* parts = (const RECT*)rgnData->Buffer;
                for (DWORD p = 0; p < rgnData->rdh.nCount; p++)
                {
                    status = SendWindowDamageEvent(entry->Handle,
                        parts[p].left - entry->X, parts[p].top - entry->Y,
                        parts[p].right - parts[p].left, parts[p].bottom - parts[p].top);
                    if (ERROR_SUCCESS != status)
                    {
                        win_perror2(status, "SendWindowDamageEvent (daemon-drive settle)");
                        goto cleanup;
                    }
                }
            }
            else
            {
                // Region read failed: full-window fallback - staleness would be worse.
                status = SendWindowDamageEvent(entry->Handle, 0, 0,
                    (int)entry->Width, (int)entry->Height);
                if (ERROR_SUCCESS != status)
                {
                    win_perror2(status, "SendWindowDamageEvent (daemon-drive settle)");
                    goto cleanup;
                }
            }
        }

        // skip windows that aren't in the changed area
        if (!daemonHoldDamage)
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
        if (g_ZOrderValid && ClaimsOcclusionArea(entry))
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
// Apply every message currently queued on the vchan. Factored out of the vchan case of
// WatchForEvents so the FRAME case can drain first (see the comment there: the frame event
// outranks the vchan event, so without this a slow frame lets input pile up and the backlog
// replays the user's whole drag).
//
// Returns FALSE when the caller must break out of the event loop; *exitLoop is set then.
// Screen-destroyed handling deliberately stays in the vchan case: it is a teardown decision
// for the loop, and reaching it one iteration later is harmless.
static BOOL DrainVchanInput(IN OUT struct _CAPTURE_CONTEXT* capture, OUT BOOL* exitLoop)
{
    if (!g_Vchan)
        return TRUE;

    EnterCriticalSection(&g_VchanCriticalSection);

    if (!libvchan_is_open(g_Vchan))
    {
        // KEEP-FATAL, same rule as the vchan case: the daemon is gone (case (a)).
        LogError("vchan disconnected");
        *exitLoop = TRUE;
        LeaveCriticalSection(&g_VchanCriticalSection);
        return FALSE;
    }

    BOOL screenDestroyed = FALSE;
    while (VchanGetReadBufferSize(g_Vchan) > 0)
    {
        ULONG status = HandleServerData(!g_LocalScreenDestroyed, capture, &screenDestroyed);
        if (ERROR_SUCCESS != status)
        {
            // KEEP-FATAL for the same reason as the vchan case: a partially consumed
            // body means the stream is desynced and later bytes could be parsed as
            // synthesized input.
            *exitLoop = TRUE;
            LogError("HandleServerData failed: 0x%x", status);
            LeaveCriticalSection(&g_VchanCriticalSection);
            return FALSE;
        }
    }

    // A drain batch may have carried many MSG_CONFIGUREs per window (dom0 WM drag at
    // input rate); each handler only stashed the newest geometry. Apply once per window
    // now - latest-wins - instead of the per-message async SetWindowPos flood the guest
    // window used to play back for seconds after the drag ended.
    ApplyAllPendingDaemonMoves();

    LeaveCriticalSection(&g_VchanCriticalSection);
    return TRUE;
}

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

    // FIRST-BOOT SELF-HEAL. On the FIRST boot of a freshly created AppVM this agent sits
    // here forever: the log's last line is "Awaiting for a vchan client" and no gui-daemon
    // ever attaches, so the qube runs with qrexec working and NOTHING on screen. Measured
    // 3/3 on a Windows 10 AppVM built from a Windows 10 template (2026-08-14); the second
    // and later boots of the same qube are fine, which is why it reads as "the qube starts
    // and then does nothing" rather than as a crash (forum 42717 post 56).
    //
    // The daemon is not the problem: restarting ONLY this agent, with the qube untouched,
    // makes the windows appear immediately. So the vchan server created on that first boot
    // is the thing that is dead - plausibly because the Xen devices are re-enumerated under
    // us while Windows specialises itself into a new qube identity, after we opened it.
    //
    // Exit and let the watchdog service restart us: that is exactly the recovery the
    // experiment performed, rather than a narrower guess at re-initialising the vchan alone.
    // Bounded by a persisted counter so a guest that legitimately has no daemon (gui off)
// How long the agent waits for the gui daemon to confirm MSG_DESTROY of the screen window before
// deciding the confirm is not coming, and how many times it re-sends the destroy first. While that
// confirm is outstanding NO window event is processed and no tracking pass runs, so this deadline
// is the difference between a hiccup and a qube that is up, answers qrexec, and can never show a
// window again.
#define CAPTURE_GATE_WAIT_MS 15000
#define CAPTURE_GATE_MAX_REASSERTS 2

    // cannot be restarted forever; after the budget we stay up and say so.
    ULONGLONG vchanNoClientDeadline = GetTickCount64() + VCHAN_FIRST_CLIENT_WAIT_MS;
    // Deadline for the gui-daemon to confirm the screen window's destruction, and how many times
    // the destroy has been re-sent while waiting. 0 = not waiting.
    ULONGLONG captureGateDeadline = 0;
    DWORD captureGateReasserts = 0;

    // FAULT INJECTION for the gate above (HKLM\SOFTWARE\QubesIDD!CaptureGateFaultInject, read
    // once here, dead code when unset - same convention as SoloFaultInject/ModeSnapFaultInject):
    // Bit flags, combined:
    //   1 = ignore the daemon's confirm            (the gate then never opens on its own)
    //   2 = also disable the deadline              (the PRE-FIX behaviour: frozen for ever)
    //   4 = raise ONE capture error 20 s after start, which is what opens the gate
    // So 5 = fix under test, 7 = defect re-introduced, on the same binary.
    // Bit 4 exists because the natural producer is not reliable: a resolution change does raise
    // AcquireNextFrame 0x887a0026, but on this build it was absorbed by the A7 degraded-retry path
    // (StartFrameProcessingWithRetry) and never reached the gate at all - measured 2026-08-15.
    // A test that cannot enter the state it is testing proves nothing.
    BOOL captureGateFaultFired = FALSE;
    ULONGLONG captureGateFaultDue = GetTickCount64() + 20000;
    DWORD captureGateFault = 0, cbCgf = sizeof(captureGateFault), typeCgf = 0;
    if (ERROR_SUCCESS != RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\QubesIDD",
                                      L"CaptureGateFaultInject", RRF_RT_REG_DWORD, &typeCgf,
                                      &captureGateFault, &cbCgf))
        captureGateFault = 0;
    if (captureGateFault)
        LogWarning("CAPTUREGATE CaptureGateFaultInject=%lu - the gui daemon's screen-destroy "
            L"confirm will be IGNORED on purpose%s", captureGateFault,
            (captureGateFault == 2) ? L" and the deadline is DISABLED (pre-fix behaviour)" : L"");
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

        // FAULT INJECTION (test builds only, rank 1). Before the wait, not after it: the
        // fault must reproduce a pump that services NOTHING - not the vchan fd, not frames,
        // not window events - which is the agent side of the proven H2 causality. Waiting
        // first would make the stall depend on something happening to wake the loop.
        {
            DWORD fiStallMs = FiPumpStallMs();
            if (fiStallMs != 0)
                Sleep(fiStallMs);
        }

        vchanIoInProgress = TRUE;

        // A7: while degraded, wake in time for the next capture-init retry; the
        // vchan/input events below are still serviced normally in between.
        DWORD waitTimeout = INFINITE;
        if (captureDegraded)
        {
            ULONGLONG now64 = GetTickCount64();
            waitTimeout = (captureRetryDue > now64) ? (DWORD)(captureRetryDue - now64) : 0;
        }

        // [CaptureGateFaultInject bit 4] Raise one capture error, once, to open the gate.
        if ((captureGateFault & 4) && !captureGateFaultFired &&
            GetTickCount64() > captureGateFaultDue && g_VchanClientConnected)
        {
            captureGateFaultFired = TRUE;
            LogWarning("CAPTUREGATE CaptureGateFaultInject: raising a capture error on purpose to "
                L"open the gate");
            SetEvent(captureErrorEvent);
        }

        // CAPTURE GATE DEADLINE. While g_LocalScreenDestroyed is set the agent discards every
        // window event and runs no tracking pass, so if the daemon's MSG_DESTROY(0) confirm never
        // arrives the qube keeps whatever dom0 already had and NO NEW WINDOW CAN EVER APPEAR -
        // apps start and are never announced, which is exactly the "qube is up, qrexec fine,
        // nothing usable on screen" report. Wake up in time to notice.
        if (g_LocalScreenDestroyed && captureGateDeadline != 0)
        {
            ULONGLONG now64 = GetTickCount64();
            DWORD toGate = (captureGateDeadline > now64) ? (DWORD)(captureGateDeadline - now64) : 0;
            // Compare as DWORD: /W4 with warnings-as-errors rejects the mixed-sign compare.
            if (waitTimeout == INFINITE || toGate < (DWORD)waitTimeout)
                waitTimeout = toGate;
        }

        // Daemon-settle work must not depend on another frame or vchan message ever
        // arriving (a static desktop after a drag produces neither): bound the wait so
        // the WAIT_TIMEOUT sweep below can flush the resting announce, apply the last
        // dictated move, and release held damage.
        if (waitTimeout == INFINITE && g_VchanClientConnected && DaemonSettleWorkPending())
            waitTimeout = 100;

        // DRAG SMOOTHNESS. While the user drags a window, its POSITION is the only thing
        // that matters (its content is frozen for the duration), yet g_WindowEventSignal
        // sits LAST in the wait array so a pending frame always wins. Announces are then
        // quantised to the capture-frame clock - measured ~46 ms at 5120x1440, i.e. dom0
        // receives the window in 40-80 px steps, which is exactly the "jumpy" motion the
        // user reports. Drain pending window events FIRST while the latch is armed, so
        // announces flow at input rate (~45 Hz) instead of frame rate (~21 Hz).
        // This cannot starve frames: ProcessWindowEvents drains the queue and returns,
        // the frame event stays signalled, and the wait below services it immediately.
        if (g_DragEventPriority && g_InputDragWindow &&
            g_VchanClientConnected && g_SeamlessMode && !g_LocalScreenDestroyed)
        {
            // Input FIRST. The mouse motion that drives the app's own modal move loop
            // arrives on the vchan, which the pump only drains on its own event or at the
            // top of a frame - so during a drag the motion is delivered in clumps at frame
            // cadence and the application moves its window ONCE PER CLUMP. Measured in the
            // guest on the previous build: the window's own rect advanced only every
            // 54-70 ms in 12-68 px hops (~16 Hz) while dom0 was sending motion far faster.
            // That is the jumpiness; announcing more often cannot fix it, because the
            // window genuinely is not moving in between.
            if (VchanGetReadBufferSize(g_Vchan) > 0 && !DrainVchanInput(capture, &exitLoop))
                break;
            // Then the position announce for whatever that input just moved.
            if (WaitForSingleObject(g_WindowEventSignal, 0) == WAIT_OBJECT_0)
                ProcessWindowEvents();
        }

        // THE GATE NEVER OPENED. Two escalations, in order, because the cheap one is also the
        // likely one: the destroy may simply have been lost (a ring that was full when it was
        // sent, a daemon that restarted mid-exchange), so re-send it before concluding anything.
        // Deliberately NOT tearing capture down here: dom0 may still have the framebuffer mapped
        // and revoking it early is the one thing this gate exists to prevent.
        if (g_LocalScreenDestroyed && captureGateDeadline != 0 && !(captureGateFault & 2) &&
            GetTickCount64() > captureGateDeadline && g_VchanClientConnected)
        {
            if (captureGateReasserts < CAPTURE_GATE_MAX_REASSERTS)
            {
                captureGateReasserts++;
                LogWarning("CAPTUREGATE no confirm from the gui daemon in %lu ms - re-sending the "
                    L"screen destroy (attempt %lu of %lu). Until this clears, no window can be "
                    L"announced to dom0.", CAPTURE_GATE_WAIT_MS, captureGateReasserts,
                    (DWORD)CAPTURE_GATE_MAX_REASSERTS);
                EnterCriticalSection(&g_VchanCriticalSection);
                (void)SendWindowUnmap(NULL);
                (void)SendWindowDestroy(NULL);
                LeaveCriticalSection(&g_VchanCriticalSection);
                captureGateDeadline = GetTickCount64() + CAPTURE_GATE_WAIT_MS;
            }
            else
            {
                // Out of cheap options. A fresh agent re-runs the handshake, re-creates window 0
                // and re-announces every window it can see, which is the only remaining way to
                // get the qube its GUI back - and it is exactly what a user does by hand when
                // they restart a qube that "shows nothing". The watchdog backs off if this
                // repeats, so a permanently wedged daemon cannot turn into a restart storm.
                LogError("CAPTUREGATE the gui daemon never confirmed the screen destroy after %lu "
                    L"re-sends - window tracking has been frozen for %lu ms and nothing can reach "
                    L"dom0. Exiting so the watchdog respawns the agent and the session is rebuilt.",
                    (DWORD)CAPTURE_GATE_MAX_REASSERTS,
                    (DWORD)(CAPTURE_GATE_WAIT_MS * (CAPTURE_GATE_MAX_REASSERTS + 1)));
                status = ERROR_TIMEOUT;
                exitLoop = TRUE;
                break;
            }
        }

        // No daemon has EVER attached and the grace period is over: restart (see the
        // FIRST-BOOT SELF-HEAL note above). Checked before the wait so a quiet agent that
        // nothing wakes still gets here - waitTimeout is bounded.
        if (!g_VchanClientConnected && vchanNoClientDeadline != 0 &&
            GetTickCount64() > vchanNoClientDeadline)
        {
            DWORD restarts = 0;
            (void)CfgReadDword(NULL, REG_CONFIG_VCHAN_RESTARTS_VALUE, &restarts, NULL);
            if (restarts < VCHAN_FIRST_CLIENT_MAX_RESTARTS)
            {
                (void)CfgWriteDword(NULL, REG_CONFIG_VCHAN_RESTARTS_VALUE, restarts + 1, NULL);
                // Two calls, not a ternary: the log macros paste an L onto the format literal,
                // so a conditional expression there does not compile.
                DWORD hadClientEarly = 0;
                (void)CfgReadDword(NULL, REG_CONFIG_HAD_CLIENT_VALUE, &hadClientEarly, NULL);
                if (hadClientEarly)
                    LogError("no gui-daemon client in %lu ms - exiting so the watchdog respawns the agent (attempt %lu of %lu). This guest HAS had a daemon before, so this is a LOST session, not a first boot: dom0's gui-daemon for this qube most likely exited.",
                        VCHAN_FIRST_CLIENT_WAIT_MS, restarts + 1, (DWORD)VCHAN_FIRST_CLIENT_MAX_RESTARTS);
                else
                    LogError("no gui-daemon client in %lu ms and none ever connected - exiting so the watchdog respawns the agent (attempt %lu of %lu). This is the first-boot AppVM case: the qube has qrexec but no windows.",
                        VCHAN_FIRST_CLIENT_WAIT_MS, restarts + 1, (DWORD)VCHAN_FIRST_CLIENT_MAX_RESTARTS);
                status = ERROR_TIMEOUT;
                exitLoop = TRUE;
                break;
            }
            DWORD hadClient = 0;
            (void)CfgReadDword(NULL, REG_CONFIG_HAD_CLIENT_VALUE, &hadClient, NULL);
            if (hadClient)
                LogError("no gui-daemon client in %lu ms after %lu restarts, but this guest HAS had one before - dom0's gui-daemon for this qube is gone and is not coming back on its own. The qube keeps running (qrexec works) and will show no windows until it is restarted.",
                    VCHAN_FIRST_CLIENT_WAIT_MS, restarts);
            else
                LogError("no gui-daemon client in %lu ms after %lu restarts and none ever connected - staying up without one. If this qube is meant to have a GUI, check that its gui feature is set and that a gui-daemon runs for it in dom0.",
                    VCHAN_FIRST_CLIENT_WAIT_MS, restarts);
            vchanNoClientDeadline = 0; // said once; do not spin
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

        // No event fired within the bounded wait: run the settle sweep armed above (and
        // fall through - the capture-retry logic after the switch also keys on timeouts).
        if (WAIT_TIMEOUT == signaledEvent && g_VchanClientConnected)
            DaemonSettleSweep();

        switch (signaledEvent)
        {
        case 1: // new frame available
            LogVerbose("new frame");

            // APPLY QUEUED INPUT BEFORE THE FRAME. This one loop runs both the frame
            // work and the vchan drain, and WaitForMultipleObjects prefers the lower
            // index - so the frame (1) always wins over vchan (4). A slow frame (a
            // PrintWindow recapture on this GPU-less guest is 15-18 ms, and damage
            // frames of 45 ms were measured) therefore applies NO motion while it runs;
            // dom0 keeps forwarding MSG_MOTION into the ring, and after the user
            // releases, the whole backlog is drained in one burst - re-injecting the
            // entire stale drag trajectory through SendInput, which Windows performs as
            // the window JUMPING BACK AND REPLAYING THE DRAG (user-reported 2026-08-12;
            // root-caused to this serialization, not to any coordinate/echo bug).
            //
            // Draining here bounds input latency to one frame instead of one drag.
            // Safe: HandleServerData already runs only on this thread, the vchan lock is
            // released before ProcessNewFrame takes g_csWatchedWindows (no inversion),
            // and input injection touches neither capture content nor the geometry/CREATE
            // contract. FIFO order is preserved (single thread, in-order drain).
            if (g_VchanClientConnected && !DrainVchanInput(capture, &exitLoop))
                break;

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
                // KEEP-FATAL: this flag is also cleared by the bounded send (vchan.c) when
                // it gives up, and that happens while the vchan itself can still be OPEN.
                // Without this test the give-up would look exactly like "a new client
                // connected" and re-run the handshake: a protocol version re-sent
                // mid-stream to a daemon that is not expecting one, then a blocking wait
                // for a version reply that will never come. That desync is strictly worse
                // than the wedge that preceded it, and unlike the wedge it is silent.
                // Exit instead and let the service respawn do the reconnect - a fresh
                // agent is the only state from which the handshake is well defined.
                if (VchanSendWedged())
                {
                    LogError("vchan send gave up on this connection (see VCHANWEDGE) - "
                        "refusing to re-run the handshake, exiting for a clean respawn");
                    exitLoop = TRUE;
                    break;
                }

                vchanIoInProgress = FALSE;
                libvchan_cleanup(g_Vchan); // needed to cleanup xenstore entry

                LogInfo("A vchan client has connected");

                // needs to be set before enumerating windows so maps get sent
                // (and before sending anything really)
                g_VchanClientConnected = TRUE;
                // A daemon attached, so the first-boot restart budget has done its job (or
                // was never needed): clear it, or a later genuine first-boot failure on this
                // guest would start with the budget already spent.
                vchanNoClientDeadline = 0;
                (void)CfgWriteDword(NULL, REG_CONFIG_VCHAN_RESTARTS_VALUE, 0, NULL);

                // Remember that this guest HAS had a daemon. A respawned agent cannot otherwise
                // tell "this qube never had a GUI" from "its GUI died a minute ago", and it
                // currently reports the first in both cases - which sends the reader looking for a
                // missing gui feature when the real answer is that dom0's gui-daemon for this qube
                // exited and did not restart.
                (void)CfgWriteDword(NULL, REG_CONFIG_HAD_CLIENT_VALUE, 1, NULL);

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
            // Same latest-wins apply as DrainVchanInput's drain: this is the OTHER
            // steady-state drain (vchan event with no frame pending), and without the
            // apply here a configure batch arriving frameless would wait for the next
            // frame or the 100ms settle sweep (review finding).
            if (ERROR_SUCCESS == status)
                ApplyAllPendingDaemonMoves();
            LeaveCriticalSection(&g_VchanCriticalSection);

            if (screenDestroyed && captureGateFault)
            {
                LogWarning("CAPTUREGATE confirm received and DISCARDED (CaptureGateFaultInject=%lu)",
                    captureGateFault);
                screenDestroyed = FALSE;
            }

            if (screenDestroyed)
            {
                // Both edges of this state were below the default log level, so a shipped
                // guest recorded NOTHING when capture stopped or restarted - the one
                // question a "my qube is frozen" report needs answered.
                LogInfo("CAPTUREGATE gui daemon confirms screen destruction - restarting capture");
                captureGateDeadline = 0;
                captureGateReasserts = 0;
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
            LogWarning("CAPTUREGATE capture error - screen window destroyed, waiting for the "
                L"gui-daemon confirm before capture can restart");

            // NEVEREXIT: a stale error event from a torn-down capture generation can
            // fire while degraded (capture == NULL); StopFrameProcessing dereferences
            // *capture. Nothing to stop in that case.
            if (capture)
                StopFrameProcessing(&capture);
            // CaptureTeardown() is delayed until we receive confirming MSG_DESTROY for 0x0 from gui daemon
            // revoking framebuffer access before that is unsafe
            captureGateDeadline = GetTickCount64() + CAPTURE_GATE_WAIT_MS;
            captureGateReasserts = 0;
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
    // ...and "daemon gone" must not include "daemon merely unresponsive". The bounded send
    // (vchan.c) clears g_VchanClientConnected for a daemon that stopped draining the ring,
    // but such a daemon is still RUNNING and still mapping our grants - exactly the live
    // mapper this branch must never revoke under. Only the vchan actually being closed, or
    // a give-up that reported the peer DEAD, count as gone.
    // Two ways to be "alive but not draining": the send layer is in its degraded state
    // (connection kept, sends failing fast - the usual case now), or it gave up entirely
    // but reported UNRESPONSIVE rather than DEAD. Both mean a live mapper.
    BOOL daemonUnresponsive = VchanSendDegraded() ||
        (VchanSendWedged() && VchanWedgeResult() == VCHAN_SEND_UNRESPONSIVE);
    if ((!g_VchanClientConnected && !daemonUnresponsive) || !libvchan_is_open(g_Vchan))
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
    // Same place and same reason as PerfInit: the switches must be resolved and logged
    // before anything can consult them, and before any window or frame work starts.
    // Compiles to nothing unless the build was made with -p:QgaFaultInjection=1.
    FiInit();
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
    // WAITING variant: the agent is started very early by the watchdog service, routinely
    // before the IddCx monitor has arrived, and the old one-shot call treated that race as
    // a permanent failure. 20 s is generous against a measured arrival of a few seconds and
    // costs nothing on a guest with no IDD, which returns immediately.
    if (ERROR_SUCCESS != EnsureQubesIddSoloWaiting(20000))
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
