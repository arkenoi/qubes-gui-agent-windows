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

#include "capture.h"
#include "common.h"
#include "main.h"

#include <log.h>

#include <assert.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <strsafe.h>

// TODO: configure timeout through registry config (milliseconds)
#define FRAME_TIMEOUT 1000

// A6: how long a superseded screen grant waits for dom0's MSG_WINDOW_DUMP_ACK before
// the capture thread falls back to retrying the revocation on its own (design 2.1).
#define A6_ACK_TIMEOUT_MS 5000

volatile LONG g_CaptureThreadEnable = 0;

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout);
static HRESULT ReleaseFrame(IN OUT CAPTURE_CONTEXT* ctx);
static DWORD WINAPI CaptureThread(void* param);

// ---------------------------------------------------------------------------
// STAGING: the screen framebuffer granted to dom0 EXACTLY ONCE per agent lifetime.
//
// Evidence (FINDINGS.md 2026-08-05 "cont 8"): at the whole-guest livelock the wedged
// domain held ~22,000 ACTIVE grant entries still mapped by dom0 - dozens of stale
// framebuffer generations accumulated because every resize re-granted the desktop
// surface and dom0-side release could not keep pace. This object removes the
// accumulation class structurally: ONE maximum-size page-aligned buffer is
// VirtualAlloc'd and granted the first time capture initializes, every frame is
// COPIED into it (dirty rects only), and every window-0 MSG_WINDOW_DUMP re-uses the
// same refs. Resizes stop creating grant traffic entirely.
//
// Lifetime: this state deliberately lives OUTSIDE CAPTURE_CONTEXT. It survives
// RecreateDuplication, CaptureTeardown(!) and reinitialization, and has its OWN
// xencontrol handle so CaptureTeardown's XcClose(ctx->xc) cannot orphan the
// revocation path. Revoked only in the A6 exit path (CaptureStagingRevokeOnExit),
// after the drain, best effort.
//
// Cost trade: one memcpy of the dirty region per frame (~microseconds per MB)
// against 4.7-6.5 ms per 1080p re-grant and ~90 us per revoke on the direct path
// (FINDINGS.md measured grant costs) - and against the livelock above.
typedef struct _STAGING_GRANT
{
    PXENCONTROL_CONTEXT xc; // dedicated handle, never closed by capture teardown
    BYTE* buffer;           // VirtualAlloc'd, page-aligned
    size_t size;            // bytes == page_count * PAGE_SIZE
    size_t page_count;
    ULONG* refs;
    void* handle;           // grant handle from XcGnttabPermitForeignAccess2
    UINT cap_width;         // capacity geometry (for logs/guards only; the current
    UINT cap_height;        // frame geometry lives in ctx->width/height)
} STAGING_GRANT;

static STAGING_GRANT g_Staging;

// Guaranteed minimum capacity; the actual capacity is the larger of this and the
// dom0 host resolution from msg_xconf (known before capture ever initializes:
// HandleXconf runs before StartFrameProcessing).
#define STAGING_MIN_WIDTH  2560
#define STAGING_MIN_HEIGHT 1600

// TRUE when a frame of width x height fits the granted staging buffer.
static BOOL StagingCapacityOk(IN UINT width, IN UINT height)
{
    return g_Staging.handle && ((size_t)width * height * 4 <= g_Staging.size);
}

// note: win_perror* functions set last error

static IDXGIAdapter* GetAdapter(void)
{
    LogVerbose("start");
    IDXGIAdapter* ret_adapter = NULL;
    // need the ...1 interfaces for output duplication
    IDXGIFactory1* factory = NULL;
    HRESULT status = CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)(&factory));
    if (FAILED(status))
    {
        win_perror2(status, "CreateDXGIFactory1");
        goto end;
    }

    IDXGIAdapter* adapter = NULL;
    UINT i = 0;
    while (IDXGIFactory1_EnumAdapters(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC desc;
        IDXGIAdapter_GetDesc(adapter, &desc);
        LogDebug("DXGI adapter %d: %s", i, desc.Description);
        // first adapter returned contains primary desktop
        if (i > 0)
            IDXGIAdapter_Release(adapter);
        else
            ret_adapter = adapter;

        i++;
    }

    IDXGIFactory1_Release(factory);
end:
    LogVerbose("end");
    return ret_adapter;
}

static ID3D11Device* GetDevice(IN IDXGIAdapter* adapter)
{
    LogVerbose("start");
    ID3D11Device* device = NULL;

    D3D_FEATURE_LEVEL supported_feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1,
    };

    HRESULT status = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        0, /*D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_DEBUG,*/
        supported_feature_levels, ARRAYSIZE(supported_feature_levels), D3D11_SDK_VERSION,
        &device, NULL, NULL);

    if (FAILED(status))
    {
        win_perror2(status, "D3D11CreateDevice");
        return NULL;
    }

    LogVerbose("end");
    return device;
}

static IDXGIOutput1* GetOutput(IN IDXGIAdapter* adapter)
{
    LogVerbose("start");
    IDXGIOutput* output = NULL;
    UINT i = 0;

    while (IDXGIAdapter1_EnumOutputs(adapter, i, &output) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_OUTPUT_DESC desc = { 0 };

        HRESULT status = IDXGIOutput_GetDesc(output, &desc);
        if (FAILED(status))
        {
            IDXGIOutput_Release(output);
            win_perror2(status, "output->GetDesc()");
            goto fail;
        }

        LogDebug("Output %u: %s, attached to desktop: %d", i, desc.DeviceName, desc.AttachedToDesktop);

        if (desc.AttachedToDesktop)
        {
            // IDXGIOutput1 is needed for output duplication
            IDXGIOutput1* output1 = NULL;
            status = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1);
            if (FAILED(status))
            {
                win_perror2(status, "output->QueryInterface(IDXGIOutput1)");
                IDXGIOutput_Release(output);
                goto fail;
            }

            IDXGIOutput_Release(output);
            LogVerbose("end");
            SetLastError(status);
            return output1;
        }

        IDXGIOutput_Release(output);
        i++;
    }

fail:
    LogVerbose("end");
    SetLastError((DWORD)DXGI_ERROR_NOT_FOUND);
    return NULL;
}

static IDXGIOutputDuplication* GetDuplication(IN IDXGIOutput1* output, IN ID3D11Device* device, OUT UINT* width, OUT UINT* height);

// Rebuild the duplication object in place after DXGI_ERROR_ACCESS_LOST/ACCESS_DENIED,
// WITHOUT disturbing the watched window list. Returns TRUE if capture can continue.
//
// The mode and DesktopImageInSystemMemory can both change across the event (that is
// often why it fired), so the desc is re-read by GetDuplication and a resolution change
// is reported the same way the initial setup does. If the desktop is momentarily
// unavailable - which is exactly what the secure desktop looks like - retry briefly
// rather than giving up: the secure desktop typically lasts seconds.
// Re-attach THIS thread to the current input desktop.
//
// DuplicateOutput() returns E_ACCESSDENIED (0x80070005) when the calling thread is not on the
// input desktop, which is exactly the state a desktop switch leaves the capture thread in -
// and a desktop switch is one of the main things that invalidates the duplication in the
// first place. Without this, every retry in RecreateDuplication() fails for the whole retry
// window, recovery gives up, and the caller falls back to the full teardown that unmaps every
// window - the precise outcome the in-place recovery exists to avoid.
//
// Deliberately NOT AttachToInputDesktop(): that helper also writes the shared globals
// g_DesktopWindow/g_StartWindow/g_SearchWindow unsynchronised and CloseDesktop()s the
// process-default desktop handle, which MSDN forbids and which a further caller would
// double-close. See the same reasoning at the hook thread in main.c. Only the per-thread part
// is wanted here, and only this thread's own handle is ever closed.
static void AttachCaptureThreadToInputDesktop(void)
{
    static HDESK previous = NULL; // only ever a handle THIS function opened

    HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!desktop)
        return; // not fatal: the retry loop will try again

    if (!SetThreadDesktop(desktop))
    {
        // fails if the thread owns windows or hooks; the capture thread owns neither, but
        // never leak the handle if it ever does
        CloseDesktop(desktop);
        return;
    }

    // Do NOT CloseDesktop() the handle we previously installed.
    //
    // Closing it on a cold boot leaves the MAIN thread's desktop invalid: EnumWindows then
    // returns ERROR_INVALID_HANDLE on every resync, no window is ever added to the watched
    // list, and the qube renders nothing in dom0. Bisected to this change - the build
    // immediately before it reports 0 failures, this one reports 8.
    //
    // It does not reproduce when the agent is restarted in a live session, because that
    // re-establishes a valid desktop, which is why every check in the suite missed it.
    //
    // Leaking one desktop handle per recovery is the correct trade: recoveries are rare, the
    // handle is released when the process exits, and MSDN warns against closing a desktop
    // that may still be in use by another thread of the process.
    previous = desktop;
    (void)previous;
}

// A6: park the current screen grant for later, ack-gated revocation and clear the
// context's grant fields so GetFrame re-maps and re-grants the next surface. dom0 still
// maps these pages - revoking here is the revoke-before-notify inversion this design
// removes (the revoke could fail, admitted in-code at the old call site). Called with
// ctx->frame.lock held; takes only the leaf stale_lock inside.
//
// STAGING made this DORMANT for the screen path: with the persistent staging grant
// there are no screen re-grants, so nothing is ever parked (the call site is gated on
// !uses_staging). Kept intact for the direct-map fallback (StagingGrant=0, or a
// geometry beyond the staging capacity); the per-window path has its own machinery
// in perwindow.c and is unaffected either way.
static void ParkStaleScreenGrant(IN OUT CAPTURE_CONTEXT* ctx)
{
    STALE_GRANT* s = (STALE_GRANT*)malloc(sizeof(*s));
    if (!s)
    {
        // Out of memory: better the old immediate (possibly failing) revoke than an
        // untracked leak.
        ULONG status = XcGnttabRevokeForeignAccess(ctx->xc, ctx->framebuffer);
        if (status != ERROR_SUCCESS)
            win_perror2(status, "XcGnttabRevokeForeignAccess (park OOM fallback)");
        free(ctx->grant_refs);
    }
    else
    {
        s->framebuffer = ctx->framebuffer;
        s->grant_refs = ctx->grant_refs;
        s->deadline = GetTickCount64() + A6_ACK_TIMEOUT_MS;
        s->timeout_logged = FALSE;
        EnterCriticalSection(&ctx->stale_lock);
        s->next = ctx->stale_grants;
        ctx->stale_grants = s;
        LeaveCriticalSection(&ctx->stale_lock);
        LogInfo("A6PARK old screen grant %p parked, awaiting window-0 MSG_WINDOW_DUMP_ACK", s->framebuffer);
    }
    ctx->grant_refs = NULL;
    ctx->framebuffer = NULL;
}

// A6: one revoke attempt per parked grant. force revokes everything now (ack received,
// exit drain, teardown); !force is the capture-thread tick and touches only grants whose
// ack deadline has passed, logging the fallback loudly once per grant. Failures stay
// queued for the next call - no waiting, no spinning, bounded work per call.
static void StaleGrantSweep(IN OUT CAPTURE_CONTEXT* ctx, IN BOOL force, IN const WCHAR* why)
{
    EnterCriticalSection(&ctx->stale_lock);
    STALE_GRANT** link = &ctx->stale_grants;
    while (*link)
    {
        STALE_GRANT* s = *link;
        if (!force && GetTickCount64() < s->deadline)
        {
            link = &s->next;
            continue;
        }
        if (!force && !s->timeout_logged)
        {
            s->timeout_logged = TRUE;
            LogWarning("A6ACKTIMEOUT no window-0 dump ack within %u ms for old grant %p"
                " - falling back to revoke retries on the capture tick",
                A6_ACK_TIMEOUT_MS, s->framebuffer);
        }
        ULONG status = XcGnttabRevokeForeignAccess(ctx->xc, s->framebuffer);
        if (status == ERROR_SUCCESS)
        {
            LogInfo("A6REVOKE old screen grant %p revoked (%s)", s->framebuffer, why);
            *link = s->next;
            free(s->grant_refs);
            free(s);
        }
        else
        {
            // Expected while dom0 still maps the pages; converges once the daemon
            // processes the superseding dump (or exits).
            LogVerbose("old screen grant %p still busy: 0x%x", s->framebuffer, status);
            link = &s->next;
        }
    }
    LeaveCriticalSection(&ctx->stale_lock);
}

void CaptureRevokeStaleGrants(IN OUT CAPTURE_CONTEXT* ctx, IN const WCHAR* why)
{
    StaleGrantSweep(ctx, TRUE, why);
}

BOOL CaptureHasStaleGrants(IN CAPTURE_CONTEXT* ctx)
{
    EnterCriticalSection(&ctx->stale_lock);
    BOOL any = (ctx->stale_grants != NULL);
    LeaveCriticalSection(&ctx->stale_lock);
    return any;
}

static BOOL RecreateDuplication(IN OUT CAPTURE_CONTEXT* ctx)
{
    const UINT maxAttempts = 20;
    const DWORD retryDelayMs = 250;
    UINT width = 0, height = 0;

    EnterCriticalSection(&ctx->frame.lock);
    if (ctx->frame.texture) // never leak a frame we were holding
    {
        IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
        IDXGIResource_Release(ctx->frame.texture);
        ctx->frame.texture = NULL;
    }
    // ReleaseFrame() can fail at UnMapDesktopSurface and leave `mapped` set, and it frees
    // the rects only on the path it completes. The duplication object is going away, so
    // reset the whole frame state or the replacement inherits stale flags and the next
    // GetFrame asserts on a non-NULL texture / double-unmaps.
    ctx->frame.mapped = FALSE;
    // The desktop surface backing the published framebuffer pointer is going away with this
    // duplication. Synthesis reads that pointer from the window-event thread, outside the
    // frame loop, so it must be dropped here - under frame.lock - not merely overwritten by
    // the next ProcessNewFrame.
    PwInvalidateFramebuffer();
    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
    if (ctx->duplication)
    {
        IDXGIOutputDuplication_Release(ctx->duplication);
        ctx->duplication = NULL;
    }

    // Drop the framebuffer grant too. The desktop surface belongs to the duplication we are
    // discarding; the replacement may map somewhere else entirely. GetFrame only maps and
    // grants when grant_refs is NULL, so leaving it set means the agent never looks at the
    // new surface and the daemon reads the old pages forever - windows stay mapped and
    // correctly positioned, but their contents freeze at the moment of the loss.
    // A6: the grant is PARKED, not revoked - dom0 still maps these pages and releases
    // them only when it processes the superseding window-0 MSG_WINDOW_DUMP; revocation
    // happens on its MSG_WINDOW_DUMP_ACK (or the timeout fallback). Both grants are
    // transiently live, which the grant table comfortably holds (design 2.1).
    if (ctx->xc && ctx->grant_refs)
    {
        if (ctx->uses_staging)
        {
            // STAGING dormant-park-path: with the persistent staging grant there is no
            // re-grant on recovery - dom0 keeps mapping the SAME pages - so the A6
            // park/ack-revoke machinery above has nothing to do for the screen path.
            // Keep the refs: the re-dump after recovery is a pure header refresh over
            // the same grant, and the next frame refills the buffer (full copy below).
            LogInfo("STAGING dormant-park-path (screen grant kept across duplication recreate)");
        }
        else
            ParkStaleScreenGrant(ctx);
    }
    LeaveCriticalSection(&ctx->frame.lock);

    for (UINT attempt = 0; attempt < maxAttempts; attempt++)
    {
        if (!InterlockedCompareExchange(&g_CaptureThreadEnable, FALSE, FALSE))
            return FALSE; // asked to stop while recovering

        // A desktop switch both invalidates the duplication AND leaves this thread on the
        // old desktop, so re-attach before every attempt, not once before the loop: the
        // switch may still be in progress on the first try.
        AttachCaptureThreadToInputDesktop();

        IDXGIOutputDuplication* duplication = GetDuplication(ctx->output, ctx->device, &width, &height);
        if (duplication)
        {
            EnterCriticalSection(&ctx->frame.lock);
            ctx->duplication = duplication;

            if (width != ctx->width || height != ctx->height)
            {
                // STAGING guard: never overflow the granted buffer. A geometry beyond
                // the staging capacity rejects the in-place adoption and takes the old
                // teardown path (return FALSE -> reinitialize; the next
                // CaptureInitialize then falls back to the direct per-geometry grant
                // for the oversized mode).
                if (ctx->uses_staging && !StagingCapacityOk(width, height))
                {
                    LogWarning("STAGING too-large %ux%u > capacity %ux%u - taking teardown path",
                        width, height, g_Staging.cap_width, g_Staging.cap_height);
                    IDXGIOutputDuplication_Release(ctx->duplication);
                    ctx->duplication = NULL;
                    LeaveCriticalSection(&ctx->frame.lock);
                    return FALSE;
                }

                // A6 (approved design, 2.1): adopt the new geometry IN PLACE instead of
                // failing out to the full teardown that unmapped and destroyed every
                // window - the one path a resolution change always took. Everything
                // downstream is already sized from these fields (A3): GetFrame maps the
                // new surface and grants FRAMEBUFFER_PAGE_COUNT(ctx->width, ctx->height)
                // pages, and the frame loop re-sends MSG_WINDOW_DUMP + MSG_CONFIGURE for
                // window 0 from the same fields (main.c, grants_changed).
                LogInfo("A6REGRANT resolution changed during recovery (%ux%u -> %ux%u), adopting in place",
                    ctx->width, ctx->height, width, height);
                ctx->width = width;
                ctx->height = height;
            }

            if (ctx->uses_staging)
            {
                // Same effect the direct path gets from clearing + re-granting in
                // GetFrame: re-send the window-0 dump - now a header refresh over the
                // SAME refs (CaptureGrantPageCount is constant) - and refill the whole
                // staging buffer before the repaint that follows it.
                ctx->grants_changed = TRUE;
                ctx->staging_full_copy = TRUE;
            }
            LeaveCriticalSection(&ctx->frame.lock);

            // INFO, not DEBUG: the guest runs LogLevel=3 by default, so at DEBUG this
            // recovery is invisible and an operator cannot tell in-place recovery from
            // a silent teardown. This line is the evidence that the fix worked.
            LogInfo("duplication recreated in place after %u attempt(s) - windows kept", attempt + 1);
            return TRUE;
        }

        Sleep(retryDelayMs);
    }

    LogError("failed to recreate duplication after %u attempts", maxAttempts);
    return FALSE;
}

static IDXGIOutputDuplication* GetDuplication(IN IDXGIOutput1* output, IN ID3D11Device* device, OUT UINT* width, OUT UINT* height)
{
    LogVerbose("start");
    IDXGIOutputDuplication* duplication = NULL;
    HRESULT status = IDXGIOutput1_DuplicateOutput(output, (IUnknown*)device, &duplication);

    if (FAILED(status))
    {
        win_perror2(status, "output->DuplicateOutput()");
        goto fail;
    }

    DXGI_OUTDUPL_DESC desc;
    IDXGIOutputDuplication_GetDesc(duplication, &desc);
    LogDebug("Got output duplication. Surface dimensions = %ux%u %.2f fps, "
        L"format %d, scanline order %d, mapped in memory %d",
        desc.ModeDesc.Width, desc.ModeDesc.Height,
        (float)desc.ModeDesc.RefreshRate.Numerator / (float)desc.ModeDesc.RefreshRate.Denominator,
        desc.ModeDesc.Format, desc.ModeDesc.ScanlineOrdering,
        desc.DesktopImageInSystemMemory);

    if (!desc.DesktopImageInSystemMemory)
    {
        // this should never happen with the basic display driver
        IDXGIOutputDuplication_Release(duplication);
        LogError("TODO: desktop is not in system memory");
        SetLastError((DWORD)DXGI_ERROR_UNSUPPORTED);
        goto fail;
    }

    *width = desc.ModeDesc.Width;
    *height = desc.ModeDesc.Height;

    LogVerbose("end");
    return duplication;

fail:
    LogVerbose("end");
    return NULL;
}

static void XcLogger(IN XENCONTROL_LOG_LEVEL logLevel, IN const char* function, IN const wchar_t* format, IN va_list args)
{
    wchar_t buf[1024];

    StringCbVPrintfW(buf, sizeof(buf), format, args);
    // XC log levels are the same as ours
    _LogFormat(logLevel, /*raw=*/FALSE, function, buf);
}

// Allocate and grant the staging buffer, once per process. Returns TRUE when the
// grant is live. On failure everything is released and the next CaptureInitialize
// retries (at most one grant attempt per init - no accumulation). Called only from
// CaptureInitialize, so single-threaded by construction.
static BOOL StagingEnsure(void)
{
    if (g_Staging.handle)
        return TRUE;

    if (!g_StagingGrant)
        return FALSE; // registry gate: direct-map A/B build

    UINT capW = (UINT)g_HostScreenWidth;
    UINT capH = (UINT)g_HostScreenHeight;
    if ((size_t)capW * capH < (size_t)STAGING_MIN_WIDTH * STAGING_MIN_HEIGHT)
    {
        capW = STAGING_MIN_WIDTH;
        capH = STAGING_MIN_HEIGHT;
    }

    size_t page_count = FRAMEBUFFER_PAGE_COUNT(capW, capH);
    size_t size = page_count * PAGE_SIZE;

    BYTE* buffer = (BYTE*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
    {
        win_perror("VirtualAlloc(staging framebuffer)");
        return FALSE;
    }

    ULONG* refs = (ULONG*)malloc(page_count * sizeof(ULONG));
    if (!refs)
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return FALSE;
    }

    PXENCONTROL_CONTEXT xc = NULL;
    DWORD status = XcOpen(XcLogger, &xc);
    if (status != ERROR_SUCCESS || !xc)
    {
        win_perror2(status, "XcOpen (staging)");
        free(refs);
        VirtualFree(buffer, 0, MEM_RELEASE);
        return FALSE;
    }
    XcSetLogLevel(xc, LogGetLevel());

    void* handle = NULL;
    status = XcGnttabPermitForeignAccess2(xc,
        g_GuiDomainId,
        buffer,
        (ULONG)page_count,
        0,
        0,
        XENIFACE_GNTTAB_READONLY,
        &handle,
        refs);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "XcGnttabPermitForeignAccess2 (staging)");
        XcClose(xc);
        free(refs);
        VirtualFree(buffer, 0, MEM_RELEASE);
        return FALSE;
    }
    assert(handle == buffer);

    g_Staging.xc = xc;
    g_Staging.buffer = buffer;
    g_Staging.size = size;
    g_Staging.page_count = page_count;
    g_Staging.refs = refs;
    g_Staging.handle = handle;
    g_Staging.cap_width = capW;
    g_Staging.cap_height = capH;

    LogInfo("STAGING granted %lu pages capacity %ux%u", (ULONG)page_count, capW, capH);
    return TRUE;
}

size_t CaptureGrantPageCount(IN const CAPTURE_CONTEXT* ctx)
{
    // Constant full-capacity count with the staging grant; the daemon accepts a count
    // larger than width*height needs and exit(1)s only on a TOO SMALL one (see the
    // declaration in capture.h).
    if (ctx->uses_staging)
        return g_Staging.page_count;
    return FRAMEBUFFER_PAGE_COUNT(ctx->width, ctx->height);
}

void CaptureStagingRevokeOnExit(void)
{
    if (!g_Staging.handle)
        return;

    ULONG status = XcGnttabRevokeForeignAccess(g_Staging.xc, g_Staging.handle);
    if (status != ERROR_SUCCESS)
    {
        // dom0 still maps the pages (daemon alive or wedged). Best effort by design:
        // leak the one buffer loudly rather than spin - the process is exiting and
        // this is a single bounded allocation, not the per-resize accumulation the
        // staging grant exists to prevent.
        LogWarning("STAGING revoke failed on exit (0x%x) - leaking the staging grant", status);
        return;
    }

    LogInfo("STAGING revoked on exit (%lu pages)", (ULONG)g_Staging.page_count);
    XcClose(g_Staging.xc);
    free(g_Staging.refs);
    VirtualFree(g_Staging.buffer, 0, MEM_RELEASE);
    ZeroMemory(&g_Staging, sizeof(g_Staging));
}

// TODO: use callbacks instead of events
CAPTURE_CONTEXT* CaptureInitialize(HANDLE frame_event, HANDLE error_event)
{
    LogVerbose("start");

    CAPTURE_CONTEXT* ctx = (CAPTURE_CONTEXT*)calloc(1, sizeof(CAPTURE_CONTEXT));
    if (!ctx)
    {
        // NEVEREXIT (CONVERT, was exit(ERROR_OUTOFMEMORY)): a failed ~1 KB alloc must
        // fail this capture generation, not the process - the caller's A7 degraded
        // path keeps the vchan alive and retries the init.
        LogError("out of memory allocating CAPTURE_CONTEXT");
        return NULL;
    }

    InitializeCriticalSection(&ctx->frame.lock);
    InitializeCriticalSection(&ctx->stale_lock); // A6: parked-grant list

    DWORD status = XcOpen(XcLogger, &ctx->xc);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "Failed to open xencontrol handle");
        goto fail;
    }

    if (!ctx->xc)
        goto fail;

    XcSetLogLevel(ctx->xc, LogGetLevel()); // XC log levels are the same as ours
    ctx->adapter = GetAdapter();
    if (!ctx->adapter)
        goto fail;

    ctx->device = GetDevice(ctx->adapter);
    if (!ctx->device)
        goto fail;

    ctx->output = GetOutput(ctx->adapter);
    if (!ctx->output)
        goto fail;

    ctx->duplication = GetDuplication(ctx->output, ctx->device, &ctx->width, &ctx->height);
    if (!ctx->duplication)
        goto fail;

    // STAGING: adopt the persistent staging grant when enabled and the geometry fits
    // its capacity. framebuffer/grant_refs become ALIASES of the module-lifetime
    // staging state - with grant_refs pre-set, GetFrame's map-and-grant first-frame
    // branch is never entered, and every teardown/park path is gated on
    // !uses_staging so the aliases are never freed or revoked.
    if (StagingEnsure())
    {
        if (StagingCapacityOk(ctx->width, ctx->height))
        {
            ctx->uses_staging = TRUE;
            ctx->staging_full_copy = TRUE; // first frame fills the whole buffer
            ctx->framebuffer = g_Staging.buffer;
            ctx->grant_refs = g_Staging.refs;
            ctx->granted_once = TRUE;
        }
        else
        {
            // Never overflow the buffer: this generation runs the legacy direct
            // per-geometry grant instead (the old teardown-path behavior).
            LogWarning("STAGING too-large %ux%u > capacity %ux%u - falling back to direct grant",
                ctx->width, ctx->height, g_Staging.cap_width, g_Staging.cap_height);
        }
    }

    // get one frame to acquire framebuffer map (staging: to fill the staging buffer)
    if (FAILED(GetFrame(ctx, 5*FRAME_TIMEOUT)))
        goto fail;

    if (FAILED(ReleaseFrame(ctx)))
        goto fail;

    ctx->frame_event = frame_event;
    ctx->ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx->error_event = error_event;

    LogVerbose("end");
    return ctx;

fail:
    CaptureTeardown(ctx);
    LogVerbose("end (%x)", GetLastError());
    return NULL;
}

// preserves last error
void CaptureTeardown(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");

    DWORD status = GetLastError(); // preserve
    CaptureStop(ctx);

    // Same hazard as in RecreateDuplication: the mapped desktop surface is about to be
    // released, so stop publishing a pointer into it.
    EnterCriticalSection(&ctx->frame.lock);
    PwInvalidateFramebuffer();
    LeaveCriticalSection(&ctx->frame.lock);

    if (ctx->ready_event)
        CloseHandle(ctx->ready_event);

    // STAGING: framebuffer/grant_refs are then aliases of the module-lifetime staging
    // grant, which deliberately SURVIVES capture teardown and reinitialization - the
    // whole point is that the next generation re-uses the same grant, so a resize
    // (teardown + reinit) creates zero grant traffic. Its one revocation point is the
    // A6 exit path (CaptureStagingRevokeOnExit).
    if (ctx->xc && ctx->grant_refs && !ctx->uses_staging)
    {
        // grants are not automatically revoked when the xeniface device handle is closed
        assert(ctx->framebuffer);
        status = XcGnttabRevokeForeignAccess(ctx->xc, ctx->framebuffer);
        if (status != ERROR_SUCCESS)
        {
            win_perror2(status, "XcGnttabRevokeForeignAccess");
        }
    }

    // A6: last chance for any parked grants (the capture thread is already joined).
    // Failures here mean dom0 still maps the pages; mirror perwindow.c's shutdown and
    // leak them loudly rather than free bookkeeping for a live grant.
    if (ctx->xc)
        StaleGrantSweep(ctx, TRUE, L"teardown");
    if (ctx->stale_grants)
        LogWarning("A6LEAK leaking un-revoked superseded screen grant(s) at capture teardown");

    ReleaseFrame(ctx);

    DeleteCriticalSection(&ctx->frame.lock);
    DeleteCriticalSection(&ctx->stale_lock);

    if (ctx->duplication)
        IDXGIOutputDuplication_Release(ctx->duplication);

    if (ctx->output)
        IDXGIOutput1_Release(ctx->output);

    if (ctx->device)
        ID3D11Device_Release(ctx->device);

    if (ctx->adapter)
        IDXGIAdapter_Release(ctx->adapter);

    if (ctx->xc)
        XcClose(ctx->xc);

    free(ctx);
    LogVerbose("end");
    SetLastError(status);
}

HRESULT CaptureStart(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");
    HRESULT status = ERROR_SUCCESS;
    InterlockedExchange(&g_CaptureThreadEnable, TRUE);
    ctx->thread = CreateThread(NULL, 0, CaptureThread, ctx, 0, NULL);
    if (!ctx->thread)
    {
        InterlockedExchange(&g_CaptureThreadEnable, FALSE);
        status = win_perror("CreateThread");
    }

    LogVerbose("end");
    return status;
}

void CaptureStop(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");
    InterlockedExchange(&g_CaptureThreadEnable, FALSE);
    if (ctx->thread)
    {
        if (WaitForSingleObject(ctx->thread, 2 * FRAME_TIMEOUT) != WAIT_OBJECT_0)
        {
            LogWarning("capture thread timeout");
            TerminateThread(ctx->thread, 0);
        }
    }
    ctx->thread = NULL;

    LogVerbose("end");
}

// STAGING: copy the acquired frame into the persistent staging buffer. Runs on the
// capture thread, under ctx->frame.lock, with a frame acquired (the thread already
// owns the frame - no locking change). Maps the DXGI desktop surface, copies
// row-by-row honoring the SOURCE Pitch - which also removes the latent
// pitch != width*4 hazard of the direct-map path, since the staging buffer is
// always tightly packed at width*4 - restricted to the frame's dirty rects unless
// a full copy is pending, then unmaps the surface again. On success the published
// frame pointer/pitch (frame.rect) are swapped to the staging buffer, so every
// consumer (ProcessNewFrame, slice copies, synthesis) reads the pages dom0 maps;
// the locking structure around those consumers is unchanged.
static HRESULT StagingCopyFrame(IN OUT CAPTURE_CONTEXT* ctx)
{
    if (!ctx->staging_full_copy && ctx->frame.dirty_rects_count == 0)
        return S_OK; // no pixels changed; the staging content is already current

    DXGI_MAPPED_RECT src;
    HRESULT status = IDXGIOutputDuplication_MapDesktopSurface(ctx->duplication, &src);
    if (FAILED(status))
    {
        win_perror2(status, "MapDesktopSurface (staging copy)");
        return status;
    }

    if (src.Pitch <= 0)
    {
        // never observed with the DDA; refuse to walk a bogus source layout
        LogError("STAGING source pitch %d unusable", src.Pitch);
        IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
        return E_UNEXPECTED;
    }

    const UINT width = ctx->width;
    const UINT height = ctx->height;
    const size_t dstPitch = (size_t)width * 4;
    RECT full = { 0, 0, (LONG)width, (LONG)height };
    const RECT* rects = &full;
    UINT count = 1;

    if (!ctx->staging_full_copy)
    {
        rects = ctx->frame.dirty_rects;
        count = ctx->frame.dirty_rects_count;
    }

    for (UINT i = 0; i < count; i++)
    {
        RECT r;
        // clip to the current geometry; capacity >= width*height*4 is guaranteed by
        // the StagingCapacityOk gates at init/recreate, so clipped rows cannot
        // overrun the buffer
        if (!IntersectRect(&r, &rects[i], &full))
            continue;
        const BYTE* s = (const BYTE*)src.pBits + (size_t)r.top * src.Pitch + (size_t)r.left * 4;
        BYTE* d = g_Staging.buffer + (size_t)r.top * dstPitch + (size_t)r.left * 4;
        size_t rowBytes = (size_t)(r.right - r.left) * 4;
        for (LONG row = r.top; row < r.bottom; row++)
        {
            memcpy(d, s, rowBytes);
            s += src.Pitch;
            d += dstPitch;
        }
    }
    // %s is WIDE here: the Log* macros L-paste the format (wide printf semantics)
    LogVerbose("STAGING copy %u rect(s)%s", count, ctx->staging_full_copy ? L" (full)" : L"");
    ctx->staging_full_copy = FALSE;

    status = IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
    if (FAILED(status))
        win_perror2(status, "UnMapDesktopSurface (staging copy)"); // copy is done; not fatal

    // Publish the staging buffer as THE frame pixels. frame.mapped stays FALSE: the
    // transient DXGI mapping is already gone, there is nothing for ReleaseFrame (or
    // the fail paths) to undo, and nothing may free the aliased grant_refs.
    ctx->frame.rect.pBits = g_Staging.buffer;
    ctx->frame.rect.Pitch = (INT)dstPitch;
    return S_OK;
}

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout)
{
    LogVerbose("start");
    EnterCriticalSection(&ctx->frame.lock);
    assert(!ctx->frame.texture);

    LONGLONG perf_t0 = PerfNow();
    HRESULT status = IDXGIOutputDuplication_AcquireNextFrame(ctx->duplication,
        timeout, &ctx->frame.info, &ctx->frame.texture);
    ctx->frame.perf.acquire_ticks = PerfNow() - perf_t0;
    if (FAILED(status))
    {
        if (status != DXGI_ERROR_WAIT_TIMEOUT) // don't spam log with timeouts
        {
            win_perror2(status, "duplication->AcquireNextFrame()");
        }
        goto fail1;
    }

    // STAGING: a pending full copy must not take this early-out - after a geometry
    // change the staging content is laid out at the old pitch and must be refilled
    // from the current desktop image even if nothing new was presented.
    if (ctx->frame.info.LastPresentTime.QuadPart == 0 && ctx->grant_refs &&
        !(ctx->uses_staging && ctx->staging_full_copy))
    {
        // only skip here after we shared the framebuffer
        LogVerbose("framebuffer unchanged");
        ctx->frame.mapped = FALSE;
        goto end;
    }

    // we only really need to map the framebuffer to get its pointer for sharing
    if (!ctx->grant_refs)
    {
        LogDebug("1st frame, sharing framebuffer");

        status = IDXGIOutputDuplication_MapDesktopSurface(ctx->duplication, &ctx->frame.rect);
        if (FAILED(status))
        {
            win_perror2(status, "duplication->MapDesktopSurface()");
            goto fail2;
        }

        ctx->frame.mapped = TRUE;

        size_t page_count = FRAMEBUFFER_PAGE_COUNT(ctx->width, ctx->height);
        assert(page_count < ULONG_MAX);
        ctx->grant_refs = malloc(page_count * sizeof(ULONG));

        status = XcGnttabPermitForeignAccess2(ctx->xc,
            g_GuiDomainId,
            ctx->frame.rect.pBits,
            (ULONG)page_count,
            0,
            0,
            XENIFACE_GNTTAB_READONLY,
            &ctx->framebuffer,
            ctx->grant_refs);

        if (status != ERROR_SUCCESS)
        {
            win_perror("sharing framebuffer with GUI domain");
            goto fail3;
        }

        assert(ctx->framebuffer == ctx->frame.rect.pBits);

        // Tell the frame loop to re-send MSG_WINDOW_DUMP with these refs - but only if this
        // is a RE-grant. StartFrameProcessing already sends the refs for the first grant, so
        // flagging that one too would send a duplicate dump and force a redundant full-screen
        // repaint on every startup, logged as if a recovery had happened.
        ctx->grants_changed = ctx->granted_once;
        ctx->granted_once = TRUE;
    }

    // Instrumentation: is GetFrameMoveRects ever non-empty? See the TODO below.
    // MSDN mandates move rects are retrieved before dirty rects, so this must stay here.
    // The results are only measured, never used to produce output.
    ctx->frame.perf.move_rects_count = 0;
    ctx->frame.perf.moverect_ticks = 0;
    if (g_PerfEnabled)
    {
        DXGI_OUTDUPL_MOVE_RECT move_rects[64];
        UINT mr_size = 0;
        RECT no_rect = { 0, 0, 0, 0 };

        perf_t0 = PerfNow();
        HRESULT mr_status = IDXGIOutputDuplication_GetFrameMoveRects(ctx->duplication,
            (UINT)sizeof(move_rects), move_rects, &mr_size);
        ctx->frame.perf.moverect_ticks = PerfNow() - perf_t0;

        if (SUCCEEDED(mr_status))
        {
            ctx->frame.perf.move_rects_count = mr_size / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            if (ctx->frame.perf.move_rects_count > 0)
                PerfNoteMoveRects(ctx->frame.perf.move_rects_count,
                    move_rects[0].SourcePoint.x, move_rects[0].SourcePoint.y,
                    &move_rects[0].DestinationRect);
        }
        else if (mr_status == DXGI_ERROR_MORE_DATA)
        {
            // more than ARRAYSIZE(move_rects) move rects: report the count, don't retry
            ctx->frame.perf.move_rects_count = mr_size / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            PerfNoteMoveRects(ctx->frame.perf.move_rects_count, 0, 0, &no_rect);
        }
        else
        {
            ctx->frame.perf.move_rects_count = (UINT)-1; // query failed
        }
    }

    // dirty rects
    perf_t0 = PerfNow();
    UINT dr_size = 1; // initial buffer can't be empty
    RECT temp_rect;

    // query required size
    ctx->frame.dirty_rects = NULL;
    status = IDXGIOutputDuplication_GetFrameDirtyRects(ctx->duplication, dr_size, &temp_rect, &dr_size);
    if (FAILED(status) && status != DXGI_ERROR_MORE_DATA)
    {
        win_perror2(status, "initial GetFrameDirtyRects");
        goto fail4;
    }

    ctx->frame.dirty_rects = (RECT*)malloc(dr_size);
    if (!ctx->frame.dirty_rects)
    {
        win_perror2(ERROR_OUTOFMEMORY, "allocating dirty rects buffer");
        goto fail4;
    }

    status = IDXGIOutputDuplication_GetFrameDirtyRects(ctx->duplication, dr_size, ctx->frame.dirty_rects, &dr_size);
    if (FAILED(status))
    {
        win_perror2(status, "GetFrameDirtyRects");
        goto fail4;
    }

    ctx->frame.dirty_rects_count = dr_size / sizeof(RECT);
    ctx->frame.perf.dirtyrect_ticks = PerfNow() - perf_t0;

    // Instrumentation: total damaged area, computed outside the timed region so
    // it doesn't inflate the number it annotates. Capture thread only.
    ctx->frame.perf.dirty_area = 0;
    if (g_PerfEnabled)
    {
        for (UINT i = 0; i < ctx->frame.dirty_rects_count; i++)
        {
            ctx->frame.perf.dirty_area +=
                (UINT64)(ctx->frame.dirty_rects[i].right - ctx->frame.dirty_rects[i].left) *
                (UINT64)(ctx->frame.dirty_rects[i].bottom - ctx->frame.dirty_rects[i].top);
        }
    }

#ifdef _DEBUG
    LogVerbose("%u dirty rects", ctx->frame.dirty_rects_count);
    for (UINT i = 0; i < ctx->frame.dirty_rects_count; i++)
        LogVerbose("DR#%u: (%d,%d) %dx%d", i, ctx->frame.dirty_rects[i].left, ctx->frame.dirty_rects[i].top,
            ctx->frame.dirty_rects[i].right - ctx->frame.dirty_rects[i].left,
            ctx->frame.dirty_rects[i].bottom - ctx->frame.dirty_rects[i].top);
#endif

    // TODO: GetFrameMoveRects (they seem to always be empty when testing)
    // MSDN note: To produce a visually accurate copy of the desktop,
    // an application must first process all move RECTs before it processes dirty RECTs.

    // STAGING: copy this frame's changes into the persistently granted buffer. Needs
    // the dirty rects, so it runs after their retrieval; the DXGI surface is mapped
    // and unmapped inside (frame.mapped stays FALSE - see the fail3 note below).
    if (ctx->uses_staging)
    {
        status = StagingCopyFrame(ctx);
        if (FAILED(status))
            goto fail4;
    }

end:
    LeaveCriticalSection(&ctx->frame.lock);
    LogVerbose("end");
    return 0;

fail4:
    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
fail3:
    // Only the direct-map grant branch arrives here with frame.mapped set; the staging
    // path never sets it (StagingCopyFrame unmaps before returning), so the aliased
    // staging grant_refs can never be freed here.
    if (ctx->frame.mapped)
    {
        free(ctx->grant_refs);
        ctx->grant_refs = NULL;
        IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
    }
fail2:
    IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
fail1:
    LogVerbose("end (%x)", status);
    ctx->frame.texture = NULL;
    LeaveCriticalSection(&ctx->frame.lock);
    SetLastError(status);
    return status;
}

static HRESULT ReleaseFrame(IN OUT CAPTURE_CONTEXT* ctx)
{
    LogVerbose("start");
    EnterCriticalSection(&ctx->frame.lock);
    HRESULT status = ERROR_INVALID_PARAMETER;
    if (!ctx->frame.texture)
        goto end;

    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;

    if (ctx->frame.mapped)
    {
        status = IDXGIOutputDuplication_UnMapDesktopSurface(ctx->duplication);
        if (FAILED(status))
        {
            win_perror2(status, "duplication->UnMapDesktopSurface");
            goto end;
        }
        ctx->frame.mapped = FALSE;
    }

    status = IDXGIResource_Release(ctx->frame.texture);
    if (FAILED(status))
    {
        win_perror2(status, "frame->Release");
        goto end;
    }

    ctx->frame.texture = NULL;

    status = IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
    if (FAILED(status))
    {
        win_perror2(status, "duplication->ReleaseFrame");
        goto end;
    }

    status = ERROR_SUCCESS;
end:
    LeaveCriticalSection(&ctx->frame.lock);
    LogVerbose("end (%x)", status);
    return status;
}

static DWORD WINAPI CaptureThread(void* param)
{
    DWORD status = ERROR_SUCCESS;
    CAPTURE_CONTEXT* capture = (CAPTURE_CONTEXT*)param;
    LogDebug("starting, resolution %ux%u", capture->width, capture->height);

    while (TRUE)
    {
        LogVerbose("loop start");
        if (!InterlockedCompareExchange(&g_CaptureThreadEnable, FALSE, FALSE))
        {
            LogDebug("stopping (disabled)");
            break;
        }

        // A6 timeout fallback: this loop runs at least once per FRAME_TIMEOUT even on an
        // idle desktop, so parked grants whose ack deadline passed are retried here.
        // One attempt per grant per pass - never a wait.
        StaleGrantSweep(capture, FALSE, L"timeout");

        status = GetFrame(capture, FRAME_TIMEOUT);
        if (FAILED(status))
        {
            if (status == DXGI_ERROR_WAIT_TIMEOUT)
            {
                LogVerbose("frame timeout");
                continue; // no new frame available, wait for next one
            }

            // DXGI_ERROR_ACCESS_LOST (0x887A0026) is the ROUTINE "your duplication object
            // is stale, make a new one" signal: it fires on mode changes, desktop
            // switches, and every trip to the secure desktop (UAC / Ctrl-Alt-Del).
            // DXGI_ERROR_ACCESS_DENIED (0x887A002B) is the secure desktop itself.
            //
            // Tearing everything down for these is wrong and user-visible: the main loop
            // reinitialises and unmaps EVERY window, so the qube's windows vanish and
            // reappear in dom0 after an entirely expected event. It also makes
            // resolution changes (and therefore any future resize-follows-dom0-window
            // feature) glitch by construction, since those raise ACCESS_LOST constantly.
            //
            // Note the log message this used to produce was itself misleading:
            // win_perror2 renders 0x887A0026 through FormatMessage(FROM_SYSTEM), which
            // prints the unrelated string "The keyed mutex was abandoned." There is no
            // keyed mutex involved - see instrumentation/ACCESS-LOST-BUG.md.
            if (status == DXGI_ERROR_ACCESS_LOST || status == DXGI_ERROR_ACCESS_DENIED)
            {
                if (RecreateDuplication(capture))
                    continue; // recovered in place, window list untouched

                LogError("duplication lost and could not be recreated, reinitializing");
            }
            else
            {
                LogWarning("failed to get frame");
            }

            InterlockedExchange(&g_CaptureThreadEnable, FALSE);
            // notify main loop, it'll reinitialize everything
            SetEvent(capture->error_event);
            break;
        }

        if (capture->frame.dirty_rects_count == 0)
        {
            PerfNoteSkippedFrame();
            goto end_frame; // framebuffer contents not changed
        }

        // notify main loop that there's a new frame
        capture->frame.perf.signal_qpc = PerfNow();
        SetEvent(capture->frame_event);

        // wait until main loop processes the frame
        // XXX arbitrary timeout
        if (WaitForSingleObject(capture->ready_event, 1000) != WAIT_OBJECT_0)
        {
            LogWarning("error/timeout waiting for frame processing");
            // probably something bad happened, exit
            status = ERROR_TIMEOUT;
            ReleaseFrame(capture);
            SetEvent(capture->error_event);
            break;
        }

end_frame:
        status = ReleaseFrame(capture);
        if (FAILED(status))
        {
            // ACCESS_LOST is raised here far more often than from AcquireNextFrame: by the
            // time a desktop switch or mode change lands we are typically holding a frame,
            // so the invalidation surfaces on release. Verified on the live guest - a
            // desktop switch produced two ReleaseFrame 0x887A0026 failures and six window
            // unmaps, with the acquire-side recovery never firing. Handle it identically
            // here or the in-place recovery is unreachable in practice.
            if (status == DXGI_ERROR_ACCESS_LOST || status == DXGI_ERROR_ACCESS_DENIED)
            {
                if (RecreateDuplication(capture))
                    continue; // recovered in place, window list untouched

                LogError("duplication lost on release and could not be recreated, reinitializing");
            }

            LogDebug("signaling error due to failed frame release");
            SetEvent(capture->error_event);
            break;
        }
    }
    LogDebug("exiting");
    return status;
}
