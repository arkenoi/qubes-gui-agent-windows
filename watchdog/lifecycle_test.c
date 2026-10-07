/*
 * lifecycle_test.c - offline suite for include/qga-lifecycle.h, the watchdog service's decision on an agent exit
 * (docs/ADR-supervision.md section 5, main repo) and the agent's own reading of its exit code (WinMain).
 *
 * gcc on the dev qube, no Windows: the header is pure (a table and a lookup), so the suite holds every row of the
 * table against the rules the owner set on 2026-10-07 - R1 nothing is launched into a session that announced its
 * end; R2 a death is Windows' to recover from (the service fails itself, it never relaunches); R5 a requested or
 * expected exit is not a death. Driven by tools/tests/lifecycle-selftest.sh (main repo), which also builds it with
 * each QGA_LIFECYCLE_DEFECT_* and requires the suite to FAIL on every one (a guard never seen to fail is decoration):
 *   TERMINATEDDEATH   the system's termination status is an ordinary death: relaunched into the ending session (the
 *                     measured defect: three agent instances per shutdown)
 *   RELAUNCHDEATH     the service relaunches a dead agent itself (the old loop) instead of failing for the SCM
 *   RECONNECTDEATH    a reconnect exit writes a death record
 *   REQUESTEDDEATH    a requested exit is a death
 *   REQUESTEDERROR    the agent logs an expected exit as a failure (the stale-GetLastError ERROR line)
 * Prints "ok <case>" / "FAIL <case>" lines; exit 0 iff no FAIL.
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "qga-lifecycle.h"

static int g_run, g_fail;
static void check(const char *name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}

/* every attribute of a verdict, compared at once */
static int verdictIs(QGA_EXIT_VERDICT v, QGA_EXIT_DECISION d, int relaunch, int noLaunch, int record, int fail, int error)
{
    return v.Decision == d && !!v.RelaunchNow == relaunch && !!v.NoLaunchIntoSession == noLaunch &&
           !!v.WriteDeathRecord == record && !!v.FailService == fail && !!v.IsError == error;
}

int main(void)
{
    QGA_EXIT_VERDICT v;
    size_t i;
    const DWORD crashes[] = { 0xC0000005UL, 0xC0000409UL, 0x5aaUL, 0x0UL, 0xb7UL, 1UL, 0xFFFFFFFFUL };

    /* ---- 1. the defined codes --------------------------------------------------------------------------- */
    v = QgaDecideAgentExit(QGA_EXIT_REQUESTED, FALSE);
    check("requested exit: nothing - no record, no relaunch, no service failure, INFO", verdictIs(v, QGA_DECIDE_NOTHING, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_REQUESTED, TRUE);
    check("requested exit during a session end: still nothing", verdictIs(v, QGA_DECIDE_NOTHING, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_NO_GUI_DOMAIN, FALSE);
    check("no GUI domain: the boot latch - no record, no relaunch, no failure", verdictIs(v, QGA_DECIDE_NOGUI_LATCH, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_RECONNECT, FALSE);
    check("reconnect: relaunch at once, no record, no failure, INFO (the one relaunch)", verdictIs(v, QGA_DECIDE_RECONNECT, 1, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_RECONNECT, TRUE);
    check("reconnect after an acknowledged notice: a notice wins - no launch into the ending session, no record",
          verdictIs(v, QGA_DECIDE_SESSION_END, 0, 1, 0, 0, 0));

    /* ---- 2. the session end ------------------------------------------------------------------------------ */
    v = QgaDecideAgentExit(QGA_EXIT_SESSION_END, TRUE);
    check("session end after an acknowledged notice: INFO, no launch into that session, no record", verdictIs(v, QGA_DECIDE_SESSION_END, 0, 1, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_SESSION_END, FALSE);
    check("session end with no notice seen: ERROR (the channel failed), still no launch into that session, no record",
          verdictIs(v, QGA_DECIDE_SESSION_END_UNNOTICED, 0, 1, 0, 0, 1));
    v = QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE);
    check("0x40010004 after an acknowledged notice: the orderly exit was cut short - ERROR, no launch into that session, no record, service stays",
          verdictIs(v, QGA_DECIDE_SESSION_END_FORCED, 0, 1, 0, 0, 1));
    v = QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE);
    check("0x40010004 with no notice: NOTICE MISSED - ERROR, no launch into that session, no record, service stays (the measured defect)",
          verdictIs(v, QGA_DECIDE_NOTICE_MISSED, 0, 1, 0, 0, 1));
    check("0x40010004 is never relaunched by the service and never fails the service",
          !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE).RelaunchNow && !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE).FailService &&
          !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE).RelaunchNow && !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE).FailService);

    /* ---- 3. deaths: every undefined code, crash or not ---------------------------------------------------- */
    for (i = 0; i < RTL_NUMBER_OF(crashes); i++)
    {
        char name[160];
        v = QgaDecideAgentExit(crashes[i], FALSE);
        snprintf(name, sizeof(name), "death 0x%08lx: 4001 + ERROR, the service fails itself, no relaunch by the service", (unsigned long)crashes[i]);
        check(name, verdictIs(v, QGA_DECIDE_DEATH, 0, 0, 1, 1, 1));
        v = QgaDecideAgentExit(crashes[i], TRUE);
        snprintf(name, sizeof(name), "death 0x%08lx in an ending session: 4001 + ERROR, no launch into it, the service stays", (unsigned long)crashes[i]);
        check(name, verdictIs(v, QGA_DECIDE_DEATH_IN_ENDING_SESSION, 0, 1, 1, 0, 1));
    }
    check("a clean exit 0 is a death (no path returns 0 on purpose)", QgaDecideAgentExit(0, FALSE).Decision == QGA_DECIDE_DEATH);
    check("the stale 0xb7 the old WinMain returned is a death, not a requested exit", QgaDecideAgentExit(0xb7, FALSE).Decision == QGA_DECIDE_DEATH);

    /* ---- 4. the invariants over the whole table ---------------------------------------------------------- */
    {
        int relaunchOnlyReconnect = 1, noLaunchWhenAcked = 1, recordIffDeath = 1, failIffPlainDeath = 1, everyDecisionNamed = 1;
        const DWORD codes[] = { QGA_EXIT_REQUESTED, QGA_EXIT_SESSION_END, QGA_EXIT_RECONNECT, QGA_EXIT_NO_GUI_DOMAIN,
                                QGA_EXIT_SYSTEM_TERMINATED, 0xC0000005UL, 0, 1, 0xb7UL };
        size_t c;
        int acked;
        for (c = 0; c < RTL_NUMBER_OF(codes); c++)
            for (acked = 0; acked < 2; acked++)
            {
                v = QgaDecideAgentExit(codes[c], acked);
                if (v.RelaunchNow && v.Decision != QGA_DECIDE_RECONNECT) relaunchOnlyReconnect = 0;
                if (acked && codes[c] != QGA_EXIT_REQUESTED && codes[c] != QGA_EXIT_NO_GUI_DOMAIN && !v.NoLaunchIntoSession) noLaunchWhenAcked = 0;
                if (v.WriteDeathRecord != (v.Decision == QGA_DECIDE_DEATH || v.Decision == QGA_DECIDE_DEATH_IN_ENDING_SESSION)) recordIffDeath = 0;
                if (v.FailService != (v.Decision == QGA_DECIDE_DEATH)) failIffPlainDeath = 0;
                if (!v.Name || !v.Name[0]) everyDecisionNamed = 0;
            }
        check("invariant: the service relaunches on RECONNECT and on nothing else (R2)", relaunchOnlyReconnect);
        check("invariant: after an acknowledged notice nothing is launched into that session, whatever the exit code (R1)", noLaunchWhenAcked);
        check("invariant: a 4001 record exactly for the two death decisions (R5)", recordIffDeath);
        check("invariant: the service fails itself exactly for a plain death (the SCM's recovery is the relauncher)", failIffPlainDeath);
        check("invariant: every decision has a log token", everyDecisionNamed);
    }

    /* ---- 5. the agent's own reading of its exit (WinMain): expected vs failure --------------------------- */
    check("agent: a requested exit is expected (INFO)", QgaExitIsExpected(QGA_EXIT_REQUESTED));
    check("agent: a session-end exit is expected", QgaExitIsExpected(QGA_EXIT_SESSION_END));
    check("agent: a reconnect exit is expected", QgaExitIsExpected(QGA_EXIT_RECONNECT));
    check("agent: no GUI domain is expected", QgaExitIsExpected(QGA_EXIT_NO_GUI_DOMAIN));
    check("agent: a Win32 error is a failure (ERROR with the code)", !QgaExitIsExpected(0xb7) && !QgaExitIsExpected(ERROR_GEN_FAILURE));
    check("agent: 0 is a failure, never a clean exit", !QgaExitIsExpected(0));
    check("agent: every expected exit has a reason name that is not the failure text",
          wcsstr(QgaExitReasonName(QGA_EXIT_REQUESTED), L"requested") != NULL &&
          wcsstr(QgaExitReasonName(QGA_EXIT_SESSION_END), L"session") != NULL &&
          wcsstr(QgaExitReasonName(QGA_EXIT_RECONNECT), L"reconnect") != NULL &&
          wcsstr(QgaExitReasonName(0xb7), L"failure") != NULL);

    /* ---- 6. the channel names -------------------------------------------------------------------------- */
    {
        WCHAR a[96], b[96];
        check("channel: names are per pid and per object",
              SUCCEEDED(QgaLifecycleObjectName(a, 96, 1234, QGA_LIFECYCLE_NOTICE)) &&
              SUCCEEDED(QgaLifecycleObjectName(b, 96, 1235, QGA_LIFECYCLE_NOTICE)) && wcscmp(a, b) != 0 &&
              wcscmp(a, L"Global\\QGA_LIFECYCLE_1234_notice") == 0);
        check("channel: the DACL is SYSTEM-only", wcscmp(QGA_LIFECYCLE_SDDL, L"D:P(A;;GA;;;SY)") == 0);
        check("budgets: the ack wait is inside Windows' 5 s hung-app window, the exit wait inside its 20 s",
              QGA_ENDSESSION_ACK_WAIT_MS < 5000 && QGA_ENDSESSION_EXIT_WAIT_MS < 20000);
    }

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
