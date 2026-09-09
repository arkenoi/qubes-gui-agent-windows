/*
 * notifyerr_test - offline suite for the secondary error-delivery route (notifyerr.h policy core
 * + notifyerr.c glue, exercised through the POSIX platform layer with test hooks).
 *
 * Self-contained: no rig, no qrexec, no Windows APIs. On any host:
 *   gcc -std=c99 -Wall -Wextra -Werror -I. notifyerr.c notifyerr_test.c -o notifyerr_test && ./notifyerr_test
 * and on the CI Windows runner through agent/vs2022/notifyerr-test (which defines
 * NOTIFYERR_TEST_LAYER so notifyerr.c uses its plain-C test layer instead of the agent's Win32 one).
 * Exit 0 = every case matched; nonzero = at least one mismatch.
 *
 * WHAT IT PROVES, guard by guard - and each guard has been SEEN TO FAIL (CLAUDE.md): rebuilding
 * with one of the defect defines removes that guard and this suite MUST then exit nonzero
 * (tools/tests/notifyerr-selftest.sh runs the whole matrix and inverts the exit code):
 *   severity   NOTIFYERR_DEFECT_SEVERITY  - a DEGRADED event must be rejected, ACTION sent
 *   dedupe     NOTIFYERR_DEFECT_RATELIMIT - the same (component,id) twice in one boot sends once;
 *                                           a marker from an EARLIER boot does not suppress
 *   cap        NOTIFYERR_DEFECT_CAP       - the 9th distinct error in one boot is suppressed
 *   close reboot NOTIFYERR_DEFECT_CLOSEREBOOT - two boots whose tokens are near each other are
 *                                           still two boots; the 4.3.22 +/-120 s tolerance made a
 *                                           73 s reboot look like one and swallowed the error
 *   redaction  NOTIFYERR_DEFECT_REDACT    - secret-shaped text is refused, at the pure level and
 *                                           end-to-end (no marker, no spawn)
 *   fail-open  NOTIFYERR_DEFECT_FAILOPEN  - transport missing/failed/unwritable store: the caller
 *                                           gets a return, not a crash, and the failure is logged
 *                                           ONCE per process for repeated errors
 * Plus the gate (off -> nothing leaves the log), the notify-file text shape, and the marker
 * file contract shared with guest/qwt-notify-error.ps1.
 */
#include "notifyerr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define TestMkdir(d) _mkdir(d)
#else
#define TestMkdir(d) mkdir((d), 0700)
#endif

extern int  (*QerrTestSpawnHook)(const char* notifyPath);
extern void (*QerrTestLogHook)(const char* line);
extern long long QerrTestBootStamp;

static unsigned g_run = 0, g_fail = 0;
static char g_base[256];
static char g_dir[300];
static unsigned g_storeN = 0;

/* --- captured log + spawn ---------------------------------------------------------------- */
static char g_log[64][1024];
static unsigned g_logN = 0;
static void LogSink(const char* line)
{
    if (g_logN < 64) { strncpy(g_log[g_logN], line, 1023); g_log[g_logN][1023] = 0; }
    g_logN++;
}
static unsigned LogCount(const char* needle)
{
    unsigned i, n = 0;
    for (i = 0; i < g_logN && i < 64; i++) if (strstr(g_log[i], needle)) n++;
    return n;
}
static unsigned g_spawned = 0;
static char g_lastNotify[2048];
static int SpawnOk(const char* path)
{
    FILE* f = fopen(path, "rb");
    size_t rd = 0;
    if (f) { rd = fread(g_lastNotify, 1, sizeof(g_lastNotify) - 1, f); fclose(f); }
    g_lastNotify[rd] = 0;
    g_spawned++;
    return 1;
}
static int SpawnFail(const char* path) { (void)path; return 0; }

/* --- harness ------------------------------------------------------------------------------- */
static void Check(const char* name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}
static void CheckDecision(const char* name, QerrDecision got, QerrDecision want)
{
    g_run++;
    if (got != want) g_fail++;
    printf("%s %-58s want %-22s got %s\n", got == want ? "ok  " : "FAIL", name,
           QerrDecisionName(want), QerrDecisionName(got));
}
/* A FRESH store per section (no recursive delete needed - portable), plus cleared hooks. */
static void ResetStore(void)
{
    snprintf(g_dir, sizeof(g_dir), "%s/store%u", g_base, ++g_storeN);
    if (TestMkdir(g_dir) != 0 && errno != EEXIST) { fprintf(stderr, "cannot create %s\n", g_dir); exit(2); }
    g_logN = 0; g_spawned = 0; g_lastNotify[0] = 0;
}
static int FileExists(const char* rel)
{
    char p[512]; FILE* f;
    snprintf(p, sizeof(p), "%s/%s", g_dir, rel);
    f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
static void WriteFileRel(const char* rel, const char* text)
{
    char p[512]; FILE* f;
    snprintf(p, sizeof(p), "%s/%s", g_dir, rel);
    f = fopen(p, "wb"); if (!f) { perror(p); exit(2); }
    fputs(text, f); fclose(f);
}

int main(void)
{
    const long long BOOT = 1757400000LL;   /* any fixed per-boot token; only equality matters */
#if defined(NOTIFYERR_DEFECT_SEVERITY) || defined(NOTIFYERR_DEFECT_RATELIMIT) || \
    defined(NOTIFYERR_DEFECT_CAP) || defined(NOTIFYERR_DEFECT_REDACT) || \
    defined(NOTIFYERR_DEFECT_FAILOPEN) || defined(NOTIFYERR_DEFECT_CLOSEREBOOT)
    const int defectBuild = 1;
    printf("NOTE: a NOTIFYERR_DEFECT_* switch is compiled in - this suite MUST fail now.\n");
#else
    const int defectBuild = 0;
#endif
    {
        /* Scratch root: NOTIFYERR_TEST_DIR, else TEMP/TMPDIR, else the cwd. Portable: no mkdtemp. */
        const char* envdir = getenv("NOTIFYERR_TEST_DIR");
        const char* base = envdir && *envdir ? envdir : getenv("TEMP");
        if (!base || !*base) base = getenv("TMPDIR");
        if (!base || !*base) base = ".";
        /* UNIQUE PER PROCESS, not per second: the selftest runner starts six binaries within one
         * second, and a shared root let the clean run's markers suppress sends in the defect runs
         * (measured 2026-09-09: defect fail counts inflated 5->16). Probe suffixes until mkdir
         * creates a fresh directory; EEXIST means try the next one. */
        unsigned n;
        for (n = 0; n < 10000; n++) {
            snprintf(g_base, sizeof(g_base), "%s/notifyerr-test-%lu-%u", base, (unsigned long)time(NULL), n);
            if (TestMkdir(g_base) == 0) break;
            if (errno != EEXIST) { perror(g_base); return 2; }
        }
        if (n == 10000) { fprintf(stderr, "cannot create a fresh scratch dir under %s\n", base); return 2; }
    }

    QerrTestLogHook = LogSink;
    QerrTestBootStamp = BOOT;

    /* ---- 1. pure redaction --------------------------------------------------------------- */
    {
        const char* clean = "Qubes Windows Tools, gui-agent: de-slice broker is not running\r\n"
                            "Error id: deslicedown. Reported once per boot; the detail is in the guest log: "
                            "C:\\Qubes Logs\\gui-agent-20260909-101010.log";
        Check("redact: templated text with a log path is clean", QerrRedactReason(clean) == NULL);
        Check("redact: 'password=' refused",       QerrRedactReason("agent failed: password=hunter2") != NULL);
        Check("redact: 'DefaultPassword' refused", QerrRedactReason("LSA DefaultPassword missing") != NULL);
        Check("redact: 'Authorization' refused",   QerrRedactReason("proxy said Authorization: Basic x") != NULL);
        Check("redact: PEM header refused",        QerrRedactReason("-----BEGIN RSA PRIVATE KEY-----") != NULL);
        Check("redact: 40-char base64 run refused",
              QerrRedactReason("blob QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU2Nzg5 end") != NULL);
        Check("redact: 32 hex digits (a hash) refused",
              QerrRedactReason("hash 0123456789abcdef0123456789abcdef mismatch") != NULL);
        Check("redact: 31 hex digits pass",
              QerrRedactReason("id 0123456789abcdef0123456789abcde") == NULL);
        Check("redact: control character refused", QerrRedactReason("bad \x01 byte") != NULL);
        Check("redact: 7 lines (file-shaped) refused", QerrRedactReason("a\nb\nc\nd\ne\nf\ng") != NULL);
        {
            char big[QERR_MAX_TEXT + 50];
            size_t i;
            for (i = 0; i < sizeof(big) - 1; i++) big[i] = (i % 7 == 0) ? ' ' : 'x';
            big[sizeof(big) - 1] = 0;
            Check("redact: > QERR_MAX_TEXT bytes refused", QerrRedactReason(big) != NULL);
        }
        Check("redact: empty refused", QerrRedactReason("") != NULL);
    }

    /* ---- 2. names + marker contract ------------------------------------------------------ */
    {
        long long b = 0; unsigned c = 0; char buf[64];
        Check("name: 'gui-agent' valid", QerrNameValid("gui-agent", QERR_MAX_COMPONENT));
        Check("name: upper-case rejected", !QerrNameValid("GuiAgent", QERR_MAX_COMPONENT));
        Check("name: path chars rejected", !QerrNameValid("../x", QERR_MAX_ID));
        Check("name: leading dash rejected", !QerrNameValid("-x", QERR_MAX_ID));
        Check("name: empty rejected", !QerrNameValid("", QERR_MAX_ID));
        Check("marker: parse 'boot=N'", QerrParseMarker("boot=1757400000\n", &b) && b == 1757400000LL);
        Check("marker: parse CRLF (PowerShell writer)", QerrParseMarker("boot=1757400000\r\n", &b) && b == 1757400000LL);
        Check("marker: garbage does not parse", !QerrParseMarker("hello", &b));
        Check("count: parse both keys", QerrParseCount("boot=5\r\ncount=3\r\n", &b, &c) && b == 5 && c == 3);
        Check("count: missing count does not parse", !QerrParseCount("boot=5\n", &b, &c));
        Check("count: format round-trips", QerrFormatCount(buf, sizeof(buf), 9, 2) &&
              QerrParseCount(buf, &b, &c) && b == 9 && c == 2);
        Check("boot: the same token is one boot", QerrBootMatch(1000, 1000));
        /* NOTIFYERR_DEFECT_CLOSEREBOOT. The shipped 4.3.22 matched boot stamps with a +/-120 s
         * tolerance, so two boots whose stamps were close counted as one and the second boot's
         * ACTION error was swallowed as a duplicate. Measured on win11-ne 2026-09-09: a real
         * reboot 73 s apart did exactly that. Consecutive stamps are (previous uptime + downtime)
         * apart, so this is the ordinary chained-update reboot, not a corner case. These two are
         * the regression: they FAIL the moment any margin is reintroduced. */
        Check("boot: a token 73 s away is ANOTHER boot (close reboot, the 4.3.22 defect)",
              !QerrBootMatch(1000, 1000 + 73));
        Check("boot: a token 1 s away is ANOTHER boot", !QerrBootMatch(1000, 1000 + 1));
        Check("boot: tokens differing either way are different boots",
              !QerrBootMatch(1000, 927) && !QerrBootMatch(1000, 5000));
    }

    /* ---- 3. pure decision: severity threshold ------------------------------------------- */
    {
        unsigned nc = 0;
        const char* t = "Qubes Windows Tools, gui-agent: x\r\nError id: y. log: z";
        CheckDecision("decide: INFO rejected by severity",
            QerrDecide(QERR_SEV_INFO, "gui-agent", "y", t, 0, 0, 0, 0, 0, BOOT, &nc), QERR_REJECT_SEVERITY);
        CheckDecision("decide: DEGRADED rejected by severity",
            QerrDecide(QERR_SEV_DEGRADED, "gui-agent", "y", t, 0, 0, 0, 0, 0, BOOT, &nc), QERR_REJECT_SEVERITY);
        CheckDecision("decide: ACTION sends",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 0, 0, 0, 0, 0, BOOT, &nc), QERR_SEND);
        Check("decide: first send counts 1", nc == 1);
        CheckDecision("decide: same boot marker -> duplicate",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 1, BOOT, 0, 0, 0, BOOT, &nc), QERR_SUPPRESS_DUP);
        CheckDecision("decide: earlier-boot marker -> sends",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 1, BOOT - 5000, 0, 0, 0, BOOT, &nc), QERR_SEND);
        /* The same defect at the decision layer, not just the comparator: a marker left by a boot
         * that ended 73 s ago must not suppress this boot's error. */
        CheckDecision("decide: marker from a boot 73 s earlier -> sends (close reboot)",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 1, BOOT - 73, 0, 0, 0, BOOT, &nc), QERR_SEND);
        CheckDecision("decide: cap from a boot 73 s earlier does not count (close reboot)",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 0, 0, 1, BOOT - 73, QERR_CAP_PER_BOOT, BOOT, &nc),
            QERR_SEND);
        CheckDecision("decide: cap reached -> suppressed",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 0, 0, 1, BOOT, QERR_CAP_PER_BOOT, BOOT, &nc), QERR_SUPPRESS_CAP);
        CheckDecision("decide: cap from an earlier boot does not count",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", t, 0, 0, 1, BOOT - 5000, QERR_CAP_PER_BOOT, BOOT, &nc), QERR_SEND);
        Check("decide: earlier-boot cap restarts the count at 1", nc == 1);
        CheckDecision("decide: bad id rejected",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "Bad Id", t, 0, 0, 0, 0, 0, BOOT, &nc), QERR_REJECT_NAME);
        CheckDecision("decide: secret-shaped text rejected before the store",
            QerrDecide(QERR_SEV_ACTION, "gui-agent", "y", "password=abc", 0, 0, 0, 0, 0, BOOT, &nc), QERR_REJECT_REDACT);
    }

    /* ---- 4. glue end-to-end: gate off ---------------------------------------------------- */
    ResetStore();
    QerrTestSpawnHook = SpawnOk;
    QerrInit(0, g_dir);
    CheckDecision("gate off: ACTION error is not sent", QerrReport("gui-agent", "deslicedown", QERR_SEV_ACTION, "x", NULL), QERR_REJECT_SEVERITY);
    Check("gate off: nothing spawned, no marker", g_spawned == 0 && !FileExists("gui-agent.deslicedown"));

    /* ---- 5. glue end-to-end: sends, dedupe, cap ----------------------------------------- */
    ResetStore();
    QerrInit(1, g_dir);
    CheckDecision("send: first ACTION report sends",
        QerrReport("gui-agent", "deslicedown", QERR_SEV_ACTION, "de-slice broker is not running", "C:\\Qubes Logs\\gui-agent.log"), QERR_SEND);
    Check("send: notifhost spawned once with a file", g_spawned == 1);
    Check("send: notify text summary line names the component",
          strncmp(g_lastNotify, "Qubes Windows Tools, gui-agent: de-slice broker is not running\r\n", 62) == 0);
    Check("send: notify text body carries the id and the log pointer",
          strstr(g_lastNotify, "Error id: deslicedown.") != NULL &&
          strstr(g_lastNotify, "C:\\Qubes Logs\\gui-agent.log") != NULL);
    Check("send: marker and count files written", FileExists("gui-agent.deslicedown") && FileExists(".count"));
    CheckDecision("dedupe: the same error again this boot is suppressed",
        QerrReport("gui-agent", "deslicedown", QERR_SEV_ACTION, "de-slice broker is not running", NULL), QERR_SUPPRESS_DUP);
    Check("dedupe: no second spawn", g_spawned == 1);
    CheckDecision("dedupe: a different id still sends",
        QerrReport("gui-agent", "deskstuck", QERR_SEV_ACTION, "secure desktop for 30 s", NULL), QERR_SEND);
    CheckDecision("severity: DEGRADED report is rejected end-to-end",
        QerrReport("gui-agent", "brokerdied", QERR_SEV_DEGRADED, "broker died, relaunching", NULL), QERR_REJECT_SEVERITY);
    Check("severity: rejected event left no marker and no spawn", !FileExists("gui-agent.brokerdied") && g_spawned == 2);
    /* a marker from the previous boot must not suppress */
    WriteFileRel("gui-agent.oldboot", "boot=1757300000\n");
    CheckDecision("dedupe: marker from an earlier boot does not suppress",
        QerrReport("gui-agent", "oldboot", QERR_SEV_ACTION, "x", NULL), QERR_SEND);
    /* cap: we are at 3 sends; push to the cap with distinct ids */
    {
        char id[16]; unsigned i; QerrDecision last = QERR_SEND;
        for (i = 3; i < QERR_CAP_PER_BOOT; i++) {
            snprintf(id, sizeof(id), "cap-%u", i);
            last = QerrReport("gui-agent", id, QERR_SEV_ACTION, "x", NULL);
        }
        CheckDecision("cap: the 8th distinct error still sends", last, QERR_SEND);
        CheckDecision("cap: the 9th distinct error is suppressed",
            QerrReport("gui-agent", "cap-9", QERR_SEV_ACTION, "x", NULL), QERR_SUPPRESS_CAP);
        Check("cap: exactly QERR_CAP_PER_BOOT spawns this boot", g_spawned == QERR_CAP_PER_BOOT);
        Check("cap: suppression was logged", LogCount("suppressed:cap") == 1);
    }

    /* ---- 6. glue end-to-end: redaction refuses ----------------------------------------- */
    ResetStore();
    QerrInit(1, g_dir);
    CheckDecision("redact e2e: summary with a password is refused",
        QerrReport("activate-idd", "reboot-refused", QERR_SEV_ACTION, "shutdown refused, password=abc", NULL), QERR_REJECT_REDACT);
    Check("redact e2e: refused text spawned nothing and left no marker",
          g_spawned == 0 && !FileExists("activate-idd.reboot-refused"));
    Check("redact e2e: refusal logged", LogCount("rejected:redact") == 1);

    /* ---- 7. fail-open ------------------------------------------------------------------ */
    ResetStore();
    QerrInit(1, g_dir);
    QerrTestSpawnHook = NULL;   /* notifhost.exe missing */
    CheckDecision("fail-open: exe missing -> caller gets a return, transport failed",
        QerrReport("gui-agent", "a", QERR_SEV_ACTION, "x", NULL), QERR_FAIL_TRANSPORT);
    (void)QerrReport("gui-agent", "b", QERR_SEV_ACTION, "x", NULL);
    (void)QerrReport("gui-agent", "c", QERR_SEV_ACTION, "x", NULL);
    Check("fail-open: three failures, the missing exe logged ONCE", LogCount("NOT PRESENT") == 1);
    QerrTestSpawnHook = SpawnFail;
    (void)QerrReport("gui-agent", "d", QERR_SEV_ACTION, "x", NULL);
    (void)QerrReport("gui-agent", "e", QERR_SEV_ACTION, "x", NULL);
    Check("fail-open: spawn failures logged ONCE", LogCount("CreateProcess") == 1);
    Check("fail-open: caller reached this line (no crash, no exit)", 1);
    /* unwritable store: no send, one line */
    {
        char bad[400];
        snprintf(bad, sizeof(bad), "%s/.count", g_dir);   /* a FILE where a dir is needed */
        WriteFileRel(".count", "boot=1\ncount=0\n");
        g_logN = 0;
        QerrInit(1, bad);
        QerrTestSpawnHook = SpawnOk; g_spawned = 0;
        CheckDecision("fail-open: unwritable store -> no send",
            QerrReport("gui-agent", "f", QERR_SEV_ACTION, "x", NULL), QERR_FAIL_TRANSPORT);
        (void)QerrReport("gui-agent", "g", QERR_SEV_ACTION, "x", NULL);
        Check("fail-open: unwritable store spawned nothing", g_spawned == 0);
        Check("fail-open: unwritable store logged ONCE", LogCount("not writable") == 1);
    }

    printf("--- %u checks, %u failed%s\n", g_run, g_fail, defectBuild ? " (defect build: failure expected)" : "");
    if (defectBuild && g_fail == 0) {
        printf("DEFECT BUILD PASSED - the guard it removes is decoration. FAIL.\n");
        return 3;
    }
    return g_fail ? 1 : 0;
}
