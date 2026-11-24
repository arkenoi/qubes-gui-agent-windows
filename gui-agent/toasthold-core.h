/*
 * toasthold-core.h - the PURE state machine behind the agent's toast-banner hold (toasthold.c).
 *
 * WHAT IT DECIDES, per banner window and per pass of the tracking loop: is the shell's toast banner
 * window to be SHOWN (mapped in dom0), HELD (kept unmapped for now, re-examined at *Due and on the
 * bridge's verdict event) or SUPPRESSED (kept unmapped while this content is displayed, because the
 * bridge forwards this toast to dom0 - mapping it too would be the owner's double). The decision is
 * taken from inputs the glue gathers: whether the banner's content identity has been read (UIA),
 * whether a bridge record matched it and what that record's verdict is NOW, whether the bridge is
 * alive, and whether the window turned out not to be a toast banner at all.
 *
 * THE OWNER'S REQUIREMENTS AND HOW EACH IS CARRIED HERE (ADR-toasts 13, "Claim, no timer", 2026-11-24):
 *   no doubles   while the bridge is up a banner is mapped ONLY on a `window` record: the classifier's, the
 *                bridge's own word, or the agent's won claim pending -> window (bridge down). verdict=bridge
 *                keeps it unmapped for as long as that content is displayed (it times out unseen in the guest).
 *   never lost   the agent waits for the verdict for as long as the banner lives. At its end (TH_BANNER_END_MS
 *                after first sight) with the record still pending it does NOT map it - mapped that late the shell
 *                dismisses it unseen - it claims the record pending -> bridge (ThCoreClaimWanted; the glue makes
 *                the compare-exchange) and the bridge forwards it, logged as an ERROR. A banner that ends with no
 *                record is not mapped either: the glue remembers its identity and claims the record listed later.
 *                A SUPPRESSED banner is re-read every pass and shown when the bridge turns its record `window`
 *                (the bridge gave it back). Bridge down or dying (a lesser problem, owner): shown, as before.
 *   no flash     the hold starts the moment the banner is first examined, before anything of it is
 *                mapped. A SHOWN banner is not this core's: its new frames are held by the glue until a
 *                read names the cards they show, at most 3 s (ADR-toasts 13, owner-confirmed 2026-11-24:
 *                "A toast is never held back because another toast is on screen or queued. Pre-emption is
 *                removed."). Pre-emption - unmapping or gating a shown banner because a newer record
 *                existed - is gone from here with its input, state, events and reason.
 *
 * PURE: no Windows calls, no allocation, no I/O - the same header compiles with gcc for the offline
 * suite (toasthold_test.c), which drives every transition.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts once it has been seen to FAIL):
 *   TOASTHOLD_DEFECT_NOBOUND               a hold never fails open (the deadline is ignored)
 *   TOASTHOLD_DEFECT_FAILCLOSED            doubt resolves to SUPPRESS instead of SHOW
 *   TOASTHOLD_DEFECT_SUPPRESS_FINAL        a suppressed banner never re-reads its record: the bridge's later
 *                                          `window`, and its death before `forwarded`, are ignored (a LOSS)
 *   TOASTHOLD_DEFECT_NORECLAIM             a banner whose identity merely completed cannot re-claim the record
 *                                          it consumed on the first (partial) read -> 3 s fail-open -> double
 *   TOASTHOLD_DEFECT_NOIDENT_IGNORES_BRIDGE the no-identity hold waits out its bound with the bridge down
 *   TOASTHOLD_DEFECT_NOFORWARDBOUND        a suppression awaiting dom0's ack never reopens: the bridge's slow
 *                                          failure paths correct the record after the banner is gone (a LOSS)
 *   TOASTHOLD_DEFECT_NOCARD_UNPACED        the second card-less read counts whatever its spacing: a real banner
 *                                          mid-grow is classed not-a-banner within milliseconds (doubles for good)
 *   TOASTHOLD_DEFECT_SIZE60                the old absolute 60 % ceiling: 573 px banners at 768/900 px screens or
 *                                          125 %+ DPI are never held
 *   TOASTHOLD_DEFECT_DEADRECORDS           a dead bridge's `bridge`/`pending` records stay authoritative: a toast
 *                                          nobody will forward stays suppressed (a LOSS)
 *   TOASTHOLD_DEFECT_NOBACKOFF             a refused identity request is retried without back-off (a spin on a
 *                                          full worker queue)
 *   TOASTHOLD_DEFECT_RECLAIM_ANY           the own consumed record is re-claimable for ANY new content in the
 *                                          window, not only for a reading that completes the old one: a new
 *                                          same-title toast inherits another toast's verdict (review N5)
 */
#ifndef QWT_TOASTHOLD_CORE_H
#define QWT_TOASTHOLD_CORE_H

#include "toastident.h"

#ifdef __cplusplus
extern "C" {
#endif

/* While held with NO record (the bridge has not listed the toast yet, or the identity read saw a
 * half-built card), re-read the identity and re-match this often - up to the banner's end (TH_BANNER_END_MS). */
#define TH_HOLD_RECHECK_MS     500
/* A window whose first read found no toast card gets ONE more look this long after it: a banner's card
 * is built within the collapse-and-grow animation (~300 ms measured), a Quick Settings / volume flyout
 * never has one. Two card-less reads = not a banner: shown at once, never held for the 3 s bound. */
#define TH_NOCARD_RETRY_MS     250
/* A record older than this can no longer be the toast of a banner appearing now (the shell's queue
 * drains far faster); the glue stops considering it. */
#define TH_RECORD_TTL_MS    120000
/* Records the agent has tied to a banner, by sequence. Toasts are sparse; 64 outlasts the TTL. */
#define TH_CONSUMED_SLOTS       64
/* A SUPPRESSED banner whose record has not turned `forwarded` (dom0's ack) within this long after the
 * suppression is shown after all, loudly: the bridge's own slow failure paths (its 15 s ack timeout, a
 * reconnect before the next listing, the rejection cap) correct the record only after a 5 s banner is
 * gone, and a toast nobody forwards must not vanish. dom0's ack normally arrives within the forward's
 * round trip: MEASURED from the captured bridge logs (FWD_RTT ok=1, 85 forwards, 2026-10-06): min 1 ms,
 * p50 9 ms, p90 17 ms, max 24 ms - so 3 s is more than a hundred times the worst measured ack, and the
 * same latency the owner accepted for a first toast's hold. Armed only while a suppression awaits its
 * ack (no timer at rest). */
#define TH_FORWARD_BOUND_MS   3000
/* A decided banner whose last read FAILED re-reads on this many deadlines at most before it waits for
 * its next event: a bounded failure-state retry (the banner's life bounds it too), never a timer at rest. */
#define TH_READ_RETRY_MAX        6
/* A surface this large in EITHER dimension is not a banner: the Notification Center and the clock flyout
 * are full height, a snap/notification bar is full width. RELATIVE, not absolute (a 60 % cut in physical
 * pixels excluded the measured 573 px banners at 1366x768 / 1600x900 / 125 %+ DPI - review N3). */
#define TH_BANNER_FULL_PERCENT  90

typedef enum _TH_DECISION
{
    ThDecShow = 0,       /* map it (or keep it mapped) */
    ThDecHold = 1,       /* keep it unmapped; re-examine at Due and on the verdict event */
    ThDecSuppress = 2    /* keep it unmapped for this content: forwarded to dom0 */
} TH_DECISION;

typedef enum _TH_EVENT   /* what changed in THIS call, for the glue to log (none = nothing new) */
{
    ThEvNone = 0,
    ThEvHoldStart,       /* a hold began (a new banner, or new content) */
    ThEvSuppress,        /* verdict=bridge: never mapped */
    ThEvShowVerdict,     /* verdict=window: mapped */
    ThEvShowNoBridge,    /* bridge not running: window path at once (by design, ADR-toasts 3) */
    ThEvFailOpen,        /* the bridge died with this banner held or suppressed-unconfirmed: mapped - a FAILURE STATE */
    ThEvContentChanged,  /* the displayed content changed under a decided banner (in-place swap) */
    ThEvShowCorrected,   /* a suppressed banner's record turned `window`: shown after all */
    ThEvNotBanner,       /* the window is not a toast banner (no card after two reads): shown, never held again */
    ThEvAckConfirmed,    /* dom0 acknowledged the forward: the suppression stands - reported for its TIMING */
    ThEvBannerEnd,       /* the banner's end with nothing to claim (no identity, or no record): NOT mapped */
    ThEvForwardLate      /* suppressed with no dom0 ack within TH_FORWARD_BOUND_MS: said once, NOT mapped */
} TH_EVENT;

typedef enum _TH_REASON
{
    ThReasonNone = 0,
    ThReasonNoIdentity,     /* the UIA read never yielded a title within the bound */
    ThReasonNoRecord,       /* the bridge never listed a matching notification within the bound */
    ThReasonVerdictPending, /* a record matched but its verdict never arrived within the bound */
    ThReasonBridgeDown,     /* no bridge running when the banner appeared */
    ThReasonBridgeExited,   /* the bridge died while this banner was held or suppressed-unconfirmed */
    ThReasonCorrected,      /* the bridge turned a suppressed banner's record to window */
    ThReasonForwardUnconfirmed /* suppressed, but dom0 never acknowledged the forward within TH_FORWARD_BOUND_MS */
} TH_REASON;

typedef enum _TH_PHASE { ThPhaseIdle = 0, ThPhaseHolding = 1, ThPhaseDecided = 2 } TH_PHASE;

typedef struct _TH_CORE          /* per banner window; zero-initialised = idle */
{
    TH_PHASE    Phase;
    UINT64      Ident;           /* Combined hash of the content this hold/decision is about; 0 = not known */
    TH_DECISION Decision;        /* valid while Phase == Decided */
    ULONGLONG   HoldSince;       /* tick the current hold started (the banner's first examination) */
    ULONGLONG   Due;             /* fail-open deadline of the current hold; 0 = none */
    LONG        RecordSeq;       /* the matched record (0 = none), re-read every pass */
    UINT32      NotifId;         /* its notification id (logs) */
    int         FailedOpen;      /* the current SHOW came from a fail-open */
    int         NotBanner;       /* not a toast banner: SHOW for good, nothing is ever held again */
    ULONGLONG   SuppressSince;   /* tick the current SUPPRESS was decided: the forward bound counts from here */
    int         AckSeen;         /* dom0's ack has been reported once for THIS suppression (ThEvAckConfirmed) */
    int         Ended;           /* the banner reached its end unshown with no record: a record listed later is adopted */
    int         LateSaid;        /* ThEvForwardLate reported for this suppression */
} TH_CORE;

typedef struct _TH_INPUT         /* gathered by the glue for one pass */
{
    int       IdentKnown;        /* a completed identity read exists for this window (a read in flight does NOT
                                    make it unknown: the last reading stands until the new one lands) */
    UINT64    Ident;             /* its Combined hash (0 = the read found a card but no title: not known) */
    int       BridgeUp;          /* the agent holds the bridge's validated process handle */
    int       MatchFound;        /* a record is tied to this content: the one reserved earlier (re-read), or a new
                                    match (FULL, or the unique PARTIAL) */
    LONG      MatchSeq;
    UINT32    MatchNotifId;
    LONG      MatchVerdict;      /* TH_VERDICT_PENDING / BRIDGE / WINDOW / FORWARDED, as of this pass - after the
                                    glue's claim (ThCoreClaimWanted), so a lost claim reads the winner's verdict */
    int       NotBanner;         /* the glue found no toast card twice: not a banner */
} TH_INPUT;

typedef struct _TH_OUTPUT
{
    TH_DECISION Decision;
    ULONGLONG   Due;             /* 0 = no deadline armed for this window */
    TH_EVENT    Event;
    TH_REASON   Reason;
    ULONGLONG   HeldMs;          /* now - HoldSince (0 when not holding) */
    LONG        ReleaseSeq;      /* a PENDING record reserved for content that turned out to be something
                                    else: the glue un-consumes it so that toast's own banner can still match */
} TH_OUTPUT;

/* ---- the consumed set (records already tied to a banner) ------------------------------------ */

typedef struct _TH_CONSUMED { LONG Seq[TH_CONSUMED_SLOTS]; int Next; } TH_CONSUMED;

TI_INLINE int ThConsumedHas(const TH_CONSUMED* c, LONG seq)
{
    int i;
    if (seq == 0) return 0;
    for (i = 0; i < TH_CONSUMED_SLOTS; i++) if (c->Seq[i] == seq) return 1;
    return 0;
}
TI_INLINE void ThConsumedAdd(TH_CONSUMED* c, LONG seq)
{
    if (seq == 0 || ThConsumedHas(c, seq)) return;
    c->Seq[c->Next] = seq;
    c->Next = (c->Next + 1) % TH_CONSUMED_SLOTS;
}
TI_INLINE void ThConsumedRemove(TH_CONSUMED* c, LONG seq)
{
    int i;
    for (i = 0; i < TH_CONSUMED_SLOTS; i++) if (c->Seq[i] == seq) c->Seq[i] = 0;
}

/* Does the NEW reading of a banner merely COMPLETE the OLD one - the same sender and title, and the old
 * message empty or a prefix of the new one under the shared normalization? Then the banner may re-claim
 * the record it consumed on the old reading (a half-built card on the first read). Anything else is NEW
 * content swapped into the window: a different toast, which must match its own record or fail open -
 * inheriting the old record's verdict would route it on another toast's answer (review N5). */
TI_INLINE int ThCoreReadingExtends(const TOAST_IDENT* oldId, const WCHAR* oldMsg, size_t oldLen,
                                   const TOAST_IDENT* newId, const WCHAR* newMsg, size_t newLen)
{
#ifdef TOASTHOLD_DEFECT_RECLAIM_ANY
    (void)oldId; (void)oldMsg; (void)oldLen; (void)newId; (void)newMsg; (void)newLen;
    return 1;   /* DEFECT: the own record is re-claimable for ANY new content */
#else
    size_t i;
    if (!oldId || !newId) return 0;
    if (oldId->Sender != newId->Sender) return 0;
    if (oldId->Title == 0 || oldId->Title != newId->Title) return 0;
    if (oldLen == 0) return 1;
    if (!oldMsg || !newMsg || newLen < oldLen) return 0;
    for (i = 0; i < oldLen; i++) if (oldMsg[i] != newMsg[i]) return 0;
    return 1;
#endif
}

/* The candidate set for one banner from a snapshot of the ring: unconsumed records within their TTL -
 * plus the record THIS banner already consumed (`ownSeq`; the glue passes 0 unless the new reading
 * EXTENDS the one that claimed it, ThCoreReadingExtends), so content that merely COMPLETES (a first
 * read of a half-built card took the unique partial match; the full read changes the identity) can
 * re-claim it instead of finding "no record" and failing open at the bound (review #5). A record some
 * OTHER banner consumed is never a candidate: it was shown or suppressed already. */
TI_INLINE int ThCoreCandidates(const TH_IPC_RECORD* recs, int n, const TH_CONSUMED* cons, LONG ownSeq,
                               ULONGLONG now, TI_CANDIDATE* out)
{
    int i;
    for (i = 0; i < n; i++)
    {
        const ULONGLONG arrival = recs[i].ArrivalTick > now ? now : recs[i].ArrivalTick;
        int consumed = ThConsumedHas(cons, recs[i].Seq);
#ifdef TOASTHOLD_DEFECT_NORECLAIM
        int own = 0; (void)ownSeq;
#else
        int own = (ownSeq != 0 && recs[i].Seq == ownSeq);
#endif
        out[i].Ident = recs[i].Ident;
        out[i].ArrivalTick = arrival;
        out[i].Seq = recs[i].Seq;
        out[i].Eligible = recs[i].Seq != 0 && (!consumed || own) && (now - arrival) < TH_RECORD_TTL_MS;
    }
    return n;
}

/* ---- small pure rules the glue applies (each with its knob and its test) ---------------------- */

/* Is this card-less read the counted SECOND look? Only when it was requested TH_NOCARD_RETRY_MS or later
 * after the first card-less read's request: the first-sight read and a LOCATIONCHANGE re-read are
 * unpaced, and two of them within milliseconds during the grow animation say nothing (review N2). */
TI_INLINE int ThCoreNoCardCounts(ULONGLONG firstNoCardReq, ULONGLONG thisReq)
{
#ifdef TOASTHOLD_DEFECT_NOCARD_UNPACED
    (void)firstNoCardReq; (void)thisReq;
    return 1;
#else
    return thisReq >= firstNoCardReq + TH_NOCARD_RETRY_MS;
#endif
}

/* Is a surface of this RAW size too big to be a toast banner? Full height or full width (>= 90 % of
 * the screen) - the Notification Center and the clock flyout - and nothing else: banners measure up to
 * 573 px tall and must pass at 768 px screens and 150 % DPI alike. */
TI_INLINE BOOL ThCoreSizeExcludes(ULONG rawW, ULONG rawH, ULONG screenW, ULONG screenH)
{
#ifdef TOASTHOLD_DEFECT_SIZE60
    return (screenW && rawW * 100 > screenW * 60) || (screenH && rawH * 100 > screenH * 60);
#else
    return (screenW && rawW * 100 >= screenW * TH_BANNER_FULL_PERCENT) ||
           (screenH && rawH * 100 >= screenH * TH_BANNER_FULL_PERCENT);
#endif
}

/* The verdict a record carries once the bridge that wrote it is DEAD: a `bridge`/`pending` it never
 * confirmed is a toast nobody will forward (a relaunched bridge baselines the center as seen), so it
 * reads as `window`; `forwarded` (dom0 acknowledged) and `window` stand. `deadCeiling` is the ring's
 * NextSeq captured at the death (0 = no dead instance); newer records belong to the next instance. */
TI_INLINE LONG ThCoreDeadVerdict(LONG seq, LONG verdict, LONG deadCeiling)
{
#ifdef TOASTHOLD_DEFECT_DEADRECORDS
    (void)seq; (void)deadCeiling;
    return verdict;
#else
    if (deadCeiling != 0 && seq != 0 && seq <= deadCeiling &&
        (verdict == TH_VERDICT_BRIDGE || verdict == TH_VERDICT_PENDING))
        return TH_VERDICT_WINDOW;
    return verdict;
#endif
}

/* How long to wait before the next identity-read attempt after `refused` consecutive refusals by the
 * worker's (8-slot) queue: 250, 500, 1000, then 2000 ms. A deadline anchored at a refused attempt with
 * this delay can never be in the past - the pass that sees it refused arms the next one from NOW. */
TI_INLINE ULONGLONG ThCoreRetryDelayMs(UINT refused)
{
#ifdef TOASTHOLD_DEFECT_NOBACKOFF
    (void)refused;
    return TH_NOCARD_RETRY_MS;
#else
    return refused >= 3 ? 2000ULL : (ULONGLONG)TH_NOCARD_RETRY_MS << refused;
#endif
}

/* ---- the machine ----------------------------------------------------------------------------- */

TI_INLINE void ThCoreStartHold(TH_CORE* c, ULONGLONG now, UINT64 ident)
{
    c->Phase = ThPhaseHolding;
    c->Ident = ident;
    c->HoldSince = now;
    c->Due = now + TH_BANNER_END_MS;
    c->RecordSeq = 0;
    c->NotifId = 0;
    c->FailedOpen = 0;
    c->SuppressSince = 0;
    c->AckSeen = 0;
    c->Ended = 0;
    c->LateSaid = 0;
}

TI_INLINE void ThCoreDecideNow(TH_CORE* c, TH_DECISION d, int failedOpen)
{
    c->Phase = ThPhaseDecided;
    c->Decision = d;
    c->FailedOpen = failedOpen;
    c->Due = 0;
}

TI_INLINE int ThCoreBoundPassed(ULONGLONG due, ULONGLONG now)
{
#ifdef TOASTHOLD_DEFECT_NOBOUND
    (void)due; (void)now;
    return 0;   /* DEFECT: the hold never ends by itself */
#else
    return due != 0 && now >= due;
#endif
}

/* Doubt resolves to SHOW - the window path that shipped (ADR-toasts 3). */
TI_INLINE TH_DECISION ThCoreFailOpenDecision(void)
{
#ifdef TOASTHOLD_DEFECT_FAILCLOSED
    return ThDecSuppress;
#else
    return ThDecShow;
#endif
}

TI_INLINE ULONGLONG ThMinTick(ULONGLONG a, ULONGLONG b) { return (a == 0) ? b : (b == 0) ? a : (a < b ? a : b); }

/* The claim the GLUE must make on the matched record before this pass decides (the core is pure and cannot touch the
 * ring). `in->MatchVerdict` is the ring's own PENDING here. TH_VERDICT_WINDOW: the pass would map without a window verdict
 * (the bridge is down). TH_VERDICT_BRIDGE: the banner reached its end unshown (ADR-toasts 13) - held past
 * TH_BANNER_END_MS, or ended earlier with no record and this record was listed since. TH_VERDICT_NONE: keep waiting. */
TI_INLINE LONG ThCoreClaimWanted(const TH_CORE* c, const TH_INPUT* in, ULONGLONG now)
{
    if (!in->MatchFound || in->MatchVerdict != TH_VERDICT_PENDING) return TH_VERDICT_NONE;
    if (!in->BridgeUp) return TH_VERDICT_WINDOW;
    if (c->Phase == ThPhaseDecided && c->Decision == ThDecSuppress && c->Ended && c->RecordSeq == 0) return TH_VERDICT_BRIDGE;
    if (c->Phase == ThPhaseHolding && ThCoreBoundPassed(c->Due, now)) return TH_VERDICT_BRIDGE;
    return TH_VERDICT_NONE;
}

/* The decision for one pass. `c` persists per banner window; `in` is this pass's view. */
TI_INLINE void ThCoreDecide(TH_CORE* c, const TH_INPUT* in, ULONGLONG now, TH_OUTPUT* out)
{
    out->Event = ThEvNone;
    out->Reason = ThReasonNone;
    out->Due = 0;
    out->HeldMs = 0;
    out->ReleaseSeq = 0;

    /* ---- not a toast banner at all: shown, for good, nothing to hold ---------------------------- */
    if (in->NotBanner || c->NotBanner)
    {
        if (!c->NotBanner)
        {
            if (c->Phase == ThPhaseHolding && c->RecordSeq) out->ReleaseSeq = c->RecordSeq;   /* a reservation nothing uses */
            if (c->HoldSince) out->HeldMs = now - c->HoldSince;
            ThCoreDecideNow(c, ThDecShow, 0);
            c->NotBanner = 1;
            c->RecordSeq = 0;
            out->Event = ThEvNotBanner;
        }
        out->Decision = ThDecShow;
        return;
    }

    /* ---- the identity is not known (no completed read for this window yet) ---------------------- */
    if (!in->IdentKnown || in->Ident == 0)
    {
        if (c->Phase == ThPhaseDecided)
            goto decided;                        /* keep the standing decision */
        if (c->Phase == ThPhaseIdle)
        {
            ThCoreStartHold(c, now, 0);
            out->Event = ThEvHoldStart;
            out->Reason = ThReasonNoIdentity;
        }
        out->HeldMs = now - c->HoldSince;
#ifndef TOASTHOLD_DEFECT_NOIDENT_IGNORES_BRIDGE
        if (!in->BridgeUp)
        {
            /* No bridge: no record can ever name this banner - the window path at once, identity or not. */
            ThCoreDecideNow(c, ThCoreFailOpenDecision(), 0);
            out->Event = ThEvShowNoBridge;
            out->Reason = ThReasonBridgeDown;
            goto decided;
        }
#endif
        if (ThCoreBoundPassed(c->Due, now))
        {
            /* THE BANNER'S END WITH NOTHING TO CLAIM: never mapped (a banner is mapped only on a window record). */
            ThCoreDecideNow(c, ThDecSuppress, 0);
            c->Ended = 1;
            out->Event = ThEvBannerEnd;
            out->Reason = ThReasonNoIdentity;
            goto decided;
        }
        out->Decision = ThDecHold;
        out->Reason = ThReasonNoIdentity;
        out->Due = ThMinTick(c->Due, now + TH_HOLD_RECHECK_MS);
        return;
    }

    /* ---- the identity is known ---------------------------------------------------------------- */
    if (c->Phase == ThPhaseDecided)
    {
        if (c->Ident == in->Ident)
            goto decided;
        /* The same window now shows OTHER content: a banner arrived in place. Start over for it. (A
         * record this banner consumed stays consumed unless the glue re-claimed it for the new reading -
         * then in->MatchSeq names it and the hold below takes it straight back.) */
        ThCoreStartHold(c, now, in->Ident);
        out->Event = ThEvContentChanged;
    }
    else if (c->Phase == ThPhaseIdle)
    {
        ThCoreStartHold(c, now, in->Ident);
        out->Event = ThEvHoldStart;
    }
    else
    {
        /* Holding since the banner appeared; the read has now named (or re-named) the content. A
         * record reserved for the previous reading belongs to nobody - unless this reading re-claimed
         * the very same record (the identity merely completed). */
        if (c->Ident != in->Ident && c->RecordSeq != 0 && !(in->MatchFound && in->MatchSeq == c->RecordSeq))
        {
            out->ReleaseSeq = c->RecordSeq;
            c->RecordSeq = 0;
            c->NotifId = 0;
        }
        c->Ident = in->Ident;
    }
    out->HeldMs = now - c->HoldSince;

    if (in->MatchFound)
    {
        c->RecordSeq = in->MatchSeq;
        c->NotifId = in->MatchNotifId;
        if (TiVerdictSuppresses(in->MatchVerdict))
        {
            ThCoreDecideNow(c, ThDecSuppress, 0);
            c->SuppressSince = now;
            c->AckSeen = 0;        /* a fresh suppression: this one's ack has not been reported */
            out->Event = ThEvSuppress;
            goto decided;
        }
        if (in->MatchVerdict == TH_VERDICT_WINDOW)
        {
            ThCoreDecideNow(c, ThDecShow, 0);
            out->Event = ThEvShowVerdict;
            goto decided;
        }
        /* pending */
        if (!in->BridgeUp)
        {
            ThCoreDecideNow(c, ThCoreFailOpenDecision(), 1);
            out->Event = ThEvFailOpen;
            out->Reason = ThReasonBridgeExited;
            goto decided;
        }
        /* THE AGENT WAITS FOR AS LONG AS THE BANNER LIVES. A pending record is never mapped here: at the banner's end the
         * glue claims it pending -> bridge (ThCoreClaimWanted) and passes what the record then holds, so this is reached
         * past the end only when no claim could be made. */
        out->Decision = ThDecHold;
        out->Reason = ThReasonVerdictPending;
        out->Due = ThCoreBoundPassed(c->Due, now) ? now + TH_HOLD_RECHECK_MS : c->Due;   /* the verdict event wakes us earlier */
        return;
    }

    /* no record for this content */
    if (!in->BridgeUp)
    {
        /* No bridge, no records, no reason to wait: the window path, by design. Not a fail-open of
         * this hold (the bridge being down is reported where it happens), so not ThEvFailOpen. */
        ThCoreDecideNow(c, ThCoreFailOpenDecision(), 0);
        out->Event = ThEvShowNoBridge;
        out->Reason = ThReasonBridgeDown;
        goto decided;
    }
    if (ThCoreBoundPassed(c->Due, now))
    {
        /* THE BANNER'S END WITH NO RECORD: never mapped. The glue remembers its identity; the record listed later is
         * claimed for the bridge (adopted in `decided` below while this window lives). */
        ThCoreDecideNow(c, ThDecSuppress, 0);
        c->Ended = 1;
        out->Event = ThEvBannerEnd;
        out->Reason = ThReasonNoRecord;
        goto decided;
    }
    out->Decision = ThDecHold;
    out->Reason = ThReasonNoRecord;
    out->Due = ThMinTick(c->Due, now + TH_HOLD_RECHECK_MS);   /* re-read + re-match on the tick; the bridge's event wakes us earlier */
    return;

decided:
    if (c->HoldSince)
        out->HeldMs = now - c->HoldSince;

    if (c->Decision == ThDecSuppress)
    {
        if (c->Ended && c->RecordSeq == 0 && in->MatchFound)
        {
            /* A record listed after this banner ended unshown. The glue claimed it for the bridge before this call:
             * MatchVerdict is what it holds now - bridge/forwarded stands, window (the bridge was first) is shown below. */
            c->RecordSeq = in->MatchSeq;
            c->NotifId = in->MatchNotifId;
            if (TiVerdictSuppresses(in->MatchVerdict)) { c->SuppressSince = now; c->AckSeen = 0; out->Event = ThEvSuppress; }
        }
#ifndef TOASTHOLD_DEFECT_SUPPRESS_FINAL
        /* A suppression is not final (nothing lost). THE BRIDGE'S LATER CORRECTION: our record turned
         * `window` - the forward failed for good, or the toast was listed while dom0 was unreachable - so
         * dom0 will not show it; the banner must. THE BRIDGE DIED before dom0 acknowledged the forward
         * (`forwarded`): nobody can tell whether dom0 has it - the banner must. */
        if (in->MatchFound && in->MatchSeq == c->RecordSeq && in->MatchVerdict == TH_VERDICT_WINDOW)
        {
            c->Decision = ThDecShow;
            c->FailedOpen = 0;
            out->Event = ThEvShowCorrected;
            out->Reason = ThReasonCorrected;
        }
        else if (!in->BridgeUp && !(in->MatchFound && in->MatchVerdict == TH_VERDICT_FORWARDED))
        {
            c->Decision = ThCoreFailOpenDecision();
            c->FailedOpen = 1;
            out->Event = ThEvFailOpen;
            out->Reason = ThReasonBridgeExited;
        }
        else if (!(in->MatchFound && in->MatchVerdict == TH_VERDICT_FORWARDED))
        {
            /* Awaiting dom0's ack. NOT shown when it is late (ADR-toasts 13: a banner is mapped only on a window record,
             * and this record is the bridge's - mapping it while the bridge may still forward is a double): said once,
             * loudly, at TH_FORWARD_BOUND_MS. The bridge's own give-up turns the record window and shows it above. */
#ifndef TOASTHOLD_DEFECT_NOFORWARDBOUND
            if (c->SuppressSince != 0 && now >= c->SuppressSince + TH_FORWARD_BOUND_MS)
            {
                if (!c->LateSaid) { c->LateSaid = 1; out->Event = ThEvForwardLate; out->Reason = ThReasonForwardUnconfirmed; }
            }
            else if (c->SuppressSince != 0)
                out->Due = c->SuppressSince + TH_FORWARD_BOUND_MS;
#endif
        }
        else if (!c->AckSeen && c->SuppressSince != 0)
        {
            /* THE ACK IS IN HAND and this suppression stands for good - the only outcome of a hold that
             * was previously SILENT. Reported once, with the interval TH_FORWARD_BOUND_MS was actually
             * racing: from the tick this banner was suppressed to the pass that read `forwarded`. That is
             * the bound's OWN clock, which the bridge's FWDWAIT cannot be (it starts at the verdict, and
             * when the record was created already carrying one it over-reads by the record-match latency -
             * about 0.5 s on the measured double; Jev scored the bridge-side field alone 0.39 for deciding
             * the bound and named this tick the missing measurement at 0.60).
             * Without it the log holds only the FAILURES (QGATOASTHOLDLATE), so there is no distribution of
             * how close a HEALTHY ack runs to 3000 ms - and a bound can only be moved against that. */
            c->AckSeen = 1;   /* latched even when another event owns this pass: the ack IS in hand */
            /* Never CLOBBER this pass's event (the file's idiom). A banner
             * suppressed straight onto an already-forwarded record - an allowlisted toast acked before its
             * banner appeared - reports ThEvSuppress with verdict=forwarded on this very pass, and its wait
             * was zero, so there is nothing for a second line to add. */
            if (out->Event == ThEvNone)
            {
                out->Event = ThEvAckConfirmed;
                out->HeldMs = now - c->SuppressSince;   /* suppress-relative */
            }
        }
#endif
    }

    if (c->Decision == ThDecShow)
    {
        /* A shown banner arms nothing here. Its new frames are held by the glue until a read names the cards they
         * show (ADR-toasts 13, the settle hold); no toast is held because another is queued, so pre-emption is gone. */
        out->Decision = ThDecShow;
        out->Due = 0;
        return;
    }
    out->Decision = ThDecSuppress;   /* out->Due: the forward bound while the ack is awaited, else 0 */
}

#ifdef __cplusplus
}
#endif

#endif /* QWT_TOASTHOLD_CORE_H */
