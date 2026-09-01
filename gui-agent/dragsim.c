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

#include "dragsim.h"
#include "main.h"
#include "perf.h"
#include "vchan-handlers.h"
#include "xorg-keymap.h"

#include <log.h>
#include <config.h>
#include <stdint.h>

/* ------------------------------------------------- dom0 model: independent announce log --- */

#define SIM_ANN_MAX 512
typedef struct { DWORD Tick; int X, Y; } SIM_ANN;
static SIM_ANN       g_SimAnn[SIM_ANN_MAX];
static volatile LONG g_SimAnnHead = 0;   // monotone write counter
static volatile LONG g_SimActive = 0;

void DragSimNoteAnnounce(IN int x, IN int y)
{
    if (!g_SimActive)
        return;
    LONG n = InterlockedIncrement(&g_SimAnnHead) - 1;
    SIM_ANN* a = &g_SimAnn[n % SIM_ANN_MAX];
    a->X = x;
    a->Y = y;
    a->Tick = GetTickCount();  // written last: a reader seeing the tick sees the position
}

/*
 * dom0's applied origin at (now - lagMs), modelled the way dom0 really behaves: a STEP at the
 * moment it applies a configure, not a ramp. Returns the newest announce at least lagMs old.
 */
static void SimDom0Origin(IN DWORD lagMs, IN OUT int* x, IN OUT int* y)
{
    const DWORD now = GetTickCount();
    const LONG head = g_SimAnnHead;
    const LONG count = (head < SIM_ANN_MAX) ? head : SIM_ANN_MAX;

    for (LONG i = 1; i <= count; i++)
    {
        const SIM_ANN* a = &g_SimAnn[(head - i) % SIM_ANN_MAX];
        if ((DWORD)(now - a->Tick) >= lagMs)
        {
            *x = a->X;
            *y = a->Y;
            return;
        }
    }
    if (count > 0)
    {
        const SIM_ANN* a = &g_SimAnn[(head - count) % SIM_ANN_MAX];
        *x = a->X;
        *y = a->Y;
    }
}

/* --------------------------------------------------------------------- the simulator ------ */

typedef struct
{
    DWORD DurationMs;   // DragSimMs
    DWORD LagMs;        // DragSimLagMs    - modelled dom0 apply lag (measured p75 = 17)
    DWORD RateHz;       // DragSimRateHz   - motion event rate (dom0 measured ~45/s)
    DWORD AmpPx;        // DragSimAmpPx    - half-travel of the triangle path
    DWORD SpeedPxS;     // DragSimSpeedPxS - hand speed along the path
    DWORD Hwnd;         // DragSimHwnd     - target window, 0 = pick one
} SIM_CFG;

static DWORD SimCfgDword(IN const WCHAR* name, IN DWORD dflt)
{
    WCHAR moduleName[CFG_MODULE_MAX];
    DWORD v = 0;
    if (ERROR_SUCCESS != CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
        return dflt;
    if (ERROR_SUCCESS == CfgReadDword(moduleName, name, &v, NULL))
        return v;
    return dflt;
}

/*
 * The window to drag: the largest tracked, visible, non-iconic window with a title bar - i.e.
 * one a user could actually grab. Returns its ANNOUNCED rect, which is the space dom0's
 * window-relative coordinates are expressed in.
 */
static BOOL SimPickWindow(IN DWORD wanted, OUT HWND* window, OUT RECT* announced)
{
    BOOL found = FALSE;
    LONG bestArea = 0;

    EnterCriticalSection(&g_csWatchedWindows);
    for (LIST_ENTRY* e = g_WatchedWindowsList.Flink; e != &g_WatchedWindowsList; e = e->Flink)
    {
        WINDOW_DATA* d = CONTAINING_RECORD(e, WINDOW_DATA, ListEntry);
        if (d->Synthesized || d->DeletePending || d->IsIconic || !d->IsVisible)
            continue;
        if (wanted && (DWORD)(ULONG_PTR)d->Handle != wanted)
            continue;
        if (!wanted && !(d->Style & WS_CAPTION))
            continue;
        const LONG area = (LONG)d->Width * (LONG)d->Height;
        if (found && area <= bestArea)
            continue;
        bestArea = area;
        found = TRUE;
        *window = d->Handle;
        announced->left = d->X;
        announced->top = d->Y;
        announced->right = d->X + (int)d->Width;
        announced->bottom = d->Y + (int)d->Height;
    }
    LeaveCriticalSection(&g_csWatchedWindows);
    return found;
}

/* Triangle wave: constant speed, direction reversed ONLY at the turning points. A sine would
 * reverse continuously and make "the window moved against the hand" unmeasurable. */
static int SimPath(IN DWORD elapsedMs, IN const SIM_CFG* cfg, OUT int* leg)
{
    const int amp = (int)cfg->AmpPx;
    const DWORD legMs = (cfg->SpeedPxS == 0) ? 1000 : (2 * (DWORD)amp * 1000) / cfg->SpeedPxS;
    if (legMs == 0)
    {
        *leg = 0;
        return 0;
    }
    const DWORD phase = elapsedMs % (2 * legMs);
    *leg = (int)(elapsedMs / legMs);
    if (phase < legMs)
        return -amp + (int)((2 * (INT64)amp * phase) / legMs);       // left -> right
    return amp - (int)((2 * (INT64)amp * (phase - legMs)) / legMs);  // right -> left
}

typedef struct
{
    int WinX;   // raw window origin (GetWindowRect space)
    int CurX;   // true dom0 cursor this step
    int Leg;
} SIM_SAMPLE;

#define SIM_SAMPLE_MAX 2048
static SIM_SAMPLE g_Samples[SIM_SAMPLE_MAX];

static void SimRunOnce(void)
{
    SIM_CFG cfg;
    cfg.DurationMs = SimCfgDword(L"DragSimMs", 6000);
    cfg.LagMs = SimCfgDword(L"DragSimLagMs", 17);   // measured dom0 apply lag, p75
    cfg.RateHz = SimCfgDword(L"DragSimRateHz", 45); // measured dom0 motion rate
    cfg.AmpPx = SimCfgDword(L"DragSimAmpPx", 220);
    cfg.SpeedPxS = SimCfgDword(L"DragSimSpeedPxS", 600);
    cfg.Hwnd = SimCfgDword(L"DragSimHwnd", 0);

    if (cfg.RateHz == 0 || cfg.RateHz > 500)
        cfg.RateHz = 45;
    if (cfg.DurationMs > 60000)
        cfg.DurationMs = 60000;

    HWND window = NULL;
    RECT ann = { 0 };
    if (!SimPickWindow(cfg.Hwnd, &window, &ann))
    {
        LogWarning("QGADRAGSIM NOT RUN - no draggable tracked window (hwnd filter 0x%x). "
            L"Open a normal window in the guest first.", cfg.Hwnd);
        return;
    }

    RECT raw;
    if (!GetWindowRect(window, &raw))
    {
        LogWarning("QGADRAGSIM NOT RUN - GetWindowRect failed on 0x%x",
            (uint32_t)(ULONG_PTR)window);
        return;
    }

    // Grab point on the title bar, in dom0's space (the ANNOUNCED rect)...
    const int annW = ann.right - ann.left;
    const int grabX = annW / 2;
    const int grabY = 12;
    // ...and the same point in GetWindowRect space, where the window itself lives. The two
    // differ by the DWM trim, and a metric that ignored it would report a constant fake
    // deviation on every run.
    const int grabRawX = (ann.left + grabX) - raw.left;

    LogInfo("QGADRAGSIM start hwnd=0x%x ann=(%d,%d %dx%d) raw=(%d,%d) grab=(%d,%d) "
        L"dur=%lums lag=%lums rate=%luHz amp=%lupx speed=%lupx/s "
        L"[quantise=%d interp=%d adopt=%lu announce=%lu freeze=%d servo=%d cfgguard=%d]",
        (uint32_t)(ULONG_PTR)window, ann.left, ann.top, annW, ann.bottom - ann.top,
        raw.left, raw.top, grabX, grabY,
        cfg.DurationMs, cfg.LagMs, cfg.RateHz, cfg.AmpPx, cfg.SpeedPxS,
        g_InputDragQuantise ? 1 : 0, g_InputDragOriginInterp ? 1 : 0,
        g_InputDragAdoptMs, g_InputDragAnnounceMs,
        g_InputDragFreeze ? 1 : 0, g_InputDragServo ? 1 : 0,
        g_InputDragCfgGuard ? 1 : 0);

    UINT sampleCount = 0;

    // Arm the model with the window's current announced position: until the first mid-drag
    // announce lands, dom0's origin cannot differ from it.
    g_SimAnnHead = 0;
    InterlockedExchange(&g_SimActive, 1);
    DragSimNoteAnnounce(ann.left, ann.top);

    // The press. ProcessButtonEvent is the real handler: it arms the latch, seeds the announce
    // ring, and injects a real LEFTDOWN at the translated position, which starts Windows' own
    // modal move loop on the real window.
    const int cur0X = ann.left + grabX;
    const int cur0Y = ann.top + grabY;
    (void)ProcessButtonEvent(window, grabX, grabY, Button1, ButtonPress);

    const DWORD stepMs = 1000 / cfg.RateHz;
    const DWORD startTick = GetTickCount();
    int lastRelX = grabX, lastRelY = grabY;

    for (;;)
    {
        const DWORD elapsed = GetTickCount() - startTick;
        if (elapsed >= cfg.DurationMs)
            break;

        int leg = 0;
        const int curX = cur0X + SimPath(elapsed, &cfg, &leg);
        const int curY = cur0Y;

        int dX = ann.left, dY = ann.top;
        SimDom0Origin(cfg.LagMs, &dX, &dY);

        lastRelX = curX - dX;
        lastRelY = curY - dY;
        (void)ProcessMotionEvent(window, lastRelX, lastRelY, FALSE);

        // Ground truth for this step. Sampled AFTER the injection and after a settle, because
        // the modal move loop runs on the app's thread and SendInput is asynchronous.
        Sleep(stepMs);
        RECT now;
        if (GetWindowRect(window, &now) && sampleCount < SIM_SAMPLE_MAX)
        {
            SIM_SAMPLE* s = &g_Samples[sampleCount++];
            s->WinX = now.left;
            s->CurX = curX;
            s->Leg = leg;
        }
    }

    (void)ProcessButtonEvent(window, lastRelX, lastRelY, Button1, ButtonRelease);
    Sleep(400); // let the release settle and the resting announce flush
    InterlockedExchange(&g_SimActive, 0);

    /* ---- metrics ------------------------------------------------------------------------
     * REVERSAL: a step where the window moved AGAINST the hand by more than the noise floor.
     * The path is a constant-velocity triangle, so within a leg the hand never reverses and
     * any such step is the feedback loop, not the stimulus. Steps at a turning point are
     * excluded (the leg index changes there), where the hand really does reverse.
     * DEVIATION: |(window + grab) - cursor| - how far the window is from where the hand is
     * holding it. A stable law keeps this bounded; the gain-1 oscillator does not.
     */
    UINT steps = 0, reversals = 0;
    int maxBack = 0, maxDev = 0;
    INT64 devSum = 0;
    int devHist[16] = { 0 };  // 16 px buckets: a p90 without sorting

    for (UINT i = 1; i < sampleCount; i++)
    {
        if (g_Samples[i].Leg != g_Samples[i - 1].Leg)
            continue; // turning point: the hand really did reverse here
        const int dHand = g_Samples[i].CurX - g_Samples[i - 1].CurX;
        const int dWin = g_Samples[i].WinX - g_Samples[i - 1].WinX;
        if (dHand == 0)
            continue;
        steps++;
        if (dWin != 0 && ((dWin > 0) != (dHand > 0)) && (dWin > 3 || dWin < -3))
        {
            reversals++;
            const int back = (dWin < 0) ? -dWin : dWin;
            if (back > maxBack)
                maxBack = back;
        }
        int dev = (g_Samples[i].WinX + grabRawX) - g_Samples[i].CurX;
        if (dev < 0)
            dev = -dev;
        devSum += dev;
        if (dev > maxDev)
            maxDev = dev;
        devHist[(dev / 16 < 15) ? (dev / 16) : 15]++;
    }

    int devP90 = 0;
    if (steps > 0)
    {
        const UINT target = (steps * 90) / 100;
        UINT acc = 0;
        for (int b = 0; b < 16; b++)
        {
            acc += devHist[b];
            if (acc >= target)
            {
                devP90 = b * 16;
                break;
            }
        }
    }

    if (sampleCount == 0 || steps == 0)
    {
        // MISSING DATA FAILS. A run that sampled nothing must not print a zero-reversal line
        // that reads as a pass - that is the exact failure this file exists to stop repeating.
        LogWarning("QGADRAGSIM NO DATA - samples=%u usable_steps=%u. The run produced nothing "
            L"to score; this is a FAILED run, not a clean one.", sampleCount, steps);
        return;
    }

    LogInfo("QGADRAGSIM result steps=%u reversals=%u pct=%u maxback=%dpx dev_mean=%dpx "
        L"dev_p90=%dpx dev_max=%dpx samples=%u  (SYNTHETIC - not a verdict on the wobble, "
        L"see dragsim.h)",
        steps, reversals, reversals * 100 / steps, maxBack,
        (int)(devSum / steps), devP90, maxDev, sampleCount);
}

static DWORD WINAPI SimThread(void* param)
{
    UNREFERENCED_PARAMETER(param);
    WCHAR moduleName[CFG_MODULE_MAX];

    if (ERROR_SUCCESS != CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
        return 0;

    LogInfo("QGADRAGSIM poller up: set DragSimGo=1 under the gui-agent key to run one "
        L"synthetic guest-native drag");

    for (;;)
    {
        DWORD go = 0;
        if (ERROR_SUCCESS == CfgReadDword(moduleName, L"DragSimGo", &go, NULL) && go != 0)
        {
            // Clear FIRST: a run that crashes or is killed must not re-trigger on every poll.
            (void)CfgWriteDword(moduleName, L"DragSimGo", 0, NULL);
            SimRunOnce();
        }
        Sleep(500);
    }
}

void DragSimStart(void)
{
    if (SimCfgDword(L"DragSim", 0) == 0)
        return;

    HANDLE t = CreateThread(NULL, 0, SimThread, NULL, 0, NULL);
    if (!t)
    {
        win_perror("QGADRAGSIM CreateThread");
        return;
    }
    CloseHandle(t);
}
