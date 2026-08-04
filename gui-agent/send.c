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

#include <windows.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "common.h"
#include "send.h"
#include "perf.h"
#include "main.h"
#include "vchan.h"
#include "capture.h" // FRAMEBUFFER_PAGE_COUNT, for the A3CHECK instrumentation

#include <qubes-gui-protocol.h>

#include <log.h>

#include <strsafe.h>

static_assert(sizeof(ULONG) == sizeof(uint32_t), "ULONG has a different size than uint32_t");

// Defined with the created-window set below; declared here because MSG_WINDOW_DUMP is
// emitted above it and is just as fatal to the daemon when the window has no CREATE.
static BOOL MaySendForWindowLocked(IN HWND window, IN const WCHAR *messageName);

ULONG SendWindowDump(IN HWND window, IN ULONG width, IN ULONG height,
    IN size_t numGrants, IN const ULONG* refs)
{
    ULONG status = ERROR_INVALID_PARAMETER;
    struct msg_hdr header;
    struct msg_window_dump_hdr dumpHdr;

    LogVerbose("start, window 0x%x %ux%u", window, width, height);

    if (refs == NULL)
    {
        LogError("grant refs are NULL");
        goto end;
    }

    if (numGrants == 0 || numGrants > MAX_GRANT_REFS_COUNT)
    {
        LogError("invalid grant count: %lu", numGrants);
        goto end;
    }

    header.type = MSG_WINDOW_DUMP;
    header.window = (uint32_t)(uintptr_t)window; // 0 == whole screen
    size_t untrusted_len = sizeof(dumpHdr) + numGrants * sizeof(ULONG);
    assert(untrusted_len < UINT32_MAX);
    header.untrusted_len = (uint32_t)untrusted_len;

    EnterCriticalSection(&g_VchanCriticalSection);
    if (!MaySendForWindowLocked(window, L"MSG_WINDOW_DUMP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        status = ERROR_SUCCESS; // dropped on purpose; not an error the caller can act on
        goto end;
    }
    if (!VCHAN_SEND(header, L"MSG_WINDOW_DUMP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        status = win_perror2(ERROR_UNIDENTIFIED_ERROR, "VCHAN_SEND(header)");
        goto end;
    }

    dumpHdr.type = WINDOW_DUMP_TYPE_GRANT_REFS;
    dumpHdr.bpp = 32;
    dumpHdr.width = width;
    dumpHdr.height = height;

    if (!VCHAN_SEND(dumpHdr, L"dumpHdr"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        status = win_perror2(ERROR_UNIDENTIFIED_ERROR, "VCHAN_SEND(dumpHdr)");
        goto end;
    }

    status = ERROR_SUCCESS;
    if (!VchanSendBuffer(g_Vchan, refs, numGrants * sizeof(ULONG), L"refs"))
    {
        status = win_perror2(ERROR_UNIDENTIFIED_ERROR, "VchanSendBuffer(grants)");
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

end:
    LogVerbose("end (%x)", status);

    return status;
}

ULONG SendScreenGrants(IN size_t numGrants, IN const ULONG* refs,
    IN UINT ctxWidth, IN UINT ctxHeight)
{
    // A3 instrumentation (log-only): the header geometry below and the page count computed
    // by the callers derive from g_ScreenWidth/Height (written by the resolution-change
    // thread), while the grant refs were sized from the capture context's DXGI desc on the
    // capture thread (capture.c, FRAMEBUFFER_PAGE_COUNT(ctx->width, ctx->height)). If they
    // diverge, a short count makes dom0's gui-daemon exit(1) and a long one reads past the
    // malloc'd grant_refs array. Log both derivations on every screen dump.
    ULONG dumpWidth = g_ScreenWidth;
    ULONG dumpHeight = g_ScreenHeight;
    size_t pagesG = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);
    size_t pagesCtx = FRAMEBUFFER_PAGE_COUNT(ctxWidth, ctxHeight);
    LogInfo("A3CHECK g=%lux%lu ctx=%ux%u pages_g=%lu pages_ctx=%lu",
        g_ScreenWidth, g_ScreenHeight, ctxWidth, ctxHeight, (ULONG)pagesG, (ULONG)pagesCtx);
    if (dumpWidth != ctxWidth || dumpHeight != ctxHeight || numGrants != pagesCtx)
        LogInfo("A3MISMATCH sent=%lux%lu pages_sent=%lu ctx=%ux%u pages_ctx=%lu",
            dumpWidth, dumpHeight, (ULONG)numGrants, ctxWidth, ctxHeight, (ULONG)pagesCtx);

    return SendWindowDump(NULL, dumpWidth, dumpHeight, numGrants, refs);
}

// --- recently-destroyed window ring -------------------------------------------------
// Guarded by g_VchanCriticalSection. SendWindowDestroy marks the hwnd inside the same
// lock hold that emits MSG_DESTROY; SendWindowDamageEvent checks it inside the hold that
// would emit MSG_SHMIMAGE. Damage-after-destroy (which makes gui-daemon exit(1) with
// "msg without CREATE", killing the whole qube's GUI) is thereby excluded by
// linearization on the vchan lock alone. The capture thread must NOT take
// g_csWatchedWindows for this: the main thread dispatches inbound messages while HOLDING
// the vchan lock (main.c WatchForEvents) and its handlers take the watched-windows lock
// inside it, so taking the two locks in the opposite order on the capture thread would
// be an ABBA deadlock.
// The ring only needs to cover in-flight capture callbacks: WcRemoveWindow stops new
// captures synchronously before MSG_DESTROY is sent, so at most one already-collected
// callback batch per window can still fire. Entries are cleared when an hwnd is reused
// for a new CREATE.
#define DESTROYED_RING_SIZE 64
static HWND g_DestroyedRing[DESTROYED_RING_SIZE];
static ULONG g_DestroyedRingNext;

static void MarkWindowDestroyedLocked(IN HWND window)
{
    g_DestroyedRing[g_DestroyedRingNext % DESTROYED_RING_SIZE] = window;
    g_DestroyedRingNext++;
}

static BOOL WasWindowDestroyedLocked(IN HWND window)
{
    for (int i = 0; i < DESTROYED_RING_SIZE; i++)
        if (g_DestroyedRing[i] == window)
            return TRUE;
    return FALSE;
}

static void ClearWindowDestroyedLocked(IN HWND window)
{
    for (int i = 0; i < DESTROYED_RING_SIZE; i++)
        if (g_DestroyedRing[i] == window)
            g_DestroyedRing[i] = NULL;
}

// --- created-window set: the protocol backstop ---------------------------------------
// gui-daemon calls exit(1) the moment it receives ANY per-window message for a window it
// never got a CREATE for ("msg 0x86 without CREATE for 0x20340" in guid.<vm>.log). That
// does not just break one window - it kills the GUI for the entire qube, and nothing
// restarts the daemon, so the agent then waits forever for a vchan client. An agent-side
// bookkeeping bug must never have that blast radius.
//
// WINDOW_DATA.CreateSent cannot serve as the guard: it lives under g_csWatchedWindows,
// which the capture thread must not take (ABBA against the vchan lock - see the
// destroyed-ring note above), and materialization (main.c, "owner geometry changed")
// clears Synthesized while leaving CreateSent FALSE on an entry that stays in the watched
// list, after which the ordinary damage/configure paths pick it up as a normal window.
// That is exactly the hole Word's MSO_BORDEREFFECT strips fell through.
//
// So the set is maintained here, by the very calls that emit CREATE and DESTROY, under
// g_VchanCriticalSection - the same lock that orders the messages themselves. Whatever
// the callers believe, what the daemon has actually been told is what gets checked.
static HWND *g_CreatedWindows;
static ULONG g_CreatedCount;
static ULONG g_CreatedCapacity;
static ULONG64 g_GateDrops;

static BOOL IsWindowCreatedLocked(IN HWND window)
{
    for (ULONG i = 0; i < g_CreatedCount; i++)
        if (g_CreatedWindows[i] == window)
            return TRUE;
    return FALSE;
}

static void MarkWindowCreatedLocked(IN HWND window)
{
    if (IsWindowCreatedLocked(window))
        return;

    if (g_CreatedCount == g_CreatedCapacity)
    {
        ULONG newCapacity = g_CreatedCapacity ? g_CreatedCapacity * 2 : 64;
        HWND *grown = realloc(g_CreatedWindows, newCapacity * sizeof(HWND));
        if (!grown)
        {
            // Degrade to a window that never updates, never to a message the daemon kills
            // us for: without the record every later message for this hwnd is dropped.
            LogWarning("out of memory tracking created window 0x%x; its updates will be dropped", window);
            return;
        }
        g_CreatedWindows = grown;
        g_CreatedCapacity = newCapacity;
    }
    g_CreatedWindows[g_CreatedCount++] = window;
}

static void ClearWindowCreatedLocked(IN HWND window)
{
    for (ULONG i = 0; i < g_CreatedCount; i++)
    {
        if (g_CreatedWindows[i] == window)
        {
            g_CreatedWindows[i] = g_CreatedWindows[--g_CreatedCount];
            return;
        }
    }
}

// TRUE if this message may go out. Screen-scoped messages (window == NULL, which the
// protocol carries as window 0) are always allowed - the daemon requires no CREATE for
// those. Call inside the g_VchanCriticalSection hold that would emit the message.
// A reconnecting gui-daemon starts with an empty window table, so everything the previous
// one was told is worthless: without this the set would vouch for windows the new daemon
// has never seen, which is the exact condition it exits on. Call before sending anything
// to a newly connected client.
// Today this is only defence in depth - WatchForEvents runs once per process and returns
// on vchan EOF, so a new client always means a freshly respawned agent with an empty set.
// It matters the moment the agent is made to survive a daemon restart, and costs nothing
// now. (Such an agent would also have to re-announce its existing windows; the gate would
// then correctly drop their traffic rather than let it kill the new daemon.)
void SendResetCreatedWindows(void)
{
    EnterCriticalSection(&g_VchanCriticalSection);
    g_CreatedCount = 0;
    g_GateDrops = 0;
    LeaveCriticalSection(&g_VchanCriticalSection);
}

static BOOL MaySendForWindowLocked(IN HWND window, IN const WCHAR *messageName)
{
    if (!window || IsWindowCreatedLocked(window))
        return TRUE;

    // A window in this state is already broken; log enough to find the bug without
    // flooding the file at frame rate when it repeats.
    g_GateDrops++;
    if (g_GateDrops <= 20 || (g_GateDrops % 1000) == 0)
    {
        LogWarning("dropping %s for 0x%x: no CREATE was sent for this window "
            "(agent bug - sending it would make gui-daemon exit and take down the qube's GUI); "
            "%I64u such drops so far", messageName, window, g_GateDrops);
    }
    return FALSE;
}

ULONG SendWindowCreate(IN const WINDOW_DATA *windowData)
{
    WINDOWINFO wi;
    struct msg_hdr header;
    struct msg_create createMsg;
    ULONG status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    wi.cbSize = sizeof(wi);
    // special case for full screen
    if (windowData == NULL)
    {
        LogDebug("fullscreen");
        // TODO: multiple screens?
        wi.rcWindow.left = 0;
        wi.rcWindow.top = 0;

        wi.rcWindow.right = GetSystemMetrics(SM_CXSCREEN);
        wi.rcWindow.bottom = GetSystemMetrics(SM_CYSCREEN);

        header.window = 0;
    }
    else
    {
        LogDebug("0x%x, (%d,%d) %dx%d, override=%d", windowData->Handle,
                 windowData->X, windowData->Y, windowData->Width, windowData->Height,
                 windowData->IsOverrideRedirect);

#pragma warning(suppress:4311)
        header.window = (uint32_t)windowData->Handle;
        wi.rcWindow.left = windowData->X;
        wi.rcWindow.top = windowData->Y;
        wi.rcWindow.right = windowData->X + windowData->Width;
        wi.rcWindow.bottom = windowData->Y + windowData->Height;
    }

    header.type = MSG_CREATE;

    createMsg.x = wi.rcWindow.left;
    createMsg.y = wi.rcWindow.top;
    createMsg.width = wi.rcWindow.right - wi.rcWindow.left;
    createMsg.height = wi.rcWindow.bottom - wi.rcWindow.top;
#pragma warning(suppress:4311)
    createMsg.parent = UINT32_MAX; // ignored by daemon
    createMsg.override_redirect = windowData ? windowData->IsOverrideRedirect : FALSE;
    LogDebug("(%d,%d) %ux%u", createMsg.x, createMsg.y, createMsg.width, createMsg.height);

    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=CREATE,hwnd=0x%x,x=%d,y=%d,w=%u,h=%u,ovr=%d,style=0x%08x,ex=0x%08x",
            windowData ? (uint32_t)(ULONG_PTR)windowData->Handle : 0,
            createMsg.x, createMsg.y, createMsg.width, createMsg.height,
            createMsg.override_redirect,
            windowData ? windowData->Style : 0, windowData ? windowData->ExStyle : 0);

    EnterCriticalSection(&g_VchanCriticalSection);
    // The OS can reuse a destroyed hwnd: a fresh CREATE re-legitimizes it for damage.
    if (windowData)
        ClearWindowDestroyedLocked(windowData->Handle);
    if (!VCHAN_SEND_MSG(header, createMsg, L"MSG_CREATE"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    // Recorded only after the CREATE is actually on the wire, so the set never claims more
    // than the daemon has been told.
    if (windowData)
        MarkWindowCreatedLocked(windowData->Handle);
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (windowData)
    {
        status = SendWindowHints(windowData->Handle, PPosition); // program-specified position
        if (ERROR_SUCCESS != status)
            return status;
    }

    return ERROR_SUCCESS;
}

ULONG SendWindowDestroy(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogDebug("0x%x", window);
    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=DESTROY,hwnd=0x%x", (uint32_t)(ULONG_PTR)window);
    header.type = MSG_DESTROY;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    // A DESTROY for a window the daemon never saw created is fatal to it just like any
    // other orphaned message, so it goes through the same gate.
    status = MaySendForWindowLocked(window, L"MSG_DESTROY") ? VCHAN_SEND(header, L"MSG_DESTROY") : TRUE;
    // In the same lock hold as the send: from here on, damage for this hwnd is dropped
    // (see the destroyed-ring comment above SendWindowDamageEvent).
    MarkWindowDestroyedLocked(window);
    ClearWindowCreatedLocked(window);
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowFlags(IN HWND window, IN uint32_t flagsToSet, IN uint32_t flagsToUnset)
{
    struct msg_hdr header;
    struct msg_window_flags flags;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogDebug("0x%x: set 0x%x, unset 0x%x", window, flagsToSet, flagsToUnset);
    header.type = MSG_WINDOW_FLAGS;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    flags.flags_set = flagsToSet;
    flags.flags_unset = flagsToUnset;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_WINDOW_FLAGS") ? VCHAN_SEND_MSG(header, flags, L"MSG_WINDOW_FLAGS") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowHints(IN HWND window, IN uint32_t flags)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    hintsMsg.flags = flags;
    LogDebug("flags: 0x%lx", flags);

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_WINDOW_HINTS") ? VCHAN_SEND_MSG(header, hintsMsg, L"MSG_WINDOW_HINTS") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendScreenHints(void)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    hintsMsg.flags = PMinSize; // minimum size
    hintsMsg.min_width = MIN_RESOLUTION_WIDTH;
    hintsMsg.min_height = MIN_RESOLUTION_HEIGHT;
    LogDebug("min %dx%d", hintsMsg.min_width, hintsMsg.min_height);

    header.window = 0; // screen
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = VCHAN_SEND_MSG(header, hintsMsg, L"MSG_WINDOW_HINTS screen");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowUnmap(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    LogInfo("Unmapping window 0x%x", window);

    header.type = MSG_UNMAP;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.untrusted_len = 0;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_UNMAP") ? VCHAN_SEND(header, L"MSG_UNMAP") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

// if windowData == 0, use the whole screen
ULONG SendWindowMap(IN const WINDOW_DATA *windowData OPTIONAL)
{
    struct msg_hdr header;
    struct msg_map_info mapMsg;
    ULONG status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    if (windowData)
        LogInfo("Mapping window 0x%x", windowData->Handle);
    else
        LogInfo("Mapping desktop window");

    header.type = MSG_MAP;
    if (windowData)
#pragma warning(suppress:4311)
        header.window = (uint32_t)windowData->Handle;
    else
        header.window = 0;
    header.untrusted_len = 0;

    if (windowData && windowData->ModalParent)
    {
        LogDebug("0x%x is transient for 0x%x", windowData->Handle, windowData->ModalParent);
#pragma warning(suppress:4311)
        mapMsg.transient_for = (uint32_t)windowData->ModalParent;
    }
    else
    {
        mapMsg.transient_for = 0;
    }

    if (windowData)
        mapMsg.override_redirect = windowData->IsOverrideRedirect;
    else
        mapMsg.override_redirect = 0;

    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=MAP,hwnd=0x%x,ovr=%d,transient=0x%x,style=0x%08x,ex=0x%08x,vis=%d,w=%u,h=%u",
            windowData ? (uint32_t)(ULONG_PTR)windowData->Handle : 0,
            mapMsg.override_redirect, mapMsg.transient_for,
            windowData ? windowData->Style : 0, windowData ? windowData->ExStyle : 0,
            windowData ? windowData->IsVisible : 0,
            windowData ? windowData->Width : 0, windowData ? windowData->Height : 0);

    EnterCriticalSection(&g_VchanCriticalSection);
    if (windowData && !MaySendForWindowLocked(windowData->Handle, L"MSG_MAP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_SUCCESS;
    }
    if (!VCHAN_SEND_MSG(header, mapMsg, L"MSG_MAP"))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    // if the window takes the whole screen (like logon window), try to make it fullscreen in dom0
    if (!windowData || (windowData->Width == g_ScreenWidth && windowData->Height == g_ScreenHeight))
    {
        status = SendScreenHints(); // min/max screen size
        if (ERROR_SUCCESS != status)
            return status;

        status = SendWindowName(NULL, NULL); // desktop
        if (ERROR_SUCCESS != status)
            return status;

        if (g_ScreenWidth == g_HostScreenWidth && g_ScreenHeight == g_HostScreenHeight)
        {
            LogDebug("fullscreen window");
            status = SendWindowFlags(windowData ? windowData->Handle : NULL, WINDOW_FLAG_FULLSCREEN, 0);
            if (ERROR_SUCCESS != status)
                return status;
        }
    }

    return ERROR_SUCCESS;
}

// if window == 0, use the whole screen
ULONG SendWindowConfigure(HANDLE window, int x, int y, int width, int height, BOOL popup)
{
    struct msg_hdr header;
    struct msg_configure configureMsg;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;

    header.type = MSG_CONFIGURE;

    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=CONFIGURE,hwnd=0x%x,x=%d,y=%d,w=%d,h=%d,ovr=%d",
            (uint32_t)(ULONG_PTR)window, x, y, width, height, popup);

    configureMsg.x = x;
    configureMsg.y = y;
    configureMsg.width = width;
    configureMsg.height = height;
    configureMsg.override_redirect = popup;
    LogVerbose("0x%x: (%d,%d) %dx%d ovr=%d", window, configureMsg.x, configureMsg.y,
        configureMsg.width, configureMsg.height, configureMsg.override_redirect);

    BOOL status = TRUE;
    EnterCriticalSection(&g_VchanCriticalSection);

    // don't send resize to 0x0 - this window is just hiding itself, MSG_UNMAP will follow
    if (configureMsg.width > 0 && configureMsg.height > 0)
    {
        status = MaySendForWindowLocked(window, L"MSG_CONFIGURE") ? VCHAN_SEND_MSG(header, configureMsg, L"MSG_CONFIGURE") : TRUE;
        if (!status)
            goto cleanup;
    }

cleanup:
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height)
{
    if (g_ProtoTrace)
    {
        // Wobble is a desync between the geometry dom0 believes and where the window actually
        // is in the live shared framebuffer. Record both at the instant damage goes out: `a*`
        // is the origin this damage was registered against (what dom0 will add back), `l*` is
        // where the window really is right now. A non-zero delta during motion IS the wobble,
        // measured with no cross-VM capture skew.
        RECT live;
        if (window && GetRealWindowRect(window, &live) == ERROR_SUCCESS)
        {
            // Also called from the capture thread: the list walk must hold the lock, and
            // it must be RELEASED before the vchan lock is taken below (the main thread
            // holds the vchan lock while taking this one - see ring comment above).
            EnterCriticalSection(&g_csWatchedWindows);
            WINDOW_DATA* wd = FindWindowByHandle(window);
            int ax = wd ? wd->X : 0, ay = wd ? wd->Y : 0;
            LeaveCriticalSection(&g_csWatchedWindows);
            LogInfo("QGAPROTO,msg=DAMAGE,hwnd=0x%x,rx=%d,ry=%d,w=%d,h=%d,ax=%d,ay=%d,lx=%d,ly=%d",
                (uint32_t)(ULONG_PTR)window, x, y, width, height,
                ax, ay, live.left, live.top);
        }
        else
        {
            LogInfo("QGAPROTO,msg=DAMAGE,hwnd=0x%x,rx=%d,ry=%d,w=%d,h=%d",
                (uint32_t)(ULONG_PTR)window, x, y, width, height);
        }
    }

    struct msg_shmimage shmMsg;
    struct msg_hdr header;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    // Screen-window damage after StopFrameProcessing sent MSG_DESTROY for window 0 (a
    // frame event can already be pending when the capture error is processed) is the
    // same daemon-killer as the per-window case. All window-0 damage is sent from the
    // main thread, which also sets this flag, so a plain check is race-free.
    if (!window && g_LocalScreenDestroyed)
        return ERROR_SUCCESS;

    LogVerbose("0x%x: (%d,%d)-(%d,%d) %dx%d", window, x, y, x + width, y + height, width, height);
    header.type = MSG_SHMIMAGE;
#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    shmMsg.x = x;
    shmMsg.y = y;
    shmMsg.width = width;
    shmMsg.height = height;
    EnterCriticalSection(&g_VchanCriticalSection);
    // Re-check under the lock: the teardown path closes/frees the vchan under this CS,
    // and the first check above races it from the capture thread.
    if (!g_VchanClientConnected)
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_SUCCESS;
    }
    if (window && WasWindowDestroyedLocked(window))
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        LogVerbose("0x%x: dropping damage for destroyed window", window);
        return ERROR_SUCCESS;
    }
    status = MaySendForWindowLocked(window, L"MSG_SHMIMAGE") ? VCHAN_SEND_MSG(header, shmMsg, L"MSG_SHMIMAGE") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendWindowName(IN HWND window, IN const WCHAR *caption OPTIONAL)
{
    struct msg_hdr header;
    struct msg_wmname nameMsg;
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    if (window)
    {
        if (caption)
        {
            StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%S", caption);
        }
        else
        {
            if (0 == GetWindowTextA(window, nameMsg.data, RTL_NUMBER_OF(nameMsg.data)))
            {
                win_perror("GetWindowTextA");
                return ERROR_SUCCESS; // whatever
            }
        }
    }
    else
    {
        StringCchPrintfA(nameMsg.data, RTL_NUMBER_OF(nameMsg.data), "%s (Windows Desktop)", g_DomainName);
    }

    LogDebug("0x%x %S", window, nameMsg.data);

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.type = MSG_WMNAME;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_WMNAME") ? VCHAN_SEND_MSG(header, nameMsg, L"MSG_WMNAME") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}

ULONG SendProtocolVersion(void)
{
    uint32_t version = QUBES_GUID_PROTOCOL_VERSION;

    EnterCriticalSection(&g_VchanCriticalSection);
    BOOL status = VCHAN_SEND(version, L"version");
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : ERROR_UNIDENTIFIED_ERROR;
}
