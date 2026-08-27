// wincapture - per-window content capture engine (see wincapture.h).
//
// ENGINE CHOICE, from e2e measurement on the target guest (FINDINGS.md 2026-08-01):
// Windows.Graphics.Capture cannot be activated in the agent's process context (SYSTEM
// token in session 1): IsSupported() fails with 0x8007000E and real CreateForWindow
// throws too, while the identical code works from a user-context probe. PrintWindow
// with PW_RENDERFULLCONTENT was proven byte-correct for fully occluded windows on this
// guest (Gate 0, 3/3 runs, negative control held), works under GDI in this context,
// and needs no session broker. So the engine is PrintWindow-based:
//
//  - the agent's frame path calls WcMarkDirty(hwnd) when DDA dirty rects intersect a
//    window (active windows refresh within one frame period);
//  - a round-robin sweep refreshes one attached window per pass regardless, so windows
//    occluded in the GUEST (whose updates never appear in DDA dirty rects, but which
//    dom0 may be showing on top) converge too;
//  - every capture is row-diffed against the window's granted buffer: unchanged
//    content costs one compare and produces NO vchan traffic; changed rows are copied
//    and reported as window-relative damage.
//
// The fresh-DIB-per-capture structure deliberately mirrors pwprobe's proven capture
// path (a reused-DIB fast path AV'd unexplained in bench; see FINDINGS).

#include <windows.h>

#include <vector>
#include <memory>
#include <atomic>
#include <cstring>

#include "wincapture.h"
#include "faultinject.h"

#include <log.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

const DWORD SWEEP_INTERVAL_MS = 250;  // one round-robin slot per this interval
const int DEAD_AFTER_FAILURES = 5;    // consecutive PrintWindow failures => dead

struct Channel
{
    HWND hwnd = nullptr;
    int width = 0, height = 0;   // visible (buffer) size
    int cropX = 0, cropY = 0;    // visible-rect offset inside the OS window rect
    BYTE* buffer = nullptr;      // caller-owned, width*height*4
    std::atomic<bool> dirty{ true }; // capture requested (starts dirty: initial fill)
    // While TRUE the frame loop owns the buffer (DDA slice copies) and the engine must
    // not write it: the async capture loop leaves the channel alone (a pending dirty
    // stays pending and is honoured when ownership drops), and the round-robin sweep
    // skips it - the sweep exists for guest-occluded windows invisible to DDA, and a
    // DDA-active window is by definition foreground and unoccluded, so sweeping it is
    // a full PrintWindow + whole-buffer diff 4x/s for nothing (measured: the agent's
    // ~2.5-point idle CPU floor over stock). Direct WcPrefill calls are unaffected -
    // the frame loop uses them to establish the buffer it is taking ownership of.
    std::atomic<bool> ddaOwned{ false };
    int failures = 0;
    bool dead = false;
    // Telemetry (added for the 2026-08-27 field black-window diagnosis: this engine
    // previously logged NOTHING, so a dead channel and a black-rendering one were both
    // indistinguishable from healthy in every agent log).
    DWORD lastErr = 0;      // last PrintWindow/GetWindowRect failure code
    int blackRun = 0;       // consecutive successful captures that were all-black
    bool blackLogged = false;
    // Buffer-relative regions this channel must NOT write: they are owned by
    // synthesized child windows and are patched in from the composited desktop by
    // the frame loop (see PwPatchSynthChildren in main.c). Without the mask, each
    // capture would row-diff the patched pixels against PrintWindow's owner-only
    // content and overwrite them every pass - the popup would flicker.
    RECT mask[WC_MAX_MASK] = {};
    int maskCount = 0;
};

struct Engine
{
    HANDLE thread = nullptr;
    std::atomic<bool> quit{ false };
    SRWLOCK lock = SRWLOCK_INIT;                 // guards channels vector
    std::vector<std::unique_ptr<Channel>> channels;
    WC_DAMAGE_CALLBACK callback = nullptr;
    size_t sweepNext = 0;
    DWORD lastSweep = 0;
};

Engine* g_eng = nullptr;

void AttachThreadToInputDesktop()
{
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (d)
    {
        SetThreadDesktop(d);
        // handle intentionally kept for the thread's lifetime
    }
}

// One pending damage report, produced under the engine lock and fired AFTER it is
// released (the callback re-enters agent locks that the frame thread holds while
// calling WcMarkDirty - firing it under the engine lock is a lock-order inversion that
// deadlocks the whole frame pipeline).
struct DamageOut
{
    HWND hwnd;
    int w, y0, y1;
};

// PrintWindow the window into a fresh DIB, diff the cropped region against the
// caller buffer, copy changed rows. If content changed, *out receives the row band and
// the function does NOT call the callback (the caller fires it after unlocking).
// Returns FALSE only if PrintWindow itself failed.
bool CaptureAndDiff(Engine& e, Channel& c, DamageOut* out)
{
    (void)e;
    // PrintWindow round-trips synchronously into the target app with no timeout: a hung
    // app would park this thread inside the engine lock, and anything then waiting for
    // the lock exclusively (window removal, running under g_csWatchedWindows) would
    // freeze the whole agent behind it. Skip hung windows as a transient condition; the
    // periodic sweep re-marks them dirty, so they catch up when they recover.
    if (IsHungAppWindow(c.hwnd))
        return true;
    RECT wr;
    if (!GetWindowRect(c.hwnd, &wr))
    {
        c.lastErr = GetLastError();
        return false;
    }
    int capW = wr.right - wr.left, capH = wr.bottom - wr.top;
    if (capW < c.cropX + c.width) capW = c.cropX + c.width;
    if (capH < c.cropY + c.height) capH = c.cropY + c.height;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = capW;
    bi.bmiHeader.biHeight = -capH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    VOID* bits = nullptr;
    HDC memdc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!memdc || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (memdc) DeleteDC(memdc);
        return true; // transient resource issue; not a window failure
    }
    HGDIOBJ old = SelectObject(memdc, bmp);
    BOOL ok = PrintWindow(c.hwnd, memdc, PW_RENDERFULLCONTENT);
    if (ok && FiPrintWindowFail())
        ok = FALSE; // injected: exercise the failure path incl. the WCDEAD latch
    if (!ok)
        c.lastErr = GetLastError();
    GdiFlush();

    int y0 = -1, y1 = -1;
    if (ok)
    {
        // The row walk below is a single left-to-right sweep and only works if the mask
        // rects are ordered by left edge: it copies [segStart, mask[seg].left) and then
        // jumps segStart to mask[seg].right. The agent supplies them in
        // watched-window-list order (SynthUpdateMask walks that list), which is
        // DISCOVERY order, not x order (that list is only ever InsertTailList'd). With
        // two synthesized children on one owner sharing rows - up to WC_MAX_MASK are
        // admitted, see SynthOwnerQualifies - and the LEFT one discovered second, the
        // first step copies [0, right-child.left), which spans the left child's columns,
        // so every capture overwrites that child's patched pixels with PrintWindow's
        // owner-only content. PrintWindow does not render owned popups, so the child
        // reads as a blank rectangle until real damage makes the frame loop re-patch it.
        // Sorting a COPY makes the sweep correct for any input order; overlapping and
        // contained rects stay absorbed by the segEnd/nextStart clamps below. The
        // channel's own array must not be sorted in place: captures hold the engine lock
        // only SHARED, so two of them (the capture thread and WcPrefill on the frame
        // thread) can run at once and would be writing it concurrently.
        RECT mask[WC_MAX_MASK] = {};
        for (int i = 0; i < c.maskCount; i++)
            mask[i] = c.mask[i];
        for (int i = 1; i < c.maskCount; i++) // insertion sort, at most WC_MAX_MASK entries
        {
            const RECT key = mask[i];
            int j = i - 1;
            while (j >= 0 && mask[j].left > key.left)
            {
                mask[j + 1] = mask[j];
                j--;
            }
            mask[j + 1] = key;
        }

        const size_t rowBytes = (size_t)c.width * 4;
        for (int y = 0; y < c.height; y++)
        {
            const BYTE* srow = (const BYTE*)bits +
                (size_t)(y + c.cropY) * capW * 4 + (size_t)c.cropX * 4;
            BYTE* drow = c.buffer + (size_t)y * rowBytes;

            // Masked rows are compared/copied in the segments between the masked
            // column ranges, so synthesized-child pixels survive untouched.
            int segStart = 0;
            bool rowChanged = false;
            for (int seg = 0; seg <= c.maskCount; seg++)
            {
                int segEnd = c.width;
                int nextStart = c.width;
                if (seg < c.maskCount)
                {
                    const RECT& m = mask[seg];
                    if (y < m.top || y >= m.bottom)
                        continue; // mask does not cover this row: no split here
                    segEnd = m.left < segStart ? segStart : (m.left > c.width ? c.width : m.left);
                    nextStart = m.right < segStart ? segStart : (m.right > c.width ? c.width : m.right);
                }
                if (segEnd > segStart)
                {
                    size_t off = (size_t)segStart * 4;
                    size_t len = (size_t)(segEnd - segStart) * 4;
                    if (memcmp(drow + off, srow + off, len) != 0)
                    {
                        memcpy(drow + off, srow + off, len);
                        rowChanged = true;
                    }
                }
                segStart = nextStart > segStart ? nextStart : segStart;
                if (segStart >= c.width)
                    break;
            }
            if (rowChanged)
            {
                if (y0 < 0)
                    y0 = y;
                y1 = y;
            }
        }

        // Black-capture telemetry (field diagnosis 2026-08-27): a PrintWindow that
        // SUCCEEDS but renders (essentially) black - DirectComposition content it
        // cannot reach - produced logs identical to a healthy capture; dom0 showed a
        // black window with nothing to go on. Sample the cropped region (1/64 of
        // pixels, RGB only - PrintWindow leaves alpha 0); a capture counts as black
        // when >= 99% of samples are near-black (< 0x0C per channel: a partial render
        // can still paint the 1px frame, and real windows measure <= 4% near-black -
        // 25x separation). Latch one warning per channel after 3 consecutive black
        // captures, one info line if content returns.
        int samples = 0, nearBlack = 0;
        for (int y = 0; y < c.height; y += 8)
        {
            const DWORD* srow = (const DWORD*)((const BYTE*)bits +
                (size_t)(y + c.cropY) * capW * 4 + (size_t)c.cropX * 4);
            for (int x = 0; x < c.width; x += 8)
            {
                samples++;
                const DWORD px = srow[x];
                if ((px & 0xFF) < 0x0C && ((px >> 8) & 0xFF) < 0x0C && ((px >> 16) & 0xFF) < 0x0C)
                    nearBlack++;
            }
        }
        const bool allBlack = samples > 0 && nearBlack * 100 >= samples * 99;
        if (allBlack)
        {
            if (++c.blackRun == 3 && !c.blackLogged)
            {
                c.blackLogged = true;
                LogWarning("WCBLACK 0x%x: PrintWindow succeeds but returns >=99%% near-black %dx%d "
                    "content (3 consecutive) - dom0 is showing this window black",
                    c.hwnd, c.width, c.height);
            }
        }
        else
        {
            if (c.blackLogged)
                LogInfo("WCBLACK 0x%x: content non-black again after %d black captures",
                    c.hwnd, c.blackRun);
            c.blackRun = 0;
            c.blackLogged = false;
        }
    }
    SelectObject(memdc, old);
    DeleteObject(bmp);
    DeleteDC(memdc);

    if (ok && y0 >= 0 && out)
    {
        out->hwnd = c.hwnd;
        out->w = c.width;
        out->y0 = y0;
        out->y1 = y1;
    }
    return ok ? true : false;
}

DWORD WINAPI CaptureThread(LPVOID param)
{
    Engine& e = *(Engine*)param;
    AttachThreadToInputDesktop();
    std::vector<DamageOut> fired;
    while (!e.quit.load())
    {
        bool didWork = false;
        fired.clear();
        AcquireSRWLockShared(&e.lock);
        const size_t n = e.channels.size();

        // round-robin sweep slot: pick one live channel per interval and mark it
        // dirty, so guest-occluded windows (invisible to DDA) still converge
        DWORD now = GetTickCount();
        if (n > 0 && (now - e.lastSweep) >= SWEEP_INTERVAL_MS)
        {
            e.lastSweep = now;
            for (size_t k = 0; k < n; k++)
            {
                Channel& sc = *e.channels[(e.sweepNext + k) % n];
                if (!sc.dead && !sc.ddaOwned.load())
                {
                    sc.dirty.store(true);
                    e.sweepNext = (e.sweepNext + k + 1) % n;
                    break;
                }
            }
        }

        for (auto& cp : e.channels)
        {
            Channel& c = *cp;
            // ddaOwned checked BEFORE consuming dirty: a mark arriving while the frame
            // loop owns the buffer stays pending and is served when ownership drops,
            // instead of being eaten here or triggering a write into an owned buffer.
            if (c.dead || c.ddaOwned.load() || !c.dirty.exchange(false))
                continue;
            didWork = true;
            DamageOut dmg{ nullptr, 0, 0, 0 };
            if (CaptureAndDiff(e, c, &dmg))
            {
                c.failures = 0;
                if (dmg.hwnd)
                    fired.push_back(dmg);
            }
            else if (++c.failures >= DEAD_AFTER_FAILURES)
            {
                c.dead = true;
                // Field diagnosis 2026-08-27: this latch was silent, so a window whose
                // captures all failed just stayed black in dom0 with a clean-looking log.
                LogWarning("WCDEAD 0x%x: %d consecutive capture failures (last error 0x%x) - "
                    "channel dead, dom0 keeps this window's last content (black if none)",
                    c.hwnd, c.failures, c.lastErr);
            }
            else
            {
                // window may live on another desktop now (UAC/lock); re-attach and retry later
                AttachThreadToInputDesktop();
                c.dirty.store(true);
            }
        }
        ReleaseSRWLockShared(&e.lock);

        // Fire callbacks with NO engine lock held: the callback path takes agent locks
        // (g_csWatchedWindows) that the frame thread holds while calling WcMarkDirty,
        // so calling back under e.lock would invert the order and deadlock.
        if (e.callback)
            for (auto& d : fired)
                e.callback(d.hwnd, 0, d.y0, d.w, d.y1 - d.y0 + 1);

        Sleep(didWork ? 2 : 8);
    }
    return 0;
}

} // namespace

extern "C" {

ULONG WcProbeSupport(void)
{
    return 0; // PrintWindow(PW_RENDERFULLCONTENT) exists on everything QWT supports
}

BOOL WcIsSupported(void)
{
    return TRUE;
}

ULONG WcInit(WC_DAMAGE_CALLBACK callback)
{
    if (g_eng)
        return ERROR_ALREADY_INITIALIZED;
    auto e = std::make_unique<Engine>();
    e->callback = callback;
    e->thread = CreateThread(nullptr, 0, CaptureThread, e.get(), 0, nullptr);
    if (!e->thread)
        return GetLastError();
    g_eng = e.release();
    return ERROR_SUCCESS;
}

void WcShutdown(void)
{
    if (!g_eng)
        return;
    g_eng->quit.store(true);
    WaitForSingleObject(g_eng->thread, 5000);
    CloseHandle(g_eng->thread);
    AcquireSRWLockExclusive(&g_eng->lock);
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
    auto c = std::make_unique<Channel>();
    c->hwnd = hwnd;
    c->width = width;
    c->height = height;
    c->cropX = cropX;
    c->cropY = cropY;
    c->buffer = (BYTE*)buffer;
    AcquireSRWLockExclusive(&g_eng->lock);
    g_eng->channels.push_back(std::move(c));
    ReleaseSRWLockExclusive(&g_eng->lock);
    return ERROR_SUCCESS;
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
            g_eng->channels.erase(g_eng->channels.begin() + i);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_eng->lock);
}

// Update the visible-rect offset inside the OS window rect. It is computed at WcAddWindow from
// (announce rect - GetWindowRect), i.e. the DWM invisible border, and was then FROZEN for the
// channel's whole life - but that relationship is not constant: it changes when a window's frame
// state changes, and nothing re-attaches on a plain move. A stale crop of 0 makes the buffer hold
// the window INCLUDING its invisible border, which dom0 then draws at the visible-rect position -
// the 7 px black band down the left edge measured after a drag on 2026-08-16.
void WcSetCrop(HWND hwnd, int cropX, int cropY)
{
    if (!g_eng)
        return;
    if (cropX < 0) cropX = 0;
    if (cropY < 0) cropY = 0;
    AcquireSRWLockExclusive(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            if (ch->cropX != cropX || ch->cropY != cropY)
            {
                ch->cropX = cropX;
                ch->cropY = cropY;
                ch->dirty.store(true); // the whole buffer is now wrong; re-capture it
            }
            break;
        }
    ReleaseSRWLockExclusive(&g_eng->lock);
}

void WcSetMask(HWND hwnd, const RECT* rects, int count)
{
    if (!g_eng)
        return;
    if (count > WC_MAX_MASK)
        count = WC_MAX_MASK;
    AcquireSRWLockExclusive(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            for (int i = 0; i < count; i++)
                ch->mask[i] = rects[i];
            ch->maskCount = count;
            ch->dirty.store(true); // re-capture so unmasked areas refresh promptly
            break;
        }
    ReleaseSRWLockExclusive(&g_eng->lock);
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

void WcMarkDirty(HWND hwnd)
{
    if (!g_eng)
        return;
    AcquireSRWLockShared(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            ch->dirty.store(true);
            break;
        }
    ReleaseSRWLockShared(&g_eng->lock);
}

void WcSetDdaOwned(HWND hwnd, BOOL owned)
{
    if (!g_eng)
        return;
    AcquireSRWLockShared(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            ch->ddaOwned.store(owned ? true : false);
            break;
        }
    ReleaseSRWLockShared(&g_eng->lock);
}

ULONG WcPrefill(HWND hwnd)
{
    if (!g_eng)
        return ERROR_NOT_READY;
    Channel* c = nullptr;
    AcquireSRWLockShared(&g_eng->lock);
    for (auto& ch : g_eng->channels)
        if (ch->hwnd == hwnd)
        {
            c = ch.get();
            break;
        }
    // capture synchronously on the calling thread (same proven GDI path); pass no
    // DamageOut - the caller sends a full-window damage after the dump, and firing the
    // callback here (under the shared lock, on the frame thread) risks the same
    // lock-order inversion the async path avoids.
    ULONG status = ERROR_NOT_FOUND;
    if (c)
        status = CaptureAndDiff(*g_eng, *c, NULL) ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
    ReleaseSRWLockShared(&g_eng->lock);
    return status;
}

} // extern "C"
