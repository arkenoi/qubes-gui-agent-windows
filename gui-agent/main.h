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

    // Hash of the SCREEN pixels over this window's rect at the last recapture trigger.
    // Windows 11 presents ~1.9x more frames than Windows 10 for identical input (measured
    // with agent, display path and resolution held constant: 488 vs 259 frames over the same
    // 20 s workload), and every present whose dirty rect touches a window triggers a
    // PrintWindow recapture. The surplus captures are byte-identical, so the row-diff sends
    // nothing - the cost is the capture itself, ~15-18 ms on a WARP guest, and it is not
    // detectable from the dirty rects. Comparing the screen bytes first turns that into a
    // memcmp-class hash (~0.2 ms). Valid ONLY while the window is unoccluded; see
    // PwScreenUnchanged.
    DWORD PwDdaVerifyTick;   // last forced PrintWindow while DDA-sourced
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
ULONG GetRealWindowRect(IN HWND window, OUT RECT* rect);

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
