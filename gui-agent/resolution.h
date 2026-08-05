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
// M6: dom0 work-area feed changed - rewrite the registry set if it differs from
// the last written one. Registry only, NO reload (no blink); called from
// WorkAreaSetDom0.
void ResolutionRecomputeIddModeSet(void);
void InitVideoModes();
