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
    // best effort: drain pending revokes (daemon may be gone; bounded retries)
    for (int i = 0; i < 3 && g_PwPending; i++)
    {
        PwRevokeTick();
        if (g_PwPending)
            Sleep(100);
    }
    if (g_PwPending)
        LogWarning("per-window: leaking un-revoked grants at shutdown");
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

    PVOID buffer = VirtualAlloc(NULL, (size_t)pageCount * PAGE_SIZE,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
        return ERROR_NOT_ENOUGH_MEMORY;

    ULONG* refs = (ULONG*)malloc(pageCount * sizeof(ULONG));
    if (!refs)
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    PVOID shared = NULL;
    status = XcGnttabPermitForeignAccess2(g_PwXc, g_GuiDomainId, buffer, pageCount,
                                          0, 0, XENIFACE_GNTTAB_READONLY,
                                          &shared, refs);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "XcGnttabPermitForeignAccess2(window)");
        free(refs);
        VirtualFree(buffer, 0, MEM_RELEASE);
        return status;
    }

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
            PwQueueRevoke(shared, refs, buffer);
            PwRevokeTick(); // nothing maps it yet; usually succeeds immediately
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
    LogInfo("0x%x: per-window buffer %ux%u (%lu pages) attached%s",
             entry->Handle, entry->Width, entry->Height, pageCount,
             sliceFed ? " (slice-fed)" : "");
    return ERROR_SUCCESS;
}

void PwDetachWindow(IN OUT WINDOW_DATA* entry)
{
    if (!entry->PwDumpSent)
        return;
    LogInfo("0x%x: per-window buffer %ux%u detached%s", entry->Handle,
            entry->PwWidth, entry->PwHeight, entry->PwSliceFed ? " (slice-fed)" : "");
    if (!entry->PwSliceFed)
        WcRemoveWindow(entry->Handle);
    PwQueueRevoke(entry->PwGrantHandle, entry->PwGrantRefs, entry->PwBuffer);
    entry->PwBuffer = NULL;
    entry->PwPageCount = 0;
    entry->PwGrantRefs = NULL;
    entry->PwGrantHandle = NULL;
    entry->PwWidth = 0;
    entry->PwHeight = 0;
    entry->PwDumpSent = FALSE;
    entry->PwSliceFed = FALSE;
    entry->PwSliceNeedsFull = FALSE;
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
