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

#include "perf.h"

#include <log.h>
#include <config.h>
#include <qubesdb-client.h>   // dom0-set feature: /qubes-service/enableWinKey

BOOL     g_PerfEnabled = FALSE;
BOOL     g_ProtoTrace  = FALSE;
BOOL     g_ProtoTraceWobble = FALSE;
BOOL     g_ButtonAbsolute = TRUE;
// DEFAULT NONE (2026-08-12 verdict, wf_82456c4a): every classified shell surface stays an
// override-redirect corner popup. A WM-managed Start is announced at a MOVED rect, but its
// content is slice-fed from the composited desktop at that rect - and a DirectComposition
// Start does not paint its card at the moved HWND position, so the user saw bare wallpaper.
// Slice-feed is only correct at the surface's NATURAL anchor, and no alternative capture
// path exists for these surfaces (PrintWindow/WGC of the XAML host is blank from our
// context). The OR corner Start is the one configuration ever confirmed to render
// correctly. Movable Start needs a frozen-anchor architecture (capture window-relative at
// the natural anchor, let dom0 move only the frame) - a separate future experiment.
DWORD    g_ShellManaged = SHELL_MANAGED_NONE;
BOOL     g_SeamlessStart = FALSE;
BOOL     g_BlockMenuKey = TRUE;
BOOL     g_FocusRaise  = FALSE;
BOOL     g_DdaCapture  = TRUE;
BOOL     g_FrameDrop   = FALSE;
BOOL     g_SweepDdaExempt = TRUE;
BOOL     g_InputDragFreeze = FALSE; // fallback tier only; the servo below is the default fix
// DEFAULT OFF (2026-08-13). The Smith-predictor servo is an EXPERIMENT, not a shipped
// fix: side-by-side on the guest the user judged servo-on vs servo-off "marginal, both
// suck in a way". It also misbehaved in both directions while being tuned - extrapolated
// jumps when the reconstructed origin was applied at full gain, and stalls when that
// reconstruction ran ahead so the deviation collapsed to zero. Shipping the historic
// translation keeps the drag path predictable and free of a mechanism that can fail in two
// modes; the residual ~16% announce wobble is the SAME one present in the build the user
// called "works well", and its real fix is structural (see docs/PLAN-drag-quality.md).
// Set InputDragServo=1 to re-enable for further experiments.
BOOL     g_InputDragServo = FALSE;

// QUANTISED-ORIGIN DRAG (InputDragQuantise=1). Reconstruct the dom0 cursor against the last
// announce dom0 has CERTAINLY applied, instead of the live window position (which leads dom0 and
// closes the oscillating loop) or a predicted origin (the servo, whose estimate was the weak part).
// InputDragAdoptMs is how long an announce is assumed to take to land - below it the previous
// origin is kept. InputDragAnnounceMs paces announces during the drag: larger means dom0's window
// steps rather than glides, but the origin is settled a larger fraction of the time.
BOOL     g_InputDragQuantise = FALSE;
DWORD    g_InputDragAdoptMs = 120;
DWORD    g_InputDragAnnounceMs = 0;   // 0 = announce at the natural rate
DWORD    g_InputDragServoGainPct = 85;  // user-accepted on the guest 2026-08-13 (was 60):
                                       // damped enough to absorb predictor error, snappy
                                       // enough that slow drags track cleanly
DWORD    g_InputDragServoTauMs = 25;   // assumed announce transit+apply time
DWORD    g_InputDragServoDeadband = 3;
DWORD    g_InputDragServoFastPx = 24;
BOOL     g_InputDragServoClamp = TRUE;
// Gain scheduling is NEUTRAL by default (fast gain == base gain). At 100% a mis-reconstructed
// dom0 origin was applied in full and produced 'crazy extrapolated jumps' on fast drags
// (user, 2026-08-13); the damping had been absorbing those prediction errors. Re-enable per
// guest via InputDragServoFastGainPct once the clamp below is proven in the field.
DWORD    g_InputDragServoFastGainPct = 85;
BOOL     g_DdaMoveInvalidate = TRUE;
BOOL     g_InputDragSlice = TRUE;
BOOL     g_InputDragFreezeContent = TRUE;
BOOL     g_DragEventPriority = TRUE; // default ON: announces at input rate while dragging
BOOL     g_MonInfoCache = TRUE;   // measured 3x interleaved: upd p95 3457->1631us,
                                  // upd max 40.2->14.6ms (the announce-blocking stalls)
BOOL     g_ResyncDragDefer = FALSE;
LONGLONG g_PerfFreq = 0;
DWORD    g_PerfEveryN = 1;

__declspec(thread) LONGLONG g_PerfSendTicks = 0;
__declspec(thread) LONG     g_PerfSendCount = 0;

// Accumulated over g_PerfEveryN frames. Touched only by the main loop.
typedef struct _PERF_ACC
{
    UINT     frames;
    LONGLONG dt;        // wall time covered by those frames
    LONGLONG acquire;
    LONGLONG wakeup;
    LONGLONG moverect;
    LONGLONG dirtyrect;
    LONGLONG update;
    LONGLONG enumerate;
    LONGLONG remove;
    LONGLONG damage;
    LONGLONG send;
    LONGLONG total;
    LONG     sends;
    UINT     dirty_rects;
    UINT     move_rects;
    UINT64   dirty_area;
    UINT     interrogated;  // windows actually queried
    UINT     events;        // window events applied
    UINT     windows;   // last value, not a sum
    BOOL     seamless;  // last value
} PERF_ACC;

static PERF_ACC g_Acc;
static UINT64   g_Seq = 0;              // processed frames since agent start
static UINT     g_MoveRectsMax = 0;     // high water mark, survives into every record
static BOOL     g_MoveRectsReported = FALSE;
static LONGLONG g_PrevFrameQpc = 0;
static LONGLONG g_EmitTicks = 0;        // cost of the *previous* emit, reported as "log"
static volatile LONG g_SkippedFrames = 0;   // capture thread -> main loop
static volatile LONG g_PwSkipped = 0;       // per-window recaptures avoided (screen bytes unchanged)
static volatile LONG g_PwCaptured = 0;      // per-window recaptures actually issued
static volatile LONG g_PwRefuse[PW_REFUSE_MAX];  // why the fast path declined, by cause
static volatile LONG g_RedundantFrames = 0;      // frames dropped: damage reported, pixels identical
static volatile LONG g_DdaCaptures = 0;          // windows served from the composited desktop

void PerfInit(void)
{
    LARGE_INTEGER freq;
    WCHAR moduleName[CFG_MODULE_MAX];
    WCHAR env[8];
    DWORD value;
    BOOL enabled = (QGA_PERF_DEFAULT != 0);

    QueryPerformanceFrequency(&freq);
    g_PerfFreq = freq.QuadPart;

    if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
    {
        {
            DWORD qv = 0;
            if (ERROR_SUCCESS == CfgReadDword(moduleName, L"InputDragQuantise", &qv, NULL))
                g_InputDragQuantise = (qv != 0);
            DWORD av = 0;
            if (ERROR_SUCCESS == CfgReadDword(moduleName, L"InputDragAdoptMs", &av, NULL) && av > 0)
                g_InputDragAdoptMs = av;
            DWORD nv = 0;
            if (ERROR_SUCCESS == CfgReadDword(moduleName, L"InputDragAnnounceMs", &nv, NULL))
                g_InputDragAnnounceMs = nv;
            LogInfo("QGADRAGQUANT %s (adopt=%lu ms, announce pacing=%lu ms)",
                g_InputDragQuantise ? L"on" : L"off", g_InputDragAdoptMs, g_InputDragAnnounceMs);
        }

        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_PERF_VALUE, &value, NULL))
            enabled = (value != 0);

        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_PERF_EVERY_VALUE, &value, NULL) && value > 0)
            g_PerfEveryN = value;
    }

    if (GetEnvironmentVariable(PERF_ENV_VALUE, env, RTL_NUMBER_OF(env)) > 0)
        enabled = (env[0] != L'0');

    g_PerfEnabled = enabled;

    // Protocol trace is independent of the perf switch: it is about correctness, not cost.
    {
        BOOL proto = FALSE;
        DWORD pv = 0;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_PROTO_VALUE, &pv, NULL))
            proto = (pv != 0);
        if (GetEnvironmentVariable(PROTO_ENV_VALUE, env, RTL_NUMBER_OF(env)) > 0)
            proto = (env[0] != L'0');
        g_ProtoTrace = proto;

        // The per-rect live-rect (wobble) probe is opt-in on top of ProtoTrace: it costs a
        // DWM query + lock per damage rect, which under a drag is a measured tail hazard.
        BOOL wobble = FALSE;
        DWORD wv = 0;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_PROTO_WOBBLE_VALUE, &wv, NULL))
            wobble = (wv != 0);
        g_ProtoTraceWobble = wobble;
        LogInfo("QGAPROTO %s (wobble probe %s)", g_ProtoTrace ? L"on" : L"off",
            g_ProtoTraceWobble ? L"on" : L"off");
    }

    // Z-order sync switch. Read here for the same reason as ProtoTrace: it is behaviour, not
    // measurement, so it must apply whether or not the perf log is on. Logged unconditionally
    // so any captured log states which condition produced it - a hit rate is meaningless
    // without knowing whether the raise was active.
    {
        BOOL raise = FALSE;
        DWORD rv = 0;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_FOCUS_RAISE_VALUE, &rv, NULL))
            raise = (rv != 0);
        g_FocusRaise = raise;
        LogInfo("QGAFOCUSRAISE %s", g_FocusRaise ? L"on" : L"off");
    }

    // Button events carry their own absolute position (fixes clicks landing wherever the
    // last motion happened to be - the unclickable-toast defect). Default ON; the registry
    // switch exists as the escape hatch for the interleaved regression run.
    {
        BOOL btnAbs = TRUE;
        DWORD bv = 1;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_BUTTON_ABS_VALUE, &bv, NULL))
            btnAbs = (bv != 0);
        g_ButtonAbsolute = btnAbs;
        LogInfo("QGABUTTONABS %s", g_ButtonAbsolute ? L"on" : L"off");
    }

    // Shell-surface policy: 0 = none (all or=1), 1 = all managed, 2 = Start-only managed
    // (default - the GWeck goal state: movable+size-locked Start, corner-popup toasts).
    {
        DWORD mv = SHELL_MANAGED_NONE;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_SHELL_MANAGED_VALUE, &mv, NULL))
        {
            if (mv > SHELL_MANAGED_START)
                mv = SHELL_MANAGED_START;
        }
        else
            mv = SHELL_MANAGED_NONE;
        g_ShellManaged = mv;
        LogInfo("QGASHELLMANAGED policy=%u (0=none 1=all 2=start-only)", g_ShellManaged);
    }

    // Menu-key block (seamless only; see perf.h).
    //
    // SCOPE, deliberately: the block is applied as `g_BlockMenuKey && g_SeamlessMode`, so this
    // flag NEVER affects fullscreen mode. There the guest owns the whole screen and its own
    // desktop conventions, so the Super/Windows key always reaches Windows regardless of what is
    // configured here. Everything below is about SEAMLESS mode only, where dom0 owns the key.
    {
        DWORD bv = 1;
        BOOL blk = TRUE;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_BLOCK_MENU_KEY_VALUE, &bv, NULL))
            blk = (bv != 0);

        // dom0-side knob, and it WINS over the guest registry value: the admin decides what a
        // qube may take from the window manager, not the guest. Set from dom0 with
        //
        //     qvm-features <vm> service.enableWinKey 1
        //
        // `service.`-prefixed features are the ones Qubes exports into the guest's qubesdb (as
        // /qubes-service/<name>); a plain feature stays dom0-side and the guest could never see
        // it, which is why the name carries that prefix.
        //
        // DEFAULT OFF: absent feature means the key stays blocked in seamless mode, which is the
        // behaviour every existing qube already has. It exists for guests running a third-party
        // shell - OpenShell and friends - where the Super key is how the user opens their menu and
        // swallowing it makes the shell unusable.
        {
            qdb_handle_t qdb = qdb_open(NULL);
            if (qdb)
            {
                char *v = qdb_read(qdb, "/qubes-service/enableWinKey", NULL);
                if (v)
                {
                    // Any value but "0" enables the key; Qubes writes "1" for a set service.
                    blk = (v[0] == '0');
                    LogInfo("QGABLOCKWIN qubesdb enableWinKey=%S -> block %s", v, blk ? L"on" : L"off");
                    free(v);
                }
                qdb_close(qdb);
            }
        }

        g_BlockMenuKey = blk;
        LogInfo("QGABLOCKWIN %s (seamless only; fullscreen always passes the key)",
                g_BlockMenuKey ? L"on" : L"off");
    }

    // Attribution switches - registry default, marker file overrides at runtime.
    {
        DWORD v = 0;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
        {
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_DDA_CAPTURE_VALUE, &v, NULL))
                g_DdaCapture = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_FRAME_DROP_VALUE, &v, NULL))
                g_FrameDrop = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_SWEEP_EXEMPT_VALUE, &v, NULL))
                g_SweepDdaExempt = (v != 0);
        }
        LogInfo("QGADDACAPTURE %s", g_DdaCapture ? L"on" : L"off");
        LogInfo("QGAFRAMEDROP %s", g_FrameDrop ? L"on" : L"off");
        LogInfo("QGASWEEPEXEMPT %s", g_SweepDdaExempt ? L"on" : L"off");
    }

    // Drag-wobble / mis-render fixes and upd-spike experiments. Logged unconditionally,
    // like the switches above: an acceptance run is meaningless without knowing which
    // condition the deployed binary ran under.
    {
        DWORD v = 0;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
        {
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_FREEZE_VALUE, &v, NULL))
                g_InputDragFreeze = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_VALUE, &v, NULL))
                g_InputDragServo = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_GAIN_VALUE, &v, NULL))
            {
                // 0 would inject a constant (a freeze that still announces - nonsense);
                // >100 would overshoot every event. The unstable 66..100 range stays
                // reachable ON PURPOSE: gain=100 is the defect-reintroduction falsifier.
                if (v < 1)
                    v = 1;
                if (v > 100)
                    v = 100;
                g_InputDragServoGainPct = v;
            }
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_TAU_VALUE, &v, NULL))
            {
                if (v > 250) // beyond the max measured apply lag: a misconfiguration
                    v = 250;
                g_InputDragServoTauMs = v;
            }
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_FASTPX_VALUE, &v, NULL))
                g_InputDragServoFastPx = v;
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_FASTGAIN_VALUE, &v, NULL) && v <= 100)
                g_InputDragServoFastGainPct = v;
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_CLAMP_VALUE, &v, NULL))
                g_InputDragServoClamp = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_DEADBAND_VALUE, &v, NULL))
            {
                if (v > 50) // larger than the smallest measured oscillation (40 px):
                    v = 50; // past that the dead zone is itself a visible defect
                g_InputDragServoDeadband = v;
            }
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_DDA_MOVE_INVALIDATE_VALUE, &v, NULL))
                g_DdaMoveInvalidate = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SLICE_VALUE, &v, NULL))
                g_InputDragSlice = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_FREEZE_CONTENT_VALUE, &v, NULL))
                g_InputDragFreezeContent = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_DRAG_EVENT_PRIORITY_VALUE, &v, NULL))
                g_DragEventPriority = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_MON_INFO_CACHE_VALUE, &v, NULL))
                g_MonInfoCache = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_START_VALUE, &v, NULL))
                g_SeamlessStart = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_RESYNC_DRAG_DEFER_VALUE, &v, NULL))
                g_ResyncDragDefer = (v != 0);
        }
        LogInfo("QGADRAGFREEZE %s", g_InputDragFreeze ? L"on" : L"off");
        LogInfo("QGADRAGSERVO %s gain=%u%% tau=%ums deadband=%upx fast>=%upx@%u%% clamp=%s",
            g_InputDragServo ? L"on" : L"off", g_InputDragServoGainPct,
            g_InputDragServoTauMs, g_InputDragServoDeadband,
            g_InputDragServoFastPx, g_InputDragServoFastGainPct,
            g_InputDragServoClamp ? L"on" : L"off");
        LogInfo("QGADDAMOVEINV %s", g_DdaMoveInvalidate ? L"on" : L"off");
        LogInfo("QGADRAGSLICE %s", g_InputDragSlice ? L"on" : L"off");
        LogInfo("QGADRAGFREEZECONTENT %s", g_InputDragFreezeContent ? L"on" : L"off");
        LogInfo("QGADRAGEVTPRIO %s", g_DragEventPriority ? L"on" : L"off");
        LogInfo("QGAMONCACHE %s", g_MonInfoCache ? L"on" : L"off");
        LogInfo("QGASEAMLESSSTART %s", g_SeamlessStart ? L"on" : L"off (Start hidden in seamless)");
        LogInfo("QGARESYNCDEFER %s", g_ResyncDragDefer ? L"on" : L"off");
    }

    if (!g_PerfEnabled)
    {
        LogInfo("QGAPERF off");
        return;
    }

    // Calibrate the self-cost: on a Xen HVM guest QPC may be backed by the TSC
    // (~20-30ns) or by an emulated timer (~1us), which changes how much of the
    // numbers below is measurement overhead. Measure instead of assuming.
    LARGE_INTEGER a, b, t;
    LONGLONG sink = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 10000; i++)
    {
        QueryPerformanceCounter(&t);
        sink += t.QuadPart;
    }
    QueryPerformanceCounter(&b);
    LONGLONG qpcCostNs = g_PerfFreq ? ((b.QuadPart - a.QuadPart) * 1000000000LL) / g_PerfFreq / 10000LL : 0;

    LogInfo("QGAPERF on: freq=%I64d everyN=%u qpc_cost_ns=%I64d default=%d (sink %I64d)",
        g_PerfFreq, g_PerfEveryN, qpcCostNs, QGA_PERF_DEFAULT, sink);
    LogInfo("QGAPERF-HEADER v=%d fields: seq,n,mode,dt,acq,wak,mrq,drq,upd,enu,rem,dmg,snd,tot,dr,mr,mrmax,area,win,iwn,wev,sends,skip,pwskip,pwcap,pwnofb,pwnoz,pwoff,pwocc,pwnofg,pwovl,pwfirst,pwchg,frdrop,ddacap,ddmov,ddgeo,ddoff,ddlay,ddfg,ddovl,log (times in microseconds)",
        PERF_RECORD_VERSION);
}

void PerfNoteSkippedFrame(void)
{
    if (!g_PerfEnabled)
        return;

    InterlockedIncrement(&g_SkippedFrames);
}

void PerfNotePwDecision(IN BOOL skipped)
{
    if (!g_PerfEnabled)
        return;

    InterlockedIncrement(skipped ? &g_PwSkipped : &g_PwCaptured);
}

void PerfNoteRedundantFrame(void)
{
    if (!g_PerfEnabled)
        return;

    InterlockedIncrement(&g_RedundantFrames);
}

void PerfNoteDdaCapture(void)
{
    if (!g_PerfEnabled)
        return;

    InterlockedIncrement(&g_DdaCaptures);
}

void PerfNotePwRefusal(IN PW_REFUSE_REASON reason)
{
    if (!g_PerfEnabled || (unsigned)reason >= PW_REFUSE_MAX)
        return;

    InterlockedIncrement(&g_PwRefuse[reason]);
}

void PerfNoteMoveRects(IN UINT count, IN LONG srcX, IN LONG srcY, IN const RECT* dst)
{
    if (!g_PerfEnabled || count == 0)
        return;

    if (count > g_MoveRectsMax)
        g_MoveRectsMax = count;

    if (!g_MoveRectsReported)
    {
        g_MoveRectsReported = TRUE;
        // This is the answer to the "they seem to always be empty when testing"
        // TODO in capture.c: if this line ever appears, move rects are usable.
        LogInfo("QGAPERF-MOVERECTS: first non-empty GetFrameMoveRects: count=%u src=(%d,%d) dst=(%d,%d)-(%d,%d)",
            count, srcX, srcY, dst->left, dst->top, dst->right, dst->bottom);
    }
}

void PerfEmitFrame(
    IN BOOL seamless,
    IN LONGLONG frame_start_qpc,
    IN LONGLONG total_ticks,
    IN LONGLONG update_ticks,
    IN LONGLONG enum_ticks,
    IN LONGLONG remove_ticks,
    IN LONGLONG damage_ticks,
    IN LONGLONG send_ticks,
    IN LONG send_count,
    IN const PERF_CAPTURE* cap,
    IN UINT dirty_rects,
    IN UINT window_count,
    IN UINT interrogated,
    IN UINT window_events)
{
    if (!g_PerfEnabled)
        return;

    g_Seq++;

    g_Acc.frames++;
    g_Acc.dt += g_PrevFrameQpc ? (frame_start_qpc - g_PrevFrameQpc) : 0;
    g_PrevFrameQpc = frame_start_qpc;

    g_Acc.acquire += cap->acquire_ticks;
    g_Acc.wakeup += cap->signal_qpc ? (frame_start_qpc - cap->signal_qpc) : 0;
    g_Acc.moverect += cap->moverect_ticks;
    g_Acc.dirtyrect += cap->dirtyrect_ticks;
    g_Acc.dirty_area += cap->dirty_area;
    if (cap->move_rects_count != (UINT)-1)
        g_Acc.move_rects += cap->move_rects_count;

    g_Acc.update += update_ticks;
    g_Acc.enumerate += enum_ticks;
    g_Acc.remove += remove_ticks;
    g_Acc.damage += damage_ticks;
    g_Acc.send += send_ticks;
    g_Acc.total += total_ticks;
    g_Acc.sends += send_count;
    g_Acc.dirty_rects += dirty_rects;
    g_Acc.interrogated += interrogated;
    g_Acc.events += window_events;
    g_Acc.windows = window_count;
    g_Acc.seamless = seamless;

    if (g_Acc.frames < g_PerfEveryN)
        return;

    LONGLONG emitStart = PerfNow();

    // One line, integers only, no allocation and no string building of our own.
    LogInfo("QGAPERF,v=%d,seq=%I64u,n=%u,mode=%c,dt=%I64d,acq=%I64d,wak=%I64d,mrq=%I64d,drq=%I64d,"
        L"upd=%I64d,enu=%I64d,rem=%I64d,dmg=%I64d,snd=%I64d,tot=%I64d,"
        L"dr=%u,mr=%u,mrmax=%u,area=%I64u,win=%u,iwn=%u,wev=%u,sends=%d,skip=%d,pwskip=%d,pwcap=%d,"
        L"pwnofb=%d,pwnoz=%d,pwoff=%d,pwocc=%d,pwnofg=%d,pwovl=%d,pwfirst=%d,pwchg=%d,frdrop=%d,ddacap=%d,"
        L"ddmov=%d,ddgeo=%d,ddoff=%d,ddlay=%d,ddfg=%d,ddovl=%d,log=%I64d",
        PERF_RECORD_VERSION,
        g_Seq,
        g_Acc.frames,
        g_Acc.seamless ? L's' : L'f',
        PerfUs(g_Acc.dt),
        PerfUs(g_Acc.acquire),
        PerfUs(g_Acc.wakeup),
        PerfUs(g_Acc.moverect),
        PerfUs(g_Acc.dirtyrect),
        PerfUs(g_Acc.update),
        PerfUs(g_Acc.enumerate),
        PerfUs(g_Acc.remove),
        PerfUs(g_Acc.damage),
        PerfUs(g_Acc.send),
        PerfUs(g_Acc.total),
        g_Acc.dirty_rects,
        g_Acc.move_rects,
        g_MoveRectsMax,
        g_Acc.dirty_area,
        g_Acc.windows,
        g_Acc.interrogated,
        g_Acc.events,
        g_Acc.sends,
        InterlockedExchange(&g_SkippedFrames, 0),
        InterlockedExchange(&g_PwSkipped, 0),
        InterlockedExchange(&g_PwCaptured, 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_NO_FB], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_NO_ZORDER], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_OFFSCREEN], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_OCCLUDED], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_NOT_FOREGROUND], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_OVERLAP], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_FIRST_SEEN], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_CONTENT_CHANGED], 0),
        InterlockedExchange(&g_RedundantFrames, 0),
        InterlockedExchange(&g_DdaCaptures, 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_MOVING], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_GEOMETRY], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_OFFSCREEN], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_LAYERED], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_NOTFG], 0),
        InterlockedExchange(&g_PwRefuse[PW_REFUSE_DDA_OVERLAP], 0),
        PerfUs(g_EmitTicks));

    g_EmitTicks = PerfNow() - emitStart;
    ZeroMemory(&g_Acc, sizeof(g_Acc));
}
