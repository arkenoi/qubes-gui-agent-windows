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
#define WGCBRK_ABI_VERSION  12u  /* 12: PubColours - a signature of the frame actually delivered */
#define WGCBRK_MAX_SLOTS    32
/* Longest a PrintWindow-captured window may go unrendered when no damage poke arrives. A bound
 * on staleness, not a polling rate: with a working poke path it should almost never fire. */
#define WGCBRK_POKE_SAFETY_MS 1000
/* Minimum gap between two PrintWindow renders of the same window, however many pokes arrive.
 * Input can poke at input rate, and PrintWindow costs p50 31.7 ms on a large window rendered on
 * the application's own UI thread, so without this a mouse moving across a window would restore
 * exactly the fixed-tick cost this design exists to avoid. 100 ms caps it at ~10 renders/s while
 * interacting and 0 when idle. It is a CEILING ON COST, not a refresh rate. */
#define WGCBRK_POKE_MIN_INTERVAL_MS 100
/* Ceiling for the adaptive backoff. A window whose renders keep changing nothing is asked less
 * and less often, up to this; any changed render or any input poke snaps it back to the floor. */
#define WGCBRK_POKE_BACKOFF_MAX_MS 8000
/* How long a VISIBLE window's WGC feed may stay silent before we conclude WGC is not serving it
 * and re-route to PrintWindow. Keyed on the symptom rather than on window structure. Liberal on
 * purpose: a wrong re-route now costs a single render before the adaptive backoff decays it. */
#define WGCBRK_WGC_QUIET_MS 2000
/* After a quiet re-route turns out to have been wrong - the window was merely static - do not
 * re-test it for this long. Without the hysteresis a static window is re-tested every quiet
 * period for ever, which is a churn loop costing a render every couple of seconds. */
#define WGCBRK_WGC_PROBE_BACKOFF_MS 30000
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
    /* Input pokes dropped because the watched-window lock was busy. The input path acquires that
     * lock with TryEnterCriticalSection and never waits, so dom0's input is never delayed by a
     * poke - but a dropped poke means that repaint waits for the staleness bound instead. This
     * project's rule is that a fallback firing is logged loudly and diagnosed, never silent, so
     * count it: a PokeLockMiss that climbs alongside SafetyPolls says the contention is real and
     * the design needs a different signal, not that it is working. Layout-compatible - it takes
     * the place of a pad, so no ABI bump is needed and no reader is invalidated. */
    volatile LONG      PokeLockMiss;
    /* ABI 8: the relay capability AS LATCHED, published so it is visible from outside. A capability
     * that fails to latch must be LOUD - a silent off is the forbidden silent downgrade, and this one
     * WAS silent: the first R1 build used VerifyVersionInfo, which the compatibility-manifest shim
     * lies to, so the relay never engaged on a 26200 guest and the only symptom was relayOk=0 with
     * relayFail=0. Taken from the reserved padding so the header size stays 128. */
    volatile LONG      RelayCapable;   /* 1 = the broker latched the relay ON at startup */
    volatile LONG      RelayOsBuild;   /* the build it decided from, via RtlGetVersion */
    BYTE               _pad2[48];
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
    volatile LONG     FramesDropSize;    /* dropped: ContentSize != pool size */
    volatile LONG     RecreateOk;        /* pool.Recreate() succeeded */
    volatile LONG     RecreateFail;      /* pool.Recreate() threw - the stale-pool trap */
    volatile LONG     LastContentW;      /* ContentSize of the most recent arrival */
    volatile LONG     LastContentH;
    volatile LONG     PoolW;             /* the pool size it was compared against */
    volatile LONG     PoolH;
    volatile LONG     _padAbi6;
    /* ABI 7. DAMAGE-DRIVEN POLLING for the PrintWindow path.
     *
     * WHY. A window whose content is rendered by a CROSS-PROCESS CHILD has an empty surface of its
     * own, so a WGC session on it delivers nothing - measured 2026-09-25: PrintWindow(flags=0) on
     * the Settings frame returned ONE distinct colour over 1216x941 while PW_RENDERFULLCONTENT
     * returned 191 and the whole page. Those windows must therefore be captured with PrintWindow.
     * But PrintWindow is expensive and SYNCHRONOUS ON THE CAPTURED APPLICATION'S UI THREAD:
     * measured p50 31.7 ms (p90 38.9, max 53.3, n=40) on that window, and the broker's polled path
     * runs at ~13.5/s, i.e. about 43% of one core, for ONE window, for ever. Jev on those numbers:
     * unacceptable-as-is 1.00, damage-driven-polling 0.87.
     *
     * So the agent, which already computes per-window damage by intersecting the desktop's dirty
     * rects with each window rect, bumps PokeSeq when this window's pixels actually changed. The
     * broker renders only when PokeSeq != PokeAck, then stores the value it serviced. A window
     * nobody is touching costs nothing.
     *
     * SafetyPolls bounds staleness if the damage signal is ever wrong: the broker still polls
     * after WGCBRK_POKE_SAFETY_MS of silence. That is a BOUND, not a fallback - it is counted, and
     * a SafetyPolls that climbs while PollsSkipped climbs means the poke path is not working and
     * must be diagnosed, not tolerated. */
    volatile LONG     PokeSeq;           /* agent: bumped when this window's pixels changed */
    volatile LONG     PokeAck;           /* broker: the PokeSeq value it last rendered for */
    volatile LONG     PollsServiced;     /* renders actually performed */
    volatile LONG     PollsSkipped;      /* ticks where nothing had changed, so nothing was done */
    volatile LONG     SafetyPolls;       /* renders forced by the staleness bound, not by a poke */
    /* Times this slot was re-routed from WGC to PrintWindow because the cross-process content
     * child appeared AFTER the session opened. The frame exists before the app creates its
     * content, so the first routing decision is usually taken too early; without a re-check the
     * window stays on a session that will never deliver. A previous attempt at this fix failed
     * for exactly that reason, so the recovery is counted rather than silent. */
    volatile LONG     Reroutes;
    /* Current adaptive interval, ms. Visible so the gate can be judged from the rig instead of
     * assumed: an idle window should climb to the ceiling, and a self-updating one should not. */
    volatile LONG     BackoffMs;
    /* Re-routes triggered by the BEHAVIOURAL test (a visible window whose WGC feed never spoke),
     * as opposed to the structural fast path. Separated so the two can be told apart on the rig:
     * if QuietReroutes carries the load, the structural test is not earning its place. */
    volatile LONG     QuietReroutes;
    /* Quiet re-routes the first PrintWindow render DISPROVED - the card matched what WGC had
     * already published, so the window was static rather than broken and was handed back. This
     * is the false-positive rate of the behavioural detector, measured rather than argued. */
    volatile LONG     ProbeBounces;
    /* ABI 8: WHICH WRITER IS FEEDING THIS SLOT. TickPw already said "polled PrintWindow or not", but
     * with the relay there are THREE answers and collapsing them loses the one that matters: a slot
     * fed by a thumbnail relay is ARRIVAL-DRIVEN like a WGC slot, not polled like a PrintWindow one,
     * and a ledger that cannot tell them apart cannot show the fallback going away. */
    volatile LONG     Route;        /* WGCBRK_ROUTE_* */
    volatile LONG     RelayOk;      /* thumbnail registrations that produced a capturable dest */
    volatile LONG     RelayFail;    /* relay attempts that failed; the slot then falls back */
    volatile UINT64   RelayDest;    /* the destination HWND we own, for cross-checking the agent's
                                     * broker-owned-window exclusion from outside */
    /* ABI 9: DID THE EVENT FIRE, OR DID WE THROW IT AWAY? FramesArrived is incremented AFTER the
     * arrival handler's re-validation guard, so a guard that starts rejecting looks exactly like a
     * feed that stopped - and the quiet detector then demotes a healthy channel. That is not
     * hypothetical: five relay slots delivered 83/25/29/3/2 frames and were all demoted, and Jev put
     * the guard at 0.70 as the mechanism against every other candidate below 0.19. These two count
     * the raw invocation and the rejection, so the two cases can never be confused again. */
    volatile LONG     ArrivalRaw;       /* handler entered, before any guard */
    volatile LONG     ArrivalRejected;  /* guard returned early: pool null or a different sender */
    /* ABI 10: A POKE IS NOT EVIDENCE THE SOURCE CHANGED. The agent derives damage from DESKTOP dirty
     * rects, so a poke fires whenever anything repaints inside a window's screen rectangle - a
     * passing cursor, an overlapping window - whether or not that window's own content moved. The
     * quiet detector demoted on "a poke arrived and no frame followed", which a healthy relay on a
     * STATIC window satisfies for ever: measured 2026-09-26 on win11de-led2, all four rogue classes
     * sat on route 2 with relayOk=1, frames delivered (56/28/6/3) and then PokeSeq 11772 against
     * FramesArrived 56. Jev: root_cause=quiet-rule-demotes-healthy-static-relays 1.00, and the fix
     * is to require evidence the SOURCE changed (0.76). This counts the times that evidence was
     * absent and the demotion was therefore declined - if it never advances, the new rule is not
     * firing and any pass is unproven. */
    volatile LONG     RelayStaticHolds;
    /* ABI 11: WHY did the source test decide what it decided? RelayStaticHolds alone could not say.
     * Measured 2026-09-26 on win11de-led3: three rogue classes held arrival-driven routes with the
     * hold counter CLIMBING (37->48), while both ULW-layered slots FROZE at 1 and 4 and fell to the
     * polled path - so the test had returned SAME a few times and then something else. Two
     * mechanisms fit that equally (PrintWindow succeeding but returning unstable pixels on a
     * per-pixel-alpha window, versus PrintWindow failing outright, which the code maps to CHANGED by
     * design) and Jev rated the evidence `insufficient-evidence` 0.85 to separate them, naming
     * exactly this instrumentation as the next measurement (0.83). One counter per outcome, so the
     * two can never again be confused. */
    volatile LONG     RelaySrcChanged;      /* measured: the source hash differed -> demotion allowed */
    volatile LONG     RelaySrcUnmeasured;   /* throttled: no test ran, neither demote nor hold */
    volatile LONG     RelayPwFail;          /* the test could not render the source at all */
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
} WGCBRK_SLOT;

/* Route values. Deliberately explicit rather than a bool pair: the whole point of the relay is to
 * move slots OFF route 2, and "how many slots are still on 2" must be a single readable number. */
#define WGCBRK_ROUTE_WGC    0u   /* WGC on the window itself - arrival-driven */
#define WGCBRK_ROUTE_RELAY  1u   /* WGC on a destination carrying a DWM thumbnail - arrival-driven */
#define WGCBRK_ROUTE_PW     2u   /* polled PrintWindow - the fallback this relay exists to retire */

#define WGCBRK_HDR(base)       ((WGCBRK_HEADER*)(base))
#define WGCBRK_SLOTS(base)     ((WGCBRK_SLOT*)((BYTE*)(base)+sizeof(WGCBRK_HEADER)))
#define WGCBRK_ARENA(base,off) ((BYTE*)(base)+(off))
#define WGCBRK_HEADER_BYTES    (sizeof(WGCBRK_HEADER)+WGCBRK_MAX_SLOTS*sizeof(WGCBRK_SLOT))
