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

#pragma once
#include <windef.h>

#include <list.h>

extern BOOL g_UseDirtyBits;
extern BOOL g_SeamlessMode;
// dom0's window-0 position as last reported by the daemon (MSG_CONFIGURE);
// echoed back in our own w0 configures so the daemon never repositions.
extern LONG g_ScreenWinX;
extern LONG g_ScreenWinY;
extern DWORD g_ScreenHeight;
extern DWORD g_ScreenWidth;
extern BOOL g_LocalScreenDestroyed;
extern DWORD g_HostScreenWidth;
extern DWORD g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
extern BOOL g_StagingGrant;
extern HWND g_DesktopWindow;
// Window currently being dragged with a held mouse button (input path); NULL when none.
// Suppresses per-window PrintWindow recapture for that window - see its definition.
extern volatile HWND g_InputDragWindow;
// Tick of the last input event for the latched window; the pump sweep disarms a latch that
// has seen no input for INPUT_DRAG_STUCK_MS (a lost Button1 release must not freeze a
// window's announces and content indefinitely).
extern DWORD g_InputDragLastEventTick;
// Translation origin FROZEN at the Button1 press that armed the latch (drag-wobble
// fix; see the definitions in main.c). Valid only while g_InputDragWindow is set and
// the press found the window tracked.
extern int g_InputDragOriginX;
extern int g_InputDragOriginY;
extern BOOL g_InputDragOriginValid;
// Live-feedback drag servo (D1, second iteration; mechanism comment at g_DragAnnounces
// in main.c): grab offset captured at the Button1 press, plus the timestamped ring of
// position announces sent for the latched window from which the input path reconstructs
// dom0's applied origin. Pump-thread-only, like the frozen-origin state above.
extern int g_DragLastInjectedX;
extern int g_DragLastInjectedY;
extern int g_InputDragGrabX;
extern int g_InputDragGrabY;
void DragAnnounceReset(IN int x, IN int y);
void DragAnnounceClear(void);
void DragAnnounceRecord(IN int x, IN int y);
BOOL DragAnnounceMoved(void);
BOOL DragAnnounceOriginAt(IN DWORD atTick, OUT int* x, OUT int* y);
extern char g_DomainName[256];
extern USHORT g_GuiDomainId;
extern CRITICAL_SECTION g_csWatchedWindows;

typedef struct _WINDOW_DATA
{
    HWND Handle;
    DWORD Style;
    DWORD ExStyle;
    BOOL IsIconic;
    BOOL IsVisible;
    BOOL DeletePending;
    WCHAR Caption[256];
    WCHAR Class[256];

    // These coords are a minimal bounding rectangle for the visible portion of the window.
    // This may be different than the real RECT of the window.
    int X;
    int Y;
    DWORD Width;
    DWORD Height;

    // Insets already SUBTRACTED from the raw rect above (toastcrop.c): X/Y/Width/Height
    // stay the single geometry source of truth and hold post-crop values, these record by
    // how much, for the consumers that must reconstruct the raw rect (HandleConfigure) or
    // re-apply the same crop to a freshly sampled one (the frame-loop refresh). All zero
    // means the window is uncropped, which is every window but a shell toast banner.
    int CropLeft;
    int CropTop;
    int CropRight;
    int CropBottom;

    // Position as of the frame currently being processed. Dirty rects come from a frame
    // captured BEFORE this frame's tracking update ran, so converting them to
    // window-relative coordinates with the freshly-updated X/Y mis-registers the damage by
    // however far the window moved in between - which dom0 renders as the content sliding
    // inside the frame while dragging. Snapshotted before TrackWindows(), used for damage.

    LIST_ENTRY ListEntry;

    BOOL IsOverrideRedirect;
    // Position in the guest's z-order, 0 = topmost. Recomputed per frame; used to clip damage
    // so a window never receives the pixels of a window stacked above it.
    int ZOrder;
    HWND ModalParent; // if nonzero, this window is modal in relation to window pointed by this field

    // Owner (GW_OWNER), not parent: every top-level window is parented to the desktop, but
    // compound-window chrome (Office 2013+ shadow strips, menus, tooltips) is OWNED by the
    // frame it decorates. Only sampled when the window is visible, which is all
    // ShouldAcceptWindow() needs: it rejects invisible windows before looking at this.
    HWND Owner;
    // Creator process. Win11 XAML windowed popups (Xaml_WindowedPopupClass "PopupHost":
    // teaching bubbles, WinUI menus/flyouts) carry no usable GW_OWNER link to the window
    // they belong to, so synthesis falls back to "topmost same-process window whose
    // granted buffer contains the popup" - which needs this. Sampled with Owner.
    DWORD ProcessId;

    // Uniform alpha of a layered window; 255 (opaque) for everything we cannot ask about.

    // Per-window capture state (perwindow.c). All zero while the window is on the
    // legacy screen-slice path; PwDumpSent is the "attached" flag.
    void* PwBuffer;        // page-aligned BGRA framebuffer granted to the gui domain
    ULONG PwPageCount;
    ULONG* PwGrantRefs;
    void* PwGrantHandle;   // sharedAddress from XcGnttabPermitForeignAccess2
    BOOL PwDumpSent;
    // Geometry the CURRENT buffer/dump was built for. The live Width/Height can move
    // ahead of it (tracking, dom0-initiated resize); any divergence triggers a rebuild
    // in UpdateWindowData, and remap re-announces THESE dims, never the live ones - a
    // dump claiming more pixels than the granted pages makes gui-daemon exit(1).
    ULONG PwWidth;
    ULONG PwHeight;

    // Last MSG_CONFIGURE sent for this window: byte-identical repeats are suppressed
    // (bursts of 4+ duplicates were measured during drags), and geometry the daemon
    // itself just dictated is recorded here so it is not echoed back at it.
    BOOL CfgSentValid;
    int LastCfgX;
    int LastCfgY;
    int LastCfgW;
    int LastCfgH;
    BOOL LastCfgOvr;

    // Rate limit for position-only announces (see SendWindowConfigureIfChanged): tick of the
    // last one sent, and the coordinates withheld by the limiter so the FINAL position is
    // always flushed when motion stops.
    DWORD CfgLastSentTick;
    BOOL  CfgPendingPos;
    int   CfgPendingX;
    int   CfgPendingY;

    // Daemon-driven geometry (HandleConfigure). During a dom0 title-bar drag the daemon
    // streams MSG_CONFIGURE at input rate; applying each one as its own async SetWindowPos
    // queued dozens of moves the guest window then played back over seconds (the window
    // applies them at frame cadence), and the frame path re-announced every lagging step -
    // dom0 replayed the whole trajectory after release (user-reproduced 2026-08-12, trace:
    // the same walk re-sent at ~10 Hz offset by the DWM border delta). Latest-wins instead:
    // the newest daemon geometry is stashed here and at most ONE async SetWindowPos is in
    // flight per window (ApplyPendingDaemonMove).
    BOOL  DaemonMovePending;   // a daemon-dictated geometry is waiting to be applied
    int   DaemonMoveX;         // latest daemon geometry, announce space
    int   DaemonMoveY;
    int   DaemonMoveW;
    int   DaemonMoveH;
    BOOL  DaemonMoveNoMove;    // position unchanged at receive time (SWP_NOMOVE)
    BOOL  DaemonMoveNoSize;    // size unchanged at receive time (SWP_NOSIZE)
    BOOL  DaemonPostedValid;   // an async SetWindowPos was posted and may still be in flight
    int   DaemonPostedX;       // its target, SetWindowPos (GetWindowRect) space
    int   DaemonPostedY;
    DWORD DaemonPostedTick;
    // Announce space (GetRealWindowRect: DWM extended frame bounds) minus SetWindowPos space
    // (GetWindowRect): the invisible-border delta, +7px for a themed Win11 window. The old
    // code passed daemon coords (announce space) straight to SetWindowPos, so every applied
    // move landed off by this delta, the frame path announced the shifted position, and the
    // daemon treated it as a real move - one guaranteed post-drop hop, and during a drag a
    // continuous fight with the dom0 WM. Cached per window; refreshed at most every 500 ms
    // (95492ed recomputed per configure - three DWM/display calls at input rate - and was
    // reverted for making the symptom worse).
    BOOL  DaemonOffValid;
    int   DaemonOffX;
    int   DaemonOffY;
    DWORD DaemonOffTick;
    // Tick of the last daemon MSG_CONFIGURE for this window (DriveTick), and of the last
    // one that had a predecessor within DAEMON_DRIVE_ACTIVE_MS (StreamTick - i.e. a dom0
    // WM drag arriving at input rate, vs a LONE placement configure when a window is first
    // mapped). While the STREAM is recent, the daemon is dictating this window's geometry:
    // position-only announces from the tracking/frame paths are withheld
    // (SendWindowConfigureIfChanged) - dom0 already knows where its own window is, and
    // echoing the guest's lagging position back is what made the dom0 window fight the WM
    // and replay the drag path. A lone configure must NOT suppress or hold anything: that
    // would delay a freshly-placed window's first paint by the whole hold window.
    DWORD DaemonDriveTick;
    DWORD DaemonStreamTick;
    // Damage for this window was withheld during a daemon drive (the announced origin is
    // the daemon's framebuffer read origin for slice-fed windows, and it is frozen while
    // announces are suppressed - sending damage against it would paint pixels from the
    // window's OLD screen region). Cleared by the one full-window settle repaint that
    // fires when the drive ends.
    BOOL  DaemonDamageHeld;

    // FROZEN ANCHOR (WM-managed shell surfaces, ShellManaged != none). dom0 owns this
    // window's PLACEMENT: the guest HWND is never moved (it must keep painting at its
    // natural anchor, which is the only place a DirectComposition shell surface renders),
    // and the agent stops announcing position so dom0 cannot be snapped back to the
    // anchor. Correctness rests on slice-fed surfaces being copied into a PER-WINDOW
    // buffer (PwSliceCopyAndDamage): what dom0 displays is position-independent, so the
    // card renders correctly wherever the user drags the frame. Input stays correct too -
    // HandleButton/HandleMotion translate dom0's window-RELATIVE coords against the
    // tracked anchor, which is where the surface really is.
    BOOL  DaemonOwnsPos;

    // Card size the dom0 size-lock hint was last sent for (WM-managed shell surfaces only).
    // -1 = never sent; re-sent when the announced card size changes.
    int SizeLockW;
    int SizeLockH;

    // Size the dom0 WM settled on for this window while maximized (from the daemon's
    // MSG_CONFIGURE): its decorations eat into the screen, so it can display slightly
    // less than the guest work area. While maximized, the reported/granted geometry is
    // capped to this so the dump matches the dom0 window exactly (no cut-off band).
    BOOL DaemonMaxValid;
    DWORD DaemonMaxW;
    DWORD DaemonMaxH;

    // Slice-fed per-window buffer: content is copied from the composited DDA screen
    // framebuffer (agent-side slice) instead of PrintWindow. Used for windows PrintWindow
    // cannot capture (ULW / WS_EX_NOREDIRECTIONBITMAP overlays). Content stays
    // window-relative, so dom0 renders it correctly wherever it places the window - unlike
    // the daemon-side legacy slice, which sources by the DAEMON's window position and
    // misregisters as soon as dom0 moves the window (e.g. force_on_screen pushing a
    // fullscreen overlay below its panel).
    BOOL PwSliceFed;
    BOOL PwSliceNeedsFull; // one full-window copy pending (fresh attach/remap)

    // Move-only drag fast path (ProcessNewFrame, PrintWindow-fed branch): a pure
    // position change does not alter the window's own content - the per-window buffer
    // is position-invariant and dom0 repositions it from MSG_CONFIGURE alone - so the
    // screen-dirty-rect recapture trigger is suppressed while the window is moving.
    // PwFrameX/Y is the position the previous processed frame saw; PwLastMoveTick is
    // when a position change was last observed (motion counts as over only after
    // PW_MOVE_SETTLE_MS of quiet - single frames with no applied LOCATIONCHANGE
    // happen mid-drag); PwSettleDue arms one unconditional recapture for when motion
    // ends; PwLastMoveCapTick rate-limits mid-motion content refreshes. All accessed
    // under g_csWatchedWindows; all reset on channel attach/detach.
    BOOL PwFrameXYValid;
    int PwFrameX;
    int PwFrameY;
    BOOL PwSettleDue;
    DWORD PwLastMoveTick;
    DWORD PwLastMoveCapTick;

    // INPUT-DRAG SLICE MODE (InputDragSlice knob; ProcessNewFrame moving branch). While
    // the user drags this window, its content is refreshed by a row-diffed copy out of
    // the composited desktop framebuffer instead of PrintWindow. WHY: PrintWindow is a
    // synchronous cross-process render (WM_PRINT-class) that executes on the DRAGGED
    // APP'S UI THREAD - the very thread running the modal move loop - measured p50
    // 49.4 ms per call at 2.6 Mpx and 150-250 ms under concurrent DWM compositing at
    // 5120x1440. The in-guest sampler showed the injected cursor travelling up to 221 px
    // while the window stayed frozen 193-211 ms at drag start (30-40 ms warm), and the
    // whole drag advanced in metronomic 193-277 ms stair-steps: one window step per
    // PrintWindow block. The drag-slice removes every cross-process call from the drag
    // path. While TRUE the frame loop owns the engine buffer (WcSetDdaOwned, same
    // contract as PwDdaActive); cleared by the settle branch (which drops ownership so
    // the settle WcMarkDirty can reach the engine - the D2 lesson) and on channel
    // attach/detach. Accessed under g_csWatchedWindows.
    BOOL PwDragSlice;
    // Frames this drag spent HOLDING content because the slice was ineligible (a TOPMOST
    // surface overlapped the window). Reported in the settle line so a hold is visible at
    // the shipped LogLevel=3 - an invisible content hold is the defect that shipped today.
    UINT PwDragHoldFrames;
    // Content was frozen for this window during a drag (InputDragFreezeContent): the
    // settle recapture is then mandatory and must repaint the WHOLE window, because
    // dom0 has been showing a pre-drag bitmap the whole time.
    BOOL PwDragFrozen;

    // Hash of the SCREEN pixels over this window's rect at the last recapture trigger.
    // Windows 11 presents ~1.9x more frames than Windows 10 for identical input (measured
    // with agent, display path and resolution held constant: 488 vs 259 frames over the same
    // 20 s workload), and every present whose dirty rect touches a window triggers a
    // PrintWindow recapture. The surplus captures are byte-identical, so the row-diff sends
    // nothing - the cost is the capture itself, ~15-18 ms on a WARP guest, and it is not
    // detectable from the dirty rects. Comparing the screen bytes first turns that into a
    // memcmp-class hash (~0.2 ms). Valid ONLY while the window is unoccluded; see
    // PwScreenUnchanged.
    BOOL  PwDdaActive;       // serving this window from the composited desktop
    ULONGLONG PwScreenHash;
    BOOL PwScreenHashValid;

    // Composite synthesis (CLAUDE.md 2A-chrome, taken further): an override-redirect
    // window fully contained in its owner's rect is NOT announced to dom0 at all -
    // no CREATE/MAP/CONFIGURE/DAMAGE/UNMAP/DESTROY ever names it. Instead the frame
    // loop patches its region from the composited desktop into the OWNER's buffer,
    // and the owner's capture masks that region so it cannot overwrite it. Result:
    // menus/tooltips/bubbles appear inside their window, with no floating bordered
    // rectangles in dom0.
    // TRUE once MSG_CREATE has been sent for this window. The daemon exit(1)s on any
    // message naming a window it has no CREATE for, so UNMAP/DESTROY at teardown must
    // be gated on this - a window that was synthesized (or whose announce failed) must
    // die silently.
    BOOL CreateSent;
    BOOL Synthesized;      // this window is composited into SynthOwner, never announced
    HWND SynthOwner;       // owner hwnd at synthesis time
    UINT SynthChildCount;  // (owners) number of active synthesized children
    DWORD SynthLastFullPatch; // (owners) GetTickCount() of the last full-rect child re-copy
    // Deferred capture-mask update (owners): the geometry-change paths never push the
    // mask directly - owner and child positions are refreshed by SEPARATE
    // UpdateWindowData interrogations, so a mid-pass push publishes a mixed-state
    // (fresh owner + stale child, or vice versa) mask and the later interrogation
    // pushes again to restore it, each push forcing a full recapture. They set this
    // flag instead, and TrackWindows flushes ONCE per tracking pass after all
    // interrogations completed (SynthFlushMasks), when every position is from the
    // same consistent snapshot.
    BOOL SynthMaskPending;
    // Last mask pushed to this owner's capture channel (SynthUpdateMask): WcSetMask
    // takes the engine lock EXCLUSIVELY (stalls behind an in-flight PrintWindow) and
    // forces a full recapture, so a byte-identical mask is never re-pushed. Rect
    // order is stable across passes because g_WatchedWindowsList is insertion-ordered
    // (InsertTailList only, never reordered in place). Zeroed at entry creation
    // (ZeroMemory) and on channel attach/detach - a fresh channel has no mask.
    int SynthMaskLastCount;
    RECT SynthMaskLast[8]; // == WC_MAX_MASK; C_ASSERTed in main.c
} WINDOW_DATA;

BOOL ShouldAcceptWindow(
    IN const WINDOW_DATA* data
    );

// Visible window rect as managed by DWM (GetWindowRect includes invisible resize grips), DPI
// adjusted. This is the geometry announced to the daemon, so dom0's frame hugs the window.
// How long after the last daemon MSG_CONFIGURE *stream* the daemon is considered to be
// actively dictating a window's geometry (dom0 WM drag). Shared with HandleConfigure,
// which uses it to tell a stream (two configures this close together) from a lone
// placement configure.
#define DAEMON_DRIVE_ACTIVE_MS 300

ULONG GetRealWindowRect(IN HWND window, OUT RECT* rect);

// Invalidate GetRealWindowRect's per-HMONITOR monitor/display-mode cache (R1,
// "MonInfoCache"). Called from the WM_DISPLAYCHANGE listener (workarea.c) - the one
// event on which the cached values can actually change. Safe from any thread.
void MonitorCacheInvalidate(void);

// Apply the newest daemon-dictated geometry for this window if no earlier async
// SetWindowPos is still in flight (latest-wins; see DaemonMove* in WINDOW_DATA).
// Call with g_csWatchedWindows held.
void ApplyPendingDaemonMove(IN OUT WINDOW_DATA* entry);

// ApplyPendingDaemonMove for every watched window; takes g_csWatchedWindows itself.
// Called after a vchan drain so a configure flood collapses to one move per window.
void ApplyAllPendingDaemonMoves(void);

// TRUE while any window owes daemon-settle work (withheld announce, unapplied daemon
// move, held damage) - the pump then bounds its wait instead of sleeping forever.
BOOL DaemonSettleWorkPending(void);

// Timer-driven settle for the no-frames case (static desktop after a drag): flush,
// apply, and release held damage for every window. Takes g_csWatchedWindows itself.
void DaemonSettleSweep(void);

WINDOW_DATA *FindWindowByHandle(
    IN HWND window
    );

ULONG AddWindow(
    IN WINDOW_DATA* entry
    );

ULONG RemoveWindow(
    IN OUT WINDOW_DATA *entry
    );

// This (re)initializes watched windows, hooks etc.
ULONG SetSeamlessMode(
    IN BOOL seamlessMode,
    IN BOOL forceUpdate
    );

// Drop the desktop image published for composite synthesis. The capture layer must call this
// under ctx->frame.lock whenever the mapped desktop surface is released (duplication recreate
// or teardown): synthesis paints from the window-event thread and would otherwise read a
// pointer into an unmapped surface.
void PwInvalidateFramebuffer(void);

// Wake the main loop's window-tracking pass (thread-safe; used by the toastcrop worker
// when an async crop measurement resolves).
void PokeWindowTracking(void);
