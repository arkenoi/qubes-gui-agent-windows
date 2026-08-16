/*
 * perwindow - per-window framebuffer management. See perwindow.h.
 */

#include <windows.h>
#include <stdint.h>
#include <strsafe.h>

#include "common.h"
#include "main.h"
#include "send.h"
#include "wincapture.h"
#include "perwindow.h"

#include <log.h>
#include <config.h>
#include <xencontrol.h>
#pragma warning(disable:4200) // nonstandard extension used: zero-sized array (flexible array member)
#include <qubes-gui-protocol.h>

#define REG_CONFIG_PERWINDOW_VALUE L"PerWindowCapture"
#define PERWINDOW_ENV_VALUE L"QGA_PERWINDOW"

extern BOOL g_VchanClientConnected; // main.c

static BOOL g_PwOn = FALSE;                   // config + WGC support + init success
static uint32_t g_PwDaemonVersion = 0;        // learned from MSG_VERSION
static PXENCONTROL_CONTEXT g_PwXc = NULL;

// Grants whose windows are gone/resized, awaiting successful revocation. Revoke fails
// with a busy status while the gui domain still maps the segment; the daemon releases
// it when it processes the superseding MSG_WINDOW_DUMP, MSG_UNMAP or MSG_DESTROY, so
// retrying on ticks/ACKs converges.
typedef struct _PW_PENDING_REVOKE
{
    PVOID Shared;
    ULONG* Refs;
    PVOID Buffer;
    struct _PW_PENDING_REVOKE* Next;
} PW_PENDING_REVOKE;

static PW_PENDING_REVOKE* g_PwPending = NULL;
static CRITICAL_SECTION g_PwPendingLock;

static void PwXcLogger(IN XENCONTROL_LOG_LEVEL logLevel, IN const char* function,
                       IN const wchar_t* format, IN va_list args)
{
    wchar_t buf[1024];
    if (FAILED(StringCbVPrintfW(buf, sizeof(buf), format, args)))
        return;
    _LogFormat(logLevel, /*raw=*/FALSE, function, buf);
}

// WGC damage callback: runs on the capture thread. SendWindowDamageEvent takes the
// vchan lock internally; coordinates are window-relative and clamped daemon-side
// against the window's own image, so no window-list access is needed here.
static void PwOnDamage(HWND window, int x, int y, int w, int h)
{
    if (!g_VchanClientConnected)
        return;
    ULONG status = SendWindowDamageEvent(window, x, y, w, h);
    if (status != ERROR_SUCCESS)
        LogVerbose("SendWindowDamageEvent(0x%x) failed: 0x%x", window, status);
}

void PwInit(void)
{
    InitializeCriticalSection(&g_PwPendingLock);

    DWORD enabled = 1; // default ON: this build exists to exercise the new path
    CfgReadDword(NULL, REG_CONFIG_PERWINDOW_VALUE, &enabled, NULL);

    WCHAR env[16];
    if (GetEnvironmentVariableW(PERWINDOW_ENV_VALUE, env, RTL_NUMBER_OF(env)) > 0)
        enabled = (env[0] != L'0');

    if (!enabled)
    {
        LogInfo("per-window capture disabled by config");
        return;
    }

    {
        // ADVISORY ONLY: under the agent's SYSTEM-in-session-1 token, IsSupported()
        // activation fails with 0x8007000E even where real capture may work. The
        // authoritative test is the first actual CreateForWindow in WcAddWindow, which
        // falls back per-window on failure.
        ULONG probe = WcProbeSupport();
        if (probe != 0)
            LogWarning("WGC support probe failed 0x%x - proceeding, real attach decides", probe);
    }

    DWORD status = XcOpen(PwXcLogger, &g_PwXc);
    if (status != ERROR_SUCCESS || !g_PwXc)
    {
        win_perror2(status, "per-window capture disabled: XcOpen");
        g_PwXc = NULL;
        return;
    }
    XcSetLogLevel(g_PwXc, LogGetLevel());

    status = WcInit(PwOnDamage);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "per-window capture disabled: WcInit");
        XcClose(g_PwXc);
        g_PwXc = NULL;
        return;
    }

    g_PwOn = TRUE;
    LogInfo("per-window capture ENABLED (daemon version gate applies at attach)");
}

void PwShutdown(void)
{
    if (!g_PwOn)
        return;
    g_PwOn = FALSE;
    WcShutdown();
    // EXACTLY ONE revoke attempt, never a retry loop: a revoke racing dom0's unmap
    // can spin unboundedly inside xenbus (NMI-dump-proven, FINDINGS 2026-08-05
    // cont 9; recurred on the OS-shutdown path, cont 11). Each retry is another
    // roll of that race. Whatever stays busy is leaked LOUDLY - the domain
    // teardown reclaims the entries; buffers are deliberately not freed (dom0 may
    // still map them).
    PwRevokeTick();
    if (g_PwPending)
        LogWarning("A6LEAK per-window: abandoning un-revoked grants at shutdown (dom0 still maps them)");
    if (g_PwXc)
    {
        XcClose(g_PwXc);
        g_PwXc = NULL;
    }
}

BOOL PwEnabled(void)
{
    return g_PwOn;
}

void PwSetDaemonVersion(uint32_t version)
{
    g_PwDaemonVersion = version;
    if (g_PwOn && version < QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK)
        LogWarning("per-window capture inactive: daemon protocol 0x%x < 0x%x",
                   version, QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK);
}

BOOL PwIsAttached(IN const WINDOW_DATA* entry)
{
    return entry->PwDumpSent;
}

// GRANT SLAB POOL. The Windows grant driver as shipped never returns a reference: xeniface's only
// RevokeForeignAccess call sits inside an ASSERT, which a release build compiles out (verified by
// disassembling the shipped xeniface.sys). So every grant this agent creates is permanent for the
// life of the domain, and a per-window buffer granted per window APPEARANCE burns ~2025 refs at
// 1080p and never gets them back. Measured on 2026-08-16: the guest's pool is ~1,048,576 refs and
// dies at 144 agent restarts (7200 pages each); at ~2025 per window it dies after ~517 windows -
// an afternoon of opening and closing applications. The end state is a qube that runs, answers
// qrexec, and can never show a window again until it is restarted.
//
// We cannot fix the driver (it is XenProject's, and we stage it bit-identical from a signed MSI),
// so the agent must stop making grants a function of ACTIVITY. Slabs are granted once, kept for
// ever, and reused: references then scale with PEAK CONCURRENT WINDOWS instead of with window
// history, which is a constant a guest cannot walk off the end of.
//
// Sizing: capacity is rounded up to PW_SLAB_GRANULARITY pages so that similar windows share a
// class and a resize inside the class needs no new grant at all. Handing the daemon a LARGER ref
// count than the geometry needs is explicitly safe - the screen path already relies on it ("the
// daemon accepts a larger-than-needed count; only a too-small one is exit(1)").
//
// Quarantine: dom0 may still map a buffer for a moment after detach (that is why the revoke path
// was deferred in the first place). A slab therefore becomes reusable only after
// PW_SLAB_QUARANTINE_MS, so a fresh window cannot be handed pages the daemon is still compositing
// the previous one from. Same-guest only - no isolation boundary is involved - but it would be a
// visible flash of the wrong window.
#define PW_SLAB_GRANULARITY   256      // pages (1 MB) - bounds waste per window
#define PW_SLAB_QUARANTINE_MS 2000

typedef struct _PW_SLAB
{
    PVOID  Buffer;
    ULONG* Refs;
    PVOID  Shared;      // grant handle
    ULONG  Pages;       // granted capacity, >= any window that uses it
    BOOL   InUse;
    ULONGLONG FreeAt;   // tick after which a released slab may be reused
    struct _PW_SLAB* Next;
} PW_SLAB;

static PW_SLAB* g_PwSlabs = NULL;
static CRITICAL_SECTION g_PwSlabLock;
static BOOL g_PwSlabLockInit = FALSE;
static ULONG g_PwSlabsCreated = 0;   // grants we had to make (the number that must stay bounded)
static ULONG g_PwSlabsReused = 0;    // attaches served without a new grant

static void PwSlabLockInit(void)
{
    if (!g_PwSlabLockInit)
    {
        InitializeCriticalSection(&g_PwSlabLock);
        g_PwSlabLockInit = TRUE;
    }
}

// Take a slab that can hold pageCount pages, granting a new one only if nothing fits.
static PW_SLAB* PwSlabAcquire(IN ULONG pageCount)
{
    PwSlabLockInit();
    const ULONGLONG now = GetTickCount64();
    PW_SLAB* best = NULL;

    EnterCriticalSection(&g_PwSlabLock);
    for (PW_SLAB* s = g_PwSlabs; s; s = s->Next)
    {
        if (s->InUse || s->Pages < pageCount || now < s->FreeAt)
            continue;
        if (!best || s->Pages < best->Pages)   // smallest that fits: keep big slabs for big windows
            best = s;
    }
    if (best)
    {
        best->InUse = TRUE;
        g_PwSlabsReused++;
        LeaveCriticalSection(&g_PwSlabLock);

        // ZERO IT. A window's buffer is filled by damage, so any region the first frames do not
        // cover keeps whatever the pages already held. Freshly VirtualAlloc'd pages are zero, so
        // before pooling that showed as BLACK until the window painted - observed live on a drag,
        // where a moved window came back with its right-hand side black. A REUSED slab would show
        // the PREVIOUS WINDOW'S PIXELS there instead, which is a far worse failure: the user sees
        // one application's content inside another's frame. Same guest, so no isolation boundary
        // is crossed, but it must not happen. Zeroing restores exactly the pre-pool behaviour.
        memset(best->Buffer, 0, (size_t)best->Pages * PAGE_SIZE);
        LogDebug("PWSLAB reused %lu-page slab for %lu pages, zeroed (created=%lu reused=%lu)",
                 best->Pages, pageCount, g_PwSlabsCreated, g_PwSlabsReused);
        return best;
    }
    LeaveCriticalSection(&g_PwSlabLock);

    // Nothing fits: this is the only path that consumes grant references, and every one it
    // consumes is permanent. Round up so the slab can serve a whole size class later.
    ULONG capacity = ((pageCount + PW_SLAB_GRANULARITY - 1) / PW_SLAB_GRANULARITY) * PW_SLAB_GRANULARITY;
    if (capacity < pageCount)
        capacity = pageCount;   // overflow guard

    PW_SLAB* slab = (PW_SLAB*)calloc(1, sizeof(*slab));
    if (!slab)
        return NULL;

    slab->Buffer = VirtualAlloc(NULL, (size_t)capacity * PAGE_SIZE,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!slab->Buffer)
    {
        free(slab);
        return NULL;
    }
    slab->Refs = (ULONG*)malloc(capacity * sizeof(ULONG));
    if (!slab->Refs)
    {
        VirtualFree(slab->Buffer, 0, MEM_RELEASE);
        free(slab);
        return NULL;
    }

    ULONG status = XcGnttabPermitForeignAccess2(g_PwXc, g_GuiDomainId, slab->Buffer, capacity,
                                                0, 0, XENIFACE_GNTTAB_READONLY,
                                                &slab->Shared, slab->Refs);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "XcGnttabPermitForeignAccess2(slab)");
        free(slab->Refs);
        VirtualFree(slab->Buffer, 0, MEM_RELEASE);
        free(slab);
        return NULL;
    }

    slab->Pages = capacity;
    slab->InUse = TRUE;
    EnterCriticalSection(&g_PwSlabLock);
    slab->Next = g_PwSlabs;
    g_PwSlabs = slab;
    g_PwSlabsCreated++;
    LeaveCriticalSection(&g_PwSlabLock);

    LogInfo("PWSLAB granted a new %lu-page slab for %lu pages (created=%lu reused=%lu) - this is "
            L"the only path that spends grant references, and they are never returned",
            capacity, pageCount, g_PwSlabsCreated, g_PwSlabsReused);
    return slab;
}

// Return a slab to the pool. Deliberately does NOT revoke: the revoke is a no-op in the shipped
// driver, and keeping the grant is the entire point - the pages are ours to hand out again.
static void PwSlabRelease(IN PVOID buffer)
{
    if (!buffer)
        return;
    PwSlabLockInit();
    EnterCriticalSection(&g_PwSlabLock);
    for (PW_SLAB* s = g_PwSlabs; s; s = s->Next)
    {
        if (s->Buffer == buffer)
        {
            s->InUse = FALSE;
            s->FreeAt = GetTickCount64() + PW_SLAB_QUARANTINE_MS;
            break;
        }
    }
    LeaveCriticalSection(&g_PwSlabLock);
}

static void PwQueueRevoke(PVOID shared, ULONG* refs, PVOID buffer)
{
    PW_PENDING_REVOKE* p = (PW_PENDING_REVOKE*)malloc(sizeof(*p));
    if (!p)
    {
        LogError("out of memory queuing revoke; leaking grant %p", shared);
        return;
    }
    p->Shared = shared;
    p->Refs = refs;
    p->Buffer = buffer;
    EnterCriticalSection(&g_PwPendingLock);
    p->Next = g_PwPending;
    g_PwPending = p;
    LeaveCriticalSection(&g_PwPendingLock);
}

BOOL PwRevokePending(void)
{
    EnterCriticalSection(&g_PwPendingLock);
    BOOL pending = (g_PwPending != NULL);
    LeaveCriticalSection(&g_PwPendingLock);
    return pending;
}

void PwRevokeTick(void)
{
    if (!g_PwXc)
        return;
    EnterCriticalSection(&g_PwPendingLock);
    PW_PENDING_REVOKE** link = &g_PwPending;
    while (*link)
    {
        PW_PENDING_REVOKE* p = *link;
        DWORD status = XcGnttabRevokeForeignAccess(g_PwXc, p->Shared);
        if (status == ERROR_SUCCESS)
        {
            *link = p->Next;
            free(p->Refs);
            VirtualFree(p->Buffer, 0, MEM_RELEASE);
            free(p);
        }
        else
        {
            LogVerbose("revoke %p still busy: 0x%x", p->Shared, status);
            link = &p->Next;
        }
    }
    LeaveCriticalSection(&g_PwPendingLock);
}

// Whether PrintWindow can produce correct pixels for this window. UpdateLayeredWindow
// (ULW) surfaces have no GDI-paintable content: PrintWindow returns the premultiplied
// source bits, which renders translucent regions as garbage (a dimming backdrop comes out
// near-black). GetLayeredWindowAttributes FAILING on a layered window is the ULW
// discriminator; colorkeyed windows are equally uncapturable (the key color would show as
// opaque). Plain SetLayeredWindowAttributes alpha windows paint via WM_PAINT and capture
// fine (menus fading in were validated on the per-window path) - keep those attached.
BOOL PwWindowEligible(IN const WINDOW_DATA* entry)
{
    // Override-redirect windows (menus, tooltips, bubbles, splash overlays) are slice-fed
    // as a class. They are topmost by nature, so the composited screen region IS their
    // correct content - and PrintWindow is unreliable for them from the agent's
    // SYSTEM/session-1 context: Edge's "Restore pages" bubble captures fine from a
    // user-context probe but comes back blank in the agent (the WGC lesson again:
    // user-context probes do not predict SYSTEM-context behavior). A blank capture
    // row-diffs as "no change" against the blank prefill, so the failure mode is a
    // permanently black window with a healthy-looking channel.
    if (entry->IsOverrideRedirect)
        return FALSE;

    // No GDI redirection surface AT ALL (DirectComposition-only content): PrintWindow
    // has nothing to read regardless of layering. Edge's true first-run takeover window
    // is created with this bit from birth.
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
    if (entry->ExStyle & WS_EX_NOREDIRECTIONBITMAP)
        return FALSE;

    if (!(entry->ExStyle & WS_EX_LAYERED))
        return TRUE;

    COLORREF key;
    BYTE alpha;
    DWORD lwFlags;
    if (!GetLayeredWindowAttributes(entry->Handle, &key, &alpha, &lwFlags))
        return FALSE; // ULW-style layered window
    if (lwFlags & LWA_COLORKEY)
        return FALSE;
    return TRUE;
}

ULONG PwAttachWindow(IN OUT WINDOW_DATA* entry)
{
    if (!g_PwOn)
        return ERROR_NOT_SUPPORTED;
    if (entry->Synthesized)
        return ERROR_NOT_SUPPORTED; // composited into its owner; a dump would name an unknown hwnd
    if (g_PwDaemonVersion < QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK)
    {
        LogInfo("0x%x: attach skipped, daemon version 0x%x < 0x%x",
                entry->Handle, g_PwDaemonVersion, QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK);
        return ERROR_NOT_SUPPORTED;
    }
    if (entry->PwDumpSent)
        return ERROR_SUCCESS;
    if (entry->Width == 0 || entry->Height == 0)
        return ERROR_INVALID_PARAMETER;

    // Windows PrintWindow cannot capture still get their own buffer, fed from the
    // composited screen framebuffer by the frame loop (see PwSliceFed in main.h).
    const BOOL sliceFed = !PwWindowEligible(entry);

    ULONG status;
    const size_t imageBytes = (size_t)entry->Width * entry->Height * 4;
    const ULONG pageCount = (ULONG)((imageBytes + PAGE_SIZE - 1) / PAGE_SIZE);

    // Take a pooled slab. Only a pool MISS spends grant references, and on this driver they are
    // spent for ever - see the PW_SLAB comment. The slab may be larger than this window needs;
    // that is deliberate and safe, and it is what lets the next window (or the next size within
    // the class) attach without granting anything at all.
    PW_SLAB* slab = PwSlabAcquire(pageCount);
    if (!slab)
        return ERROR_NOT_ENOUGH_MEMORY;

    PVOID buffer = slab->Buffer;
    ULONG* refs = slab->Refs;
    PVOID shared = slab->Shared;
    const ULONG grantedPages = slab->Pages;   // what the daemon is told about

    if (!sliceFed)
    {
        // Crop: WGC captures the OS window rect; the daemon knows the DWM visible bounding
        // rect (entry->X/Y/W/H). The offset between them is what we skip in each frame.
        RECT wr;
        int cropX = 0, cropY = 0;
        if (GetWindowRect(entry->Handle, &wr))
        {
            cropX = entry->X - wr.left;
            cropY = entry->Y - wr.top;
            if (cropX < 0) cropX = 0;
            if (cropY < 0) cropY = 0;
        }

        status = WcAddWindow(entry->Handle, (int)entry->Width, (int)entry->Height,
                             cropX, cropY, buffer);
        if (status != ERROR_SUCCESS)
        {
            LogInfo("WcAddWindow(0x%x) failed 0x%x - staying on legacy path",
                     entry->Handle, status);
            PwSlabRelease(buffer);   // back to the pool; never revoked, never freed
            return status;
        }

        // Real pixels before the first WGC frame; failure just means a black window until
        // the first frame, so it is logged and ignored.
        if (WcPrefill(entry->Handle) != ERROR_SUCCESS)
            LogDebug("WcPrefill(0x%x) failed", entry->Handle);
    }

    status = SendWindowDump(entry->Handle, entry->Width, entry->Height,
                            pageCount, refs);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "SendWindowDump");
        if (!sliceFed)
            WcRemoveWindow(entry->Handle);
        PwQueueRevoke(shared, refs, buffer);
        PwRevokeTick();
        return status;
    }

    entry->PwBuffer = buffer;
    entry->PwPageCount = pageCount;
    entry->PwGrantRefs = refs;
    entry->PwGrantHandle = shared;
    entry->PwWidth = entry->Width;
    entry->PwHeight = entry->Height;
    entry->PwDumpSent = TRUE;
    entry->PwSliceFed = sliceFed;
    entry->PwSliceNeedsFull = sliceFed; // first frame does one full-window copy
    // Fresh channel: no mask has been pushed to it yet, and no move state carries
    // over from a previous buffer (a resize rebuild lands here mid-drag).
    entry->SynthMaskLastCount = 0;
    entry->PwFrameXYValid = FALSE;
    // Clear with the rest of the Pw state: a hash from a previous attachment
    // would suppress the FIRST capture after re-attach.
    entry->PwScreenHashValid = FALSE;
    entry->PwScreenHash = 0;
    entry->PwSettleDue = FALSE;
    entry->PwLastMoveTick = 0;
    entry->PwLastMoveCapTick = 0;
    // Fresh channel starts un-owned in the engine, so the drag-slice must re-engage
    // (re-claim ownership) before its next copy - a mid-drag resize lands here.
    entry->PwDragSlice = FALSE;
    LogInfo("0x%x: per-window buffer %ux%u (%lu pages of a %lu-page slab) attached%s",
             entry->Handle, entry->Width, entry->Height, pageCount, grantedPages,
             sliceFed ? L" (slice-fed)" : L"");
    return ERROR_SUCCESS;
}

void PwDetachWindow(IN OUT WINDOW_DATA* entry)
{
    if (!entry->PwDumpSent)
        return;
    LogInfo("0x%x: per-window buffer %ux%u detached%s", entry->Handle,
            entry->PwWidth, entry->PwHeight, entry->PwSliceFed ? L" (slice-fed)" : L"");
    if (!entry->PwSliceFed)
        WcRemoveWindow(entry->Handle);
    // The slab goes back to the pool, still granted. Revoking would achieve nothing on the shipped
    // driver and freeing the pages would strand the references for ever.
    PwSlabRelease(entry->PwBuffer);
    entry->PwBuffer = NULL;
    entry->PwPageCount = 0;
    entry->PwGrantRefs = NULL;
    entry->PwGrantHandle = NULL;
    entry->PwWidth = 0;
    entry->PwHeight = 0;
    entry->PwDumpSent = FALSE;
    entry->PwSliceFed = FALSE;
    entry->PwSliceNeedsFull = FALSE;
    // The channel (and the mask it held) is gone; move state dies with it.
    entry->SynthMaskLastCount = 0;
    entry->PwFrameXYValid = FALSE;
    // Clear with the rest of the Pw state: a hash from a previous attachment
    // would suppress the FIRST capture after re-attach.
    entry->PwScreenHashValid = FALSE;
    entry->PwScreenHash = 0;
    entry->PwSettleDue = FALSE;
    entry->PwLastMoveTick = 0;
    entry->PwLastMoveCapTick = 0;
    // The channel this flag described is gone; a re-attach gets a fresh channel whose
    // buffer holds no established content, so DDA mode must re-enter via prefill.
    // Left stale, the steady-state branch would slice-copy into an unestablished buffer.
    entry->PwDdaActive = FALSE;
    // Same reasoning for the drag-slice: its engine-buffer ownership died with the
    // channel, and a stale flag would make the moving branch skip re-engagement.
    entry->PwDragSlice = FALSE;
}

// Drop an attached window back to the legacy screen-slice path at runtime. The daemon
// keeps compositing from the detached (stale, pinned) buffer until something makes it
// release the image; force that with an unmap/map cycle (no re-dump: we are no longer
// attached), after which the queued revoke succeeds on a tick.
void PwForceLegacy(IN OUT WINDOW_DATA* entry)
{
    if (!entry->PwDumpSent)
        return;
    PwDetachWindow(entry);
    if (entry->IsVisible || entry->IsIconic)
    {
        if (SendWindowUnmap(entry->Handle) == ERROR_SUCCESS)
            (void)SendWindowMap(entry);
    }
}

ULONG PwResizeWindow(IN OUT WINDOW_DATA* entry)
{
    if (!entry->PwDumpSent)
        return ERROR_NOT_SUPPORTED;
    // Old grant to the pending list; the daemon releases its mapping when it processes
    // the new MSG_WINDOW_DUMP below, after which revocation succeeds on a tick/ACK.
    PwDetachWindow(entry);
    ULONG status = PwAttachWindow(entry);
    if (status != ERROR_SUCCESS)
    {
        // The daemon still composites this window from the DETACHED (stale, pinned)
        // buffer until something makes it release the image. Force that now with an
        // unmap/map cycle (no re-dump: we are no longer attached), dropping the window
        // to the legacy screen path; the queued revoke then succeeds on a tick.
        LogWarning("0x%x: re-attach failed (0x%x), forcing daemon release via unmap/map",
                   entry->Handle, status);
        if (entry->IsVisible || entry->IsIconic)
        {
            if (SendWindowUnmap(entry->Handle) == ERROR_SUCCESS)
                (void)SendWindowMap(entry);
        }
    }
    return status;
}

ULONG PwRemapWindow(IN const WINDOW_DATA* entry)
{
    if (!entry->PwDumpSent)
        return ERROR_NOT_SUPPORTED;
    // MUST be the geometry the buffer was granted for, never the live entry dims: a
    // dump claiming width*height*4 > granted pages makes gui-daemon exit(1) (daemon
    // xside.c img_data_size check), taking down the whole qube's GUI.
    return SendWindowDump(entry->Handle, entry->PwWidth, entry->PwHeight,
                          entry->PwPageCount, entry->PwGrantRefs);
}
