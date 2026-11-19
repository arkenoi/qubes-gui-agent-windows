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
#define WGCBRK_ABI_VERSION  29u  /* 29: the PrintWindow route, the relay and the poke are retired
                                  *     (ADR-capture 36); their fields keep their place, and a failed
                                  *     open is held FAILED with its code (FailHr) until the agent asks
                                  *     again;
                                  * 28: CropWinValid - the SAME ambiguity as ReqCardValid in the
                                  *     other direction: the broker writes CropWinX/Y only when
                                  *     GetWindowRect succeeds, so a failure left them zero and the
                                  *     agent's QGACROPFALLBACK read a healthy window as mis-cropped
                                  *     on three cells of acceptance run 40037710497. Appended; take
                                  *     the new sizeof(WGCBRK_SLOT) from the compiler, which
                                  *     tools/tests/wgcbroker-layout-selftest.sh does - the padding
                                  *     ABI 26 left over was consumed by ReqCardValid at 27, so this
                                  *     one does grow the struct;
                                  * 27: ReqCardValid - a zero-initialised slot is not a card at (0,0);
                                  *     the >= 0 test made the broker override a good crop with (0,0).
                                  *     Appended at offset 3468, which FILLS the padding ABI 26
                                  *     already carried, so sizeof(WGCBRK_SLOT) STAYS 3472 - it does
                                  *     NOT grow to 3480, as this line claimed until 2026-10-18.
                                  *     guest/wgcbroker-peek.ps1 trusted that number and read every
                                  *     slot after slot0 at the wrong base, reporting a live slot as
                                  *     hwnd=0x2d600000282 publishes=13586208 bytes=1. Take this size
                                  *     from the compiler (a file that includes this header behind a
                                  *     windows.h shim and prints sizeof/offsetof), never by counting
                                  *     fields: appending a LONG into existing padding is free;
                                  * 26: CropUsedX/Y + CropWinX/Y - the crop the broker ACTUALLY used
                                  *     and the window origin it derived it from, because the broker
                                  *     has no log and a black cropped copy was otherwise
                                  *     undiagnosable. Appended; sizeof(WGCBRK_SLOT) 3456 -> 3472;
                                  * 25: ReqCardX/ReqCardY - the card's SCREEN position, so the broker
                                  *     derives the crop from the window's LIVE rect instead of from
                                  *     insets measured at another moment. Appended; sizeof(WGCBRK_SLOT)
                                  *     3448 -> 3456;
                                  * 24: BEHAVIOUR, not layout - the broker sets
                                  *     GraphicsCaptureSession.IncludeSecondaryWindows, so an owner's
                                  *     frames contain its owned popups and the agent stops announcing
                                  *     a popup that lies wholly inside a tracked owner. The version is
                                  *     bumped PRECISELY so an older broker cannot pair: it refuses a
                                  *     section whose AbiVersion differs (wgcbroker.cpp returns 6),
                                  *     never becomes active, and the agent's WgcBrokerActive() guard
                                  *     then keeps announcing popups as before. Without the bump the
                                  *     two would pair at 23, the agent would withhold the popup and
                                  *     the old broker would not carry it - the popup would vanish
                                  *     (Jev agent-broker-skew 0.96). No field moved;
                                  *     sizeof(WGCBRK_SLOT) is unchanged;
                                  * 23: ReqRoute - the agent ASKS for a route, so a window class WGC cannot
                                  *     capture reaches the relay instead of being served black
                                  *     (2026-10-17); appended: sizeof(WGCBRK_SLOT) 3440 -> 3448;
                                  * 22: DirtyPublishes/DirtyFullCopies/DirtyBytes (rest-zero S1 dirty regions, M9(b)),
                                  *     appended: sizeof(WGCBRK_SLOT) 3424 -> 3440;
                                  * 21: AgentWake* - the agent's main-loop wakes by what woke it (rest-zero M1);
                                  * 20: SameFrames (was BackoffMs, unused since S3) - WGC arrivals not republished
                                  *     because their card equals the published one;
                                  * 19: rest-zero S3/S4 - no heartbeats; CtlAck (the request ack the agent's
                                  *     deadlines key on), DeafHolds + WGCBRK_E_DEAF, AgentFrameWakes/AgentStalls (R5);
                                  * 18: BrokerStage + HungSkips - which call a broker hang is in, and
                                  *     PrintWindows skipped for an unresponsive target; 17: Republished */
#define WGCBRK_MAX_SLOTS    32
/* ABI 13: the delivered frame reduced to a fixed WGCBRK_TILES x WGCBRK_TILES grid of per-tile MEAN
 * RGB. Fixed on BOTH sides regardless of either side's own dimensions, which is the whole point: the
 * guest measures the WHOLE window (GetWindowRect) while the broker publishes the CONTENT it was
 * handed - measured as 360x240 against 348x234, plus a crop the guest cannot see - so any comparison
 * keyed on absolute pixel coordinates compares different physical pixels. Normalising both sides to
 * the same grid reduces that difference to a slight blur at tile boundaries instead of a
 * misalignment. 1024 regions encode spatial LAYOUT, which a distinct-colour count cannot: a shifted
 * image, a blank region, the same palette arranged wrongly and a channel swap all move the mean
 * absolute difference far outside any tolerance that absorbs a frame offset.
 * Jev chose this over a 4x4/8x8 grid, a sorted-colour-multiset hash and an exact hash over an
 * identical region, at confidence 1.00 - and warned that a PASS on it ALONE is not sufficient
 * (sufficient_for_acceptance 0.23): a static comparison cannot detect a STALE frame, which needs a
 * change driven on purpose with the delivered fingerprint required to follow it
 * (stale_frame_needs_more 0.83). ALPHA IS EXCLUDED (0.95): the two sides need not agree on it and the
 * GUI protocol carries no per-pixel alpha at all. */
#define WGCBRK_TILES        32
#define WGCBRK_TILE_BYTES   (WGCBRK_TILES*WGCBRK_TILES*3)
/* ABI 19 (docs/DESIGN-rest-zero-capture.md E): FailHr of a slot whose capture item Windows closed, was reopened once and
 * closed again before it delivered a frame. The broker holds it FAILED until the agent asks again (a new ControlSeq) -
 * never a timed retry. The agent withholds the window and logs QGAWGCDEAF. A customer-bit HRESULT, so it can never
 * collide with one the system returns. Any other FailHr on a FAILED slot is the code of the call that could not open
 * the capture (ADR-capture 36), or E_HANDLE for a window that was gone. */
#define WGCBRK_E_DEAF       ((LONG)0xA057DEAFu)
/* ABI 19 (D, R1/R4): how long a request may go unacknowledged (CtlAck behind ControlSeq) before the broker is HUNG. */
#define WGCBRK_ACK_DEADLINE_MS 2000
/* ABI 19 (D, R5): how long a published frame may wait for the agent to wake on it (AgentFrameWakes unchanged). */
#define WGCBRK_AGENT_DEADLINE_MS 5000
/* The broker's singleton mutex: created as its first act and held until it exits, so the object exists exactly while
 * a broker process does. The agent reads that by opening it and never waits on it (BrokerProcessAlive). */
#define WGCBRK_SINGLETON_NAME L"Global\\QubesWgcBrokerSingleton"
/* After a quiet re-route turns out to have been wrong - the window was merely static - do not
 * re-test it for this long. Without the hysteresis a static window is re-tested every quiet
 * period for ever, which is a churn loop costing a render every couple of seconds. */
#define WGCBRK_WGC_PROBE_BACKOFF_MS 30000
#define WGCBRK_RING         2             /* double buffer; 3 kills reader retries at 1.5x mem */

typedef enum { WGCBRK_FREE=0, WGCBRK_REQUESTED=1, WGCBRK_ACTIVE=2, WGCBRK_FAILED=3 } WGCBRK_STATE;

typedef struct _WGCBRK_HEADER {          /* 128 bytes */
    volatile LONG      Magic;            /* written LAST by agent to publish readiness */
    volatile LONG      AbiVersion;
    volatile LONG      SlotCount;        /* == WGCBRK_MAX_SLOTS */
    volatile LONG      Shutdown;         /* agent sets 1 on clean exit; broker exits */
    volatile LONG      Producing;        /* broker: 1 on Default desktop, 0 while secure (paused) */
    /* ABI 18 (was _pad0): the broker's CURRENT main-loop stage, (WGCBRK_STG_* << 8) | slot, set around every
     * call that talks to another process. A hang stops the heartbeat with this still naming the call it is
     * blocked in; the agent prints it in QGABROKERHUNG. Measured 2026-09-27: every broker "death" began
     * within seconds of a window disappearing, and moving the WGC teardown out of the slot lock did not stop
     * them - the blocking call has to be named, not guessed. */
    volatile LONG      BrokerStage;
    volatile LONGLONG  ArenaOffset;      /* bytes from base to the pixel arena */
    volatile LONGLONG  ArenaBytes;       /* total arena budget */
    /* RETIRED at ABI 19 (rest-zero S4): neither side writes or reads a heartbeat any more - a heartbeat is a timer on
     * both sides and detects only a HUNG peer. Exits are process handles; hangs are request deadlines (CtlAck, R5).
     * The fields keep their place so no offset moves. */
    volatile LONGLONG  AgentHeartbeat;
    volatile LONGLONG  BrokerHeartbeat;
    volatile LONG      AgentPid;         /* the launcher agent's pid; broker exits if it changes */
    volatile LONG      BrokerPid;
    volatile LONG      ControlGen;       /* agent bumps on ANY capture-list change */
    /* RETIRED at ABI 29 (ADR-capture 36): the poke and the relay are gone, and nothing writes or reads these. The fields
     * keep their place so no offset moves. */
    volatile LONG      PokeLockMiss;
    volatile LONG      RelayCapable;
    volatile LONG      RelayOsBuild;
    /* ABI 19, R5 (docs/DESIGN-rest-zero-capture.md D): the agent bumps AgentFrameWakes on every main-loop wake (the
     * broker's frame event is one); a frame the broker published that has not moved it within WGCBRK_AGENT_DEADLINE_MS
     * is a HUNG agent,
     * counted here (a medium-IL broker cannot reap a SYSTEM agent; the remedy is the watchdog's). Was _pad2[0..15]. */
    volatile LONG      AgentFrameWakes;
    volatile LONG      AgentStalls;
    volatile LONGLONG  AgentStallTick;
    /* ABI 19, R1: bumped by the broker's main loop every time it starts or finishes a call (StageScope) and every slot
     * Reconcile handles. A request unacknowledged for WGCBRK_ACK_DEADLINE_MS is a HANG only if this did not move in that
     * time either: measured 2026-10-01, a fresh broker opening eight WGC sessions in one pass acknowledged the last one
     * after > 2 s while busy the whole time, and was reaped as hung. Was _pad2[0..3]. */
    volatile LONG      BrokerProgress;
    /* ABI 21, rest-zero M1 attribution (was _pad2[28]): the agent's main-loop wakes, by what woke it. At rest the loop
     * waits on events alone; these say which of them fired - a deadline that keeps firing at rest would be ours. */
    volatile LONG      AgentWakeFrame;     /* a desktop-duplication frame (the capture thread) */
    volatile LONG      AgentWakeVchan;     /* dom0 traffic */
    volatile LONG      AgentWakeWinEvent;  /* window events the hook thread queued */
    volatile LONG      AgentWakeBroker;    /* the broker's frame event */
    volatile LONG      AgentWakeDeadline;  /* the wait timed out: a deadline this loop armed */
    volatile LONG      AgentWakeHelper;    /* a helper's process handle, the bridge's ready event, the capture thread's exit */
    volatile LONG      AgentWakeOther;     /* shutdown, fullscreen on/off, capture error */
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
    volatile LONG   HungSkips;           /* RETIRED at ABI 29 with the PrintWindow route it counted; keeps its place */
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
    volatile LONG     TickPw;            /* RETIRED at ABI 29 (no PrintWindow path); keeps its place */
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
    volatile LONG     PollCount;         /* RETIRED at ABI 29 (no PrintWindow path); keeps its place */
    /* ABI 19 (was _padTick2), R1/R4: the ControlSeq the broker's main loop last HANDLED for this slot - written at the
     * end of each Reconcile pass over it, after any open or close. The agent arms a deadline when it bumps ControlSeq
     * and disarms it when this catches up: unanswered for WGCBRK_ACK_DEADLINE_MS = a hung broker (QGABROKERHUNG). For
     * an unregister it is also the moment the broker can no longer write the slot's buffers (CloseChannel ran under the
     * slot lock), so the arena regions are reclaimed on it instead of on a timer. */
    volatile LONG     CtlAck;
    /* ABI 6. WGC FRAME ACCOUNTING. Why: a window can sit with AckState==ACTIVE, a clean open
     * (every stage tick populated) and a healthy broker, and still publish nothing ever again -
     * measured 2026-09-25 on Settings, which published exactly 2 frames and then stopped for
     * good while another slot published ~2/s throughout. From the outside that is indistinguishable
     * from "WGC delivers nothing for this window class", but the two have OPPOSITE fixes, and
     * the shared state could not tell them apart: FrameArrived drops a frame whose ContentSize
     * differs from the pool, and if pool.Recreate() throws, poolW/poolH stay stale and EVERY
     * later frame is dropped the same way - a permanent feed loss that looks identical to a
     * dead feed. These count what actually happened, so the next capture says which it was
     * instead of being argued about. Diagnostic only: the broker never decides on them. */
    volatile LONG     FramesArrived;     /* FrameArrived callbacks entered for this channel */
    volatile LONG     FramesPublished;   /* frames that reached the publish path */
    volatile LONG     FramesDropSize;    /* dropped: ContentSize != pool size, or a surface smaller than its content */
    volatile LONG     RecreateOk;        /* pool.Recreate() succeeded */
    volatile LONG     RecreateFail;      /* pool.Recreate() threw - the stale-pool trap */
    volatile LONG     LastContentW;      /* ContentSize of the most recent arrival */
    volatile LONG     LastContentH;
    volatile LONG     PoolW;             /* the pool size it was compared against */
    volatile LONG     PoolH;
    volatile LONG     DeafHolds;         /* ABI 19 (was _padAbi6): times this slot went FAILED with WGCBRK_E_DEAF */
    /* RETIRED at ABI 29 (ADR-capture 36): the poke and the PrintWindow route they served. Nothing writes or reads
     * these five; they keep their place so no offset moves. */
    volatile LONG     PokeSeq;           /* agent: bumped when this window's pixels changed */
    volatile LONG     PokeAck;           /* broker: the PokeSeq value it last rendered for */
    volatile LONG     PollsServiced;     /* renders actually performed */
    volatile LONG     PollsSkipped;      /* ticks where nothing had changed, so nothing was done */
    volatile LONG     SafetyPolls;       /* renders forced by the staleness bound, not by a poke */
    /* Sessions the broker reopened on this slot on its own: a capture item Windows closed, or a frame dropped for size
     * (Reconcile). Counted, never silent. */
    volatile LONG     Reroutes;
    /* ABI 20 (was BackoffMs, the adaptive poll interval S3 removed): WGC arrivals whose card was byte-identical to the
     * published one and were therefore NOT republished - the agent was not woken for them. The owner's rule: the same
     * pixels repainted are not a change. Only in steady state; an arrival that answers a registration is published. */
    volatile LONG     SameFrames;
    /* RETIRED at ABI 29 (ADR-capture 36): the relay, its quiet ladder and the route counters. Nothing writes or reads
     * these; they keep their place so no offset moves. */
    volatile LONG     QuietReroutes;
    volatile LONG     ProbeBounces;
    volatile LONG     Route;
    volatile LONG     RelayOk;
    volatile LONG     RelayFail;
    volatile UINT64   RelayDest;
    /* ABI 9: DID THE EVENT FIRE, OR DID WE THROW IT AWAY? FramesArrived is incremented AFTER the
     * arrival handler's re-validation guard, so a guard that starts rejecting looks exactly like a
     * feed that stopped. These two count the raw invocation and the rejection, so the two cases can
     * never be confused. */
    volatile LONG     ArrivalRaw;       /* handler entered, before any guard */
    volatile LONG     ArrivalRejected;  /* guard returned early: pool null or a different sender */
    /* RETIRED at ABI 29 with the relay's source test; they keep their place. */
    volatile LONG     RelayStaticHolds;
    volatile LONG     RelaySrcChanged;
    volatile LONG     RelaySrcUnmeasured;
    volatile LONG     RelayPwFail;
    /* ABI 12: DO THE DELIVERED PIXELS MATCH WHAT THE WINDOW RENDERS? Route counters say where a
     * frame came from, not whether it is right, and the census's in-guest test only shows that
     * content exists outside a window's own surface. Asked to accept on that, Jev answered
     * `bar_met` 0.20 with `residual_gap = relayed-pixels-never-read` at **1.00**.
     * dom0's per-window capture cannot close it: OVERRIDE-REDIRECT windows are absent from
     * `_NET_CLIENT_LIST` by definition, so the one class that most needs checking is structurally
     * invisible there, and the alternative is photographing the whole desktop. Jev preferred
     * reading the frame in the broker (0.63) over that (0.22).
     * PubColours is the number of DISTINCT COLOURS in the frame just published, sampled every 9th
     * pixel in x and y - the identical sampling guest/window-truth-survey.ps1 uses - so the two
     * numbers mean the same thing and can be compared directly, per slot, with the slot's own Hwnd
     * giving an exact mapping instead of a guessed one. Computed at most once per second per slot. */
    volatile LONG     PubColours;
    /* ABI 13. Row-major, WGCBRK_TILES rows of WGCBRK_TILES tiles, 3 bytes per tile in R,G,B order.
     * Written at most once per second per slot, on the same throttle as PubColours. */
    volatile BYTE     PubTiles[WGCBRK_TILE_BYTES];
    /* ABI 14: THE SESSION'S OWN LIFECYCLE. Every counter before this one describes FRAMES; none
     * describes the session carrying them, and that gap made two very different faults look
     * identical from outside. Measured 2026-09-26: relay slots sat with FramesArrived frozen
     * (53 -> 53 and 2 -> 2) while direct-WGC slots in the SAME broker process advanced, and the
     * primitive was then cleared completely - a DWM thumbnail relayed into the broker's own style of
     * destination delivered 437 arrivals over 240 s without decaying. So the fault is in this code.
     * But the frame counters live HERE, in shared memory, while the session, pool and FrameArrived
     * revoker live in the broker's per-channel struct: CloseChannel resets that struct and does NOT
     * zero these, so a slot can display numbers from a PREVIOUS session while the current one is
     * absent. Jev: stale_counters_plausible **0.81**, and if they are stale it is a DIFFERENT defect
     * with the same symptom - a channel closed and never successfully reopened (0.83).
     * These four make that distinguishable without a rebuild next time. */
    volatile LONG     ChanOpens;    /* successful OpenChannel calls for this slot */
    volatile LONG     ChanCloses;   /* CloseChannel calls for this slot */
    volatile LONG     SessionLive;  /* 1 while a capture session+pool is held, 0 after a close */
    volatile LONG     ChanGen;      /* bumped on every open; identifies which session the counters belong to */
    /* ABI 15: FRAMES DELIVERED BY THE CURRENT SESSION ONLY, zeroed at every open. Every other frame
     * counter here is CUMULATIVE across the slot's whole life and is deliberately never zeroed on
     * close - which meant a slot reading FramesArrived=89 said nothing about which SESSION delivered
     * those 89. That matters because the routing ladder closes a quiet WGC-on-the-window channel and
     * reopens it as a RELAY, so a frozen relay slot at gen=2 might have inherited every one of its
     * frames from generation 1 and delivered NOTHING itself. Measured 2026-09-26: all the frozen relay
     * slots read opens=2 closes=1 gen=2, so that is exactly the ambiguity in play, and it forced the
     * withdrawal of a whole set of fidelity results that may have been measuring a frame left in the
     * shared buffer by the previous session. Jev: misattribution_plausible 0.89,
     * invalidates_fidelity 0.76, per-generation-frame-counters 0.87.
     * With this, "the relay delivered N frames" is finally a statement that can be made or refuted. */
    volatile LONG     GenFrames;
    /* ABI 16: WGC CLOSED THE CAPTURE ITEM. A GraphicsCaptureItem can be CLOSED by the system, after
     * which FrameArrived never fires again for that session. The broker never subscribed to
     * GraphicsCaptureItem.Closed, so that state was INDISTINGUISHABLE from a window whose content
     * simply stopped changing: no error anywhere, no counter, the slot still reading ACTIVE, and -
     * measured 2026-09-27 - a FRESH WGC session opened on the very same window from another process
     * returning a frame every second carrying the source's own content (colours=1080,
     * nonBlack=1080/1080) while the broker's long-lived session got nothing. Jev on that evidence:
     * brokers-own-capture-session-died 0.82, with the DWM-thumbnail and DWM-throttling readings at 0.00.
     *
     * THIS IS OBSERVATION ONLY AND DELIBERATELY CHANGES NO BEHAVIOUR. Jev rated "subscribe and observe"
     * 0.96 at 0.95 confidence, and BOTH "recreate the session now" and "observe and repair in one build"
     * 0.00 - so a repair does not get written until this counter has been seen to move on a channel that
     * is actually deaf. If it never moves while a channel is deaf, the item was NOT closed and the
     * mechanism is still unknown; a repair built now would have been built on a guess. */
    volatile LONG     ItemClosed;
    /* ABI 17: frames the broker published from its RETAINED full-window capture because the agent's
     * registration changed (ControlSeq moved) and no arrival answered it - a static window whose card
     * moved, measured on the toast host 2026-09-27 (ack=0 seq=0 for a whole census). A slot that is
     * served after a re-registration with this at 0 was served by an arrival; with it above 0, by the
     * retained capture. Lives in what was the 4-byte alignment gap before ItemClosedTick, so no other
     * field moves and sizeof(WGCBRK_SLOT) stays 3424. */
    volatile LONG     Republished;
    volatile LONGLONG ItemClosedTick;   /* GetTickCount64 of the last Closed, so it can be ordered
                                         * against CaptureTick and the poke/damage timeline */
    /* ABI 22, rest-zero S1 (docs/DESIGN-rest-zero-capture.md A) and M9(b): a WGC session in DirtyRegionMode keeps a CPU
     * copy of the window current from each arrival's dirty regions and writes only the regions that changed into the
     * spare ring buffer. DirtyPublishes: frames published from regions; DirtyFullCopies: arrivals read whole (first
     * frame, resize, no dirty-region API, the relay); DirtyBytes: bytes written into the ring, cumulative. */
    volatile LONG     DirtyPublishes;
    volatile LONG     DirtyFullCopies;
    volatile LONGLONG DirtyBytes;
    /* RETIRED at ABI 29 (ADR-capture 36) with the relay it asked for; keeps its place (3440). */
    volatile LONG     ReqRoute;
    /* ABI 25: THE CARD'S POSITION IN SCREEN COORDINATES, so the crop stops being a race.
     *
     * ReqCropX/Y are an OFFSET INSIDE THE WINDOW, measured by the agent's card walk at one moment
     * and applied by the broker to whatever the window is at capture time. For a shell surface
     * those two moments disagree: the walk reported "toast card in 666x750 window, insets l=12
     * t=12" while the SAME handle measured 5120x1392 seconds later (2026-10-17, win11de-cold) -
     * the shell's CoreWindow is full-screen while it composes and card-sized once the menu is
     * placed. Cropping at (12,12) in the full-screen state lifts the top-left corner of the
     * screen, which is empty: exactly the QGASLICEBLACK nonblack=0/7371 that has made the Start
     * menu unpaintable.
     *
     * So the agent publishes where the card IS, and the broker derives the offset from the
     * window's LIVE rect when it captures: cropX = ReqCardX - winRect.left. Whatever size the
     * window is at that instant, the crop points at the card. -1 means "not supplied" and the
     * broker uses ReqCropX/Y exactly as before, which is what every non-shell window does. */
    volatile LONG     ReqCardX, ReqCardY;
    /* ABI 26: THE CROP THE BROKER ACTUALLY USED, and the window rect it derived it from.
     *
     * The broker has no log of any kind - it speaks only through this section - so when a cropped
     * copy comes out black there is no way to tell "the crop was wrong" from "the crop was right
     * and the pixels are not there". That is exactly where the Start menu stands on 2026-10-17:
     * the slot captured 10 frames of 5120x1392 and the agent's copy of the card rect was still
     * nonblack=0/7371, and I could not say which step lost them. Guessing at that is what this
     * whole day went wrong doing.
     *
     * So the broker publishes what it used. Written on every publish that crops; read by
     * guest/wgcbroker-peek.ps1 beside the request. CropUsedX/Y are in the captured texture's
     * frame; CropWinX/Y are the window rect's origin at that instant, so a reader can check the
     * arithmetic rather than trust it. */
    volatile LONG     CropUsedX, CropUsedY;
    volatile LONG     CropWinX, CropWinY;
    /* ABI 27: ARE ReqCardX/Y MEANINGFUL? 1 = yes, 0 = no.
     *
     * A zero-initialised slot cannot be told from a card at (0,0), and the first version of this
     * tested ReqCardX >= 0. The slot's zeros passed that test, the broker derived 0 - 0 = (0,0),
     * and it OVERRODE the agent's own reqcrop=12,12 to publish the empty top-left corner of a
     * 5120x1392 window - which is the nonblack=0/7371 the Start menu was still producing after the
     * "fix". Caught only because the broker had just been taught to report the crop it used:
     * CROP used=0,0 winorigin=0,0 reqcard=0,0 reqcrop=12,12.
     *
     * A sentinel inside the value's own range is not a sentinel. This flag is written by the agent
     * on every registration AND every retarget, next to the fields it describes. */
    volatile LONG     ReqCardValid;
    /* ABI 28: IS CropWinX/Y MEANINGFUL? 1 = yes, 0 = the broker had no window rect. THE SAME
     * AMBIGUITY AS ReqCardValid, IN THE OTHER DIRECTION, and it produced the same false alarm.
     *
     * The broker publishes the origin only when GetWindowRect succeeds, so on failure CropWinX/Y
     * keep what they held - zero on a fresh slot - and the agent's QGACROPFALLBACK, which computes
     * `ReqCard - CropWin`, subtracts ZERO from a SCREEN coordinate and reports a healthy window as
     * mis-cropped. MEASURED on acceptance run 40037710497 (4.3.37.1073), the ONLY breach in each of
     * win11-clean, win11-reinstall and win11-appvm: `QGACROPFALLBACK hwnd=0x310232 card outside the
     * capture - cropped 0,0 not 4740,1251`, where 4740,1251 is the card's real screen position -
     * the same log carries `rect=4724,1221,5120,1392` for that window and `insets l=16 t=30`, and
     * 4724+16, 1221+30 is exactly it. GetWindowRect was failing for that handle: the agent logged
     * `inverted DWM bounds (5120,1392)-(5120,1392) and no usable GetWindowRect - rejecting` three
     * times in the second before. Jev on the cause: cropwin-unwritten 1.00, card-position-garbage
     * 0.00; on this fix shape, abi-validity-flag 0.86 against a sentinel at 0.11.
     *
     * THE COMMENT IN THE BROKER CLAIMED THIS WAS ALREADY CLOSED - "the origin is published
     * unconditionally and the compare becomes exact in both directions" (2026-10-18). It was not
     * unconditional: it was conditional on haveRect. That fix closed the "the derivation never ran"
     * path and left "GetWindowRect failed" open, with the identical symptom. Written by the BROKER
     * on every publish that reaches the crop step. */
    volatile LONG     CropWinValid;
} WGCBRK_SLOT;

/* ABI 18: WGCBRK_HEADER.BrokerStage codes (bits 8+; the low 8 bits are the slot). */
#define WGCBRK_STG_LOOP       0u   /* main loop, between the stages below */
#define WGCBRK_STG_RECONCILE  1u   /* Reconcile, between its calls */
#define WGCBRK_STG_OPEN       2u   /* OpenChannel: CreateForWindow, frame pool, StartCapture */
#define WGCBRK_STG_CLOSE      3u   /* CloseChannel teardown: revoke, session.Close, pool.Close */
/* 4-7 were the relay's and the PrintWindow route's stages, retired at ABI 29 (ADR-capture 36); the numbers are not reused. */
#define WGCBRK_STG_REPUBLISH  8u   /* RepublishRetained */

/* HOW LONG A DECLARED STAGE MAY LAST BEFORE IT IS A HANG (ABI 19, 2026-10-07; findings/issues.md P1, Jev 0.97).
 * The agent's R1 deadline asks two things - the request unacknowledged, and no progress - and a broker inside ONE long
 * call satisfies both, because BrokerProgress only moves when a stage is entered or left. Measured 2026-10-01: a WGC
 * channel open for a window that had just appeared took longer than WGCBRK_ACK_DEADLINE_MS and the agent terminated a
 * broker that was working. So the deadline is per STAGE: a stage the broker has DECLARED gets the budget below, and
 * only a stage that has not changed for that long is a hang. This is not a timeout standing in for an observable
 * condition - the observable conditions (the ack, the progress counter) are still required; this is the third exit,
 * and its expiry is the loud QGABROKERHUNG that names the stage.
 * WGCBRK_STG_OPEN and _CLOSE call into WGC, which can block on a window that is itself busy; the rest are ours. */
static __inline unsigned WgcbrkStageBudgetMs(unsigned stageCode)
{
    switch (stageCode)
    {
    case WGCBRK_STG_OPEN:
    case WGCBRK_STG_CLOSE:
        return 8000u;    /* a capture session on a window that has just appeared */
    default:
        return WGCBRK_ACK_DEADLINE_MS;   /* our own bookkeeping: the ordinary deadline */
    }
}
#define WGCBRK_STAGE_CODE(stg) ((unsigned)(((ULONG)(stg)) >> 8))
#define WGCBRK_STAGE_SLOT(stg) ((int)(((ULONG)(stg)) & 0xFFu))
#define WGCBRK_STG_SIGN       9u   /* FlushPendingSignatures */
#define WGCBRK_STG_CLOSE_LOCK 10u  /* CloseChannel: acquiring the slot lock a FrameArrived handler may hold */
#define WGCBRK_STG_CLOSE_REAP 11u  /* CloseChannel: handing the WGC teardown to its own thread (never waits) */

#define WGCBRK_HDR(base)       ((WGCBRK_HEADER*)(base))
#define WGCBRK_SLOTS(base)     ((WGCBRK_SLOT*)((BYTE*)(base)+sizeof(WGCBRK_HEADER)))
#define WGCBRK_ARENA(base,off) ((BYTE*)(base)+(off))
#define WGCBRK_HEADER_BYTES    (sizeof(WGCBRK_HEADER)+WGCBRK_MAX_SLOTS*sizeof(WGCBRK_SLOT))
