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
BOOL     g_BlockMenuKey = TRUE;
BOOL     g_FocusRaise  = FALSE;
BOOL     g_DdaCapture  = TRUE;
BOOL     g_FrameDrop   = FALSE;
BOOL     g_SweepDdaExempt = TRUE;
BOOL     g_InputDragFreeze = FALSE; // fallback tier only; the servo below is the default fix
BOOL     g_InputDragServo = TRUE;
DWORD    g_InputDragServoGainPct = 60; // beta*100; 70 is already unstable under a 46 ms mismatch
DWORD    g_InputDragServoTauMs = 25;   // assumed announce transit+apply time
DWORD    g_InputDragServoDeadband = 3; // px per axis
BOOL     g_DdaMoveInvalidate = TRUE;
BOOL     g_MonInfoCache = FALSE;
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
    {
        DWORD bv = 1;
        BOOL blk = TRUE;
        if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
            ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_BLOCK_MENU_KEY_VALUE, &bv, NULL))
            blk = (bv != 0);
        g_BlockMenuKey = blk;
        LogInfo("QGABLOCKWIN %s", g_BlockMenuKey ? L"on" : L"off");
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
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_DEADBAND_VALUE, &v, NULL))
            {
                if (v > 50) // larger than the smallest measured oscillation (40 px):
                    v = 50; // past that the dead zone is itself a visible defect
                g_InputDragServoDeadband = v;
            }
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_DDA_MOVE_INVALIDATE_VALUE, &v, NULL))
                g_DdaMoveInvalidate = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_MON_INFO_CACHE_VALUE, &v, NULL))
                g_MonInfoCache = (v != 0);
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_RESYNC_DRAG_DEFER_VALUE, &v, NULL))
                g_ResyncDragDefer = (v != 0);
        }
        LogInfo("QGADRAGFREEZE %s", g_InputDragFreeze ? L"on" : L"off");
        LogInfo("QGADRAGSERVO %s gain=%u%% tau=%ums deadband=%upx",
            g_InputDragServo ? L"on" : L"off", g_InputDragServoGainPct,
            g_InputDragServoTauMs, g_InputDragServoDeadband);
        LogInfo("QGADDAMOVEINV %s", g_DdaMoveInvalidate ? L"on" : L"off");
        LogInfo("QGAMONCACHE %s", g_MonInfoCache ? L"on" : L"off");
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
