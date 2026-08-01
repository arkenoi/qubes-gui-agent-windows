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
extern DWORD g_ScreenHeight;
extern DWORD g_ScreenWidth;
extern BOOL g_LocalScreenDestroyed;
extern DWORD g_HostScreenWidth;
extern DWORD g_HostScreenHeight;
extern BOOL g_VchanClientConnected;
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
