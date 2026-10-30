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
#include <stdint.h>

#include "main.h"

// window size hint constants
// http://tronche.com/gui/x/icccm/sec-4.html
#define USPosition  1       // User-specified x, y
#define USSize      2       // User-specified width, height
#define PPosition   4       // Program-specified position
#define PSize       8       // Program-specified size
#define PMinSize    16      // Program-specified minimum size
#define PMaxSize    32      // Program-specified maximum size
#define PResizeInc  64      // Program-specified resize increments
#define PAspect     128 	// Program-specified min and max aspect ratios
#define PBaseSize   256 	// Program-specified base size
#define PWinGravity 512 	// Program-specified window gravity

// window hint flags
#define InputHint          1  // input
#define StateHint          2  // initial_state
#define IconPixmapHint     4  // icon_pixmap
#define IconWindowHint     8  // icon_window
#define IconPositionHint  16  // icon_x & icon_y
#define IconMaskHint      32  // icon_mask
#define WindowGroupHint   64  // window_group
#define MessageHint      128  // (this bit is obsolete)
#define UrgencyHint      256  // urgency

// ctxWidth/ctxHeight are the capture context's DXGI-desc-derived geometry (the size the
// grant refs were actually allocated for); the dump header is built from them, falling
// back to g_ScreenWidth/Height only when they are unavailable (zero).
ULONG SendScreenGrants(IN size_t numGrants, IN const ULONG* refs,
    IN UINT ctxWidth, IN UINT ctxHeight);

// Per-window MSG_WINDOW_DUMP: announce a window's own framebuffer (grant refs) to the
// daemon. window==0 is the whole-screen dump (SendScreenGrants).
ULONG SendWindowDump(IN HWND window, IN ULONG width, IN ULONG height,
    IN size_t numGrants, IN const ULONG* refs);
// Announce a window to dom0. windowData == NULL is the screen pseudo-window.
// Sends MSG_CREATE at most ONCE per hwnd lifetime: a repeat call for a window the daemon
// already has is answered with MSG_CONFIGURE instead, because a second CREATE for a live id
// makes gui-daemon exit(1) and takes the whole qube's GUI with it.
// Returns ERROR_INVALID_DATA when the window's geometry was refused at the wire (send.c).
// That is NOT a transport failure and must not be treated as one: the daemon has no CREATE
// for this hwnd, so the caller must leave it unannounced (CreateSent FALSE) and let a later
// pass retry, rather than sending dependent messages the daemon would exit on.
ULONG SendWindowCreate(IN const WINDOW_DATA *windowData);

// FI_DUP_CREATE only - emits MSG_CREATE even when the hwnd already has one, bypassing the
// create-once rule that is the code under test. No legitimate caller exists; in a release
// build its only call site is behind a FiShouldDupCreate() compiled to a constant FALSE.
ULONG SendWindowCreateForced(IN const WINDOW_DATA *windowData);
ULONG SendWindowDestroy(IN HWND window);

// Forget which windows the daemon has been told about (see send.c). Must be called
// when a new vchan client connects, before anything is sent to it.
void SendResetCreatedWindows(void);
ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset);
ULONG SendWindowHints(IN HWND window, IN uint32_t flags);
ULONG SendWindowSizeLock(IN HWND window, IN uint32_t width, IN uint32_t height);
ULONG SendScreenHints(void);
ULONG SendWindowUnmap(IN HWND window);
ULONG SendWindowMap(IN const WINDOW_DATA *windowData OPTIONAL); // if windowData == 0, use the whole screen
ULONG SendWindowConfigure(HANDLE window, int x, int y, int width, int height, BOOL popup);
ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height);

// Swap-edge frame gate (ADR-toasts 10). While a toast banner is pre-empted its window stays mapped
// and dom0 keeps the frame it has, but SendWindowDamageEvent forwards none of that window's new
// frames. Both are idempotent and lock-free; see send.c for why the frame path takes no lock.
//   Gate   -> TRUE if the window is gated after the call. FALSE means NO SLOT WAS FREE, and the
//             caller must then fall back to unmapping the banner: leaving it mapped and ungated
//             would show the queued toast's pixels as the displayed one's.
//   Ungate -> TRUE if the window WAS gated, in which case the caller owes it a full-window
//             SendWindowDamageEvent at once - held frames left dom0 with a stale one and nothing
//             else will re-send the current content. framesHeld, when given, receives how many
//             frames were actually dropped: reported once here rather than once per frame, which
//             is both the log policy and the measurement of what the gate held back.
#define SEND_GATE_REFUSED 0   // no slot was free - the caller must unmap instead of leaving it mapped
#define SEND_GATE_NEW     1   // this call closed the gate (log the swap here, not on every re-assert)
#define SEND_GATE_ALREADY 2   // it was already closed
int  SendWindowFrameGate(IN HWND window);
BOOL SendWindowFrameUngate(IN HWND window, OUT ULONG *framesHeld OPTIONAL);
ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL);
ULONG SendProtocolVersion(void);
