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
 * Optional per-frame performance instrumentation for the seamless capture path.
 *
 * It answers three questions that decide where the seamless-mode latency goes:
 *   (a) how much of a frame is spent tracking windows (UpdateWindowData + EnumWindows),
 *   (b) how much is spent extracting/intersecting damage,
 *   (c) how much is spent writing messages to the vchan,
 * and it records whether IDXGIOutputDuplication::GetFrameMoveRects ever returns
 * anything (the open TODO in capture.c).
 *
 * Everything here is inert when disabled: the only cost on the hot path is a
 * predictable test of a global BOOL. Output goes to the regular agent log
 * (which already rotates: one file per process start, older than LogRetention
 * seconds purged at startup), one CSV-ish line per emit, at LOG_LEVEL_INFO so
 * the default LogLevel=3 does not need to be raised (raising it to DEBUG or
 * VERBOSE would itself dominate what we are trying to measure).
 */

#pragma once
#include <windows.h>

// Compile-time default for the master switch, used when neither the registry
// value nor the environment variable is present.
// 1 for the Qubes display-performance measurement builds, 0 for upstream.
#ifndef QGA_PERF_DEFAULT
#define QGA_PERF_DEFAULT 1
#endif

// Registry values, read from HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent
// (falling back to the parent key, like every other config value in the agent).
#define REG_CONFIG_PERF_VALUE       L"PerfLog"      // DWORD 0/1
#define REG_CONFIG_PERF_EVERY_VALUE L"PerfEveryN"   // DWORD >= 1, frames aggregated per log line

// Environment override (wins over the registry): QUBES_GUI_PERF=0|1
#define PERF_ENV_VALUE L"QUBES_GUI_PERF"

// Version of the CSV record format, bumped when fields change.
// v2 added iwn/wev (window tracking became event driven, see PHASE2A-NOTES.md).
#define PERF_RECORD_VERSION 2

extern BOOL     g_PerfEnabled;  // master switch
extern LONGLONG g_PerfFreq;     // QueryPerformanceFrequency, ticks per second
extern DWORD    g_PerfEveryN;   // emit one line per this many processed frames

// Time spent inside vchan sends, per thread (so the capture thread and the
// resolution-change thread can't corrupt the main loop's accounting).
// Maintained by VchanSendTimed(), sampled by ProcessNewFrame().
extern __declspec(thread) LONGLONG g_PerfSendTicks;
extern __declspec(thread) LONG     g_PerfSendCount;

// Per-frame numbers produced by the capture thread and consumed by the main
// loop. Written under CAPTURE_FRAME.lock while the main loop is blocked on
// frame_event, read by the main loop while the capture thread is blocked on
// ready_event, so no extra synchronization is needed.
typedef struct _PERF_CAPTURE
{
    LONGLONG acquire_ticks;     // AcquireNextFrame() - mostly idle wait, reported but not part of the total
    LONGLONG moverect_ticks;    // GetFrameMoveRects()
    LONGLONG dirtyrect_ticks;   // GetFrameDirtyRects() (size query + fetch + malloc)
    LONGLONG signal_qpc;        // QPC sampled right before SetEvent(frame_event)
    UINT     move_rects_count;  // move rects reported for this frame ((UINT)-1 == query failed)
    UINT64   dirty_area;        // sum of dirty rect areas, pixels
} PERF_CAPTURE;

// Read config, calibrate QPC, log the banner + CSV header. Call once from Init().
void PerfInit(void);

// Capture thread: a frame arrived with no dirty rects and was dropped.
void PerfNoteSkippedFrame(void);

// Capture thread: GetFrameMoveRects returned a non-empty set. Logs the first
// occurrence loudly (this is the answer to the capture.c move-rects TODO),
// afterwards only keeps the running maximum.
void PerfNoteMoveRects(IN UINT count, IN LONG srcX, IN LONG srcY, IN const RECT* dst);

// Main loop: account one processed frame and, every g_PerfEveryN frames, emit the record.
// All *_ticks are QPC deltas with nested send time already subtracted (except send_ticks).
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
    IN UINT interrogated,   // windows whose state was actually queried this frame
    IN UINT window_events); // window events applied this frame

// Returns 0 when instrumentation is disabled, so all the deltas computed from it are 0.
static __forceinline LONGLONG PerfNow(void)
{
    LARGE_INTEGER li;

    if (!g_PerfEnabled)
        return 0;

    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// QPC ticks -> microseconds
static __forceinline LONGLONG PerfUs(IN LONGLONG ticks)
{
    return g_PerfFreq ? (ticks * 1000000LL) / g_PerfFreq : 0;
}
