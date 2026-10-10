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

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
