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

// Diagnostic gate, defined in perf.c. Declared here rather than including perf.h, which is a C
// header carrying agent-side structs this translation unit has no business seeing.
extern "C" BOOL g_ProtoTrace;

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

const DWORD SWEEP_INTERVAL_MS = 250;
// A channel that keeps answering "nothing changed" is swept at double the interval each time, up
// to this cap. It still CONVERGES - the sweep exists because an occluded window is invisible to
// the desktop-duplication producer - it just stops costing an expensive application its UI thread
// several times a second to say nothing. Any real mark (WcMarkDirty) resets it to the base
// interval immediately, so a window that actually changes is never penalised.
const DWORD SWEEP_BACKOFF_MAX_MS = 2000;  // one round-robin slot per this interval
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
    // A channel that has never been captured jumps the queue - see the two-pass service loop.
    std::atomic<bool> firstPending{ true };
    // Claimed by exactly one capture worker at a time. CaptureAndDiff writes this channel's
    // buffer and its failure counters, so two workers must never be inside the same channel;
    // different channels are independent (every GDI object in CaptureAndDiff is local to the
    // call and it does not touch the Engine at all).
    std::atomic<bool> busy{ false };
    // SWEEP BACKOFF. The round-robin sweep marks a channel speculatively - nothing has said its
    // content changed - and a capture costs a full PrintWindow on the target window's own UI
    // thread. Measured 2026-09-24 with a slow window present: 30 captures, median 334 ms each, of
    // which 28 produced NO DAMAGE - about 9.4 seconds of another application's UI thread spent
    // discovering nothing had changed, with the engine ~97% occupied by it. So a channel whose
    // SPECULATIVE captures keep coming back unchanged is swept progressively less often.
    // realMark separates the two producers: a mark from WcMarkDirty means something ACTUALLY saw
    // this window's screen region change, and must never be backed off.
    std::atomic<bool>  realMark{ false };
    std::atomic<DWORD> sweepDelay{ 0 };   // current speculative interval, 0 = base
    std::atomic<DWORD> sweepDue{ 0 };     // tick from which this channel may be swept again
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

// CAPTURE WORKERS. Every capture is a PrintWindow that round-trips synchronously into a
// DIFFERENT application's UI thread, measured at 32-438 ms each on this guest, so with a single
// thread one slow application delayed the refresh of every other window and a new window's first
// frame queued behind all of them. The owner saw it as "the first window was snappy. subsequent
// ones were not." Jev rated the serial design a structural latency defect at 0.92 and parallel
// capture as the fix at 0.86. The work is waiting on other processes, not computing, so this is
// NOT sized to core count.
//
// TWO, not four. Four was tried first and MEASURED WORSE overall: on a burst of 4 Explorer windows
// opened 700 ms apart, four workers made windows APPEAR sooner (window 4 median 688 -> 423 ms,
// 3/3 pairs) but FINISH later (sum of per-window time-to-stable medians 2294 -> 3438 ms). The
// mechanism, rated credible at 0.84: PrintWindow executes ON THE TARGET WINDOW'S UI THREAD, so
// capturing several windows of the SAME process concurrently serialises inside that process
// anyway AND competes with the application's own painting - and a burst of Explorer windows is
// exactly that case. Two workers keep one slow application from blocking every other window
// (the structural defect, rated 0.92) without piling four synchronous renders onto one shell.
// ONE. Parallel capture was built, measured on a validated instrument, and did not earn its
// keep. With a deliberately slow-painting (not hung) window present, two workers improved a new
// window's TIME TO APPEAR (median 613 -> 493 ms) but regressed its TIME TO STABLE (median 829 ->
// 1372 ms) - and time-to-stable is the metric Jev rated as matching what the owner actually
// perceives (0.84), with time-to-first-pixels rated as flattering (0.87). Jev's verdict on the
// result leaned revert (0.46 revert / 0.29 insufficient / 0.14 keep).
// The structural criticism remains TRUE and is recorded rather than fixed: this is one thread, and
// every capture blocks it for another application's paint time, so one slow application still
// delays every other window's refresh. What is NOT true is that four - or two - concurrent
// PrintWindows are an improvement: PrintWindow executes on the TARGET window's UI thread, so
// concurrency there competes with the very painting it is waiting for. A real fix has to stop
// blocking on other processes, not do more of it at once.
// The pool machinery is kept (per-channel busy claim, per-worker wake) so the count is one
// constant away, but it ships at 1 until something measures better.
#define WC_WORKERS 4   // compile-time BOUND; the active count is chosen once at WcInit

struct Engine
{
    HANDLE threads[WC_WORKERS] = {};
    int workers = 1;                 // ACTIVE worker count, decided once at WcInit
    std::atomic<bool> quit{ false };
    SRWLOCK lock = SRWLOCK_INIT;                 // guards channels vector
    std::vector<std::unique_ptr<Channel>> channels;
    WC_DAMAGE_CALLBACK callback = nullptr;
    size_t sweepNext = 0;
    DWORD lastSweep = 0;
    // Auto-reset wake for CaptureThread. Producers (WcMarkDirty, WcSetDdaOwned(FALSE),
    // WcSetCrop/WcSetMask, WcAddWindow, WcShutdown) set it after storing their flag;
    // the loop waits on it instead of Sleep(8). Before this the thread woke ~125x/s for
    // the agent's whole life on an idle desktop, taking the lock and walking every
    // channel to find nothing, and a dirty mark could sit up to 8 ms before service.
    // One auto-reset wake per worker: an auto-reset event releases exactly ONE waiter, so a
    // single shared event would wake one worker and leave the others asleep with work pending.
    HANDLE wake[WC_WORKERS] = {};
};

// Per-worker start context (the thread proc needs to know which slot it is).
struct WorkerCtx { Engine* eng; int id; };
static WorkerCtx g_wctx[WC_WORKERS];

// Set-after-store: an auto-reset event stays signalled until a wait consumes it, so a
// mark landing between the consumer's channel walk and its wait is never lost.
void WakeCapture(Engine& e)
{
    for (int i = 0; i < e.workers; i++)
        if (e.wake[i])
            SetEvent(e.wake[i]);
}

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
    WorkerCtx* wc = (WorkerCtx*)param;
    Engine& e = *wc->eng;
    const int id = wc->id;
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
        // Only worker 0 runs the round-robin bookkeeping: lastSweep/sweepNext are shared
        // cursors, not per-channel state, and marking is cheap. Every worker then SERVICES
        // whatever is dirty, so the sweep's single owner is not a bottleneck.
        DWORD now = GetTickCount();
        if (id == 0 && n > 0 && (now - e.lastSweep) >= SWEEP_INTERVAL_MS)
        {
            e.lastSweep = now;
            for (size_t k = 0; k < n; k++)
            {
                Channel& sc = *e.channels[(e.sweepNext + k) % n];
                if (sc.dead || sc.ddaOwned.load())
                    continue;
                // Backed off: not due yet. Signed compare so tick wraparound is handled.
                if ((LONG)(now - sc.sweepDue.load()) < 0)
                    continue;
                sc.dirty.store(true);
                const DWORD d = sc.sweepDelay.load();
                sc.sweepDue.store(now + (d ? d : SWEEP_INTERVAL_MS));
                e.sweepNext = (e.sweepNext + k + 1) % n;
                break;
            }
        }

        // TWO PASSES, and the order is the point. Every capture below is a PrintWindow that
        // round-trips synchronously into a DIFFERENT application's UI thread, and this is the
        // only thread that services channels - so whatever is serviced first delays everything
        // after it by that application's paint time (measured on this guest: 32-438 ms EACH).
        // A window that has never been captured is the one case where the delay is visible as
        // "the new window has not appeared yet", because dom0 has nothing for it at all; an
        // already-captured window merely refreshes slightly later. So first captures go FIRST.
        //
        // This is what the owner observed and called counterintuitive: "the first window was
        // snappy. subsequent ones were not" - with nothing else open a new window's first capture
        // ran immediately, and with other windows open it queued behind their refreshes. Jev put
        // the cause at 0.95, called the serial design a structural latency defect at 0.92, and
        // named this fix at 0.94.
        for (int pass = 0; pass < 2; pass++)
        for (auto& cp : e.channels)
        {
            Channel& c = *cp;
            const bool isFirst = c.firstPending.load();
            if ((pass == 0) != isFirst)
                continue;                      // pass 0: first captures only; pass 1: the rest
            // ddaOwned checked BEFORE consuming dirty: a mark arriving while the frame
            // loop owns the buffer stays pending and is served when ownership drops,
            // instead of being eaten here or triggering a write into an owned buffer.
            if (c.dead || c.ddaOwned.load())
                continue;
            // Claim BEFORE consuming dirty, so a channel another worker is already capturing
            // is skipped without swallowing its mark.
            if (c.busy.exchange(true))
                continue;
            if (!c.dirty.exchange(false))
            {
                c.busy.store(false);
                continue;
            }
            if (isFirst)
                c.firstPending.store(false);   // cleared once served, success or not
            didWork = true;
            DamageOut dmg{ nullptr, 0, 0, 0 };
            // PER-CAPTURE COST (ProtoTrace only). Time-to-stable regressed under 2 workers in a
            // harness where the two windows were DIFFERENT PROCESSES, which refutes
            // same-application contention as the cause (Jev 0.86) and leaves the cause
            // NOT ESTABLISHED (0.89). A capture is a PrintWindow on the target window's own UI
            // thread, so the question is whether captures of a window get SLOWER, or merely more
            // frequent, when another worker is running - and that needs the cost of each one.
            LARGE_INTEGER capT0, capT1, capFreq;
            const bool traceCap = g_ProtoTrace != FALSE;
            if (traceCap) QueryPerformanceCounter(&capT0);
            const bool capOk = CaptureAndDiff(e, c, &dmg);
            if (traceCap)
            {
                QueryPerformanceCounter(&capT1);
                QueryPerformanceFrequency(&capFreq);
                const LONGLONG us = capFreq.QuadPart
                    ? ((capT1.QuadPart - capT0.QuadPart) * 1000000) / capFreq.QuadPart : -1;
                LogInfo("QGAPROTO,msg=WCCAP,hwnd=0x%x,worker=%d,first=%d,us=%lld,changed=%d",
                        (DWORD)(ULONG_PTR)c.hwnd, id, isFirst ? 1 : 0, us,
                        (capOk && dmg.hwnd) ? 1 : 0);
            }
            // Was this capture asked for by a producer that SAW a change, or merely swept?
            const bool wasReal = c.realMark.exchange(false);
            if (capOk)
            {
                c.failures = 0;
                if (dmg.hwnd)
                {
                    fired.push_back(dmg);
                    // Produced real damage: this channel is worth watching closely again.
                    c.sweepDelay.store(0);
                    c.sweepDue.store(GetTickCount());
                }
                else if (!wasReal)
                {
                    // A SPECULATIVE capture that produced nothing. Double the interval, capped.
                    const DWORD cur = c.sweepDelay.load();
                    DWORD next = cur ? cur * 2 : SWEEP_INTERVAL_MS * 2;
                    if (next > SWEEP_BACKOFF_MAX_MS) next = SWEEP_BACKOFF_MAX_MS;
                    c.sweepDelay.store(next);
                    c.sweepDue.store(GetTickCount() + next);
                }
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
            c.busy.store(false);
        }
        ReleaseSRWLockShared(&e.lock);

        // Fire callbacks with NO engine lock held: the callback path takes agent locks
        // (g_csWatchedWindows) that the frame thread holds while calling WcMarkDirty,
        // so calling back under e.lock would invert the order and deadlock.
        if (e.callback)
            for (auto& d : fired)
                e.callback(d.hwnd, 0, d.y0, d.w, d.y1 - d.y0 + 1);

        // Event-driven idle instead of Sleep(8) polling (audit 2026-09-08: 125 wakeups/s
        // per agent on an idle desktop, each taking e.lock and walking every channel).
        // After work: 2 ms pacing as before. Idle: sleep until the next round-robin sweep
        // slot is due (the sweep is the only time-driven consumer), or until a producer
        // signals e.wake - a dirty mark is then served immediately instead of <= 8 ms
        // late. Bounded by SWEEP_INTERVAL_MS, never INFINITE: quit is a polled flag.
        DWORD waitMs = 2;
        if (!didWork)
        {
            waitMs = SWEEP_INTERVAL_MS;
            if (n > 0)
            {
                const DWORD sinceSweep = GetTickCount() - e.lastSweep;
                waitMs = sinceSweep >= SWEEP_INTERVAL_MS ? 1 : SWEEP_INTERVAL_MS - sinceSweep;
            }
        }
        if (e.wake[id])
            WaitForSingleObject(e.wake[id], waitMs);
        else
            Sleep(waitMs);
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

ULONG WcInit(WC_DAMAGE_CALLBACK callback, ULONG workers)
{
    if (g_eng)
        return ERROR_ALREADY_INITIALIZED;
    auto e = std::make_unique<Engine>();
    e->callback = callback;
    // Decided ONCE, here, and never revisited - a capability, not a runtime knob.
    e->workers = (int)(workers < 1 ? 1 : (workers > WC_WORKERS ? WC_WORKERS : workers));
    for (int i = 0; i < e->workers; i++)
    {
        e->wake[i] = CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
        if (!e->wake[i])
        {
            ULONG err = GetLastError();
                for (int j = 0; j < i; j++) CloseHandle(e->wake[j]);
            return err;
        }
    }
    // The pool must be fully constructed before any worker runs: a worker dereferences
    // g_wctx[i].eng immediately, and worker 0 owns the sweep cursors.
    int started = 0;
    for (int i = 0; i < e->workers; i++)
    {
        g_wctx[i].eng = e.get();
        g_wctx[i].id = i;
        e->threads[i] = CreateThread(nullptr, 0, CaptureThread, &g_wctx[i], 0, nullptr);
        if (!e->threads[i])
            break;
        started++;
    }
    if (started == 0)
    {
        ULONG err = GetLastError();
        for (int i = 0; i < e->workers; i++) CloseHandle(e->wake[i]);
        return err;
    }
    // A partially started pool is FINE and is not silently accepted: it still captures, just
    // with less parallelism, and worker 0 (the sweep owner) is started first.
    if (started < e->workers)
        LogWarning("WCPOOL only %d of %d capture workers started (0x%x) - capture continues "
            "with reduced parallelism", started, e->workers, GetLastError());
    LogInfo("WCPOOL %d capture worker(s)", e->workers);
    g_eng = e.release();
    return ERROR_SUCCESS;
}

void WcShutdown(void)
{
    if (!g_eng)
        return;
    g_eng->quit.store(true);
    WakeCapture(*g_eng); // the idle wait is now up to SWEEP_INTERVAL_MS; cut it short
    for (int i = 0; i < g_eng->workers; i++)
    {
        if (!g_eng->threads[i])
            continue;
        // Each worker can be parked inside a PrintWindow on a hung application, so give the
        // same 5 s per worker the single thread used to get - they wait concurrently.
        WaitForSingleObject(g_eng->threads[i], 5000);
        CloseHandle(g_eng->threads[i]);
    }
    AcquireSRWLockExclusive(&g_eng->lock);
    g_eng->channels.clear();
    ReleaseSRWLockExclusive(&g_eng->lock);
    for (int i = 0; i < g_eng->workers; i++)
        if (g_eng->wake[i])
            CloseHandle(g_eng->wake[i]);
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
    WakeCapture(*g_eng); // new channel starts dirty (initial fill)
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
                WakeCapture(*g_eng);
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
            WakeCapture(*g_eng);
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
            ch->realMark.store(true);
            ch->sweepDelay.store(0);     // something SAW a change: watch this channel closely again
            ch->sweepDue.store(GetTickCount());
            ch->dirty.store(true);
            WakeCapture(*g_eng);
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
            // Ownership dropping is what releases a dirty mark that arrived while the
            // frame loop owned the buffer (kept pending, see CaptureThread); wake so it
            // is served now rather than at the next sweep slot.
            if (!owned)
                WakeCapture(*g_eng);
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
