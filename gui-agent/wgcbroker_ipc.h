// wgcbroker_ipc.h - cross-session shared-memory contract between the SYSTEM gui-agent and
// the user-session wgcbroker.exe. Included by both (C and C++/WinRT). x64 guest; all fields
// naturally aligned; volatile + explicit MemoryBarrier() at the seqlock sites.
//
// SECURITY NOTE (adversary (b)): the CONTROL fields are agent-written / broker-read, and the
// STATUS+DATA fields are broker-written / agent-read. The section DACL grants the interactive
// user R/W, so a hostile user-IL process CAN corrupt broker-written fields. The agent therefore
// MUST bounds-check every broker-written offset/stride/dim (BufOffset, Stride, FrameWidth/Height,
// ActiveBuffer) against ArenaBytes AND the destination slab size on EVERY read before use -
// never trust them to index the arena. See WgcBrokerFrameValid() in the agent.
#pragma once
#include <windows.h>

#define WGCBRK_MAGIC        0x4257434Bu   /* 'KCWB' */
#define WGCBRK_ABI_VERSION  5u   /* 5: first-frame stage ticks are QPC counts, not ms */
#define WGCBRK_MAX_SLOTS    32
#define WGCBRK_RING         2             /* double buffer; 3 kills reader retries at 1.5x mem */

typedef enum { WGCBRK_FREE=0, WGCBRK_REQUESTED=1, WGCBRK_ACTIVE=2, WGCBRK_FAILED=3 } WGCBRK_STATE;

// A slot whose Hwnd is this sentinel means "capture the PRIMARY MONITOR" (CreateForMonitor)
// instead of a window. The agent slices static/override-redirect windows (menus, popups,
// toasts) out of this composited monitor frame by screen rect - the user-session WGC
// replacement for the SYSTEM agent's DDA slice. ReqWidth/Height = full screen; content is
// screen-relative (srcOrigin 0,0), unlike per-window slots (window-relative).
#define WGCBRK_MONITOR_HWND  ((UINT64)0xFFFFFFFFFFFFFFFFULL)

typedef struct _WGCBRK_HEADER {          /* 128 bytes */
    volatile LONG      Magic;            /* written LAST by agent to publish readiness */
    volatile LONG      AbiVersion;
    volatile LONG      SlotCount;        /* == WGCBRK_MAX_SLOTS */
    volatile LONG      Shutdown;         /* agent sets 1 on clean exit; broker exits */
    volatile LONG      Producing;        /* broker: 1 on Default desktop, 0 while secure (paused) */
    volatile LONG      _pad0;
    volatile LONGLONG  ArenaOffset;      /* bytes from base to the pixel arena */
    volatile LONGLONG  ArenaBytes;       /* total arena budget */
    volatile LONGLONG  AgentHeartbeat;   /* GetTickCount64, bumped each supervise pass */
    volatile LONGLONG  BrokerHeartbeat;  /* GetTickCount64, bumped each broker pass */
    volatile LONG      AgentPid;         /* the launcher agent's pid; broker exits if it changes */
    volatile LONG      BrokerPid;
    volatile LONG      ControlGen;       /* agent bumps on ANY capture-list change */
    volatile LONG      _pad1;
    BYTE               _pad2[56];
} WGCBRK_HEADER;

typedef struct _WGCBRK_SLOT {
    /* ---- CONTROL: agent writes, broker reads (cache line 0) ---- */
    volatile UINT64 Hwnd;                /* target window; 0 == slot free */
    volatile LONG   ReqWidth;            /* FULL WGC capture size (OS window rect) */
    volatile LONG   ReqHeight;
    volatile LONG   ReqCropX;            /* agent hint; broker MAY ignore in v1 (captures full) */
    volatile LONG   ReqCropY;
    volatile LONG   ReqState;            /* WGCBRK_REQUESTED / WGCBRK_FREE */
    volatile LONG   ControlSeq;          /* agent bumps after editing the fields above */
    volatile LONGLONG BufOffset[WGCBRK_RING]; /* agent-assigned arena offsets, packed w*4 rows */
    volatile LONGLONG BufBytes;          /* capacity of EACH ring buffer (>= ReqW*ReqH*4) */
    /* ---- STATUS/DATA: broker writes, agent reads (cache line 1) ---- */
    volatile LONG   AckState;            /* WGCBRK_ACTIVE / WGCBRK_FAILED / WGCBRK_FREE */
    volatile LONG   FailHr;              /* HRESULT if FAILED */
    volatile LONG   FrameWidth;          /* dims of the currently published frame */
    volatile LONG   FrameHeight;
    volatile LONG   Stride;              /* == FrameWidth*4 (packed) */
    volatile LONG   ActiveBuffer;        /* index in [0,WGCBRK_RING) holding the latest frame */
    volatile LONG   Seq;                 /* SEQLOCK: odd = write in progress, even = stable */
    volatile LONG   _pad0;
    volatile UINT64 FrameId;             /* monotonic; agent skips a slot with unchanged FrameId */
    volatile LONGLONG CaptureTick;       /* GetTickCount64 at publish; freshness vs secure-left */
    /* Pixel-exact crop: the broker PrintWindow-renders the FULL window (transparent margin comes
     * out black) and reports the menu's true OPAQUE bounding box as insets from the window rect.
     * The agent tightens the crop to these instead of UIA's +/-1-2px estimate; they never cut
     * opaque content. 0/0/0/0 = not reported (agent keeps UIA). Occupies the old _pad1[16]. */
    volatile LONG   OpaqueL, OpaqueT, OpaqueR, OpaqueB;
    /* ---- DIAGNOSTIC: broker writes, agent reads. GetTickCount64 at each stage of the
     * first-frame path for this slot, so a slot's first-content latency can be ATTRIBUTED
     * instead of guessed. 0 = stage not reached. Measured 2026-09-24: menu held_ms spans
     * 156-547 ms with everything else constant, and nothing distinguishes "the WGC session
     * took that long to set up" from "the session was ready and the application had not
     * painted yet" - which have completely different fixes. These six ticks split exactly
     * that. Diagnostic only: the agent never makes a decision on them. */
    /* The window these ticks describe, and whether that open got past CreateForWindow. Slots are
     * RECYCLED - menus churn through one slot constantly - and OpenChannel stamps OpenTick then
     * ZEROES the rest before it can fail on `!IsWindow(hwnd)` for a window that has already gone.
     * Without this the agent read the ticks of a LATER, FAILED open of the same slot while the
     * frame it was reporting on came from an earlier successful one: OpenTick present, all five
     * stages zero, which is exactly what shipped in 4.3.32 and measured nothing. */
    /* ABI 4. The block is claimed by EITHER capture path, and TickPw says which - because the
     * path that menus actually take was the one not instrumented. WGC CreateForWindow REJECTS
     * override-redirect menus/popups, so OpenChannel falls back to polled PrintWindow; the ABI-3
     * tick writes sat inside the WGC success branch, so for every menu they never ran and
     * TickHwnd still held the last WGC window. The agent then reported "slot reopened for another
     * window" for all five menus of a run - a false cause (Jev 0.87), and no measurement of the
     * only path menus use. Stage meanings differ per path, hence TickPw rather than one schema. */
    volatile UINT64   TickHwnd;          /* Hwnd the ticks below belong to; 0 = never opened */
    volatile LONG     TickOpenOk;        /* 1 once this open is past the point that can fail */
    volatile LONG     TickPw;            /* 1 = polled PrintWindow path, 0 = WGC */
    /* ABI 5: these six are QueryPerformanceCounter COUNTS, not milliseconds - the reader divides
     * by QueryPerformanceFrequency. GetTickCount64 advances in ~15.6 ms steps and every component
     * of a menu's first-content latency measured 16-31 ms, i.e. one or two of those steps: across
     * 4 runs of ONE unchanged binary the ranking of the four components inverted every time and
     * each was both largest and smallest at least once (Jev: clock-resolution 0.81). The split was
     * quantisation noise. The heartbeat and CaptureTick fields stay on GetTickCount64 - they are
     * compared against it elsewhere. */
    volatile LONGLONG OpenTick;          /* OpenChannel entered (both paths) */
    volatile LONGLONG ItemTick;          /* WGC: CreateForWindow returned.     PW: unused (0) */
    volatile LONGLONG PoolTick;          /* WGC: pool + session created.       PW: unused (0) */
    volatile LONGLONG StartTick;         /* WGC: StartCapture returned.        PW: first poll entered */
    volatile LONGLONG FirstArrivedTick;  /* WGC: first FrameArrived.           PW: first PrintWindow returned */
    volatile LONGLONG FirstPublishTick;  /* first publish completed (both paths) */
    volatile LONG     PollCount;         /* PW: polls entered for this open; WGC: 0 */
    volatile LONG     _padTick2;
} WGCBRK_SLOT;

#define WGCBRK_HDR(base)       ((WGCBRK_HEADER*)(base))
#define WGCBRK_SLOTS(base)     ((WGCBRK_SLOT*)((BYTE*)(base)+sizeof(WGCBRK_HEADER)))
#define WGCBRK_ARENA(base,off) ((BYTE*)(base)+(off))
#define WGCBRK_HEADER_BYTES    (sizeof(WGCBRK_HEADER)+WGCBRK_MAX_SLOTS*sizeof(WGCBRK_SLOT))
