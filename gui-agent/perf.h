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

/*
 * Optional per-frame performance instrumentation for the seamless capture path.
 *
 * It answers three questions that decide where the seamless-mode latency goes:
 *   (a) how much of a frame is spent tracking windows (UpdateWindowData + EnumWindows),
 *   (b) how much is spent extracting/intersecting damage,
 *   (c) how much is spent writing messages to the vchan,
 * and it records whether IDXGIOutputDuplication::GetFrameMoveRects ever returns
 * anything (the open TODO in capture.c).
 *
 * Everything here is inert when disabled: the only cost on the hot path is a
 * predictable test of a global BOOL. Output goes to the regular agent log
 * (which already rotates: one file per process start, older than LogRetention
 * seconds purged at startup), one CSV-ish line per emit, at LOG_LEVEL_INFO so
 * the default LogLevel=3 does not need to be raised (raising it to DEBUG or
 * VERBOSE would itself dominate what we are trying to measure).
 */

#pragma once
#include <windows.h>

// Compile-time default for the master switch, used when neither the registry
// value nor the environment variable is present.
// 1 for the Qubes display-performance measurement builds, 0 for upstream.
#ifndef QGA_PERF_DEFAULT
#define QGA_PERF_DEFAULT 1
#endif

// Registry values, read from HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent
// (falling back to the parent key, like every other config value in the agent).
#define REG_CONFIG_PERF_VALUE       L"PerfLog"      // DWORD 0/1
#define REG_CONFIG_PERF_EVERY_VALUE L"PerfEveryN"   // DWORD >= 1, frames aggregated per log line

// Environment override (wins over the registry): QUBES_GUI_PERF=0|1
#define PERF_ENV_VALUE L"QUBES_GUI_PERF"
#define PROTO_ENV_VALUE L"QUBES_GUI_PROTO"
#define REG_CONFIG_PROTO_VALUE L"ProtoTrace"
#define REG_CONFIG_PROTO_WOBBLE_VALUE L"ProtoTraceWobble"
#define REG_CONFIG_BUTTON_ABS_VALUE L"ButtonAbsolute"
#define REG_CONFIG_SHELL_MANAGED_VALUE L"ShellManaged"
#define REG_CONFIG_BLOCK_MENU_KEY_VALUE L"BlockMenuKey"
// Z-order sync experiment: raise the window dom0 focused (see HandleFocus). DWORD 0/1.
#define REG_CONFIG_FOCUS_RAISE_VALUE L"FocusRaise"
// Attribution switches. The typing improvement was measured on a build carrying BOTH
// frame-level redundant-frame dropping AND DDA-sourced capture, so it could not be assigned
// to either. These allow each to be disabled independently.
#define REG_CONFIG_DDA_CAPTURE_VALUE L"DdaCapture"     // DWORD 0/1, default 1
#define REG_CONFIG_FRAME_DROP_VALUE  L"FrameDrop"      // DWORD 0/1, default 0 - see below
// Exempt DDA-active windows from the wincapture engine's 250ms round-robin sweep.
// The sweep exists so guest-occluded windows (invisible to DDA) still converge, but a
// DDA-active window is by definition foreground and unoccluded - sweeping it runs a full
// PrintWindow + DIB alloc + whole-buffer row-diff 4x/s for a window whose every real
// change the DDA path already serves. Measured as the fork's ~2.5-point idle CPU floor
// vs stock (idle 3.04% vs 0.57%; the whole typing gap, since the typing increment over
// idle is equal on both sides). The sweep also violates the ESTABLISH-ONCE invariant
// ("while DDA-active nothing ever marks the window dirty") - it re-opens the
// buffer-ownership race and can re-introduce the 4 Hz content swap that rework removed.
#define REG_CONFIG_SWEEP_EXEMPT_VALUE L"SweepDdaExempt" // DWORD 0/1, default 1

// Version of the CSV record format, bumped when fields change.
// v2 added iwn/wev (window tracking became event driven, see PHASE2A-NOTES.md).
#define PERF_RECORD_VERSION 7

extern BOOL     g_PerfEnabled;  // master switch

// Protocol trace: records what the agent actually TELLS THE DAEMON, as opposed to what the
// guest state implies it should. Every acceptance failure so far came from checking guest
// state or agent intent and inferring the wire content - e.g. concluding menus were sent
// override_redirect from their WS_POPUP style, while dom0 drew a decoration on them. The
// only ground truth for a protocol defect is the field that went out.
// Off by default; registry DWORD "ProtoTrace" or env QUBES_GUI_PROTO=1.
extern BOOL     g_ProtoTrace;

// Per-damage-rect live-rect probe on top of ProtoTrace (DWM query + lock per rect - a
// measured drag-time tail hazard, so diagnostic-only). Registry DWORD "ProtoTraceWobble".
extern BOOL     g_ProtoTraceWobble;

// Button events carry their own absolute position (default ON; "ButtonAbsolute"=0 restores
// the historic click-at-last-motion semantics).
extern BOOL     g_ButtonAbsolute;

// Announce classified shell surfaces (toasts/Start/Search) as WM-managed windows instead
// of override-redirect, so the dom0 WM can frame and move them. Three-valued policy
// ("ShellManaged"): 0 = none (every shell surface stays an override-redirect popup,
// Linux-agent parity), 1 = all classified surfaces, 2 = START ONLY (the GWeck goal state,
// default: Start is movable + size-locked, toasts stay corner popups per the 2026-08-12
// user spec).
#define SHELL_MANAGED_NONE  0
#define SHELL_MANAGED_ALL   1
#define SHELL_MANAGED_START 2
extern DWORD    g_ShellManaged;

// Drop the forwarded Super/Windows key (and whole Mod4 chords) in seamless mode
// ("BlockMenuKey", default on): dom0 owns that key, a stray forward pops the guest Start
// over the seamless desktop (GWeck #44), and the sanctioned way to open Start is the
// dom0 appmenu shortcut, which injects guest-side and never crosses this filter.
extern BOOL     g_BlockMenuKey;

// Raise on MSG_FOCUS, making guest z-order agree with dom0 for the focused window.
// Off by default = historic behaviour. Read once in PerfInit().
extern BOOL     g_FocusRaise;

// Feature switches for attribution. Registry sets the default; a MARKER FILE overrides it at
// runtime without elevation - which matters because qrexec runs unelevated on clean-room
// guests, and that is exactly what reduced the FocusRaise A/B to zero valid points.
//   C:\Users\Public\qga-dda-off     present -> DDA-sourced capture disabled
//   C:\Users\Public\qga-frdrop-off  present -> redundant-frame dropping disabled
// Public is writable by any user, and these only affect the guest's own capture path - no
// isolation property depends on them.
// MEASURED AND TURNED OFF. Four-condition run, one binary, interleaved:
//     baseline (both off)   typing 11.508  drag 14.541  scroll 10.041
//     frame-drop only       typing 14.765 (+28%)  drag 20.051 (+38%)  scroll 10.159 (+1%)
//     DDA only              typing  4.089 (-64%)  drag 13.498 ( -7%)  scroll  4.611 (-54%)
//     both                  typing  5.313 (-54%)  drag 16.408 (+13%)  scroll  7.971 (-21%)
// Frame dropping is a NET LOSS under workload and it degrades DDA when combined - "both" is
// worse than "DDA only" on every metric. The reason is plain once measured: FrameSignature
// hashes the damaged pixels every frame, and during a workload those pixels really did change,
// so the hash almost never matches. It is ~440k pixels hashed per frame for nothing.
// It only pays at IDLE, where ~90% of presents carry no pixel change - and idle is the row
// worth 0.343 %CPU. Default OFF; the code and counters stay so the idle case can be revisited
// deliberately rather than re-derived.
//   C:\Users\Public\qga-sweepdda-off  present -> DDA-active sweep exemption disabled
extern BOOL     g_DdaCapture;
extern BOOL     g_FrameDrop;
extern BOOL     g_SweepDdaExempt;
extern LONGLONG g_PerfFreq;     // QueryPerformanceFrequency, ticks per second
extern DWORD    g_PerfEveryN;   // emit one line per this many processed frames

// Time spent inside vchan sends, per thread (so the capture thread and the
// resolution-change thread can't corrupt the main loop's accounting).
// Maintained by VchanSendTimed(), sampled by ProcessNewFrame().
extern __declspec(thread) LONGLONG g_PerfSendTicks;
extern __declspec(thread) LONG     g_PerfSendCount;

// Per-frame numbers produced by the capture thread and consumed by the main
// loop. Written under CAPTURE_FRAME.lock while the main loop is blocked on
// frame_event, read by the main loop while the capture thread is blocked on
// ready_event, so no extra synchronization is needed.
typedef struct _PERF_CAPTURE
{
    LONGLONG acquire_ticks;     // AcquireNextFrame() - mostly idle wait, reported but not part of the total
    LONGLONG moverect_ticks;    // GetFrameMoveRects()
    LONGLONG dirtyrect_ticks;   // GetFrameDirtyRects() (size query + fetch + malloc)
    LONGLONG signal_qpc;        // QPC sampled right before SetEvent(frame_event)
    UINT     move_rects_count;  // move rects reported for this frame ((UINT)-1 == query failed)
    UINT64   dirty_area;        // sum of dirty rect areas, pixels
} PERF_CAPTURE;

// Read config, calibrate QPC, log the banner + CSV header. Call once from Init().
void PerfInit(void);

// Capture thread: a frame arrived with no dirty rects and was dropped.
void PerfNoteSkippedFrame(void);

// Main loop: one per-window recapture DECISION taken on the screen-hash fast path.
// skipped == TRUE  -> the window's screen bytes were provably unchanged, no PrintWindow
// skipped == FALSE -> the bytes moved (or the guard refused), so the window was recaptured
//
// Both halves are required. The fix's headline claim is a RATE - captures avoided over
// captures considered - and a counter that records only the skips cannot express it: it
// grows with workload length and says nothing about how often the path applied. An earlier
// revision incremented a file-static that was never read by anything, which made the effect
// unmeasurable and its acceptance criterion unable to pass.
void PerfNotePwDecision(IN BOOL skipped);

// Why a per-window fast-path decision refused to skip. A bare hit rate of 0 % cannot
// distinguish a guard that always refuses (a defect in the check) from content that genuinely
// changes every frame (which would falsify the premise that the extra presents are redundant).
// Those call for opposite responses, so the cause is counted rather than inferred.
typedef enum _PW_REFUSE_REASON
{
    PW_REFUSE_NO_FB = 0,        // no framebuffer / zero pitch - cannot look at the screen
    PW_REFUSE_NO_ZORDER,        // Z-order unknown - cannot reason about occlusion
    PW_REFUSE_OFFSCREEN,        // window not wholly inside the framebuffer
    PW_REFUSE_OCCLUDED,         // something is stacked above it; screen is not its content
    PW_REFUSE_NOT_FOREGROUND,   // Z-order unknown and this is not the foreground window
    PW_REFUSE_OVERLAP,          // Z-order unknown and another visible window overlaps it
    PW_REFUSE_FIRST_SEEN,       // no previous hash yet - unavoidable once per window
    PW_REFUSE_CONTENT_CHANGED,  // hashed and DIFFERENT: a genuine repaint, not a redundant one
    // DDA-path refusals. Separate from the screen-hash refusals above because they are a
    // DIFFERENT decision: those say "the pixels did not change", these say "the composited
    // desktop is not a valid source for this window right now". Uninstrumented, a run where
    // DDA never engaged looked identical to one where it was never eligible - and the
    // difference was 5.3 vs 20.4 %CPU on typing.
    PW_REFUSE_DDA_MOVING,       // moving, or moved within PW_DDA_MOVE_QUIET_MS
    PW_REFUSE_DDA_GEOMETRY,     // granted buffer size != current window size
    PW_REFUSE_DDA_OFFSCREEN,    // window not wholly inside the framebuffer
    PW_REFUSE_DDA_LAYERED,      // WS_EX_LAYERED: screen shows the blended result
    PW_REFUSE_DDA_NOTFG,        // not the foreground window - see note below
    PW_REFUSE_DDA_OVERLAP,      // another visible window overlaps it
    PW_REFUSE_MAX
} PW_REFUSE_REASON;

void PerfNotePwRefusal(IN PW_REFUSE_REASON reason);

// Main loop: a whole frame was dropped because its damaged pixels were byte-identical to the
// previous frame's. Distinct from PerfNoteSkippedFrame, which counts frames that arrived with
// NO dirty rects: this one had damage reported and none of it was real.
void PerfNoteRedundantFrame(void);

// Main loop: a window's damage was served by copying the damaged sub-rects out of the
// composited desktop instead of re-rendering the whole window with PrintWindow. The ratio of
// this to pwcap is what the typing/scroll gap against stock turns on.
void PerfNoteDdaCapture(void);

// Capture thread: GetFrameMoveRects returned a non-empty set. Logs the first
// occurrence loudly (this is the answer to the capture.c move-rects TODO),
// afterwards only keeps the running maximum.
void PerfNoteMoveRects(IN UINT count, IN LONG srcX, IN LONG srcY, IN const RECT* dst);

// Main loop: account one processed frame and, every g_PerfEveryN frames, emit the record.
// All *_ticks are QPC deltas with nested send time already subtracted (except send_ticks).
void PerfEmitFrame(
    IN BOOL seamless,
    IN LONGLONG frame_start_qpc,
    IN LONGLONG total_ticks,
    IN LONGLONG update_ticks,
    IN LONGLONG enum_ticks,
    IN LONGLONG remove_ticks,
    IN LONGLONG damage_ticks,
    IN LONGLONG send_ticks,
    IN LONG send_count,
    IN const PERF_CAPTURE* cap,
    IN UINT dirty_rects,
    IN UINT window_count,
    IN UINT interrogated,   // windows whose state was actually queried this frame
    IN UINT window_events); // window events applied this frame

// Returns 0 when instrumentation is disabled, so all the deltas computed from it are 0.
static __forceinline LONGLONG PerfNow(void)
{
    LARGE_INTEGER li;

    if (!g_PerfEnabled)
        return 0;

    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// QPC ticks -> microseconds
static __forceinline LONGLONG PerfUs(IN LONGLONG ticks)
{
    return g_PerfFreq ? (ticks * 1000000LL) / g_PerfFreq : 0;
}
