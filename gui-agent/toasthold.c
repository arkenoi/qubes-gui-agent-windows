/*
 * toasthold - keep a shell toast banner unmapped in dom0 until that toast's bridge verdict is known.
 * See toasthold.h for what this fixes; toasthold-core.h for the state machine (pure, unit-tested);
 * toastident.h for the identity contract shared with the bridge.
 *
 * THREADS. The decision runs on the main-loop thread inside the tracking pass (g_csWatchedWindows
 * held by the caller; this module takes only its own g_ThLock inside it and never calls back into
 * main.c under that lock). The identity READ runs on toastcrop.c's UIA worker thread - every UIA call
 * is a synchronous cross-process RPC into ShellExperienceHost, bounded only by the 500 ms connection
 * and transaction timeouts that worker's automation object carries, and the tracking thread must
 * never wait for one (the 2026-08-12 input stall). The read's result is applied under g_ThLock and the
 * window is queued for a tracking pass, where the decision is taken. The WinEvent hook thread only
 * requests reads for windows this module already tracks.
 *
 * AT REST this module does nothing: no timer, no poll. It wakes the main loop only through the
 * bridge's verdict event (in the wait array), the worker's completed read (a queued window event) and
 * the fail-open deadline of a banner that is currently held - a failure-state deadline, armed only
 * while something is held, disarmed the moment nothing is. A decided banner (shown or suppressed)
 * arms nothing; main.c also takes it out of the crop-before-show re-check sweep (ToastHoldOwned).
 */

#include <windows.h>
#include <strsafe.h>

// NO UI Automation header here, on purpose: in C the SDK's UIAutomationClient.h defines its UIA_*Id /
// AnnotationType_* ids as `const long` objects with EXTERNAL linkage, so a second translation unit
// including it duplicates hundreds of symbols at link time (LNK2005, CI run 37474444216). Every UIA
// call of the agent lives in toastcrop.c; this module receives the card's three raw texts from the
// worker through toastcrop.h's TOAST_CARD_TEXTS contract and applies them (ToastHoldApplyIdentity).
#include "main.h"
#include "toasthold.h"
#include "toastcrop.h"

#include <log.h>
#include <config.h>

#define REG_CONFIG_TOASTHOLD_DISABLE_VALUE L"ToastHoldDisable"


// How many banner windows can be tracked at once. There is ONE banner window per session (measured);
// the slack covers a shell restart (a new HWND) and the ShellExperienceHost flyouts that pass the
// toast classifier until their first two reads say "no card". LRU-evicted; eviction is also explicit
// from RemoveWindow.
#define TH_ENTRIES   8

// Outcome of one identity read (worker thread).
#define TH_READ_NONE     (-1)  // no read has completed yet
#define TH_READ_OK       0     // a title was found (message/sender may be empty)
#define TH_READ_NOCARD   1     // no NormalToastView under the window: a card not built yet, or not a banner
#define TH_READ_NOTITLE  2     // a card, but no Title/TitleText block (tree still populating)
#define TH_READ_UIAFAIL  3     // ElementFromHandle/Find failed (a busy or closing shell host)

typedef struct _TH_ENTRY
{
    HWND      Window;
    LONG      Incarnation;    // this entry's id; a read carries the incarnation it was requested for, and a read
                              // for an evicted-and-re-added window (same HWND, new incarnation) is discarded
    TH_CORE   Core;
    // identity reads
    BOOL      ReadInFlight;
    BOOL      ReadDirty;      // a change arrived while a read was in flight: read again when it lands
    ULONGLONG LastReadReq;    // tick of the last request (0 = never)
    BOOL      IdentValid;     // some completed read yielded a title (the LAST such reading is in Ident)
    TOAST_IDENT Ident;
    WCHAR     MsgNorm[TI_NORM_MAX];   // the latest reading's NORMALIZED message (for ThCoreReadingExtends)
    size_t    MsgLen;
    // The reading that CLAIMED Core.RecordSeq: only a later reading that merely completes it (same sender and
    // title, message extended) may re-claim that record; different content is a new toast (review N5).
    TOAST_IDENT ClaimIdent;
    WCHAR     ClaimMsgNorm[TI_NORM_MAX];
    size_t    ClaimMsgLen;
    int       ReadStatus;     // TH_READ_* of the last completed read
    UINT      ReadsWithoutTitle;
    UINT      ReadFails;      // consecutive completed reads that FAILED (any status but OK); bounds the decided-entry retry
    UINT      NoCardReads;    // card-less reads that COUNT: the first, then one requested >= TH_NOCARD_RETRY_MS later (2 = not a banner)
    ULONGLONG NoCardFirstReq; // request tick of the first card-less read
    ULONGLONG LastReadAttempt;// tick of the last request ATTEMPT, queued or refused (deadlines anchor here, never in the past)
    UINT      ReadQueueFails; // consecutive attempts the worker's queue refused (back-off)
    BOOL      NotBanner;
    BOOL      NoCardLogged;   // one QGATOASTIDENT line per window for an unreadable card
    ULONGLONG Due;            // deadline armed for this window (0 = none)
    ULONGLONG LastUse;
} TH_ENTRY;

static BOOL g_ThLockInit = FALSE;
static CRITICAL_SECTION g_ThLock;
static BOOL g_ThGate = FALSE;        // the resolved bridge gate
static BOOL g_ThKnobOff = FALSE;     // ToastHoldDisable=1
static BOOL g_ThActive = FALSE;      // the decision, once, at init
static TH_IPC_HEADER* g_ThIpc = NULL;
static HANDLE g_ThEvt = NULL;
static volatile LONG g_ThBridgeUp = 0;
// The ring's NextSeq when the bridge last DIED (0 = never): records at or below it belong to a dead instance, and a
// `bridge`/`pending` it never confirmed reads as `window` (ThCoreDeadVerdict) - nobody will forward that toast.
static LONG g_ThDeadSeq = 0;

static TH_ENTRY g_Th[TH_ENTRIES];
static volatile LONG g_ThEntryCount = 0;   // live entries, for the hook thread's lock-free early-out
static volatile LONG g_ThIncarnations = 0;
static TH_CONSUMED g_ThCons;
static ULONGLONG g_ThClock = 0;
static ULONGLONG g_ThNextDue = 0;    // earliest armed deadline over all entries; 0 = nothing armed

static void ThEnsureLock(void)
{
    if (!g_ThLockInit)
    {
        InitializeCriticalSection(&g_ThLock);
        g_ThLockInit = TRUE;
    }
}

// ---- init / gate -------------------------------------------------------------------------------

void ToastHoldAttachIpc(IN TH_IPC_HEADER* ipc, IN HANDLE verdictEvent)
{
    ThEnsureLock();
    g_ThIpc = ipc;
    g_ThEvt = verdictEvent;
}

HANDLE ToastHoldVerdictEvent(void)
{
    return g_ThActive ? g_ThEvt : NULL;
}

void ToastHoldInit(IN BOOL bridgeGate)
{
    WCHAR moduleName[CFG_MODULE_MAX];
    DWORD value = 0;

    ThEnsureLock();
    g_ThGate = bridgeGate;
    if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)) &&
        ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTHOLD_DISABLE_VALUE, &value, NULL))
        g_ThKnobOff = (value != 0);

    const BOOL worker = ToastCropWorkerAvailable();
    const BOOL ipc = (g_ThIpc != NULL && g_ThEvt != NULL);
    g_ThActive = g_ThGate && !g_ThKnobOff && worker && ipc;

    // Logged unconditionally and in one shape: a captured log must state which condition produced
    // it. INERT with the gate ON is the state the owner must never find silently - every bridged
    // toast then shows twice - so that case is an ERROR, not an Info.
    if (g_ThActive)
        LogInfo("QGATOASTHOLD gate: ACTIVE (bridge gate on, ToastHoldDisable=0, UIA worker up, IPC up) - a toast banner "
            L"is mapped only after its verdict: bridge=never, window=at once, no verdict within %u ms=mapped+reported",
            (unsigned)TH_HOLD_BOUND_MS);
    else if (!g_ThGate)
        LogInfo("QGATOASTHOLD gate: inactive (bridge gate off) - toast banners map as before");
    else if (g_ThKnobOff)
        LogWarning("QGATOASTHOLD gate: OFF by ToastHoldDisable=1 - a bridged toast shows TWICE (guest banner + dom0 "
            L"notification); this is the escape hatch, not a configuration");
    else
        LogError("QGATOASTHOLD INERT with the bridge gate ON: uiaWorker=%d ipc=%d - every bridged toast will show "
            L"TWICE on this run. uiaWorker=0 means ToastCropDisable/forced insets or a failed worker thread; ipc=0 "
            L"means the shared section or verdict event could not be created (see QGANOTIFIPC).",
            worker, ipc);
}

BOOL ToastHoldActive(void)
{
    return g_ThActive;
}

// ---- entries ----------------------------------------------------------------------------------

// g_ThLock held.
static TH_ENTRY* ThFindLocked(IN HWND window)
{
    for (int i = 0; i < TH_ENTRIES; i++)
        if (g_Th[i].Window == window)
            return &g_Th[i];
    return NULL;
}

// g_ThLock held. Find or create (LRU victim). A new entry gets a fresh incarnation, so a read still
// in flight for the window's previous life cannot land in it.
static TH_ENTRY* ThGetLocked(IN HWND window)
{
    TH_ENTRY* e = ThFindLocked(window);
    int victim = 0;
    if (e) { e->LastUse = ++g_ThClock; return e; }
    for (int i = 0; i < TH_ENTRIES; i++)
    {
        if (g_Th[i].Window == NULL) { victim = i; break; }
        if (g_Th[i].LastUse < g_Th[victim].LastUse) victim = i;
    }
    e = &g_Th[victim];
    if (e->Window == NULL) InterlockedIncrement(&g_ThEntryCount);   // a reused victim keeps the count
    ZeroMemory(e, sizeof(*e));
    e->Window = window;
    e->Incarnation = InterlockedIncrement(&g_ThIncarnations);
    e->ReadStatus = TH_READ_NONE;
    e->LastUse = ++g_ThClock;
    return e;
}

// g_ThLock held.
static void ThRecomputeDueLocked(void)
{
    ULONGLONG next = 0;
    for (int i = 0; i < TH_ENTRIES; i++)
        if (g_Th[i].Window && g_Th[i].Due != 0 && (next == 0 || g_Th[i].Due < next))
            next = g_Th[i].Due;
    g_ThNextDue = next;
}

// g_ThLock held. Ask the UIA worker for a read of this window's card (coalesced by the worker's
// queue: one pending identity request per window, carrying the newest incarnation). If the worker
// refuses, the hold's own bound is what ends the wait - never a read on this thread.
static void ThRequestReadLocked(IN TH_ENTRY* e, IN ULONGLONG now)
{
    if (e->NotBanner) return;
    if (e->ReadInFlight) { e->ReadDirty = TRUE; return; }
    e->LastReadAttempt = now;
    if (ToastCropRequestIdentity(e->Window, e->Incarnation))
    {
        e->ReadInFlight = TRUE;
        e->ReadDirty = FALSE;
        e->LastReadReq = now;
        e->ReadQueueFails = 0;
    }
    else
        e->ReadQueueFails++;   // the worker's queue is full: the next attempt waits ThCoreRetryDelayMs (review N7)
}

// ---- the records (bridge-written, UNTRUSTED) ------------------------------------------------------

// A consistent copy of every live record. Returns the count.
static int ThSnapshotRecords(OUT TH_IPC_RECORD* recs, IN int cap)
{
    int n = 0;
    if (!g_ThIpc || !ThIpcValid(g_ThIpc)) return 0;
    for (int i = 0; i < TH_IPC_RECORDS && n < cap; i++)
        if (ThIpcRead(g_ThIpc, i, &recs[n]))
            n++;
    return n;
}

static BOOL ThFindBySeq(IN const TH_IPC_RECORD* recs, IN int n, IN LONG seq, OUT TH_IPC_RECORD* out)
{
    for (int i = 0; i < n; i++)
        if (recs[i].Seq == seq) { *out = recs[i]; return TRUE; }
    return FALSE;
}

static const WCHAR* ThReasonName(IN TH_REASON r)
{
    switch (r)
    {
    case ThReasonNoIdentity:     return L"no-identity";
    case ThReasonNoRecord:       return L"no-record";
    case ThReasonVerdictPending: return L"verdict-pending";
    case ThReasonBridgeDown:     return L"bridge-down";
    case ThReasonBridgeExited:   return L"bridge-exited";
    case ThReasonPreempt:        return L"preempt";
    case ThReasonCorrected:      return L"record-turned-window";
    case ThReasonForwardUnconfirmed: return L"forward-unconfirmed";
    default:                     return L"-";
    }
}

static const WCHAR* ThReadStatusName(IN int s)
{
    switch (s)
    {
    case TH_READ_NONE:    return L"none";
    case TH_READ_OK:      return L"ok";
    case TH_READ_NOCARD:  return L"no-card";
    case TH_READ_NOTITLE: return L"no-title";
    default:              return L"uia-failed";
    }
}

static const WCHAR* ThVerdictName(IN LONG v)
{
    switch (v)
    {
    case TH_VERDICT_PENDING:   return L"pending";
    case TH_VERDICT_BRIDGE:    return L"bridge";
    case TH_VERDICT_WINDOW:    return L"window";
    case TH_VERDICT_FORWARDED: return L"forwarded";
    default:                   return L"-";
    }
}

// ---- the decision -----------------------------------------------------------------------------

TH_DECISION ToastHoldDecide(IN const WINDOW_DATA* entry)
{
    if (!g_ThActive || !entry)
        return ThDecShow;

    const ULONGLONG now = GetTickCount64();
    TH_IPC_RECORD recs[TH_IPC_RECORDS];
    const int nrec = ThSnapshotRecords(recs, TH_IPC_RECORDS);   // no lock: the ring has its own seqlock
    TH_INPUT in;
    TH_OUTPUT out;
    TI_MATCH quality = TiMatchNone;
    BOOL logPartial = FALSE;
    TOAST_IDENT identForLog;
    UINT32 notifForLog = 0;
    UINT32 preemptNotif = 0;
    int readStatus = TH_READ_NONE;
    UINT noCardReads = 0;

    ZeroMemory(&in, sizeof(in));
    ZeroMemory(&out, sizeof(out));
    ZeroMemory(&identForLog, sizeof(identForLog));

    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThGetLocked(entry->Handle);

    in.BridgeUp = (g_ThBridgeUp != 0);
    // The LAST completed reading stands while a new read is in flight: an unknown identity on the pass
    // the swap's event triggers would end a pre-emption and map the window while the bridged toast paints
    // (review #1). The new reading, when it lands, re-drives this pass through ContentChanged.
    in.IdentKnown = e->IdentValid;
    in.Ident = e->IdentValid ? e->Ident.Combined : 0;
    in.NotBanner = e->NotBanner;
    if (e->IdentValid) identForLog = e->Ident;
    readStatus = e->ReadStatus;
    noCardReads = e->NoCardReads;

    // First sight of this window: a read is needed before anything can be decided.
    if (e->LastReadReq == 0)
        ThRequestReadLocked(e, now);

    // The record tied to this content, re-read EVERY pass - a decided banner's record can still move
    // (bridge -> window when the forward fails for good; bridge -> forwarded on dom0's ack), and the
    // machine acts on both (review #2). `sameContent`: an unknown identity is taken to be the one we
    // decided on (a read is in flight; the reading that lands says otherwise if it is).
    const BOOL sameContent = !in.IdentKnown || e->Core.Ident == in.Ident;
    const LONG deadSeq = g_ThDeadSeq;   // a dead instance's unconfirmed records read as window (review N6)
    if (!e->NotBanner)
    {
        TH_IPC_RECORD r;
        if (e->Core.RecordSeq != 0 && sameContent && ThFindBySeq(recs, nrec, e->Core.RecordSeq, &r))
        {
            in.MatchFound = 1; in.MatchSeq = r.Seq; in.MatchNotifId = r.NotifId;
            in.MatchVerdict = ThCoreDeadVerdict(r.Seq, r.Verdict, deadSeq);
        }
        else if (in.IdentKnown && (e->Core.RecordSeq == 0 || !sameContent))
        {
            // A new match for this reading. The entry's own consumed record stays a candidate ONLY when this
            // reading merely completes the one that claimed it (same sender and title, message extended -
            // a half-built card on the first read, review #5); different content in the same window is a
            // new toast and must find its own record or fail open (review N5). Other banners' records never
            // are candidates.
            TI_CANDIDATE cand[TH_IPC_RECORDS];
            const LONG ownSeq = (e->Core.RecordSeq != 0 &&
                                 ThCoreReadingExtends(&e->ClaimIdent, e->ClaimMsgNorm, e->ClaimMsgLen,
                                                      &e->Ident, e->MsgNorm, e->MsgLen)) ? e->Core.RecordSeq : 0;
            const int n = ThCoreCandidates(recs, nrec, &g_ThCons, ownSeq, now, cand);
            const int pick = TiSelect(cand, n, &e->Ident, &quality);
            if (pick >= 0)
            {
                in.MatchFound = 1; in.MatchSeq = recs[pick].Seq; in.MatchNotifId = recs[pick].NotifId;
                in.MatchVerdict = ThCoreDeadVerdict(recs[pick].Seq, recs[pick].Verdict, deadSeq);
                ThConsumedAdd(&g_ThCons, recs[pick].Seq);
                logPartial = (quality == TiMatchPartial);
                // The reading this record is claimed on: the yardstick for a later re-claim.
                e->ClaimIdent = e->Ident;
                e->ClaimMsgLen = e->MsgLen;
                memcpy(e->ClaimMsgNorm, e->MsgNorm, e->MsgLen * sizeof(WCHAR));
            }
        }
    }

    // Pre-emption, evaluated on EVERY pass (identity known or not) while the bridge is UP: a bridge-bound or
    // still-pending toast listed AFTER this banner's own toast is queued behind it and may paint into this
    // window in place. A dead bridge's records are nobody's queue (its unconfirmed ones read as window).
    if (!e->NotBanner && in.BridgeUp)
    {
        const LONG baseline = in.MatchFound ? in.MatchSeq : e->Core.RecordSeq;
        for (int i = 0; i < nrec; i++)
        {
            ULONGLONG due = 0;
            TH_IPC_RECORD eff = recs[i];
            if (ThConsumedHas(&g_ThCons, recs[i].Seq)) continue;   // some banner's own record: shown or suppressed already
            eff.Verdict = ThCoreDeadVerdict(recs[i].Seq, recs[i].Verdict, deadSeq);
            if (ThCorePreemptBy(&eff, baseline, now, &due))
            {
                in.PreemptWanted = 1;
                if (in.PreemptDue == 0 || due < in.PreemptDue) in.PreemptDue = due;
                if (preemptNotif == 0) preemptNotif = recs[i].NotifId;
            }
        }
    }

    ThCoreDecide(&e->Core, &in, now, &out);

    if (out.ReleaseSeq)
        ThConsumedRemove(&g_ThCons, out.ReleaseSeq);
    if (in.MatchFound) notifForLog = in.MatchNotifId;
    if (e->Core.NotifId) notifForLog = e->Core.NotifId;

    // Reads, bounded by the hold's own deadlines and never while one is in flight. Every interval is measured
    // from the last ATTEMPT and stretched by ThCoreRetryDelayMs while the worker's queue refuses (review N7),
    // so a deadline derived here is never in the past:
    //   * held with no identity or no record: on the recheck tick (a half-built card, a late listing);
    //   * a first read that found NO card: one more look, requested >= TH_NOCARD_RETRY_MS after the first
    //     card-less read's request - that second one says "not a banner" (a Quick Settings / volume / clock
    //     flyout passes the toast classifier too; a banner mid-grow does not, review N2);
    //   * a last read that FAILED (UIA error, no title, no card on a banner already identified), in ANY phase:
    //     once per recheck interval, at most TH_READ_RETRY_MAX times in a row (review #8) - a decided banner
    //     with a stale reading must not keep it for ever, and must not be polled for ever either.
    BOOL armRetry = FALSE;
    ULONGLONG retryAt = 0;
    if (!e->NotBanner && !e->ReadInFlight && e->LastReadAttempt != 0)
    {
        const ULONGLONG since = now - e->LastReadAttempt;
        const ULONGLONG backoff = ThCoreRetryDelayMs(e->ReadQueueFails);
        const ULONGLONG recheck = backoff > TH_HOLD_RECHECK_MS ? backoff : TH_HOLD_RECHECK_MS;
        const BOOL secondLookDue = (e->ReadStatus == TH_READ_NOCARD && e->NoCardReads < 2 &&
                                    now >= e->NoCardFirstReq + TH_NOCARD_RETRY_MS && since >= backoff);
        const BOOL failedRetryDue = (e->ReadStatus != TH_READ_OK && e->ReadStatus != TH_READ_NONE &&
                                     e->ReadFails < TH_READ_RETRY_MAX && since >= recheck);
        if (out.Decision == ThDecHold &&
            (out.Reason == ThReasonNoIdentity || out.Reason == ThReasonNoRecord) && since >= recheck)
            ThRequestReadLocked(e, now);
        else if (secondLookDue || failedRetryDue)
            ThRequestReadLocked(e, now);
        // Whatever happened above, arm the moment the next attempt becomes due - anchored at the attempt
        // just made (or the refused one), so a refused request waits its back-off instead of spinning.
        if (!e->ReadInFlight && !e->NotBanner)
        {
            const ULONGLONG wait = ThCoreRetryDelayMs(e->ReadQueueFails);
            if (e->ReadStatus == TH_READ_NOCARD && e->NoCardReads < 2)
            {
                retryAt = e->NoCardFirstReq + TH_NOCARD_RETRY_MS;
                if (retryAt < e->LastReadAttempt + wait) retryAt = e->LastReadAttempt + wait;
                armRetry = TRUE;
            }
            else if (e->ReadStatus != TH_READ_OK && e->ReadFails < TH_READ_RETRY_MAX)
            {
                retryAt = e->LastReadAttempt + (wait > TH_HOLD_RECHECK_MS ? wait : TH_HOLD_RECHECK_MS);
                armRetry = TRUE;   // #8: a decided banner's failed reading is retried on a deadline, bounded
            }
            else if (e->ReadQueueFails > 0)
            {
                retryAt = e->LastReadAttempt + wait;
                armRetry = TRUE;
            }
        }
    }

    e->Due = out.Due;
    if (armRetry)
        e->Due = ThMinTick(e->Due, retryAt);
    ThRecomputeDueLocked();
    LeaveCriticalSection(&g_ThLock);

    const DWORD hw = (DWORD)(ULONG_PTR)entry->Handle;
    const unsigned long long hc = (unsigned long long)identForLog.Combined;
    const unsigned long long hs = (unsigned long long)identForLog.Sender;
    const unsigned long long ht = (unsigned long long)identForLog.Title;
    const unsigned long long hm = (unsigned long long)identForLog.Message;
    if (logPartial)
        LogWarning("QGATOASTIDENT hwnd=0x%x PARTIAL match: the banner's message differs from the record's (same sender+title, "
            L"unique candidate) ident=%016llx s=%016llx t=%016llx m=%016llx id=%lu - the bridge's text and the banner's text "
            L"are spelled differently for this toast (compare with the bridge's HOLD line for this id); the match is taken "
            L"because nothing else fits", hw, hc, hs, ht, hm, (ULONG)notifForLog);

    switch (out.Event)
    {
    case ThEvHoldStart:
        LogInfo("QGATOASTHOLD hwnd=0x%x state=hold reason=%s ident=%016llx s=%016llx t=%016llx m=%016llx bridgeUp=%d "
            L"(banner withheld from dom0 until its verdict)",
            hw, ThReasonName(out.Reason == ThReasonNone ? (in.MatchFound ? ThReasonVerdictPending : ThReasonNoRecord) : out.Reason),
            hc, hs, ht, hm, in.BridgeUp);
        break;
    case ThEvSuppress:
        LogInfo("QGATOASTHOLD hwnd=0x%x state=suppress ident=%016llx id=%lu verdict=%s held_ms=%llu (forwarded to dom0, the guest "
            L"banner is never mapped; reopened if the record turns window or the bridge dies before dom0's ack)",
            hw, hc, (ULONG)notifForLog, ThVerdictName(in.MatchVerdict), out.HeldMs);
        break;
    case ThEvShowVerdict:
        LogInfo("QGATOASTHOLD hwnd=0x%x state=show ident=%016llx id=%lu held_ms=%llu (verdict=window: the banner is the toast)",
            hw, hc, (ULONG)notifForLog, out.HeldMs);
        break;
    case ThEvShowCorrected:
        LogWarning("QGATOASTHOLD hwnd=0x%x state=show reason=record-turned-window ident=%016llx id=%lu after %llu ms suppressed: "
            L"the bridge could not forward this toast after all (forward failed for good, or dom0 was unreachable) - the "
            L"banner is shown now, late; if the banner has already timed out in the guest the toast is in its Notification "
            L"Center only", hw, hc, (ULONG)notifForLog, out.HeldMs);
        break;
    case ThEvShowNoBridge:
        LogInfo("QGATOASTHOLD hwnd=0x%x state=show reason=bridge-down ident=%016llx (no bridge running: window path at once, by design)",
            hw, hc);
        break;
    case ThEvNotBanner:
        LogInfo("QGATOASTIDENT hwnd=0x%x not a toast banner (no NormalToastView in %u reads, %ux%u) - shown, never held: a "
            L"ShellExperienceHost flyout that passes the toast classifier",
            hw, noCardReads, entry->Width, entry->Height);
        break;
    case ThEvFailOpen:
        // The anomaly this module exists to make visible: a hold that ended by its bound or by the bridge
        // dying is mapped - possibly as the second copy of a forwarded toast (the owner's double).
        LogWarning("QGATOASTHOLDLATE hwnd=0x%x mapped FAIL-OPEN after %llu ms: reason=%s ident=%016llx s=%016llx t=%016llx "
            L"m=%016llx id=%lu lastRead=%s bridgeUp=%d records=%d - the banner is shown without a verdict; if the bridge "
            L"forwarded this toast it now shows TWICE. reason=no-identity: the card's Title/TitleText block was not "
            L"readable (a build whose banner tree differs?); no-record: the bridge never listed a matching notification "
            L"in time (compare s/t/m with its HOLD lines); verdict-pending: the classifier did not answer in time; "
            L"bridge-exited: the bridge died mid-hold or before dom0 acknowledged the forward; forward-unconfirmed: "
            L"suppressed, but dom0 did not acknowledge the forward within the bound (the bridge's own failure paths "
            L"are slower than a banner's life)",
            hw, out.HeldMs, ThReasonName(out.Reason), hc, hs, ht, hm, (ULONG)notifForLog,
            ThReadStatusName(readStatus), in.BridgeUp, nrec);
        break;
    case ThEvContentChanged:
        LogInfo("QGATOASTHOLD hwnd=0x%x content changed in place: new ident=%016llx s=%016llx t=%016llx m=%016llx - held again "
            L"for its own verdict (the window stays unmapped until then)", hw, hc, hs, ht, hm);
        break;
    case ThEvPreemptStart:
        LogWarning("QGATOASTPREEMPT hwnd=0x%x unmapped: a bridge-bound or still-pending toast (id=%lu) is queued behind the displayed "
            L"banner and would paint into this window in place; the displayed window-path banner loses the rest of its dom0 "
            L"display (it stays in the guest's Notification Center). Bounded: re-mapped if the queued toast resolves to "
            L"window, or at its deadline", hw, (ULONG)preemptNotif);
        break;
    case ThEvPreemptEnd:
        LogInfo("QGATOASTPREEMPT hwnd=0x%x re-mapped after %llu ms: the queued toast resolved to window (or its window passed)",
            hw, out.HeldMs);
        break;
    default:
        break;
    }

    return out.Decision;
}

// Called on the WinEvent HOOK thread for every EVENT_OBJECT_LOCATIONCHANGE / NAMECHANGE of a top-level
// window (main.c's callback, next to CropNoteWindowChanged). Only a window this module already tracks
// is touched - entries are created by the tracking pass, never here - so for every other window this
// is one volatile read and a return. A tracked banner's event means its content MAY be new (a banner
// arriving in place has the same rect: the event is the only signal), so a re-read is queued; a read
// in flight is marked dirty and repeated when it lands, so bursts cost one extra read, never a pile.
void ToastHoldNoteChanged(IN HWND window)
{
    if (!g_ThActive || !window || g_ThEntryCount == 0) return;
    const ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (e)
        ThRequestReadLocked(e, now);
    LeaveCriticalSection(&g_ThLock);
}

void ToastHoldEvict(IN HWND window)
{
    if (!g_ThLockInit || !window) return;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (e)
    {
        // A record reserved for a hold that never resolved belongs to nobody now; a decided record
        // stays consumed (its banner was shown or suppressed).
        if (e->Core.Phase == ThPhaseHolding && e->Core.RecordSeq)
            ThConsumedRemove(&g_ThCons, e->Core.RecordSeq);
        ZeroMemory(e, sizeof(*e));
        InterlockedDecrement(&g_ThEntryCount);
    }
    ThRecomputeDueLocked();
    LeaveCriticalSection(&g_ThLock);
}

void ToastHoldSetBridgeUp(IN BOOL up)
{
    const LONG was = InterlockedExchange(&g_ThBridgeUp, up ? 1 : 0);
    if (!g_ThActive) return;
    if (!up)
    {
        // Everything the dead instance published is now a dead letter unless dom0 acknowledged it: capture
        // the ring position so its `bridge`/`pending` records read as window from here on (review N6). The
        // next instance continues the sequence above this mark.
        LONG ceiling = 0;
        EnterCriticalSection(&g_ThLock);
        if (g_ThIpc && ThIpcValid(g_ThIpc))
        {
            ceiling = TI_LOAD32(&g_ThIpc->NextSeq);
            if (ceiling > g_ThDeadSeq) g_ThDeadSeq = ceiling;
        }
        LeaveCriticalSection(&g_ThLock);
        if (was)
        {
            LogInfo("QGATOASTHOLD bridge down (records <= seq %ld are a dead instance's: unconfirmed bridge/pending read as "
                L"window): every banner is re-examined now - a held one fails open at once, a suppressed one whose forward "
                L"dom0 never acknowledged is shown", ceiling);
            ToastHoldOnSignal();
        }
    }
}

void ToastHoldOnSignal(void)
{
    HWND wake[TH_ENTRIES];
    int n = 0;
    if (!g_ThActive) return;
    EnterCriticalSection(&g_ThLock);
    for (int i = 0; i < TH_ENTRIES; i++)
        if (g_Th[i].Window && !g_Th[i].NotBanner)
            wake[n++] = g_Th[i].Window;
    LeaveCriticalSection(&g_ThLock);
    for (int i = 0; i < n; i++)
        PokeWindowTrackingFor(wake[i]);
}

BOOL ToastHoldTracks(IN HWND window)
{
    BOOL tracks;
    if (!g_ThActive || !window || g_ThEntryCount == 0) return FALSE;
    EnterCriticalSection(&g_ThLock);
    tracks = (ThFindLocked(window) != NULL);
    LeaveCriticalSection(&g_ThLock);
    return tracks;
}

ULONGLONG ToastHoldNextDue(void)
{
    return g_ThActive ? g_ThNextDue : 0;
}

void ToastHoldSweep(void)
{
    HWND wake[TH_ENTRIES];
    int n = 0;
    if (!g_ThActive || g_ThNextDue == 0) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_ThNextDue) return;
    EnterCriticalSection(&g_ThLock);
    for (int i = 0; i < TH_ENTRIES; i++)
    {
        if (g_Th[i].Window && g_Th[i].Due != 0 && now >= g_Th[i].Due)
        {
            // NOT cleared: the pass this poke asks for re-arms Due through ToastHoldDecide - but a pass
            // can leave early (the window not measurable this pass, hidden for a moment) without reaching
            // the decision, and a cleared deadline would then leave a held banner with nothing to wake it
            // (review #14). Moved forward instead, so the worst case is one more look in a recheck.
            g_Th[i].Due = now + TH_HOLD_RECHECK_MS;
            wake[n++] = g_Th[i].Window;
        }
    }
    ThRecomputeDueLocked();
    LeaveCriticalSection(&g_ThLock);
    for (int i = 0; i < n; i++)
        PokeWindowTrackingFor(wake[i]);
}

// ---- the identity read: applied here, performed in toastcrop.c (the one UIA translation unit) -------

// Compile-time: the worker's text buffers are exactly the identity's field width (toastident.h).
typedef char ThAssertCardTextUnits[(TOAST_CARD_TEXT_UNITS == TI_NORM_MAX) ? 1 : -1];

void ToastHoldApplyIdentity(IN HWND window, IN LONG incarnation, IN TOAST_CARD_STATUS cardStatus,
                            IN const TOAST_CARD_TEXTS* texts)
{
    TOAST_IDENT ident;
    int status;
    ZeroMemory(&ident, sizeof(ident));
    switch (cardStatus)
    {
    case ToastCardOk:      status = TH_READ_OK; break;
    case ToastCardNoCard:  status = TH_READ_NOCARD; break;
    case ToastCardNoTitle: status = TH_READ_NOTITLE; break;
    default:               status = TH_READ_UIAFAIL; break;
    }
    WCHAR msgNorm[TI_NORM_MAX];
    size_t msgLen = 0;
    if (status == TH_READ_OK && texts)
    {
        TiIdentFromTexts(texts->Sender, texts->Title, texts->Message, &ident);   // the same normalization the bridge applies
        msgLen = TiNormalize(texts->Message, msgNorm, TI_NORM_MAX);             // kept in clear for the re-claim rule only
    }
    else if (status == TH_READ_OK)
        status = TH_READ_UIAFAIL;
    const ULONGLONG now = GetTickCount64();
    BOOL again = FALSE, logNoCard = FALSE, changed = FALSE, notBanner = FALSE;
    UINT64 before = 0, after = 0;
    UINT withoutTitle = 0;

    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (!e || e->Incarnation != incarnation)
    {
        // Evicted while the read ran, or the window was removed and re-added meanwhile (a new incarnation
        // of this HWND): this reading is about a banner that is gone.
        LeaveCriticalSection(&g_ThLock);
        return;
    }
    e->ReadInFlight = FALSE;
    e->ReadStatus = status;
    if (status == TH_READ_OK)
    {
        before = e->IdentValid ? e->Ident.Combined : 0;
        after = ident.Combined;
        changed = (before != after);
        e->Ident = ident;
        e->IdentValid = TRUE;
        e->MsgLen = msgLen;
        memcpy(e->MsgNorm, msgNorm, msgLen * sizeof(WCHAR));
        e->ReadsWithoutTitle = 0;
        e->ReadFails = 0;
        e->NoCardReads = 0;
        e->NoCardFirstReq = 0;
    }
    else
    {
        // The previous identity (if any) stands: a read failing mid-swap says nothing about the
        // content; the window's own events bring the next read. Counted so the fail-open line can say
        // how many reads never saw a title, and so the decided-entry retry stays bounded.
        e->ReadsWithoutTitle++;
        e->ReadFails++;
        withoutTitle = e->ReadsWithoutTitle;
        if (status == TH_READ_NOCARD)
        {
            // Two card-less reads say "not a banner" ONLY when the second was requested TH_NOCARD_RETRY_MS or
            // later after the first (ThCoreNoCardCounts): the first-sight read and a LOCATIONCHANGE re-read
            // are unpaced, and a banner mid-grow could answer both within milliseconds (review N2).
            // e->LastReadReq is this read's request tick (one read in flight at a time).
            if (e->NoCardReads == 0)
            {
                e->NoCardReads = 1;
                e->NoCardFirstReq = e->LastReadReq;
            }
            else if (ThCoreNoCardCounts(e->NoCardFirstReq, e->LastReadReq))
            {
                e->NoCardReads = 2;
                if (!e->IdentValid)
                {
                    e->NotBanner = TRUE;   // two paced looks, no card: a flyout, not the banner - ToastHoldDecide shows it
                    notBanner = TRUE;
                }
            }
        }
        else
        {
            e->NoCardReads = 0;
            e->NoCardFirstReq = 0;
        }
        if (!e->IdentValid && !e->NoCardLogged && status != TH_READ_UIAFAIL && !notBanner)
        {
            e->NoCardLogged = TRUE;   // once per window; the fail-open line carries the final status
            logNoCard = TRUE;
        }
    }
    again = e->ReadDirty && !e->NotBanner;
    e->ReadDirty = FALSE;
    if (again) ThRequestReadLocked(e, now);
    LeaveCriticalSection(&g_ThLock);

    if (logNoCard)
        LogInfo("QGATOASTIDENT hwnd=0x%x card not readable yet (%s, read %u): no NormalToastView / no Title|TitleText block - "
            L"held until a read succeeds, a second card-less read says not-a-banner, or the %u ms bound opens it",
            (DWORD)(ULONG_PTR)window, ThReadStatusName(status), withoutTitle, (unsigned)TH_HOLD_BOUND_MS);
    else if (status == TH_READ_OK && changed)
        LogDebug("QGATOASTIDENT hwnd=0x%x identity %016llx -> %016llx (s=%016llx t=%016llx m=%016llx)", (DWORD)(ULONG_PTR)window,
            (unsigned long long)before, (unsigned long long)after, (unsigned long long)ident.Sender,
            (unsigned long long)ident.Title, (unsigned long long)ident.Message);

    // The decision is taken on the tracking pass, never here.
    PokeWindowTrackingFor(window);
}
