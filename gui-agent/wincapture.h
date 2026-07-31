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

// Begin capturing hwnd into 'buffer' (caller-allocated, width*height*4 bytes,
// page-aligned). cropX/cropY: offset of the visible bounding rect (what the daemon was
// told about via MSG_CONFIGURE) inside the OS window rect that WGC captures - DWM
// extended frame bounds exclude the invisible resize borders GetWindowRect includes.
// Fails (nonzero) if a capture session cannot be created; caller then leaves the window
// on the legacy screen path.
ULONG WcAddWindow(HWND hwnd, int width, int height, int cropX, int cropY, void* buffer);

// Stop capturing hwnd. After return the module no longer touches the buffer.
void WcRemoveWindow(HWND hwnd);

// Synchronously render the window's current content into the buffer via
// PrintWindow(PW_RENDERFULLCONTENT) so the daemon has real pixels before the first WGC
// frame arrives. Window-relative crop applied as in WcAddWindow.
ULONG WcPrefill(HWND hwnd);

#ifdef __cplusplus
}
#endif
