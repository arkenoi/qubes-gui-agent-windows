/*
 * workarea - guest work-area synchronization with dom0's usable workspace.
 * See DESIGN-workarea-propagation.md in the driver repo.
 *
 * A maximized Windows window sizes itself to the GUEST work area; without dom0's
 * workspace geometry it overflows the dom0 client area (panels + per-window WM
 * decorations). Sources, in priority order:
 *   1. registry REG_CONFIG_WORKAREA_VALUE = "x,y,w,h"  (guest-final rect, verbatim)
 *   2. qubesdb /qubes-workarea = "x y w h fl fr ft fb" (dom0 work area + WM frame
 *      extents, written by the optional dom0 watcher script; watched for changes,
 *      so dom0 panel/monitor changes propagate live)
 *   3. inference from daemon-dictated window origins (the WM-placed client offset
 *      observed in MSG_CONFIGURE reveals panel + frame margins)
 * The same computation also serves MSG_WORKAREA if a protocol-1.9 daemon ever
 * sends it (see vchan-handlers.c).
 */
#pragma once
#include <windows.h>

// Initialize the module lock only. MUST run before any thread that can execute
// WaWndProc exists, i.e. before the window-event thread is started (the broadcast
// listener's re-assert path takes the lock). Idempotent, but only against callers on
// the same thread - it is meant for single-threaded startup (Init()).
void WorkAreaLockInit(void);

// Full init: start the qubesdb watch thread and allow applies (idempotent, safe
// before vchan connect). Until this runs, WorkAreaApply/WorkAreaReassert/
// WorkAreaEnsureApplied are no-ops.
void WorkAreaInit(void);

// Recompute the target work area from the best available source and apply it
// (SPI_SETWORKAREA + re-fit of maximized windows) if it changed.
void WorkAreaApply(void);

// Re-apply the current target even if it did not change (something overwrote it).
void WorkAreaReassert(void);

// Create the hidden top-level window that receives WM_SETTINGCHANGE/WM_DISPLAYCHANGE
// broadcasts. Must be called on a thread that pumps messages (the window-event thread)
// AND whose desktop handle was opened with DESKTOP_CREATEWINDOW, after
// WorkAreaLockInit. Idempotent per thread (replaces a leftover listener).
void WorkAreaCreateListener(void);

// Destroy the listener. Must be called on the thread that created it, and before that
// thread re-attaches to another desktop (SetThreadDesktop fails while the thread owns
// windows on its current desktop).
void WorkAreaDestroyListener(void);

// Drift check: re-assert if SPI_GETWORKAREA no longer matches the last applied rect
// (an asynchronous Explorer overwrite the broadcast listener missed). Rate-limited
// internally; called from the main loop's frame and window-event paths, so it fires
// as long as either flows - a fully idle desktop is covered only by the listener.
// Takes no locks across SPI/window calls.
void WorkAreaEnsureApplied(void);

// Record a dom0-provided work area + frame extents (qubesdb or MSG_WORKAREA).
void WorkAreaSetDom0(int x, int y, int w, int h, int fl, int fr, int ft, int fb);

// Inference sample: a daemon-dictated window origin from MSG_CONFIGURE.
void WorkAreaNoteDaemonOrigin(int x, int y);
