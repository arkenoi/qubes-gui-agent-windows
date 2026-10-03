/*
 * deathevent_test.c - offline suite for include/deathevent.h, the one Event Log record a supervisor
 * writes for a child that died unasked (docs/ADR-supervision.md section 2, main repo).
 *
 * gcc on the dev qube, no Windows: the three Win32 calls the writer makes are stubbed HERE and every
 * argument they receive is recorded, so the suite pins what the Event Log would get - the source, the
 * type, the id, the six strings and their order - not a re-implementation of it. Driven by
 * tools/tests/deathevent-selftest.sh (main repo), which also builds it with each DEATHEVENT_DEFECT_*
 * and requires the suite to FAIL on every one (a guard never seen to fail is decoration):
 *   WARNING   the record is written as a warning   (the owner: "it is not fucking warning!")
 *   SHAREDID  every death gets the agent's id       (the reporter could not tell the children apart)
 *   NOCODE    the exit code is dropped              (the one fact a crash leaves behind)
 * Prints "ok <case>" / "FAIL <case>" lines; exit 0 iff no FAIL.
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#include "deathevent.h"

/* --- stubs: what the writer hands to Win32 ----------------------------------------------- */
static int g_registerCalls, g_reportCalls, g_deregisterCalls, g_perrorCalls;
static int g_registerFails, g_reportFails;
static wchar_t g_source[64];
static const void *g_server = (void *)1;
static WORD g_type, g_category, g_numStrings;
static DWORD g_eventId, g_dataSize;
static const void *g_sid, *g_raw;
static wchar_t g_strings[DEATHEVENT_STRINGS][DEATHEVENT_TEXT_MAX];
static HANDLE g_handleOut = (HANDLE)0x5EED;
static HANDLE g_handleIn;

HANDLE RegisterEventSourceW(LPCWSTR server, LPCWSTR source)
{
    g_registerCalls++;
    g_server = server;
    wcsncpy(g_source, source ? source : L"(null)", 63);
    return g_registerFails ? NULL : g_handleOut;
}
BOOL ReportEventW(HANDLE h, WORD type, WORD category, DWORD id, PSID sid, WORD n, DWORD dataSize,
                  LPCWSTR *strings, LPVOID raw)
{
    WORD i;
    g_reportCalls++;
    g_handleIn = h; g_type = type; g_category = category; g_eventId = id; g_sid = sid;
    g_numStrings = n; g_dataSize = dataSize; g_raw = raw;
    for (i = 0; i < n && i < DEATHEVENT_STRINGS; i++)
        wcsncpy(g_strings[i], strings[i] ? strings[i] : L"(null)", DEATHEVENT_TEXT_MAX - 1);
    return g_reportFails ? FALSE : TRUE;
}
BOOL DeregisterEventSource(HANDLE h) { g_deregisterCalls++; g_handleIn = h; return TRUE; }
DWORD _win_perror(IN const char *fn, IN const WCHAR *prefix) { (void)fn; (void)prefix; g_perrorCalls++; return 5; }

static void reset(void)
{
    g_registerCalls = g_reportCalls = g_deregisterCalls = g_perrorCalls = 0;
    g_registerFails = g_reportFails = 0;
    g_source[0] = 0; g_type = 0; g_eventId = 0; g_numStrings = 0; g_handleIn = NULL;
    memset(g_strings, 0, sizeof(g_strings));
}

/* --- harness ------------------------------------------------------------------------------- */
static int g_run, g_fail;
static void check(const char *name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}
static int has(const wchar_t *hay, const wchar_t *needle) { return wcsstr(hay, needle) != NULL; }

int main(void)
{
    DEATHEVENT ev;
    WORD n;
    wchar_t longDetail[DEATHEVENT_DETAIL_MAX + 200];
    wchar_t longExe[DEATHEVENT_EXE_MAX + 50];
    size_t i;

    /* 1. the composer: ids, type, the six strings and their order */
    n = DeathEventCompose(&ev, DEATHEVENT_ID_GUI_AGENT, L"gui-agent.exe", 1234, 0xC0000409UL,
                          754000ULL, L"the watchdog relaunches it; its log is in the Qubes Tools log directory");
    check("compose: six insertion strings", n == DEATHEVENT_STRINGS);
    check("compose: the record is an ERROR, never a warning", ev.Type == EVENTLOG_ERROR_TYPE);
    check("compose: the id is the caller's", ev.EventId == DEATHEVENT_ID_GUI_AGENT);
    check("compose: %1 is the rendered text", ev.Strings[0] == ev.Text);
    check("compose: text names the exe", has(ev.Text, L"gui-agent.exe"));
    check("compose: text names the pid", has(ev.Text, L"PID 1234"));
    check("compose: text carries the exit code as 0x%08X", has(ev.Text, L"0xC0000409"));
    check("compose: text says how long it ran (h:mm:ss and ms)", has(ev.Text, L"0:12:34 (754000 ms)"));
    check("compose: text says it was not asked to exit", has(ev.Text, L"without being asked to"));
    check("compose: text carries the supervisor's detail", has(ev.Text, L"the watchdog relaunches it"));
    check("compose: %2 exe", wcscmp(ev.Strings[1], L"gui-agent.exe") == 0);
    check("compose: %3 pid decimal", wcscmp(ev.Strings[2], L"1234") == 0);
    check("compose: %4 exit code hex", wcscmp(ev.Strings[3], L"0xC0000409") == 0);
    check("compose: %5 run time in decimal ms", wcscmp(ev.Strings[4], L"754000") == 0);
    check("compose: %6 detail verbatim", wcscmp(ev.Strings[5], L"the watchdog relaunches it; its log is in the Qubes Tools log directory") == 0);

    /* 2. the four children have four distinct ids in our range */
    {
        DWORD ids[4] = { DEATHEVENT_ID_GUI_AGENT, DEATHEVENT_ID_WGCBROKER, DEATHEVENT_ID_NOTIFBRIDGE, DEATHEVENT_ID_ETWPROXY };
        int distinct = 1, inRange = 1, k, j;
        for (k = 0; k < 4; k++)
        {
            DeathEventCompose(&ev, ids[k], L"x.exe", 1, 0, 1, NULL);
            if (ev.EventId < 4001 || ev.EventId > 4004) inRange = 0;
            for (j = 0; j < k; j++)
            {
                DEATHEVENT other;
                DeathEventCompose(&other, ids[j], L"x.exe", 1, 0, 1, NULL);
                if (other.EventId == ev.EventId) distinct = 0;
            }
        }
        check("ids: the four supervised children get four distinct ids", distinct);
        check("ids: all in the 4001..4004 range the reporter subscribes to", inRange);
    }

    /* 3. unknowns are written as the word, never as a number that could be mistaken for one */
    DeathEventCompose(&ev, DEATHEVENT_ID_WGCBROKER, L"wgcbroker.exe", 77, DEATHEVENT_EXIT_UNKNOWN,
                      DEATHEVENT_RAN_UNKNOWN, L"hung; reaped by the agent before the relaunch");
    check("unknown: exit code unknown -> %4 is the word", wcscmp(ev.Strings[3], L"unknown") == 0);
    check("unknown: text says exit code unknown", has(ev.Text, L"exit code unknown"));
    check("unknown: run time unknown -> %5 is the word", wcscmp(ev.Strings[4], L"unknown") == 0);
    check("unknown: text says an unknown time", has(ev.Text, L"after running an unknown time"));
    DeathEventCompose(&ev, DEATHEVENT_ID_WGCBROKER, L"wgcbroker.exe", 77, 0, 0, NULL);
    check("zero: exit code 0 is a real code (a clean exit nobody asked for), not unknown", wcscmp(ev.Strings[3], L"0x00000000") == 0);
    check("zero: 0 ms is a real run time, not unknown", wcscmp(ev.Strings[4], L"0") == 0);
    check("null detail: %6 is empty, not (null)", ev.Strings[5][0] == 0);

    /* 4. bounded: oversize inputs are truncated, never overflow, always terminated */
    for (i = 0; i < RTL_NUMBER_OF(longDetail) - 1; i++) longDetail[i] = L'd';
    longDetail[RTL_NUMBER_OF(longDetail) - 1] = 0;
    for (i = 0; i < RTL_NUMBER_OF(longExe) - 1; i++) longExe[i] = L'e';
    longExe[RTL_NUMBER_OF(longExe) - 1] = 0;
    DeathEventCompose(&ev, DEATHEVENT_ID_ETWPROXY, longExe, 4294967295UL, 0xE06D7363UL, 123456789012ULL, longDetail);
    check("bounds: a too-long detail is cut to the field", wcslen(ev.Detail) == DEATHEVENT_DETAIL_MAX - 1);
    check("bounds: a too-long exe name is cut to the field", wcslen(ev.Exe) == DEATHEVENT_EXE_MAX - 1);
    check("bounds: the text stays inside its buffer", wcslen(ev.Text) < DEATHEVENT_TEXT_MAX);
    check("bounds: the largest pid prints", wcscmp(ev.Strings[2], L"4294967295") == 0);
    check("bounds: a multi-day run time prints in hours", has(ev.Text, L"34293:33:09"));

    /* 5. the writer: what reaches the Event Log, and the failure paths */
    reset();
    check("report: success returns TRUE",
          DeathEventReport(DEATHEVENT_ID_NOTIFBRIDGE, L"notifhost.exe", 4321, 0xC0000005UL, 60000ULL, L"relaunch in 60 s") == TRUE);
    check("report: the source is ours, on the local machine", wcscmp(g_source, DEATHEVENT_SOURCE) == 0 && g_server == NULL);
    check("report: register -> report -> deregister, once each", g_registerCalls == 1 && g_reportCalls == 1 && g_deregisterCalls == 1);
    check("report: the handle registered is the one reported on and deregistered", g_handleIn == g_handleOut);
    check("report: type ERROR", g_type == EVENTLOG_ERROR_TYPE);
    check("report: category 0, no sid, no raw data", g_category == 0 && g_sid == NULL && g_dataSize == 0 && g_raw == NULL);
    check("report: id is the bridge's", g_eventId == DEATHEVENT_ID_NOTIFBRIDGE);
    check("report: six strings", g_numStrings == DEATHEVENT_STRINGS);
    check("report: %1 renders the exe, pid and code", has(g_strings[0], L"notifhost.exe (PID 4321) exited without being asked to: exit code 0xC0000005"));
    check("report: %2..%5 structured", wcscmp(g_strings[1], L"notifhost.exe") == 0 && wcscmp(g_strings[2], L"4321") == 0 &&
          wcscmp(g_strings[3], L"0xC0000005") == 0 && wcscmp(g_strings[4], L"60000") == 0);
    check("report: no perror on success", g_perrorCalls == 0);

    reset(); g_registerFails = 1;
    check("report: RegisterEventSource failure -> FALSE, one perror, nothing reported",
          DeathEventReport(DEATHEVENT_ID_GUI_AGENT, L"gui-agent.exe", 1, 1, 1, NULL) == FALSE &&
          g_perrorCalls == 1 && g_reportCalls == 0 && g_deregisterCalls == 0);
    reset(); g_reportFails = 1;
    check("report: ReportEvent failure -> FALSE, one perror, the source is still deregistered",
          DeathEventReport(DEATHEVENT_ID_GUI_AGENT, L"gui-agent.exe", 1, 1, 1, NULL) == FALSE &&
          g_perrorCalls == 1 && g_deregisterCalls == 1);

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
