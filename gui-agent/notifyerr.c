/*
 * notifyerr.c - gui-agent glue for the secondary error-delivery route (notifyerr.h has the
 * policy and the honest limit; docs/DESIGN-error-notify.md has the design).
 *
 * QerrReport(): policy decision (pure, notifyerr.h) -> marker + count files under the state dir
 * -> a UTF-16LE notify file -> `notifhost.exe --notify-file <path>` spawned next to
 * gui-agent.exe and NOT waited for. That is the whole transport, and it is the transport the
 * QGADIRECTSUPPRESS user message already uses (main.c DirectSuppressNotifyUser): a plain
 * CreateProcess from the SYSTEM agent, which already lives in the interactive session, with no
 * Task Scheduler and no shell dependency - the fewest links, because it is needed when things
 * are broken. What remains, unavoidably: qrexec-agent (notifhost speaks qubes.Notifications
 * through qrexec-client-vm, which hands the relay to the local qrexec-agent service), the
 * packaged notifhost.exe, and dom0 policy. None of that is checked here beyond "the exe
 * exists" - notifhost logs its own outcome (NOTIFY one-shot: sent ok=...) in bridge.log.
 *
 * THE ERROR WINDOW (owner 2026-10-07, docs/ADR-supervision.md 6): when the policy said to tell
 * dom0 and dom0 was not told - the transport failed, or the operator's gate keeps the route off -
 * the same text is shown as a Windows message box on the console session (errbox.h), AFTER the
 * per-boot record was written, so the box shares the route's dedupe and cap: never a storm, never
 * a box beside a dom0 notification for one error. Without a per-boot record (no boot token, the
 * store unwritable) there is no dedupe, so the box is shown at most once per process then.
 *
 * FAIL-OPEN CONTRACT: this function never blocks (no waits, no round trips), never raises, and
 * returns to the caller with its state untouched whatever happened. Its own failures are
 * logged ONCE per process per kind (no exe, spawn failed, store unwritable) - the
 * NOTIFYERR_DEFECT_FAILOPEN switch turns that into every-call logging so the offline test can
 * watch the guard fail.
 *
 * PORTABLE ON PURPOSE: the platform layer at the bottom has a Win32 body (the agent) and a plain-C
 * TEST body (notifyerr_test.c: gcc on any host, or MSVC with NOTIFYERR_TEST_LAYER defined by the
 * notifyerr-test vcxproj - stdio only, the spawn, the box and the log routed through test hooks),
 * so the offline suite exercises THIS file's control flow - the fail-open path, the marker and
 * count writes, the once-per-process logging, the window - not a re-implementation of it.
 */
#include "notifyerr.h"
#include <stdarg.h>
#include <stdlib.h>

#if defined(_WIN32) && !defined(NOTIFYERR_TEST_LAYER)
#define QERR_AGENT_LAYER 1
#endif

#ifdef QERR_AGENT_LAYER
#include <windows.h>
#include <strsafe.h>
#include <log.h>
#include "errbox.h"
#else
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define QerrMkdir(d) _mkdir(d)
#else
#define QerrMkdir(d) mkdir((d), 0700)
#endif
#endif

static int  g_QerrGate = 0;
static char g_QerrStateDir[512] = { 0 };

/* --- platform layer ----------------------------------------------------------------------- */
static void      PlatLog(const char* fmt, ...);
static void      PlatLogInfo(const char* fmt, ...);
static long long PlatBootStamp(void);
static int       PlatEnsureDir(const char* dir);
static int       PlatReadSmall(const char* path, char* buf, size_t cap);   /* 1 = read, 0 = absent/failed */
static int       PlatWriteSmall(const char* path, const char* text);      /* 1 = written */
static int       PlatWriteNotifyFile(const char* path, const char* utf8); /* UTF-16LE + BOM */
static int       PlatSpawnNotify(const char* notifyPath, int* exeMissing);
static int       PlatShowErrorBox(const char* header, const char* text, unsigned long* error);   /* 1 = shown */
static void      PlatDefaultStateDir(char* out, size_t cap);
static unsigned long PlatPid(void);                                       /* this process, for the technical line */

#ifndef QERR_AGENT_LAYER
/* Test hooks (notifyerr_test.c): the spawn outcome, the box and the log sink are the test's. */
int  (*QerrTestSpawnHook)(const char* notifyPath) = NULL;   /* NULL = "notifhost.exe missing" */
int  (*QerrTestBoxHook)(const char* header, const char* text) = NULL;   /* NULL = the box cannot be shown */
void (*QerrTestLogHook)(const char* line) = NULL;
long long QerrTestBootStamp = 0;                              /* 0 = wall clock (the suite always pins it) */
unsigned long QerrTestPid = 4242;                             /* the pid the technical line shows under test */
#endif

/* --- API ---------------------------------------------------------------------------------- */
void QerrInit(int gateOn, const char* stateDirUtf8)
{
    g_QerrGate = gateOn ? 1 : 0;
    if (stateDirUtf8 && *stateDirUtf8) {
        snprintf(g_QerrStateDir, sizeof(g_QerrStateDir), "%s", stateDirUtf8);
    } else {
        PlatDefaultStateDir(g_QerrStateDir, sizeof(g_QerrStateDir));
    }
    PlatLogInfo("NOTIFYERR gate: enabled=%d state=%s (secondary route: dom0 notification via "
            "notifhost/qubes.Notifications; needs qrexec-agent; the log stays primary; when dom0 cannot be "
            "told - transport failed, or gate off - the error is shown as a window on the console session)",
            g_QerrGate, g_QerrStateDir);
}

/* Once-per-process failure logging. Under NOTIFYERR_DEFECT_FAILOPEN every call logs, which is
 * the storm the guard exists to prevent and what the offline test must catch. */
static void LogOnce(int* flag, const char* fmt, ...)
{
    char line[1024];
    va_list ap;
#ifdef NOTIFYERR_DEFECT_FAILOPEN
    (void)flag;
#else
    if (*flag) return;
    *flag = 1;
#endif
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    PlatLog("%s", line);
}

/* THE WINDOW: the one place the box is shown from. d is what the route decided; the box appears only when the
 * pure rule says dom0 was not told (QerrWindowWanted). The outcome is logged either way - a box that could not be
 * shown is a loud error of its own, since it was the last way to reach a human. */
static void ShowWindowFallback(QerrDecision d, const char* component, const char* id, const char* header,
                               const char* text, const char* why)
{
    unsigned long err = 0;
    if (!QerrWindowWanted(d)) return;
    if (PlatShowErrorBox(header, text, &err))
        PlatLog("QGAERRBOX %s.%s shown as an error window on the console session (%s)",
                component ? component : "-", id ? id : "-", why);
    else
        PlatLog("QGAERRBOX %s.%s could NOT be shown as an error window (error %lu) after %s - the log is the only "
                "record", component ? component : "-", id ? id : "-", err, why);
}

/* No per-boot record could be written (no boot token, the store unwritable): no dedupe is possible, so the box is
 * shown at most ONCE per process - a window per call would storm, which is the defect the dedupe exists to prevent. */
static void ShowWindowWithoutRecord(const char* component, const char* id, const char* header, const char* text,
                                    const char* why)
{
    static int s_shown = 0;
    if (s_shown) return;
    s_shown = 1;
    ShowWindowFallback(QERR_FAIL_TRANSPORT, component, id, header, text, why);
}

QerrDecision QerrReport(const char* component, const char* id, int sev, const char* header,
                        const char* next, const char* cause, const char* tech)
{
    static int s_LoggedStore = 0, s_LoggedNoExe = 0, s_LoggedSpawn = 0;
    static unsigned s_Seq = 0;
    char text[QERR_MAX_TEXT + 256];
    char markerPath[640], countPath[640], notifyPath[640];
    char fileBuf[128];
    long long markerBoot = 0, countBoot = 0, now;
    unsigned count = 0, newCount = 0;
    int markerPresent, countPresent, exeMissing = 0;
    QerrDecision d;

    /* Compose first: the text is what redaction judges, and a text that does not fit - or lacks
     * its header, line 1 or technical line - is not sent at all (QerrComposeNotifyText returns 0).
     * Logged: a notification that is never composed is otherwise a silent one. */
    if (!QerrComposeNotifyText(text, sizeof(text), header ? header : "", next ? next : "", cause,
                               tech ? tech : ""))
    {
        PlatLog("NOTIFYERR %s.%s not sent: rejected:redact (the text did not compose - empty "
                "header, line 1 or technical line, or over %u bytes)",
                component ? component : "-", id ? id : "-", (unsigned)sizeof(text));
        return QERR_REJECT_REDACT;
    }

#ifdef NOTIFYERR_DEFECT_BOXSTORM
    /* DEFECT: the gated window skips the policy - a box per call, duplicates and the cap ignored. */
    if (!g_QerrGate && sev >= QERR_SEV_THRESHOLD) {
        ShowWindowFallback(QERR_GATED, component, id, header, text, "gated (defect: no dedupe)");
        return QERR_GATED;
    }
#endif

    /* Cheap, store-free checks come first inside QerrDecide; only a candidate for sending
     * reads the store. Read both files up front anyway - they are tiny and this keeps the
     * decision a single pure call the test can pin. */
    now = PlatBootStamp();
    if (!now) {
        /* No boot identity means neither once-per-boot nor the cap can be honoured. Guessing would
         * either storm dom0 or swallow errors, so this fails - LOUDLY, because on a guest where the
         * agent runs as SYSTEM this cannot happen by design and is therefore a bug of ours, not a
         * condition to degrade around. The window is still the user's last chance to see it: once. */
        PlatLog("QGANOTIFYERR no per-boot token (HKLM\\%s): dedupe and the per-boot cap cannot be "
                "honoured, so %s.%s is NOT notified - it is in the log only",
                QERR_BOOT_KEY, component ? component : "-", id ? id : "-");
        if (sev >= QERR_SEV_THRESHOLD)
            ShowWindowWithoutRecord(component, id, header, text, "no boot identity");
        return QERR_FAIL_TRANSPORT;
    }
    snprintf(markerPath, sizeof(markerPath), "%s/%s.%s", g_QerrStateDir,
             component ? component : "-", id ? id : "-");
    snprintf(countPath, sizeof(countPath), "%s/.count", g_QerrStateDir);
    markerPresent = PlatReadSmall(markerPath, fileBuf, sizeof(fileBuf)) && QerrParseMarker(fileBuf, &markerBoot);
    countPresent  = PlatReadSmall(countPath, fileBuf, sizeof(fileBuf)) && QerrParseCount(fileBuf, &countBoot, &count);

    d = QerrDecide(sev, component, id, text, markerPresent, markerBoot, countPresent, countBoot,
                   count, now, &newCount);
    if (d != QERR_SEND) {
        /* Rejections are not failures of the route; they are the policy working. One log line
         * each at debug cost - severity rejections are the common case and stay quiet. No window:
         * the policy suppressed it, dom0 was not meant to be told either. */
        if (d != QERR_REJECT_SEVERITY)
            PlatLog("NOTIFYERR %s.%s not sent: %s", component ? component : "-", id ? id : "-",
                    QerrDecisionName(d));
        return d;
    }

    /* MISSING DATA FAILS: no persisted marker -> no send. Without persistence the dedupe is
     * per-process, and a relaunched helper would notify once per launch. The record is written
     * BEFORE the choice between dom0 and the window, so the window shares it. */
    if (!PlatEnsureDir(g_QerrStateDir) ||
        !QerrFormatMarker(fileBuf, sizeof(fileBuf), now) || !PlatWriteSmall(markerPath, fileBuf) ||
        !QerrFormatCount(fileBuf, sizeof(fileBuf), now, newCount) || !PlatWriteSmall(countPath, fileBuf))
    {
        LogOnce(&s_LoggedStore, "NOTIFYERR state dir %s is not writable - the route is OFF for "
                "this process (a notification with no once-per-boot record would repeat)",
                g_QerrStateDir);
        ShowWindowWithoutRecord(component, id, header, text, "the state dir could not be written");
        return QERR_FAIL_TRANSPORT;
    }

    /* GATED: the operator turned the route off, so dom0 is not told - but "not available" includes a
     * disabled route (Jev 0.76), and a loud error must reach a human: the window, under the record
     * just written (once per (component, id) per boot, inside the cap). */
    if (!g_QerrGate) {
        PlatLog("NOTIFYERR %s.%s not sent to dom0: gated (service.notify-errors off) - shown as a window instead",
                component, id);
        ShowWindowFallback(QERR_GATED, component, id, header, text, "gated");
        return QERR_GATED;
    }

    snprintf(notifyPath, sizeof(notifyPath), "%s/out-%s-%u.txt", g_QerrStateDir,
             component ? component : "-", ++s_Seq);
    if (!PlatWriteNotifyFile(notifyPath, text)) {
        LogOnce(&s_LoggedStore, "NOTIFYERR cannot write %s - notification not sent", notifyPath);
        ShowWindowFallback(QERR_FAIL_TRANSPORT, component, id, header, text, "the notify file could not be written");
        return QERR_FAIL_TRANSPORT;
    }
    if (!PlatSpawnNotify(notifyPath, &exeMissing)) {
        if (exeMissing)
            LogOnce(&s_LoggedNoExe, "NOTIFYERR notifhost.exe is NOT PRESENT next to gui-agent.exe - "
                    "dom0 notifications cannot be sent (PACKAGING GAP in the reporting path; the "
                    "log remains the record)");
        else
            LogOnce(&s_LoggedSpawn, "NOTIFYERR CreateProcess(notifhost --notify-file) failed - "
                    "notification not sent (the log remains the record)");
        ShowWindowFallback(QERR_FAIL_TRANSPORT, component, id, header, text,
                           exeMissing ? "notifhost.exe is missing" : "notifhost could not be started");
        return QERR_FAIL_TRANSPORT;
    }
    PlatLog("NOTIFYERR %s.%s sent to dom0 (#%u this boot; delivery is notifhost's to log)",
            component, id, newCount);
    return QERR_SEND;
}

/* One of the agent's own texts (notifytexts.h), rendered the way the offline render test renders
 * it: the header's %s filled, the technical line composed from the row and this process's pid. */
QerrDecision QerrReportText(const QerrText* t, const char* idOverride, const char* headerArg)
{
    char header[200], tech[400];
    if (!t) {
        PlatLog("QGANOTIFYERR a notification text row is missing (a bug of ours: the key a call "
                "site asked for is not in notifytexts.h) - nothing sent");
        return QERR_REJECT_NAME;
    }
    if (!QerrFormatHeader(header, sizeof(header), t->header, headerArg) ||
        !QerrFormatTechLine(tech, sizeof(tech), t->subject, PlatPid(), t->code, NULL,
                            t->count ? t->count : "reported once per boot", t->evidence))
    {
        PlatLog("NOTIFYERR %s.%s not sent: the header or the technical line did not render",
                t->component, idOverride ? idOverride : t->id);
        return QERR_REJECT_REDACT;
    }
    return QerrReport(t->component, idOverride ? idOverride : t->id, t->sev, header, t->next,
                      t->cause, tech);
}

/* ========================================================================================= */
#ifdef QERR_AGENT_LAYER
/* --- Win32 platform layer (the agent) ----------------------------------------------------- */

static void PlatLog(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)StringCchVPrintfA(line, RTL_NUMBER_OF(line), fmt, ap);
    va_end(ap);
    LogWarning("%S", line);   /* WARNING: visible in the default log like QGADESLICEDOWN */
}

/* THE SAME CHANNEL AT INFO, for the lines that report CONFIGURATION rather than a fault. PlatLog
 * put everything at WARNING, so the gate's own status line - what the gate is set to, printed once
 * per agent instance - arrived as a warning: 6 of the ~20 agent warnings on a fully idle clean
 * cycle, measured 2026-10-08. Owner the same day, for our own components: "no warnings on normal
 * operation. warning means something is not quite normal, yet workable." Nothing a fault reports
 * moves: the once-per-process failure line, the did-not-render line and QGAERRBOX all keep
 * PlatLog. */
static void PlatLogInfo(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)StringCchVPrintfA(line, RTL_NUMBER_OF(line), fmt, ap);
    va_end(ap);
    LogInfo("%S", line);
}

/* An opaque token minted once per boot and identical for every process in that boot.
 *
 * A VOLATILE registry key is the OS's own per-boot object: the kernel discards it at shutdown, so
 * the key's EXISTENCE is the boot. No clock arithmetic is involved, which matters twice over -
 * the old (wall clock - uptime) derivation needed a tolerance and so could not tell two close
 * reboots apart (see QerrBootMatch), and it also moved whenever the wall clock was set, which on
 * a Qubes guest is routine (the host clock is pushed in after every start).
 *
 * Returns 0 when the token cannot be established. That is NOT a fallback to some other identity:
 * without a boot identity neither once-per-boot nor the per-boot cap can be honoured, and silently
 * guessing would either storm dom0 or swallow errors. QerrReport turns 0 into a loud failure.
 *
 * The mint race is resolved FAIL-OPEN on purpose: if two processes mint at once one may briefly
 * read its own value, costing at most one duplicate notification. A duplicate is a nuisance; a
 * suppressed ACTION error is the defect this whole route exists to prevent. */
static long long PlatBootStamp(void)
{
    static volatile LONG64 s_cached = 0;
    HKEY key = NULL;
    DWORD disp = 0, type = 0, cb = (DWORD)sizeof(LONG64);
    LONG64 token = 0;

    if (s_cached) return (long long)s_cached;

    /* KEY_WOW64_64KEY pins the NATIVE view. HKLM\SOFTWARE is WOW64-redirected, so a 32-bit caller
     * would otherwise mint its own token under Wow6432Node - and the agent (x64) and a 32-bit
     * PowerShell host would then hold two different ideas of "this boot", quietly undoing the
     * cross-language dedupe this key exists to provide. The PowerShell twin pins Registry64 for
     * the same reason. */
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, QERR_BOOT_KEY, 0, NULL, REG_OPTION_VOLATILE,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, &disp) != ERROR_SUCCESS)
        return 0;

    if (disp == REG_OPENED_EXISTING_KEY &&
        RegQueryValueExA(key, "Token", NULL, &type, (LPBYTE)&token, &cb) == ERROR_SUCCESS &&
        type == REG_QWORD && cb == (DWORD)sizeof(LONG64) && token != 0)
    {
        RegCloseKey(key);
        InterlockedCompareExchange64(&s_cached, token, 0);
        return (long long)s_cached;
    }

    /* Mint. The token only ever needs to differ from the PREVIOUS boot's, so the 100 ns system
     * time mixed with the pid is ample; 0 is reserved as the "no token" sentinel. */
    {
        FILETIME ft; ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        token = (LONG64)(u.QuadPart ^ ((ULONGLONG)GetCurrentProcessId() << 48));
        if (!token) token = 1;
    }
    if (RegSetValueExA(key, "Token", 0, REG_QWORD, (const BYTE*)&token, sizeof(token)) != ERROR_SUCCESS)
        token = 0;
    RegCloseKey(key);
    if (!token) return 0;
    InterlockedCompareExchange64(&s_cached, token, 0);
    return (long long)s_cached;
}

static void PlatDefaultStateDir(char* out, size_t cap)
{
    char pd[MAX_PATH] = { 0 };
    if (!GetEnvironmentVariableA("ProgramData", pd, RTL_NUMBER_OF(pd)) || !pd[0])
        StringCchCopyA(pd, RTL_NUMBER_OF(pd), "C:\\ProgramData");
    StringCchPrintfA(out, cap, "%s\\Qubes\\notify-errors", pd);
}

static unsigned long PlatPid(void)
{
    return (unsigned long)GetCurrentProcessId();
}

static int PlatEnsureDir(const char* dir)
{
    char parent[512]; char* sl;
    if (CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    StringCchCopyA(parent, RTL_NUMBER_OF(parent), dir);
    sl = strrchr(parent, '\\');
    if (!sl) return 0;
    *sl = 0;
    if (!CreateDirectoryA(parent, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    return CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int PlatReadSmall(const char* path, char* buf, size_t cap)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD rd = 0; BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = ReadFile(h, buf, (DWORD)(cap - 1), &rd, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    buf[rd] = 0;
    return 1;
}

static int PlatWriteSmall(const char* path, const char* text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0; BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, text, (DWORD)strlen(text), &wr, NULL);
    CloseHandle(h);
    return ok && wr == strlen(text);
}

static int PlatWriteNotifyFile(const char* path, const char* utf8)
{
    static const BYTE bom[2] = { 0xFF, 0xFE };
    WCHAR wide[QERR_MAX_TEXT + 256];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)RTL_NUMBER_OF(wide));
    HANDLE h; DWORD wr = 0; BOOL ok;
    if (n <= 1) return 0;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, bom, 2, &wr, NULL) &&
         WriteFile(h, wide, (DWORD)((n - 1) * sizeof(WCHAR)), &wr, NULL);
    CloseHandle(h);
    return ok ? 1 : 0;
}

static int PlatSpawnNotify(const char* notifyPath, int* exeMissing)
{
    WCHAR exe[MAX_PATH] = { 0 }, cmd[MAX_PATH * 2], wpath[640];
    WCHAR* sl;
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    *exeMissing = 0;
    if (!GetModuleFileNameW(NULL, exe, RTL_NUMBER_OF(exe))) return 0;
    sl = wcsrchr(exe, L'\\'); if (sl) *(sl + 1) = 0;
    StringCchCatW(exe, RTL_NUMBER_OF(exe), L"notifhost.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { *exeMissing = 1; return 0; }
    if (!MultiByteToWideChar(CP_UTF8, 0, notifyPath, -1, wpath, (int)RTL_NUMBER_OF(wpath))) return 0;
    StringCchPrintfW(cmd, RTL_NUMBER_OF(cmd), L"\"%s\" --notify-file \"%s\"", exe, wpath);
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);   /* fire and forget: no wait, ever */
    return 1;
}

/* The box: a system-drawn message box on the console session (errbox.h). Non-blocking; the box outlives this call. */
static int PlatShowErrorBox(const char* header, const char* text, unsigned long* error)
{
    DWORD e = 0;
    const BOOL ok = QerrShowErrorBox(header, text, &e);
    if (error) *error = (unsigned long)e;
    return ok ? 1 : 0;
}

#else
/* --- plain-C test platform layer (offline suite only, any host) --------------------------- */

static void PlatLog(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (QerrTestLogHook) QerrTestLogHook(line); else fprintf(stderr, "%s\n", line);
}

/* The offline twin of PlatLogInfo. It goes through the SAME test hook, so the suite still sees the
 * line and its assertions do not change - only the level the Win32 layer picks does. */
static void PlatLogInfo(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (QerrTestLogHook) QerrTestLogHook(line); else fprintf(stderr, "%s\n", line);
}

static long long PlatBootStamp(void)
{
    /* The suite pins the token. Unpinned there is no per-boot object in plain C, so latch one
     * value for the life of the process: tokens now compare EXACTLY, and a bare time(NULL) read
     * twice would look like two different boots a second apart. */
    static long long s_token = 0;
    if (QerrTestBootStamp) return QerrTestBootStamp;
    if (!s_token) s_token = (long long)time(NULL);
    return s_token;
}

static void PlatDefaultStateDir(char* out, size_t cap)
{
    snprintf(out, cap, "%s", "qwt-notify-errors-test");
}

static unsigned long PlatPid(void)
{
    return QerrTestPid;   /* pinned: the suite compares rendered text byte for byte */
}

static int PlatEnsureDir(const char* dir)
{
    if (QerrMkdir(dir) == 0 || errno == EEXIST) return 1;
    return 0;
}

static int PlatReadSmall(const char* path, char* buf, size_t cap)
{
    FILE* f = fopen(path, "rb");
    size_t rd;
    if (!f) return 0;
    rd = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[rd] = 0;
    return 1;
}

static int PlatWriteSmall(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    size_t n = strlen(text);
    int ok;
    if (!f) return 0;
    ok = fwrite(text, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

static int PlatWriteNotifyFile(const char* path, const char* utf8)
{
    /* The test reads it back as UTF-8; the BOM/UTF-16 encoding is a Win32 detail. */
    return PlatWriteSmall(path, utf8);
}

static int PlatSpawnNotify(const char* notifyPath, int* exeMissing)
{
    *exeMissing = 0;
    if (!QerrTestSpawnHook) { *exeMissing = 1; return 0; }
    return QerrTestSpawnHook(notifyPath);
}

static int PlatShowErrorBox(const char* header, const char* text, unsigned long* error)
{
    if (error) *error = 0;
    if (!QerrTestBoxHook) { if (error) *error = 1; return 0; }
    return QerrTestBoxHook(header, text);
}
#endif
