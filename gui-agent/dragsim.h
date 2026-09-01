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
#include <windows.h>

/*
 * SYNTHETIC GUEST-NATIVE DRAG.
 *
 * READ THIS BEFORE TRUSTING ANY NUMBER IT PRODUCES. The owner's standing verdict
 * (2026-09-01): "previous times, we were not able to capture the problem properly unless I
 * dragged the window manually. Instrumented drag that does not depend on pointer fails to do
 * it. You may try, but don't rely on it." That is recorded here rather than in a commit
 * message because it is the operating rule for this file: a PASS from DragSim is NOT evidence
 * that the wobble is gone, and only a hand drag decides that.
 *
 * WHAT IT IS FOR, then. Two things it can honestly do:
 *   1. PROVE THE INSTRUMENT WRITES DATA before a human is asked to drag. Three hand drags on
 *      2026-09-01 produced zero measurements between them - a poisoned build, a log level that
 *      filtered the lines, and a sampler whose "armed: true" was not evidence of sampling. A
 *      synthetic drag exercises the same QGAPROTO lines end to end, so "the trace is live" is
 *      checked from here instead of being spent on someone's hand.
 *   2. REPRODUCE A DEFECT IT CAN REACH. It differs from every previous scripted drag in one
 *      way that matters: earlier ones injected mouse input in-guest, which never sends
 *      MSG_BUTTON and therefore never arms the drag latch, so they "cannot show this defect at
 *      all" (docs/PLAN-drag-quality.md). This one drives the real ProcessButtonEvent /
 *      ProcessMotionEvent, so the latch, the announce ring and all three translation branches
 *      really run, and announces really go to the real gui-daemon whose real MSG_CONFIGURE
 *      replies really come back. If it reproduces something, that something is real.
 *
 * WHAT IS MODELLED, and why that is the limitation: exactly one quantity, D - dom0's applied
 * window origin - reconstructed from an INDEPENDENT announce history delayed by DragSimLagMs.
 * A real pointer carries timing, ordering and dom0-side effects this cannot fabricate, which is
 * the likeliest reason hand drags see what scripts do not.
 */

/* Every position announce that actually reached the daemon, recorded unconditionally (not
 * gated on the drag latch, unlike DragAnnounceRecord - driving the stimulus from the agent's
 * own estimator would cancel the loop by construction and blind the instrument). */
void DragSimNoteAnnounce(IN int x, IN int y);

/* Spawn the trigger poller. No-op unless the DragSim registry value is set. */
void DragSimStart(void);
