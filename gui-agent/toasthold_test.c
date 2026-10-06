/*
 * toasthold_test - offline suite for the toast-banner hold: the shared identity contract
 * (toastident.h: normalization, hashing, the content matcher, the bridge->agent record ring) and
 * the agent's pure hold state machine (toasthold-core.h).
 *
 * Self-contained: no rig, no UIA, no COM, no bridge. On the CI Windows runner right after msbuild
 * (agent/vs2022/toasthold-test), and on any host with a C compiler:
 *   gcc -std=c99 -Wall -Wextra -Werror -I. toasthold_test.c -o toasthold_test && ./toasthold_test
 * Exit 0 = every case matched; nonzero = at least one mismatch.
 *
 * WHAT IT PROVES (docs/ADR-toasts.md 10, measured facts of 2026-10-04):
 *   identity  the bridge's listing text (DisplayName, text[0], text[1..] joined by '\n') and the
 *             banner's UIA text (SenderName, Title|TitleText, MessageText blocks joined by ' ')
 *             hash to the SAME identity; whitespace, case (ASCII + Latin-1), bidi marks and NBSP do
 *             not matter; both AutomationId sets (Win11 Title / Win10 TitleText) are recognised and
 *             the localized card Name is never consulted.
 *   matcher   content first, arrival order only as the tie-break: a bannerless notification listed
 *             BEFORE the one on screen must NOT claim its banner (the desync that would misroute a
 *             window-path toast to nowhere); identical toasts resolve FIFO; a partial match (same
 *             sender+title, different message) is taken only when it is the unique candidate.
 *   ring      publish/read is torn-proof (seqlock), verdicts update in place, a verdict settled by the
 *             listing (WindowOnly, allowlist, forward outcome) is never overridden by the classifier's
 *             late answer, and a verdict outside the contract reads as `window` (fail open).
 *   machine   pending -> bridge (never mapped), pending -> window (mapped at the verdict), pending ->
 *             timeout (mapped, FAIL-OPEN reported), no identity / no record -> timeout, bridge down at
 *             the start -> shown at once, bridge death while held -> shown at once, content change in
 *             place -> a fresh hold, a reserved pending record released when the content turns out
 *             different, pre-emption of a mapped window-path banner by a queued bridge-bound toast and
 *             its end, and no flicker while a re-read is in flight.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts as evidence only once it has been seen to FAIL).
 * Each define below removes one guard; this suite MUST then exit nonzero (tools/tests/toasthold-
 * selftest.sh runs the matrix with gcc; CI runs it with msbuild /p:ToastHoldDefect=<define>):
 *   TOASTIDENT_DEFECT_NOFOLD           multi-line bodies stop matching (every such toast doubles)
 *   TOASTIDENT_DEFECT_ORDERONLY        the matcher goes by arrival order alone (the desync case)
 *   TOASTIDENT_DEFECT_VERDICTOVERRIDE  the classifier overrides a settled route
 *   TOASTHOLD_DEFECT_NOBOUND           a hold never fails open (a lost toast instead of a late one)
 *   TOASTHOLD_DEFECT_FAILCLOSED        doubt suppresses instead of showing
 *   TOASTHOLD_DEFECT_NOPREEMPT         a queued bridge-bound toast no longer pre-empts (the flash)
 *   TOASTHOLD_DEFECT_PREEMPT_IDENTGATE pre-emption ends while an identity read is in flight: the in-place
 *                                      swap's own event queues a read, so the swap pass MAPS the window while
 *                                      the bridged toast paints (review blocker #1; TestReviewProbe step 3)
 *   TOASTHOLD_DEFECT_SUPPRESS_FINAL    a suppressed banner never re-reads its record: the bridge's later
 *                                      `window` and its death before `forwarded` are ignored - a LOSS (review
 *                                      blocker #2; TestReviewProbe step 5, TestSuppressReopen)
 *   TOASTHOLD_DEFECT_NORECLAIM         a banner whose identity merely completed (half-built card on the first
 *                                      read) loses the record it consumed -> no record -> 3 s -> double (#5)
 *   TOASTHOLD_DEFECT_NOIDENT_IGNORES_BRIDGE a banner with no identity yet waits out 3 s although the bridge is
 *                                      down and no record can ever come (#13)
 *   TOASTHOLD_DEFECT_NOFORWARDBOUND    a suppression awaiting dom0's ack never reopens (second review #4;
 *                                      TestForwardBound)
 *   TOASTHOLD_DEFECT_NOCARD_UNPACED    an unpaced second card-less read classes a banner mid-grow as a flyout
 *                                      (N2; TestPureRules)
 *   TOASTHOLD_DEFECT_SIZE60            the absolute 60 % ceiling that never held a 573 px banner at 768/900 px
 *                                      screens or 125 %+ DPI (N3; TestPureRules)
 *   TOASTHOLD_DEFECT_DEADRECORDS       a dead bridge's records stay authoritative / keep pre-empting (N6)
 *   TOASTHOLD_DEFECT_NOBACKOFF         a refused identity request is retried without back-off (N7)
 *   TOASTIDENT_DEFECT_TIE_SLOTORDER    equal arrival ticks broken by ring slot: across a wrap the newer record
 *                                      (slot 0) is taken for the earlier banner (#12; TestSelect)
 *   TOASTHOLD_DEFECT_RECLAIM_ANY       the own consumed record is re-claimable for ANY new in-place content: a
 *                                      new same-title toast inherits another toast's verdict (N5; TestReclaim)
 *   TOASTIDENT_DEFECT_MARK_BY_SLOT     the agent's shown mark is a flag in the slot, not the sequence it marks: a
 *                                      store racing the bridge's republish of the slot makes the NEW toast read as
 *                                      shown, and its failed dom0 action goes unreported (ADR-toasts 11; TestRing)
 */

#include "toasthold-core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_run = 0, g_fail = 0;

static void Check(const char* name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}

/* ---- UTF-16 literals on any host: ASCII in, WCHAR out (a rotating pool of buffers) ---------- */
static WCHAR g_pool[32][256];
static int g_poolNext = 0;
static const WCHAR* W(const char* ascii)
{
    WCHAR* b = g_pool[g_poolNext++ % 32];
    size_t i;
    for (i = 0; ascii[i] && i < 255; i++) b[i] = (WCHAR)(unsigned char)ascii[i];
    b[i] = 0;
    return b;
}
static const WCHAR* WU(const unsigned* units)   /* explicit code units for non-ASCII */
{
    WCHAR* b = g_pool[g_poolNext++ % 32];
    size_t i;
    for (i = 0; units[i] && i < 255; i++) b[i] = (WCHAR)units[i];
    b[i] = 0;
    return b;
}
static int UnitsEq(const WCHAR* a, size_t n, const char* ascii)
{
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != (WCHAR)(unsigned char)ascii[i]) return 0;
    return ascii[n] == 0;
}

/* ---- identity ----------------------------------------------------------------------------- */
static void TestNormalization(void)
{
    WCHAR out[64];
    size_t n;

    n = TiNormalize(W("  Windows   PowerShell  "), out, 64);
    Check("normalize: trims and collapses runs", UnitsEq(out, n, "windows powershell"));

    n = TiNormalize(W("line2\nline3"), out, 64);
    Check("normalize: newline folds to one space (bridge body spelling)", UnitsEq(out, n, "line2 line3"));

    n = TiNormalize(W("line2 line3"), out, 64);
    Check("normalize: space stays one space (UIA block spelling)", UnitsEq(out, n, "line2 line3"));

    {
        static const unsigned nbsp[] = { 'a', 0xA0, 0xA0, 'b', 0x200E, 'c', 0x2028, 'd', 0 };   /* NBSP x2, LRM, line sep */
        n = TiNormalize(WU(nbsp), out, 64);
        Check("normalize: NBSP/line separator are spaces, bidi mark is dropped", UnitsEq(out, n, "a b cd") || UnitsEq(out, n, "a bc d"));
        /* exact: 'a' NBSP NBSP 'b' -> "a b"; LRM dropped -> "a bc"; LSEP -> ' '; 'd' -> "a bc d" */
        Check("normalize: exact result", UnitsEq(out, n, "a bc d"));
    }
    {
        static const unsigned latin[] = { 0xC4, 'r', 'g', 'e', 'r', 0 };   /* "Ärger" */
        n = TiNormalize(WU(latin), out, 64);
        Check("normalize: Latin-1 capital folds", n == 5 && out[0] == 0xE4 && out[1] == 'r');
    }
    {
        static const unsigned mult[] = { 'A', 0xD7, 'B', 0 };   /* multiplication sign is not a letter */
        n = TiNormalize(WU(mult), out, 64);
        Check("normalize: U+00D7 is not folded", n == 3 && out[1] == 0xD7);
    }
    n = TiNormalize(NULL, out, 64);
    Check("normalize: NULL is empty", n == 0);
    n = TiNormalize(W("   \n  "), out, 64);
    Check("normalize: whitespace-only is empty", n == 0);

    Check("hash: empty is 0", TiHashUnits(out, 0) == 0);
    Check("hash: non-empty is never 0", TiHashUnits(W("x"), 1) != 0);
}

static void TestIdentityAgreement(void)
{
    TOAST_IDENT bridge, uia, other;

    /* the measured probe toast, both spellings */
    TiIdentFromTexts(W("Windows PowerShell"), W("PROBE A1"), W("banner window probe"), &bridge);
    TiIdentFromTexts(W("Windows PowerShell"), W("PROBE A1"), W("banner window probe"), &uia);
    Check("identity: listing and banner of the probe toast agree", bridge.Combined == uia.Combined && bridge.Combined != 0);

    /* three text lines: the listener joins with '\n', the banner's blocks are joined with ' ' */
    TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice\nMeeting moved to 3pm, see attached agenda"), &bridge);
    TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice Meeting moved to 3pm, see attached agenda"), &uia);
    Check("identity: multi-line body agrees across the two spellings", bridge.Combined == uia.Combined);
    Check("identity: ...and so does the message hash", bridge.Message == uia.Message && bridge.Message != 0);
    Check("identity: ...with a FULL match", TiMatch(&bridge, &uia) == TiMatchFull);

    /* the sender missing on the listener side (AppInfo resolution failed) still matches */
    TiIdentFromTexts(NULL, W("PROBE A1"), W("banner window probe"), &bridge);
    TiIdentFromTexts(W("Windows PowerShell"), W("PROBE A1"), W("banner window probe"), &uia);
    Check("identity: a missing sender on one side is not evidence against", TiMatch(&bridge, &uia) == TiMatchFull);
    Check("identity: Sender hash 0 means absent", bridge.Sender == 0 && uia.Sender != 0);

    /* different senders, same words: two toasts */
    TiIdentFromTexts(W("Command Prompt"), W("PROBE A1"), W("banner window probe"), &other);
    Check("identity: a different sender with the same text is a different toast", TiMatch(&other, &uia) == TiMatchNone);

    /* different title: none */
    TiIdentFromTexts(W("Windows PowerShell"), W("PROBE A2"), W("banner window probe"), &other);
    Check("identity: a different title never matches", TiMatch(&other, &uia) == TiMatchNone);

    /* no title anywhere: nothing can be tied */
    TiIdentFromTexts(W("App"), NULL, W("body only"), &other);
    Check("identity: no title on the record -> none", TiMatch(&other, &uia) == TiMatchNone);

    /* long body with an appended attribution line: prefix tolerance -> FULL */
    TiIdentFromTexts(W("News"), W("Breaking"), W("The quick brown fox jumps over the lazy dog twice today"), &bridge);
    TiIdentFromTexts(W("News"), W("Breaking"), W("The quick brown fox jumps over the lazy dog twice today Via Example News"), &uia);
    Check("identity: a long body with extra trailing text matches by prefix", TiMatch(&bridge, &uia) == TiMatchFull);

    /* short body differing: PARTIAL (same sender+title), never NONE, never FULL */
    TiIdentFromTexts(W("News"), W("Breaking"), W("short one"), &bridge);
    TiIdentFromTexts(W("News"), W("Breaking"), W("short two"), &uia);
    Check("identity: same sender+title, different short body -> PARTIAL", TiMatch(&bridge, &uia) == TiMatchPartial);

    /* one side has no message at all: PARTIAL */
    TiIdentFromTexts(W("News"), W("Breaking"), NULL, &bridge);
    Check("identity: a message missing on one side -> PARTIAL", TiMatch(&bridge, &uia) == TiMatchPartial);
    /* both without a message: FULL */
    TiIdentFromTexts(W("News"), W("Breaking"), W(""), &uia);
    Check("identity: no message on either side -> FULL", TiMatch(&bridge, &uia) == TiMatchFull);

    /* case and whitespace: the same toast */
    TiIdentFromTexts(W("WINDOWS POWERSHELL"), W("probe a1"), W("Banner  Window\tProbe"), &bridge);
    TiIdentFromTexts(W("Windows PowerShell"), W("PROBE A1"), W("banner window probe"), &uia);
    Check("identity: case and whitespace differences are the same toast", bridge.Combined == uia.Combined);

    Check("identity: empty everything has Combined 0", (TiIdentFromTexts(NULL, NULL, NULL, &other), other.Combined == 0));
}

static void TestAutomationIds(void)
{
    Check("aid: NormalToastView is the card", TiIsCardAutomationId(W("NormalToastView")));
    Check("aid: FlexibleToastView is NOT the card key (ClassName, Win11 only)", !TiIsCardAutomationId(W("FlexibleToastView")));
    Check("aid: SenderName", TiIsSenderAutomationId(W("SenderName")));
    Check("aid: Title (Win11)", TiIsTitleAutomationId(W("Title")));
    Check("aid: TitleText (Win10)", TiIsTitleAutomationId(W("TitleText")));
    Check("aid: TitleX is not a title", !TiIsTitleAutomationId(W("TitleX")));
    Check("aid: MessageText", TiIsMessageAutomationId(W("MessageText")));
    Check("aid: MessageText2 (a numbered sibling) is message text", TiIsMessageAutomationId(W("MessageText2")));
    Check("aid: DismissTextBlock is not message text", !TiIsMessageAutomationId(W("DismissTextBlock")));
    Check("aid: VerbText (button label) is not message text", !TiIsMessageAutomationId(W("VerbText")) && !TiIsTitleAutomationId(W("VerbText")));
    Check("aid: empty id is nothing", !TiIsMessageAutomationId(W("")) && !TiIsTitleAutomationId(W("")) && !TiIsSenderAutomationId(W("")));
    Check("aid: NULL is nothing", !TiIsMessageAutomationId(NULL) && !TiIsCardAutomationId(NULL));
}

/* ---- the matcher over candidate records ----------------------------------------------------- */
static void Cand(TI_CANDIDATE* c, const char* sender, const char* title, const char* msg, UINT64 tick, LONG seq, int eligible)
{
    TiIdentFromTexts(W(sender), W(title), W(msg), &c->Ident);
    c->ArrivalTick = tick; c->Seq = seq; c->Eligible = eligible;
}

static void TestSelect(void)
{
    TI_CANDIDATE c[4];
    TOAST_IDENT seen;
    TI_MATCH q;
    int i;

    /* THE DESYNC CASE. A (older) produced no banner (do-not-disturb for that app); B's banner is on
     * screen. Order alone would hand A's record - and A's verdict - to B's banner. */
    Cand(&c[0], "Weather", "Rain later", "Take an umbrella", 1000, 1, 1);
    Cand(&c[1], "Mail", "New message", "From Bob", 1500, 2, 1);
    TiIdentFromTexts(W("Mail"), W("New message"), W("From Bob"), &seen);
    i = TiSelect(c, 2, &seen, &q);
    Check("select: a bannerless OLDER record does not claim the displayed banner (content wins)", i == 1 && q == TiMatchFull);

    /* identical toasts: FIFO tie-break */
    Cand(&c[0], "Dl", "Download complete", "file.zip", 2000, 3, 1);
    Cand(&c[1], "Dl", "Download complete", "file.zip", 2600, 4, 1);
    TiIdentFromTexts(W("Dl"), W("Download complete"), W("file.zip"), &seen);
    i = TiSelect(c, 2, &seen, &q);
    Check("select: identical toasts resolve to the EARLIEST arrival", i == 0 && q == TiMatchFull);
    c[0].Eligible = 0;   /* consumed by the first banner */
    i = TiSelect(c, 2, &seen, &q);
    Check("select: ...and the next banner takes the next one", i == 1);

    /* FULL beats PARTIAL even when the partial arrived first */
    Cand(&c[0], "News", "Breaking", "story one", 3000, 5, 1);
    Cand(&c[1], "News", "Breaking", "story two", 3100, 6, 1);
    TiIdentFromTexts(W("News"), W("Breaking"), W("story two"), &seen);
    i = TiSelect(c, 2, &seen, &q);
    Check("select: a FULL match beats an earlier PARTIAL", i == 1 && q == TiMatchFull);

    /* two PARTIALs, no FULL: nothing is claimed (the message was the only discriminator) */
    TiIdentFromTexts(W("News"), W("Breaking"), W("story three"), &seen);
    i = TiSelect(c, 2, &seen, &q);
    Check("select: two partial candidates -> none claimed (fail open, logged)", i == -1 && q == TiMatchNone);

    /* one PARTIAL alone: accepted (a body the banner renders differently) */
    c[1].Eligible = 0;
    i = TiSelect(c, 2, &seen, &q);
    Check("select: the unique partial candidate is accepted", i == 0 && q == TiMatchPartial);

    /* nothing eligible */
    c[0].Eligible = 0;
    i = TiSelect(c, 2, &seen, &q);
    Check("select: nothing eligible -> none", i == -1);

    /* a record whose title differs is never taken, however old */
    Cand(&c[0], "Other", "Else", "x", 100, 7, 1);
    i = TiSelect(c, 1, &seen, &q);
    Check("select: an unrelated record is never taken", i == -1);

    /* #12: equal arrival ticks (one listing pass) are broken by SEQUENCE, not by slot - across a ring wrap
     * the slot order is the reverse of the sequence order. Build it the way the glue does: ThIpcRead over
     * the wrapped ring, slot by slot. */
    {
        unsigned char block[TH_IPC_BYTES];
        TH_IPC_HEADER* h = (TH_IPC_HEADER*)block;
        TH_IPC_RECORD recs[TH_IPC_RECORDS];
        TI_CANDIDATE cand[TH_IPC_RECORDS];
        TH_CONSUMED cons;
        TOAST_IDENT same, filler;
        int n = 0, k, pick;
        memset(&cons, 0, sizeof(cons));
        ThIpcInit(h);
        TiIdentFromTexts(W("Dl"), W("Download complete"), W("file.zip"), &same);
        TiIdentFromTexts(W("X"), W("unrelated"), W("y"), &filler);
        for (k = 0; k < TH_IPC_RECORDS - 1; k++) ThIpcPublish(h, 500 + (UINT32)k, 0, 0, &filler, TH_VERDICT_WINDOW, 1000);   /* seq 1..31 */
        ThIpcPublish(h, 601, 0, 0, &same, TH_VERDICT_WINDOW, 7000);   /* seq 32 -> slot 31: the EARLIER identical toast */
        ThIpcPublish(h, 602, 0, 0, &same, TH_VERDICT_BRIDGE, 7000);   /* seq 33 -> slot 0 (wrap): the LATER one, same tick */
        for (k = 0; k < TH_IPC_RECORDS; k++) if (ThIpcRead(h, k, &recs[n])) n++;
        Check("select: (setup) the wrap put seq 33 into slot 0 and seq 32 into slot 31", recs[0].Seq == 33 && recs[n - 1].Seq == 32);
        ThCoreCandidates(recs, n, &cons, 0, 7100, cand);
        pick = TiSelect(cand, n, &same, &q);
        Check("select: equal ticks across a ring wrap -> the EARLIER sequence (32) wins, whatever slot it sits in", pick >= 0 && cand[pick].Seq == 32 && recs[pick].NotifId == 601);
        ThConsumedAdd(&cons, 32);
        ThCoreCandidates(recs, n, &cons, 0, 7100, cand);
        pick = TiSelect(cand, n, &same, &q);
        Check("select: ...and the next banner takes seq 33", pick >= 0 && cand[pick].Seq == 33);
        Check("select: TiEarlier - same tick, lower seq is earlier; lower tick wins regardless of seq",
              TiEarlier(&cand[n - 1], &cand[0]) && !TiEarlier(&cand[0], &cand[n - 1]) && TiEarlier(&cand[1], &cand[0]));
    }
}

/* ---- the shared ring -------------------------------------------------------------------------- */
static void TestRing(void)
{
    unsigned char block[TH_IPC_BYTES];
    TH_IPC_HEADER* h = (TH_IPC_HEADER*)block;
    TH_IPC_RECORD r;
    TOAST_IDENT id1, id2;
    LONG s1, s2, s3;
    int i, found;

    memset(block, 0xAB, sizeof(block));
    Check("ring: a garbage block is not valid", !ThIpcValid(h));
    ThIpcInit(h);
    Check("ring: initialised block is valid", ThIpcValid(h));
    Check("ring: layout - record is 80 bytes, header 64", sizeof(TH_IPC_RECORD) == 80 && sizeof(TH_IPC_HEADER) == 64);
    Check("ring: empty slot reads FALSE", !ThIpcRead(h, 0, &r));
    Check("ring: out-of-range index reads FALSE", !ThIpcRead(h, TH_IPC_RECORDS, &r) && !ThIpcRead(h, -1, &r));

    TiIdentFromTexts(W("A"), W("one"), W("x"), &id1);
    TiIdentFromTexts(W("B"), W("two"), W("y"), &id2);
    s1 = ThIpcPublish(h, 118, 0, 0x1111, &id1, TH_VERDICT_PENDING, 5000);
    s2 = ThIpcPublish(h, 119, TH_REC_FLAG_ALLOWLISTED, 0x2222, &id2, TH_VERDICT_BRIDGE, 5100);
    Check("ring: sequences are 1, 2", s1 == 1 && s2 == 2);
    Check("ring: slot 0 reads record 118 pending", ThIpcRead(h, 0, &r) && r.NotifId == 118 && r.Verdict == TH_VERDICT_PENDING && r.Seq == 1 && r.ArrivalTick == 5000 && r.Ident.Combined == id1.Combined);
    Check("ring: slot 1 reads record 119 bridge", ThIpcRead(h, 1, &r) && r.NotifId == 119 && r.Verdict == TH_VERDICT_BRIDGE && r.Flags == TH_REC_FLAG_ALLOWLISTED);

    Check("ring: verdict pending -> bridge updates in place", ThIpcSetVerdict(h, 118, TH_VERDICT_BRIDGE, TRUE) && ThIpcRead(h, 0, &r) && r.Verdict == TH_VERDICT_BRIDGE);
    Check("ring: onlyIfPending refuses to override a settled verdict", !ThIpcSetVerdict(h, 118, TH_VERDICT_WINDOW, TRUE) && ThIpcRead(h, 0, &r) && r.Verdict == TH_VERDICT_BRIDGE);
    Check("ring: an unconditional update overrides (forward failed -> window)", ThIpcSetVerdict(h, 118, TH_VERDICT_WINDOW, FALSE) && ThIpcRead(h, 0, &r) && r.Verdict == TH_VERDICT_WINDOW);
    Check("ring: unknown id -> not found", !ThIpcSetVerdict(h, 999, TH_VERDICT_WINDOW, FALSE));

    /* forwarded: the ack'd state, valid and suppressing; set unconditionally by the listing after dom0's ack */
    Check("ring: bridge -> forwarded (unconditional) reads back as forwarded", ThIpcSetVerdict(h, 119, TH_VERDICT_FORWARDED, FALSE) && ThIpcRead(h, 1, &r) && r.Verdict == TH_VERDICT_FORWARDED);
    Check("ring: forwarded suppresses like bridge; pending/window do not", TiVerdictSuppresses(TH_VERDICT_FORWARDED) && TiVerdictSuppresses(TH_VERDICT_BRIDGE) && !TiVerdictSuppresses(TH_VERDICT_WINDOW) && !TiVerdictSuppresses(TH_VERDICT_PENDING));
    Check("ring: the classifier's late 'bridge' cannot override forwarded (compare-exchange from PENDING only)", !ThIpcSetVerdict(h, 119, TH_VERDICT_BRIDGE, TRUE) && ThIpcRead(h, 1, &r) && r.Verdict == TH_VERDICT_FORWARDED);
    /* the compare-exchange primitive itself */
    {
        volatile LONG x = TH_VERDICT_PENDING;
        Check("cas: PENDING -> BRIDGE succeeds and returns the value seen (PENDING)", TiCas32(&x, TH_VERDICT_BRIDGE, TH_VERDICT_PENDING) == TH_VERDICT_PENDING && x == TH_VERDICT_BRIDGE);
        Check("cas: a second PENDING -> WINDOW fails and leaves BRIDGE", TiCas32(&x, TH_VERDICT_WINDOW, TH_VERDICT_PENDING) == TH_VERDICT_BRIDGE && x == TH_VERDICT_BRIDGE);
    }
    /* the display mode the agent publishes for the bridge (non-seamless: forward nothing) */
    Check("ring: header Seamless is 0 after init (the agent publishes the mode itself)", TI_LOAD32(&h->Seamless) == 0);
    TI_STORE32(&h->Seamless, 1);
    Check("ring: Seamless reads back 1 after the agent's store", TI_LOAD32(&h->Seamless) == 1 && ThIpcValid(h));

    /* an out-of-contract verdict (hostile or corrupt writer) reads as WINDOW */
    TI_STORE32(&ThIpcRecords(h)[1].Verdict, 77);
    Check("ring: a verdict outside the contract reads as window (fail open)", ThIpcRead(h, 1, &r) && r.Verdict == TH_VERDICT_WINDOW);

    /* a slot being rewritten (Seq cleared) is not read */
    TI_STORE32(&ThIpcRecords(h)[1].Seq, 0);
    Check("ring: a slot under rewrite (Seq 0) is not read", !ThIpcRead(h, 1, &r));

    /* wrap: 31 more publishes land seq 3..33; seq 33 reuses slot 0 */
    for (i = 0; i < 31; i++) s3 = ThIpcPublish(h, 200 + (UINT32)i, 0, 0, &id1, TH_VERDICT_PENDING, 6000 + (UINT64)i);
    Check("ring: 33rd publish wraps into slot 0 with seq 33", s3 == 33 && ThIpcRead(h, 0, &r) && r.Seq == 33 && r.NotifId == 230);
    found = 0;
    for (i = 0; i < TH_IPC_RECORDS; i++) if (ThIpcRead(h, i, &r) && r.NotifId == 118) found = 1;
    Check("ring: the overwritten record 118 is gone", !found);
    Check("ring: the header's NextSeq is 33", TI_LOAD32(&h->NextSeq) == 33);

    /* the one agent-written record field (ADR-toasts 11): the shown mark the glue stores when it maps a banner for
     * a record that reads window, read back by the bridge after a failed dom0 action turned the record window. The
     * mark is the record's SEQUENCE: the glue's store (find the slot, then store) races the bridge's republish of
     * that slot, and a late store must not read as the next toast's banner shown (review 2026-10-07). */
    {
        LONG seq = 0, s4;
        int slot;
        ThIpcInit(h);
        s4 = ThIpcPublish(h, 300, 0, 0x9, &id1, TH_VERDICT_FORWARDED, 7000);
        Check("agent mark: a fresh record reads none", ThIpcRead(h, 0, &r) && r.AgentShownSeq == 0 && ThIpcAgentState(h, s4) == TH_AGENT_NONE);
        Check("agent mark: the bridge's unconditional window turn hands back the record's sequence", ThIpcSetVerdictSeq(h, 300, TH_VERDICT_WINDOW, FALSE, &seq) && seq == s4);
        ThIpcAgentMarkShown(h, s4);
        Check("agent mark: stored as the sequence, read back for that sequence", ThIpcAgentState(h, s4) == TH_AGENT_SHOWN && ThIpcRead(h, 0, &r) && r.AgentShownSeq == s4);
        ThIpcAgentMarkShown(h, s4 + 5);
        Check("agent mark: a sequence not in the ring is not marked and reads none", ThIpcAgentState(h, s4 + 5) == TH_AGENT_NONE);
        slot = ThIpcFindSeq(h, s4);
        for (i = 0; i < TH_IPC_RECORDS; i++) s3 = ThIpcPublish(h, 400 + (UINT32)i, 0, 0, &id2, TH_VERDICT_PENDING, 8000);
        Check("agent mark: the ring turned - the slot holds the new toast, the old record is gone", slot == 0 && ThIpcFindSeq(h, s4) < 0 && ThIpcFindSeq(h, s3) == slot);
        ThIpcAgentStoreShown(h, slot, s4);   /* the RACE: the old toast's late store lands after the republish */
        Check("agent mark: RACE - a late store of the old toast's mark does not read as the new toast's banner shown", ThIpcAgentState(h, s3) == TH_AGENT_NONE);
        ThIpcAgentMarkShown(h, s3);
        Check("agent mark: the new toast's own mark reads back", ThIpcAgentState(h, s3) == TH_AGENT_SHOWN);
    }
}

/* ---- the state machine ------------------------------------------------------------------------ */
static TH_INPUT In(int identKnown, UINT64 ident, int bridgeUp, int match, LONG seq, LONG verdict, int preempt, ULONGLONG preemptDue)
{
    TH_INPUT in;
    memset(&in, 0, sizeof(in));
    in.IdentKnown = identKnown; in.Ident = ident; in.BridgeUp = bridgeUp;
    in.MatchFound = match; in.MatchSeq = seq; in.MatchNotifId = (UINT32)(100 + seq); in.MatchVerdict = verdict;
    in.PreemptWanted = preempt; in.PreemptDue = preemptDue;
    return in;
}

static void TestMachine(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    const UINT64 IA = 0xA1A1A1A1ULL, IB = 0xB2B2B2B2ULL;
    const ULONGLONG t0 = 10000;

    /* 1. pending -> bridge: never mapped */
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: a new banner with no identity yet is HELD at once (hold starts before any map)", out.Decision == ThDecHold && out.Event == ThEvHoldStart && out.Reason == ThReasonNoIdentity);
    Check("machine: ...with a recheck deadline inside the bound", out.Due == t0 + TH_HOLD_RECHECK_MS);
    in = In(1, IA, 1, 1, 7, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0 + 120, &out);
    Check("machine: identity known, record pending -> still HELD, deadline = the bound", out.Decision == ThDecHold && out.Reason == ThReasonVerdictPending && out.Due == t0 + TH_HOLD_BOUND_MS && out.Event == ThEvNone);
    in = In(1, IA, 1, 1, 7, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0 + 900, &out);
    Check("machine: verdict bridge -> SUPPRESS (never mapped), event reported once with held_ms", out.Decision == ThDecSuppress && out.Event == ThEvSuppress && out.HeldMs == 900 && c.NotifId == 107);
    ThCoreDecide(&c, &in, t0 + 2000, &out);
    Check("machine: the standing SUPPRESS is quiet (no repeated event); the forward bound is armed while dom0's ack is awaited",
        out.Decision == ThDecSuppress && out.Event == ThEvNone && out.Due == t0 + 900 + TH_FORWARD_BOUND_MS);
    in = In(0, 0, 1, 0, 0, 0, 0, 0);   /* a re-read in flight after a LOCATIONCHANGE */
    ThCoreDecide(&c, &in, t0 + 2100, &out);
    Check("machine: a re-read in flight keeps the standing decision", out.Decision == ThDecSuppress && out.Event == ThEvNone);

    /* 2. pending -> window: mapped at the verdict */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 8, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: first examination with identity -> HELD (HoldStart)", out.Decision == ThDecHold && out.Event == ThEvHoldStart);
    in = In(1, IA, 1, 1, 8, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0 + 400, &out);
    Check("machine: verdict window -> SHOW at once", out.Decision == ThDecShow && out.Event == ThEvShowVerdict && out.HeldMs == 400);
    ThCoreDecide(&c, &in, t0 + 500, &out);
    Check("machine: the standing SHOW is quiet", out.Decision == ThDecShow && out.Event == ThEvNone && out.Due == 0);

    /* 3. pending -> timeout: FAIL OPEN, reported */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 9, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    ThCoreDecide(&c, &in, t0 + TH_HOLD_BOUND_MS - 1, &out);
    Check("machine: one ms before the bound still HELD", out.Decision == ThDecHold);
    ThCoreDecide(&c, &in, t0 + TH_HOLD_BOUND_MS, &out);
    Check("machine: at the bound with the verdict still pending -> SHOW by FAIL-OPEN (loud)", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonVerdictPending && c.FailedOpen);
    in = In(1, IA, 1, 1, 9, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0 + TH_HOLD_BOUND_MS + 500, &out);
    Check("machine: a verdict landing after the fail-open does not flip a shown banner", out.Decision == ThDecShow && out.Event == ThEvNone);

    /* 4. no record at all: recheck ticks, then fail open at the bound */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: identity but no record -> HELD with a recheck tick", out.Decision == ThDecHold && out.Reason == ThReasonNoRecord && out.Due == t0 + TH_HOLD_RECHECK_MS);
    ThCoreDecide(&c, &in, t0 + 2800, &out);
    Check("machine: the recheck deadline never passes the bound", out.Due == t0 + TH_HOLD_BOUND_MS);
    ThCoreDecide(&c, &in, t0 + TH_HOLD_BOUND_MS + 1, &out);
    Check("machine: no record within the bound -> FAIL-OPEN no-record", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonNoRecord);

    /* 5. no identity ever: fail open at the bound */
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    ThCoreDecide(&c, &in, t0 + TH_HOLD_BOUND_MS, &out);
    Check("machine: no identity within the bound -> FAIL-OPEN no-identity (the Win10-structure guard)", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonNoIdentity);

    /* 6. bridge down when the banner appears: shown at once, no 3 s wait */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 0, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: bridge down + no record -> SHOW immediately (window path by design)", out.Decision == ThDecShow && out.Event == ThEvShowNoBridge && out.Reason == ThReasonBridgeDown && !c.FailedOpen);

    /* 7. bridge dies while a pending record is held: shown at once */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 10, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, IA, 0, 1, 10, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0 + 700, &out);
    Check("machine: bridge death while held -> SHOW at once, FAIL-OPEN bridge-exited", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonBridgeExited && out.HeldMs == 700);

    /* 8. content changes in place under a decided banner: a fresh hold for the new content */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 11, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: (setup) suppressed", out.Decision == ThDecSuppress);
    in = In(1, IB, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0 + 6000, &out);
    Check("machine: new content in the same window -> ContentChanged, HELD again", out.Decision == ThDecHold && out.Event == ThEvContentChanged && c.HoldSince == t0 + 6000 && c.RecordSeq == 0);
    in = In(1, IB, 1, 1, 12, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0 + 6300, &out);
    Check("machine: ...and the new content's own verdict decides it", out.Decision == ThDecShow && out.Event == ThEvShowVerdict && out.HeldMs == 300);

    /* 9. a reserved pending record is released when the held content turns out different */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 13, TH_VERDICT_PENDING, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, IB, 1, 0, 0, 0, 0, 0);   /* the re-read says: different content, nothing matches it */
    ThCoreDecide(&c, &in, t0 + 200, &out);
    Check("machine: a pending reservation is released when the content turns out different", out.ReleaseSeq == 13 && c.RecordSeq == 0 && out.Decision == ThDecHold);

    /* 10. pre-emption: a mapped window-path banner is unmapped for a queued bridge-bound toast */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 14, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: (setup) shown", out.Decision == ThDecShow);
    in = In(1, IA, 1, 1, 14, TH_VERDICT_WINDOW, 1, t0 + 5000);
    ThCoreDecide(&c, &in, t0 + 1000, &out);
    Check("machine: a queued bridge-bound toast PRE-EMPTS the mapped banner (unmapped before the swap)", out.Decision == ThDecHold && out.Event == ThEvPreemptStart && out.Reason == ThReasonPreempt && out.Due == t0 + 5000);
    ThCoreDecide(&c, &in, t0 + 1500, &out);
    Check("machine: pre-emption is quiet while it lasts", out.Decision == ThDecHold && out.Event == ThEvNone);
    in = In(1, IA, 1, 1, 14, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0 + 2000, &out);
    Check("machine: the queued toast resolved to window -> re-mapped (PreemptEnd)", out.Decision == ThDecShow && out.Event == ThEvPreemptEnd && out.HeldMs == 1000);

    /* 11. a suppressed banner is never pre-empted into anything else */
    memset(&c, 0, sizeof(c));
    in = In(1, IA, 1, 1, 15, TH_VERDICT_BRIDGE, 1, t0 + 5000);
    ThCoreDecide(&c, &in, t0, &out);
    Check("machine: pre-emption does not touch a suppressed banner", out.Decision == ThDecSuppress && out.Event == ThEvSuppress);
}

static void TestPreemptPredicate(void)
{
    TH_IPC_RECORD r;
    ULONGLONG due = 0;
    const ULONGLONG now = 50000;
    memset(&r, 0, sizeof(r));

    r.Seq = 5; r.Verdict = TH_VERDICT_PENDING; r.ArrivalTick = now - 1000;
    Check("preempt: a pending record newer than the banner's pre-empts, due = arrival + bound", ThCorePreemptBy(&r, 4, now, &due) && due == now - 1000 + TH_HOLD_BOUND_MS);
    Check("preempt: ...but not one OLDER than the banner's own record (bannerless by FIFO)", !ThCorePreemptBy(&r, 5, now, &due) && !ThCorePreemptBy(&r, 9, now, &due));
    r.ArrivalTick = now - TH_HOLD_BOUND_MS;
    Check("preempt: a pending record past its bound no longer pre-empts", !ThCorePreemptBy(&r, 0, now, &due));
    r.Verdict = TH_VERDICT_BRIDGE; r.ArrivalTick = now - 4000;
    Check("preempt: a bridge record pre-empts within its window", ThCorePreemptBy(&r, 0, now, &due) && due == now - 4000 + TH_PREEMPT_WINDOW_MS);
    r.ArrivalTick = now - TH_PREEMPT_WINDOW_MS;
    Check("preempt: a bridge record past its window no longer pre-empts (bannerless toast)", !ThCorePreemptBy(&r, 0, now, &due));
    r.Verdict = TH_VERDICT_WINDOW; r.ArrivalTick = now - 100;
    Check("preempt: a window-verdict record never pre-empts", !ThCorePreemptBy(&r, 0, now, &due));
    r.Verdict = TH_VERDICT_FORWARDED; r.ArrivalTick = now - 100;
    Check("preempt: a forwarded record pre-empts like a bridge one", ThCorePreemptBy(&r, 0, now, &due));
    r.Verdict = TH_VERDICT_BRIDGE; r.ArrivalTick = now + 99999;
    Check("preempt: an arrival tick from the future reads as now", ThCorePreemptBy(&r, 0, now, &due) && due == now + TH_PREEMPT_WINDOW_MS);
    r.Seq = 0;
    Check("preempt: an empty slot never pre-empts", !ThCorePreemptBy(&r, 0, now, &due));
}

/* ---- the review's probe (2026-10-06), as a checked scenario -------------------------------------
 * Banner A (window) is mapped; toast B (bridge) is listed behind it; the in-place swap's LOCATIONCHANGE
 * queues a read BEFORE the tracking pass runs, so that pass sees the identity "in flight". Before the fix the
 * pass ended the pre-emption and MAPPED the window while B painted (blocker #1); and once B was suppressed,
 * the bridge's later `window` (forward failed for good) was ignored - B shown nowhere (blocker #2). The glue
 * now keeps the last reading (IdentKnown stays 1 with a read in flight) and the core keeps a pre-emption in
 * force on an identity-less pass; the suppressed banner re-reads its record every pass. */
static void TestReviewProbe(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    ULONGLONG t = 1000;
    memset(&c, 0, sizeof(c));

    in = In(1, 0xA, 1, 1, 1, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe 1: banner A, record window -> SHOW", out.Decision == ThDecShow && out.Event == ThEvShowVerdict);

    t += 1000;
    in = In(1, 0xA, 1, 1, 1, TH_VERDICT_WINDOW, 1, t + 14000);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe 2: record B (bridge) queued behind -> pre-empted (HOLD, unmapped)", out.Decision == ThDecHold && out.Event == ThEvPreemptStart);

    /* 3a. the glue's new contract: a read in flight does NOT make the identity unknown - and it computes
     *     PreemptWanted on every pass. */
    t += 4000;
    in = In(1, 0xA, 1, 1, 1, TH_VERDICT_WINDOW, 1, t + 10000);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe 3a: the swap pass with the last reading standing and pre-emption still wanted -> HOLD", out.Decision == ThDecHold && out.Event == ThEvNone);
    /* 3b. an identity-less pass (a banner whose card never read, shown by fail-open) with pre-emption WANTED
     *     holds (the PREEMPT_IDENTGATE defect ignores PreemptWanted without an identity and maps it)... */
    in = In(0, 0, 1, 0, 0, 0, 1, t + 10000);
    ThCoreDecide(&c, &in, t + 1, &out);
    Check("probe 3b: an identity-less pass with pre-emption wanted -> HOLD", out.Decision == ThDecHold && out.Event == ThEvNone && c.Preempting);
    /* 3c. ...and an identity-less pass with pre-emption NO LONGER wanted ENDS it - the glue computes
     *     PreemptWanted on every pass, so nothing else may keep a pre-emption alive (review N1: a term that
     *     did made it permanent for a banner whose card never read). */
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t + 2, &out);
    Check("probe 3c: an identity-less pass with pre-emption no longer wanted ENDS it -> SHOW, PreemptEnd, no deadline", out.Decision == ThDecShow && out.Event == ThEvPreemptEnd && !c.Preempting && out.Due == 0);
    /* back to the pre-empted state for the rest of the probe */
    in = In(1, 0xA, 1, 1, 1, TH_VERDICT_WINDOW, 1, t + 10000);
    ThCoreDecide(&c, &in, t + 3, &out);
    Check("probe 3d: pre-empted again (setup)", out.Decision == ThDecHold && out.Event == ThEvPreemptStart);

    t += 150;
    in = In(1, 0xB, 1, 1, 2, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe 4: the read lands with B (bridge) -> SUPPRESS", out.Decision == ThDecSuppress && out.Event == ThEvSuppress);

    t += 6000;
    in = In(1, 0xB, 1, 1, 2, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe 5: B's record turns window (forward failed for good) -> SHOWN after all (corrected)", out.Decision == ThDecShow && out.Event == ThEvShowCorrected && out.Reason == ThReasonCorrected);
    ThCoreDecide(&c, &in, t + 10, &out);
    Check("probe 5b: ...and the correction is quiet afterwards", out.Decision == ThDecShow && out.Event == ThEvNone);
}

/* ---- blocker #2, the other half: bridge death reopens a suppression dom0 never acknowledged ------ */
static void TestSuppressReopen(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    const ULONGLONG t0 = 5000;

    /* record still `bridge` (forward not acknowledged) when the bridge dies -> shown, loud */
    memset(&c, 0, sizeof(c));
    in = In(1, 0xC, 1, 1, 3, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("reopen: (setup) suppressed on bridge", out.Decision == ThDecSuppress);
    in = In(1, 0xC, 0, 1, 3, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0 + 800, &out);
    Check("reopen: bridge dies before dom0's ack -> SHOW, FAIL-OPEN bridge-exited", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonBridgeExited);

    /* record `forwarded` (dom0 acknowledged) -> the suppression stands through the bridge's death */
    memset(&c, 0, sizeof(c));
    in = In(1, 0xD, 1, 1, 4, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0xD, 1, 1, 4, TH_VERDICT_FORWARDED, 0, 0);
    ThCoreDecide(&c, &in, t0 + 300, &out);
    Check("reopen: bridge -> forwarded keeps the suppression, quietly, and disarms the forward bound", out.Decision == ThDecSuppress && out.Event == ThEvNone && out.Due == 0);
    in = In(1, 0xD, 0, 1, 4, TH_VERDICT_FORWARDED, 0, 0);
    ThCoreDecide(&c, &in, t0 + 900, &out);
    Check("reopen: bridge dies AFTER dom0's ack -> stays suppressed (dom0 has it)", out.Decision == ThDecSuppress && out.Event == ThEvNone);

    /* record gone from the ring (wrapped) when the bridge dies -> nobody can vouch for the forward -> shown */
    memset(&c, 0, sizeof(c));
    in = In(1, 0xE, 1, 1, 5, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0xE, 0, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0 + 500, &out);
    Check("reopen: record gone + bridge down -> SHOW (unconfirmed)", out.Decision == ThDecShow && out.Event == ThEvFailOpen);

    /* a suppressed banner's record turning window with the bridge UP is the correction, not a fail-open */
    memset(&c, 0, sizeof(c));
    in = In(1, 0xF, 1, 1, 6, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0xF, 1, 1, 6, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0 + 2000, &out);
    Check("reopen: record -> window with the bridge up -> SHOW corrected, FailedOpen stays 0", out.Decision == ThDecShow && out.Event == ThEvShowCorrected && !c.FailedOpen);

    /* a record that is somebody else's (different seq) turning window is NOT our correction */
    memset(&c, 0, sizeof(c));
    in = In(1, 0x10, 1, 1, 7, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0x10, 1, 1, 8, TH_VERDICT_WINDOW, 0, 0);
    ThCoreDecide(&c, &in, t0 + 100, &out);
    Check("reopen: another record's window verdict does not reopen this suppression", out.Decision == ThDecSuppress && out.Event == ThEvNone);
}

/* ---- #5: a completed identity re-claims the record it consumed on the first (partial) read -------- */
static void TestReclaim(void)
{
    unsigned char block[TH_IPC_BYTES];
    TH_IPC_HEADER* h = (TH_IPC_HEADER*)block;
    TH_IPC_RECORD recs[TH_IPC_RECORDS];
    TI_CANDIDATE cand[TH_IPC_RECORDS];
    TH_CONSUMED cons;
    TOAST_IDENT rec, firstRead, fullRead, other;
    TI_MATCH q;
    int n, i, pick;
    const ULONGLONG now = 20000;

    memset(&cons, 0, sizeof(cons));
    ThIpcInit(h);
    TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice about the agenda"), &rec);
    ThIpcPublish(h, 300, 0, 0, &rec, TH_VERDICT_BRIDGE, now - 900);
    n = 0;
    for (i = 0; i < TH_IPC_RECORDS; i++) if (ThIpcRead(h, i, &recs[n])) n++;

    /* first read: the MessageText block is not built yet -> message empty -> the unique PARTIAL is taken and consumed */
    TiIdentFromTexts(W("Mail"), W("New message"), NULL, &firstRead);
    ThCoreCandidates(recs, n, &cons, 0, now, cand);
    pick = TiSelect(cand, n, &firstRead, &q);
    Check("reclaim: half-built card -> unique PARTIAL taken", pick == 0 && q == TiMatchPartial);
    ThConsumedAdd(&cons, recs[pick].Seq);
    Check("reclaim: ...and consumed", ThConsumedHas(&cons, recs[0].Seq));

    /* the full read: the identity COMPLETES (message now present) - the own record must still be a candidate */
    TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice about the agenda"), &fullRead);
    ThCoreCandidates(recs, n, &cons, recs[0].Seq /* own */, now + 200, cand);
    pick = TiSelect(cand, n, &fullRead, &q);
    Check("reclaim: the completed identity re-claims its own consumed record (FULL)", pick == 0 && q == TiMatchFull);

    /* without the own seq (another banner) the consumed record is not a candidate */
    ThCoreCandidates(recs, n, &cons, 0, now + 200, cand);
    pick = TiSelect(cand, n, &fullRead, &q);
    Check("reclaim: another banner cannot take a record this one consumed", pick == -1);

    /* N5: the re-claim is offered ONLY to a reading that COMPLETES the claiming one (same sender+title, old
     * message empty or a prefix of the new one). Different content swapped into the window is a new toast. */
    {
        WCHAR oldMsg[TI_NORM_MAX], newMsg[TI_NORM_MAX];
        size_t oldLen, newLen;
        TOAST_IDENT alice, aliceLong, bob;
        TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice"), &alice);
        oldLen = TiNormalize(W("From Alice"), oldMsg, TI_NORM_MAX);
        TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice about the agenda"), &aliceLong);
        newLen = TiNormalize(W("From Alice about the agenda"), newMsg, TI_NORM_MAX);
        Check("extends: empty old message -> completes", ThCoreReadingExtends(&firstRead, NULL, 0, &fullRead, newMsg, newLen));
        Check("extends: old message a prefix of the new one -> completes", ThCoreReadingExtends(&alice, oldMsg, oldLen, &aliceLong, newMsg, newLen));
        TiIdentFromTexts(W("Mail"), W("New message"), W("From Bob about lunch"), &bob);
        newLen = TiNormalize(W("From Bob about lunch"), newMsg, TI_NORM_MAX);
        Check("extends: same sender+title, a DIFFERENT non-extending message -> new content, no re-claim", !ThCoreReadingExtends(&alice, oldMsg, oldLen, &bob, newMsg, newLen));
        Check("extends: a shorter message is not an extension", !ThCoreReadingExtends(&aliceLong, newMsg, newLen, &alice, oldMsg, oldLen) || newLen <= oldLen);
        TiIdentFromTexts(W("Chat"), W("New message"), W("From Alice about the agenda"), &bob);
        newLen = TiNormalize(W("From Alice about the agenda"), newMsg, TI_NORM_MAX);
        Check("extends: another sender never completes", !ThCoreReadingExtends(&alice, oldMsg, oldLen, &bob, newMsg, newLen));
        TiIdentFromTexts(W("Mail"), W("Reminder"), W("From Alice about the agenda"), &bob);
        Check("extends: another title never completes", !ThCoreReadingExtends(&alice, oldMsg, oldLen, &bob, newMsg, newLen));

        /* THE DEFECT SCENARIO: reading A claimed R (bridge); new in-place content B with the same sender+title
         * but a different message, B's own record not yet published. The glue passes ownSeq=0 (not an
         * extension), so R is not a candidate: no match -> held, fail-open at the bound - never R's verdict. */
        memset(&cons, 0, sizeof(cons));
        ThIpcInit(h);
        TiIdentFromTexts(W("Mail"), W("New message"), W("From Alice about the agenda"), &rec);
        ThIpcPublish(h, 310, 0, 0, &rec, TH_VERDICT_BRIDGE, now - 900);
        n = 0;
        for (i = 0; i < TH_IPC_RECORDS; i++) if (ThIpcRead(h, i, &recs[n])) n++;
        oldLen = TiNormalize(W("From Alice about the agenda"), oldMsg, TI_NORM_MAX);
        ThCoreCandidates(recs, n, &cons, 0, now, cand);
        pick = TiSelect(cand, n, &rec, &q);
        Check("N5: (setup) reading A claims its record", pick == 0);
        ThConsumedAdd(&cons, recs[0].Seq);
        TiIdentFromTexts(W("Mail"), W("New message"), W("From Bob about lunch"), &bob);
        newLen = TiNormalize(W("From Bob about lunch"), newMsg, TI_NORM_MAX);
        {
            const LONG ownSeq = ThCoreReadingExtends(&rec, oldMsg, oldLen, &bob, newMsg, newLen) ? recs[0].Seq : 0;
            ThCoreCandidates(recs, n, &cons, ownSeq, now + 500, cand);
            pick = TiSelect(cand, n, &bob, &q);
            Check("N5: new same-title content without its own record does NOT inherit the old record's verdict (no match)", pick == -1);
        }
        /* and once B's own record is published it is taken, FULL */
        ThIpcPublish(h, 311, 0, 0, &bob, TH_VERDICT_WINDOW, now - 100);
        n = 0;
        for (i = 0; i < TH_IPC_RECORDS; i++) if (ThIpcRead(h, i, &recs[n])) n++;
        ThCoreCandidates(recs, n, &cons, 0, now + 600, cand);
        pick = TiSelect(cand, n, &bob, &q);
        Check("N5: B's own record, once published, is taken FULL", pick == 1 && q == TiMatchFull && recs[1].NotifId == 311);
        (void)other;
    }

    /* the core: a hold reserved on seq 1 whose reading changes to the same record does NOT release it */
    {
        TH_CORE c; TH_INPUT in; TH_OUTPUT out;
        memset(&c, 0, sizeof(c));
        in = In(1, firstRead.Combined, 1, 1, 1, TH_VERDICT_PENDING, 0, 0);
        ThCoreDecide(&c, &in, now, &out);
        in = In(1, fullRead.Combined, 1, 1, 1, TH_VERDICT_PENDING, 0, 0);   /* the glue re-claimed seq 1 */
        ThCoreDecide(&c, &in, now + 200, &out);
        Check("reclaim: core keeps the reservation when the completed reading re-claims the same record", out.ReleaseSeq == 0 && c.RecordSeq == 1 && out.Decision == ThDecHold);
        /* and a decided banner whose reading completes onto the same record is decided again at once, same way */
        in = In(1, fullRead.Combined, 1, 1, 1, TH_VERDICT_BRIDGE, 0, 0);
        ThCoreDecide(&c, &in, now + 300, &out);
        Check("reclaim: (setup) suppressed", out.Decision == ThDecSuppress);
        in = In(1, 0x77, 1, 1, 1, TH_VERDICT_BRIDGE, 0, 0);
        ThCoreDecide(&c, &in, now + 400, &out);
        Check("reclaim: a decided banner re-claiming its record on a changed reading is suppressed again in the same pass", out.Decision == ThDecSuppress && out.Event == ThEvSuppress && c.RecordSeq == 1);
    }
}

/* ---- the review's probe 3 (2026-10-06, N1): a banner whose card never reads must not pre-empt for ever -- */
static void TestReviewProbe3(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    ULONGLONG t = 1000;
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe3 1: first sight, no identity -> HOLD", out.Decision == ThDecHold);
    t += 3000;
    ThCoreDecide(&c, &in, t, &out);
    Check("probe3 2: bound passed -> SHOW (fail-open, mapped)", out.Decision == ThDecShow && out.Event == ThEvFailOpen);
    t += 1000;
    in = In(0, 0, 1, 0, 0, 0, 1, t + 15000);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe3 3: a newer bridge record -> pre-empted with the record's deadline", out.Decision == ThDecHold && out.Due == t + 15000);
    t += 15001;
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t, &out);
    Check("probe3 4: the record's window passed -> SHOW again (pre-emption ENDS; no deadline left)", out.Decision == ThDecShow && out.Event == ThEvPreemptEnd && out.Due == 0);
    t += 600000;
    ThCoreDecide(&c, &in, t, &out);
    Check("probe3 5: ten minutes later still SHOW, nothing armed", out.Decision == ThDecShow && out.Event == ThEvNone && out.Due == 0);
}

/* ---- the forward bound: a suppression nobody acknowledges reopens (second review #4) -------------- */
static void TestForwardBound(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    const ULONGLONG t0 = 30000;

    memset(&c, 0, sizeof(c));
    in = In(1, 0x21, 1, 1, 9, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    Check("fwdbound: suppressed on bridge arms the forward bound on the deciding pass itself", out.Decision == ThDecSuppress && out.Event == ThEvSuppress && out.Due == t0 + TH_FORWARD_BOUND_MS);
    ThCoreDecide(&c, &in, t0 + 100, &out);
    Check("fwdbound: awaiting dom0's ack -> deadline = suppress + bound", out.Decision == ThDecSuppress && out.Due == t0 + TH_FORWARD_BOUND_MS);
    ThCoreDecide(&c, &in, t0 + TH_FORWARD_BOUND_MS - 1, &out);
    Check("fwdbound: one ms before the bound still suppressed", out.Decision == ThDecSuppress && out.Event == ThEvNone);
    ThCoreDecide(&c, &in, t0 + TH_FORWARD_BOUND_MS, &out);
    Check("fwdbound: no ack within the bound -> SHOW, FAIL-OPEN forward-unconfirmed (loud)", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonForwardUnconfirmed && c.FailedOpen);
    in = In(1, 0x21, 1, 1, 9, TH_VERDICT_FORWARDED, 0, 0);
    ThCoreDecide(&c, &in, t0 + TH_FORWARD_BOUND_MS + 500, &out);
    Check("fwdbound: a late ack does not flip the shown banner back", out.Decision == ThDecShow && out.Event == ThEvNone);

    /* the ack arrives in time: nothing reopens, nothing stays armed */
    memset(&c, 0, sizeof(c));
    in = In(1, 0x22, 1, 1, 10, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0x22, 1, 1, 10, TH_VERDICT_FORWARDED, 0, 0);
    ThCoreDecide(&c, &in, t0 + 120, &out);
    Check("fwdbound: ack in time -> suppressed, no deadline", out.Decision == ThDecSuppress && out.Due == 0);
    ThCoreDecide(&c, &in, t0 + 60000, &out);
    Check("fwdbound: ...and stays suppressed a minute later, quietly", out.Decision == ThDecSuppress && out.Event == ThEvNone && out.Due == 0);

    /* suppressed straight onto an already-forwarded record (allowlisted toast acked before its banner): no bound */
    memset(&c, 0, sizeof(c));
    in = In(1, 0x23, 1, 1, 11, TH_VERDICT_FORWARDED, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    ThCoreDecide(&c, &in, t0 + 10000, &out);
    Check("fwdbound: suppressed on an already-forwarded record never reopens", out.Decision == ThDecSuppress && out.Event == ThEvNone && out.Due == 0);

    /* the record vanished from the ring while awaited: counts as unconfirmed at the bound */
    memset(&c, 0, sizeof(c));
    in = In(1, 0x24, 1, 1, 12, TH_VERDICT_BRIDGE, 0, 0);
    ThCoreDecide(&c, &in, t0, &out);
    in = In(1, 0x24, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, t0 + TH_FORWARD_BOUND_MS, &out);
    Check("fwdbound: record gone + no ack within the bound -> SHOW", out.Decision == ThDecShow && out.Event == ThEvFailOpen && out.Reason == ThReasonForwardUnconfirmed);
}

/* ---- the small pure rules: paced second look, size rule, dead-bridge records, retry back-off --------- */
static void TestPureRules(void)
{
    /* N2: the second card-less read counts only when requested >= 250 ms after the first's request */
    Check("nocard: a second look requested 5 ms after the first does NOT count (banner mid-grow)", !ThCoreNoCardCounts(1000, 1005));
    Check("nocard: ...249 ms does not", !ThCoreNoCardCounts(1000, 1249));
    Check("nocard: ...250 ms counts", ThCoreNoCardCounts(1000, 1250));

    /* N3: relative size rule - banners pass at every resolution/DPI, full-height/width surfaces do not */
    Check("size: 396x573 banner at 1366x768 is held (75 %)", !ThCoreSizeExcludes(396, 573, 1366, 768));
    Check("size: 396x573 banner at 1600x900 is held", !ThCoreSizeExcludes(396, 573, 1600, 900));
    Check("size: 495x716 (573 px banner at 125 % DPI) at 1920x1080 is held", !ThCoreSizeExcludes(495, 716, 1920, 1080));
    Check("size: 396x152 banner at 1920x1080 is held", !ThCoreSizeExcludes(396, 152, 1920, 1080));
    Check("size: 400x1032 Notification Center at 1080p (95 % tall) is excluded", ThCoreSizeExcludes(400, 1032, 1920, 1080));
    Check("size: 400x730 clock flyout at 768 (95 %) is excluded", ThCoreSizeExcludes(400, 730, 1366, 768));
    Check("size: a full-width bar is excluded", ThCoreSizeExcludes(1920, 300, 1920, 1080));
    Check("size: unknown screen (0) excludes nothing", !ThCoreSizeExcludes(5000, 5000, 0, 0));

    /* N6: a dead bridge's unconfirmed records read as window; forwarded/window stand; newer records untouched */
    Check("dead: bridge at/below the dead ceiling -> window", ThCoreDeadVerdict(5, TH_VERDICT_BRIDGE, 7) == TH_VERDICT_WINDOW);
    Check("dead: pending at/below the ceiling -> window", ThCoreDeadVerdict(7, TH_VERDICT_PENDING, 7) == TH_VERDICT_WINDOW);
    Check("dead: forwarded stands (dom0 has it)", ThCoreDeadVerdict(5, TH_VERDICT_FORWARDED, 7) == TH_VERDICT_FORWARDED);
    Check("dead: window stands", ThCoreDeadVerdict(5, TH_VERDICT_WINDOW, 7) == TH_VERDICT_WINDOW);
    Check("dead: a NEWER record (the next instance's) is untouched", ThCoreDeadVerdict(8, TH_VERDICT_BRIDGE, 7) == TH_VERDICT_BRIDGE);
    Check("dead: no dead instance (ceiling 0) changes nothing", ThCoreDeadVerdict(5, TH_VERDICT_BRIDGE, 0) == TH_VERDICT_BRIDGE);
    {
        TH_CORE c; TH_INPUT in; TH_OUTPUT out;
        memset(&c, 0, sizeof(c));
        in = In(1, 0x31, 1, 1, 20, TH_VERDICT_WINDOW, 0, 0);
        ThCoreDecide(&c, &in, 1000, &out);
        in = In(1, 0x31, 0, 1, 20, TH_VERDICT_WINDOW, 1, 20000);
        ThCoreDecide(&c, &in, 1100, &out);
        Check("dead: no pre-emption while the bridge is down (its records are nobody's queue)", out.Decision == ThDecShow && out.Event == ThEvNone);
    }

    /* N7: refused identity requests back off: 250, 500, 1000, 2000, 2000... */
    Check("backoff: 0 refusals -> 250 ms", ThCoreRetryDelayMs(0) == 250);
    Check("backoff: 1 -> 500", ThCoreRetryDelayMs(1) == 500);
    Check("backoff: 2 -> 1000", ThCoreRetryDelayMs(2) == 1000);
    Check("backoff: 3 -> 2000", ThCoreRetryDelayMs(3) == 2000);
    Check("backoff: 9 -> capped at 2000", ThCoreRetryDelayMs(9) == 2000);
}

/* ---- #13: no identity yet, bridge down -> no 3 s wait ------------------------------------------------ */
static void TestNoIdentityBridgeDown(void)
{
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 0, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, 7000, &out);
    Check("no-identity + bridge down -> SHOW at once (no record can ever name it)", out.Decision == ThDecShow && out.Event == ThEvShowNoBridge && out.Reason == ThReasonBridgeDown && !c.FailedOpen);
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 1, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, 7000, &out);
    in = In(0, 0, 0, 0, 0, 0, 0, 0);
    ThCoreDecide(&c, &in, 7400, &out);
    Check("no-identity hold + the bridge dies -> SHOW at once", out.Decision == ThDecShow && out.Event == ThEvShowNoBridge);
}

/* ---- the consumed set + not-a-banner --------------------------------------------------------------- */
static void TestConsumedAndNotBanner(void)
{
    TH_CONSUMED cons;
    TH_CORE c;
    TH_INPUT in;
    TH_OUTPUT out;
    int i;
    memset(&cons, 0, sizeof(cons));
    Check("consumed: empty set has nothing; seq 0 is never 'in'", !ThConsumedHas(&cons, 5) && !ThConsumedHas(&cons, 0));
    ThConsumedAdd(&cons, 5); ThConsumedAdd(&cons, 5); ThConsumedAdd(&cons, 0);
    Check("consumed: add is idempotent and ignores 0", ThConsumedHas(&cons, 5) && cons.Next == 1);
    ThConsumedRemove(&cons, 5);
    Check("consumed: remove", !ThConsumedHas(&cons, 5));
    for (i = 1; i <= TH_CONSUMED_SLOTS + 3; i++) ThConsumedAdd(&cons, i);
    Check("consumed: the ring wraps (oldest forgotten, newest kept)", !ThConsumedHas(&cons, 1) && ThConsumedHas(&cons, TH_CONSUMED_SLOTS + 3));

    /* not a banner: shown at once, once-logged, never held, pre-emption irrelevant */
    memset(&c, 0, sizeof(c));
    in = In(0, 0, 1, 0, 0, 0, 1, 99999);
    ThCoreDecide(&c, &in, 1000, &out);
    Check("not-banner: (setup) first sight is held", out.Decision == ThDecHold);
    in.NotBanner = 1;
    ThCoreDecide(&c, &in, 1300, &out);
    Check("not-banner: two card-less reads -> SHOW with the NotBanner event", out.Decision == ThDecShow && out.Event == ThEvNotBanner && c.NotBanner);
    ThCoreDecide(&c, &in, 1400, &out);
    Check("not-banner: ...quiet afterwards, pre-emption ignored", out.Decision == ThDecShow && out.Event == ThEvNone && out.Due == 0);
}

int main(void)
{
#if defined(TOASTIDENT_DEFECT_NOFOLD) || defined(TOASTIDENT_DEFECT_ORDERONLY) || defined(TOASTIDENT_DEFECT_VERDICTOVERRIDE) || \
    defined(TOASTHOLD_DEFECT_NOBOUND) || defined(TOASTHOLD_DEFECT_FAILCLOSED) || defined(TOASTHOLD_DEFECT_NOPREEMPT) || \
    defined(TOASTHOLD_DEFECT_PREEMPT_IDENTGATE) || defined(TOASTHOLD_DEFECT_SUPPRESS_FINAL) || \
    defined(TOASTHOLD_DEFECT_NORECLAIM) || defined(TOASTHOLD_DEFECT_NOIDENT_IGNORES_BRIDGE) || \
    defined(TOASTHOLD_DEFECT_NOFORWARDBOUND) || defined(TOASTHOLD_DEFECT_NOCARD_UNPACED) || \
    defined(TOASTHOLD_DEFECT_SIZE60) || defined(TOASTHOLD_DEFECT_DEADRECORDS) || defined(TOASTHOLD_DEFECT_NOBACKOFF) || \
    defined(TOASTIDENT_DEFECT_TIE_SLOTORDER) || defined(TOASTHOLD_DEFECT_RECLAIM_ANY) || \
    defined(TOASTIDENT_DEFECT_MARK_BY_SLOT)
    printf("DEFECT BUILD: a TOASTIDENT_/TOASTHOLD_DEFECT_* switch is compiled in - this run MUST fail\n");
#endif
    TestNormalization();
    TestIdentityAgreement();
    TestAutomationIds();
    TestSelect();
    TestRing();
    TestMachine();
    TestPreemptPredicate();
    TestReviewProbe();
    TestReviewProbe3();
    TestSuppressReopen();
    TestForwardBound();
    TestPureRules();
    TestReclaim();
    TestNoIdentityBridgeDown();
    TestConsumedAndNotBanner();
    printf("%u checks, %u failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
