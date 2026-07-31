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

volatile LONG g_CaptureThreadEnable = 0;

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout);
static HRESULT ReleaseFrame(IN OUT CAPTURE_CONTEXT* ctx);
static DWORD WINAPI CaptureThread(void* param);

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

    if (previous)
        CloseDesktop(previous);
    previous = desktop;
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
    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
    if (ctx->duplication)
    {
        IDXGIOutputDuplication_Release(ctx->duplication);
        ctx->duplication = NULL;
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
            LeaveCriticalSection(&ctx->frame.lock);

            if (width != ctx->width || height != ctx->height)
            {
                // The framebuffer geometry changed underneath us; the grants and the
                // dom0-side window are sized for the old one, so a full reinit is the
                // correct response here (unlike a plain ACCESS_LOST).
                LogWarning("resolution changed during recovery (%ux%u -> %ux%u), reinitializing",
                    ctx->width, ctx->height, width, height);
                return FALSE;
            }

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

// TODO: use callbacks instead of events
CAPTURE_CONTEXT* CaptureInitialize(HANDLE frame_event, HANDLE error_event)
{
    LogVerbose("start");

    CAPTURE_CONTEXT* ctx = (CAPTURE_CONTEXT*)calloc(1, sizeof(CAPTURE_CONTEXT));
    if (!ctx)
        exit(ERROR_OUTOFMEMORY);

    InitializeCriticalSection(&ctx->frame.lock);

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

    // get one frame to acquire framebuffer map
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

    if (ctx->ready_event)
        CloseHandle(ctx->ready_event);

    if (ctx->xc && ctx->grant_refs)
    {
        // grants are not automatically revoked when the xeniface device handle is closed
        assert(ctx->framebuffer);
        status = XcGnttabRevokeForeignAccess(ctx->xc, ctx->framebuffer);
        if (status != ERROR_SUCCESS)
        {
            win_perror2(status, "XcGnttabRevokeForeignAccess");
        }
    }

    ReleaseFrame(ctx);

    DeleteCriticalSection(&ctx->frame.lock);

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

static HRESULT GetFrame(IN OUT CAPTURE_CONTEXT* ctx, IN UINT timeout)
{
    LogVerbose("start");
    EnterCriticalSection(&ctx->frame.lock);
    assert(!ctx->frame.texture);

    HRESULT status = IDXGIOutputDuplication_AcquireNextFrame(ctx->duplication,
        timeout, &ctx->frame.info, &ctx->frame.texture);
    if (FAILED(status))
    {
        if (status != DXGI_ERROR_WAIT_TIMEOUT) // don't spam log with timeouts
        {
            win_perror2(status, "duplication->AcquireNextFrame()");
        }
        goto fail1;
    }

    if (ctx->frame.info.LastPresentTime.QuadPart == 0 && ctx->grant_refs)
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
    }

    // dirty rects
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

end:
    LeaveCriticalSection(&ctx->frame.lock);
    LogVerbose("end");
    return 0;

fail4:
    free(ctx->frame.dirty_rects);
    ctx->frame.dirty_rects = NULL;
    ctx->frame.dirty_rects_count = 0;
fail3:
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
            goto end_frame; // framebuffer contents not changed

        // notify main loop that there's a new frame
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
