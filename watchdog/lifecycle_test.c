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
 *   HELPERSYSKILLDEATH  a helper the system killed (0x40010004) is written up as a death
 *   HELPERSTOPTEARDOWN  a helper's stop wait expiring during the session's teardown is graded an ERROR miss (the
 *                       measured 2026-10-10 defect: "did not leave within 3 s of the stop file" at every shutdown)
 *   PRESHELLLOCK        a LOCK reading is asserted with no shell seen or found (the measured 2026-10-10 defect:
 *                       "the session is LOCKED after 0 s" on an agent's first secure frame, dom0 notified)
 *   SHELLPROCSILENT     an UNREADABLE process list is read as "no shell" on the LOCK-without-window path (the
 *                       silent-skip shape the review named: a real lock would then be PRE_SHELL, unsaid)
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
    v = QgaDecideAgentExit(QGA_EXIT_REQUESTED, FALSE, FALSE);
    check("requested exit: nothing - no record, no relaunch, no service failure, INFO", verdictIs(v, QGA_DECIDE_NOTHING, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_REQUESTED, TRUE, FALSE);
    check("requested exit during a session end: still nothing", verdictIs(v, QGA_DECIDE_NOTHING, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_NO_GUI_DOMAIN, FALSE, FALSE);
    check("no GUI domain: the boot latch - no record, no relaunch, no failure", verdictIs(v, QGA_DECIDE_NOGUI_LATCH, 0, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_RECONNECT, FALSE, FALSE);
    check("reconnect: relaunch at once, no record, no failure, INFO (the one relaunch)", verdictIs(v, QGA_DECIDE_RECONNECT, 1, 0, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_RECONNECT, TRUE, FALSE);
    check("reconnect after an acknowledged notice: a notice wins - no launch into the ending session, no record",
          verdictIs(v, QGA_DECIDE_SESSION_END, 0, 1, 0, 0, 0));

    /* ---- 2. the session end ------------------------------------------------------------------------------ */
    v = QgaDecideAgentExit(QGA_EXIT_SESSION_END, TRUE, FALSE);
    check("session end after an acknowledged notice: INFO, no launch into that session, no record", verdictIs(v, QGA_DECIDE_SESSION_END, 0, 1, 0, 0, 0));
    v = QgaDecideAgentExit(QGA_EXIT_SESSION_END, FALSE, FALSE);
    check("session end with no notice seen: ERROR (the channel failed), still no launch into that session, no record",
          verdictIs(v, QGA_DECIDE_SESSION_END_UNNOTICED, 0, 1, 0, 0, 1));
    /* THE SYSTEM'S KILL, SPLIT ON WHETHER THE ORDERLY EXIT HAD FINISHED. Both cases arrive as 0x40010004 and
       before 2026-10-07 both were SESSIONEND-FORCED, so every clean shutdown wrote an ERROR line about an
       exit that had in fact completed (measured: the agent logged "orderly exit complete" at 153432.110 and
       the watchdog the forced end at 153432.214). The agent now signals completion on the channel, so the
       one that really is an error still is one and the benign reap is not. */
    v = QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, FALSE);
    check("0x40010004 after an acknowledged notice, orderly exit NOT signalled: cut short - ERROR, no launch into that session, no record, service stays",
          verdictIs(v, QGA_DECIDE_SESSION_END_FORCED, 0, 1, 0, 0, 1));
    v = QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, TRUE);
    check("0x40010004 after an acknowledged notice AND a signalled orderly exit: reaped after finishing - INFO, no launch, no record",
          verdictIs(v, QGA_DECIDE_SESSION_END_REAPED, 0, 1, 0, 0, 0));
    check("the only thing that separates those two is the agent's own completion signal",
          QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, TRUE).IsError == FALSE &&
          QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, FALSE).IsError == TRUE);
    check("a completion signal NEVER excuses a missing notice - that is still NOTICEMISSED at ERROR",
          QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE, TRUE).Decision == QGA_DECIDE_NOTICE_MISSED &&
          QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE, TRUE).IsError == TRUE);
    check("nor does it excuse a death: an undefined code with the signal set is still a death",
          QgaDecideAgentExit(0xC0000005UL, FALSE, TRUE).Decision == QGA_DECIDE_DEATH &&
          QgaDecideAgentExit(0xC0000005UL, FALSE, TRUE).WriteDeathRecord == TRUE);
    v = QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE, FALSE);
    check("0x40010004 with no notice: NOTICE MISSED - ERROR, no launch into that session, no record, service stays (the measured defect)",
          verdictIs(v, QGA_DECIDE_NOTICE_MISSED, 0, 1, 0, 0, 1));
    check("0x40010004 is never relaunched by the service and never fails the service",
          !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE, FALSE).RelaunchNow && !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, FALSE, FALSE).FailService &&
          !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, FALSE).RelaunchNow && !QgaDecideAgentExit(QGA_EXIT_SYSTEM_TERMINATED, TRUE, FALSE).FailService);

    /* ---- 2b. THE HELPER EXITS (QgaDecideHelperExit) ------------------------------------------------------
       The agent supervises the broker, the notification bridge and the ETW proxy, and until 2026-10-10 all
       three asked only "are helper launches disarmed?" - true only once Windows has told the AGENT its
       session is ending. MEASURED on win10-acc that day: two Application 4003 records for notifhost.exe,
       killed 27 s after boot by an ordinary shutdown, each escalated into a dom0 "exited unexpectedly ...
       this is a major error" notification. The exit code settles it without the race. */
    /* CORRECTED 2026-10-10. This row used to end "whatever the code" and asserted that a disarmed
       access violation is expected - which is the hole the owner closed: "yet, a REAL abnormal
       termination should be always reported loudly". The disarm now excuses only the two ends that ARE
       the shutdown; the crash clause moved to the rows below, where it expects a DEATH. */
    check("helper, launches disarmed: expected, no record - for the shutdown's own ends",
          QgaDecideHelperExit(TRUE, TRUE, 0) == QGA_HELPER_EXPECTED_DISARMED &&
          QgaDecideHelperExit(TRUE, TRUE, QGA_EXIT_SYSTEM_TERMINATED) == QGA_HELPER_EXPECTED_DISARMED &&
          QgaDecideHelperExit(TRUE, FALSE, QGA_EXIT_SYSTEM_TERMINATED) == QGA_HELPER_EXPECTED_DISARMED);
    check("helper killed by the system BEFORE the agent was told: expected, no record - the measured 4003 case",
          QgaDecideHelperExit(FALSE, TRUE, QGA_EXIT_SYSTEM_TERMINATED) == QGA_HELPER_EXPECTED_SYSKILL);

    // A REAL ABNORMAL TERMINATION IS ALWAYS LOUD (owner 2026-10-10), so the disarm excuses only the two
    // ends that ARE the shutdown. Before this, `if (disarmed) return EXPECTED` read no exit code at all
    // and a crash mid-shutdown was silent (Jev: hides_a_real_kill 0.70 then, crash_must_be_loud 0.95 now).
    check("disarmed + an access violation: a DEATH - the disarm never excuses a crash",
          QgaDecideHelperExit(TRUE, TRUE, 0xC0000005UL) == QGA_HELPER_DEATH);
    check("disarmed + a fast-fail abort: a DEATH",
          QgaDecideHelperExit(TRUE, TRUE, 0xC0000409UL) == QGA_HELPER_DEATH);
    check("disarmed + heap corruption, a stack overflow, an illegal instruction: all DEATHS",
          QgaDecideHelperExit(TRUE, TRUE, 0xC0000374UL) == QGA_HELPER_DEATH &&
          QgaDecideHelperExit(TRUE, TRUE, 0xC00000FDUL) == QGA_HELPER_DEATH &&
          QgaDecideHelperExit(TRUE, TRUE, 0xC000001DUL) == QGA_HELPER_DEATH);
    check("disarmed + an unhandled C++ or .NET exception: a DEATH (customer-flagged, same severity test)",
          QgaDecideHelperExit(TRUE, TRUE, 0xE06D7363UL) == QGA_HELPER_DEATH &&
          QgaDecideHelperExit(TRUE, TRUE, 0xE0434352UL) == QGA_HELPER_DEATH);
    check("disarmed + its own failure exit (2): a DEATH, not excused by the shutdown",
          QgaDecideHelperExit(TRUE, TRUE, 2UL) == QGA_HELPER_DEATH);
    check("disarmed + Windows' teardown code: still EXPECTED - the shutdown case stays silent",
          QgaDecideHelperExit(TRUE, TRUE, QGA_EXIT_SYSTEM_TERMINATED) == QGA_HELPER_EXPECTED_DISARMED);
    check("disarmed + a clean exit 0: EXPECTED - a helper leaving tidily as we go down",
          QgaDecideHelperExit(TRUE, TRUE, 0UL) == QGA_HELPER_EXPECTED_DISARMED);
    check("disarmed + it never exited (a hang, no code to read): EXPECTED here - the stop-wait decision owns that",
          QgaDecideHelperExit(TRUE, FALSE, 0UL) == QGA_HELPER_EXPECTED_DISARMED);
    check("the severity test is the predicate: 0x40010004 is severity 1 and not a crash, 0xC0000005 is",
          !QgaExitIsCrash(QGA_EXIT_SYSTEM_TERMINATED) && QgaExitIsCrash(0xC0000005UL) &&
          !QgaExitIsCrash(0UL) && !QgaExitIsCrash(2UL));
    check("that arm is SEPARATE from the disarmed one, so it can be logged louder and found in a sweep",
          QgaDecideHelperExit(FALSE, TRUE, QGA_EXIT_SYSTEM_TERMINATED) !=
          QgaDecideHelperExit(TRUE, TRUE, QGA_EXIT_SYSTEM_TERMINATED));
    check("a HANG is never excused: no exit code exists to read, so it stays a death",
          QgaDecideHelperExit(FALSE, FALSE, QGA_EXIT_SYSTEM_TERMINATED) == QGA_HELPER_DEATH);
    check("a crash is a death", QgaDecideHelperExit(FALSE, TRUE, 0xC0000005UL) == QGA_HELPER_DEATH);
    check("a clean exit nobody asked for is STILL a death (exit 0 is not an excuse)",
          QgaDecideHelperExit(FALSE, TRUE, 0) == QGA_HELPER_DEATH);
    check("no other session-end code excuses a helper: only the system's own kill does",
          QgaDecideHelperExit(FALSE, TRUE, QGA_EXIT_SESSION_END) == QGA_HELPER_DEATH &&
          QgaDecideHelperExit(FALSE, TRUE, QGA_EXIT_RECONNECT) == QGA_HELPER_DEATH);

    /* ---- 2c. THE HELPER'S BOUNDED STOP WAIT (QgaHelperStopOutcome) --------------------------------------
       On its way out the agent asks each resident helper to leave and waits a bounded time on its handle;
       an expiry was an ERROR unconditionally. MEASURED 2026-10-10 (instance 3936, 4.3.36.915): QGAENDSESSION
       at 10:49:13.652, the bridge's stop file written at :14.141, "did not leave within 3 s" at :17.166, and
       bridge.log stops at :13 with no stop-file line - the file went to a process Windows was already tearing
       down. Nothing was wrong. */
    check("stop wait satisfied: the helper left - nothing to say, teardown or not",
          QgaHelperStopOutcome(FALSE, WAIT_OBJECT_0) == QGA_HELPER_STOP_LEFT &&
          QgaHelperStopOutcome(TRUE, WAIT_OBJECT_0) == QGA_HELPER_STOP_LEFT);
    check("stop wait EXPIRED while the session is ending: a teardown, INFO - the measured 3 s case",
          QgaHelperStopOutcome(TRUE, WAIT_TIMEOUT) == QGA_HELPER_STOP_TEARDOWN);
    check("stop wait expired with no teardown: the helper ignored its stop - ERROR, exactly as before",
          QgaHelperStopOutcome(FALSE, WAIT_TIMEOUT) == QGA_HELPER_STOP_EXPIRED);
    check("a FAILED wait is never excused by a teardown: a bad handle is a defect of ours",
          QgaHelperStopOutcome(TRUE, WAIT_FAILED) == QGA_HELPER_STOP_EXPIRED &&
          QgaHelperStopOutcome(FALSE, WAIT_FAILED) == QGA_HELPER_STOP_EXPIRED);
    check("the teardown arm is SEPARATE from the satisfied one: it is still said (INFO), not swallowed",
          QgaHelperStopOutcome(TRUE, WAIT_TIMEOUT) != QGA_HELPER_STOP_LEFT);

    /* ---- 2d. THE LOCK VERDICT (QgaLockVerdict) -----------------------------------------------------------
       MEASURED 2026-10-10 (gui-agent-20261010.log:569-570 and :827-828, 4.3.36.915): two agent instances each
       reported "the session is LOCKED after 0 s" ~2 s after their own start, on the first secure frame they
       ever saw, and dom0 was notified each time. Owner: "it was NEVER a locked guest. it was secure-desktop
       detected on display reattach before main desktop owns it." No lock is asserted until this instance has
       seen a shell; a real lock still reports at once (the once per boot is notifyerr's, not tested here). */
    check("LOCK read after a shell WINDOW was seen, no teardown: LOCKED - the process is not even consulted",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_UNREAD, FALSE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_LOCKED &&
          QgaLockVerdict(TRUE, QGA_SHELLPROC_ABSENT, FALSE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_LOCKED);
    /* THE REVIEW'S ONE OBJECTION (latch-never-set, 0.75): GetShellWindow() is per desktop, so a fresh agent on
       the Winlogon desktop never sees the window even while explorer is alive on Default - the window's absence
       must not be read as the session's absence, or a guest that really is locked when the agent starts would
       never be reported. The shell PROCESS in the console session is the desktop-independent fact. */
    check("LOCK read, no shell window seen, a shell PROCESS in the session: LOCKED - a real lock met by a fresh agent, reported",
          QgaLockVerdict(FALSE, QGA_SHELLPROC_PRESENT, FALSE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_LOCKED);
    check("LOCK read, no shell window seen, NO shell process: PRE_SHELL, nothing reported - the measured false report",
          QgaLockVerdict(FALSE, QGA_SHELLPROC_ABSENT, FALSE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_PRE_SHELL);
    check("LOCK read, no shell window seen, the process list UNREADABLE: NO_FACT - never PRE_SHELL-silent, never LOCKED",
          QgaLockVerdict(FALSE, QGA_SHELLPROC_UNREAD, FALSE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_NO_FACT);
    check("a session that is ending is a TEARDOWN whatever the flags say, shell seen or found or not",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_UNREAD, TRUE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_TEARDOWN &&
          QgaLockVerdict(FALSE, QGA_SHELLPROC_PRESENT, TRUE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_TEARDOWN &&
          QgaLockVerdict(FALSE, QGA_SHELLPROC_ABSENT, TRUE, 1, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_TEARDOWN &&
          QgaLockVerdict(TRUE, QGA_SHELLPROC_UNREAD, TRUE, 0, QGA_WTS_SESSIONSTATE_UNKNOWN) == QGA_LOCK_TEARDOWN);
    check("a failed query (no level read) is NO_FACT, never a lock - and a shell process does not rescue it",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_UNREAD, FALSE, 0, QGA_WTS_SESSIONSTATE_UNKNOWN) == QGA_LOCK_NO_FACT &&
          QgaLockVerdict(FALSE, QGA_SHELLPROC_PRESENT, FALSE, 0, QGA_WTS_SESSIONSTATE_UNKNOWN) == QGA_LOCK_NO_FACT);
    check("a level the union does not document is NO_FACT even when its flags word reads LOCK",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_PRESENT, FALSE, 2, QGA_WTS_SESSIONSTATE_LOCK) == QGA_LOCK_NO_FACT);
    check("WTS_SESSIONSTATE_LOCK is 0, so a ZEROED buffer reads as a lock unless the level is checked: level 0 + flags 0 -> NO_FACT",
          QGA_WTS_SESSIONSTATE_LOCK == 0 && QgaLockVerdict(TRUE, QGA_SHELLPROC_PRESENT, FALSE, 0, 0) == QGA_LOCK_NO_FACT);
    check("an UNLOCK reading is a fact and not a lock, whatever the shell facts say",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_UNREAD, FALSE, 1, QGA_WTS_SESSIONSTATE_UNLOCK) == QGA_LOCK_UNLOCKED &&
          QgaLockVerdict(FALSE, QGA_SHELLPROC_ABSENT, FALSE, 1, QGA_WTS_SESSIONSTATE_UNLOCK) == QGA_LOCK_UNLOCKED &&
          QgaLockVerdict(FALSE, QGA_SHELLPROC_PRESENT, FALSE, 1, QGA_WTS_SESSIONSTATE_UNLOCK) == QGA_LOCK_UNLOCKED);
    check("a flags value that is neither LOCK nor UNLOCK is NO_FACT",
          QgaLockVerdict(TRUE, QGA_SHELLPROC_PRESENT, FALSE, 1, QGA_WTS_SESSIONSTATE_UNKNOWN) == QGA_LOCK_NO_FACT &&
          QgaLockVerdict(TRUE, QGA_SHELLPROC_PRESENT, FALSE, 1, 7) == QGA_LOCK_NO_FACT);
    {
        /* the invariants over the whole space: LOCKED needs no teardown, level 1, flags LOCK, and a shell either
           SEEN or FOUND; PRE_SHELL needs the same reading with the shell neither seen nor found; an UNREAD
           process on the LOCK-without-window path is NO_FACT and nothing else */
        const DWORD flags[] = { QGA_WTS_SESSIONSTATE_LOCK, QGA_WTS_SESSIONSTATE_UNLOCK, QGA_WTS_SESSIONSTATE_UNKNOWN, 2 };
        const QGA_SHELL_PROCESS procs[] = { QGA_SHELLPROC_UNREAD, QGA_SHELLPROC_ABSENT, QGA_SHELLPROC_PRESENT };
        int lockedOnlyOneWay = 1, preShellOnlyOneWay = 1, unreadIsNoFact = 1, seen, ending;
        DWORD lvl;
        size_t fi, pi;
        for (seen = 0; seen < 2; seen++)
            for (ending = 0; ending < 2; ending++)
                for (lvl = 0; lvl < 3; lvl++)
                    for (fi = 0; fi < RTL_NUMBER_OF(flags); fi++)
                        for (pi = 0; pi < RTL_NUMBER_OF(procs); pi++)
                        {
                            const QGA_LOCK_VERDICT lv = QgaLockVerdict(seen, procs[pi], ending, lvl, flags[fi]);
                            const int lockRead = !ending && lvl == 1 && flags[fi] == QGA_WTS_SESSIONSTATE_LOCK;
                            const int shouldLock = lockRead && (seen || procs[pi] == QGA_SHELLPROC_PRESENT);
                            const int shouldPre = lockRead && !seen && procs[pi] == QGA_SHELLPROC_ABSENT;
                            if ((lv == QGA_LOCK_LOCKED) != shouldLock) lockedOnlyOneWay = 0;
                            if ((lv == QGA_LOCK_PRE_SHELL) != shouldPre) preShellOnlyOneWay = 0;
                            if (lockRead && !seen && procs[pi] == QGA_SHELLPROC_UNREAD && lv != QGA_LOCK_NO_FACT) unreadIsNoFact = 0;
                        }
        check("invariant: LOCKED exactly when not ending AND level 1 AND flags LOCK AND (shell seen OR shell process present), over the whole space", lockedOnlyOneWay);
        check("invariant: PRE_SHELL exactly when that reading has the shell neither seen nor found, over the whole space", preShellOnlyOneWay);
        check("invariant: an UNREADABLE process list on the LOCK-without-window path is NO_FACT, never silent, never a lock", unreadIsNoFact);
    }

    /* ---- 3. deaths: every undefined code, crash or not ---------------------------------------------------- */
    for (i = 0; i < RTL_NUMBER_OF(crashes); i++)
    {
        char name[160];
        v = QgaDecideAgentExit(crashes[i], FALSE, FALSE);
        snprintf(name, sizeof(name), "death 0x%08lx: 4001 + ERROR, the service fails itself, no relaunch by the service", (unsigned long)crashes[i]);
        check(name, verdictIs(v, QGA_DECIDE_DEATH, 0, 0, 1, 1, 1));
        v = QgaDecideAgentExit(crashes[i], TRUE, FALSE);
        snprintf(name, sizeof(name), "death 0x%08lx in an ending session: 4001 + ERROR, no launch into it, the service stays", (unsigned long)crashes[i]);
        check(name, verdictIs(v, QGA_DECIDE_DEATH_IN_ENDING_SESSION, 0, 1, 1, 0, 1));
    }
    check("a clean exit 0 is a death (no path returns 0 on purpose)", QgaDecideAgentExit(0, FALSE, FALSE).Decision == QGA_DECIDE_DEATH);
    check("the stale 0xb7 the old WinMain returned is a death, not a requested exit", QgaDecideAgentExit(0xb7, FALSE, FALSE).Decision == QGA_DECIDE_DEATH);

    /* ---- 4. the invariants over the whole table ---------------------------------------------------------- */
    {
        int relaunchOnlyReconnect = 1, noLaunchWhenAcked = 1, recordIffDeath = 1, failIffPlainDeath = 1, everyDecisionNamed = 1;
        const DWORD codes[] = { QGA_EXIT_REQUESTED, QGA_EXIT_SESSION_END, QGA_EXIT_RECONNECT, QGA_EXIT_NO_GUI_DOMAIN,
                                QGA_EXIT_SYSTEM_TERMINATED, 0xC0000005UL, 0, 1, 0xb7UL };
        size_t c;
        int acked, done;
        /* the invariants hold over the WHOLE space, including the completion signal added 2026-10-07 - a
           signal must not buy a relaunch, a missing record, or a quiet death anywhere in the table */
        for (c = 0; c < RTL_NUMBER_OF(codes); c++)
            for (acked = 0; acked < 2; acked++)
            for (done = 0; done < 2; done++)
            {
                v = QgaDecideAgentExit(codes[c], acked, done);
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

    /* ---- QGADESLICEDOWN: a zeroed clock is NOT ARMED (the 2026-10-11 false-toast defect) ---- */
    /* The exact state measured on win11-app: the hold zeroed both values, the agent had been up 46 s,
     * and the old test `now >= nextWarn` fired because zero is in the past. */
    /* ---- NOTIFBRIDGESYSKILL: the verdict waits for the fact that decides it (2026-10-19) ---- */
    /* MEASURED win10-clean, agent pid 3944: Windows killed the bridge at 233257.268 and told the
     * agent the session was ending at 233257.319 - 51 ms later - so "were we told?" was false at the
     * exit and a durable Event Log record was written for an ordinary teardown. */
    check("syskill: nothing pending is no verdict",
          QgaSysKillVerdict(0, 0, 999999, 2000) == QGA_SYSKILL_PENDING);
    check("syskill: inside the grace with no session end yet is PENDING - the 51 ms case says nothing",
          QgaSysKillVerdict(0, 233257268ULL, 233257319ULL, 2000) == QGA_SYSKILL_PENDING);
    check("syskill: the session end arriving makes it a TEARDOWN, not a death",
          QgaSysKillVerdict(1, 233257268ULL, 233257319ULL, 2000) == QGA_SYSKILL_TEARDOWN);
    check("syskill: a session end at the LAST moment of the grace still wins",
          QgaSysKillVerdict(1, 1000, 1000 + 1999, 2000) == QGA_SYSKILL_TEARDOWN);
    check("syskill: the grace expiring with no session end is the ANOMALY the warning is for",
          QgaSysKillVerdict(0, 1000, 1000 + 2000, 2000) == QGA_SYSKILL_ANOMALY);
    check("syskill: and it stays the anomaly afterwards",
          QgaSysKillVerdict(0, 1000, 60000, 2000) == QGA_SYSKILL_ANOMALY);

    check("deslice: held clock (both zero) is NOT due, however long the agent has been up",
          QgaDesliceWarnDue(0, 0, 46250) == FALSE);
    check("deslice: a down-clock with no warn tick is NOT due",
          QgaDesliceWarnDue(1000, 0, 999999) == FALSE);
    check("deslice: a warn tick with no down-clock is NOT due",
          QgaDesliceWarnDue(0, 30000, 999999) == FALSE);
    check("deslice: armed and BEFORE the deadline is not due",
          QgaDesliceWarnDue(1000, 31000, 30999) == FALSE);
    check("deslice: armed and AT the deadline is due - a real outage is still reported",
          QgaDesliceWarnDue(1000, 31000, 31000) == TRUE);
    check("deslice: armed and past the deadline is due",
          QgaDesliceWarnDue(1000, 31000, 120000) == TRUE);

    /* THE ANCHOR. Measured 2026-10-23: a broker ready at 20:57:39 and killed at 21:00:52 was
       reported as "not running for 210 s" and the complaint fired 29 ms after the detection,
       because the clock re-armed at the launch tick (20:57:25) whose +30 s deadline had long
       passed. Ticks below are ms, in the shape of that capture. */
    check("deslice anchor: launched and NEVER ready still anchors at the launch (true outage)",
          QgaDesliceAnchor(1, 100000, 0, 160000) == 100000);
    check("deslice anchor: once it HAS been ready, a later outage anchors at NOW, not the launch",
          QgaDesliceAnchor(1, 100000, 114000, 307000) == 307000);
    check("deslice anchor: and that is what restores the 30 s grace a relaunch can use",
          QgaDesliceWarnDue(QgaDesliceAnchor(1, 100000, 114000, 307000),
                            QgaDesliceAnchor(1, 100000, 114000, 307000) + 30000, 307000) == FALSE);
    check("deslice anchor: the stale anchor was due IMMEDIATELY - the defect, stated as a test",
          QgaDesliceWarnDue(100000, 100000 + 30000, 307000) == TRUE);
    check("deslice anchor: no launch yet anchors at now (nothing could have been running)",
          QgaDesliceAnchor(0, 0, 0, 55000) == 55000);

    /* THE STARVATION DETECTOR, WHOSE ZERO FIRINGS WERE ABOUT TO BE USED AS EVIDENCE.
       QGAHOLDSTARVED has been emitted 0 times in every capture on this rig, and BROKERHOLD holds
       occur 117 times (all why=slot-ack-pending), so "the hold is a benign transient" rested entirely
       on this predicate having never said otherwise - while the predicate had no test at all. Jev
       scored that 0.49: a never-fired detector cannot be told apart from one that cannot fire. The
       bound is QGA_HOLD_STARVE_MS = 10000 ms; ticks below are ms. */
    check("starve: holding, content older than the bound - THE FIRING CASE",
          QgaHoldStarveDue(1000, 1000, 1000 + 10000, 10000) == TRUE);
    check("starve: one ms inside the bound is NOT due",
          QgaHoldStarveDue(1000, 1000, 1000 + 9999, 10000) == FALSE);
    check("starve: not holding at all declares nothing",
          QgaHoldStarveDue(0, 5000, 100000, 10000) == FALSE);
    check("starve: a window that never had painted content cannot show STALE pixels (QGADIRECTSUPPRESS owns it)",
          QgaHoldStarveDue(1000, 0, 1000 + 60000, 10000) == FALSE);
    check("starve: content arriving AFTER the hold began restarts the clock from the content",
          QgaHoldStarveDue(1000, 9000, 9000 + 9999, 10000) == FALSE &&
          QgaHoldStarveDue(1000, 9000, 9000 + 10000, 10000) == TRUE);
    check("starve: a clock that went backwards is not due (now <= from)",
          QgaHoldStarveDue(50000, 50000, 40000, 10000) == FALSE);

    /* A GRACEFUL STAND-DOWN IS ULTIMATE until something starts it. Owner, 2026-10-24: "of course
       graceful shutdown should be ultimate until something starts it" and "when the broker is told
       to go down, it stays down, no timer, no supervisor until next rearm". The defect it replaces:
       BrokerSupervise recognised the deliberate stand-down and returned from THAT pass only, so the
       next wake found g_BrokerReady already 0, skipped the was-ready branch, armed the clock and
       complained about a broker nobody wanted running. The latch's EVENTS live in main.c (set at the
       non-death exit, cleared when the broker is seen alive or a launch is issued); what is pure, and
       what is asserted here, is that NOTHING ABOUT TIME can end it. */
    check("stand-down: held while nothing has started it",
          QgaDesliceSupervisionHeld(1, 0, 0) == 1);
    check("stand-down: STILL held an hour later - time cannot end it, which is the whole decision",
          QgaDesliceSupervisionHeld(1, 1000, 1000 + 3600000ULL) == 1);
    check("stand-down: still held long past the first warn interval (the pre-latch lapse point)",
          QgaDesliceSupervisionHeld(1, 1000, 1000 + 30001ULL) == 1);
    check("stand-down: a broker that was never stood down is supervised normally",
          QgaDesliceSupervisionHeld(0, 0, 500000) == 0);
    check("stand-down: and not held for a never-stood-down broker however long it has been down",
          QgaDesliceSupervisionHeld(0, 1000, 1000 + 3600000ULL) == 0);
    /* AND IT MUST NOT SILENCE A REAL OUTAGE: the only thing the latch gates is the condition the
       agent itself asked for. A death keeps its own path - QgaDecideHelperExit returns
       QGA_HELPER_DEATH, the latch is never set, and the clock and the report run as before. */
    check("stand-down: a death is not a stand-down, so it is still supervised",
          QgaDecideHelperExit(0, 1, 0xC0000005u) == QGA_HELPER_DEATH &&
          QgaDesliceSupervisionHeld(0, 1000, 1000 + 60000ULL) == 0);

    /* ONE SITUATION, ONE DOM0 ROW. Measured 2026-10-23: one injected broker death produced BOTH
       `gui-agent.deslice-down` and `wgcbroker.death-1`, two seconds apart. Jev 1.00 that the
       de-slice row is the one to drop. */
    check("deslice double: while a death is being reported, the de-slice row is owned by it",
          QgaDesliceOwnedByDeath(307000) == TRUE);
    check("deslice double: with no death in flight the de-slice row is the only report, so it goes",
          QgaDesliceOwnedByDeath(0) == FALSE);

    /* GRACEFUL SHUTDOWN IS NOT A FAULT (owner 2026-10-23: "graceful shutdown should NOT emit
       error"). Measured: an ERROR line, the DesliceBrokerDown flag and a dom0 toast all went out
       9 s before `preshutdown` on a guest that was shutting down deliberately. */
    check("deslice teardown: helpers disarmed excuses it - that is the end-session handshake's first act",
          QgaDesliceTeardownExcused(1, 0) == TRUE);
    check("deslice teardown: a session ending excuses it too (logoff and fast-user-switch included)",
          QgaDesliceTeardownExcused(0, 1) == TRUE);
    check("deslice teardown: on a running guest nothing is excused - a real outage still reports",
          QgaDesliceTeardownExcused(0, 0) == FALSE);

    /* ---- "it is our shell", not a hardcoded name (owner 2026-10-11) ---- */
    check("shell: explorer is always a shell, even with no configured value read",
          QgaIsSessionShellImage(L"explorer.exe", L"") == TRUE);
    check("shell: explorer matches case-insensitively, as Windows file names do",
          QgaIsSessionShellImage(L"EXPLORER.EXE", NULL) == TRUE);
    check("shell: a KIOSK guest's configured shell counts as the shell",
          QgaIsSessionShellImage(L"kioskapp.exe", L"kioskapp.exe") == TRUE);
    check("shell: some other process is not the shell",
          QgaIsSessionShellImage(L"notepad.exe", L"kioskapp.exe") == FALSE);
    check("shell: an unreadable configured value never narrows - explorer still matches",
          QgaIsSessionShellImage(L"explorer.exe", L"kioskapp.exe") == TRUE);

    /* ---- a hold while the broker is still starting is not a fault (2026-10-11) ---- */
    check("hold: broker never ready yet -> STARTUP, the designed path on every cold boot",
          QgaHoldIsStartup(FALSE, FALSE) == TRUE);
    check("hold: broker WAS ready and is gone -> a fault, loud as before",
          QgaHoldIsStartup(FALSE, TRUE) == FALSE);
    check("hold: broker active and the window still holds -> a fault, loud as before",
          QgaHoldIsStartup(TRUE, TRUE) == FALSE);
    check("hold: broker active but never recorded ready -> still not a startup hold",
          QgaHoldIsStartup(TRUE, FALSE) == FALSE);

    /* ---- the hidden Start menu is CLOSED by returning activation, and the closure is SAID (2026-10-17) ----
     * The posted Escape was never going to work and this project had already measured that: main.c's
     * own comment of 2026-10-04 reads "A posted WM_KEYDOWN Escape to that window does NOT close
     * Start; a real Escape keystroke does (measured)". Targeting it better was tried and REFUTED on
     * win11de-sm the same day - the Escape landed on the window holding the input (0x10188) and the
     * menu was "STILL open" 1000 and 1063 ms later, twice. So the mechanism changed, and with it the
     * thing under test: not "which window gets the keystroke" but "what should happen to this
     * opening now". A real Escape via SendInput stays forbidden - measured closing Open-Shell's own
     * menu, Jev race_is_real 0.96.
     * The settle time is a PARAMETER here, so these assert the BOUND and not the literal 1000. */
    {
        const unsigned long long S = 1000;   /* START_DISMISS_SETTLE_MS at the call site */

        /* the eight rows, each driven on its own */
        check("start: not attempted, input not on Start yet -> look again",
              QgaHiddenStartVerdict(FALSE, TRUE, FALSE, TRUE, TRUE, 0, S) == QGA_START_LOOK_AGAIN);
        check("start: not attempted, Start holds input, dom0 named a live window -> return focus",
              QgaHiddenStartVerdict(FALSE, TRUE, TRUE, TRUE, TRUE, 0, S) == QGA_START_RETURN_FOCUS);
        check("start: dom0 named none -> the shell desktop",
              QgaHiddenStartVerdict(FALSE, TRUE, TRUE, FALSE, TRUE, 0, S) == QGA_START_RETURN_DESKTOP);
        check("start: no dom0 window and no shell window -> NO TARGET, nothing is activated",
              QgaHiddenStartVerdict(FALSE, TRUE, TRUE, FALSE, FALSE, 0, S) == QGA_START_NO_TARGET);
        check("start: attempted, surface gone -> closed (cloaked)",
              QgaHiddenStartVerdict(TRUE, FALSE, TRUE, TRUE, TRUE, 10, S) == QGA_START_CLOSED_CLOAKED);
        check("start: attempted, foreground left Start -> closed (fg left)",
              QgaHiddenStartVerdict(TRUE, TRUE, FALSE, TRUE, TRUE, 10, S) == QGA_START_CLOSED_FG_LEFT);
        check("start: attempted, still up inside the bound -> closing",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 328, S) == QGA_START_CLOSING);
        check("start: attempted, still up past the bound -> STUCK",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 1031, S) == QGA_START_STUCK);

        /* THE MEASURED SHAPE, from the run that refuted the previous fix: attempted, the surface
         * still visible, SearchHost still in front, 1031 ms gone against a 1000 ms bound. */
        check("start: the 2026-10-17 measured shape reads STUCK",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 1031, S) == QGA_START_STUCK);

        /* the boundary, from both sides - the bound itself is "settled" */
        check("start: 999 ms is still closing",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 999, S) == QGA_START_CLOSING);
        check("start: exactly the settle time is already stuck",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 1000, S) == QGA_START_STUCK);
        /* ...and it is the PARAMETER, not the number: a 4 s bound moves it */
        check("start: with a 4 s bound, 1031 ms is still closing",
              QgaHiddenStartVerdict(TRUE, TRUE, TRUE, TRUE, TRUE, 1031, 4000) == QGA_START_CLOSING);

        /* cloaked beats foreground-left when both hold: the surface being gone is what the user sees */
        check("start: surface gone AND foreground left -> cloaked wins",
              QgaHiddenStartVerdict(TRUE, FALSE, FALSE, TRUE, TRUE, 10, S) == QGA_START_CLOSED_CLOAKED);
        /* a live dom0 window wins over the desktop fallback */
        check("start: dom0's window wins over the shell desktop",
              QgaHiddenStartVerdict(FALSE, TRUE, TRUE, TRUE, TRUE, 0, S) != QGA_START_RETURN_DESKTOP);

        /* WHOLE-SPACE INVARIANTS, every combination of the five flags x four elapsed values. These
         * are what stop a later row from quietly activating NULL or reporting a closure that was
         * never attempted. */
        int combos = 0, badClosure = 0, badAttempted = 0, badFg = 0, badDesktop = 0, badPref = 0;
        for (int a = 0; a < 2; a++)
        for (int sv = 0; sv < 2; sv++)
        for (int fg = 0; fg < 2; fg++)
        for (int df = 0; df < 2; df++)
        for (int sw = 0; sw < 2; sw++)
        {
            const unsigned long long elapsed[4] = { 0, 1, 999, 5000 };
            for (int e = 0; e < 4; e++)
            {
                const QGA_START_VERDICT v = QgaHiddenStartVerdict((BOOL)a, (BOOL)sv, (BOOL)fg,
                    (BOOL)df, (BOOL)sw, elapsed[e], S);
                const int closureish = (v == QGA_START_CLOSED_CLOAKED || v == QGA_START_CLOSED_FG_LEFT ||
                                        v == QGA_START_CLOSING || v == QGA_START_STUCK);
                const int actionish = (v == QGA_START_RETURN_FOCUS || v == QGA_START_RETURN_DESKTOP ||
                                       v == QGA_START_NO_TARGET);
                combos++;
                if (closureish && !a)                                   badAttempted++;
                if (actionish && a)                                     badClosure++;
                if (actionish && !fg)                                   badFg++;
                if (v == QGA_START_RETURN_DESKTOP && !sw)               badDesktop++;
                if (df && fg && !a && v != QGA_START_RETURN_FOCUS)      badPref++;
            }
        }
        /* One assertion per invariant over the whole space, so a violation names WHICH rule broke
         * and in how many combinations - rather than 3200 ok lines burying the named cases above. */
        check("start invariant: every combination was driven (5 flags x 4 elapsed)", combos == 128);
        check("start invariant: a closure verdict only ever follows an attempt", badAttempted == 0);
        check("start invariant: an activation is only decided when nothing was attempted yet", badClosure == 0);
        check("start invariant: an activation is only decided while Start holds the input", badFg == 0);
        check("start invariant: the desktop is NEVER chosen without a shell window - no activation of NULL", badDesktop == 0);
        check("start invariant: dom0's live window is preferred to the desktop fallback", badPref == 0);
    }

    /* a Start replacement is identified by its OWN executable: nothing else is knowable before its
     * menu window exists, and its menu window exists only after the key we are deciding about. */
    {
        check("replacement: Open-Shell's own executable counts",
              QgaIsStartReplacementImage(L"StartMenu.exe") == TRUE);
        check("replacement: case does not matter",
              QgaIsStartReplacementImage(L"startmenu.EXE") == TRUE);
        check("replacement: the default shell is NOT a replacement",
              QgaIsStartReplacementImage(L"explorer.exe") == FALSE);
        check("replacement: an unrelated process is not one",
              QgaIsStartReplacementImage(L"notepad.exe") == FALSE);
        check("replacement: an empty or missing image is not one",
              QgaIsStartReplacementImage(L"") == FALSE && QgaIsStartReplacementImage(NULL) == FALSE);
    }

    /* ---- the host's menu key may not summon a surface we cannot close (2026-10-17) ----
     * MEASURED: with SeamlessStart=0 the native Start is not presented and the agent cannot close
     * it - a posted Escape leaves it open 1000/1063 ms, and returning activation is REFUSED even
     * with an input nudge that injects cleanly. A real injected Escape does close it but rides the
     * system input queue, which is a race. So in that cell the key is dropped and nothing opens.
     * CONDITIONAL, because a third-party Start replacement wants that key: dropping it whenever we
     * suppress would silently break the user's own menu. Jev: conditional-on-a-replacement 0.99,
     * and FORWARD when we cannot tell (0.65) because that failure is the invisible one. */
    {
        /* not seamless: never our business */
        check("menukey: fullscreen always passes the key",
              QgaMenuKeyPolicy(FALSE, TRUE, FALSE, TRUE, FALSE) == QGA_MENUKEY_FORWARD);
        /* the existing rule, unchanged */
        check("menukey: blocked in seamless stays blocked",
              QgaMenuKeyPolicy(TRUE, TRUE, FALSE, TRUE, FALSE) == QGA_MENUKEY_DROP_BLOCKED);
        /* dom0 asked for the key and we PRESENT what it summons */
        check("menukey: SeamlessStart=1 forwards - we present what it opens",
              QgaMenuKeyPolicy(TRUE, FALSE, TRUE, TRUE, FALSE) == QGA_MENUKEY_FORWARD);
        /* dom0 asked for it, we suppress, and no replacement wants it */
        check("menukey: suppressing with no replacement -> dropped, so nothing unclosable opens",
              QgaMenuKeyPolicy(TRUE, FALSE, FALSE, TRUE, FALSE) == QGA_MENUKEY_DROP_UNCLOSABLE);
        /* a replacement IS present: the key is its own */
        check("menukey: a Start replacement is present -> forwarded, it is that menu's key",
              QgaMenuKeyPolicy(TRUE, FALSE, FALSE, TRUE, TRUE) == QGA_MENUKEY_FORWARD);
        /* undecidable -> forward, because breaking the user's own menu is the invisible failure */
        check("menukey: cannot tell whether a replacement exists -> forwarded",
              QgaMenuKeyPolicy(TRUE, FALSE, FALSE, FALSE, FALSE) == QGA_MENUKEY_FORWARD);

        /* whole space: the key is dropped as UNCLOSABLE only in the one cell that earns it */
        int combos = 0, badDrop = 0, badBlock = 0;
        for (int sm = 0; sm < 2; sm++)
        for (int bm = 0; bm < 2; bm++)
        for (int ss = 0; ss < 2; ss++)
        for (int rk = 0; rk < 2; rk++)
        for (int rp = 0; rp < 2; rp++)
        {
            const QGA_MENUKEY_POLICY v = QgaMenuKeyPolicy((BOOL)sm, (BOOL)bm, (BOOL)ss, (BOOL)rk, (BOOL)rp);
            combos++;
            if (v == QGA_MENUKEY_DROP_UNCLOSABLE && !(sm && !bm && !ss && rk && !rp)) badDrop++;
            if (v == QGA_MENUKEY_DROP_BLOCKED && !(sm && bm))                         badBlock++;
        }
        check("menukey invariant: every combination driven", combos == 32);
        check("menukey invariant: dropped-as-unclosable ONLY when seamless, allowed, suppressing, and no replacement is known absent", badDrop == 0);
        check("menukey invariant: dropped-as-blocked only when seamless and blocked", badBlock == 0);
    }

    /* ---- the settings matrix, and the COMPANION half of "fully suppressed" (2026-10-17) ----
     * A 25H2 Start opening brings up two surfaces: StartMenuExperienceHost's, and a SearchHost one
     * carrying the search box ('Suche' on the German guest, 800x750). Refusing only the first left
     * the second on dom0's screen. The stock rule for this compared the handle to a caption lookup
     * (FindWindow(..., L"Search")) - measured 0x0 on that guest while the caption is 'Suche', so it
     * never fired on the configuration the defect came from.
     * hiddenStartOpen is the ONLY fact separating the companion from a standalone Win+S Search the
     * user asked for and must get, so the suite drives both directions. */
    {
        /* the four shipped cells x the surface kinds */
        check("surface: non-seamless presents everything, Start included",
              QgaShellSurfacePolicy(FALSE, FALSE, QGA_SURFACE_KIND_START, FALSE) == QGA_SURFACE_PRESENT);
        check("surface: SeamlessStart=1 presents the Start surface",
              QgaShellSurfacePolicy(TRUE, TRUE, QGA_SURFACE_KIND_START, FALSE) == QGA_SURFACE_PRESENT);
        check("surface: seamless + SeamlessStart=0 refuses the Start surface",
              QgaShellSurfacePolicy(TRUE, FALSE, QGA_SURFACE_KIND_START, FALSE) == QGA_SURFACE_REFUSE_START);
        check("surface: the Search companion OF a hidden Start opening is refused",
              QgaShellSurfacePolicy(TRUE, FALSE, QGA_SURFACE_KIND_SEARCH, TRUE) == QGA_SURFACE_REFUSE_COMPANION);
        check("surface: a STANDALONE Search is presented - the user asked for it",
              QgaShellSurfacePolicy(TRUE, FALSE, QGA_SURFACE_KIND_SEARCH, FALSE) == QGA_SURFACE_PRESENT);
        check("surface: a toast is presented whatever Start is doing",
              QgaShellSurfacePolicy(TRUE, FALSE, QGA_SURFACE_KIND_TOAST, TRUE) == QGA_SURFACE_PRESENT);
        check("surface: SeamlessStart=1 presents the companion too",
              QgaShellSurfacePolicy(TRUE, TRUE, QGA_SURFACE_KIND_SEARCH, TRUE) == QGA_SURFACE_PRESENT);
        /* whole space: nothing is refused outside seamless, and only Start/Search are ever refused */
        {
            int badOutside = 0, badKind = 0, n = 0;
            for (int sm = 0; sm < 2; sm++)
            for (int ss = 0; ss < 2; ss++)
            for (int k = 0; k <= QGA_SURFACE_KIND_SEARCH; k++)
            for (int ho = 0; ho < 2; ho++)
            {
                const QGA_SURFACE_POLICY p = QgaShellSurfacePolicy((BOOL)sm, (BOOL)ss, k, (BOOL)ho);
                n++;
                if (p != QGA_SURFACE_PRESENT && (!sm || ss)) badOutside++;
                if (p != QGA_SURFACE_PRESENT && k != QGA_SURFACE_KIND_START && k != QGA_SURFACE_KIND_SEARCH) badKind++;
            }
            check("surface invariant: every cell driven", n == 32);
            check("surface invariant: nothing is refused outside seamless-with-SeamlessStart=0", badOutside == 0);
            check("surface invariant: only Start and Search surfaces are ever refused", badKind == 0);
        }

        /* ---- MAY A SHELL SURFACE BE A MOVABLE (WM-MANAGED) WINDOW? ----
         * The owner, 2026-10-18: "resizable may drop", "movable is important". Start is announced
         * or=0 and size-locked. The guard is the screen-sized state: measured the same day, the
         * host's CoreWindow held 5120x1392 at 0,0 for a whole opening while its card was 642x726,
         * and 2026-08-12 recorded a shell CoreWindow announced or=0 at 5120x1336 - a full-screen
         * framed draggable window on dom0's screen. */
        check("frame: a measured Start card on a 5120x1440 screen is movable",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   642, 726, 5120, 1440) == TRUE);
        check("frame: the SCREEN-SIZED state of that same surface is refused the frame",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   5120, 1392, 5120, 1440) == FALSE);
        check("frame: the 2026-08-12 shape, or=0 at 5120x1336, is refused",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   5120, 1336, 5120, 1440) == FALSE);
        check("frame: a card on a 1920x1080 screen is still movable (the test is not tuned to 5120)",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   640, 750, 1920, 1080) == TRUE);
        /* "no crop resolved yet -> no frame" is the CALLER's gate and is deliberately not a
         * parameter here: at the only call site it would always be TRUE, which is an inert
         * argument. */
        check("frame: policy NONE keeps every surface override-redirect",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_NONE, QGA_SURFACE_KIND_START,
                                   642, 726, 5120, 1440) == FALSE);
        check("frame: START-only policy does not frame a toast",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_TOAST,
                                   364, 326, 5120, 1440) == FALSE);
        check("frame: policy ALL does frame a toast",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_ALL, QGA_SURFACE_KIND_TOAST,
                                   364, 326, 5120, 1440) == TRUE);
        check("frame: an unclassified window is never framed by this rule",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_ALL, QGA_SURFACE_KIND_NONE,
                                   642, 726, 5120, 1440) == FALSE);
        check("frame: a zero-sized announce is refused",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   0, 726, 5120, 1440) == FALSE);
        /* screen not known yet (g_ScreenWidth is 0 until the first xconf): the proportion test is
         * disabled, everything else still applies - refusing every shell frame until then would be
         * its own defect. */
        check("frame: an unknown screen size does not refuse the frame outright",
              QgaShellFrameAllowed(QGA_SHELL_MANAGED_START, QGA_SURFACE_KIND_START,
                                   642, 726, 0, 0) == TRUE);
    }

    /* ---- the card rule follows the KIND, and Search gets NO rule (2026-10-17) ----
     * MEASURED, the same Search window at its two raw sizes:
     *   raw 800x798 -> largest=(2364,655) 774x724   union=(2363,655) 775x777
     *   raw 800x750 -> largest=(2196,687) 728x32    union=(2196,687) 728x32
     * At the smaller size BOTH candidates collapse to the same 728x32 sliver - the search input
     * field, not the card - so a crop from either would clip the flyout. Jev on those walk lines:
     * neither-announce-uncropped 0.72. The selector used to be one BOOL ("is it a menu popup"), so
     * every non-menu surface got the TOAST rule by construction. */
    {
        check("card: a menu popup uses the union rule",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_NONE, TRUE) == QGA_CARD_RULE_MENU_UNION);
        check("card: a toast uses the largest rule",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_TOAST, FALSE) == QGA_CARD_RULE_TOAST_LARGEST);
        check("card: the 25H2 Start card is still found by the largest rule",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_START, FALSE) == QGA_CARD_RULE_TOAST_LARGEST);
        check("card: a Search flyout gets NO rule - both candidates collapse at one raw size",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_SEARCH, FALSE) == QGA_CARD_RULE_NONE);
        check("card: a menu popup wins over the kind",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_SEARCH, TRUE) == QGA_CARD_RULE_MENU_UNION);
        check("card: an unclassified surface gets no rule",
              QgaCardRuleForSurface(QGA_SURFACE_KIND_NONE, FALSE) == QGA_CARD_RULE_NONE);
    }

    /* ---- xencontrol's per-notify Debug line is dropped at our callback (2026-10-17) ----
     * MEASURED: 118 identical "XcEvtchnNotify: LocalPort: 19" lines at Debug in one run - a line
     * per vchan notification, i.e. one per success. Dropped at the callback rather than by capping
     * xencontrol's level, which would also hide its grant and map lines. 4 == LOG_LEVEL_DEBUG. */
    {
        check("xc: the per-notify Debug line is dropped",
              QgaXcLineKept(4, "XcEvtchnNotify") == FALSE);
        check("xc: that same function at WARNING is KEPT - a failing notify path is not suppressed",
              QgaXcLineKept(2, "XcEvtchnNotify") == TRUE);
        check("xc: another function at Debug is kept (the level is not capped)",
              QgaXcLineKept(4, "XcGnttabPermitForeignAccess") == TRUE);
        check("xc: a NULL function name is kept - never drop what cannot be identified",
              QgaXcLineKept(4, NULL) == TRUE);
    }

    /* ---- BROKERHOLD's level follows its REASON (2026-10-17) ----
     * BrokerFreshFrame refuses a frame for eleven reasons and now names which. Exactly ONE is
     * normal: a slot the broker has registered but not yet published to, which carries
     * FrameWidth=FrameHeight=0 and so lands in the dims branch silently (its log is gated on
     * fw>0 && fh>0 - which is why 589 holds coexisted with ZERO BROKERDIMS and why reading that
     * silence as "not the dims cause" was backwards). QGADIRECTSUPPRESS reports exactly this
     * condition persisting, so here the safety net is the right one rather than a borrowed one.
     * Every other reason is a different finding and stays loud. */
    check("holdreason: a slot with no first frame yet is normal - quiet",
          QgaHoldReasonIsNormal(L"first-frame-pending") == TRUE);
    check("holdreason: the agent and broker disagreeing about a slot owner is NOT normal",
          QgaHoldReasonIsNormal(L"slot-holds-another-hwnd") == FALSE);
    check("holdreason: a feed lost past its bound is NOT normal",
          QgaHoldReasonIsNormal(L"dims-mismatch-STUCK") == FALSE);
    check("holdreason: a NON-ZERO dims mismatch is a real size disagreement, still loud",
          QgaHoldReasonIsNormal(L"dims-mismatch-transient") == FALSE);
    check("holdreason: the secure desktop pausing the broker is still said",
          QgaHoldReasonIsNormal(L"broker-paused") == FALSE);
    check("holdreason: contention on the ring is still said",
          QgaHoldReasonIsNormal(L"torn-read-4-retries") == FALSE);
    check("holdreason: a slot that is not ACTIVE is still said",
          QgaHoldReasonIsNormal(L"slot-not-ACTIVE") == FALSE);
    check("holdreason: a reason nobody has classified is LOUD by default, not quiet",
          QgaHoldReasonIsNormal(L"some-reason-added-later") == FALSE);
    check("holdreason: and no reason at all is never quiet",
          QgaHoldReasonIsNormal(NULL) == FALSE);

    /* ---- the first seconds of a slow capture pass are not a warning (2026-10-17) ----
     * Measured over run 38994277148: every slow pass in the campaign was 1-3 s against the 15 s
     * wedge, the wedge arm fired ZERO times, all recovered, all during install/boot load. The
     * split is HALF THE WEDGE - derived from the wedge, not from those numbers, so it would sit in
     * the same place whatever the campaign had shown. Jev: graduate the report 0.86, raise the
     * threshold 0.00. */
    check("slowpass: 1 s of a 15 s budget is brief - Info",
          QgaCaptureSlowIsNormal(1000, 15000) == TRUE);
    check("slowpass: the longest pass the campaign measured, 3 s, is still brief",
          QgaCaptureSlowIsNormal(3000, 15000) == TRUE);
    check("slowpass: just under half the budget is still brief",
          QgaCaptureSlowIsNormal(7499, 15000) == TRUE);
    check("slowpass: AT half the budget it is a warning - more spent than left",
          QgaCaptureSlowIsNormal(7500, 15000) == FALSE);
    check("slowpass: well past half, loud",
          QgaCaptureSlowIsNormal(14000, 15000) == FALSE);
    check("slowpass: the boundary follows the wedge, not a literal - a 4 s wedge warns at 2 s",
          QgaCaptureSlowIsNormal(2000, 4000) == FALSE);
    check("slowpass: and is brief below it on that same wedge",
          QgaCaptureSlowIsNormal(1999, 4000) == TRUE);
    check("slowpass: no wedge configured -> nothing to be a fraction of, so never quiet",
          QgaCaptureSlowIsNormal(1000, 0) == FALSE);

    /* ---- a LATE registration: which outage was it the tail of? (2026-10-11) ----
     * The first fix here asked QgaHoldIsStartup(FALSE, brokerReady) at a site where brokerReady is
     * true by construction, so the quiet branch was unreachable and the guest kept logging W. These
     * drive both facts that actually vary. */
    check("latereg: never refused, never had a slot -> the broker had not started: quiet",
          QgaLateRegIsStartup(FALSE, FALSE) == TRUE);
    check("latereg: a RUNNING broker refused it (no slot / arena full) -> capacity fault, loud",
          QgaLateRegIsStartup(TRUE, FALSE) == FALSE);
    check("latereg: it HELD a slot and lost it -> the broker died, loud",
          QgaLateRegIsStartup(FALSE, TRUE) == FALSE);
    check("latereg: refused AND previously sourced -> loud",
          QgaLateRegIsStartup(TRUE, TRUE) == FALSE);

    /* ---- the per-window fault clock cannot start before the broker can feed it (2026-10-11) ---- */
    /* THE MEASURED CASE: registered at 1000 while the broker was still coming up, ready only at
     * 26000; at 30000 the window is 29 s old but has had the broker for 4 s - not a fault yet. */
    check("fault: registered before the broker was ready -> the grace runs from broker-ready",
          QgaWindowFaultDue(1000, 26000, 30000, 10000) == FALSE);
    check("fault: and it IS due once the grace has run since broker-ready",
          QgaWindowFaultDue(1000, 26000, 37000, 10000) == TRUE);
    check("fault: registered with a broker already ready -> the grace runs from registration",
          QgaWindowFaultDue(26000, 1000, 37000, 10000) == TRUE);
    check("fault: inside the grace is never due",
          QgaWindowFaultDue(26000, 1000, 30000, 10000) == FALSE);
    check("fault: a broker that was NEVER ready does not postpone the report",
          QgaWindowFaultDue(1000, 0, 12000, 10000) == TRUE);
    check("fault: a window never registered has no clock at all",
          QgaWindowFaultDue(0, 1000, 99000, 10000) == FALSE);

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
