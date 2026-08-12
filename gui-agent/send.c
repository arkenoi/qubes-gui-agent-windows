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
#include "faultinject.h" // FI_NEG_CREATE / FI_DUP_CREATE aim at the two rules below
#include "resolution.h" // ResolutionAdoptCurrent, for the A3CHECK self-heal
#include "capture.h" // FRAMEBUFFER_PAGE_COUNT, for the A3CHECK instrumentation

#include <qubes-gui-protocol.h>

#include <log.h>

#include <strsafe.h>

static_assert(sizeof(ULONG) == sizeof(uint32_t), "ULONG has a different size than uint32_t");

// Defined with the created-window set below; declared here because MSG_WINDOW_DUMP is
// emitted above it and is just as fatal to the daemon when the window has no CREATE.
static BOOL MaySendForWindowLocked(IN HWND window, IN const WCHAR *messageName);

// --- wire geometry sanitizer ----------------------------------------------------------
// dom0's gui-daemon is a CONTRACT, and its geometry contract is not "be reasonable" - it
// is a specific set of comparisons in xside.c, each of which either clamps the value,
// pops a modal dialog, or exits the process:
//
//   MSG_CREATE       xside.c:2937       VERIFY((int)width >= 0 && (int)height >= 0) -> DIALOG
//                    xside.c:2939-2946  w=min(w,MAX_W); h=min(h,MAX_H);
//                                       x=max(-MAX_W,min(x,MAX_W)); y likewise
//   MSG_CONFIGURE    xside.c:2093-2103  the same clamps, then VERIFY(w > 0 && h > 0) -> DIALOG
//   MSG_SHMIMAGE     xside.c:2261       any negative field -> the update is silently discarded
//   MSG_WINDOW_DUMP  xside.c:3894       size above the protocol maximum -> errx(1), daemon dies
//
// WHY THE DIALOG IS THE POINT. While that modal dialog is up the daemon stops draining the
// vchan. Measured 2026-08-11: the dialog preceded the vchan flood by ~6.5 s and the capture
// thread died behind it. One suspicious message is therefore a display DoS for the entire
// qube, so a message that would raise it must never be put on the wire at all - hoping the
// user clicks Ignore is not a design.
//
// WHY HERE AND NOT AT INGESTION. The per-window capture crop origin is derived from the RAW
// WINDOW_DATA X/Y (perwindow.c), so clamping there would move the crop and the pixels would
// no longer match the rect we announce. The wire is the only place where the announced
// value can be corrected without moving anything else.
//
// This never weakens a dom0-side check: it makes the agent satisfy the check that already
// exists, and dom0 re-validates everything regardless.

// Rate-limit counters for the two classes of refusal. Deliberately NOT atomic and
// deliberately not taken under g_VchanCriticalSection: they only decide whether a line is
// logged, a lost increment costs at most one duplicated or missing line, and holding the
// send lock across log file I/O would add latency to every other sender. Two separate
// counters so that a flood of dropped damage cannot consume the budget that would have made
// a single dropped CREATE visible.
static ULONG g_GeomDrops;    // MSG_CREATE / MSG_CONFIGURE / MSG_WINDOW_DUMP
static ULONG g_GeomClamps;   // values rewritten rather than refused
static ULONG g_DamageDrops;  // MSG_SHMIMAGE

// The evidence hook. The 25H2 negative-geometry case has never been reproduced here (four
// storm runs, zero occurrences), so when it does recur in the field this line is the only
// thing that will say which message and which field went wrong.
static void LogGeometryRefused(IN OUT ULONG *counter, IN HWND window,
    IN const WCHAR *messageName, IN const WCHAR *rule,
    IN int x, IN int y, IN int width, IN int height)
{
    ULONG count = ++(*counter);

    // Same shape as the created-window gate below: loud while it is news, decimated after,
    // so a stuck path cannot turn the log file into the next problem.
    if (count <= 20 || (count % 1000) == 0)
    {
        LogWarning("GEOMDROP msg=%s hwnd=0x%x rule=%s x=%d y=%d w=%d h=%d - message not sent; "
            "%u such drops so far", messageName, window, rule, x, y, width, height, count);
    }
}

// Make x/y/width/height exactly what gui-daemon would have stored for MSG_CREATE and
// MSG_CONFIGURE, or refuse the message outright. Returns FALSE when the caller must not
// send anything at all.
static BOOL SanitizeWireGeometry(IN HWND window, IN const WCHAR *messageName,
    IN OUT int32_t *x, IN OUT int32_t *y, IN OUT uint32_t *width, IN OUT uint32_t *height)
{
    // Read SIGNED, which is the whole trick: the protocol fields are uint32_t, so an
    // inverted rect arrives as a huge positive number and only the cast xside.c:2937 itself
    // performs can see it. The old "configureMsg.width > 0" test was an unsigned comparison
    // and a negative width sailed straight through it.
    int32_t w = (int32_t)*width;
    int32_t h = (int32_t)*height;
    int32_t cx = *x;
    int32_t cy = *y;

    if (w <= 0 || h <= 0)
    {
        // CREATE dialogs on < 0 and CONFIGURE on <= 0; one rule for both, taken as the
        // stricter of the two, because a zero-sized window is nothing dom0 can render.
        LogGeometryRefused(&g_GeomDrops, window, messageName,
            L"width/height not positive (dom0 VERIFY: modal dialog, stops draining the vchan)",
            cx, cy, w, h);
        return FALSE;
    }

    if (w > MAX_WINDOW_WIDTH)
        w = MAX_WINDOW_WIDTH;
    if (h > MAX_WINDOW_HEIGHT)
        h = MAX_WINDOW_HEIGHT;
    if (cx > MAX_WINDOW_WIDTH)
        cx = MAX_WINDOW_WIDTH;
    if (cx < -MAX_WINDOW_WIDTH)
        cx = -MAX_WINDOW_WIDTH;
    if (cy > MAX_WINDOW_HEIGHT)
        cy = MAX_WINDOW_HEIGHT;
    if (cy < -MAX_WINDOW_HEIGHT)
        cy = -MAX_WINDOW_HEIGHT;

    if (cx != *x || cy != *y || (uint32_t)w != *width || (uint32_t)h != *height)
    {
        // Not a failure: dom0 would have stored these values anyway. Sending the clamped
        // ones is what keeps both sides' idea of the window identical - every later
        // CONFIGURE and every damage rect is computed against it - instead of only the
        // agent believing in a rect dom0 silently narrowed.
        ULONG count = ++g_GeomClamps;
        if (count <= 20 || (count % 1000) == 0)
        {
            LogWarning("GEOMCLAMP msg=%s hwnd=0x%x (%d,%d) %dx%d -> (%d,%d) %dx%d "
                "(dom0 clamps to %dx%d, xside.c:2939-2946); %u so far",
                messageName, window, *x, *y, (int)*width, (int)*height,
                cx, cy, w, h, MAX_WINDOW_WIDTH, MAX_WINDOW_HEIGHT, count);
        }
        *x = cx;
        *y = cy;
        *width = (uint32_t)w;
        *height = (uint32_t)h;
    }

    return TRUE;
}

ULONG SendWindowDump(IN HWND window, IN ULONG width, IN ULONG height,
    IN size_t numGrants, IN const ULONG* refs)
{
    ULONG status = ERROR_INVALID_PARAMETER;
    struct msg_hdr header;
    struct msg_window_dump_hdr dumpHdr;
    VCHAN_IOV iov[3];

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

    // The one geometry dom0 does not merely dislike: xside.c:3894 calls errx(1) - the daemon
    // dies and the whole qube's GUI dies with it - on a dump above the protocol maximum, and
    // a zero-sized dump describes no image at all. NOT clamped, unlike CREATE/CONFIGURE: the
    // header has to describe the buffer the grant refs actually cover, so a wrong-but-
    // accepted header would put a corrupt image on the screen. Refusing leaves whatever dom0
    // already has mapped, which is the better of the two failures.
    if ((int)width <= 0 || (int)height <= 0 ||
        width > MAX_WINDOW_WIDTH || height > MAX_WINDOW_HEIGHT)
    {
        LogGeometryRefused(&g_GeomDrops, window, L"MSG_WINDOW_DUMP",
            L"size outside (0, protocol maximum] (dom0 xside.c:3894 errx(1)s on it)",
            0, 0, (int)width, (int)height);
        status = ERROR_INVALID_DATA;
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

    dumpHdr.type = WINDOW_DUMP_TYPE_GRANT_REFS;
    dumpHdr.bpp = 32;
    dumpHdr.width = width;
    dumpHdr.height = height;

    // ONE message, ONE reservation. The daemon reads sizeof(msg_hdr) and then exactly
    // untrusted_len further bytes, so these three buffers are a single indivisible unit on
    // the wire. Sent as three independent writes (as this was), a send that gave up
    // between them - which the bounded send in vchan.c can now do - would leave the daemon
    // reading the NEXT message's bytes as this dump's grant refs, and there is no
    // resynchronisation point in the protocol. VchanSendVectored either places all of it
    // or none of it, and is also the only path that can carry a grant slab larger than the
    // write ring (protocol maximum geometry = 384 KiB of refs).
    iov[0].Data = &header;
    iov[0].Size = sizeof(header);
    iov[1].Data = &dumpHdr;
    iov[1].Size = sizeof(dumpHdr);
    iov[2].Data = refs;
    iov[2].Size = numGrants * sizeof(ULONG);

    status = ERROR_SUCCESS;
    if (!VchanSendVectored(g_Vchan, iov, (int)RTL_NUMBER_OF(iov), L"MSG_WINDOW_DUMP"))
    {
        status = win_perror2(VchanLastSendError(), "VchanSendVectored(MSG_WINDOW_DUMP)");
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

end:
    LogVerbose("end (%x)", status);

    return status;
}

ULONG SendScreenGrants(IN size_t numGrants, IN const ULONG* refs,
    IN UINT ctxWidth, IN UINT ctxHeight)
{
    // Single source of truth: the header geometry (here) and the grant count (at the call
    // sites) both derive from the capture context's DXGI desc - the size grant_refs was
    // actually allocated for. g_ScreenWidth/Height are written by the resolution-change
    // thread and can lag or lead the capture geometry; a count derived from them can be
    // short (dom0's gui-daemon exit(1)s) or long (reads past the malloc'd grant_refs).
    // The A3CHECK instrumentation stays so the fix is visible: a fixed build must log
    // zero A3MISMATCH.
    ULONG dumpWidth = ctxWidth;
    ULONG dumpHeight = ctxHeight;
    if (ctxWidth == 0 || ctxHeight == 0)
    {
        // capture geometry not available: keep the legacy g_Screen* path, visibly
        LogInfo("A3FALLBACK g=%lux%lu", g_ScreenWidth, g_ScreenHeight);
        dumpWidth = g_ScreenWidth;
        dumpHeight = g_ScreenHeight;
    }
    size_t pagesG = FRAMEBUFFER_PAGE_COUNT(g_ScreenWidth, g_ScreenHeight);
    size_t pagesCtx = FRAMEBUFFER_PAGE_COUNT(ctxWidth, ctxHeight);
    LogInfo("A3CHECK g=%lux%lu ctx=%ux%u pages_g=%lu pages_ctx=%lu",
        g_ScreenWidth, g_ScreenHeight, ctxWidth, ctxHeight, (ULONG)pagesG, (ULONG)pagesCtx);

    // Self-heal, not just a log line: the capture context's DXGI desc IS the desktop
    // Windows composits, so a divergent g_Screen* is stale belief (the live-proven case:
    // an out-of-band mode reversion nobody re-read - FINDINGS 2026-08-12). Adopt reality
    // via the same guarded path the drift tick uses; it no-ops during an exact obtain's
    // settle window, so transit sizes cannot fight an in-flight resolution change.
    if (ctxWidth != 0 && ctxHeight != 0 &&
        (g_ScreenWidth != ctxWidth || g_ScreenHeight != ctxHeight))
        ResolutionAdoptCurrent();
    // Count check is one-sided BY DESIGN: too small is the fatal direction - gui-daemon
    // exit(1)s when the dump's data size is below width*height*4 (xside.c:3903-3913) -
    // while a LARGER count is accepted. The staging grant (capture.c) intentionally
    // sends its constant full-capacity count with every geometry, so numGrants >
    // pagesCtx is the normal steady state there, not a mismatch.
    if (dumpWidth != ctxWidth || dumpHeight != ctxHeight || numGrants < pagesCtx)
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

// The screen pseudo-window (protocol window 0) has no HWND to key the set on, and it is the
// one CREATE whose duplicate is documented to kill the daemon outright - handle_message
// exit(1)s on a CREATE for an id it already tracks (xside.c:3943-3948). main.c tracks the
// same fact as g_ScreenAnnounced for its own control flow; this is the copy held by the code
// that actually emits the message, under the lock that orders it.
static BOOL g_ScreenCreated;

// Rate limit for suppressed duplicate CREATEs (see SendWindowCreateInternal).
static ULONG g_DupCreates;

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
    // A new daemon has never been told about the screen either, so the create-once rule
    // must let the next CREATE(0) through or the qube would render nothing at all.
    g_ScreenCreated = FALSE;
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

// allowDuplicate is TRUE for exactly one caller, SendWindowCreateForced, which exists only
// so FI_DUP_CREATE can re-introduce the defect the create-once rule below removes.
static ULONG SendWindowCreateInternal(IN const WINDOW_DATA *windowData, IN BOOL allowDuplicate)
{
    WINDOWINFO wi;
    struct msg_hdr header;
    struct msg_create createMsg;
    ULONG status;
    HWND window = windowData ? windowData->Handle : NULL; // NULL == the screen pseudo-window

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

    // FAULT INJECTION (test builds only, rank 1). Puts the historical 25H2 rect on the wire
    // so the sanitizer below can be SEEN to catch it, and so that with the sanitizer removed
    // xside.c:2937's dialog can be seen to fire. It has to run before the sanitizer: the
    // sanitizer is the code under test. Asked exactly once per CREATE, as the shot is
    // consumed by the query.
    if (FiShouldNegCreate(window))
    {
        createMsg.width = (uint32_t)(-(int32_t)createMsg.width);
        createMsg.height = (uint32_t)(-(int32_t)createMsg.height);
    }

    // THE choke point. A refused CREATE is reported with a status no transport failure can
    // produce, because the caller must react differently: AddWindow has to leave CreateSent
    // FALSE so that the created-window gate quarantines every dependent message for this
    // hwnd. Failure then degrades to "the window is invisible until a later pass re-creates
    // it with a sane rect", never to agent and daemon disagreeing about what exists.
    if (!SanitizeWireGeometry(window, L"MSG_CREATE", &createMsg.x, &createMsg.y,
        &createMsg.width, &createMsg.height))
    {
        return ERROR_INVALID_DATA;
    }

    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=CREATE,hwnd=0x%x,x=%d,y=%d,w=%u,h=%u,ovr=%d,style=0x%08x,ex=0x%08x",
            windowData ? (uint32_t)(ULONG_PTR)windowData->Handle : 0,
            createMsg.x, createMsg.y, createMsg.width, createMsg.height,
            createMsg.override_redirect,
            windowData ? windowData->Style : 0, windowData ? windowData->ExStyle : 0);

    EnterCriticalSection(&g_VchanCriticalSection);

    // CREATE ONCE PER HWND LIFETIME. gui-daemon exit(1)s on a CREATE for an id it already
    // tracks (xside.c:3943-3948), and a Start-menu flip storm emitted 13 CREATEs for one
    // LIVE hwnd on 2026-08-11. What a repeat caller actually needs dom0 to learn is the
    // window's current geometry, and for a window dom0 already has that is MSG_CONFIGURE.
    // The test is the created-window set, not WINDOW_DATA.CreateSent, for the same reason
    // the send gate uses it: this set records what the DAEMON was told, under the very lock
    // that ordered the telling. HWND reuse is therefore handled for free - SendWindowDestroy
    // calls ClearWindowCreatedLocked in the same hold as MSG_DESTROY, so a recycled handle
    // is absent from the set and creates normally.
    if (!allowDuplicate && (windowData ? IsWindowCreatedLocked(window) : g_ScreenCreated))
    {
        // Created-set membership means the daemon still tracks this id, so damage for it is
        // legitimate. Without this, a DESTROY that FAILED to send (degraded ring) leaves the
        // hwnd in the created set AND the destroyed ring: this branch then suppresses the
        // re-CREATE (correct) but the stale destroyed-ring mark keeps dropping the window's
        // damage forever (adversarially verified 2026-08-12, send.c 626 vs 634-636).
        if (windowData)
            ClearWindowDestroyedLocked(windowData->Handle);

        // Released before the follow-up below, which takes the same lock.
        LeaveCriticalSection(&g_VchanCriticalSection);

        ULONG count = ++g_DupCreates;
        if (count <= 20 || (count % 1000) == 0)
        {
            LogWarning("CREATEDUP suppressing a second MSG_CREATE for 0x%x ((%d,%d) %ux%u): "
                "dom0 exits on a CREATE for an id it already tracks; sending MSG_CONFIGURE "
                "instead; %u suppressed so far",
                window, createMsg.x, createMsg.y, createMsg.width, createMsg.height, count);
        }

        // The status deliberately ignores the follow-up's: the CREATE the caller asked for
        // has been satisfied since the first one, and a lost CONFIGURE is a lost geometry
        // update, not a failed announcement. Reporting failure here would make AddWindow
        // leave CreateSent FALSE for a window the daemon does have, after which RemoveWindow
        // would skip its MSG_DESTROY and strand the hwnd in the created set forever.
        if (windowData)
        {
            (void)SendWindowConfigure(window, createMsg.x, createMsg.y,
                (int)createMsg.width, (int)createMsg.height, createMsg.override_redirect);
        }
        return ERROR_SUCCESS;
    }

    // The OS can reuse a destroyed hwnd: a fresh CREATE re-legitimizes it for damage.
    if (windowData)
        ClearWindowDestroyedLocked(windowData->Handle);
    if (!VCHAN_SEND_MSG(header, createMsg, L"MSG_CREATE"))
    {
        // Distinguish the two failures rather than flattening both to
        // ERROR_UNIDENTIFIED_ERROR: ERROR_VC_DISCONNECTED means the daemon is gone (the
        // handshake and the other fatal callers may stop at once), ERROR_TIMEOUT means it
        // is alive but wedged, which the never-exit paths treat as a lost message. Every
        // caller only tests against ERROR_SUCCESS, so nothing changes for them.
        LeaveCriticalSection(&g_VchanCriticalSection);
        return VchanLastSendError();
    }
    // Recorded only after the CREATE is actually on the wire, so the set never claims more
    // than the daemon has been told.
    if (windowData)
        MarkWindowCreatedLocked(windowData->Handle);
    else
        g_ScreenCreated = TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (windowData)
    {
        status = SendWindowHints(windowData->Handle, PPosition); // program-specified position
        if (ERROR_SUCCESS != status)
            return status;

        // A WM-managed shell surface (Start/toast: or=0 with a crop) is draggable but must
        // NOT be resizeable - lock its size so the dom0 WM shows no resize handles. A resize
        // would stretch the frame past the fixed-size guest card and expose desktop behind
        // it. Keyed on the crop, so ordinary windows are unaffected.
        if (!windowData->IsOverrideRedirect &&
            (windowData->CropLeft || windowData->CropTop ||
             windowData->CropRight || windowData->CropBottom))
        {
            status = SendWindowSizeLock(windowData->Handle, windowData->Width, windowData->Height);
            if (ERROR_SUCCESS != status)
                return status;
        }
    }

    return ERROR_SUCCESS;
}

ULONG SendWindowCreate(IN const WINDOW_DATA *windowData)
{
    return SendWindowCreateInternal(windowData, FALSE);
}

// FI_DUP_CREATE ONLY. There is no legitimate caller: a second MSG_CREATE for a live id is
// precisely what gui-daemon exit(1)s on. It exists so the create-once rule above can be
// shown to FAIL when the defect is deliberately re-introduced, which is the only way its
// PASS counts (CLAUDE.md). In a release build its single call site sits behind a
// FiShouldDupCreate() that is compiled to a constant FALSE, so nothing can reach it.
ULONG SendWindowCreateForced(IN const WINDOW_DATA *windowData)
{
    return SendWindowCreateInternal(windowData, TRUE);
}

ULONG SendWindowDestroy(IN HWND window)
{
    struct msg_hdr header;
    BOOL status;
    BOOL gated;

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
    gated = MaySendForWindowLocked(window, L"MSG_DESTROY");
    status = gated ? VCHAN_SEND(header, L"MSG_DESTROY") : TRUE;

    // In the same lock hold as the send: from here on, damage for this hwnd is dropped
    // (see the destroyed-ring comment above SendWindowDamageEvent). Unconditional - the
    // window is gone on OUR side regardless of what reached the daemon, and damage for a
    // dead hwnd must never be sent.
    MarkWindowDestroyedLocked(window);

    // But the created-set is our model of what the DAEMON knows, so it may only be cleared
    // when the daemon has actually been told, or when the gate proved it never knew this
    // window. Since a send can now fail while the connection SURVIVES (VCHANSLOW), clearing
    // unconditionally would let a later CREATE for the same hwnd through to a daemon that
    // still has it tracked - and an already-tracked id is one of the cases the daemon
    // answers with exit(1), killing every window of this qube.
    if (!gated || status)
    {
        ClearWindowCreatedLocked(window);
        // The daemon forgets window 0 on DESTROY, so the create-once rule must let the next
        // CREATE(0) through - StopFrameProcessing destroys and re-announces the screen on every
        // capture restart, and blocking that would leave the qube with no screen window at all.
        if (!window)
            g_ScreenCreated = FALSE;
    }
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : VchanLastSendError();
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
}

// Lock a window's size on the dom0 side: PMinSize == PMaxSize means the WM offers no resize
// handles at all. Used for WM-managed shell surfaces (Start/toasts) - they are DRAGGABLE but
// must not be RESIZEABLE: the guest card is a fixed size, and a dom0 resize would stretch the
// frame past the card, exposing desktop behind it (user-reported 2026-08-12). width/height are
// the ANNOUNCED (cropped) size.
ULONG SendWindowSizeLock(IN HWND window, IN uint32_t width, IN uint32_t height)
{
    struct msg_hdr header;
    struct msg_window_hints hintsMsg = { 0 };
    BOOL status;

    if (!g_VchanClientConnected)
        return ERROR_SUCCESS;

    hintsMsg.flags = PMinSize | PMaxSize;
    hintsMsg.min_width = width;
    hintsMsg.min_height = height;
    hintsMsg.max_width = width;
    hintsMsg.max_height = height;

#pragma warning(suppress:4311)
    header.window = (uint32_t)window;
    header.type = MSG_WINDOW_HINTS;

    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_WINDOW_HINTS sizelock")
        ? VCHAN_SEND_MSG(header, hintsMsg, L"MSG_WINDOW_HINTS sizelock") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : VchanLastSendError();
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
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
        return VchanLastSendError();
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

    configureMsg.x = x;
    configureMsg.y = y;
    configureMsg.width = width;
    configureMsg.height = height;
    configureMsg.override_redirect = popup;

    // don't send resize to 0x0 - this window is just hiding itself, MSG_UNMAP will follow.
    // Kept as its own QUIET case ahead of the sanitizer: zero is routine and expected, and
    // it must not consume the GEOMDROP log budget that has to stay available for the rects
    // that are actually anomalous.
    if (width == 0 || height == 0)
        return ERROR_SUCCESS;

    // Only negatives can be refused here, and a negative CONFIGURE is genuinely anomalous:
    // it is xside.c:2098's VERIFY(width > 0 && height > 0), the modal dialog that stops the
    // daemon draining the vchan. Note that it was NOT caught before - the old test was
    // "configureMsg.width > 0" on the uint32_t field, an UNSIGNED comparison that every
    // negative width passed, after which dom0 clamped 0xFFFFFFFF up to MAX_WINDOW_WIDTH
    // (xside.c:2093) and rendered a 16384 px wide window.
    // A refusal returns success: unlike MSG_CREATE there is no registration state riding on
    // this message, so dom0 simply keeps the rect it already has - which is the correct
    // outcome - and no caller loop should abort over one bad geometry update.
    if (!SanitizeWireGeometry(window, L"MSG_CONFIGURE", &configureMsg.x, &configureMsg.y,
        &configureMsg.width, &configureMsg.height))
    {
        return ERROR_SUCCESS;
    }

    // Traced AFTER sanitizing, so the trace shows what actually went on the wire.
    if (g_ProtoTrace)
        LogInfo("QGAPROTO,msg=CONFIGURE,hwnd=0x%x,x=%d,y=%d,w=%u,h=%u,ovr=%d",
            (uint32_t)(ULONG_PTR)window, configureMsg.x, configureMsg.y,
            configureMsg.width, configureMsg.height, popup);

    LogVerbose("0x%x: (%d,%d) %ux%u ovr=%d", window, configureMsg.x, configureMsg.y,
        configureMsg.width, configureMsg.height, configureMsg.override_redirect);

    BOOL status;
    EnterCriticalSection(&g_VchanCriticalSection);
    status = MaySendForWindowLocked(window, L"MSG_CONFIGURE") ? VCHAN_SEND_MSG(header, configureMsg, L"MSG_CONFIGURE") : TRUE;
    LeaveCriticalSection(&g_VchanCriticalSection);

    return status ? ERROR_SUCCESS : VchanLastSendError();
}

ULONG SendWindowDamageEvent(IN HWND window, IN int x, IN int y, IN int width, IN int height)
{
    // dom0 discards any update with a negative field on the floor (xside.c:2261), so this
    // one cannot hurt the daemon - but it is wire traffic that can never draw anything, and
    // a negative value here means the damage was computed against an origin the window no
    // longer has. Refuse it and record it: the log line is the only evidence that would
    // otherwise exist. Zero-sized rects are deliberately left alone - dom0 accepts them and
    // they are ordinary churn, not a defect. Placed before the trace block so a dropped rect
    // does not pay for GetRealWindowRect.
    if (x < 0 || y < 0 || width < 0 || height < 0)
    {
        LogGeometryRefused(&g_DamageDrops, window, L"MSG_SHMIMAGE",
            L"negative damage rect (dom0 xside.c:2261 discards it unseen)", x, y, width, height);
        return ERROR_SUCCESS; // dropped on purpose; not an error the caller can act on
    }

    if (g_ProtoTrace)
    {
        // Wobble is a desync between the geometry dom0 believes and where the window actually
        // is in the live shared framebuffer. Record both at the instant damage goes out: `a*`
        // is the origin this damage was registered against (what dom0 will add back), `l*` is
        // where the window really is right now. A non-zero delta during motion IS the wobble,
        // measured with no cross-VM capture skew.
        //
        // The live-rect probe is a DWM query plus a g_csWatchedWindows acquisition PER RECT,
        // and a drag emits hundreds of rects per second - measured as the dominant tail cost
        // of ProtoTrace itself (tot max 580 ms vs 66 ms with it off, FINDINGS 2026-08-12).
        // It is diagnostic-grade, so it hides behind its own switch; plain ProtoTrace keeps
        // the cheap constant-cost line.
        RECT live;
        if (g_ProtoTraceWobble && window && GetRealWindowRect(window, &live) == ERROR_SUCCESS)
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
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

    return status ? ERROR_SUCCESS : VchanLastSendError();
}

ULONG SendProtocolVersion(void)
{
    uint32_t version = QUBES_GUID_PROTOCOL_VERSION;

    EnterCriticalSection(&g_VchanCriticalSection);
    BOOL status = VCHAN_SEND(version, L"version");
    LeaveCriticalSection(&g_VchanCriticalSection);

    if (!status)
    {
        // The handshake is the one caller that must fail fast rather than degrade - with
        // no version exchanged there is no protocol to speak - and it is the one place
        // where knowing WHICH failure happened changes the diagnosis. Both end in the same
        // exit, so the distinction has to be in the log or it is lost.
        LogError("could not send the protocol version: the daemon is %s",
            VchanLastSendResult() == VCHAN_SEND_DEAD
                ? L"gone (vchan closed)"
                : L"connected but has not drained the vchan for seconds");
        return VchanLastSendError();
    }

    return ERROR_SUCCESS;
}
