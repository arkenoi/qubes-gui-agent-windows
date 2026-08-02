/*
 * wincapture - per-window content capture (Windows.Graphics.Capture) behind a C API.
 *
 * Feeds the per-window framebuffer model: each captured window renders into a
 * caller-owned, page-aligned BGRA buffer of exactly width*height*4 bytes (the buffer
 * the caller grants to the gui domain). Frames are diffed row-wise against the buffer;
 * only changed row bands are written back and reported, so idle WGC redelivery (which
 * carries no damage information on Win10) costs one compare and no vchan traffic.
 *
 * Threading: one internal capture thread polls all sessions and invokes the damage
 * callback from that thread. The callback must be safe to call concurrently with the
 * main/frame threads (SendWindowDamageEvent takes the vchan lock internally).
 */

#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// x/y/w/h are window-relative (buffer coordinates).
typedef void (*WC_DAMAGE_CALLBACK)(HWND window, int x, int y, int w, int h);

// Initialize D3D + capture thread. Returns ERROR_SUCCESS or a Win32/HRESULT-ish code.
ULONG WcInit(WC_DAMAGE_CALLBACK callback);

// Stop the capture thread and drop all sessions. Buffers are caller-owned; none are
// freed here.
void WcShutdown(void);

// TRUE if Windows.Graphics.Capture is available on this OS.
BOOL WcIsSupported(void);

// 0 if WGC is usable; otherwise the failing HRESULT/status (for logging: WHY it is
// unavailable - apartment init, activation factory, IsSupported() itself).
ULONG WcProbeSupport(void);

// Begin capturing hwnd into 'buffer' (caller-allocated, width*height*4 bytes,
// page-aligned). cropX/cropY: offset of the visible bounding rect (what the daemon was
// told about via MSG_CONFIGURE) inside the OS window rect that WGC captures - DWM
// extended frame bounds exclude the invisible resize borders GetWindowRect includes.
// Fails (nonzero) if a capture session cannot be created; caller then leaves the window
// on the legacy screen path.
ULONG WcAddWindow(HWND hwnd, int width, int height, int cropX, int cropY, void* buffer);

// Stop capturing hwnd. After return the module no longer touches the buffer.
void WcRemoveWindow(HWND hwnd);

// TRUE if the channel for hwnd died (exception during capture) or does not exist.
// The owner should detach and fall back to the legacy path.
BOOL WcIsDead(HWND hwnd);

// Request a capture of hwnd soon (called from the frame path when screen dirty rects
// intersect the window). Cheap; coalesces.
void WcMarkDirty(HWND hwnd);

// Maximum masked regions per window (synthesized children composited by the frame
// loop; see WcSetMask).
#define WC_MAX_MASK 8

// Set the buffer-relative regions this window's capture must not write, because a
// synthesized child owns those pixels. Replaces any previous mask; count 0 clears.
// Rects may be given in any order and may overlap; the engine sorts its own copy.
// Takes the engine lock exclusively while captures hold it shared, so it returns only
// once no capture is in flight - i.e. it is a rendezvous, and pixels written into the
// masked regions AFTER it returns cannot be overwritten by an older capture.
void WcSetMask(HWND hwnd, const RECT* rects, int count);

// Synchronously render the window's current content into the buffer via
// PrintWindow(PW_RENDERFULLCONTENT) so the daemon has real pixels before the first WGC
// frame arrives. Window-relative crop applied as in WcAddWindow.
ULONG WcPrefill(HWND hwnd);

#ifdef __cplusplus
}
#endif
