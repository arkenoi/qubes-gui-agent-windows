/*
 * toastident.h - the TOAST IDENTITY contract between the SYSTEM gui-agent (C) and the user-session
 * notification bridge notifhost.exe (C++). Included by both, like wgcbroker_ipc.h / notifyerr.h.
 *
 * WHAT IT DECIDES (docs/ADR-toasts.md 10, accepted 2026-10-04). In seamless mode a guest toast reaches
 * the user only because the agent MAPS the shell's banner window into dom0. The bridge forwards a toast
 * whose verdict is `bridge` to dom0's own notification service; its guest banner must then never be
 * mapped, or the toast shows twice (the owner's P1). The per-application ShowBanner=0 switch that used
 * to do this is retired: it was written only AFTER a toast had forwarded (so the first toast of every
 * app doubled) and stood until the bridge exited (so a later interactive toast of that app was shown
 * nowhere). Now the AGENT holds each banner unmapped until THAT toast's verdict is known.
 *
 * THE PROBLEM THIS HEADER SOLVES: tying a banner to a notification. Measured on retail 26300 and on
 * 19045 (scratchpad/toast-banner-probe, 2026-10-04): ONE ShellExperienceHost CoreWindow 'New
 * notification' serves every toast of the session (same HWND for all), the shell shows banners one at
 * a time FIFO by arrival (a queued one appears seconds later - 6.9 s measured), and a new banner arrives
 * either as a collapse-and-grow cycle (396x152 -> 396x0 -> 396x43 -> 396x152) or IN PLACE (same rect, a
 * burst of EVENT_OBJECT_LOCATIONCHANGE). So neither the HWND, nor its geometry, nor timing identifies
 * the toast a banner shows. Its CONTENT does: the card's UI Automation tree carries the app display
 * name (AutomationId SenderName), the first text line (Title on 11, TitleText on 10) and the second
 * (MessageText), as unlocalized TextBlocks; the bridge sees the same strings through the listener
 * (AppInfo.DisplayInfo.DisplayName, the ToastGeneric text elements).
 *
 * THE RULE (Jev, 2026-10-04): match by CONTENT identity; use arrival order ONLY to break a tie between
 * notifications with identical text; never match by order alone - a notification that produces no
 * banner (per-app banners off in Windows, do-not-disturb, Notification Center open, an update of an
 * existing toast) would desync an order-based matcher and misroute a window-path toast to nowhere.
 *
 * NORMALIZATION (identical on both sides BY CONSTRUCTION - both compile this code): leading/trailing
 * whitespace trimmed; every run of whitespace (incl. newlines, NBSP) folded to one space - the bridge
 * joins body lines with '\n', the agent joins UIA text blocks with ' '; bidi/format marks dropped;
 * ASCII + Latin-1 letters case-folded; each field cut at TI_NORM_MAX code units. Hashes are FNV-1a 64
 * over the UTF-16 code units of the normalized field; 0 means "absent". The log token is `Combined`:
 * logs carry hashes only, never toast text or window titles (owner rule).
 *
 * SECURITY. The records live in a section the AGENT creates and the interactive user's bridge writes
 * (R/W for IU). Everything bridge-written is UNTRUSTED input to the agent: hashes are opaque, the
 * verdict is range-checked (anything else reads as `window`, the fail-open direction), ids and ticks
 * are only compared and logged, and the ring is read with a seqlock so a torn record is dropped, not
 * used. The worst a hostile user-IL writer can do is keep the user's OWN guest banners unmapped - the
 * same privilege it already has over its own notification settings. Nothing here widens what dom0
 * trusts: the agent never maps anything on the bridge's say-so, it only declines to map.
 *
 * Pure: no allocation, no I/O, no COM. Windows types where <windows.h> exists, shimmed otherwise, so
 * the offline suites (agent/gui-agent/toasthold_test.c, tools/notifhost/toasthold_bridge_test.cpp)
 * build with gcc/g++ as well as MSVC.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts once it has been seen to FAIL):
 *   TOASTIDENT_DEFECT_NOFOLD    - whitespace is not collapsed: the bridge's "a\nb" and the banner's
 *                                 "a b" stop matching (every multi-line toast would double)
 *   TOASTIDENT_DEFECT_ORDERONLY - TiSelect takes the oldest unconsumed record regardless of content:
 *                                 the pre-2026-10-04 "arrival order" idea, which a bannerless
 *                                 notification desyncs
 *   TOASTIDENT_DEFECT_VERDICTOVERRIDE - ThIpcSetVerdict ignores onlyIfPending: the classifier's
 *                                 late verdict overrides a WindowOnly/allowlist/forward-decided route
 *   TOASTIDENT_DEFECT_TIE_SLOTORDER - equal arrival ticks are broken by ring slot instead of by
 *                                 sequence: across a ring wrap the newer record (slot 0) is taken
 *                                 for the earlier banner (review #12)
 *
 * VERDICT LIFECYCLE the bridge drives: PENDING at listing (or WINDOW/BRIDGE when the listing already
 * settles it: window-only app, allowlisted app over a live connection); the classifier's answer only
 * while PENDING (compare-exchange); the listing's final word unconditionally - WINDOW when the toast
 * cannot be forwarded (dom0 unreachable, server rejections exhausted, no verdict in time), FORWARDED
 * once dom0 acknowledged the forward. The agent suppresses on BRIDGE and FORWARDED, shows on WINDOW,
 * reopens a suppressed banner whose record turns WINDOW, and reopens one whose record is still BRIDGE
 * (not FORWARDED) when the bridge dies.
 */
#ifndef QWT_TOASTIDENT_H
#define QWT_TOASTIDENT_H

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#else
/* Shims for the offline suites on a non-Windows host. WCHAR is a UTF-16 code unit on BOTH sides. */
#include <stdint.h>
typedef uint16_t WCHAR;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef unsigned int UINT;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t ULONGLONG;
typedef int      BOOL;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* MSVC's C front end spells inline as __inline; gcc/g++ accept it too. */
#ifndef TI_INLINE
#define TI_INLINE static __inline
#endif

/* --- atomics for the shared ring (one spelling per toolchain) ------------------------------- */
#ifdef _WIN32
#define TI_LOAD32(p)        ((LONG)InterlockedCompareExchange((volatile LONG*)(p), 0, 0))
#define TI_STORE32(p, v)    ((void)InterlockedExchange((volatile LONG*)(p), (LONG)(v)))
#define TI_INC32(p)         ((LONG)InterlockedIncrement((volatile LONG*)(p)))
#define TI_BARRIER()        MemoryBarrier()
#else
#define TI_LOAD32(p)        ((LONG)__atomic_load_n((volatile LONG*)(p), __ATOMIC_SEQ_CST))
#define TI_STORE32(p, v)    __atomic_store_n((volatile LONG*)(p), (LONG)(v), __ATOMIC_SEQ_CST)
#define TI_INC32(p)         ((LONG)__atomic_add_fetch((volatile LONG*)(p), 1, __ATOMIC_SEQ_CST))
#define TI_BARRIER()        __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/* Compare-exchange: returns the value SEEN; the store happened iff that equals `expected`. */
TI_INLINE LONG TiCas32(volatile LONG* p, LONG newv, LONG expected)
{
#ifdef _WIN32
    return InterlockedCompareExchange(p, newv, expected);
#else
    LONG e = expected;
    __atomic_compare_exchange_n(p, &e, newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return e;
#endif
}

/* --- normalization ----------------------------------------------------------------------- */

/* Code units kept per field AFTER normalization. The same cut on both sides, so a long body hashes
 * identically however it was truncated by the shell's layout (UIA reports the full Text property). */
#define TI_NORM_MAX     512
/* The message prefix hashed separately: the first TI_PREFIX_UNITS normalized units. Tolerates a body
 * the banner renders differently from the listener's text elements (an attribution line appended, a
 * third text line in its own block, an elided tail) without weakening the title+sender key. */
#define TI_PREFIX_UNITS 24

TI_INLINE int TiIsSpace(WCHAR c)
{
    return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C ||
           c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Format controls Windows text stacks insert and UIA may or may not surface: bidi marks/embeddings/
 * isolates, the BOM/ZWNBSP, soft hyphen. Dropped outright on both sides. */
TI_INLINE int TiIsDropped(WCHAR c)
{
    return c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF || c == 0xAD || c == 0x200C || c == 0x200D;
}

TI_INLINE WCHAR TiFold(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);   /* Latin-1 capitals */
    return c;
}

/* Normalizes `in` (NUL-terminated; NULL = empty) into out[cap]; returns the normalized length (never
 * writes a terminator - callers hash by length). Deterministic for both compilers. */
TI_INLINE size_t TiNormalize(const WCHAR* in, WCHAR* out, size_t cap)
{
    size_t n = 0;
    int pendingSpace = 0;
    if (!in || cap == 0) return 0;
    for (; *in; in++)
    {
        WCHAR c = *in;
        if (TiIsDropped(c)) continue;
#ifdef TOASTIDENT_DEFECT_NOFOLD
        if (c == 0x0A || c == 0x0D) { if (n < cap) out[n++] = c; continue; }   /* DEFECT: newlines kept */
#endif
        if (TiIsSpace(c)) { pendingSpace = (n > 0); continue; }   /* leading space never emitted */
        if (pendingSpace) { if (n < cap) out[n++] = 0x20; pendingSpace = 0; }
        if (n < cap) out[n++] = TiFold(c);
        if (n >= cap) break;
    }
    return n;   /* a trailing run of spaces is never emitted: pendingSpace is dropped at the end */
}

/* FNV-1a 64 over UTF-16 code units, byte-wise little-endian. 0 is reserved for "absent"; a real hash
 * that lands on 0 is mapped to 1 (both sides). */
TI_INLINE UINT64 TiHashUnits(const WCHAR* s, size_t n)
{
    UINT64 h = 1469598103934665603ULL;
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++)
    {
        h ^= (UINT64)(s[i] & 0xFF);  h *= 1099511628211ULL;
        h ^= (UINT64)(s[i] >> 8);    h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

/* --- the identity ------------------------------------------------------------------------ */

typedef struct _TOAST_IDENT
{
    UINT64 Sender;         /* app display name; 0 = absent on this side */
    UINT64 Title;          /* first text line;   0 = absent */
    UINT64 Message;        /* remaining text;    0 = absent */
    UINT64 MessagePrefix;  /* first TI_PREFIX_UNITS units of the normalized message; 0 = absent */
    UINT64 Combined;       /* the LOG TOKEN: hash over sender|title|message normalized; 0 = nothing */
} TOAST_IDENT;

/* Builds the identity from three NUL-terminated strings (any may be NULL/empty). The bridge passes
 * (DisplayName, text[0], text[1..] joined by '\n'); the agent passes (SenderName, Title|TitleText,
 * MessageText blocks joined by ' '). TiNormalize makes those two spellings of the body identical. */
TI_INLINE void TiIdentFromTexts(const WCHAR* sender, const WCHAR* title, const WCHAR* message,
                                TOAST_IDENT* out)
{
    WCHAR s[TI_NORM_MAX], t[TI_NORM_MAX], m[TI_NORM_MAX];
    size_t ns = TiNormalize(sender, s, TI_NORM_MAX);
    size_t nt = TiNormalize(title, t, TI_NORM_MAX);
    size_t nm = TiNormalize(message, m, TI_NORM_MAX);
    UINT64 h = 1469598103934665603ULL;
    size_t i;
    out->Sender = TiHashUnits(s, ns);
    out->Title = TiHashUnits(t, nt);
    out->Message = TiHashUnits(m, nm);
    out->MessagePrefix = TiHashUnits(m, nm < TI_PREFIX_UNITS ? nm : TI_PREFIX_UNITS);
    if (ns == 0 && nt == 0 && nm == 0) { out->Combined = 0; return; }
    /* Combined: the three normalized fields with 0x1F between them, hashed as one string. */
    for (i = 0; i < ns; i++) { h ^= (UINT64)(s[i] & 0xFF); h *= 1099511628211ULL; h ^= (UINT64)(s[i] >> 8); h *= 1099511628211ULL; }
    h ^= 0x1F; h *= 1099511628211ULL;
    for (i = 0; i < nt; i++) { h ^= (UINT64)(t[i] & 0xFF); h *= 1099511628211ULL; h ^= (UINT64)(t[i] >> 8); h *= 1099511628211ULL; }
    h ^= 0x1F; h *= 1099511628211ULL;
    for (i = 0; i < nm; i++) { h ^= (UINT64)(m[i] & 0xFF); h *= 1099511628211ULL; h ^= (UINT64)(m[i] >> 8); h *= 1099511628211ULL; }
    out->Combined = h ? h : 1;
}

/* --- the banner's UIA element names (both id sets, measured 2026-10-04) --------------------- */
/* The card: AutomationId NormalToastView on 10 and 11 (its ClassName differs - FlexibleToastView on
 * 11 only - and is NOT keyed on). The card's Name is a LOCALIZED composite and is NOT parsed. */
TI_INLINE int TiUnitsEqualAscii(const WCHAR* s, const char* ascii)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; ascii[i]; i++) if (s[i] != (WCHAR)(unsigned char)ascii[i]) return 0;
    return s[i] == 0;
}
TI_INLINE int TiUnitsStartWithAscii(const WCHAR* s, const char* ascii)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; ascii[i]; i++) if (s[i] != (WCHAR)(unsigned char)ascii[i]) return 0;
    return 1;
}
TI_INLINE int TiIsCardAutomationId(const WCHAR* aid)    { return TiUnitsEqualAscii(aid, "NormalToastView"); }
TI_INLINE int TiIsSenderAutomationId(const WCHAR* aid)  { return TiUnitsEqualAscii(aid, "SenderName"); }
TI_INLINE int TiIsTitleAutomationId(const WCHAR* aid)   { return TiUnitsEqualAscii(aid, "Title") || TiUnitsEqualAscii(aid, "TitleText"); }
/* MessageText, and any numbered sibling a build may add (MessageText2...). DismissTextBlock, VerbText
 * (button labels) and the empty-id blocks are not message text. */
TI_INLINE int TiIsMessageAutomationId(const WCHAR* aid) { return TiUnitsStartWithAscii(aid, "MessageText"); }

/* --- matching ---------------------------------------------------------------------------- */

typedef enum _TI_MATCH
{
    TiMatchNone = 0,
    TiMatchPartial = 1,   /* same sender (or one side has none) and title; message differs or is missing on one side */
    TiMatchFull = 2       /* ...and the message matches (exactly, or by its prefix) */
} TI_MATCH;

TI_INLINE TI_MATCH TiMatch(const TOAST_IDENT* rec, const TOAST_IDENT* seen)
{
    /* The title is the key: a toast with no title on either side cannot be tied to anything. */
    if (rec->Title == 0 || seen->Title == 0 || rec->Title != seen->Title) return TiMatchNone;
    /* Two different senders with the same title are two different toasts. One side lacking the sender
     * (the listener can fail to resolve AppInfo) is not evidence against. */
    if (rec->Sender && seen->Sender && rec->Sender != seen->Sender) return TiMatchNone;
    if (rec->Message == seen->Message) return TiMatchFull;                         /* incl. both absent */
    if (rec->Message && seen->Message && rec->MessagePrefix == seen->MessagePrefix) return TiMatchFull;
    return TiMatchPartial;
}

typedef struct _TI_CANDIDATE
{
    TOAST_IDENT Ident;
    UINT64      ArrivalTick;   /* the bridge's listing tick: FIFO order */
    LONG        Seq;           /* the record's publish sequence (log/bookkeeping only) */
    int         Eligible;      /* unconsumed, within its TTL - decided by the caller */
} TI_CANDIDATE;

/* Is candidate `a` EARLIER than candidate `b`? By arrival tick; equal ticks (several toasts listed in
 * one bridge pass carry the same tick) by publish SEQUENCE - never by ring slot, which reorders across
 * a wrap (slot 0 holds the newest record once the ring has turned; review #12). */
TI_INLINE int TiEarlier(const TI_CANDIDATE* a, const TI_CANDIDATE* b)
{
#ifdef TOASTIDENT_DEFECT_TIE_SLOTORDER
    return a->ArrivalTick < b->ArrivalTick;   /* DEFECT: ties fall to whichever slot the scan met first */
#else
    return a->ArrivalTick < b->ArrivalTick || (a->ArrivalTick == b->ArrivalTick && a->Seq < b->Seq);
#endif
}

/* Picks the record a displayed banner belongs to. FULL matches win; among several, the EARLIEST
 * (TiEarlier: arrival, then sequence - the shell shows banners FIFO, so identical toasts resolve in
 * order). A PARTIAL match is accepted only when it is the ONLY candidate carrying that sender+title -
 * with two of them the message is the only discriminator and it disagreed, so nothing is claimed (the
 * caller fails open and logs it). Returns the index or -1; *quality says which kind matched. */
TI_INLINE int TiSelect(const TI_CANDIDATE* c, int n, const TOAST_IDENT* seen, TI_MATCH* quality)
{
    int best = -1, partial = -1, partials = 0, i;
    TI_MATCH q = TiMatchNone;
#ifdef TOASTIDENT_DEFECT_ORDERONLY
    /* DEFECT: arrival order alone - the oldest eligible record, whatever it says. */
    for (i = 0; i < n; i++)
        if (c[i].Eligible && (best < 0 || TiEarlier(&c[i], &c[best]))) best = i;
    if (quality) *quality = best >= 0 ? TiMatchFull : TiMatchNone;
    (void)seen; (void)partial; (void)partials; (void)q;
    return best;
#else
    for (i = 0; i < n; i++)
    {
        TI_MATCH m;
        if (!c[i].Eligible) continue;
        m = TiMatch(&c[i].Ident, seen);
        if (m == TiMatchFull)
        {
            if (best < 0 || TiEarlier(&c[i], &c[best])) best = i;
            q = TiMatchFull;
        }
        else if (m == TiMatchPartial)
        {
            partials++;
            if (partial < 0 || TiEarlier(&c[i], &c[partial])) partial = i;
        }
    }
    if (best < 0 && partials == 1) { best = partial; q = TiMatchPartial; }
    if (quality) *quality = (best >= 0) ? q : TiMatchNone;
    return best;
#endif
}

/* --- THE SHARED RING: bridge -> agent, per-notification records ----------------------------- */
/* The agent creates the section (SYSTEM full; interactive user R/W) and the auto-reset VERDICT event
 * (IU: SYNCHRONIZE|MODIFY_STATE), tells the bridge they exist with a bare --hold on its command line (the bridge
 * derives both names from its --alive name: one nonce'd prefix, suffixes _hold and _verdict - two more names put
 * the task's /tr over Task Scheduler's 261 characters),
 * and keeps the event in its main loop's wait array. The bridge writes a record when it lists a new
 * toast (verdict pending, or final at once) and updates the verdict in place when it is decided; every
 * write ends with SetEvent. Nothing polls: the agent re-examines a held banner when the event fires,
 * when its own identity read lands, or at its bounded fail-open deadline. */

#define TH_IPC_MAGIC     0x54534E51u   /* 'QNST' */
#define TH_IPC_ABI       1u
#define TH_IPC_RECORDS   32            /* toasts are sparse; a record outlives any hold by a wide margin */

#define TH_VERDICT_NONE      0         /* empty slot */
#define TH_VERDICT_PENDING   1         /* listed, verdict not yet known */
#define TH_VERDICT_BRIDGE    2         /* to be forwarded to dom0: the banner is never mapped */
#define TH_VERDICT_WINDOW    3         /* window path: the banner is mapped */
#define TH_VERDICT_FORWARDED 4         /* forwarded AND acknowledged by dom0: the suppression stands even if the
                                          bridge dies now (a `bridge` without this is reopened on bridge death) */

TI_INLINE BOOL TiVerdictSuppresses(LONG v) { return v == TH_VERDICT_BRIDGE || v == TH_VERDICT_FORWARDED; }

#define TH_REC_FLAG_ALLOWLISTED 0x1u   /* informational only */
#define TH_REC_FLAG_WINDOWONLY  0x2u
#define TH_REC_FLAG_NO_SENDER   0x4u   /* the listener could not resolve the app display name */

typedef struct _TH_IPC_RECORD
{
    volatile LONG Seq;        /* 0 = empty; else the publish sequence - written LAST (seqlock) */
    volatile LONG Verdict;    /* TH_VERDICT_*, updated in place */
    UINT32 NotifId;           /* the guest notification id (logs, verdict updates) */
    UINT32 Flags;
    UINT64 ArrivalTick;       /* GetTickCount64 at listing - one system clock for both processes */
    UINT64 AumidHash;         /* FNV-1a of the AUMID (logs/diagnosis only) */
    TOAST_IDENT Ident;
    UINT64 Reserved;
} TH_IPC_RECORD;              /* 80 bytes */

typedef struct _TH_IPC_HEADER
{
    volatile LONG Magic;       /* agent: written LAST at creation */
    LONG Abi;
    LONG RecordCount;          /* TH_IPC_RECORDS */
    volatile LONG BridgeAlive; /* bridge: 1 while its main loop runs, 0 at exit */
    volatile LONG NextSeq;     /* bridge: last sequence handed out */
    volatile LONG Seamless;    /* agent: 1 while the guest is in SEAMLESS mode (the hold exists: banners are windows the
                                  agent maps or withholds); 0 in non-seamless/fullscreen mode, where the guest draws its
                                  banner inside the one desktop window and nothing can withhold it - the bridge then
                                  FORWARDS NOTHING (every toast takes the window path), or every forwarded toast would
                                  double (docs/ADR-toasts.md 10). Updated by the agent on every mode switch. */
    UINT64 BridgeStartTick;
    UINT64 Reserved[4];
} TH_IPC_HEADER;              /* 64 bytes */

#define TH_IPC_BYTES (sizeof(TH_IPC_HEADER) + (size_t)TH_IPC_RECORDS * sizeof(TH_IPC_RECORD))

TI_INLINE TH_IPC_RECORD* ThIpcRecords(TH_IPC_HEADER* h) { return (TH_IPC_RECORD*)(h + 1); }
TI_INLINE const TH_IPC_RECORD* ThIpcRecordsC(const TH_IPC_HEADER* h) { return (const TH_IPC_RECORD*)(h + 1); }

/* Creator (agent) side: zero the block and publish the header, Magic last. */
TI_INLINE void ThIpcInit(TH_IPC_HEADER* h)
{
    size_t i;
    unsigned char* p = (unsigned char*)h;
    for (i = 0; i < TH_IPC_BYTES; i++) p[i] = 0;
    h->Abi = (LONG)TH_IPC_ABI;
    h->RecordCount = TH_IPC_RECORDS;
    TI_BARRIER();
    TI_STORE32(&h->Magic, (LONG)TH_IPC_MAGIC);
}

TI_INLINE BOOL ThIpcValid(const TH_IPC_HEADER* h)
{
    return h && TI_LOAD32(&h->Magic) == (LONG)TH_IPC_MAGIC && h->Abi == (LONG)TH_IPC_ABI &&
           h->RecordCount == TH_IPC_RECORDS;
}

/* Writer (bridge) side: one record per listed notification. Returns the sequence (>= 1). The slot is
 * invalidated (Seq=0) before its fields change and republished after, so a concurrent reader either
 * sees the old record whole, nothing, or the new record whole - never a mix. */
TI_INLINE LONG ThIpcPublish(TH_IPC_HEADER* h, UINT32 notifId, UINT32 flags, UINT64 aumidHash,
                            const TOAST_IDENT* ident, LONG verdict, UINT64 arrivalTick)
{
    LONG seq = TI_INC32(&h->NextSeq);
    TH_IPC_RECORD* r = &ThIpcRecords(h)[(UINT32)(seq - 1) % TH_IPC_RECORDS];
    if (seq <= 0) { TI_STORE32(&h->NextSeq, 1); seq = 1; r = &ThIpcRecords(h)[0]; }   /* 2^31 toasts: wrap */
    TI_STORE32(&r->Seq, 0);
    TI_BARRIER();
    r->NotifId = notifId;
    r->Flags = flags;
    r->ArrivalTick = arrivalTick;
    r->AumidHash = aumidHash;
    r->Ident = *ident;
    r->Reserved = 0;
    TI_STORE32(&r->Verdict, verdict);
    TI_BARRIER();
    TI_STORE32(&r->Seq, seq);
    return seq;
}

/* Writer side: update the verdict of the record for `notifId` (the newest if several). With
 * onlyIfPending a verdict already decided is kept - the classifier's late answer must never override
 * a route the listing already settled (WindowOnly app, allowlist, forward outcome). TRUE if found. */
TI_INLINE BOOL ThIpcSetVerdict(TH_IPC_HEADER* h, UINT32 notifId, LONG verdict, BOOL onlyIfPending)
{
    int i, best = -1;
    LONG bestSeq = 0;
    TH_IPC_RECORD* recs = ThIpcRecords(h);
    for (i = 0; i < TH_IPC_RECORDS; i++)
    {
        LONG s = TI_LOAD32(&recs[i].Seq);
        if (s != 0 && recs[i].NotifId == notifId && (best < 0 || s > bestSeq)) { best = i; bestSeq = s; }
    }
    if (best < 0) return FALSE;
#ifdef TOASTIDENT_DEFECT_VERDICTOVERRIDE
    (void)onlyIfPending;
#else
    /* A real compare-exchange, not a read-then-write: the classifier's late answer (shadow worker) and the
     * listing's final word (main thread) race on this field, and PENDING -> v must be the ONLY transition the
     * late answer can make. */
    if (onlyIfPending)
        return TiCas32(&recs[best].Verdict, verdict, TH_VERDICT_PENDING) == TH_VERDICT_PENDING;
#endif
    TI_STORE32(&recs[best].Verdict, verdict);
    return TRUE;
}

/* Reader (agent) side: a consistent copy of slot `index`, or FALSE for an empty/torn slot (retry on
 * the next pass - the writer's next SetEvent or the hold's deadline brings one). The copy's Verdict is
 * range-checked: anything but PENDING/BRIDGE/FORWARDED reads as WINDOW (fail open). */
TI_INLINE BOOL ThIpcRead(const TH_IPC_HEADER* h, int index, TH_IPC_RECORD* out)
{
    const TH_IPC_RECORD* r;
    LONG s1, s2, v;
    if (!h || index < 0 || index >= TH_IPC_RECORDS || !out) return FALSE;
    r = &ThIpcRecordsC(h)[index];
    s1 = TI_LOAD32(&r->Seq);
    if (s1 == 0) return FALSE;
    TI_BARRIER();
    out->NotifId = r->NotifId;
    out->Flags = r->Flags;
    out->ArrivalTick = r->ArrivalTick;
    out->AumidHash = r->AumidHash;
    out->Ident = r->Ident;
    out->Reserved = 0;
    v = TI_LOAD32(&r->Verdict);
    TI_BARRIER();
    s2 = TI_LOAD32(&r->Seq);
    if (s1 != s2) return FALSE;
    out->Seq = s1;
    out->Verdict = (v == TH_VERDICT_PENDING || v == TH_VERDICT_BRIDGE || v == TH_VERDICT_FORWARDED) ? v : TH_VERDICT_WINDOW;
    return TRUE;
}

#ifdef __cplusplus
}
#endif

#endif /* QWT_TOASTIDENT_H */
