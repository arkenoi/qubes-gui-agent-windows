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

/*
 * Deliberate fault injection for the stability overhaul (rank 1 of
 * docs/PLAN-stability-overhaul.md).
 *
 * WHY THIS EXISTS. Every stability fix below rank 1 is claimed against a failure that
 * the natural reproducer does not reliably produce: four post-reboot storm runs yielded
 * ZERO negative geometry, and the capture-thread death and the vchan wedge are both
 * timing-dependent. CLAUDE.md's validation rule is that "a check counts as evidence only
 * once it has been seen to FAIL on a build with the defect deliberately re-introduced".
 * Without a toggle that re-introduces each defect on demand, every PASS below is
 * unproven. This module is that toggle.
 *
 * SECURITY. This runs inside a guest that the security model already treats as hostile,
 * so nothing here may increase what dom0 trusts, and nothing here does:
 *   - every injection corrupts AGENT-LOCAL state only (a counter, a timer, one thread's
 *     control flow) or emits a message a guest in full control of its own agent binary
 *     could emit anyway. A compromised guest does not need our help to send a malformed
 *     MSG_CREATE - that is precisely why gui-daemon verifies it, and FI_NEG_CREATE exists
 *     to demonstrate that the daemon's check still fires and that the agent-side
 *     sanitizer (rank 3) stops the message before it gets there;
 *   - no dom0-side check is weakened, bypassed, or informed of anything;
 *   - no isolation-relevant state (grants, the vchan peer, the protocol handshake) is
 *     forged: the worst outcome of any flag is that THIS qube's own display degrades,
 *     which is the exact failure being studied;
 *   - the switches live in HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent and
 *     in the service's environment, both of which already require guest administrator -
 *     a user who has them can replace the agent binary outright.
 * Above all: QGA_FAULT_INJECTION is OFF by default, so a RELEASE binary contains none of
 * this code and no runtime setting can turn it on.
 *
 * BUILD. Test build:  msbuild ... -p:QgaFaultInjection=1
 * The property feeds QGA_FAULT_INJECTION into the preprocessor (see gui-agent.vcxproj).
 *
 * ARMING. No fault fires until FaultArmDelaySec (default 60 s) after FiInit(). The
 * failures being reproduced are all failures of a CONNECTED, running agent; a fault that
 * lands during the daemon handshake only reproduces "the agent never came up".
 *
 * FLAGS (registry DWORD under the gui-agent module key; environment variable wins;
 * every one defaults to 0 = off, and every one is logged at startup and on each trigger):
 *
 *   FaultArmDelaySec   QUBES_GUI_FAULT_DELAY          seconds; global arming delay (default 60)
 *   FaultNegCreate     QUBES_GUI_FAULT_NEG_CREATE     N one-shots: invert the rect of the
 *                                                     next N MSG_CREATEs  [FI_NEG_CREATE]
 *   FaultNegCreateHwnd QUBES_GUI_FAULT_NEG_CREATE_HWND if non-zero, only the CREATE for
 *                                                     this hwnd (as the protocol carries
 *                                                     it: low 32 bits) is inverted
 *   FaultRawCreate     QUBES_GUI_FAULT_RAW_CREATE     mode (not a shot): skip the agent-side
 *                                                     geometry sanitizer for MSG_CREATE, so an
 *                                                     injected bad rect really reaches dom0
 *                                                     and the daemon's dialog can be seen to
 *                                                     fire                  [FI_RAW_CREATE]
 *   FaultRingStallSec  QUBES_GUI_FAULT_RING_STALL     seconds: the send wrapper sees the
 *                                                     ring as permanently full [FI_RING_STALL]
 *   FaultPumpStallSec  QUBES_GUI_FAULT_PUMP_STALL     seconds, ONE-SHOT: one WatchForEvents
 *                                                     iteration sleeps  [FI_PUMP_STALL]
 *   FaultCaptureExit   QUBES_GUI_FAULT_CAPTURE_EXIT   N one-shots (use 1): the capture
 *                                                     thread exits WITHOUT signalling its
 *                                                     error event      [FI_CAPTURE_EXIT]
 *   FaultDupCreate     QUBES_GUI_FAULT_DUP_CREATE     N one-shots: re-emit MSG_CREATE for
 *                                                     an already-created hwnd [FI_DUP_CREATE]
 *   FaultLegacySend    QUBES_GUI_FAULT_LEGACY_SEND    N one-shots: route the send through
 *                                                     the unbounded vendored path, for the
 *                                                     A/B against rank 2  [FI_LEGACY_SEND]
 *
 * "One-shot" means the counter is consumed atomically, so N=1 fires exactly once across
 * all threads. A time-window flag (RING_STALL) is active for its whole window and logs
 * its first trigger, every 1000th, and a total when the window closes - logging every
 * trigger of a per-send fault would itself be a bigger stall than the fault.
 *
 * COST WHEN COMPILED IN BUT DISARMED: one load of a static and a predictable branch per
 * query, no atomics, no allocation, no clock read. COST WHEN COMPILED OUT: nothing at all,
 * the stubs below inline to a constant.
 */

#pragma once
#include <windows.h>

// Master switch. Must stay 0 here: release artifacts are built without the property, and
// a binary that can be made to break itself on purpose is not a shipping binary.
#ifndef QGA_FAULT_INJECTION
#define QGA_FAULT_INJECTION 0
#endif

// Greppable in the built .exe, so a test run can prove WHICH build it is holding without
// waiting for a log line (CLAUDE.md: verify the artefact under test is actually installed).
extern const char g_FaultInjectionMarker[];

// [FI_GATE_OFF] bits for FiGateOff(), set as the DWORD FaultGateOff (env
// QUBES_GUI_FAULT_GATE_OFF). Each bit BYPASSES one ShouldAcceptWindow safeguard so the
// corresponding acceptance check can be seen to FAIL - H5's requirement that a check counts
// as evidence only once it has gone red on a build with the defect re-introduced.
//
// WHY A RUNTIME BITMASK RATHER THAN ONE DIAG BRANCH PER CLAUSE. The first such proof (SG2,
// 2026-08-31) removed the Mode-2 gate on a branch and built a second binary. That works, but
// leaves a sceptic room to ask whether the red came from the gate removal or from some other
// difference between two builds, and it costs a CI round trip per clause with ~19 clauses
// left. With a bitmask, BOTH sides of every proof come from ONE artifact and differ only by a
// registry value, which is a strictly stronger pairing and needs no further builds.
//
// The bits are deliberately NOT contiguous with main.c's shipped DiagWindowFilterOff: that
// one is a field-diagnosis knob present in release binaries, these are deliberate defects
// that must not be. Keep them here so they compile out.
#define FI_GATE_MODE1        0x00000001u  // boot/shutdown/logon-phase fullscreen deny (Mode 1)
#define FI_GATE_MODE2        0x00000002u  // borderless-fullscreen feature gate (Mode 2)
#define FI_GATE_START        0x00000004u  // "Start surface not presented in seamless mode"
#define FI_GATE_SHELLOVERLAY 0x00000008u  // click-through uncapturable shell overlay reject
#define FI_GATE_NOCARD       0x00000010u  // the genuine-open ("shell surface with no card") reject
//
// FI_DROP_* invert the direction. Every bit above REMOVES a safeguard, which falsifies a check
// asserting a window is DENIED. Checks that assert a window IS presented - the negative control,
// a maximized app, a toast surviving the chrome filter - cannot be falsified that way, so these
// two ADD a reject at the very end of the predicate instead. The regression they reproduce is
// real and this project has come close to shipping it: the 2A-chrome filter, aimed at Office
// shadow strips, would have silently killed every Windows notification.
#define FI_DROP_CAPTIONED    0x00000020u  // drop captioned windows the release build maps
#define FI_DROP_SHELLSURFACE 0x00000040u  // drop toast/menu shell surfaces the release build maps
// Not an accept-predicate bit at all: menus are SYNTHESIZED onto their owner's surface rather
// than mapped as windows, so no reject bit can falsify a check asserting a menu reaches the
// user. This suppresses the synth PAINT, leaving the owner's pixels unchanged.
#define FI_NOSYNTHPAINT      0x00000080u  // skip the synth paint chokepoint (PwPatchSynthChildClipped)
// Suppresses the screen-window CONFIGURE after a resolution change, so dom0 keeps the OLD
// geometry while the guest has already switched. The mode-followed-* checks read the
// A6CONFIGURE line this emits and take the LAST one, so suppressing it leaves them stale.
#define FI_NOSCREENCONFIG    0x00000100u  // do not tell dom0 the screen resolution changed

#if QGA_FAULT_INJECTION

#ifdef __cplusplus
extern "C" {
#endif

// Read the switches, log the banner. Call once from Init(), next to PerfInit().
void FiInit(void);

// [FI_NEG_CREATE] TRUE once per armed shot: the caller must put an inverted (negative
// width/height) rect on the wire for this window. Consumes the shot, so ask exactly once
// per MSG_CREATE. Rank 3's sanitizer is the code this is aimed at.
BOOL FiShouldNegCreate(IN HWND window);

// [FI_RAW_CREATE] TRUE while the raw-create mode is on: the caller must send MSG_CREATE
// WITHOUT running it through the geometry sanitizer. Pair with FI_NEG_CREATE to reproduce
// the pre-fix behaviour end to end, i.e. to make dom0's own check fire.
BOOL FiRawCreate(void);

// [FI_RING_STALL] TRUE while the stall window is open: the agent's vchan wrapper must
// behave as if the ring had no room, without consulting the real ring. Rank 2's deadline
// is the code this is aimed at.
BOOL FiRingStallActive(void);

// [FI_PUMP_STALL] Milliseconds this WatchForEvents iteration must sleep, 0 for none.
// One-shot: reproduces H2, where the pump stops draining the vchan for seconds.
DWORD FiPumpStallMs(void);

// [FI_CAPTURE_EXIT] TRUE once: the capture thread must return WITHOUT setting its error
// event, reproducing the silent death the main loop currently never learns about.
BOOL FiShouldCaptureExit(void);

// [FI_DUP_CREATE] TRUE once per armed shot: re-emit MSG_CREATE for a window that already
// has one, reproducing the 13 duplicate CREATEs seen during a Start-menu flip storm.
BOOL FiShouldDupCreate(IN HWND window);

// [FI_LEGACY_SEND] TRUE once per armed shot: route this one send through the unbounded
// vendored VchanSendBuffer instead of the bounded wrapper, so rank 2 can be A/B'd against
// the behaviour it replaces.
BOOL FiShouldLegacySend(void);

// [FI_PRINTWINDOW_FAIL] TRUE once per armed shot: the per-window capture engine must treat
// this PrintWindow as FAILED. Window destruction cannot reach the WCDEAD latch (tracking
// detaches the channel before the ~250 ms sweep retries 5 times - measured 2026-08-27), so
// this is the only way to make a LIVE channel fail 5 consecutive captures and prove the
// latch fires. Arm >= 6 shots: the failing channel re-marks itself dirty and retries on the
// engine loop's 2 ms cadence, so one channel consumes 5 shots before the sweep offers the
// fault to another.
BOOL FiPrintWindowFail(void);

// [FI_GATE_OFF] TRUE while this safeguard clause is bypassed for the run. Not a shot and not
// subject to the arming delay: a gate bypass is a mode, and it cannot disturb the handshake
// because it changes only which windows are offered. Logs once per bit, so the run's own log
// proves the bypass was in force.
BOOL FiGateOff(IN DWORD gateBit);

#ifdef __cplusplus
} // extern "C"
#endif

#else

// No-op stubs so the call sites need no #ifdef. UNREFERENCED_PARAMETER keeps /W4 quiet.
static __forceinline void  FiInit(void) { }
static __forceinline BOOL  FiShouldNegCreate(IN HWND window) { UNREFERENCED_PARAMETER(window); return FALSE; }
static __forceinline BOOL  FiRawCreate(void) { return FALSE; }
static __forceinline BOOL  FiRingStallActive(void) { return FALSE; }
static __forceinline DWORD FiPumpStallMs(void) { return 0; }
static __forceinline BOOL  FiShouldCaptureExit(void) { return FALSE; }
static __forceinline BOOL  FiShouldDupCreate(IN HWND window) { UNREFERENCED_PARAMETER(window); return FALSE; }
static __forceinline BOOL  FiShouldLegacySend(void) { return FALSE; }
static __forceinline BOOL  FiPrintWindowFail(void) { return FALSE; }
static __forceinline BOOL  FiGateOff(IN DWORD gateBit) { UNREFERENCED_PARAMETER(gateBit); return FALSE; }

#endif
