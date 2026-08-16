/*
 * perwindow - per-window framebuffer management (grants + MSG_WINDOW_DUMP lifecycle).
 *
 * Glue between the WGC capture engine (wincapture.h) and the protocol: allocates a
 * page-aligned BGRA buffer per accepted window, grants it read-only to the gui domain
 * (same call and flags as the screen framebuffer, capture.c:528), sends a per-window
 * MSG_WINDOW_DUMP so the daemon composites from the window's OWN buffer with
 * window-relative damage (gui-daemon do_shm_update own-shmseg branch), and manages
 * revocation. The daemon side needs no changes: this is the same path the Linux agent
 * uses for every window (see DESIGN-per-window-capture.md).
 *
 * Enabled by registry DWORD "PerWindowCapture" (default ON) or env QGA_PERWINDOW=0/1,
 * and only when the daemon protocol version is >= 1.7 (WINDOW_DUMP_ACK era) and WGC is
 * available. When disabled or when attach fails, windows stay on the legacy
 * screen-slice path unchanged.
 */

#pragma once
#include <windows.h>
#include "main.h"

// Call once from Init() after PerfInit. Never fails hard: on any failure per-window
// mode is simply disabled and the agent runs the legacy path.
void PwInit(void);
void PwShutdown(void);

// TRUE if per-window mode is on (config + WGC support). Daemon version is checked
// separately at attach time because it is only known after the vchan handshake.
BOOL PwEnabled(void);

// Record the daemon protocol version (from MSG_VERSION handling).
void PwSetDaemonVersion(uint32_t version);

// Attach: allocate+grant buffer, start capture, prefill, send MSG_WINDOW_DUMP and a
// full-window damage. Call AFTER SendWindowCreate and BEFORE SendWindowMap.
// On failure the window is left untouched on the legacy path (returns nonzero).
ULONG PwAttachWindow(IN OUT WINDOW_DATA* entry);

// TRUE if this window has an active per-window buffer (dump sent) - such windows are
// skipped by the legacy screen-path damage generation.
BOOL PwIsAttached(IN const WINDOW_DATA* entry);

// Detach: stop capture, queue the grant for revocation, clear entry state. Safe to
// call for non-attached windows. Call before the entry is freed.
void PwDetachWindow(IN OUT WINDOW_DATA* entry);

// Push a fresh visible-rect offset to the capture engine after a geometry change.
void PwRefreshCrop(IN const WINDOW_DATA* entry);

// FALSE if PrintWindow cannot produce correct pixels for this window (ULW-style or
// colorkeyed layered windows) - such windows must stay on the legacy screen-slice path.
BOOL PwWindowEligible(IN const WINDOW_DATA* entry);

// Detach an attached window at runtime and force the daemon to release its stale image
// via an unmap/map cycle, dropping the window to the legacy path.
void PwForceLegacy(IN OUT WINDOW_DATA* entry);

// Size changed: rebuild buffer/capture/grant and re-send MSG_WINDOW_DUMP. Call after
// SendWindowConfigure for the new size.
ULONG PwResizeWindow(IN OUT WINDOW_DATA* entry);

// Re-announce the dump after an unmap/map cycle (the daemon releases its mapping of a
// non-screen window's buffer on MSG_UNMAP; the grants stay valid, so a re-sent
// MSG_WINDOW_DUMP with the same refs restores it). Call after SendWindowMap when the
// window was already attached.
ULONG PwRemapWindow(IN const WINDOW_DATA* entry);

// Retry pending grant revocations (revoke fails with a busy status while the daemon
// still has the segment mapped; it succeeds once the daemon processed the superseding
// WINDOW_DUMP / DESTROY). Called periodically from the main loop and from the
// MSG_WINDOW_DUMP_ACK handler.
void PwRevokeTick(void);

// TRUE while any queued per-window grant revocation is still awaiting success.
// Drives the bounded drain on the exit path (A6).
BOOL PwRevokePending(void);
