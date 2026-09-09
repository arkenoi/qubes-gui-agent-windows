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
 * FAIL-OPEN CONTRACT: this function never blocks (no waits, no round trips), never raises, and
 * returns to the caller with its state untouched whatever happened. Its own failures are
 * logged ONCE per process per kind (no exe, spawn failed, store unwritable) - the
 * NOTIFYERR_DEFECT_FAILOPEN switch turns that into every-call logging so the offline test can
 * watch the guard fail.
 *
 * PORTABLE ON PURPOSE: the platform layer at the bottom has a Win32 body (the agent) and a plain-C
 * TEST body (notifyerr_test.c: gcc on any host, or MSVC with NOTIFYERR_TEST_LAYER defined by the
 * notifyerr-test vcxproj - stdio only, the spawn and the log routed through test hooks), so the
 * offline suite exercises THIS file's control flow - the fail-open path, the marker and count
 * writes, the once-per-process logging - not a re-implementation of it.
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
static long long PlatBootStamp(void);
static int       PlatEnsureDir(const char* dir);
static int       PlatReadSmall(const char* path, char* buf, size_t cap);   /* 1 = read, 0 = absent/failed */
static int       PlatWriteSmall(const char* path, const char* text);      /* 1 = written */
static int       PlatWriteNotifyFile(const char* path, const char* utf8); /* UTF-16LE + BOM */
static int       PlatSpawnNotify(const char* notifyPath, int* exeMissing);
static void      PlatDefaultStateDir(char* out, size_t cap);

#ifndef QERR_AGENT_LAYER
/* Test hooks (notifyerr_test.c): the spawn outcome and the log sink are the test's. */
int  (*QerrTestSpawnHook)(const char* notifyPath) = NULL;   /* NULL = "notifhost.exe missing" */
void (*QerrTestLogHook)(const char* line) = NULL;
long long QerrTestBootStamp = 0;                              /* 0 = wall clock (the suite always pins it) */
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
    PlatLog("NOTIFYERR gate: enabled=%d state=%s (secondary route: dom0 notification via "
            "notifhost/qubes.Notifications; needs qrexec-agent; the log stays primary)",
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

QerrDecision QerrReport(const char* component, const char* id, int sev, const char* summary,
                        const char* logHint)
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

    if (!g_QerrGate) return QERR_REJECT_SEVERITY;   /* gate off: nothing leaves the log */

    /* Compose first: the text is what redaction judges, and a text that does not fit is not
     * sent at all (QerrComposeNotifyText returns 0). */
    if (!QerrComposeNotifyText(text, sizeof(text), component ? component : "", id ? id : "",
                               summary ? summary : "", logHint))
        return QERR_REJECT_REDACT;

    /* Cheap, store-free checks come first inside QerrDecide; only a candidate for sending
     * reads the store. Read both files up front anyway - they are tiny and this keeps the
     * decision a single pure call the test can pin. */
    now = PlatBootStamp();
    snprintf(markerPath, sizeof(markerPath), "%s/%s.%s", g_QerrStateDir,
             component ? component : "-", id ? id : "-");
    snprintf(countPath, sizeof(countPath), "%s/.count", g_QerrStateDir);
    markerPresent = PlatReadSmall(markerPath, fileBuf, sizeof(fileBuf)) && QerrParseMarker(fileBuf, &markerBoot);
    countPresent  = PlatReadSmall(countPath, fileBuf, sizeof(fileBuf)) && QerrParseCount(fileBuf, &countBoot, &count);

    d = QerrDecide(sev, component, id, text, markerPresent, markerBoot, countPresent, countBoot,
                   count, now, &newCount);
    if (d != QERR_SEND) {
        /* Rejections are not failures of the route; they are the policy working. One log line
         * each at debug cost - severity rejections are the common case and stay quiet. */
        if (d != QERR_REJECT_SEVERITY)
            PlatLog("NOTIFYERR %s.%s not sent: %s", component ? component : "-", id ? id : "-",
                    QerrDecisionName(d));
        return d;
    }

    /* MISSING DATA FAILS: no persisted marker -> no send. Without persistence the dedupe is
     * per-process, and a relaunched helper would notify once per launch. */
    if (!PlatEnsureDir(g_QerrStateDir) ||
        !QerrFormatMarker(fileBuf, sizeof(fileBuf), now) || !PlatWriteSmall(markerPath, fileBuf) ||
        !QerrFormatCount(fileBuf, sizeof(fileBuf), now, newCount) || !PlatWriteSmall(countPath, fileBuf))
    {
        LogOnce(&s_LoggedStore, "NOTIFYERR state dir %s is not writable - the route is OFF for "
                "this process (a notification with no once-per-boot record would repeat)",
                g_QerrStateDir);
        return QERR_FAIL_TRANSPORT;
    }

    snprintf(notifyPath, sizeof(notifyPath), "%s/out-%s-%u.txt", g_QerrStateDir,
             component ? component : "-", ++s_Seq);
    if (!PlatWriteNotifyFile(notifyPath, text)) {
        LogOnce(&s_LoggedStore, "NOTIFYERR cannot write %s - notification not sent", notifyPath);
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
        return QERR_FAIL_TRANSPORT;
    }
    PlatLog("NOTIFYERR %s.%s sent to dom0 (#%u this boot; delivery is notifhost's to log)",
            component, id, newCount);
    return QERR_SEND;
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

static long long PlatBootStamp(void)
{
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    /* 100 ns since 1601 -> seconds since 1970, minus uptime. Tolerance in QerrBootMatch. */
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL - (long long)(GetTickCount64() / 1000ULL);
}

static void PlatDefaultStateDir(char* out, size_t cap)
{
    char pd[MAX_PATH] = { 0 };
    if (!GetEnvironmentVariableA("ProgramData", pd, RTL_NUMBER_OF(pd)) || !pd[0])
        StringCchCopyA(pd, RTL_NUMBER_OF(pd), "C:\\ProgramData");
    StringCchPrintfA(out, cap, "%s\\Qubes\\notify-errors", pd);
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

static long long PlatBootStamp(void)
{
    /* The suite pins the stamp; without a pin there is no uptime source in plain C, and the
     * wall clock is a stand-in that only ever matches itself within one run. */
    if (QerrTestBootStamp) return QerrTestBootStamp;
    return (long long)time(NULL);
}

static void PlatDefaultStateDir(char* out, size_t cap)
{
    snprintf(out, cap, "%s", "qwt-notify-errors-test");
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
#endif
