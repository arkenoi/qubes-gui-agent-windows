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

// QUANTISED-ORIGIN DRAG (InputDragQuantise=1). Reconstruct the dom0 cursor against the last
// announce dom0 has CERTAINLY applied, instead of the live window position (which leads dom0 and
// closes the oscillating loop) or a predicted origin (the servo, whose estimate was the weak part).
// InputDragAdoptMs is how long an announce is assumed to take to land - below it the previous
// origin is kept. InputDragAnnounceMs paces announces during the drag: larger means dom0's window
// steps rather than glides, but the origin is settled a larger fraction of the time.
BOOL     g_InputDragQuantise = TRUE;   // default ON: what it replaces is the oscillator itself
DWORD    g_InputDragAdoptMs = 120;
DWORD    g_InputDragAnnounceMs = 0;   // 0 = announce at the natural rate
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
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_TAU_VALUE, &v, NULL))
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_FASTPX_VALUE, &v, NULL))
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_FASTGAIN_VALUE, &v, NULL) && v <= 100)
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_CLAMP_VALUE, &v, NULL))
            if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_INPUT_DRAG_SERVO_DEADBAND_VALUE, &v, NULL))
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
