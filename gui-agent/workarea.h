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

// Start the qubesdb watch thread (idempotent, safe before vchan connect).
void WorkAreaInit(void);

// Recompute the target work area from the best available source and apply it
// (SPI_SETWORKAREA + re-fit of maximized windows) if it changed.
void WorkAreaApply(void);

// Re-apply the current target even if it did not change (something overwrote it).
void WorkAreaReassert(void);

// Create the hidden top-level window that receives WM_SETTINGCHANGE/WM_DISPLAYCHANGE
// broadcasts. Must be called on a thread that pumps messages (the window-event thread).
void WorkAreaCreateListener(void);

// Record a dom0-provided work area + frame extents (qubesdb or MSG_WORKAREA).
void WorkAreaSetDom0(int x, int y, int w, int h, int fl, int fr, int ft, int fb);

// Inference sample: a daemon-dictated window origin from MSG_CONFIGURE.
void WorkAreaNoteDaemonOrigin(int x, int y);
