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
#include "send.h"   // SendWindowFrameGate / Ungate: swap-edge gating holds frames in send.c
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
#define TH_READ_NOCARD   1     // no card element (TiIsCardAutomationId) under the window: not built yet, or not a banner
#define TH_READ_NOTITLE  2     // a card, but no Title/TitleText block (tree still populating)
#define TH_READ_UIAFAIL  3     // ElementFromHandle/Find failed (a busy or closing shell host)

// One card of a stacked banner (ADR-toasts 13), as the last read that found cards saw it. The bottom card
// is the entry's own (TH_ENTRY.Core / Ident); each card above it is matched to its OWN bridge record here.
typedef struct _TH_STACK_CARD
{
    TOAST_IDENT Ident;
    RECT      Rect;           // screen coordinates, from the read
    LONG      Seq;            // its record (0 = not matched yet)
    UINT32    NotifId;
    LONG      Verdict;        // its record's verdict as of the last pass
    ULONGLONG FirstSeen;      // the read that first showed this content
    BOOL      Titled;         // the read found its title (an untitled card is re-read within its bound)
    BOOL      Excluded;       // kept out of this window: the bridge has it, or it is still being decided
} TH_STACK_CARD;

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
    // THE SIZE AT THE FIRST CARD-LESS READ, and the size the latest pass saw. A banner still BUILDING
    // changes size between two reads; a flyout does not. This is what tells them apart - see the
    // guard in ToastHoldApplyIdentity. 0 = unknown (the guard then does not apply).
    LONG      NoCardFirstW, NoCardFirstH;
    LONG      SeenW, SeenH;   // entry->Width/Height as of the last tracking pass (main-loop thread)
    ULONGLONG LastReadAttempt;// tick of the last request ATTEMPT, queued or refused (deadlines anchor here, never in the past)
    UINT      ReadQueueFails; // consecutive attempts the worker's queue refused (back-off)
    BOOL      NotBanner;
    BOOL      NoCardLogged;   // one QGATOASTIDENT line per window for an unreadable card
    ULONGLONG Due;            // deadline armed for this window (0 = none)
    // THE SETTLE HOLD (ADR-toasts 13): the tick of the first new broker frame main.c did not copy into dom0's buffer
    // because no read had named the cards it shows; 0 when nothing is held. Bounded in ToastHoldDecide, which arms the
    // bound as a deadline: a decided banner arms nothing else, and a refused read must not leave it frozen.
    ULONGLONG SwapGateSince;
    // One repaint owed to dom0 after a hold's release: dom0 is holding a frame from before the hold and
    // nothing else re-sends the current content. Collected by main.c (ToastHoldTakeRepaintOwed) and paid
    // from its region-aware release, so the repaint is this window MINUS whatever is stacked above it.
    BOOL      RepaintOwed;
    // TRUE from a held frame (or a window event) until an OK read requested after it COMPLETES. Not ReadInFlight: a
    // read the worker's queue REFUSED leaves ReadInFlight clear, and keying on that would release the hold with the new
    // frame unnamed. Keyed on this, a refused read holds to the bound, which is the safe direction.
    BOOL      SwapGateReadPending;
    ULONG     SwapGateFrames;     // new frames not copied since the hold closed (reported on the release line)
    int       SwapGateBound;      // 1 = the bound passed with dom0's toast queued: dom0 keeps its last frame until a read
                                  //     names the cards (the reads go on); never an unmap - that would lose this toast.
                                  //     NOT BOUNDED: a banner at rest whose reads gave up stays on that frame until it is
                                  //     destroyed. Releasing on a timer would show dom0's toast late - a double.
    // A DECIDED banner with a newer toast queued behind it, kept so its end can say whether Windows ever
    // painted that toast into it: the queued toast's notification id as of the last tracking pass (0 =
    // none), and the completed re-reads that still named this banner's own content while it was queued.
    UINT32    QueuedNotif;
    UINT      QueuedSameReads;
    ULONGLONG QueuedLastSameRead;
    // THE CARD STACK, top to bottom, from the last read that found cards (Stack[StackCount - 1] is the
    // bottom card, the entry's own). CardSeen is sticky: a window a read once found a card in is a banner,
    // whatever a later read mid-swap says - "two card-less reads" may never class it a flyout after that.
    BOOL      CardSeen;
    int       StackCount;
    BOOL      StackRectsValid;
    RECT      StackRaw;               // the window rect the card rects belong to
    TH_STACK_CARD Stack[TOAST_CARDS_MAX];
    LONG      PromotedSeq;            // the bottom card left: the card now at the bottom keeps the record it had
    BOOL      BottomStacks;           // the bottom card is one the shell stacks the next toast onto (PriorityToastView*)
    UINT      StackMask;              // excluded cards (bit k = Stack[k]) as of the last pass, for the change log
    int       StackMaskN;
    // What ToastHoldCardInsets applied on the latest pass, and the bottom-anchored carry it uses while the
    // window's rect differs from the last read whose rects were valid: cards arrive on TOP and the bottom
    // card does not move (measured, win11r 26300, 2026-11-24: the reminder is 364x157 with b=13 alone and
    // with a toast stacked above it).
    BOOL      AppliedValid;
    UINT      AppliedMask;
    RECT      AppliedInsets;
    BOOL      AppliedCarry;
    int       AppliedGap;
    LONG      CarryShownH, CarryBottom, CarryLeft, CarryRight, CarryW, CarryH;
    BOOL      CarryExcl;
    BOOL      StackShown;             // the last pass showed this window for the cards above a suppressed bottom card
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

// CONTENT THAT ENDED UNSHOWN WITH NO RECORD (ADR-toasts 13: never dropped). Module-global: it must outlive its entry
// (ToastHoldEvict and the LRU victim zero it). Read only on the bridge's verdict event - no timer.
#define TH_ENDED_SLOTS 8
typedef struct _TH_ENDED { TOAST_IDENT Ident; ULONGLONG At; } TH_ENDED;
static TH_ENDED g_ThEnded[TH_ENDED_SLOTS];
static int g_ThEndedNext = 0;
typedef struct _TH_END_ITEM { LONG Seq; UINT32 NotifId; LONG Held; BOOL Won; UINT64 Ident; } TH_END_ITEM;
typedef struct _TH_END_REPORT
{
    int Claims;
    TH_END_ITEM Claim[TOAST_CARDS_MAX + 1];
    int Remembered;
    UINT64 RememberedIdent[TOAST_CARDS_MAX + 1];
} TH_END_REPORT;

// g_ThLock held.
static void ThRememberEndedLocked(IN const TOAST_IDENT* ident, IN ULONGLONG now, IN OUT TH_END_REPORT* rep)
{
    if (!ident || ident->Title == 0) return;   // TiMatch keys on the title: nothing could ever match it
    g_ThEnded[g_ThEndedNext].Ident = *ident;
    g_ThEnded[g_ThEndedNext].At = now;
    g_ThEndedNext = (g_ThEndedNext + 1) % TH_ENDED_SLOTS;
    if (rep->Remembered < TOAST_CARDS_MAX + 1) rep->RememberedIdent[rep->Remembered++] = ident->Combined;
}

// g_ThLock held. One record whose banner ended unshown: claimed for the bridge (the ring's interlocked ops need no lock).
static void ThEndClaimLocked(IN LONG seq, IN UINT32 notifId, IN UINT64 ident, IN OUT TH_END_REPORT* rep)
{
    BOOL won = FALSE;
    LONG held;
    if (!seq) return;
    if (!g_ThIpc || !g_ThBridgeUp) { ThConsumedRemove(&g_ThCons, seq); return; }   // no bridge: as before (a lesser problem)
    held = ThIpcClaimSeq(g_ThIpc, seq, TH_VERDICT_PENDING, TH_VERDICT_BRIDGE, &won);
    if (rep->Claims < TOAST_CARDS_MAX + 1)
    {
        TH_END_ITEM* it = &rep->Claim[rep->Claims++];
        it->Seq = seq; it->NotifId = notifId; it->Held = held; it->Won = won; it->Ident = ident;
    }
}

// g_ThLock held. THE ENTRY'S BANNER ENDS UNSHOWN (destroyed, or its slot is reused): its pending records are claimed for
// the bridge, an identity with no record is remembered.
static void ThEndUnshownLocked(IN TH_ENTRY* e, IN ULONGLONG now, OUT TH_END_REPORT* rep)
{
    if (e->NotBanner) return;
    if (e->Core.Phase == ThPhaseHolding && e->Core.RecordSeq)
        ThEndClaimLocked(e->Core.RecordSeq, e->Core.NotifId, e->Core.Ident, rep);
    else if (e->Core.RecordSeq == 0 && e->IdentValid &&
             (e->Core.Phase == ThPhaseHolding || (e->Core.Phase == ThPhaseDecided && e->Core.Ended)))
        ThRememberEndedLocked(&e->Ident, now, rep);
    for (int k = 0; k < e->StackCount - 1 && k < TOAST_CARDS_MAX; k++)
    {
        const TH_STACK_CARD* sc = &e->Stack[k];
        if (sc->Seq && sc->Verdict == TH_VERDICT_PENDING) ThEndClaimLocked(sc->Seq, sc->NotifId, sc->Ident.Combined, rep);
        else if (!sc->Seq && sc->Excluded) ThRememberEndedLocked(&sc->Ident, now, rep);
    }
}
static ULONGLONG g_ThClock = 0;
static ULONGLONG g_ThNextDue = 0;    // earliest armed deadline over all entries and the no-banner scan; 0 = nothing armed
static ULONGLONG g_ThScanDue = 0;    // the no-banner scan's next deadline (ThScanUnbanneredLocked); 0 = no record waiting
static ULONGLONG g_ThScanFloor = 0;  // records listed before this tick (attach, the last mode switch) are never scanned
static ULONGLONG g_ThBusyEnd = 0;    // when the last banner with a read card left the shell's window (ToastHoldEvict)

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
    g_ThScanFloor = GetTickCount64();   // a record listed before the hold existed is not the scan's (an agent restart)
    if (g_ThIpc) TI_STORE32(&g_ThIpc->HoldActive, g_ThActive ? 1 : 0);   // the bridge waits for our word only when we give one

    // Logged unconditionally and in one shape: a captured log must state which condition produced
    // it. INERT with the gate ON is the state the owner must never find silently - every bridged
    // toast then shows twice - so that case is an ERROR, not an Info.
    // toast-hold-grade.py keys on 'QGATOASTHOLD gate:', 'gate: ACTIVE', 'gate: OFF' and 'QGATOASTHOLD INERT'; what
    // each line used to explain is in the comment beside it.
    if (g_ThActive)
        // ACTIVE: a banner is mapped only after its verdict - a bridge verdict never maps it, a window verdict maps it
        // at once, and with no verdict by the banner's end (TH_BANNER_END_MS) it is never mapped: its record is claimed for
        // the bridge, which forwards it (QGATOASTCLAIM, ADR-toasts 13).
        LogInfo("QGATOASTHOLD gate: ACTIVE (bridge gate on, ToastHoldDisable=0, UIA worker up, IPC up): banners held "
            L"for their verdict until the banner's end, %u ms",
            (unsigned)TH_BANNER_END_MS);
    else if (!g_ThGate)
        LogInfo("QGATOASTHOLD gate: inactive (bridge gate off), banners map as before");
    else if (g_ThKnobOff)
        // OFF: ToastHoldDisable=1 is the escape hatch, not a configuration (toasthold.h); the double is the guest
        // banner plus the dom0 notification.
        LogWarning("QGATOASTHOLD gate: OFF by ToastHoldDisable=1, every bridged toast shows TWICE");
    else
        // INERT: uiaWorker=0 is ToastCropDisable, forced insets or a worker thread that did not start; ipc=0 is the
        // shared section or the verdict event failing to create (QGANOTIFIPC says which). The double lasts for
        // this run.
        LogError("QGATOASTHOLD INERT with the bridge gate ON (uiaWorker=%d ipc=%d): every bridged toast shows TWICE",
            worker, ipc);
}

BOOL ToastHoldActive(void)
{
    return g_ThActive;
}

void ToastHoldNoteMode(void)
{
    if (!g_ThLockInit) return;
    EnterCriticalSection(&g_ThLock);
    // A toast listed before a mode switch belongs to the mode it was listed in: non-seamless, the desktop banner showed it
    // and the bridge settled it window; the no-banner scan must not claim it once seamless returns.
    g_ThScanFloor = GetTickCount64();
    LeaveCriticalSection(&g_ThLock);
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
static TH_ENTRY* ThGetLocked(IN HWND window, OUT TH_END_REPORT* victimRep, OUT HWND* victimWnd)
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
    else { *victimWnd = e->Window; ThEndUnshownLocked(e, GetTickCount64(), victimRep); }   // its banner ends unshown here
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
    if (g_ThScanDue != 0 && (next == 0 || g_ThScanDue < next)) next = g_ThScanDue;
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

// Outside g_ThLock. One ERROR per end claim and per remembered identity (ADR-toasts 13: fail loudly).
static void ThLogEndReport(IN HWND window, IN const WCHAR* how, IN const TH_END_REPORT* rep)
{
    for (int i = 0; i < rep->Claims; i++)
        LogError("QGATOASTCLAIM hwnd=0x%x id=%lu seq=%ld ident=%016llx banner %s unshown with no verdict: claim pending->bridge %s, record "
            L"now %s - %s", (DWORD)(ULONG_PTR)window, (ULONG)rep->Claim[i].NotifId, rep->Claim[i].Seq, (unsigned long long)rep->Claim[i].Ident,
            how, rep->Claim[i].Won ? L"WON by the agent" : L"LOST", ThVerdictName(rep->Claim[i].Held),
            TiVerdictSuppresses(rep->Claim[i].Held) ? L"NOT mapped; the bridge forwards this toast to dom0" :
            rep->Claim[i].Held == TH_VERDICT_WINDOW ? L"the bridge said window and the banner is gone: shown NOWHERE" :
            L"the record left the ring: shown NOWHERE");
    for (int i = 0; i < rep->Remembered; i++)
        LogError("QGATOASTUNSHOWN hwnd=0x%x ident=%016llx banner %s unshown with NO record: its identity is remembered, a record that "
            L"matches it within %u ms is claimed for the bridge", (DWORD)(ULONG_PTR)window, (unsigned long long)rep->RememberedIdent[i],
            how, (unsigned)TH_RECORD_TTL_MS);
}

// g_ThLock held. The bridge's verdict event: a record listed AFTER its banner ended unshown is claimed for the bridge.
static void ThClaimEndedLocked(IN ULONGLONG now, IN OUT TH_END_REPORT* rep)
{
    TH_IPC_RECORD recs[TH_IPC_RECORDS];
    TI_CANDIDATE cand[TH_IPC_RECORDS];
    int nrec = -1;
    for (int i = 0; i < TH_ENDED_SLOTS; i++)
    {
        TI_MATCH q = TiMatchNone;
        if (g_ThEnded[i].At == 0) continue;
        if (now - g_ThEnded[i].At >= TH_RECORD_TTL_MS) { g_ThEnded[i].At = 0; continue; }
        if (nrec < 0) nrec = ThSnapshotRecords(recs, TH_IPC_RECORDS);
        const int n = ThCoreCandidates(recs, nrec, &g_ThCons, 0, now, cand);
        const int pick = TiSelect(cand, n, &g_ThEnded[i].Ident, &q);
        if (pick < 0) continue;
        ThConsumedAdd(&g_ThCons, recs[pick].Seq);
        ThEndClaimLocked(recs[pick].Seq, recs[pick].NotifId, g_ThEnded[i].Ident.Combined, rep);
        g_ThEnded[i].At = 0;
    }
}

// g_ThLock held. A live banner entry, or a card stacked in one, holds this record: that banner decides it.
static BOOL ThSeqHeldLocked(IN LONG seq)
{
    for (int i = 0; i < TH_ENTRIES; i++)
    {
        const TH_ENTRY* e = &g_Th[i];
        if (!e->Window || e->NotBanner) continue;
        if (e->Core.RecordSeq == seq) return TRUE;
        for (int k = 0; k < e->StackCount - 1 && k < TOAST_CARDS_MAX; k++)
            if (e->Stack[k].Seq == seq) return TRUE;
    }
    return FALSE;
}

// g_ThLock held. The shell's banner window shows a real toast card now: a live entry whose card was read (a suppressed
// bridged banner counts - it occupies the shell's window though dom0 never sees it; the text-less twin surface does not).
static BOOL ThBannerBusyLocked(void)
{
    for (int i = 0; i < TH_ENTRIES; i++)
        if (g_Th[i].Window && !g_Th[i].NotBanner && g_Th[i].IdentValid) return TRUE;
    return FALSE;
}

typedef struct _TH_NOBANNER { LONG Seq; UINT32 NotifId; LONG From; LONG Held; BOOL Won; ULONGLONG AgeMs; ULONGLONG QueuedMs; } TH_NOBANNER;

// g_ThLock held. THE TOAST NO BANNER SHOWED (ADR-toasts 13: never dropped). Nothing either side reads says "this toast has
// no banner" - per-app banners off, do-not-disturb, an open Notification Center - so it is decided here, by the agent's
// clock, the one side allowed one: a record that no live banner holds, still `pending`, or `window` with no shown mark,
// TH_BANNER_END_MS after it became the shell's turn, is claimed for the bridge by one compare-exchange, and the bridge
// forwards it. THE SHELL'S TURN: the shell can show banners one at a time, FIFO - a queued one appeared 6.9 s after its
// toast (toastident.h, 2026-10-04 probe), about one banner life (p50 6.07 s) behind the one on screen - so while a read
// banner occupies the window an unmatched record is QUEUED, not bannerless, and its 4.5 s starts when the last such banner
// leaves (g_ThBusyEnd, set by ToastHoldEvict, which re-runs this scan). A window busy for half the TTL is not a queue: the
// record is claimed then. A banner that turns up after a claim finds the record `bridge` and is suppressed - misrouted to
// dom0, loudly, never lost or doubled. Arms g_ThScanDue for the next.
static int ThScanUnbanneredLocked(IN ULONGLONG now, OUT TH_NOBANNER* out, IN int cap)
{
    TH_IPC_RECORD recs[TH_IPC_RECORDS];
    ULONGLONG due = 0;
    int n = 0;
    g_ThScanDue = 0;
    if (!g_ThActive || !g_ThIpc || !g_ThBridgeUp || !ThIpcValid(g_ThIpc) || TI_LOAD32(&g_ThIpc->Seamless) == 0) return 0;
    const BOOL busy = ThBannerBusyLocked();
    const int nrec = ThSnapshotRecords(recs, TH_IPC_RECORDS);
    for (int i = 0; i < nrec; i++)
    {
        const TH_IPC_RECORD* r = &recs[i];
        const LONG from = r->Verdict;
        if (from != TH_VERDICT_PENDING && !(from == TH_VERDICT_WINDOW && r->AgentShownSeq != r->Seq)) continue;
        if (r->Flags & TH_REC_FLAG_GONE) continue;   // its toast left the listing (dismissed, cleared): nothing to forward
        const ULONGLONG arrival = r->ArrivalTick > now ? now : r->ArrivalTick;   // a future tick never defers it
        if (arrival < g_ThScanFloor || now - arrival >= TH_RECORD_TTL_MS) continue;
        if (ThSeqHeldLocked(r->Seq)) continue;
        const ULONGLONG queueCap = arrival + TH_RECORD_TTL_MS / 2;
        if (busy && now < queueCap) { due = ThMinTick(due, queueCap); continue; }   // queued: the evict that frees the window re-scans
        const ULONGLONG turn = (g_ThBusyEnd > arrival && now < queueCap) ? g_ThBusyEnd : arrival;
        if (now < turn + TH_BANNER_END_MS) { due = ThMinTick(due, turn + TH_BANNER_END_MS); continue; }
        BOOL won = FALSE;
        const LONG held = ThIpcClaimSeq(g_ThIpc, r->Seq, from, TH_VERDICT_BRIDGE, &won);
        if (n < cap)
        {
            out[n].Seq = r->Seq; out[n].NotifId = r->NotifId; out[n].From = from; out[n].Held = held; out[n].Won = won;
            out[n].AgeMs = now - arrival;
            out[n].QueuedMs = turn - arrival;
            n++;
        }
    }
    g_ThScanDue = due;
    ThRecomputeDueLocked();
    return n;
}

// Outside g_ThLock. One ERROR per no-banner claim: the log sweep flags every one.
static void ThLogNoBanner(IN const TH_NOBANNER* nb, IN int n)
{
    for (int i = 0; i < n; i++)
        LogError("QGATOASTNOBANNER id=%lu seq=%ld no banner showed this toast within %llu ms of its listing (queued behind a shown "
            L"banner for %llu ms, then bound %u ms): claim %s->bridge %s, record now %s - %s", (ULONG)nb[i].NotifId, nb[i].Seq,
            nb[i].AgeMs, nb[i].QueuedMs, (unsigned)TH_BANNER_END_MS,
            ThVerdictName(nb[i].From), nb[i].Won ? L"WON by the agent" : L"LOST", ThVerdictName(nb[i].Held),
            TiVerdictSuppresses(nb[i].Held) ? L"the bridge forwards it to dom0" :
            nb[i].Held == TH_VERDICT_NONE ? L"the record left the ring: shown NOWHERE" : L"a banner shows it after all");
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
    int readStatus = TH_READ_NONE;
    UINT noCardReads = 0;
    BOOL stackChanged = FALSE, stackOverride = FALSE;
    int stackN = 0;
    UINT stackMask = 0;
    ULONGLONG stackDue = 0;
    UINT32 stackNotif[TOAST_CARDS_MAX] = { 0 };
    LONG stackVerdict[TOAST_CARDS_MAX] = { 0 };
    RECT stackApplied = { 0, 0, 0, 0 };
    int stackGap = 0;
    BOOL stackAppliedValid = FALSE, stackAppliedCarry = FALSE;
    // The ring's own verdict of the matched record (in.MatchVerdict has been through ThCoreDeadVerdict), and the claim
    // this pass makes on it (ADR-toasts 13).
    LONG matchRaw = TH_VERDICT_NONE, claimWant = TH_VERDICT_NONE, claimHeld = TH_VERDICT_NONE, claimSeq = 0;
    BOOL claimWon = FALSE;
    TH_END_REPORT endRep, victimRep;
    HWND victimWnd = NULL;
    ZeroMemory(&endRep, sizeof(endRep));
    ZeroMemory(&victimRep, sizeof(victimRep));

    ZeroMemory(&in, sizeof(in));
    ZeroMemory(&out, sizeof(out));
    ZeroMemory(&identForLog, sizeof(identForLog));

    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThGetLocked(entry->Handle, &victimRep, &victimWnd);
    // The geometry this pass sees, kept for the card-less guard in ToastHoldApplyIdentity (the worker
    // thread has the HWND but not WINDOW_DATA, and must not call into the window code to get it).
    e->SeenW = (LONG)entry->Width;
    e->SeenH = (LONG)entry->Height;

    // Up = the bridge process is up AND its records section is one this agent can read. A section of another ABI (a mixed
    // install) yields no records at all, and with the claim design a banner whose record never comes is never mapped - so
    // that state must read as "no bridge" (shown at once), not as a bridge that never lists anything.
    in.BridgeUp = (g_ThBridgeUp != 0) && g_ThIpc && ThIpcValid(g_ThIpc);
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
            matchRaw = r.Verdict;
        }
        else if (in.IdentKnown && (e->Core.RecordSeq == 0 || !sameContent))
        {
            // A new match for this reading. The entry's own consumed record stays a candidate ONLY when this
            // reading merely completes the one that claimed it (same sender and title, message extended -
            // a half-built card on the first read, review #5); different content in the same window is a
            // new toast and must find its own record or fail open (review N5). Other banners' records never
            // are candidates.
            TI_CANDIDATE cand[TH_IPC_RECORDS];
            // A card that moved to the bottom (the one below it left) is the same toast it was one card up:
            // the record it was matched to there is its own, not another banner's.
            LONG ownSeq = (e->Core.RecordSeq != 0 &&
                           ThCoreReadingExtends(&e->ClaimIdent, e->ClaimMsgNorm, e->ClaimMsgLen,
                                                &e->Ident, e->MsgNorm, e->MsgLen)) ? e->Core.RecordSeq : 0;
            if (ownSeq == 0) ownSeq = e->PromotedSeq;
            e->PromotedSeq = 0;
            const int n = ThCoreCandidates(recs, nrec, &g_ThCons, ownSeq, now, cand);
            const int pick = TiSelect(cand, n, &e->Ident, &quality);
            if (pick >= 0)
            {
                in.MatchFound = 1; in.MatchSeq = recs[pick].Seq; in.MatchNotifId = recs[pick].NotifId;
                in.MatchVerdict = ThCoreDeadVerdict(recs[pick].Seq, recs[pick].Verdict, deadSeq);
                matchRaw = recs[pick].Verdict;
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
    // `successorQueued`: ANY toast newer than this banner's own that is still a match candidate (the core's
    // own rule - unconsumed, younger than TH_RECORD_TTL_MS), whatever its verdict. Windows paints it into
    // this window in place when the current display ends, and no window event reliably reports that
    // repaint. Its NEW FRAME does: while one is queued (QueuedNotif below), ToastHoldNoteFrame requests a
    // read for every new captured frame, and a reading that names it takes the content-changed path.
    // NEVER ON A TIMER (TOASTBANNERLOSTONDIRECTPATH, 2026-11-24): a 500 ms re-read kept the banner it read
    // on screen - hold-on lives were their re-reads x 0.5 s ("180 re-read(s)", ~90 s, until the history
    // was cleared), while with that read removed (one-variable build 167ee3b, 4 runs) no banner outlived
    // 37 s and every queued toast was painted in place ~6 s in; Jev agent_uia_rereads 0.82. A banner at
    // rest sends no frames, so it is not read and Windows times it out.
    // `dom0Queued`: a newer, unconsumed record the bridge has (`bridge`) or dom0 acknowledged (`forwarded`). Read only at the
    // settle hold's 3 s bound: a change no read has named by then, with such a toast queued, is that toast's - dom0 shows it,
    // so this window keeps the frame dom0 has (ADR-toasts 13). Never a reason to hold anything else. `pendingQueued`: such a
    // record still `pending` can become `bridge` at any time until it is claimed (no clock), so it keeps the frame too.
    BOOL successorQueued = FALSE, dom0Queued = FALSE, pendingQueued = FALSE;
    UINT32 successorNotif = 0, dom0Notif = 0, pendingNotif = 0;
    if (!e->NotBanner && in.BridgeUp)
    {
        const LONG baseline = in.MatchFound ? in.MatchSeq : e->Core.RecordSeq;
        TI_CANDIDATE queued[TH_IPC_RECORDS];
        ThCoreCandidates(recs, nrec, &g_ThCons, 0, now, queued);
        for (int i = 0; i < nrec; i++)
        {
            if (ThConsumedHas(&g_ThCons, recs[i].Seq)) continue;   // some banner's own record: shown or suppressed already
            if (!queued[i].Eligible || recs[i].Seq <= baseline) continue;
            successorQueued = TRUE;
            successorNotif = recs[i].NotifId;
            const LONG qv = ThCoreDeadVerdict(recs[i].Seq, recs[i].Verdict, deadSeq);
            if (TiVerdictSuppresses(qv))
            {
                if (!dom0Queued) { dom0Queued = TRUE; dom0Notif = recs[i].NotifId; }
            }
            else if (qv == TH_VERDICT_PENDING)
            {
                if (!pendingQueued) { pendingQueued = TRUE; pendingNotif = recs[i].NotifId; }
            }
        }
    }

    // A banner already on screen whose reading now names a different toast was swapped in place. When no
    // window event closed the swap gate first, nothing is held, so nothing is released and no repaint is
    // owed - and a slice-fed banner at rest gets no capture frame to carry the new content (main.c, the
    // repaint debt). The show below then owes one itself.
    const BOOL shownBefore = (e->Core.Phase == ThPhaseDecided && e->Core.Decision == ThDecShow);
    const UINT64 identBefore = e->Core.Ident;
    const UINT32 notifBeforeRelease = e->Core.NotifId;

    // THE CLAIM (ADR-toasts 13, "Claim, no timer"), made here because the core is pure. A pending record is turned by ONE
    // compare-exchange before anything is decided on it: pending -> window when this pass would map without a window
    // verdict (the bridge is down; a dead instance's record reads window), pending -> bridge at the banner's end. The
    // core then decides on what the record HOLDS, so a lost claim follows the winner.
    const ULONGLONG claimAfterMs = e->Core.HoldSince ? now - e->Core.HoldSince : 0;
    if (in.MatchFound && matchRaw == TH_VERDICT_PENDING && g_ThIpc)
    {
        TH_INPUT probe = in;
        probe.MatchVerdict = TH_VERDICT_PENDING;
        claimWant = (in.MatchVerdict == TH_VERDICT_WINDOW) ? TH_VERDICT_WINDOW : ThCoreClaimWanted(&e->Core, &probe, now);
        if (claimWant != TH_VERDICT_NONE)
        {
            claimSeq = in.MatchSeq;
            claimHeld = ThIpcClaimSeq(g_ThIpc, in.MatchSeq, TH_VERDICT_PENDING, claimWant, &claimWon);
            if (claimHeld == TH_VERDICT_NONE) { in.MatchFound = 0; in.MatchSeq = 0; in.MatchNotifId = 0; }   // the record left the ring
            else in.MatchVerdict = claimHeld;
        }
    }

    ThCoreDecide(&e->Core, &in, now, &out);

    const BOOL swappedInPlace = shownBefore && out.Decision == ThDecShow && e->Core.Ident != identBefore;
    const UINT32 queuedNow = (successorQueued && e->Core.Phase == ThPhaseDecided) ? successorNotif : 0;
    if (queuedNow != e->QueuedNotif)
    {
        e->QueuedNotif = queuedNow;
        e->QueuedSameReads = 0;
        e->QueuedLastSameRead = 0;
    }

    // THE CARDS ABOVE THE BOTTOM ONE (ADR-toasts 13: stacking is normal; dom0 shows every card once). The
    // bottom card is this entry's own and was decided above. Every card above it is matched to its OWN
    // record and kept out of this window while the bridge has it or is still deciding it: a card the bridge
    // forwarded is shown by dom0's notification, and showing it here too was the double (run 45353589359
    // S3iv: a bridged toast stacked onto a shown reminder; the read saw only the first NormalToastView).
    // Unknown is kept out to its end and then claimed for the bridge (ADR-toasts 13) - never shown undecided. The
    // crop itself is ToastHoldCardInsets', from the flags set here.
    if (!e->NotBanner && e->StackCount > 1)
    {
        UINT mask = 0;
        BOOL rereadUntitled = FALSE, recordMoved = FALSE;
        stackN = e->StackCount;
        for (int k = 0; k < e->StackCount - 1; k++)
        {
            TH_STACK_CARD* sc = &e->Stack[k];
            TH_IPC_RECORD r;
            const UINT32 notifBefore = sc->NotifId;
            const LONG verdictBefore = sc->Verdict;
            if (sc->Seq == 0 && sc->Ident.Combined != 0)
            {
                TI_CANDIDATE cand[TH_IPC_RECORDS];
                TI_MATCH q = TiMatchNone;
                const int nc = ThCoreCandidates(recs, nrec, &g_ThCons, 0, now, cand);
                const int pick = TiSelect(cand, nc, &sc->Ident, &q);
                if (pick >= 0)
                {
                    sc->Seq = recs[pick].Seq;
                    sc->NotifId = recs[pick].NotifId;
                    ThConsumedAdd(&g_ThCons, sc->Seq);
                }
            }
            LONG cardRaw = TH_VERDICT_NONE;
            if (sc->Seq != 0 && ThFindBySeq(recs, nrec, sc->Seq, &r))
            {
                cardRaw = r.Verdict;
                sc->Verdict = ThCoreDeadVerdict(r.Seq, r.Verdict, deadSeq);
            }
            const BOOL ended = (now - sc->FirstSeen) >= TH_BANNER_END_MS;
            // THE SAME CLAIM PER CARD (ADR-toasts 13): pending at the card's end -> claimed for the bridge, never shown here;
            // pending with the bridge down -> shown only on a won window claim.
            if (cardRaw == TH_VERDICT_PENDING && g_ThIpc && (!in.BridgeUp || ended))
            {
                BOOL won = FALSE;
                const LONG want = in.BridgeUp ? TH_VERDICT_BRIDGE : TH_VERDICT_WINDOW;
                const LONG held = ThIpcClaimSeq(g_ThIpc, sc->Seq, TH_VERDICT_PENDING, want, &won);
                if (held != TH_VERDICT_NONE) sc->Verdict = held;
                if (in.BridgeUp && endRep.Claims < TOAST_CARDS_MAX + 1)
                {
                    TH_END_ITEM* it = &endRep.Claim[endRep.Claims++];
                    it->Seq = sc->Seq; it->NotifId = sc->NotifId; it->Held = held; it->Won = won; it->Ident = sc->Ident.Combined;
                }
            }
            const BOOL undecided = (sc->Seq == 0 || sc->Verdict == TH_VERDICT_PENDING);
            if (!in.BridgeUp)
                // No bridge: the core's dead-bridge rule for a suppressed banner (ThCoreDecide, "THE BRIDGE DIED"): a
                // card dom0 acknowledged (`forwarded`) is shown by dom0 and stays out; `bridge` and `pending` are
                // toasts nobody will forward now, so they are shown here. Forwarded-only on purpose: g_ThDeadSeq can
                // be 0 (no ceiling captured), and then ThCoreDeadVerdict leaves `bridge` standing.
                sc->Excluded = (sc->Verdict == TH_VERDICT_FORWARDED);
            else if (undecided)
                sc->Excluded = TRUE;                                     // never shown undecided, however long
            else
                sc->Excluded = TiVerdictSuppresses(sc->Verdict);
            if (in.BridgeUp && undecided && !ended)
            {
                stackDue = ThMinTick(ThMinTick(stackDue, now + TH_HOLD_RECHECK_MS), sc->FirstSeen + TH_BANNER_END_MS);
                if (!sc->Titled) rereadUntitled = TRUE;
            }
            if (sc->Excluded) mask |= 1u << k;
            // A card's record found, or its verdict moved, is said too: the line then names the toast each
            // kept-out card IS (toast-hold-grade.py keys on card=id<N>/.../out), not only that one is out.
            if (sc->NotifId != notifBefore || sc->Verdict != verdictBefore) recordMoved = TRUE;
            stackNotif[k] = sc->NotifId;
            stackVerdict[k] = sc->Verdict;
        }
        if (e->Core.Phase == ThPhaseDecided && e->Core.Decision == ThDecSuppress)
            mask |= 1u << (e->StackCount - 1);      // the bottom card's own toast went to dom0
        // A stacked card read before its title was built cannot be matched: read it again on the recheck
        // tick, and ONLY until its bound - a timed re-read keeps a banner on screen (TOASTBANNERLOSTONDIRECTPATH),
        // so this is at most TH_BANNER_END_MS / TH_HOLD_RECHECK_MS reads per card, never a standing poll.
        if (rereadUntitled && !e->ReadInFlight && (now - e->LastReadAttempt) >= TH_HOLD_RECHECK_MS)
            ThRequestReadLocked(e, now);
        stackMask = mask;
        stackChanged = (mask != e->StackMask || e->StackCount != e->StackMaskN || recordMoved);
        stackGap = e->AppliedGap;
        e->StackMask = mask;
        e->StackMaskN = e->StackCount;
        // A suppressed bottom card with a card above it that belongs here: the window shows THAT card, and
        // only once the crop keeping the bottom card out was applied on this very pass (GetWindowData runs
        // before this decision; ToastHoldCardInsets records what it applied) - never one pass early.
        const UINT all = (1u << e->StackCount) - 1;
        if (out.Decision == ThDecSuppress && e->Core.Phase == ThPhaseDecided && (all & ~mask) != 0 &&
            e->AppliedValid && e->AppliedMask == mask)
        {
            out.Decision = ThDecShow;
            stackOverride = TRUE;
        }
        stackApplied = e->AppliedInsets;
        stackAppliedValid = e->AppliedValid;
        stackAppliedCarry = e->AppliedCarry;
    }
    else if (e->StackMaskN != e->StackCount)
    {
        stackChanged = (e->StackMaskN > 1);         // a stack collapsed to one card: say so once
        stackN = e->StackCount;
        e->StackMask = 0;
        e->StackMaskN = e->StackCount;
    }
    // Every pass: ToastHoldNoteChanged gates the swap edge of a window shown for its upper cards as it does a shown banner's.
    e->StackShown = stackOverride;

    if (out.ReleaseSeq)
    {
        // The record this banner had reserved. Its content is still DISPLAYED when it only moved up the stack (a card was
        // inserted below it): then it is released and the card re-matches it. Otherwise that content left the window
        // while held - its banner ended unshown: claimed for the bridge (ADR-toasts 13).
        BOOL stillShown = FALSE;
        for (int k = 0; k < e->StackCount - 1 && k < TOAST_CARDS_MAX; k++)
            if (identBefore != 0 && e->Stack[k].Ident.Combined == identBefore) stillShown = TRUE;
        if (stillShown) ThConsumedRemove(&g_ThCons, out.ReleaseSeq);
        else ThEndClaimLocked(out.ReleaseSeq, notifBeforeRelease, identBefore, &endRep);
    }
    if (in.MatchFound) notifForLog = in.MatchNotifId;
    if (e->Core.NotifId) notifForLog = e->Core.NotifId;
    // The agent's one word back into a record (toastident.h AgentShownSeq, ADR-toasts 11): this banner shows the
    // toast of a record that reads `window` - the correction just taken (ThEvShowCorrected) or a banner already
    // visible. The bridge reads it after it turned a forwarded-with-actions toast's record `window` because the
    // dom0 click could not be carried out: a mark means the user has the guest's own buttons now; none means
    // the banner had already gone and the bridge tells the user through a dom0 error notice instead. The mark
    // is the record's own sequence, so a store that races the bridge's reuse of the slot cannot be read as the
    // next toast's.
    if (in.MatchFound && in.MatchVerdict == TH_VERDICT_WINDOW && out.Decision == ThDecShow &&
        e->Core.Phase == ThPhaseDecided && g_ThIpc)
        ThIpcAgentMarkShown(g_ThIpc, in.MatchSeq);
    // The same mark for a card stacked above the bottom one that this window shows (not kept out by the crop): the bridge
    // waits for it before it counts a `window` toast as shown (ADR-toasts 13), and the no-banner scan skips a marked record.
    if (out.Decision == ThDecShow && g_ThIpc && !e->NotBanner)
        for (int k = 0; k < e->StackCount - 1 && k < TOAST_CARDS_MAX; k++)
            if (e->Stack[k].Seq && !e->Stack[k].Excluded && e->Stack[k].Verdict == TH_VERDICT_WINDOW)
                ThIpcAgentMarkShown(g_ThIpc, e->Stack[k].Seq);

    // Reads, bounded by the hold's own deadlines and never while one is in flight. Every interval is measured
    // from the last ATTEMPT and stretched by ThCoreRetryDelayMs while the worker's queue refuses (review N7),
    // so a deadline derived here is never in the past:
    //   * held with no identity or no record: on the recheck tick (a half-built card, a late listing);
    //   * a first read that found NO card: one more look, requested >= TH_NOCARD_RETRY_MS after the first
    //     card-less read's request - that second one says "not a banner" (a Quick Settings / volume / clock
    //     flyout passes the toast classifier too; a banner mid-grow does not, review N2);
    //   * a last read that FAILED (UIA error, no title, no card on a banner already identified), in ANY phase:
    //     once per recheck interval, at most TH_READ_RETRY_MAX times in a row (review #8) - a decided banner
    //     with a stale reading must not keep it for ever, and must not be polled for ever either;
    //   * decided, with a newer toast queued behind it: never here - on its next captured frame
    //     (ToastHoldNoteFrame), because a timed read kept the banner on screen.
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
        else if (e->SwapGateReadPending && (e->SwapGateSince || e->SwapGateBound) && e->ReadQueueFails > 0 && since >= backoff)
            ThRequestReadLocked(e, now);   // the settle hold's read was REFUSED: nothing else re-requests it for a decided banner
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

    // THE SETTLE HOLD (ADR-toasts 13, owner-confirmed 2026-11-24: "a window's new frames reach dom0 only after a read has
    // named the cards they show, at most 3 s later. A toast is never held back because another toast is on screen or
    // queued."). ToastHoldNoteFrame closes it at a shown banner's new broker frame and main.c does not copy that frame;
    // it is released here when a read requested after the newest held frame has named the cards AND their crop is the
    // one applied on this very pass (GetWindowData runs before this decision), so a newly read card that must stay out
    // is never released uncropped. The bound is armed as a deadline below, so a pass always comes to end it.
    //
    // AT THE BOUND THE RECORDS DECIDE. A newer bridged or forwarded record no banner claimed (dom0Queued) means the
    // change no read has named is that toast, which dom0 shows: dom0 keeps this window's last frame - the toast it was
    // told this window carries - and the reads go on until one names the cards (SwapGateBound). The window is never
    // unmapped for it: that loses this banner's toast where a frozen frame loses nothing. A stacked window whose crop
    // is not current is kept too, because releasing it would show a card that may be dom0's. So is a newer record still
    // `pending` (pendingQueued): with no clock on either side it can become `bridge` at any moment until it is claimed
    // (ADR-toasts 13), and the frame no read has named may be that toast - said as an ERROR. Otherwise released with a
    // full repaint, and said loudly.
    BOOL settleHold = FALSE, settleReleased = FALSE, settleLate = FALSE, settleKept = FALSE;
    ULONGLONG settleMs = 0;
    ULONG settleFrames = 0;
    const UINT readFails = e->ReadFails, queueFails = e->ReadQueueFails;
    if (e->SwapGateBound && !e->SwapGateReadPending)
        e->SwapGateBound = 0;            // the read the bound waited for has landed: this pass decides on what it named
    if (e->SwapGateSince && (out.Decision != ThDecShow || entry->MapDeferred))
    {
        e->SwapGateSince = 0;            // DROPPED: unmapped now, so nothing held is released into dom0
        e->SwapGateFrames = 0;
        e->SwapGateBound = 0;
    }
    if (e->SwapGateSince)
    {
        const BOOL cropCurrent = (e->StackCount <= 1) || (e->AppliedMask == stackMask && (stackMask == 0 || e->AppliedValid));
        settleMs = now > e->SwapGateSince ? now - e->SwapGateSince : 0;
        settleFrames = e->SwapGateFrames;
        if (e->SwapGateBound)
            settleHold = TRUE;                           // latched at the bound: dom0 keeps its last frame
        else if (!e->SwapGateReadPending && cropCurrent)
            settleReleased = TRUE;                       // a read named the cards, and the crop for them is applied
        else if (settleMs < TH_HOLD_BOUND_MS)
            settleHold = TRUE;
        else
        {
            settleLate = TRUE;
            if (dom0Queued || pendingQueued || (stackMask != 0 && !cropCurrent))
            {
                e->SwapGateBound = 1;
                settleKept = TRUE;
                settleHold = TRUE;
            }
            else
                settleReleased = TRUE;
        }
        if (settleReleased)
        {
            e->SwapGateSince = 0;
            e->SwapGateFrames = 0;
        }
    }

    e->Due = out.Due;
    // A SUPPRESSED BANNER MUST STAY ARMED WHILE IT IS ON SCREEN. THIS IS THE LOST TOAST, and the
    // register's own reading of it was right: toast-hold S3i on win11r-thold, 2026-11-07, one hold
    // cycle on hwnd 0x6019e for BOTH toasts of the pair -
    //     044546.862  hold reason=no-identity bridgeUp=1          <- S3i-1's banner appears
    //     044547.460  S3i-2 fires (fires.jsonl), 838 ms after S3i-1, i.e. 598 ms INSIDE the hold
    //     044548.412  state=suppress ident=ca78903c id=10 verdict=bridge held_ms=1547
    //     044549.191  QGATOASTACK id=10 ack_wait_ms=781
    //     ...then ZERO ToastHoldDecide lines for 38 s, until S3ii at 044627.424. S3i-2 = LOST.
    // The bridge HAD turned the second toast's record to the window path at 04:45:48 (`skip id=11 ...
    // window path; window-only app`), which is precisely the condition the suppress line promises to
    // act on: "reopened if the record turns window or the bridge dies before dom0's ack". It was never
    // acted on, because once ThCoreDecide has DECIDED, out.Due is 0 - nothing is armed - and the retry
    // block above arms only for a FAILED or card-less read. So the only things that could have
    // re-examined that banner were a window event or the bridge's verdict event, and neither came.
    // The promise in that log line is only keepable if something wakes the main loop, so a suppressed
    // banner now carries the ordinary recheck tick for as long as it is tracked; ToastHoldSweep pokes
    // a pass, ThCoreDecide re-reads the record EVERY pass (it already does - "a decided banner's record
    // can still move"), and a record that turned window produces the show it owes. The banner is
    // dropped on RemoveWindow, so this costs a handful of passes over one banner's life and nothing at
    // rest. S3ii and S3iii were the SAME mixed pairs ~5.1 s apart - after the hold had resolved and the
    // window event for the next banner arrived - which is why they were never lost and this was.
    if (e->Core.Phase == ThPhaseDecided && e->Core.Decision == ThDecSuppress)
        e->Due = ThMinTick(e->Due, now + TH_HOLD_RECHECK_MS);
    // A decided banner with a newer toast queued behind it gets NO recheck tick: its re-read comes from
    // its next captured frame (ToastHoldNoteFrame), never from a timer - see successorQueued above.
    if (armRetry)
        e->Due = ThMinTick(e->Due, retryAt);
    if (settleHold && !e->SwapGateBound)
        e->Due = ThMinTick(e->Due, e->SwapGateSince + TH_HOLD_BOUND_MS);
    if (stackDue)
        e->Due = ThMinTick(e->Due, stackDue);   // a stacked card still being decided: its verdict, or its bound
    ThRecomputeDueLocked();
    LeaveCriticalSection(&g_ThLock);

    const DWORD hw = (DWORD)(ULONG_PTR)entry->Handle;
    const unsigned long long hc = (unsigned long long)identForLog.Combined;
    const unsigned long long hs = (unsigned long long)identForLog.Sender;
    const unsigned long long ht = (unsigned long long)identForLog.Title;
    const unsigned long long hm = (unsigned long long)identForLog.Message;
    // A PARTIAL match means the bridge's text and the banner's text are spelled differently for this
    // toast; it is accepted only as the sole candidate carrying that sender+title (toastident.h), and
    // the way to diagnose one is to compare s/t/m here with the bridge's HOLD line for this id.
    // toast-hold-grade.py keys on 'PARTIAL match'.
    // THE SETTLE HOLD'S END, outside g_ThLock like every send-side call this module makes. send.c's gate (claimed by
    // ToastHoldNoteFrame) holds this window's configure/rebuild while main.c holds its frames; a show with no hold opens
    // it. A release OWES the window a repaint: dom0 has a frame from before the hold and nothing else re-sends the
    // current content. It is RECORDED here and paid by main.c's region-aware release (this window minus whatever is
    // stacked above it - a full-window send would bleed the pixels above it into a slice-fed banner; Jev 0.98), after
    // main.c has copied the newest frame it did not copy while held.
    ULONG damageHeld = 0;
    const BOOL ungated = (!settleHold && out.Decision == ThDecShow && SendWindowFrameUngate(entry->Handle, &damageHeld));
    // The in-place swap no hold saw: the same debt, paid by the same release.
    const BOOL swapOwed = (!ungated && !settleReleased && swappedInPlace && !settleHold && out.Decision == ThDecShow);
    if (settleLate && settleKept && pendingQueued && !dom0Queued)
        LogError("QGATOASTGATELATE hwnd=0x%x no read named this banner's cards within %llu ms of its first held frame (%lu held, "
            L"lastRead=%s readFails=%u queueFails=%u) and a newer record id=%lu is still PENDING: dom0 keeps its last frame until a "
            L"read names the cards", hw, (unsigned long long)settleMs, (ULONG)settleFrames, ThReadStatusName(readStatus), readFails,
            queueFails, (ULONG)pendingNotif);
    else if (settleLate)
        LogWarning("QGATOASTGATELATE hwnd=0x%x no read named this banner's cards within %llu ms of its first held frame "
            L"(%lu held, lastRead=%s readFails=%u queueFails=%u, queued bridge id=%lu): %s", hw,
            (unsigned long long)settleMs, (ULONG)settleFrames, ThReadStatusName(readStatus), readFails, queueFails,
            (ULONG)dom0Notif, settleKept ? L"dom0 keeps its last frame until a read names the cards" :
            L"released with a full repaint - no newer bridged or forwarded record claims the change");
    if (ungated || settleReleased || swapOwed)
    {
        EnterCriticalSection(&g_ThLock);
        TH_ENTRY* owed = ThFindLocked(entry->Handle);
        if (owed)
            owed->RepaintOwed = TRUE;
        LeaveCriticalSection(&g_ThLock);
    }
    // toast-hold-grade.py's mapped_at reads the "frames gated" / "frames resumed" pair: a second toast in a shared banner
    // window gets no MAP of its own, only this release and its repaint, so the line must exist for it to grade as shown.
    if (ungated || settleReleased)
        LogInfo("QGATOASTGATE hwnd=0x%x frames resumed after %lu held, hold open %llu ms (%lu damage rect(s) dropped): the "
            L"newest frame is copied and the window repainted in full", hw, (ULONG)settleFrames,
            (unsigned long long)settleMs, (ULONG)damageHeld);
    else if (swapOwed)
        LogInfo("QGATOASTSWAP hwnd=0x%x id=%lu painted into the shown banner in place with no window event: a "
            L"repaint is owed so dom0 is sent this toast's pixels", hw, (ULONG)notifForLog);
    // AN UNMAPPED WINDOW KEEPS send.c's GATE: only a show opens it, and the slot comes back at ToastHoldEvict.

    if (logPartial)
        LogWarning("QGATOASTIDENT hwnd=0x%x PARTIAL match taken (same sender+title, message differs); "
            L"ident=%016llx s=%016llx t=%016llx m=%016llx id=%lu", hw, hc, hs, ht, hm, (ULONG)notifForLog);

    // ADR-toasts 13: every claim the agent makes is said - the banner-end claim as an ERROR (fail loudly).
    if (claimWant == TH_VERDICT_BRIDGE)
        LogError("QGATOASTCLAIM hwnd=0x%x id=%lu seq=%ld ident=%016llx banner end after %llu ms with no verdict: claim pending->bridge "
            L"%s, record now %s - %s", hw, (ULONG)notifForLog, (LONG)claimSeq, hc, claimAfterMs,
            claimWon ? L"WON by the agent" : L"LOST", ThVerdictName(claimHeld),
            TiVerdictSuppresses(claimHeld) ? L"NOT mapped; the bridge forwards this toast to dom0" :
            claimHeld == TH_VERDICT_WINDOW ? L"the bridge said window first: the banner is mapped" :
            L"the record left the ring: NOT mapped and NOT forwarded");
    else if (claimWant == TH_VERDICT_WINDOW)
        LogWarning("QGATOASTCLAIM hwnd=0x%x id=%lu seq=%ld ident=%016llx bridge down with the record pending: claim pending->window %s, "
            L"record now %s", hw, (ULONG)notifForLog, (LONG)claimSeq, hc, claimWon ? L"WON by the agent" : L"LOST", ThVerdictName(claimHeld));
    ThLogEndReport(entry->Handle, L"replaced in place or left the stack", &endRep);
    ThLogEndReport(victimWnd, L"evicted", &victimRep);

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
    case ThEvAckConfirmed:
        // THE HEALTHY END OF A SUPPRESSION, which used to end silently. ack_wait_ms is measured on the
        // bound's own clock - from this banner's suppression to the pass that read `forwarded` - so the
        // log finally carries the distribution the FAILURES alone could never show: how close a working
        // forward runs to TH_FORWARD_BOUND_MS. The two doubles of 41138837627 both failed open on a
        // forward that SUCCEEDED, so whether that bound sits above or inside the normal path is the open
        // question, and this is the number that answers it. INFO, not a warning: nothing went wrong.
        LogInfo("QGATOASTACK hwnd=0x%x ident=%016llx id=%lu ack_wait_ms=%llu bound_ms=%u (dom0 acknowledged the "
            L"forward: the suppression stands)",
            hw, hc, (ULONG)notifForLog, out.HeldMs, (UINT)TH_FORWARD_BOUND_MS);
        break;
    case ThEvShowCorrected:
        // The record turned window after the banner was suppressed: the bridge could not forward the toast after
        // all (the forward failed for good, or dom0 was unreachable), or a dom0 action on it could not be carried
        // out in the guest (the bridge's ACTION lines). The record carries the agent's shown mark; a banner that
        // has already timed out in the guest survives in its Notification Center only.
        LogWarning("QGATOASTHOLD hwnd=0x%x state=show reason=record-turned-window ident=%016llx id=%lu after %llu ms suppressed; "
            L"the bridge could not forward it, the banner is shown late", hw, hc, (ULONG)notifForLog, out.HeldMs);
        break;
    case ThEvShowNoBridge:
        LogInfo("QGATOASTHOLD hwnd=0x%x state=show reason=bridge-down ident=%016llx (no bridge running: window path at once, by design)",
            hw, hc);
        break;
    case ThEvNotBanner:
        LogInfo("QGATOASTIDENT hwnd=0x%x not a toast banner (no card in %u reads, %ux%u) - shown, never held: a "
            L"ShellExperienceHost flyout that passes the toast classifier",
            hw, noCardReads, entry->Width, entry->Height);
        break;
    case ThEvBannerEnd:
        if (out.Reason == ThReasonNoRecord)
            LogError("QGATOASTUNSHOWN hwnd=0x%x banner end after %llu ms with NO record: NOT mapped; ident=%016llx s=%016llx t=%016llx "
                L"m=%016llx lastRead=%s records=%d - a record listed later that matches it is claimed for the bridge",
                hw, out.HeldMs, hc, hs, ht, hm, ThReadStatusName(readStatus), nrec);
        else
            LogWarning("QGATOASTUNSHOWN hwnd=0x%x banner end after %llu ms with NO identity (lastRead=%s): NOT mapped, nothing to remember",
                hw, out.HeldMs, ThReadStatusName(readStatus));
        break;
    case ThEvForwardLate:
        LogError("QGATOASTFWDLATE hwnd=0x%x ident=%016llx id=%lu suppressed %llu ms with no dom0 acknowledgement (bound %u ms): NOT "
            L"mapped - the record is the bridge's; shown only if the bridge turns it window", hw, hc, (ULONG)notifForLog,
            out.HeldMs, (UINT)TH_FORWARD_BOUND_MS);
        break;
    case ThEvFailOpen:
        // The bridge died with this banner held, or suppressed before dom0 acknowledged the forward: mapped without a
        // verdict (a lesser problem, owner 2026-11-24). Every other end of a hold is NOT mapped (ADR-toasts 13): the
        // banner's end claims the record for the bridge (QGATOASTCLAIM), a banner with no record or no identity is
        // QGATOASTUNSHOWN. reason= decoder (ThReason, toasthold-core.h; the grader keys on 'QGATOASTHOLDLATE hwnd='):
        // bridge-exited only.
        LogWarning("QGATOASTHOLDLATE hwnd=0x%x mapped FAIL-OPEN after %llu ms: reason=%s ident=%016llx s=%016llx t=%016llx "
            L"m=%016llx id=%lu lastRead=%s bridgeUp=%d records=%d; shown TWICE if the bridge forwarded it",
            hw, out.HeldMs, ThReasonName(out.Reason), hc, hs, ht, hm, (ULONG)notifForLog,
            ThReadStatusName(readStatus), in.BridgeUp, nrec);
        break;
    case ThEvContentChanged:
        LogInfo("QGATOASTHOLD hwnd=0x%x content changed in place: new ident=%016llx s=%016llx t=%016llx m=%016llx - held again "
            L"for its own verdict (the window stays unmapped until then)", hw, hc, hs, ht, hm);
        break;
    default:
        break;
    }

    // The stack, said when it changes: which cards this window keeps out and why, and the crop that was
    // applied for it. A changed set also re-drives a pass at once, so the crop the next GetWindowData
    // applies is this one (it ran before this decision).
    if (stackChanged)
    {
        WCHAR cards[TOAST_CARDS_MAX * 48] = L"";
        for (int k = 0; k < stackN - 1 && k < TOAST_CARDS_MAX; k++)
        {
            WCHAR one[48];
            if (SUCCEEDED(StringCchPrintfW(one, RTL_NUMBER_OF(one), L" card%d=id%lu/%s/%s", k, (ULONG)stackNotif[k],
                    ThVerdictName(stackVerdict[k]), (stackMask >> k) & 1 ? L"out" : L"shown")))
                StringCchCatW(cards, RTL_NUMBER_OF(cards), one);
        }
        LogInfo("QGATOASTCARDS hwnd=0x%x n=%d excluded=0x%x bottom=%s%s (top to bottom; a card the bridge has or is still "
            L"deciding is kept out of this window) applied=%s l=%ld t=%ld r=%ld b=%ld gap=%d%s", hw, stackN, stackMask,
            stackN > 0 && ((stackMask >> (stackN - 1)) & 1) ? L"out" : L"shown", cards,
            stackAppliedValid ? (stackAppliedCarry ? L"carry" : L"fresh") : L"none",
            (long)stackApplied.left, (long)stackApplied.top, (long)stackApplied.right, (long)stackApplied.bottom, stackGap,
            stackOverride ? L" - the bottom card's toast went to dom0, this window shows the cards above it" : L"");
        PokeWindowTrackingFor(entry->Handle);
    }

    return out.Decision;
}

// The crop of a stacked banner whose cards are not all shown here (main.c GetWindowData, every pass of a
// toast window, before the decision). From the last read's card rects and the last pass's flags: the
// contiguous run of shown cards (TiCardRun). While the window's rect differs from the last read whose
// rects were valid, the run is carried anchored at the BOTTOM - cards arrive on top and the bottom card
// does not move - when that read had cards kept out, or when the window GREW over a shown bottom card the
// shell stacks onto (PriorityToastView*): the card it grew for is unknown until a read names it, and its
// record trails the growth - run 45360456502 S3iv, the window grew at 11:22:26.378 and the bridge listed
// the toast at 11:22:27, so the first frames of a forwarded card would be shown before anything knew it.
// A plain toast card swaps in place instead of stacking and is never carried. FALSE = no override.
BOOL ToastHoldCardInsets(IN const WINDOW_DATA* entry, OUT RECT* insets)
{
    BOOL have = FALSE, carried = FALSE, logGap = FALSE;
    int gap = 0;
    if (!g_ThActive || !entry || !insets) return FALSE;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(entry->Handle);
    if (e && !e->NotBanner && e->StackCount >= 1)
    {
        const int n = e->StackCount;
        const LONG x = (LONG)entry->X, y = (LONG)entry->Y, w = (LONG)entry->Width, h = (LONG)entry->Height;
        const LONG rw = e->StackRaw.right - e->StackRaw.left, rh = e->StackRaw.bottom - e->StackRaw.top;
        const BOOL fresh = e->StackRectsValid &&
                           (x == e->StackRaw.left && y == e->StackRaw.top && w == rw && h == rh);
        const BOOL shownBottom = (e->Core.Phase == ThPhaseDecided && e->Core.Decision == ThDecShow);
        UINT mask = 0;
        for (int k = 0; k < n - 1; k++)
            if (e->Stack[k].Excluded) mask |= 1u << k;
        if (e->Core.Phase == ThPhaseDecided && e->Core.Decision == ThDecSuppress)
            mask |= 1u << (n - 1);
        if (fresh)
        {
            TI_CARD_BOX raw = { e->StackRaw.left, e->StackRaw.top, e->StackRaw.right, e->StackRaw.bottom };
            TI_CARD_BOX boxes[TOAST_CARDS_MAX], run = { 0, 0, 0, 0 };
            int excl[TOAST_CARDS_MAX], t = 0, b = 0;
            for (int k = 0; k < n; k++)
            {
                boxes[k].Left = e->Stack[k].Rect.left;   boxes[k].Top = e->Stack[k].Rect.top;
                boxes[k].Right = e->Stack[k].Rect.right; boxes[k].Bottom = e->Stack[k].Rect.bottom;
                excl[k] = (int)((mask >> k) & 1);
            }
            if (TiCardRun(&raw, boxes, excl, n, &run, &t, &b, &gap))
            {
                insets->left = run.Left; insets->top = run.Top; insets->right = run.Right; insets->bottom = run.Bottom;
                have = TRUE;
                e->CarryShownH = e->Stack[b].Rect.bottom - e->Stack[t].Rect.top;
                e->CarryBottom = run.Bottom; e->CarryLeft = run.Left; e->CarryRight = run.Right;
                e->CarryExcl = TRUE;
            }
            else if (mask == 0)
            {
                // Every card shown: the crop worker's union stands; kept as the carry for a toast queued behind.
                e->CarryShownH = e->Stack[n - 1].Rect.bottom - e->Stack[0].Rect.top;
                e->CarryBottom = e->StackRaw.bottom - e->Stack[n - 1].Rect.bottom;
                e->CarryLeft = e->Stack[n - 1].Rect.left - e->StackRaw.left;
                e->CarryRight = e->StackRaw.right - e->Stack[n - 1].Rect.right;
                e->CarryExcl = FALSE;
            }
            else
                e->CarryShownH = 0;
            e->CarryW = rw;
            e->CarryH = rh;
            logGap = (gap != e->AppliedGap);
            e->AppliedGap = gap;
        }
        else if (e->CarryShownH > 0 && w == e->CarryW &&
                 (e->CarryExcl || (e->BottomStacks && shownBottom && h > e->CarryH)))
        {
            const LONG top = h - e->CarryBottom - e->CarryShownH;
            if (top >= 0)
            {
                insets->left = e->CarryLeft; insets->top = top; insets->right = e->CarryRight; insets->bottom = e->CarryBottom;
                have = carried = TRUE;
            }
        }
        e->AppliedValid = have;
        e->AppliedMask = mask;
        e->AppliedCarry = carried;
        if (have) e->AppliedInsets = *insets;
    }
    LeaveCriticalSection(&g_ThLock);
    // Cards this window should show but one rectangle cannot: a card kept out sits BETWEEN shown ones. NOT
    // DECIDED (owner question P7): the run with the bottom-most shown card is shown and these are not.
    if (logGap)
        LogInfo("QGATOASTCARDGAP hwnd=0x%x %d shown card(s) outside the shown run: a card kept out sits between "
            "cards that belong here, and one rectangle shows only the run with the bottom-most of them",
            (DWORD)(ULONG_PTR)entry->Handle, gap);
    return have;
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
    BOOL gateSwap = FALSE;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (e)
    {
        ThRequestReadLocked(e, now);
        // A WINDOW EVENT ON A SHOWN BANNER CLOSES THE SETTLE HOLD EARLY (ADR-toasts 13). The hold is keyed on frames
        // (ToastHoldNoteFrame) because no event reliably reports a repaint in place; but when one does arrive it is the
        // earliest notice, ahead of the frames it announces, and closing the hold here also withholds the window's
        // configure and buffer rebuild (send.c's gate, main.c ToastGateHoldsGeometry), so a stacked banner growing a card
        // is not rebuilt into dom0 half-built. A read that lands naming the same cards (a move, not a swap) releases it
        // at once; median read latency is 218 ms.
        //
        // ONLY FOR A SETTLED, SHOWN BANNER (decided Show, or shown for the cards above a suppressed bottom card:
        // StackShown). A banner sliding in is in Holding and unmapped; gating its every move would owe a repaint storm.
        //
        // NEVER FOR A WINDOW CLASSED NOT-A-BANNER. ThRequestReadLocked above returns at once for it, so the read that
        // is the hold's only release is never requested and it would hold to TH_HOLD_BOUND_MS (measured 2026-11-24,
        // crop-before-map on b848fded: 0x4018e "not a toast banner", then "frames resumed after 2 held, swap gate open
        // 3000 ms" per card - dom0 kept the old stack for 3 s while the new card was on the guest).
        gateSwap = (e->Core.Phase == ThPhaseDecided && (e->Core.Decision == ThDecShow || e->StackShown) && !e->NotBanner);
        // EVERY DECIDED BANNER ARMS THE DEADLINE, NOT ONLY A SHOWN ONE. This is the toast loss that
        // kept coming back (findings/issues.md TOASTBANNERLOSTONDIRECTPATH, four times, last on
        // 2026-11-07 02:45:49Z as toast-hold S3i-2=LOST with its pair 838 ms apart - inside the first
        // banner's hold). A SUPPRESSED banner arms nothing: ToastHoldDecide leaves Due at 0 once it has
        // a good read and a decided record (the retry arming above it fires only for a FAILED or
        // card-less read), and until this change the arming here was inside `if (gateSwap)` - i.e.
        // shown banners only. So for a suppressed banner whose window was repainted with a NEW toast,
        // the whole re-examination depended on ThRequestReadLocked's request landing: that request can
        // be REFUSED (the UIA worker's queue is full -> ReadQueueFails++, nothing in flight), and then
        // no read ever completes, ToastHoldApplyIdentity never runs, its PokeWindowTrackingFor never
        // fires, and NO TRACKING PASS EVER LOOKS AT THAT WINDOW AGAIN. The second toast stays unmapped
        // for as long as the shell displays it and is lost with no decision taken - which is exactly
        // what "no ToastHoldDecide on the window for the rest of the step" means in the run data.
        // The hold's own contract (toasthold.h: "a suppressed banner is re-examined on every pass ...
        // shown after all when the bridge turns its record to `window`") is only reachable if something
        // wakes the main loop, and for a suppressed banner nothing did.
        if (e->Core.Phase == ThPhaseDecided)
        {
            if (gateSwap)
            {
                // Arm the bound AND wake the main loop: a decided banner arms nothing else (ToastHoldDecide).
                if (e->SwapGateSince == 0)
                {
                    e->SwapGateSince = now;
                    e->SwapGateFrames = 0;
                }
                // Pending until an OK read COMPLETES - set after ThRequestReadLocked above, so it is TRUE even when that
                // request was refused, and the hold then lasts to its bound instead of opening with the change unnamed.
                e->SwapGateReadPending = TRUE;
                if (!e->SwapGateBound)
                    e->Due = ThMinTick(e->Due, e->SwapGateSince + TH_HOLD_BOUND_MS);
            }
            else
            {
                // Suppressed (or any other decided banner): no frame gate - its frames are not going out
                // in the first place, and gating an unmapped window would only owe a repaint on release.
                // Just the deadline, so ToastHoldSweep pokes a tracking pass and the new content gets a
                // decision. ThMinTick keeps the EARLIEST deadline, so a burst of change events cannot
                // push it out; the sweep itself re-arms at TH_HOLD_RECHECK_MS while the banner lives.
                e->Due = ThMinTick(e->Due, now + TH_HOLD_BOUND_MS);
            }
            ThRecomputeDueLocked();
        }
    }
    LeaveCriticalSection(&g_ThLock);

    // Outside the lock: this module never calls into send.c under g_ThLock. The gate itself is lock-free,
    // so it is safe on the hook thread, which must never block (the 2026-08-12 input stall).
    if (gateSwap && SendWindowFrameGate(window) == SEND_GATE_NEW)
    {
        LogInfo("QGATOASTGATE hwnd=0x%x frames gated at a window event: this shown banner's window changed, and its "
            "frames, configure and rebuild are held until a read names the cards it shows", (DWORD)(ULONG_PTR)window);
    }
}

// main.c PwConsumeBrokerFrame, BEFORE a toast banner's broker frame is copied into the buffer dom0 maps. TRUE = consume it
// without copying or damaging. `newFrame`: its id differs from the last one judged; `mapped`: dom0 has the window mapped.
//
// THE SETTLE HOLD (ADR-toasts 13, owner-confirmed 2026-11-24): "a window's new frames reach dom0 only after a read has named
// the cards they show, at most 3 s later." Every new frame of a mapped banner closes the hold and requests one read
// (coalesced: in flight -> dirty -> repeated once when it lands), so the pixels never run ahead of the identity - the
// measured double: a forwarded toast painted into a shown banner and copied out before any read named it. A frame already
// judged is held only while the hold is open; after the release it is the newest frame, named, and is copied. ToastHoldDecide
// releases and bounds the hold. Keyed on frames, not on window events: no event reliably reports a repaint in place
// (measured 2026-11-24 nr4: "QGAPROTO,msg=DAMAGE ... ry=18,w=364,h=12" at the swap and no window event).
//
// An UNMAPPED decided banner is not held (dom0 does not see its buffer); with a newer toast queued behind it, its new frame
// is how that toast's in-place swap shows itself, so it requests one read. No frame, no read: a banner at rest is never
// read, which is what lets Windows time it out.
BOOL ToastHoldNoteFrame(IN HWND window, IN BOOL newFrame, IN BOOL mapped)
{
    if (!g_ThActive || !window || g_ThEntryCount == 0) return FALSE;
    const ULONGLONG now = GetTickCount64();
    BOOL held = FALSE, opened = FALSE, issued = FALSE;
    UINT32 queued = 0;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (e && !e->NotBanner && mapped)
    {
        if (newFrame)
        {
            if (e->SwapGateSince == 0)
            {
                e->SwapGateSince = now;
                e->SwapGateFrames = 0;
                opened = TRUE;
            }
            if (e->SwapGateFrames < 0xFFFFFFFFu) e->SwapGateFrames++;
            e->SwapGateReadPending = TRUE;   // before the request: it stands when the worker's queue refuses it
            ThRequestReadLocked(e, now);
            if (!e->SwapGateBound)           // past the bound the stamp is old: arming it again would spin the sweep
            {
                e->Due = ThMinTick(e->Due, e->SwapGateSince + TH_HOLD_BOUND_MS);
                ThRecomputeDueLocked();
            }
            held = TRUE;
        }
        else
            held = (e->SwapGateSince != 0);
    }
    else if (e && !e->NotBanner && newFrame && e->Core.Phase == ThPhaseDecided && e->QueuedNotif != 0)
    {
        const BOOL wasInFlight = e->ReadInFlight;
        ThRequestReadLocked(e, now);
        issued = !wasInFlight && e->ReadInFlight;   // a new request, not a dirty mark on one in flight
        queued = e->QueuedNotif;
    }
    LeaveCriticalSection(&g_ThLock);
    // Outside g_ThLock: send.c's gate is lock-free. It withholds this window's configure and buffer rebuild while held
    // (main.c ToastGateHoldsGeometry); the frames themselves are held by the TRUE returned here.
    if (opened)
    {
        const int claim = SendWindowFrameGate(window);
        if (claim == SEND_GATE_NEW)
            LogInfo("QGATOASTGATE hwnd=0x%x frames gated at a new frame: this mapped banner's frames are held, not copied, "
                "until a read names the cards they show (at most %u ms)", (DWORD)(ULONG_PTR)window, (unsigned)TH_HOLD_BOUND_MS);
        else if (claim == SEND_GATE_REFUSED)
            LogWarning("QGATOASTGATELATE hwnd=0x%x no frame-gate slot is free: its frames are still not copied, but its "
                "configure and buffer rebuild are not withheld", (DWORD)(ULONG_PTR)window);
    }
    // The only record that this path fired: a frame-triggered read leaves no other line (2026-11-24: five toast-hold runs on
    // 1db3aae could not say which of a frame or a window event decided S3i-2). Once per issued read, not per frame.
    if (issued)
        LogInfo("QGATOASTFRAMEREAD hwnd=0x%x id=%u queued behind this decided banner: its new frame requested an "
            "identity read", (DWORD)(ULONG_PTR)window, (ULONG)queued);
    return held;
}

// Collected by main.c's tracking pass, which owns WINDOW_DATA and the visible-region arithmetic. One
// shot: the debt is cleared as it is taken, so a pass that reads it is the pass that pays it.
BOOL ToastHoldTakeRepaintOwed(IN HWND window)
{
    BOOL owed = FALSE;
    if (!g_ThLockInit || !window) return FALSE;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    if (e && e->RepaintOwed)
    {
        e->RepaintOwed = FALSE;
        owed = TRUE;
    }
    LeaveCriticalSection(&g_ThLock);
    return owed;
}

void ToastHoldEvict(IN HWND window)
{
    if (!g_ThLockInit || !window) return;
    EnterCriticalSection(&g_ThLock);
    TH_ENTRY* e = ThFindLocked(window);
    UINT32 queuedNotif = 0, ownNotif = 0;
    UINT queuedSameReads = 0;
    ULONGLONG sinceSameRead = 0;
    TH_END_REPORT endRep;
    TH_NOBANNER nb[TH_IPC_RECORDS];
    int nnb = 0;
    ZeroMemory(&endRep, sizeof(endRep));
    if (e)
    {
        const BOOL wasCard = !e->NotBanner && e->IdentValid;
        // THE BANNER ENDS UNSHOWN (ADR-toasts 13): a record it was still waiting on is claimed for the bridge, which
        // forwards the toast - mapped nowhere is not an end; content that ended with no record is remembered and the
        // record listed later is claimed. A decided record stays consumed (its banner was shown or suppressed).
        ThEndUnshownLocked(e, GetTickCount64(), &endRep);
        queuedNotif = e->QueuedNotif;
        ownNotif = e->Core.NotifId;
        queuedSameReads = e->QueuedSameReads;
        sinceSameRead = e->QueuedLastSameRead ? GetTickCount64() - e->QueuedLastSameRead : 0;
        ZeroMemory(e, sizeof(*e));
        InterlockedDecrement(&g_ThEntryCount);
        // The shell's banner window is free: a toast queued behind this banner gets its turn now - TH_BANNER_END_MS from
        // here for its own banner to appear before the no-banner scan claims it (ThScanUnbanneredLocked).
        if (wasCard && !ThBannerBusyLocked())
        {
            g_ThBusyEnd = GetTickCount64();
            nnb = ThScanUnbanneredLocked(g_ThBusyEnd, nb, TH_IPC_RECORDS);
        }
    }
    ThRecomputeDueLocked();
    LeaveCriticalSection(&g_ThLock);
    ThLogNoBanner(nb, nnb);
    ThLogEndReport(window, L"destroyed", &endRep);
    // Whether Windows ever painted the queued toast here is otherwise unrecorded: a re-read that finds the
    // same content logs nothing. One line per banner that ends with a newer toast still queued behind it.
    if (queuedNotif)
        LogInfo("QGATOASTQUEUED hwnd=0x%x destroyed with id=%lu still queued behind id=%lu: %u re-read(s) named "
            L"id=%lu's content while it was queued, the last %llu ms before the destroy",
            (DWORD)(ULONG_PTR)window, (ULONG)queuedNotif, (ULONG)ownNotif, queuedSameReads, (ULONG)ownNotif,
            (unsigned long long)sinceSameRead);
    // A window destroyed while its frames were held must not leave its handle in a gate slot: the slot
    // would be spent for the life of the process, and a recycled HWND would be born gated. No repaint
    // here - the window is gone, and there is nothing left to repaint.
    SendWindowFrameUngate(window, NULL);
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
    TH_END_REPORT endRep;
    TH_NOBANNER nb[TH_IPC_RECORDS];
    int nnb;
    if (!g_ThActive) return;
    ZeroMemory(&endRep, sizeof(endRep));
    EnterCriticalSection(&g_ThLock);
    // The bridge's verdict event: a record listed AFTER its banner ended unshown with no record is claimed for the bridge,
    // and the no-banner scan arms its deadline for a record listed now (or claims one already past it).
    ThClaimEndedLocked(GetTickCount64(), &endRep);
    nnb = ThScanUnbanneredLocked(GetTickCount64(), nb, TH_IPC_RECORDS);
    for (int i = 0; i < TH_ENTRIES; i++)
        if (g_Th[i].Window && !g_Th[i].NotBanner)
            wake[n++] = g_Th[i].Window;
    LeaveCriticalSection(&g_ThLock);
    ThLogEndReport(NULL, L"ended earlier", &endRep);
    ThLogNoBanner(nb, nnb);
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
    TH_NOBANNER nb[TH_IPC_RECORDS];
    int n = 0, nnb = 0;
    if (!g_ThActive || g_ThNextDue == 0) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_ThNextDue) return;
    EnterCriticalSection(&g_ThLock);
    if (g_ThScanDue != 0 && now >= g_ThScanDue)
        nnb = ThScanUnbanneredLocked(now, nb, TH_IPC_RECORDS);
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
    ThLogNoBanner(nb, nnb);
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
    TOAST_IDENT cardIdent[TOAST_CARDS_MAX];
    int cardCount = 0, oldCount = 0;
    ZeroMemory(cardIdent, sizeof(cardIdent));
    if (status == TH_READ_OK && texts && texts->Count > 0 && texts->Count <= TOAST_CARDS_MAX)
    {
        // The entry's own identity is the BOTTOM card's - the oldest, the one this window showed first. Every
        // card's identity is kept for the stack (ToastHoldDecide matches the ones above it to their records).
        const TOAST_CARD_ONE* bottom = &texts->Card[texts->Count - 1];
        cardCount = texts->Count;
        for (int k = 0; k < cardCount; k++)
            TiIdentFromTexts(texts->Card[k].Sender, texts->Card[k].Title, texts->Card[k].Message, &cardIdent[k]);
        ident = cardIdent[cardCount - 1];                                         // the same normalization the bridge applies
        msgLen = TiNormalize(bottom->Message, msgNorm, TI_NORM_MAX);             // kept in clear for the re-claim rule only
    }
    else if (status == TH_READ_OK)
        status = TH_READ_UIAFAIL;
    const ULONGLONG now = GetTickCount64();
    BOOL again = FALSE, logNoCard = FALSE, changed = FALSE, notBanner = FALSE, stillGrowing = FALSE;
    UINT64 before = 0, after = 0;
    UINT withoutTitle = 0;
    LONG e_firstW = 0, e_firstH = 0, e_seenW = 0, e_seenH = 0;   // copied under the lock, for the log below
    TH_END_REPORT endRep;   // cards that left the stack still undecided (ADR-toasts 13), said after the lock
    ZeroMemory(&endRep, sizeof(endRep));

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
    const BOOL uiaFailed = (status == TH_READ_UIAFAIL);   // on a banner still tracked: a read racing a close was dropped above
    if (status == TH_READ_OK)
    {
        before = e->IdentValid ? e->Ident.Combined : 0;
        after = ident.Combined;
        changed = (before != after);
        if (!changed && e->QueuedNotif)
        {
            e->QueuedSameReads++;
            e->QueuedLastSameRead = now;
        }
        // The stack, keyed by CONTENT: a card that was there before keeps its record, verdict and first-seen
        // tick wherever it now sits; a new card is kept out until its record says otherwise. A new bottom card
        // that was a card above it before keeps that card's record as its own (PromotedSeq).
        {
            TH_STACK_CARD next[TOAST_CARDS_MAX];
            ZeroMemory(next, sizeof(next));
            oldCount = e->StackCount;
            for (int k = 0; k < cardCount; k++)
            {
                next[k].Ident = cardIdent[k];
                next[k].Rect = texts->Card[k].Rect;
                next[k].FirstSeen = now;
                next[k].Titled = texts->Card[k].Titled;
                next[k].Excluded = TRUE;
                for (int j = 0; j < oldCount; j++)
                    if (e->Stack[j].Ident.Combined != 0 && e->Stack[j].Ident.Combined == cardIdent[k].Combined)
                    {
                        next[k].Seq = e->Stack[j].Seq;
                        next[k].NotifId = e->Stack[j].NotifId;
                        next[k].Verdict = e->Stack[j].Verdict;
                        next[k].FirstSeen = e->Stack[j].FirstSeen;
                        next[k].Excluded = e->Stack[j].Excluded;
                        // The inverse of PromotedSeq: the entry's own card moved UP (a card was inserted below it and
                        // is the bottom card now). Its record lives only in e->Core, which the next pass zeroes, and
                        // it stays consumed, so the card could never match it again: undecided, it was shown 3 s
                        // later beside the new bottom card although the bridge had forwarded it (DEMOTED-PLAIN). It
                        // keeps the record and the entry's standing decision as its verdict; ToastHoldDecide re-reads
                        // the record every pass. Not after a fail-open show: that card stays shown, as decided.
                        if (changed && j == oldCount - 1 && k < cardCount - 1 && e->Core.Phase == ThPhaseDecided &&
                            e->Core.RecordSeq != 0 && !e->Core.FailedOpen && e->Core.Ident == cardIdent[k].Combined)
                        {
                            next[k].Seq = e->Core.RecordSeq;
                            next[k].NotifId = e->Core.NotifId;
                            next[k].Verdict = e->Core.Decision != ThDecSuppress ? TH_VERDICT_WINDOW :
                                              e->Core.AckSeen ? TH_VERDICT_FORWARDED : TH_VERDICT_BRIDGE;
                            next[k].Excluded = (e->Core.Decision == ThDecSuppress);
                        }
                        if (k == cardCount - 1 && j < oldCount - 1 && e->Stack[j].Seq != 0 && changed)
                            e->PromotedSeq = e->Stack[j].Seq;
                        break;
                    }
            }
            // A card above the bottom one that this read no longer shows ended unshown if it was still kept out undecided:
            // its pending record is claimed for the bridge, a title with no record is remembered (ADR-toasts 13).
            for (int j = 0; j < oldCount - 1 && j < TOAST_CARDS_MAX; j++)
            {
                BOOL still = FALSE;
                for (int k = 0; k < cardCount; k++)
                    if (e->Stack[j].Ident.Combined != 0 && e->Stack[j].Ident.Combined == cardIdent[k].Combined) still = TRUE;
                if (still) continue;
                if (e->Stack[j].Seq && e->Stack[j].Verdict == TH_VERDICT_PENDING)
                    ThEndClaimLocked(e->Stack[j].Seq, e->Stack[j].NotifId, e->Stack[j].Ident.Combined, &endRep);
                else if (!e->Stack[j].Seq && e->Stack[j].Excluded)
                    ThRememberEndedLocked(&e->Stack[j].Ident, now, &endRep);
            }
            memcpy(e->Stack, next, sizeof(next));
            e->StackCount = cardCount;
            e->StackRaw = texts->Raw;
            e->StackRectsValid = texts->RectsValid;
            e->BottomStacks = texts->Card[cardCount - 1].Stacks;
        }
        e->CardSeen = TRUE;
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
        if (status == TH_READ_NOTITLE)
            e->CardSeen = TRUE;   // cards were found; the bottom one is still populating
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
                e->NoCardFirstW = e->SeenW;
                e->NoCardFirstH = e->SeenH;
            }
            else if (ThCoreNoCardCounts(e->NoCardFirstReq, e->LastReadReq))
            {
                // A WINDOW THAT CHANGED SIZE BETWEEN THE TWO LOOKS IS STILL BUILDING, AND IS NOT A
                // FLYOUT. THIS IS THE TOAST LOSS, measured 2026-11-07 on win11r-thold (the campaign's
                // toast-hold run, S3i-2=LOST) from the agent's own log - no screen needed:
                //   044457.080 CREATE hwnd=0x6019e 396x200 -> ToastHoldDecide hold reason=no-identity
                //   044457.117 PwAttachWindowCarry 0x6019e 364x157        <- it is still growing
                //   044457.126 ApplyIdentity: card not readable yet (no-card, read 1)
                //   044457.357 ToastHoldDecide: "not a toast banner (no NormalToastView in 2 reads,
                //               364x157) - shown, never held" -> SendWindowMap
                // The two looks were 250 ms apart, so ThCoreNoCardCounts (review N2's pacing rule)
                // counted them and the banner was classified a flyout 277 ms after it appeared - while
                // EVERY toast that run identified needed far longer for its card: held_ms 797, 1390,
                // 1485, 1547. So the paced-second-look rule, on its own, cannot tell a flyout from a
                // banner mid-grow; 250 ms was simply never enough, and raising it would hold real
                // flyouts (volume, Quick Settings, clock) for a second and make them stutter.
                // The SIZE does tell them apart, and the agent already has it: this banner went
                // 396x200 -> 364x157 between the looks, while a flyout stands still. A changed size
                // restarts the count with THIS read as the first look, so the hold continues and the
                // toast gets its verdict; an unchanged size concludes flyout as fast as before.
                const BOOL grew = (e->NoCardFirstW > 0 && e->NoCardFirstH > 0 && e->SeenW > 0 && e->SeenH > 0 &&
                                   (e->SeenW != e->NoCardFirstW || e->SeenH != e->NoCardFirstH));
                if (grew)
                {
                    e_firstW = e->NoCardFirstW; e_firstH = e->NoCardFirstH;   // the pair the log names
                    e_seenW = e->SeenW;         e_seenH = e->SeenH;
                    e->NoCardFirstReq = e->LastReadReq;   // this look becomes the first of a new pair
                    e->NoCardFirstW = e->SeenW;
                    e->NoCardFirstH = e->SeenH;
                    stillGrowing = TRUE;
                }
                else
                {
                    e->NoCardReads = 2;
                    if (!e->IdentValid && !e->CardSeen)
                    {
                        e->NotBanner = TRUE;   // two paced looks at the SAME size, no card: a flyout - ToastHoldDecide shows it
                        notBanner = TRUE;
                    }
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
    // The settle hold waits for a read that NAMED the cards and was requested after the newest held frame. A failed or
    // card-less read names nothing; a dirty one was requested before a frame (or event) that came after it, and the repeat
    // just issued - or refused, in which case the flag stands to the bound - is the one that counts.
    if (status == TH_READ_OK && !again)
        e->SwapGateReadPending = FALSE;
    LeaveCriticalSection(&g_ThLock);
    ThLogEndReport(window, L"left the stack", &endRep);

    // The guard above, said out loud: a card-less look that did NOT count because the window is still
    // building. toast-hold-grade.py keys on QGATOASTIDENT, and this is the line that explains a hold
    // that outlived the pacing interval instead of being released as a flyout.
    if (stillGrowing)
        LogInfo("QGATOASTIDENT hwnd=0x%x card-less look does NOT count: the window changed size since the "
            "first one (%ldx%ld -> %ldx%ld), so it is a banner still building, not a flyout - this look "
            "becomes the first of a new pair and the hold continues to the banner's end (%u ms)",
            (DWORD)(ULONG_PTR)window, (long)e_firstW, (long)e_firstH, (long)e_seenW, (long)e_seenH,
            (unsigned)TH_BANNER_END_MS);
    // A UIA call that FAILED (ElementFromHandle/Find) is not a card still building: it does not happen in normal
    // operation (owner 2026-11-24: "classification errors and timeouts should not happen. it is a sign of something
    // going wrong") - said when it happens, not only by the banner's end. The read is re-requested as before.
    if (uiaFailed)
        LogError("QGATOASTIDENT hwnd=0x%x identity read FAILED (%s): the UI Automation call on a tracked banner failed - "
            L"the banner stays held, the next read is requested", (DWORD)(ULONG_PTR)window, ThReadStatusName(status));
    if (logNoCard)
        LogInfo("QGATOASTIDENT hwnd=0x%x card not readable yet (%s, read %u): no card / no Title|TitleText block - "
            L"held until a read succeeds, a second card-less read says not-a-banner, or the banner's end (%u ms, NOT mapped) ends it",
            (DWORD)(ULONG_PTR)window, ThReadStatusName(status), withoutTitle, (unsigned)TH_BANNER_END_MS);
    else if (status == TH_READ_OK && changed)
        LogDebug("QGATOASTIDENT hwnd=0x%x identity %016llx -> %016llx (s=%016llx t=%016llx m=%016llx)", (DWORD)(ULONG_PTR)window,
            (unsigned long long)before, (unsigned long long)after, (unsigned long long)ident.Sender,
            (unsigned long long)ident.Title, (unsigned long long)ident.Message);

    // The decision is taken on the tracking pass, never here.
    PokeWindowTrackingFor(window);
}
