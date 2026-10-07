/*
 * notifyerr.h - policy core of the SECONDARY error-delivery route (docs/DESIGN-error-notify.md).
 *
 * A guest error worth a human's attention is reported to dom0 as a notification IN ADDITION TO
 * the log line that already records it - never instead of it. The log is written first and is
 * the only complete record; this route is a best-effort courtesy copy that arrives as a dom0
 * notification through notifhost's one-shot `--notify-file` (the toast bridge's proven
 * qubes.Notifications path, spawned and forgotten).
 *
 * HONEST LIMIT (owner, 2026-09-09 - do not soften this anywhere it is repeated): the transport
 * is qrexec (`qrexec-client-vm` hands the relay command line to the LOCAL qrexec-agent service,
 * which owns the vchan). So this route delivers only while qrexec-agent is up. When qrexec is
 * down - the very case that prompted this work, a guest whose PV bus never bound, so xeniface,
 * qubesdb and qrexec were all absent at once - it delivers NOTHING, and the log on disk is the
 * only record, reachable only once some channel to the guest exists again. It is a channel for
 * "qrexec is up and nobody is reading logs", not for "everything else failed".
 *
 * THIS FILE is the pure part: severity threshold, identifier validation, redaction, the
 * per-boot de-duplication + cap bookkeeping, and the notification text. No Windows headers, no
 * allocation, no I/O - so it compiles as C (gui-agent, the offline test with gcc on any host)
 * and as C++ (notifhost, which includes it via ../../agent/gui-agent/notifyerr.h like
 * wgcbroker_ipc.h). The glue that does file/registry/process work lives in notifyerr.c (agent)
 * and in notifhost.cpp's small ReportErrorSelf; the PowerShell twin is guest/qwt-notify-error.ps1
 * and implements EXACTLY these rules against the SAME marker files, so the two languages
 * de-duplicate against each other.
 *
 * POLICY, stated once here and implemented below (the design doc restates it in prose):
 *   severity   ACTION only. The product is not delivering and will not recover on its own; a
 *              human must do something. DEGRADED (a fault with bounded self-recovery in flight)
 *              and INFO are rejected by this route: they stay in the log. Rationale: the channel
 *              spends human attention in dom0, and a DEGRADED event either resolves itself or
 *              escalates into the ACTION event that follows it (broker died -> relaunch, or
 *              QGADESLICEDOWN 30 s later), so notifying it only duplicates that ACTION.
 *   dedupe     one notification per distinct (component, id) per BOOT. Marker file
 *              <state>\<component>.<id> holding "boot=<per-boot token>"; the token is minted once
 *              per boot and shared by every process in it (PlatBootStamp), so a respawned agent or
 *              a second script in the same boot sees the earlier attempt. A marker whose token is
 *              not this boot's is stale and is overwritten. Tokens compare EXACTLY - see
 *              QerrBootMatch for why the tolerance that used to live here was a defect.
 *   cap        at most QERR_CAP_PER_BOOT notifications per boot across ALL ids (file
 *              <state>\.count, "boot=<n>\ncount=<n>"): the storm guard for a bug that mints
 *              distinct ids. Rejections are logged; they never block the caller.
 *   redaction  the text is REFUSED (not masked) if it looks like it carries a secret or a file:
 *              longer than QERR_MAX_TEXT bytes, more than QERR_MAX_LINES lines, a control
 *              character, a credential keyword, or a long opaque run (>= 40 base64-class chars,
 *              or >= 32 hex digits - keys, hashes, JWTs). Callers pass templated text: component,
 *              error id, one sentence, and the log path that has the detail. Nothing else.
 *   fail-open  every failure of the route (no marker store, no notifhost.exe, spawn failure,
 *              policy rejection) returns to the caller immediately with the operation that was
 *              reporting the error unchanged; transport failures are logged ONCE per process.
 *              A marker store that cannot be written means NO SEND (missing data fails): without
 *              persistence the dedupe would be per-process, and a helper relaunched every minute
 *              would turn one fault into a notification per minute.
 *   the window WHEN DOM0 CANNOT BE TOLD, THE USER IS (owner 2026-10-07; docs/ADR-supervision.md 6,
 *              main repo): an error the policy said to send that the transport could not deliver
 *              (failed:transport), or that the operator's gate kept from dom0 (gated), is shown as
 *              a Windows message box on the console session instead (errbox.h, WTSSendMessage) -
 *              the same text, under the SAME per-boot dedupe and cap (the marker and the count are
 *              written before the choice between dom0 and the box is made), so a window never
 *              storms and never doubles a dom0 notification. QerrWindowWanted is the one rule.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts once it has been seen to FAIL). Each guard
 * has a compile-time defect switch that removes it; notifyerr_test MUST then exit nonzero:
 *   NOTIFYERR_DEFECT_SEVERITY  - threshold lowered to DEGRADED
 *   NOTIFYERR_DEFECT_RATELIMIT - the per-boot marker is ignored (every repeat sends)
 *   NOTIFYERR_DEFECT_CAP       - the per-boot cap is ignored
 *   NOTIFYERR_DEFECT_REDACT    - redaction always says "clean"
 *   NOTIFYERR_DEFECT_FAILOPEN  - transport failures are logged on every call (notifyerr.c)
 *   NOTIFYERR_DEFECT_NOBOX     - the error window is never shown (dom0 unreachable = silence)
 *   NOTIFYERR_DEFECT_BOXSTORM  - the gated window skips the dedupe and the cap (notifyerr.c)
 */
#ifndef QWT_NOTIFYERR_H
#define QWT_NOTIFYERR_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- severities ------------------------------------------------------------------------- */
#define QERR_SEV_INFO      0
#define QERR_SEV_DEGRADED  1
#define QERR_SEV_ACTION    2

#ifdef NOTIFYERR_DEFECT_SEVERITY
#define QERR_SEV_THRESHOLD QERR_SEV_DEGRADED
#else
#define QERR_SEV_THRESHOLD QERR_SEV_ACTION
#endif

/* --- limits (the marker-file contract shared with guest/qwt-notify-error.ps1) ------------ */
#define QERR_MAX_COMPONENT     24     /* [a-z0-9-], used in file names and the text */
#define QERR_MAX_ID            40     /* [a-z0-9-] */
#define QERR_MAX_TEXT          600    /* UTF-8 bytes of summary + log hint, the whole payload */
#define QERR_MAX_LINES         6
#define QERR_CAP_PER_BOOT      8
#define QERR_OPAQUE_RUN        40     /* base64-class run length that reads as a key/blob */
#define QERR_HEX_RUN           32     /* hex run length that reads as a digest/GUID/key */

/* Where the per-boot token lives. Created REG_OPTION_VOLATILE, so the kernel drops it at shutdown
 * and its presence is exactly "this boot". The PowerShell twin (guest/qwt-notify-error.ps1) mints
 * and reads the same key, so a script and the agent share one boot identity. */
#define QERR_BOOT_KEY "SOFTWARE\\Invisible Things Lab\\Qubes Tools\\NotifyErrBoot"

typedef enum QerrDecision {
    QERR_SEND = 0,
    QERR_REJECT_SEVERITY,
    QERR_REJECT_NAME,       /* component or id outside [a-z0-9-] / too long / empty */
    QERR_REJECT_REDACT,
    QERR_SUPPRESS_DUP,
    QERR_SUPPRESS_CAP,
    QERR_FAIL_TRANSPORT,    /* glue only: policy said send, the store or the spawn failed */
    QERR_GATED              /* glue only: policy said send, the operator's gate keeps it from dom0 */
} QerrDecision;

static inline const char* QerrDecisionName(QerrDecision d)
{
    switch (d) {
    case QERR_SEND:            return "send";
    case QERR_REJECT_SEVERITY: return "rejected:severity";
    case QERR_REJECT_NAME:     return "rejected:name";
    case QERR_REJECT_REDACT:   return "rejected:redact";
    case QERR_SUPPRESS_DUP:    return "suppressed:duplicate";
    case QERR_SUPPRESS_CAP:    return "suppressed:cap";
    case QERR_FAIL_TRANSPORT:  return "failed:transport";
    case QERR_GATED:           return "gated";
    }
    return "?";
}

/* THE ERROR WINDOW'S RULE (docs/ADR-supervision.md 6; Jev: also when gated, 0.76). Shown exactly when the policy said
 * to tell dom0 and dom0 was NOT told - the transport failed, or the operator's gate kept it - and never when the
 * policy itself suppressed (a duplicate, the cap, the severity, a refused text, a bad name), nor when dom0 was told.
 * Pure; the glue asks it after the per-boot record is written, so the box shares the dedupe and the cap. */
static inline int QerrWindowWanted(QerrDecision d)
{
#ifdef NOTIFYERR_DEFECT_NOBOX
    (void)d;
    return 0;   /* DEFECT: dom0 unreachable means silence */
#else
    return d == QERR_FAIL_TRANSPORT || d == QERR_GATED;
#endif
}

/* --- names ------------------------------------------------------------------------------ */
/* Component and error ids are file-name fragments and log tokens: lower-case ASCII letters,
 * digits and '-', 1..maxLen chars, no leading/trailing '-'. Anything else is rejected. */
static inline int QerrNameValid(const char* s, size_t maxLen)
{
    size_t n;
    if (!s || !*s) return 0;
    n = strlen(s);
    if (n > maxLen) return 0;
    if (s[0] == '-' || s[n - 1] == '-') return 0;
    for (; *s; s++) {
        char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') continue;
        return 0;
    }
    return 1;
}

/* --- redaction -------------------------------------------------------------------------- */
static inline int QerrIsHex_(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int QerrIsOpaque_(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '+' || c == '/' || c == '=';
}
static inline int QerrContainsCi_(const char* hay, const char* needle)
{
    size_t nl = strlen(needle), i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* Returns NULL when the text may be sent, else a short reason. The text is the COMPLETE
 * payload (summary + log hint) as UTF-8. Refuse, never mask: a masked secret is still a
 * message we did not intend, and every legitimate caller uses templated text. */
static inline const char* QerrRedactReason(const char* utf8)
{
#ifdef NOTIFYERR_DEFECT_REDACT
    (void)utf8;
    return NULL;
#else
    static const char* const kWords[] = {
        "password", "passwd", "pwd=", "secret", "token", "apikey", "api_key", "api-key",
        "authorization", "bearer ", "-----begin", "private key", "credential", "defaultpassword"
    };
    size_t i, len, lines = 1, opaque = 0, hex = 0;
    if (!utf8) return "empty";
    len = strlen(utf8);
    if (len == 0) return "empty";
    if (len > QERR_MAX_TEXT) return "too long (file-content-shaped)";
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '\n') { lines++; }
        else if (c < 0x20 && c != '\r' && c != '\t') return "control character";
        if (QerrIsOpaque_((char)c)) { if (++opaque >= QERR_OPAQUE_RUN) return "long opaque run (key/blob-shaped)"; }
        else opaque = 0;
        if (QerrIsHex_((char)c)) { if (++hex >= QERR_HEX_RUN) return "hex run (digest/key-shaped)"; }
        else hex = 0;
    }
    if (lines > QERR_MAX_LINES) return "too many lines (file-content-shaped)";
    for (i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++)
        if (QerrContainsCi_(utf8, kWords[i])) return "credential keyword";
    return NULL;
#endif
}

/* --- boot tokens and the marker files ------------------------------------------------------ */
/* EXACT equality, and the exactness is the point.
 *
 * This was |a - b| <= QERR_BOOT_TOLERANCE_S (120 s) over a stamp derived as (wall clock - uptime).
 * Two consecutive boot stamps differ by the PREVIOUS boot's uptime plus its downtime, so a guest
 * that reboots soon after booting mints stamps only tens of seconds apart - and the tolerance then
 * declared two real boots to be one, suppressing the second boot's notification as a duplicate.
 *
 * Measured on win11-ne, 2026-09-09, agent 4.3.22: a reboot whose stamps were 73 s apart COLLIDED
 * (an ACTION error was swallowed as suppressed:duplicate); one 373 s apart did not. The 73 s case
 * is not exotic - it is the chained Windows-update reboot, i.e. precisely the situation this route
 * exists to report on. The same run showed the stamp does not drift at all within a boot (two
 * reads 100 s apart were byte-identical), so the 120 s margin was absorbing no real jitter while
 * costing real reports.
 *
 * The stamp is now an opaque per-boot token whose lifetime the OS itself maintains, so identity is
 * exact and no margin is needed or wanted. Do not reintroduce one: a margin here can only ever
 * turn a distinct boot into a duplicate, and the failure is silent. */
static inline int QerrBootMatch(long long a, long long b)
{
#ifdef NOTIFYERR_DEFECT_CLOSEREBOOT
    /* The shipped 4.3.22 comparator, restored ONLY by tools/tests/notifyerr-selftest.sh so the
     * close-reboot guards are seen to fail with the defect present. Never build this otherwise. */
    long long d = a - b;
    if (d < 0) d = -d;
    return d <= 120;
#else
    return a == b;
#endif
}

/* Marker file: "boot=<n>\n". Count file: "boot=<n>\ncount=<n>\n". Parsers are tolerant of CR and
 * trailing junk and strict on the keys; a file that does not parse is treated as absent. */
static inline int QerrParseKv_(const char* text, const char* key, long long* out)
{
    const char* p = text;
    size_t kl = strlen(key);
    while (p && *p) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            long long v = 0; int neg = 0, any = 0;
            p += kl + 1;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
            if (!any) return 0;
            *out = neg ? -v : v;
            return 1;
        }
        while (*p && *p != '\n') p++;
    }
    return 0;
}
static inline int QerrParseMarker(const char* text, long long* boot)
{
    return text && QerrParseKv_(text, "boot", boot);
}
static inline int QerrFormatMarker(char* out, size_t cap, long long boot)
{
    int n = snprintf(out, cap, "boot=%lld\n", boot);
    return n > 0 && (size_t)n < cap;
}
static inline int QerrParseCount(const char* text, long long* boot, unsigned* count)
{
    long long c;
    if (!text || !QerrParseKv_(text, "boot", boot) || !QerrParseKv_(text, "count", &c)) return 0;
    if (c < 0) return 0;
    *count = (unsigned)c;
    return 1;
}
static inline int QerrFormatCount(char* out, size_t cap, long long boot, unsigned count)
{
    int n = snprintf(out, cap, "boot=%lld\ncount=%u\n", boot, count);
    return n > 0 && (size_t)n < cap;
}

/* --- the decision ------------------------------------------------------------------------ */
/* Pure. The glue reads the two files (or reports them absent), asks here, and on QERR_SEND
 * writes the marker (nowBoot) and the count (nowBoot, *newCount) BEFORE spawning the transport.
 * `text` is the complete payload that would be sent. Order of checks is the order of cost and
 * of certainty: severity and names need nothing, redaction needs only the text, the marker and
 * the cap need the store - so a rejected event never touches the store. */
static inline QerrDecision QerrDecide(int sev, const char* component, const char* id, const char* text,
                               int markerPresent, long long markerBoot,
                               int countPresent, long long countBoot, unsigned count,
                               long long nowBoot, unsigned* newCount)
{
    unsigned have = 0;
    if (newCount) *newCount = 0;
    if (sev < QERR_SEV_THRESHOLD) return QERR_REJECT_SEVERITY;
    if (!QerrNameValid(component, QERR_MAX_COMPONENT) || !QerrNameValid(id, QERR_MAX_ID))
        return QERR_REJECT_NAME;
    if (QerrRedactReason(text) != NULL) return QERR_REJECT_REDACT;
#ifndef NOTIFYERR_DEFECT_RATELIMIT
    if (markerPresent && QerrBootMatch(markerBoot, nowBoot)) return QERR_SUPPRESS_DUP;
#else
    (void)markerPresent; (void)markerBoot;
#endif
    if (countPresent && QerrBootMatch(countBoot, nowBoot)) have = count;
#ifndef NOTIFYERR_DEFECT_CAP
    if (have >= QERR_CAP_PER_BOOT) return QERR_SUPPRESS_CAP;
#endif
    if (newCount) *newCount = have + 1;
    return QERR_SEND;
}

/* --- the notification text ---------------------------------------------------------------- */
/* notifhost --notify-file format: line 1 = the HEADER, the rest = the BODY, CRLF-separated. ONE
 * shape for every sender - this file (the agent and notifhost) and guest/qwt-notify-error.ps1 (the
 * scripts and the death reporter) - decided 2026-10-03 (rz39):
 *   header   WHAT happened to WHICH component, in human names: no codes, no file names, no counts,
 *            no product prefix (dom0 shows the source qube itself); at most ~60 characters.
 *   line 1   what it means for the user and what the system does next; what the user can do, only
 *            when there is something.
 *   line 2   the cause in words WITH the code (omitted when there is no cause to state).
 *   line 3   ONE technical line, QerrFormatTechLine below: the executable, pid, code, how long it
 *            ran, "death n this boot" or "reported once per boot", and where the evidence is.
 * The code's MEANING comes from the table of the code's SOURCE (a process exit or exception code,
 * a Win32 error the SCM reports, a service-specific code, a task result) - never the process table
 * for the others. A CR or LF inside a part would move text into the wrong line unnoticed, so each
 * part is copied with CR/LF folded to a space. Returns the byte length, or 0 if the text did not
 * fit or the header, line 1 or the technical line is empty - the caller then sends nothing: a
 * truncated notification is worse than none, and the log is still the record. */
static inline int QerrPut_(char* out, size_t cap, size_t* at, const char* s, int foldNewlines)
{
    size_t i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (foldNewlines && (c == '\r' || c == '\n')) {
            if (c == '\r' && s[i + 1] == '\n') continue;   /* CRLF -> one space */
            c = ' ';
        }
        if (*at + 1 >= cap) { out[0] = 0; return 0; }
        out[(*at)++] = c;
        out[*at] = 0;
    }
    return 1;
}
#ifdef NOTIFYERR_DEFECT_FLATBODY
#define QERR_LINE_SEP " "        /* DEFECT (render test): the body collapses into one line */
#else
#define QERR_LINE_SEP "\r\n"
#endif
static inline size_t QerrComposeNotifyText(char* out, size_t cap, const char* header, const char* next,
                                           const char* cause, const char* tech)
{
    size_t at = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!header || !*header || !next || !*next || !tech || !*tech) return 0;
    if (!QerrPut_(out, cap, &at, header, 1) || !QerrPut_(out, cap, &at, "\r\n", 0) ||
        !QerrPut_(out, cap, &at, next, 1) || !QerrPut_(out, cap, &at, QERR_LINE_SEP, 0))
        return 0;
    if (cause && *cause && (!QerrPut_(out, cap, &at, cause, 1) || !QerrPut_(out, cap, &at, QERR_LINE_SEP, 0)))
        return 0;
    if (!QerrPut_(out, cap, &at, tech, 1)) return 0;
    return at;
}

/* The technical line: "<subject>[ pid <n>][; <code>][; ran <h:mm:ss>]; <count>. Evidence: <where>."
 * subject = the executable or task; code = "exit code 2" / "exception 0xC0000409" / ... already
 * phrased by the source's table, or NULL; ran = "0:12:34" or NULL; count = "death 3 this boot" or
 * "reported once per boot"; evidence = the log path, WER folder prefix, event log + id. Returns the
 * byte length, or 0 when a required part is missing or it did not fit. */
static inline int QerrPutUl_(char* out, size_t cap, size_t* at, unsigned long v)
{
    char num[24];
    snprintf(num, sizeof(num), "%lu", v);
    return QerrPut_(out, cap, at, num, 0);
}
static inline size_t QerrFormatTechLine(char* out, size_t cap, const char* subject, unsigned long pid,
                                        const char* code, const char* ran, const char* count,
                                        const char* evidence)
{
    size_t at = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!subject || !*subject || !count || !*count || !evidence || !*evidence) return 0;
    if (!QerrPut_(out, cap, &at, subject, 1)) return 0;
    if (pid && (!QerrPut_(out, cap, &at, " pid ", 0) || !QerrPutUl_(out, cap, &at, pid))) return 0;
    if (code && *code && (!QerrPut_(out, cap, &at, "; ", 0) || !QerrPut_(out, cap, &at, code, 1))) return 0;
    if (ran && *ran && (!QerrPut_(out, cap, &at, "; ran ", 0) || !QerrPut_(out, cap, &at, ran, 1))) return 0;
    if (!QerrPut_(out, cap, &at, "; ", 0) || !QerrPut_(out, cap, &at, count, 1) ||
        !QerrPut_(out, cap, &at, ". Evidence: ", 0) || !QerrPut_(out, cap, &at, evidence, 1) ||
        !QerrPut_(out, cap, &at, ".", 0))
        return 0;
    return at;
}

/* --- a sender's own texts, as data ---------------------------------------------------------- */
/* Every notification the agent and notifhost send is one row of notifytexts.h (QerrTexts[]): the
 * route id, the four parts and the technical line's fixed pieces. Keeping them as data is what
 * lets the offline render test (gui-agent/notifyrender_test.c) produce every one of them and hold
 * each to the rules above, with no agent running. A header may hold ONE "%s" for a per-instance
 * human name (the app whose window went deaf); nothing else is formatted at run time. */
typedef struct QerrText {
    const char* key;         /* unique row name: what the callers and the render test refer to */
    const char* component;   /* route component - the sender's machine id, [a-z0-9-] */
    const char* id;          /* route id, [a-z0-9-]; a caller may pass a per-instance one */
    int         sev;         /* QERR_SEV_* */
    const char* subject;     /* the technical line's executable */
    const char* code;        /* "exit code 2" (phrased by the source's table), or NULL */
    const char* header;      /* <= 60 characters, human names only; may hold one %s */
    const char* next;        /* line 1 */
    const char* cause;       /* line 2, or NULL */
    const char* evidence;    /* where to look */
    const char* count;       /* NULL = "reported once per boot" */
} QerrText;

/* Substitutes the header's one "%s" (if any) with arg; plain copy otherwise. */
static inline int QerrFormatHeader(char* out, size_t cap, const char* header, const char* arg)
{
    const char* p;
    size_t at = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!header || !*header) return 0;
    p = strstr(header, "%s");
    if (!p) return QerrPut_(out, cap, &at, header, 1);
    {
        size_t i;
        for (i = 0; header + i < p; i++) {
            if (at + 1 >= cap) { out[0] = 0; return 0; }
            out[at++] = header[i];
            out[at] = 0;
        }
    }
    if (!QerrPut_(out, cap, &at, (arg && *arg) ? arg : "an app", 1)) return 0;
    return QerrPut_(out, cap, &at, p + 2, 1);
}

/* Pure: renders one row into the complete notify-file text. headerArg fills the header's %s;
 * pid is the sender's own. Returns the byte length, or 0 when it could not be rendered. */
static inline size_t QerrRenderText(char* out, size_t cap, const QerrText* t, const char* headerArg,
                                    unsigned long pid)
{
    char header[200];
    if (!t) return 0;
    if (!QerrFormatHeader(header, sizeof(header), t->header, headerArg)) return 0;
#ifdef NOTIFYERR_DEFECT_NOTECH
    /* DEFECT (render test): the technical line is dropped - the cause takes its place */
    (void)pid;
    return QerrComposeNotifyText(out, cap, header, t->next, NULL, t->cause ? t->cause : "-");
#else
    {
        char tech[400];
        if (!QerrFormatTechLine(tech, sizeof(tech), t->subject, pid, t->code, NULL,
                                t->count ? t->count : "reported once per boot", t->evidence))
            return 0;
        return QerrComposeNotifyText(out, cap, header, t->next, t->cause, tech);
    }
#endif
}

#ifdef __cplusplus
}
#endif

/* --- agent-side glue API (notifyerr.c; not built into notifhost) -------------------------- */
#ifdef __cplusplus
extern "C" {
#endif
/* Resolve the gate ONCE at Init (registry "NotifyErrors" DWORD, then qubesdb
 * /qubes-service/notify-errors - dom0 wins - exactly like the NotifyBridge gate; read by the
 * caller, passed in). stateDir NULL = %ProgramData%\Qubes\notify-errors. */
void QerrInit(int gateOn, const char* stateDirUtf8);
/* Report one error. component/id: [a-z0-9-]. header/next/cause/tech: the four parts described at
 * QerrComposeNotifyText - templated ASCII/UTF-8, no secrets, no file contents; cause may be NULL.
 * Returns the decision for logging/testing; the caller ignores it - nothing here can fail the
 * caller. */
QerrDecision QerrReport(const char* component, const char* id, int sev, const char* header,
                        const char* next, const char* cause, const char* tech);
/* Report one of the agent's own texts (notifytexts.h). idOverride: a per-instance id in place of
 * the row's (NULL = the row's); headerArg: fills the header's %s (NULL when it has none). The
 * technical line is composed here from the row and this process's pid. */
QerrDecision QerrReportText(const QerrText* t, const char* idOverride, const char* headerArg);
#ifdef __cplusplus
}
#endif

#endif /* QWT_NOTIFYERR_H */
