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

#define COBJMACROS
#include <D3D11.h>
#include <dxgi1_2.h>
#include <initguid.h>

#include <xencontrol.h>

#include "perf.h"

#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))
#define	FRAMEBUFFER_PAGE_COUNT(width, height)	(ALIGN(((width)*(height)*4), PAGE_SIZE) / PAGE_SIZE)


// TODO: these structs should be opaque with functions to access public parts
// or rewrite everything in C++ ;)
typedef struct _CAPTURE_FRAME
{
	IDXGIResource* texture;
	DXGI_OUTDUPL_FRAME_INFO info;
	BOOL mapped;
	DXGI_MAPPED_RECT rect;
	UINT dirty_rects_count;
	RECT* dirty_rects;
	CRITICAL_SECTION lock;
	PERF_CAPTURE perf; // instrumentation, see perf.h (inert unless enabled)
} CAPTURE_FRAME;

// A6: a superseded screen grant awaiting revocation. dom0 releases its mapping of the
// old framebuffer when it processes the superseding MSG_WINDOW_DUMP for window 0
// (xside.c releases first, then maps the new refs), so revocation is gated on the
// daemon's MSG_WINDOW_DUMP_ACK - with a timeout fallback retried by the capture thread.
// Mirror of perwindow.c's PW_PENDING_REVOKE, but for the screen grant: that queue frees
// its buffers with VirtualFree and revokes on the per-window xencontrol handle, neither
// of which fits the mapped desktop surface granted on ctx->xc.
typedef struct _STALE_GRANT
{
	void* framebuffer;             // grant handle == the granted address
	ULONG* grant_refs;
	ULONGLONG deadline;            // GetTickCount64() past which the timeout fallback fires
	BOOL timeout_logged;
	struct _STALE_GRANT* next;
} STALE_GRANT;

typedef struct _CAPTURE_CONTEXT
{
	UINT width;
	UINT height;
	IDXGIAdapter* adapter;
	ID3D11Device* device;
	IDXGIOutput1* output;
	IDXGIOutputDuplication* duplication;
	HANDLE thread; // capture loop
	PXENCONTROL_CONTEXT xc;
	// mapped framebuffer location is constant as long as the capture interface is valid
	// this gets initialized when the first frame is acquired
	void* framebuffer; // framebuffer address
	ULONG* grant_refs; // xen grant refs for shared framebuffer pages
	// Set when grant_refs have been re-established for a NEW duplication (see
	// RecreateDuplication). The frame loop must re-send MSG_WINDOW_DUMP, or the daemon keeps
	// reading the pages of the duplication that was torn down.
	BOOL grants_changed;
	BOOL granted_once; // distinguishes the first grant from a re-grant after recovery
	HANDLE frame_event; // capture thread -> main loop: new frame
	HANDLE ready_event; // main loop -> capture thread: frame processed
	HANDLE error_event; // capture thread -> main loop: capture error
	CAPTURE_FRAME frame; // current frame data
	// A6: superseded screen grants awaiting revocation (see STALE_GRANT above).
	// stale_lock is a leaf lock: never take another lock while holding it.
	CRITICAL_SECTION stale_lock;
	STALE_GRANT* stale_grants;
	// STAGING: this capture generation publishes frames through the persistent
	// module-lifetime staging buffer (capture.c). framebuffer/grant_refs are then
	// ALIASES of the staging state: no teardown/park path may free or revoke them.
	BOOL uses_staging;
	// STAGING: the next successful frame must copy the FULL frame into the staging
	// buffer, ignoring dirty rects (first frame, and after any geometry change or
	// duplication recreate - staging content at the old pitch is garbage for the new).
	BOOL staging_full_copy;
} CAPTURE_CONTEXT;

// initialize capture interfaces and map framebuffer
CAPTURE_CONTEXT* CaptureInitialize(HANDLE frame_event, HANDLE error_event);

// start the capture thread
HRESULT CaptureStart(IN OUT CAPTURE_CONTEXT* ctx);

// stop the capture thread
void CaptureStop(IN OUT CAPTURE_CONTEXT* ctx);

// Rebuild the duplication and restart the capture thread WITHOUT touching the protocol: the screen
// window stays announced and the grants stay valid. Returns FALSE if the fault is not recoverable
// this way, in which case the caller must fall back to the session-level restart.
BOOL CaptureRecoverInPlace(IN OUT CAPTURE_CONTEXT* ctx);

void CaptureTeardown(IN OUT CAPTURE_CONTEXT* ctx);

// A6: try to revoke every parked (superseded) screen grant now, regardless of the ack
// deadline. Call when dom0 acknowledged the window-0 MSG_WINDOW_DUMP (it has released
// its old mapping), on the exit drain, and from teardown. `why` tags the log line.
// Never blocks: one revoke attempt per parked grant; failures stay queued.
void CaptureRevokeStaleGrants(IN OUT CAPTURE_CONTEXT* ctx, IN const WCHAR* why);

// A6: TRUE while any superseded screen grant is still awaiting successful revocation.
BOOL CaptureHasStaleGrants(IN CAPTURE_CONTEXT* ctx);

// STAGING: page count to send with the window-0 MSG_WINDOW_DUMP. With the staging
// grant this is the CONSTANT full capacity of the staging buffer regardless of the
// current geometry: gui-daemon exit(1)s only when the count is TOO SMALL for
// width*height (xside.c:3903-3913, img_data_size < w*h*4); a larger count is
// accepted. The constant count is what makes a resize a pure header refresh over
// the same grant refs. Direct-map path: the exact per-geometry count, as before.
size_t CaptureGrantPageCount(IN const CAPTURE_CONTEXT* ctx);

// STAGING: best-effort revocation of the persistent staging grant. The staging
// buffer deliberately survives CaptureTeardown and reinitialization; call this
// ONLY from the A6 exit path, after the teardown notifications and revoke drain.
// Failure (dom0 still maps the pages) is logged and the buffer leaked - the
// process is exiting.
void CaptureStagingRevokeOnExit(void);
