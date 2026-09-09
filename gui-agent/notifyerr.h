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
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts once it has been seen to FAIL). Each guard
 * has a compile-time defect switch that removes it; notifyerr_test MUST then exit nonzero:
 *   NOTIFYERR_DEFECT_SEVERITY  - threshold lowered to DEGRADED
 *   NOTIFYERR_DEFECT_RATELIMIT - the per-boot marker is ignored (every repeat sends)
 *   NOTIFYERR_DEFECT_CAP       - the per-boot cap is ignored
 *   NOTIFYERR_DEFECT_REDACT    - redaction always says "clean"
 *   NOTIFYERR_DEFECT_FAILOPEN  - transport failures are logged on every call (notifyerr.c)
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
    QERR_FAIL_TRANSPORT     /* glue only: policy said send, the store or the spawn failed */
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
    }
    return "?";
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
/* notifhost --notify-file format: line 1 = summary, the rest = body. dom0 prefixes the qube's
 * own name and colour to the summary (origin marking is the proxy's, unforgeable), so the guest
 * does not name itself; the COMPONENT is named here because dom0 cannot know it. The body says
 * where the detail is and that this is a once-per-boot report, so a reader who sees nothing
 * further does not conclude the fault went away. Returns the byte length, or 0 if it did not
 * fit (the caller then sends nothing - a truncated pointer to a log is worse than none). */
static inline size_t QerrComposeNotifyText(char* out, size_t cap, const char* component, const char* id,
                                    const char* summary, const char* logHint)
{
    int n = snprintf(out, cap,
        "Qubes Windows Tools, %s: %s\r\n"
        "Error id: %s. Reported once per boot; the detail is in the guest log: %s",
        component, summary, id, (logHint && *logHint) ? logHint : "see the gui-agent log directory");
    if (n <= 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return 0; }
    return (size_t)n;
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
/* Report one error. component/id: [a-z0-9-]. summary: one templated ASCII/UTF-8 sentence with
 * no secrets, no file contents. logHint: the path a human should open, or NULL. Returns the
 * decision for logging/testing; the caller ignores it - nothing here can fail the caller. */
QerrDecision QerrReport(const char* component, const char* id, int sev, const char* summary,
                        const char* logHint);
#ifdef __cplusplus
}
#endif

#endif /* QWT_NOTIFYERR_H */
