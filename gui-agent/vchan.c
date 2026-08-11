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
#include <assert.h>

#include "vchan.h"
#include "perf.h"
#include "faultinject.h"

#include <libvchan.h>
#include <vchan-common.h>
#include <log.h>

// Defined in main.c. Cleared here when a send gives up, which is what stops every gated
// send in send.c from trying again (they all return early on it).
extern BOOL g_VchanClientConnected;

CRITICAL_SECTION g_VchanCriticalSection;

struct libvchan *g_Vchan = NULL;

// Piece size used for the messages that cannot fit in the ring at all (see
// VchanSendOversizeLocked). Small enough that the reservation is satisfiable with a wide
// margin even if the ring turns out smaller than VCHAN_WRITE_RING_SIZE, and small enough
// that the daemon gets to drain between pieces.
#define VCHAN_SEND_CHUNK_SIZE 16384

// All four are written only under g_VchanCriticalSection, which every sender holds.
static BOOL              g_VchanSendWedged = FALSE;
static VCHAN_SEND_RESULT g_VchanWedgeResult = VCHAN_SEND_OK;
static ULONG64           g_VchanSendGiveUps = 0;

// Set when the daemon is CONNECTED but not draining (UNRESPONSIVE), cleared as soon as the
// ring has room again. Deliberately NOT the same thing as the wedge: the primary way this
// happens is a human staring at gui-daemon's modal VERIFY dialog, and a person takes far
// longer than VCHAN_SEND_DEADLINE_MS to read and dismiss one. Treating that as a dead peer
// would tear down a connection that is about to recover on its own, turning a few dropped
// frames into a forced respawn.
static BOOL              g_VchanSendDegraded = FALSE;

// Per-thread, like the perf counters: the capture thread, the resolution thread and the
// main loop all send, and each caller must read back its OWN failure reason.
static __declspec(thread) VCHAN_SEND_RESULT g_VchanLastSendResult = VCHAN_SEND_OK;

// A send has given up. Called under the sender's g_VchanCriticalSection hold.
static void VchanGiveUpLocked(IN struct libvchan *vchan, IN VCHAN_SEND_RESULT result,
    IN const WCHAR *what, IN size_t size)
{
    g_VchanLastSendResult = result;

    if (result == VCHAN_SEND_UNRESPONSIVE)
    {
        // The peer is still THERE, just not reading - the modal-dialog case. Drop this
        // message and make the next ones fail fast, but keep the connection: recovery is
        // one drained ring away and happens by itself (see VchanSendVectoredLocked).
        // Nothing is latched and g_VchanClientConnected stays TRUE, so the main loop does
        // not treat this as a disconnect and does not respawn the agent under a user who
        // is merely reading a dialog.
        g_VchanSendDegraded = TRUE;

        g_VchanSendGiveUps++;
        if (g_VchanSendGiveUps <= 20 || (g_VchanSendGiveUps % 1000) == 0)
        {
            LogWarning("VCHANSLOW dropping %s (%I64u bytes): gui-daemon has not drained the "
                "ring in %u ms (open=%d, ring free=%d) - connection KEPT, sends fail fast "
                "until it drains; %I64u drops so far",
                what, (ULONG64)size, VCHAN_SEND_DEADLINE_MS,
                libvchan_is_open(vchan), VchanGetWriteBufferSize(vchan), g_VchanSendGiveUps);
        }
        return;
    }

    // Latch before clearing the connected flag: the flag alone is ambiguous (it is also
    // FALSE before the first client connects), and the main loop must be able to tell the
    // two apart before it re-runs the handshake.
    g_VchanSendWedged = TRUE;
    g_VchanWedgeResult = result;

    // Already held by the caller, so this is the same lock hold that ordered the message
    // we just failed to write - no window in which another thread starts a send believing
    // the daemon is still there.
    g_VchanClientConnected = FALSE;

    // Same rate limit as the created-window gate in send.c: loud on the first occurrences,
    // decimated afterwards, so a stuck path cannot turn the log file into the next problem.
    g_VchanSendGiveUps++;
    if (g_VchanSendGiveUps <= 20 || (g_VchanSendGiveUps % 1000) == 0)
    {
        LogWarning("VCHANWEDGE giving up on %s (%I64u bytes): daemon %s "
            "(open=%d, ring free=%d, deadline %u ms) - the vchan client is declared gone "
            "and nothing more is sent on this connection; %I64u give-ups so far",
            what, (ULONG64)size,
            result == VCHAN_SEND_DEAD ? L"disconnected" : L"not draining the ring",
            libvchan_is_open(vchan), VchanGetWriteBufferSize(vchan),
            VCHAN_SEND_DEADLINE_MS, g_VchanSendGiveUps);
    }
}

// Wait until the write ring can take `size` bytes, then return TRUE with that space
// RESERVED: the caller holds g_VchanCriticalSection and is therefore the only writer, and
// the daemon can only ever consume, so from here until we write the free space is
// monotonically non-decreasing. The reservation cannot be stolen and the writes it covers
// cannot block.
//
// mayAbort == FALSE means the deadline may not end the wait: the caller has already put
// bytes of this message into the ring, and abandoning it there would leave the daemon
// consuming the next message's bytes as this one's body - a permanent desync of a stream
// that has no resynchronisation point. The only escape then is a peer that has actually
// gone away, where there is no stream left to desync.
static BOOL VchanReserveLocked(IN struct libvchan *vchan, IN size_t size, IN const WCHAR *what,
    IN BOOL mayAbort, OUT VCHAN_SEND_RESULT *result)
{
    // Set on the first miss, not on entry: a send that has room must not pay for a clock
    // read. The deadline therefore runs from the first observation that the ring is full,
    // microseconds after entry.
    ULONGLONG start = 0;
    ULONGLONG firstBlock = 0;
    BOOL blocked = FALSE;

    for (;;)
    {
        // Hot path first and alone: one ring query, no clock read, no liveness call. This
        // is the only added work on a send that has room, which is all of them in the
        // steady state.
        // [FI_RING_STALL] fakes "no room" without consulting the ring, so the deadline
        // path below can be exercised on demand (rank 1). Compiled out entirely in a
        // release build.
        int space = FiRingStallActive() ? 0 : VchanGetWriteBufferSize(vchan);
        if (space >= 0 && (size_t)space >= size)
        {
            if (blocked)
            {
                LogInfo("VCHANWAIT %s: ring drained after %I64u ms, %I64u bytes sent",
                    what, GetTickCount64() - start, (ULONG64)size);
            }
            return TRUE;
        }

        // No room. Now the expensive questions are worth asking.
        if (!libvchan_is_open(vchan))
        {
            // Safe to return even mid-message: the stream is gone, there is nothing left
            // to keep in sync.
            *result = VCHAN_SEND_DEAD;
            return FALSE;
        }

        if (!blocked)
        {
            blocked = TRUE;
            start = GetTickCount64();
            firstBlock = start;
            LogWarning("VCHANWAIT %s: ring full (need %I64u, free %d) - waiting up to %u ms "
                "for gui-daemon to drain", what, (ULONG64)size, space, VCHAN_SEND_DEADLINE_MS);
        }
        else if (GetTickCount64() - start >= (ULONGLONG)VCHAN_SEND_DEADLINE_MS)
        {
            if (mayAbort)
            {
                *result = VCHAN_SEND_UNRESPONSIVE;
                return FALSE;
            }

            // Committed message: a give-up here desyncs the daemon's stream, so keep
            // waiting - but NOT forever. An unbounded wait here would re-create the exact
            // wedge this whole layer exists to remove, for the one message class that can
            // reach it (an oversize MSG_WINDOW_DUMP on a very large desktop). Past the hard
            // cap the choice is between a desynced stream and a permanently frozen guest
            // display, and the desync is the recoverable one: DEAD makes the caller give up,
            // which clears g_VchanClientConnected, latches the wedge, and takes the process
            // down for a clean respawn - the only state from which the stream is well
            // defined again.
            if (GetTickCount64() - firstBlock >= (ULONGLONG)VCHAN_SEND_COMMITTED_DEADLINE_MS)
            {
                LogError("VCHANWAIT %s: no room after %u ms with a message already partly "
                    "written - abandoning the connection (the stream is desynced; the agent "
                    "exits for a clean respawn rather than hanging the display forever)",
                    what, VCHAN_SEND_COMMITTED_DEADLINE_MS);
                *result = VCHAN_SEND_DEAD;
                return FALSE;
            }

            LogWarning("VCHANWAIT %s: still no room after %u ms and this message is already "
                "partly written - waiting on (a desync would be unrecoverable), %I64u ms of "
                "%u before the connection is abandoned",
                what, VCHAN_SEND_DEADLINE_MS,
                (ULONG64)(GetTickCount64() - firstBlock), VCHAN_SEND_COMMITTED_DEADLINE_MS);
            start = GetTickCount64();
        }

        Sleep(1);
    }
}

// Messages that cannot fit in the ring at all. A protocol-legal MSG_WINDOW_DUMP for a
// maximum-geometry window carries MAX_GRANT_REFS_COUNT (98304) refs = 384 KiB of grant
// slab, six times the whole write ring, so reserving it whole would deadlock by
// construction. This is the only case that streams.
//
// The rule that keeps (b) intact: the FIRST piece is reserved abortably, because nothing
// has been written yet and a give-up there still leaves the daemon at a message boundary.
// From the moment the first byte is in the ring the message is committed - every later
// reservation is unabortable and can only end on a dead peer.
static BOOL VchanSendOversizeLocked(IN struct libvchan *vchan, IN const VCHAN_IOV *iov, IN int iovCount,
    IN size_t total, IN const WCHAR *what)
{
    VCHAN_SEND_RESULT result = VCHAN_SEND_OK;
    BOOL started = FALSE;
    int i;

    LogInfo("VCHANCHUNK %s: %I64u bytes exceed the %u byte ring - streaming in %u byte pieces",
        what, (ULONG64)total, VCHAN_WRITE_RING_SIZE, VCHAN_SEND_CHUNK_SIZE);

    for (i = 0; i < iovCount; i++)
    {
        const BYTE *data = (const BYTE *)iov[i].Data;
        size_t offset = 0;

        while (offset < iov[i].Size)
        {
            size_t chunk = iov[i].Size - offset;
            if (chunk > (size_t)VCHAN_SEND_CHUNK_SIZE)
                chunk = (size_t)VCHAN_SEND_CHUNK_SIZE;

            if (!VchanReserveLocked(vchan, chunk, what, !started, &result))
            {
                VchanGiveUpLocked(vchan, result, what, chunk);
                return FALSE;
            }

            g_PerfSendCount++;
            if (!VchanSendBuffer(vchan, data + offset, chunk, what))
            {
                // Space was reserved and cannot have been taken away, so this is a
                // libvchan-level failure: the peer is gone.
                VchanGiveUpLocked(vchan, VCHAN_SEND_DEAD, what, chunk);
                return FALSE;
            }

            started = TRUE;
            offset += chunk;
        }
    }

    return TRUE;
}

static BOOL VchanSendVectoredLocked(IN struct libvchan *vchan, IN const VCHAN_IOV *iov, IN int iovCount,
    IN const WCHAR *what)
{
    VCHAN_SEND_RESULT result = VCHAN_SEND_OK;
    size_t total = 0;
    int i;

    for (i = 0; i < iovCount; i++)
        total += iov[i].Size;

    if (total == 0)
        return TRUE;

    if (g_VchanSendWedged)
    {
        // Fail fast instead of spending another deadline per message: this connection has
        // already been declared gone. Not all senders are gated on g_VchanClientConnected
        // (MSG_WINDOW_DUMP is not), so without this a wedged agent would still burn 10 s
        // per dump.
        g_VchanLastSendResult = g_VchanWedgeResult;
        return FALSE;
    }

    if (g_VchanSendDegraded)
    {
        // A previous send timed out on a daemon that is still connected. Do not spend
        // another full deadline per message while it stays that way - but re-check the
        // ring cheaply every time, because the recovery signal IS the ring draining, and
        // nothing else will tell us the human finally dismissed that dialog. The oversize
        // path streams in pieces, so one chunk of room is enough to declare recovery.
        size_t need = total > (size_t)VCHAN_WRITE_RING_SIZE ? (size_t)VCHAN_SEND_CHUNK_SIZE : total;
        int space = VchanGetWriteBufferSize(vchan);

        if (space >= 0 && (size_t)space >= need)
        {
            g_VchanSendDegraded = FALSE;
            LogInfo("VCHANSLOW recovered: gui-daemon is draining again (ring free=%d) - "
                "resuming normal sends", space);
        }
        else
        {
            g_VchanLastSendResult = VCHAN_SEND_UNRESPONSIVE;
            return FALSE;
        }
    }

    // [FI_LEGACY_SEND] the pre-fix behaviour on demand: no reservation, no deadline,
    // straight into the vendored unbounded spin. The A/B this rank has to be proven with.
    if (FiShouldLegacySend())
    {
        for (i = 0; i < iovCount; i++)
        {
            g_PerfSendCount++;
            if (!VchanSendBuffer(vchan, iov[i].Data, iov[i].Size, what))
                return FALSE;
        }
        return TRUE;
    }

    if (total > (size_t)VCHAN_WRITE_RING_SIZE)
        return VchanSendOversizeLocked(vchan, iov, iovCount, total, what);

    // The whole message is reserved here, before a single byte of it is written. This is
    // the ONLY point at which a send can give up, and at that point the daemon's stream is
    // exactly at a message boundary - which is what makes a partial header/body write
    // impossible by construction rather than by care.
    if (!VchanReserveLocked(vchan, total, what, TRUE, &result))
    {
        VchanGiveUpLocked(vchan, result, what, total);
        return FALSE;
    }

    for (i = 0; i < iovCount; i++)
    {
        g_PerfSendCount++;
        if (!VchanSendBuffer(vchan, iov[i].Data, iov[i].Size, what))
        {
            VchanGiveUpLocked(vchan, VCHAN_SEND_DEAD, what, iov[i].Size);
            return FALSE;
        }
    }

    return TRUE;
}

BOOL VchanSendVectored(IN struct libvchan *vchan, IN const VCHAN_IOV *iov, IN int iovCount, IN const WCHAR *what)
{
    // The reservation is only sound while nothing else writes between it and the writes it
    // covers. Every sender reaches this inside the g_VchanCriticalSection hold that also
    // orders the messages themselves (send.c); this catches a future caller that forgets.
    assert(g_VchanCriticalSection.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId());

    g_VchanLastSendResult = VCHAN_SEND_OK;

    if (!g_PerfEnabled)
        return VchanSendVectoredLocked(vchan, iov, iovCount, what);

    LONGLONG t0 = PerfNow();
    // The wait for ring space is counted as send time, exactly as it was when the vendored
    // spin did the waiting inside VchanSendBuffer - so `snd` stays comparable across builds.
    BOOL status = VchanSendVectoredLocked(vchan, iov, iovCount, what);
    g_PerfSendTicks += PerfNow() - t0;
    return status;
}

BOOL VchanSendTimed(IN struct libvchan *vchan, IN const void *data, IN size_t size, IN const WCHAR *what)
{
    VCHAN_IOV iov;

    iov.Data = data;
    iov.Size = size;
    return VchanSendVectored(vchan, &iov, 1, what);
}

BOOL VchanSendMessage(IN const struct msg_hdr *header, IN int headerSize, IN const void *data, IN int dataSize, IN const WCHAR *what)
{
    VCHAN_IOV iov[2];

    LogVerbose("msg 0x%x (%s) for window 0x%x, size %d", header->type, what, header->window, header->untrusted_len);

    // One reservation for header AND body. Previously these were two independent sends
    // whose results were tested with "status < 0" - a BOOL is never negative, so a failed
    // header write was read as success and the body went out behind it, desynchronising
    // the daemon by exactly one header. Now neither can happen: either both are written or
    // nothing is.
    iov[0].Data = header;
    iov[0].Size = (size_t)headerSize;
    iov[1].Data = data;
    iov[1].Size = (size_t)dataSize;

    return VchanSendVectored(g_Vchan, iov, (int)RTL_NUMBER_OF(iov), what);
}

VCHAN_SEND_RESULT VchanLastSendResult(void)
{
    return g_VchanLastSendResult;
}

ULONG VchanLastSendError(void)
{
    if (g_VchanLastSendResult == VCHAN_SEND_DEAD)
        return ERROR_VC_DISCONNECTED;
    if (g_VchanLastSendResult == VCHAN_SEND_UNRESPONSIVE)
        return ERROR_TIMEOUT;

    // Asked only after a failure, so "no reason recorded" must not map to success - it
    // keeps the code the callers returned before this existed.
    return ERROR_UNIDENTIFIED_ERROR;
}

BOOL VchanSendWedged(void)
{
    return g_VchanSendWedged;
}

VCHAN_SEND_RESULT VchanWedgeResult(void)
{
    return g_VchanWedgeResult;
}

BOOL VchanSendDegraded(void)
{
    return g_VchanSendDegraded;
}

BOOL VchanInit(IN int domain, IN int port)
{
    // We give a 5 minute timeout here because xeniface can take some time
    // to load the first time after reboot after pvdrivers installation.

    // The old TODO here ("vchan send hangs if the data size is higher than the vchan
    // buffer size", so the ring size limits the resolution) is answered on the send side:
    // a message larger than the ring is streamed in pieces by VchanSendOversizeLocked,
    // because a MSG_WINDOW_DUMP at the protocol's maximum geometry is 384 KiB of grant
    // refs - six times this ring - and could never be reserved whole.

    g_Vchan = VchanInitServer(domain, port, VCHAN_WRITE_RING_SIZE, 5 * 60 * 1000);
    if (!g_Vchan)
    {
        LogError("VchanInitServer failed");
        return FALSE;
    }

    return TRUE;
}
