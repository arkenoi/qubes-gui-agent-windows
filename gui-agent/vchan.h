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

#pragma once
#include <windef.h>

#include <vchan-common.h>

#pragma warning(push)
#pragma warning(disable:4200) // nonstandard extension used: zero-sized array in struct/union
#include <qubes-gui-protocol.h>
#pragma warning(pop)

extern CRITICAL_SECTION g_VchanCriticalSection;
extern struct libvchan *g_Vchan;

// Size the write ring is created with. Also the largest reservation that can ever be
// satisfied, so it decides which messages have to be streamed (see VCHAN_SEND_CHUNK_SIZE
// in vchan.c). 64k allows for 4 x 1440p screens' worth of MFN dump.
#define VCHAN_WRITE_RING_SIZE 65536

// How long ONE message may wait for the daemon to make room in the write ring before the
// connection is declared unusable.
//
// The ring is drained by exactly one reader, dom0's gui-daemon, and a daemon that stops
// reading used to wedge this agent forever: the vendored VchanSendBuffer spins
// "while (VchanGetWriteBufferSize(vchan) < size) Sleep(1);" with no deadline and no
// liveness check, while the caller holds g_VchanCriticalSection. That is the whole H2
// failure class - the daemon put up a modal dialog, stopped draining for ~6.5 s, the
// agent's pump blocked in a send and the capture thread died behind it (FINDINGS
// 2026-08-11).
//
// 10 s is deliberately well ABOVE that 6.5 s: a daemon that is merely slow must be
// absorbed as backpressure, not turned into a disconnect. It is equally deliberately
// finite, so a daemon that never resumes degrades loudly instead of hanging the guest's
// display forever.
#define VCHAN_SEND_DEADLINE_MS 10000

// Hard cap for a message that is ALREADY partly written (only reachable by the oversize
// streaming path). Giving up here desyncs the daemon's stream, so it is deliberately much
// longer than the ordinary deadline - but it is finite, because an unbounded wait under
// g_VchanCriticalSection is precisely the wedge this layer removes. Past it the connection
// is abandoned and the process respawns, which is recoverable; a frozen display is not.
#define VCHAN_SEND_COMMITTED_DEADLINE_MS (3 * VCHAN_SEND_DEADLINE_MS)

// Why a write attempt failed. The two failure modes must not be collapsed: DEAD means the
// peer is gone, so a fatal caller (the handshake) can give up immediately and nothing that
// follows can matter; UNRESPONSIVE means the peer is still connected but has not drained
// the ring within VCHAN_SEND_DEADLINE_MS, which the never-exit paths treat as "this frame
// is lost", not as a reason to tear anything down.
typedef enum _VCHAN_SEND_RESULT
{
    VCHAN_SEND_OK = 0,
    VCHAN_SEND_DEAD,            // libvchan reports the channel closed, or libvchan_send failed
    VCHAN_SEND_UNRESPONSIVE,    // channel open, daemon not draining within the deadline
} VCHAN_SEND_RESULT;

// One piece of a message that must reach the ring as an indivisible unit.
typedef struct _VCHAN_IOV
{
    const void *Data;
    size_t Size;
} VCHAN_IOV;

BOOL VchanInit(IN int domain, IN int port);
BOOL VchanSendMessage(IN const struct msg_hdr* header, IN int headerSize, IN const void* data, IN int dataSize, IN const WCHAR* what);

// Send all pieces as ONE protocol message: the space for the whole thing is reserved
// before the first byte is written, so the send can only ever fail BETWEEN messages and
// the daemon's single in-order stream can never be left half-fed. Callers with a message
// built from several buffers (MSG_WINDOW_DUMP = header + dump header + grant slab) MUST
// use this rather than several VchanSendTimed calls.
// The caller must hold g_VchanCriticalSection - the reservation is only meaningful while
// no other thread can write between it and the writes it covers.
BOOL VchanSendVectored(IN struct libvchan* vchan, IN const VCHAN_IOV* iov, IN int iovCount, IN const WCHAR* what);

// Single-buffer send. Same semantics as VchanSendVectored with one piece; also accounts
// the time spent writing to the vchan (see perf.h).
BOOL VchanSendTimed(IN struct libvchan* vchan, IN const void* data, IN size_t size, IN const WCHAR* what);

// Why the last send on THIS thread failed (per-thread, like the perf counters: the capture
// thread and the main loop send concurrently and must not overwrite each other's reason).
VCHAN_SEND_RESULT VchanLastSendResult(void);

// The same thing as a Win32 status, for the ULONG-returning send.c wrappers:
// DEAD -> ERROR_VC_DISCONNECTED, UNRESPONSIVE -> ERROR_TIMEOUT.
ULONG VchanLastSendError(void);

// TRUE once a send has given up on this connection FOR GOOD. Terminal for the process: the
// agent has stopped writing and cleared g_VchanClientConnected, but the vchan itself may
// still be open, so the main loop must NOT read that cleared flag as "a new client may
// connect" and re-run the handshake (see WatchForEvents, vchan receive).
// Only a DEAD peer (or an abandoned partly-written message) latches this. A daemon that is
// merely not draining is NOT terminal - see VchanSendDegraded.
BOOL VchanSendWedged(void);

// Why it gave up (VCHAN_SEND_OK if it has not). Unlike VchanLastSendResult this is
// process-wide rather than per-thread, because the wedge belongs to the connection and the
// thread that teardown runs on is not the thread that hit it.
VCHAN_SEND_RESULT VchanWedgeResult(void);

// TRUE while the daemon is CONNECTED but has stopped draining the ring (the modal-dialog
// case): sends fail fast, the connection is kept, and this clears by itself the moment the
// ring has room again. Distinct from the wedge on purpose - a degraded daemon is still
// RUNNING and still maps our grants, so exit-time revoke paths must treat it as alive.
BOOL VchanSendDegraded(void);

#define VCHAN_SEND_MSG(header, body, what) (\
    header.untrusted_len = sizeof(body), \
    VchanSendMessage(&(header), sizeof(header), &(body), sizeof(body), what) \
    )

#define VCHAN_SEND(x, what) VchanSendTimed(g_Vchan, &(x), sizeof(x), what)
