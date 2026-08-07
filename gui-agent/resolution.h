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

// Rolling quiet-period before a dom0-requested resolution is applied. 500 ms fired
// mid-drag on every hesitation, so a window drag produced a stream of mode changes -
// each one display-topology churn implicated in the guest livelock (FINDINGS
// 2026-08-05). 1200 ms keeps single deliberate resizes responsive while riding out
// drag hesitations.
#define RESOLUTION_CHANGE_TIMEOUT 1200

// 'source' tags where the request originated (dom0, xconf, lastapplied,
// seamless-force) for the RESREQ/RESSNAP/RESAPPLIED instrumentation log lines.
// src=dom0 requests are exact-follow: the requested size is applied verbatim or
// not at all (never snapped), obtaining the mode from the Qubes IDD if needed.
// Attach the Qubes IddCx monitor and make it the SOLE active display, persisting the
// topology. Must run in the INTERACTIVE session (session 0 gets ERROR_ACCESS_DENIED).
// No-op returning ERROR_SUCCESS when no IDD is present (Basic Display Adapter guests).
// Returns ERROR_INVALID_STATE if readback shows the IDD is not solo-primary.
ULONG EnsureQubesIddSolo(void);

DWORD RequestResolutionChange(IN LONG width, IN LONG height, IN const WCHAR* source);
ULONG SetVideoMode(IN ULONG width, IN ULONG height, IN const WCHAR* source);
// TRUE while an exact-obtain (replug+apply) is in flight on the resolution thread;
// the recovery path must not tell the daemon about transitional geometries then.
BOOL ResolutionExactObtainInFlight(void);
// FALSE while a non-target geometry must not be announced to the daemon
// (exact-obtain in flight OR settle window open).
BOOL ResolutionShouldAnnounceGeometry(IN ULONG width, IN ULONG height);
// Recovery path records desktop-transit sizes here; they become echo suspects.
void ResolutionNoteTransitSize(IN ULONG width, IN ULONG height);
// M6 one-shot: publish the computed IDD mode set (target + maximize/tile-half
// from the dom0 work-area feed + fallback) and reload the driver once, so the
// habitual sizes switch with replug=0 from first use. Non-fatal on failure.
void ResolutionPublishBootModeSet(void);
// M6: dom0 work-area feed changed, or seamless mode changed (the set carries the
// host size only while seamless is active) - rewrite the registry set if it
// differs from the last written one. Registry only, NO reload (no blink);
// called from WorkAreaSetDom0 and from SetSeamlessMode after it commits the mode.
void ResolutionRecomputeIddModeSet(void);
void InitVideoModes();
// M0BLINK instrumentation: GetTickCount64 stamp of the current novel-size
// obtain's start (set before the registry write in SetVideoModeExact).
// The A6ACKREPAINT site (vchan-handlers.c) InterlockedExchange64s it to 0 and
// logs "M0BLINK repaint-sent ... sinceobtain=" when nonzero.
extern volatile LONG64 g_M0BlinkObtainStart;

// M0BLINK: same stamp, consumed by the FIRST (optimistic, post-dump) repaint in
// main.c instead of by the ack-gated one. Two variables, not one, because both
// repaints happen and each must get exactly one line per obtain - the ack-gated
// A6ACKREPAINT is NOT the moment the user sees pixels, it is one vchan round
// trip later, and reporting only it overstates the blink.
extern volatile LONG64 g_M0BlinkFirstPaintStart;

// M0BLINK: non-consuming read of the obtain-start stamp, for the intermediate
// phase markers on the applied->repaint tail (capture.c). 0 = no obtain in
// flight, marker stays silent.
LONG64 ResolutionM0BlinkObtainStart(void);
