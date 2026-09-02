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
#define WGCBRK_ABI_VERSION  1u
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
} WGCBRK_SLOT;

#define WGCBRK_HDR(base)       ((WGCBRK_HEADER*)(base))
#define WGCBRK_SLOTS(base)     ((WGCBRK_SLOT*)((BYTE*)(base)+sizeof(WGCBRK_HEADER)))
#define WGCBRK_ARENA(base,off) ((BYTE*)(base)+(off))
#define WGCBRK_HEADER_BYTES    (sizeof(WGCBRK_HEADER)+WGCBRK_MAX_SLOTS*sizeof(WGCBRK_SLOT))
