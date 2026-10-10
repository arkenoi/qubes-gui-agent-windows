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

#include "faultinject.h"

// Same string in both builds so the test never has to guess which one it installed;
// only the suffix differs. Not COMDAT data, so /OPT:REF leaves it in the image.
#if QGA_FAULT_INJECTION
const char g_FaultInjectionMarker[] = "QGA-FAULT-INJECTION:on";
#else
const char g_FaultInjectionMarker[] = "QGA-FAULT-INJECTION:off";
#endif

#if QGA_FAULT_INJECTION

#include <stdlib.h>

#include <log.h>
#include <config.h>

// Registry values (module key: HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent)
// and the environment overrides that win over them, exactly as perf.c does it.
#define REG_CONFIG_FAULT_DELAY_VALUE        L"FaultArmDelaySec"
#define REG_CONFIG_FAULT_NEG_CREATE_VALUE   L"FaultNegCreate"
#define REG_CONFIG_FAULT_NEG_HWND_VALUE     L"FaultNegCreateHwnd"
#define REG_CONFIG_FAULT_RAW_CREATE_VALUE   L"FaultRawCreate"
#define REG_CONFIG_FAULT_RING_STALL_VALUE   L"FaultRingStallSec"
#define REG_CONFIG_FAULT_PUMP_STALL_VALUE   L"FaultPumpStallSec"
#define REG_CONFIG_FAULT_CAPTURE_EXIT_VALUE L"FaultCaptureExit"
#define REG_CONFIG_FAULT_DUP_CREATE_VALUE   L"FaultDupCreate"
#define REG_CONFIG_FAULT_LEGACY_SEND_VALUE  L"FaultLegacySend"
#define REG_CONFIG_FAULT_PW_FAIL_VALUE      L"FaultPrintWindowFail"
#define REG_CONFIG_FAULT_SLAB_BIND_VALUE    L"FaultSlabDoubleBind"
#define REG_CONFIG_FAULT_GATE_OFF_VALUE     L"FaultGateOff"
#define REG_CONFIG_FAULT_DAMAGE_DELAY_VALUE L"FaultDamageDelayMs"
#define REG_CONFIG_FAULT_PUMP_LOSE_VALUE   L"FaultPumpStallLose"
#define REG_CONFIG_FAULT_MON_STALE_VALUE   L"FaultMonStale"

#define FAULT_DELAY_ENV_VALUE        L"QUBES_GUI_FAULT_DELAY"
#define FAULT_NEG_CREATE_ENV_VALUE   L"QUBES_GUI_FAULT_NEG_CREATE"
#define FAULT_NEG_HWND_ENV_VALUE     L"QUBES_GUI_FAULT_NEG_CREATE_HWND"
#define FAULT_RAW_CREATE_ENV_VALUE   L"QUBES_GUI_FAULT_RAW_CREATE"
#define FAULT_RING_STALL_ENV_VALUE   L"QUBES_GUI_FAULT_RING_STALL"
#define FAULT_PUMP_STALL_ENV_VALUE   L"QUBES_GUI_FAULT_PUMP_STALL"
#define FAULT_CAPTURE_EXIT_ENV_VALUE L"QUBES_GUI_FAULT_CAPTURE_EXIT"
#define FAULT_DUP_CREATE_ENV_VALUE   L"QUBES_GUI_FAULT_DUP_CREATE"
#define FAULT_LEGACY_SEND_ENV_VALUE  L"QUBES_GUI_FAULT_LEGACY_SEND"
#define FAULT_PW_FAIL_ENV_VALUE      L"QUBES_GUI_FAULT_PRINTWINDOW_FAIL"
#define FAULT_SLAB_BIND_ENV_VALUE    L"QUBES_GUI_FAULT_SLAB_DOUBLE_BIND"
#define FAULT_GATE_OFF_ENV_VALUE     L"QUBES_GUI_FAULT_GATE_OFF"
#define FAULT_DAMAGE_DELAY_ENV_VALUE L"QUBES_GUI_FAULT_DAMAGE_DELAY"
#define FAULT_PUMP_LOSE_ENV_VALUE    L"QUBES_GUI_FAULT_PUMP_LOSE"
#define FAULT_MON_STALE_ENV_VALUE    L"QUBES_GUI_FAULT_MON_STALE"

// Seconds between FiInit() and the first fault that may fire. See faultinject.h: every
// failure being reproduced is a failure of a CONNECTED agent, so a fault landing during
// the daemon handshake would reproduce the wrong thing.
#define FAULT_DEFAULT_ARM_DELAY_SEC 60

// Tick (GetTickCount64) before which nothing fires. Written once by FiInit, read-only
// afterwards, so the worker threads need no synchronization to see it.
static ULONGLONG g_FiArmAt = 0;

// Remaining one-shots. LONG rather than DWORD because they are consumed with
// InterlockedDecrement: the main loop, the capture thread and the resolution thread can
// all query concurrently, and two threads must never both take the last shot.
static volatile LONG g_FiNegCreate   = 0;
static volatile LONG g_FiDupCreate   = 0;
static volatile LONG g_FiLegacySend  = 0;
static volatile LONG g_FiCaptureExit = 0;
static volatile LONG g_FiPumpStall   = 0;
static volatile LONG g_FiPrintWindowFail = 0;
static volatile LONG g_FiSlabDoubleBind  = 0;
// [FI_MON_STALE] Shots of "the monitor handle went stale": GetMonitorSettings reports
// ERROR_INVALID_MONITOR_HANDLE. 1 shot proves the re-acquire measures the window anyway;
// 2 shots make BOTH asks fail, which is the path that must produce exactly one error line.
static volatile LONG g_FiMonStale        = 0;

// [FI_GATE_OFF] Bitmask of ShouldAcceptWindow safeguard clauses to BYPASS.
//
// Not a shot and not delayed. A gate bypass is a MODE, and unlike the timing faults it is
// harmless during the handshake: it changes only which windows the agent offers, never the
// protocol. Written once by FiInit, read-only afterwards.
//
// WHY THESE ARE HERE AND NOT NEXT TO g_DiagWindowFilterOff. main.c already ships a two-bit
// DiagWindowFilterOff for field diagnosis, and the obvious move was to widen it. That would
// have put every new safeguard bypass into the RELEASE binary. These bits exist only to
// re-introduce defects for H5 proofs, so they belong where the rest of the deliberate defects
// live - behind QGA_FAULT_INJECTION, compiled out of anything shipped.
static DWORD g_FiGateOff = 0;

// Which bits have already announced themselves. ShouldAcceptWindow runs for every window on
// every pass, so logging per call would bury the log; once per bit is still enough to prove
// the bypass was in force during THIS run, which is what a proof has to show.
static volatile LONG g_FiGateLogged = 0;

// Only the CREATE for this window id is inverted; 0 means "whichever comes next".
// Compared as ULONG_PTR against the low half of the HWND, the same 32-bit id the
// protocol carries. Written once by FiInit.
static DWORD g_FiNegCreateHwnd = 0;
static DWORD g_FiRawCreate     = 0;

// One-shot payload: how long the single stalled pump iteration sleeps. Written once.
static DWORD g_FiPumpStallMs = 0;
// [FI_PUMP_LOSE] with the stall: the broker's frame event is consumed at its end without being set again - an injected
// lost wakeup, so the stall's signal check (rest-zero M8) is seen to fail. Written once.
static DWORD g_FiPumpStallLose = 0;

// [FI_DAMAGE_DELAY] a MODE, not a shot: every damage message waits this long before it is traced and sent. Written once.
static DWORD g_FiDamageDelayMs = 0;
static volatile LONG g_FiDamageDelayLogged = 0;

// FI_RING_STALL is a time WINDOW, not a one-shot, because the H2 wedge is defined by its
// duration: the daemon stopped draining for ~6.5 s and the agent had to survive that.
// g_FiRingStallArmed is the hot-path fast reject and is cleared when the window closes,
// so a run stays free of clock reads once the fault is over.
static volatile LONG g_FiRingStallArmed = 0;
static ULONGLONG     g_FiRingStallUntil = 0;    // written once by FiInit
static volatile LONG g_FiRingStallHits  = 0;

// Registry first, environment second (env wins) - the same precedence PerfInit uses, so
// a fault can be armed for one qrexec-launched run without touching HKLM.
static DWORD FiReadDword(
    IN const WCHAR *moduleName,
    IN const WCHAR *regValue,
    IN const WCHAR *envValue,
    IN DWORD defaultValue)
{
    DWORD value = defaultValue;
    DWORD regData = 0;
    DWORD length;
    WCHAR env[16];

    if (ERROR_SUCCESS == CfgReadDword(moduleName, regValue, &regData, NULL))
        value = regData;

    // GetEnvironmentVariable returns the REQUIRED size when the buffer is too small and
    // leaves the buffer untouched, so a too-long value must not be parsed as a number.
    length = GetEnvironmentVariable(envValue, env, RTL_NUMBER_OF(env));
    if (length > 0 && length < RTL_NUMBER_OF(env))
        value = (DWORD)_wtoi(env);

    return value;
}

// Take one shot if any are left AND the arming delay has passed. The clock is read only
// after the counter test, so a disarmed flag costs a load and a branch on the hot path.
static BOOL FiTakeShot(IN OUT volatile LONG *remaining)
{
    if (*remaining <= 0)
        return FALSE;

    if (GetTickCount64() < g_FiArmAt)
        return FALSE;

    if (InterlockedDecrement(remaining) < 0)
    {
        // Another thread took the last one between the test and the decrement.
        InterlockedIncrement(remaining);
        return FALSE;
    }

    return TRUE;
}

void FiInit(void)
{
    WCHAR moduleName[CFG_MODULE_MAX];
    DWORD delaySec = FAULT_DEFAULT_ARM_DELAY_SEC;
    DWORD ringStallSec = 0;
    DWORD pumpStallSec = 0;

    if (ERROR_SUCCESS != CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
    {
        // Without the module name every CfgReadDword below would silently read the parent
        // key. A fault that armed itself from the wrong key would be worse than no fault
        // at all, so refuse to arm and say so.
        LogWarning("QGAFAULT CfgGetModuleName failed - fault injection stays disarmed");
        return;
    }

    delaySec       = FiReadDword(moduleName, REG_CONFIG_FAULT_DELAY_VALUE,        FAULT_DELAY_ENV_VALUE,        FAULT_DEFAULT_ARM_DELAY_SEC);
    ringStallSec   = FiReadDword(moduleName, REG_CONFIG_FAULT_RING_STALL_VALUE,   FAULT_RING_STALL_ENV_VALUE,   0);
    pumpStallSec   = FiReadDword(moduleName, REG_CONFIG_FAULT_PUMP_STALL_VALUE,   FAULT_PUMP_STALL_ENV_VALUE,   0);
    g_FiNegCreateHwnd = FiReadDword(moduleName, REG_CONFIG_FAULT_NEG_HWND_VALUE,  FAULT_NEG_HWND_ENV_VALUE,     0);
    g_FiRawCreate     = FiReadDword(moduleName, REG_CONFIG_FAULT_RAW_CREATE_VALUE, FAULT_RAW_CREATE_ENV_VALUE,  0);

    g_FiNegCreate   = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_NEG_CREATE_VALUE,   FAULT_NEG_CREATE_ENV_VALUE,   0);
    g_FiDupCreate   = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_DUP_CREATE_VALUE,   FAULT_DUP_CREATE_ENV_VALUE,   0);
    g_FiLegacySend  = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_LEGACY_SEND_VALUE,  FAULT_LEGACY_SEND_ENV_VALUE,  0);
    g_FiCaptureExit = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_CAPTURE_EXIT_VALUE, FAULT_CAPTURE_EXIT_ENV_VALUE, 0);
    g_FiPrintWindowFail = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_PW_FAIL_VALUE, FAULT_PW_FAIL_ENV_VALUE, 0);
    g_FiSlabDoubleBind  = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_SLAB_BIND_VALUE, FAULT_SLAB_BIND_ENV_VALUE, 0);
    g_FiMonStale        = (LONG)FiReadDword(moduleName, REG_CONFIG_FAULT_MON_STALE_VALUE, FAULT_MON_STALE_ENV_VALUE, 0);
    g_FiGateOff         = FiReadDword(moduleName, REG_CONFIG_FAULT_GATE_OFF_VALUE, FAULT_GATE_OFF_ENV_VALUE, 0);
    g_FiDamageDelayMs   = FiReadDword(moduleName, REG_CONFIG_FAULT_DAMAGE_DELAY_VALUE, FAULT_DAMAGE_DELAY_ENV_VALUE, 0);
    g_FiPumpStallLose   = FiReadDword(moduleName, REG_CONFIG_FAULT_PUMP_LOSE_VALUE, FAULT_PUMP_LOSE_ENV_VALUE, 0);

    g_FiArmAt = GetTickCount64() + (ULONGLONG)delaySec * 1000ULL;

    if (pumpStallSec > 0)
    {
        g_FiPumpStallMs = pumpStallSec * 1000;
        g_FiPumpStall = 1;  // one-shot by construction: one iteration, once
    }

    if (ringStallSec > 0)
    {
        g_FiRingStallUntil = g_FiArmAt + (ULONGLONG)ringStallSec * 1000ULL;
        g_FiRingStallArmed = 1;
    }

    // Loud unconditionally, at WARNING: the single most expensive mistake this module can
    // cause is a measurement run attributed to the wrong build, so every log file from a
    // fault-capable binary has to say so on its first page whether or not anything is armed.
    LogWarning("QGAFAULT-INIT build=%S armdelay=%us negcreate=%d(hwnd=0x%x) ringstall=%us "
        L"pumpstall=%us pumplose=%u captureexit=%d dupcreate=%d legacysend=%d rawcreate=%u pwfail=%d gateoff=0x%x damagedelay=%ums monstale=%d",
        g_FaultInjectionMarker,
        delaySec,
        g_FiNegCreate, g_FiNegCreateHwnd,
        ringStallSec,
        pumpStallSec, g_FiPumpStallLose,
        g_FiCaptureExit, g_FiDupCreate, g_FiLegacySend, g_FiRawCreate, g_FiPrintWindowFail,
        g_FiGateOff, g_FiDamageDelayMs, g_FiMonStale);

    if (g_FiNegCreate > 0 || g_FiDupCreate > 0 || g_FiLegacySend > 0 ||
        g_FiCaptureExit > 0 || g_FiPumpStall > 0 || g_FiRingStallArmed || g_FiRawCreate ||
        g_FiPrintWindowFail > 0 || g_FiSlabDoubleBind > 0 || g_FiGateOff != 0 || g_FiDamageDelayMs != 0 ||
        g_FiMonStale > 0)
    {
        // Armed on purpose: every result from this run describes the injected defect, not the build.
        LogWarning("QGAFAULT-INIT FAULTS ARE ARMED - this agent will break itself in %u s; "
            L"results describe the injected defect, not the build", delaySec);
    }
}

BOOL FiGateOff(IN DWORD gateBit)
{
    if ((g_FiGateOff & gateBit) == 0)
        return FALSE;

    // Announce each bypassed ShouldAcceptWindow clause exactly once, for this run. A red run that
    // cannot show the bypass was actually in force is not a proof - it is a run that happened to fail.
    if ((InterlockedOr(&g_FiGateLogged, (LONG)gateBit) & (LONG)gateBit) == 0)
    {
        LogWarning("QGAFAULT FI_GATE_OFF bit 0x%x is set: a window safeguard is bypassed, this build maps "
            L"what the release refuses", gateBit);
    }

    return TRUE;
}

BOOL FiShouldNegCreate(IN HWND window)
{
    if (g_FiNegCreate <= 0)
        return FALSE;

    // Optional targeting, checked BEFORE the shot is consumed so a non-matching window
    // does not silently burn the only shot the test armed.
    if (g_FiNegCreateHwnd != 0 &&
        (ULONG_PTR)g_FiNegCreateHwnd != ((ULONG_PTR)window & 0xFFFFFFFFULL))
        return FALSE;

    if (!FiTakeShot(&g_FiNegCreate))
        return FALSE;

    // What the shot proves: the next CREATE carries an inverted rect, and the agent-side sanitizer has to
    // stop it before it leaves; if it does not, gui-daemon's own check (xside.c:2937) rejects it and raises
    // its invalid-request dialog.
    LogWarning("QGAFAULT FI_NEG_CREATE firing for hwnd 0x%x: next CREATE gets an inverted rect; "
        L"%d shots left", window, g_FiNegCreate);
    return TRUE;
}

// [FI_RAW_CREATE] The one injection that does NOT corrupt agent state: it removes the
// agent-side sanitizer from the path so an injected bad rect actually reaches dom0. That is
// the only way to validate the instrument - CLAUDE.md requires a check to be SEEN TO FAIL,
// and the sanitizer's failure mode is "dom0 raises its invalid-GUI-request dialog", which
// cannot be observed while the sanitizer is doing its job. Not a one-shot: it is a mode.
// It stands alone deliberately - arming it WITHOUT FI_NEG_CREATE changes nothing, since a
// correct rect passes the sanitizer unaltered.
BOOL FiRawCreate(void)
{
    // Same arming delay every other fault honours (FiTakeShot line 128): a bypass that was
    // live during the daemon handshake would reproduce "the agent never came up", not the
    // defect under study.
    return g_FiRawCreate != 0 && GetTickCount64() >= g_FiArmAt;
}

BOOL FiPrintWindowFail(void)
{
    if (g_FiPrintWindowFail <= 0)
        return FALSE;

    if (!FiTakeShot(&g_FiPrintWindowFail))
        return FALSE;

    LogWarning("QGAFAULT FI_PRINTWINDOW_FAIL firing: this capture reports PrintWindow "
        L"failure (%d shots left) - 5 consecutive on one channel must latch WCDEAD",
        g_FiPrintWindowFail);
    return TRUE;
}

BOOL FiMonStale(void)
{
    if (g_FiMonStale <= 0)
        return FALSE;

    if (!FiTakeShot(&g_FiMonStale))
        return FALSE;

    LogWarning("QGAFAULT FI_MON_STALE: GetMonitorSettings reports ERROR_INVALID_MONITOR_HANDLE "
        L"(%d shots left)", g_FiMonStale);
    return TRUE;
}

// [FI_SLAB_DOUBLE_BIND] Hand out a per-window slab that a LIVE window still holds, which is
// the defect the PWCOLLISION alarm in PwAttachWindowCarry exists to catch. The open P2 is a
// field report of one window's content rendering inside another window's frame, persistently;
// that is what dom0 would draw if two windows were announced against one granted buffer. Without
// this flag the alarm has never been seen to fire, so under CLAUDE.md's rule its silence is not
// evidence of anything. Agent-local by construction: it changes which of OUR OWN already-granted
// buffers this guest reuses for its own windows. No new grant is made, no dom0-side check is
// weakened or bypassed, and nothing outside this qube's own display can be affected.
BOOL FiSlabDoubleBind(void)
{
    if (g_FiSlabDoubleBind <= 0)
        return FALSE;

    if (!FiTakeShot(&g_FiSlabDoubleBind))
        return FALSE;

    // What the shot proves: a live window still holds the slab this attach takes, so the PWCOLLISION
    // alarm must name both windows.
    LogWarning("QGAFAULT FI_SLAB_DOUBLE_BIND firing: two windows now share one slab (%d shots left)",
        g_FiSlabDoubleBind);
    return TRUE;
}

BOOL FiRingStallActive(void)
{
    ULONGLONG now;
    LONG hits;

    // Hot path: queried once per vchan send. One load of a LONG that is almost always 0.
    if (!g_FiRingStallArmed)
        return FALSE;

    now = GetTickCount64();
    if (now < g_FiArmAt)
        return FALSE;

    if (now >= g_FiRingStallUntil)
    {
        // Disarm rather than keep evaluating: the rest of the run must be free of the
        // clock read, or the fault's cost contaminates the measurement that follows it.
        if (InterlockedExchange(&g_FiRingStallArmed, 0) != 0)
        {
            LogWarning("QGAFAULT FI_RING_STALL window closed after %d faked-full sends",
                g_FiRingStallHits);
        }
        return FALSE;
    }

    hits = InterlockedIncrement(&g_FiRingStallHits);
    // Logging every trigger of a per-send fault would be a larger stall than the fault
    // itself, and would change what is being measured. First, then decimated; the closing
    // line above reports the true total.
    if (hits == 1 || (hits % 1000) == 0)
    {
        LogWarning("QGAFAULT FI_RING_STALL active: send %d sees the ring as full "
            L"(window ends at tick %I64u)", hits, g_FiRingStallUntil);
    }

    return TRUE;
}

// [FI_PUMP_LOSE] TRUE when the stall must end with an injected lost wakeup (see g_FiPumpStallLose).
BOOL FiPumpStallLose(void)
{
    return g_FiPumpStallLose != 0;
}

DWORD FiPumpStallMs(void)
{
    if (!FiTakeShot(&g_FiPumpStall))
        return 0;

    LogWarning("QGAFAULT FI_PUMP_STALL firing: this WatchForEvents iteration sleeps %u ms; "
        L"the vchan is NOT serviced for that long (H2 signature)", g_FiPumpStallMs);
    return g_FiPumpStallMs;
}

// docs/DESIGN-rest-zero-capture.md S0/M5: the typing-latency instrument (key -> the agent's first damage for that window)
// must be SEEN TO FAIL before its numbers count - this delays every damage message by a known amount.
DWORD FiDamageDelayMs(void)
{
    if (g_FiDamageDelayMs == 0 || GetTickCount64() < g_FiArmAt)
        return 0;
    if (InterlockedExchange(&g_FiDamageDelayLogged, 1) == 0)
        LogWarning("QGAFAULT FI_DAMAGE_DELAY firing: every damage message now waits %u ms before it is traced and sent",
            g_FiDamageDelayMs);
    return g_FiDamageDelayMs;
}

BOOL FiShouldCaptureExit(void)
{
    if (!FiTakeShot(&g_FiCaptureExit))
        return FALSE;

    // DELIBERATELY AVOIDS THE WORDS "capture thread". rnd8-resolution.sh counts real deaths by
    // grepping the agent log for /capture thread|thread exiting|giving up/, and the previous
    // wording of THIS message matched it. Measured 2026-08-31: with the fault armed, the only
    // line in the whole log matching that pattern was this one, so the check scored a "thread
    // death" it had detected from the injector announcing itself - a self-referential red that
    // proves nothing. An injector that trips the very check it is meant to validate is worse
    // than no injector: it manufactures the evidence.
    // What the fault does downstream, kept out of the line: the DDA worker returns without signalling
    // error_event, so the main loop is never told that duplication stopped - the silent-death shape the
    // resolution harness counts.
    LogWarning("QGAFAULT FI_CAPTURE_EXIT firing: the duplication worker quits without telling the main loop");
    return TRUE;
}

BOOL FiShouldDupCreate(IN HWND window)
{
    if (!FiTakeShot(&g_FiDupCreate))
        return FALSE;

    LogWarning("QGAFAULT FI_DUP_CREATE firing for hwnd 0x%x: re-emitting MSG_CREATE for a "
        L"window that already has one. %d shots left", window, g_FiDupCreate);
    return TRUE;
}

BOOL FiShouldLegacySend(void)
{
    if (!FiTakeShot(&g_FiLegacySend))
        return FALSE;

    // The unbounded path this send takes is the vendored vchan-common.c send (its lines 96-102 when
    // this was written), bypassing the bounded wrapper: the pre-fix behaviour vchan.c reproduces on
    // demand under this knob.
    LogWarning("QGAFAULT FI_LEGACY_SEND firing: this send may block forever (legacy path); %d shots left",
        g_FiLegacySend);
    return TRUE;
}

#endif // QGA_FAULT_INJECTION
