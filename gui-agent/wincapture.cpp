// wincapture - Windows.Graphics.Capture per-window capture engine (see wincapture.h).
//
// Design constraints, from measurement (FINDINGS.md 2026-07-31):
//  - WGC on Win10 19044 has no DirtyRegions and redelivers ~40 fps for UNCHANGED
//    windows: every frame is row-diffed against the target buffer and dropped if
//    identical, otherwise only the changed row band is written and reported.
//  - Session creation intermittently fails (0x80070057 observed under churn): a
//    failure is returned to the caller, which keeps the window on the legacy path.
//  - Frames whose ContentSize disagrees with the expected capture size are skipped
//    (resize in flight; the resize path rebuilds the channel).

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h> // raw ABI, for the exception-free support probe

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <vector>
#include <memory>
#include <atomic>
#include <cstring>

#include "wincapture.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

struct Channel
{
    HWND hwnd = nullptr;
    int width = 0, height = 0;   // visible (buffer) size
    int cropX = 0, cropY = 0;    // visible-rect offset inside the captured window rect
    int capW = 0, capH = 0;      // captured (window-rect) size = pool/staging size
    BYTE* buffer = nullptr;      // caller-owned, width*height*4
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    com_ptr<ID3D11Texture2D> staging;
    bool dead = false;
};

struct Engine
{
    com_ptr<ID3D11Device> dev;
    com_ptr<ID3D11DeviceContext> ctx;
    IDirect3DDevice rtdev{ nullptr };
    HANDLE thread = nullptr;
    std::atomic<bool> quit{ false };
    SRWLOCK lock = SRWLOCK_INIT;                 // guards channels vector
    std::vector<std::unique_ptr<Channel>> channels;
    WC_DAMAGE_CALLBACK callback = nullptr;
};

Engine* g_eng = nullptr;

// The agent's threads never initialize a WinRT/COM apartment (wgcprobe did it in
// main(), which is why the probe worked and the agent's IsSupported() threw). Make
// every C-API entry point idempotently ensure one; RPC_E_CHANGED_MODE means the thread
// already has an (STA) apartment, which WinRT statics are fine with.
void EnsureApartment()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
        throw hresult_error(hr);
}

bool MakeD3D(Engine& e)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, e.dev.put(), nullptr,
                                 e.ctx.put())))
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION, e.dev.put(),
                                     nullptr, e.ctx.put())))
            return false;
    com_ptr<IDXGIDevice> dxgi = e.dev.as<IDXGIDevice>();
    com_ptr<::IInspectable> insp;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
        return false;
    e.rtdev = insp.as<IDirect3DDevice>();
    return true;
}

GraphicsCaptureItem ItemForWindow(HWND hwnd)
{
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    check_hresult(interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(),
                                           put_abi(item)));
    return item;
}

com_ptr<ID3D11Texture2D> TexFromSurface(IDirect3DSurface const& surf)
{
    auto access = surf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex;
    check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), tex.put_void()));
    return tex;
}

// Copy the cropped region into the caller buffer, diffing rows. Returns TRUE and the
// changed row band [y0, y1] if anything changed. Alpha byte is forced opaque: the
// daemon treats the buffer as 32bpp BGRX and some WGC frames carry 0 alpha.
bool DiffCopy(Channel& c, const BYTE* src, UINT pitch, int* y0out, int* y1out)
{
    int y0 = -1, y1 = -1;
    const size_t rowBytes = (size_t)c.width * 4;
    for (int y = 0; y < c.height; y++)
    {
        const BYTE* srow = src + (size_t)(y + c.cropY) * pitch + (size_t)c.cropX * 4;
        BYTE* drow = c.buffer + (size_t)y * rowBytes;
        if (memcmp(drow, srow, rowBytes) != 0)
        {
            memcpy(drow, srow, rowBytes);
            if (y0 < 0)
                y0 = y;
            y1 = y;
        }
    }
    if (y0 < 0)
        return false;
    *y0out = y0;
    *y1out = y1;
    return true;
}

// returns true if a frame was processed
bool PollChannel(Engine& e, Channel& c)
{
    if (c.dead)
        return false;
    auto frame = c.pool.TryGetNextFrame();
    if (!frame)
        return false;

    auto cs = frame.ContentSize();
    if (cs.Width != c.capW || cs.Height != c.capH)
        return true; // resize in flight; drop the frame, resize path will rebuild

    auto tex = TexFromSurface(frame.Surface());
    e.ctx->CopyResource(c.staging.get(), tex.get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(e.ctx->Map(c.staging.get(), 0, D3D11_MAP_READ, 0, &map)))
        return true;
    int y0 = 0, y1 = 0;
    bool changed = DiffCopy(c, (const BYTE*)map.pData, map.RowPitch, &y0, &y1);
    e.ctx->Unmap(c.staging.get(), 0);

    if (changed && e.callback)
        e.callback(c.hwnd, 0, y0, c.width, y1 - y0 + 1);
    return true;
}

DWORD WINAPI CaptureThread(LPVOID param)
{
    Engine& e = *(Engine*)param;
    init_apartment(apartment_type::multi_threaded);
    while (!e.quit.load())
    {
        bool any = false;
        AcquireSRWLockShared(&e.lock);
        for (auto& c : e.channels)
        {
            try
            {
                any = PollChannel(e, *c) || any;
            }
            catch (...)
            {
                c->dead = true; // session died (window gone mid-poll etc.)
            }
        }
        ReleaseSRWLockShared(&e.lock);
        if (!any)
            Sleep(4);
    }
    uninit_apartment();
    return 0;
}

} // namespace

extern "C" {

// Raw-ABI probe: no C++/WinRT projection, no exceptions - returns the genuine HRESULT
// of whichever stage fails (RoInitialize / factory activation / IsSupported).
ULONG WcProbeSupport(void)
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return (ULONG)hr;

    HSTRING cls = nullptr;
    hr = WindowsCreateString(
        RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
        (UINT32)wcslen(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession),
        &cls);
    if (FAILED(hr))
        return (ULONG)hr;

    ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics* st = nullptr;
    hr = RoGetActivationFactory(cls,
        __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics),
        (void**)&st);
    WindowsDeleteString(cls);
    if (FAILED(hr))
        return (ULONG)hr;

    boolean ok = 0;
    hr = st->IsSupported(&ok);
    st->Release();
    if (FAILED(hr))
        return (ULONG)hr;
    return ok ? 0 : ERROR_NOT_SUPPORTED;
}

BOOL WcIsSupported(void)
{
    return WcProbeSupport() == 0 ? TRUE : FALSE;
}

ULONG WcInit(WC_DAMAGE_CALLBACK callback)
{
    if (g_eng)
        return ERROR_ALREADY_INITIALIZED;
    try
    {
        EnsureApartment();
        auto e = std::make_unique<Engine>();
        e->callback = callback;
        if (!MakeD3D(*e))
            return ERROR_DEVICE_NOT_AVAILABLE;
        e->thread = CreateThread(nullptr, 0, CaptureThread, e.get(), 0, nullptr);
        if (!e->thread)
            return GetLastError();
        g_eng = e.release();
        return ERROR_SUCCESS;
    }
    catch (hresult_error const& ex)
    {
        return (ULONG)ex.code().value;
    }
    catch (...)
    {
        return ERROR_UNIDENTIFIED_ERROR;
    }
}

void WcShutdown(void)
{
    if (!g_eng)
        return;
    g_eng->quit.store(true);
    WaitForSingleObject(g_eng->thread, 5000);
    CloseHandle(g_eng->thread);
    AcquireSRWLockExclusive(&g_eng->lock);
    for (auto& c : g_eng->channels)
    {
        try
        {
            if (c->session) c->session.Close();
            if (c->pool) c->pool.Close();
        }
        catch (...) {}
    }
    g_eng->channels.clear();
    ReleaseSRWLockExclusive(&g_eng->lock);
    delete g_eng;
    g_eng = nullptr;
}

ULONG WcAddWindow(HWND hwnd, int width, int height, int cropX, int cropY, void* buffer)
{
    if (!g_eng)
        return ERROR_NOT_READY;
    if (width <= 0 || height <= 0 || !buffer)
        return ERROR_INVALID_PARAMETER;
    try
    {
        EnsureApartment();
        RECT wr;
        if (!GetWindowRect(hwnd, &wr))
            return GetLastError();
        auto c = std::make_unique<Channel>();
        c->hwnd = hwnd;
        c->width = width;
        c->height = height;
        c->cropX = cropX;
        c->cropY = cropY;
        // The live window rect can lag the tracked size mid-resize; size the pool to
        // cover BOTH so a transient mismatch skips frames (ContentSize check below)
        // instead of failing the attach. The next tracking pass rebuilds cleanly.
        c->capW = wr.right - wr.left;
        c->capH = wr.bottom - wr.top;
        if (c->capW < cropX + width) c->capW = cropX + width;
        if (c->capH < cropY + height) c->capH = cropY + height;
        c->buffer = (BYTE*)buffer;

        auto item = ItemForWindow(hwnd);
        c->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_eng->rtdev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            { c->capW, c->capH });
        c->session = c->pool.CreateCaptureSession(item);
        try { c->session.IsCursorCaptureEnabled(false); } catch (...) {}

        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = (UINT)c->capW;
        sd.Height = (UINT)c->capH;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check_hresult(g_eng->dev->CreateTexture2D(&sd, nullptr, c->staging.put()));
        c->session.StartCapture();

        AcquireSRWLockExclusive(&g_eng->lock);
        g_eng->channels.push_back(std::move(c));
        ReleaseSRWLockExclusive(&g_eng->lock);
        return ERROR_SUCCESS;
    }
    catch (hresult_error const& ex)
    {
        return (ULONG)ex.code().value;
    }
    catch (...)
    {
        return ERROR_UNIDENTIFIED_ERROR;
    }
}

BOOL WcIsDead(HWND hwnd)
{
    if (!g_eng)
        return TRUE;
    BOOL dead = TRUE;
    AcquireSRWLockShared(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            dead = ch->dead ? TRUE : FALSE;
            break;
        }
    ReleaseSRWLockShared(&g_eng->lock);
    return dead;
}

void WcRemoveWindow(HWND hwnd)
{
    if (!g_eng)
        return;
    AcquireSRWLockExclusive(&g_eng->lock);
    for (size_t i = 0; i < g_eng->channels.size(); i++)
    {
        if (g_eng->channels[i]->hwnd == hwnd)
        {
            try
            {
                if (g_eng->channels[i]->session) g_eng->channels[i]->session.Close();
                if (g_eng->channels[i]->pool) g_eng->channels[i]->pool.Close();
            }
            catch (...) {}
            g_eng->channels.erase(g_eng->channels.begin() + i);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_eng->lock);
}

ULONG WcPrefill(HWND hwnd)
{
    if (!g_eng)
        return ERROR_NOT_READY;
    // find the channel (shared lock is enough; buffer writes race benignly with the
    // capture thread - both write identical current content)
    Channel* c = nullptr;
    AcquireSRWLockShared(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            c = ch.get();
            break;
        }
    ReleaseSRWLockShared(&g_eng->lock);
    if (!c)
        return ERROR_NOT_FOUND;

    ULONG status = ERROR_SUCCESS;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = c->capW;
    bi.bmiHeader.biHeight = -c->capH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    VOID* bits = nullptr;
    HDC memdc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memdc || !bmp)
    {
        status = ERROR_NOT_ENOUGH_MEMORY;
        goto end;
    }
    {
        HGDIOBJ old = SelectObject(memdc, bmp);
        BOOL ok = PrintWindow(hwnd, memdc, PW_RENDERFULLCONTENT);
        GdiFlush();
        if (ok)
        {
            for (int y = 0; y < c->height; y++)
                memcpy(c->buffer + (size_t)y * c->width * 4,
                       (BYTE*)bits + (size_t)(y + c->cropY) * c->capW * 4 +
                           (size_t)c->cropX * 4,
                       (size_t)c->width * 4);
        }
        else
        {
            status = ERROR_UNIDENTIFIED_ERROR;
        }
        SelectObject(memdc, old);
    }
end:
    if (bmp) DeleteObject(bmp);
    if (memdc) DeleteDC(memdc);
    return status;
}

} // extern "C"
