/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once

// qga-lifecycle.h - what the GUI agent and the QubesGuiWatchdog service agree on about the agent's END
// (docs/ADR-supervision.md section 5, main repo; decided 2026-10-07).
//
// THE DEFECT CLASS THIS CLOSES. At every guest shutdown Windows' session teardown terminated the agent with
// 0x40010004 (DBG_TERMINATE_PROCESS) 33-826 ms after the shutdown began; the service relaunched it 13 ms-1 s
// later INTO THE SAME, ENDING SESSION, where it was terminated again - three instances per shutdown, two of
// them killed with their vchan announcement left behind and a death record each. The service's guard
// (SM_SHUTTINGDOWN, its own SCM control) read 0 at every death: a session-0 service cannot see session 1's
// teardown, and PRESHUTDOWN arrived 4-32 s after it. The owner's rule (R1): if you terminate something that
// relaunches, you make sure it STOPS relaunching beforehand.
//
// THE ORDER NOW. (1) The agent's first act is a hidden top-level window on its own thread; Windows sends it
// WM_QUERYENDSESSION before the teardown. The agent then disarms its helpers' relaunch FIRST, signals the
// service "my session is ending" through the channel below and waits (bounded) for the service's
// acknowledgement that it will not launch into that session again. (2) On WM_ENDSESSION(TRUE) the agent leaves
// through its orderly exit with QGA_EXIT_SESSION_END. (3) The service reads every exit through ONE pure decision,
// QgaDecideAgentExit - a table, so the offline suite (watchdog/lifecycle_test.c) can hold every row.
//
// THE CHANNEL: three events the SERVICE creates before the agent runs (the agent is created suspended, the
// objects are made with its pid in their names and a SYSTEM-only DACL, then it is resumed), the agent only
// OPENS them. Named per agent pid so that no process can pre-create them for a pid it does not have; a name
// that already exists at creation is refused (ERROR_ALREADY_EXISTS) and the suspended agent is ended before
// it ran. Nothing is passed through them but the signal: the service knows the session it launched into.
//   notice    manual-reset, agent -> service: "Windows is ending my session"
//   ack       manual-reset, service -> agent: "acknowledged: no launch into that session from here on"
//   continue  auto-reset,   agent -> service: "the end was cancelled (WM_ENDSESSION FALSE); the session continues"
//   done      manual-reset, agent -> service: "my orderly exit is COMPLETE - the vchan is withdrawn, window 0 is
//             unmapped, the staging grants are released; a kill after this point costs nothing". Added
//             2026-10-07 because the service could not tell two cases apart from the observed process code
//             alone: killed BEFORE finishing (a real defect - dom0 loses a clean withdrawal) and reaped a
//             few milliseconds AFTER finishing, which is what every measured clean shutdown does. Both
//             landed on the same ERROR row, so our own log carried one error per shutdown. The level is not
//             demoted - the case that really is an error still is one (Jev: demoting without this evidence
//             would be hiding, 0.87; declaring it in the harness likewise, 0.78).

#include <windows.h>
#include <strsafe.h>
#include "qga-exitcodes.h"

#define QGA_LIFECYCLE_NAME_FMT      L"Global\\QGA_LIFECYCLE_%lu_%s"
#define QGA_LIFECYCLE_NOTICE        L"notice"
#define QGA_LIFECYCLE_ACK           L"ack"
#define QGA_LIFECYCLE_CONTINUE      L"continue"
#define QGA_LIFECYCLE_DONE          L"done"
// SYSTEM only: the agent runs as SYSTEM in the console session, the service as SYSTEM in session 0. Nothing
// else may signal "my session is ending" for an agent, or acknowledge it.
#define QGA_LIFECYCLE_SDDL          L"D:P(A;;GA;;;SY)"

// THE END-SESSION BUDGETS - failure detectors, never fixes (R4). Windows waits for a WM_QUERYENDSESSION answer
// (HungAppTimeout, 5 s by default) and for WM_ENDSESSION processing (WaitToKillAppTimeout, 20 s); both of
// ours are inside those, and expiry of either is a loud ERROR in the agent's log, after which the end is
// allowed anyway - a service that never answers must not stall the shutdown (the handshake-deadlock risk,
// Jev 0.42 2026-10-07).
#define QGA_ENDSESSION_ACK_WAIT_MS   2000
#define QGA_ENDSESSION_EXIT_WAIT_MS 10000
// How long the agent's first act may take to put the window up before Init proceeds without it (ERROR).
#define QGA_LIFECYCLE_WINDOW_WAIT_MS 5000

static __inline HRESULT QgaLifecycleObjectName(OUT WCHAR *out, IN size_t cch, IN DWORD agentPid, IN const WCHAR *which)
{
    return StringCchPrintfW(out, cch, QGA_LIFECYCLE_NAME_FMT, (unsigned long)agentPid, which);
}

// ---- THE DECISION ON AN AGENT EXIT (service side) --------------------------------------------------------
//
// Pure: exit code + whether this instance's end-session notice was acknowledged -> what the service does.
// Table-driven; first matching row wins. Every row is held by watchdog/lifecycle_test.c, which also builds the
// table with each QGA_LIFECYCLE_DEFECT_* below and requires the suite to FAIL on it.
typedef enum _QGA_EXIT_DECISION
{
    QGA_DECIDE_NOTHING = 0,             // a requested exit: no record, no relaunch; the service waits for its next launch trigger
    QGA_DECIDE_SESSION_END,             // orderly end after an acknowledged notice: INFO; never launch into that session again
    QGA_DECIDE_SESSION_END_UNNOTICED,   // QGA_EXIT_SESSION_END with no notice seen: the channel failed - ERROR, no launch into that session
    QGA_DECIDE_SESSION_END_FORCED,      // the system ended it after an acknowledged notice and its orderly exit had NOT finished - ERROR
    QGA_DECIDE_SESSION_END_REAPED,      // the system ended it after an acknowledged notice and AFTER its orderly exit completed: INFO, no launch into that session
    QGA_DECIDE_NOTICE_MISSED,           // the system ended it and no notice came: ERROR "end-session notice missed", no launch into that session
    QGA_DECIDE_NOGUI_LATCH,             // no GUI domain this boot: latched, nothing relaunched before the next boot
    QGA_DECIDE_RECONNECT,               // relaunch at once - the one relaunch the service performs; not a death
    QGA_DECIDE_DEATH_IN_ENDING_SESSION, // died after an acknowledged notice: 4001 + ERROR; no launch into that session; the service stays
    QGA_DECIDE_DEATH                    // died: 4001 + ERROR, and the service ends itself with QGA_SVC_EXIT_AGENT_DIED for the SCM's recovery
} QGA_EXIT_DECISION;

typedef struct _QGA_EXIT_VERDICT
{
    QGA_EXIT_DECISION Decision;
    BOOL RelaunchNow;          // the service launches a new agent at once (RECONNECT only)
    BOOL NoLaunchIntoSession;  // the session the agent ran in is ending: no launch into it until it has ended
    BOOL WriteDeathRecord;     // Application event 4001 (include/deathevent.h)
    BOOL FailService;          // the service reports STOPPED with QGA_SVC_EXIT_AGENT_DIED, for the SCM's recovery
    BOOL IsError;              // logged at ERROR (else INFO)
    const WCHAR *Name;         // the log token's tail: QGAWD<Name>
} QGA_EXIT_VERDICT;

typedef struct _QGA_EXIT_ROW
{
    DWORD ExitCode;            // QGA_EXIT_* / QGA_EXIT_SYSTEM_TERMINATED, or QGA_EXIT_ANY
    int NoticeAcked;           // 1, 0, or QGA_NOTICE_ANY
    int OrderlyDone;           // 1, 0, or QGA_DONE_ANY - the agent signalled that its orderly exit completed
    QGA_EXIT_DECISION Decision;
} QGA_EXIT_ROW;
#define QGA_EXIT_ANY    0xFFFFFFFFUL
#define QGA_NOTICE_ANY  (-1)
#define QGA_DONE_ANY    (-1)

// The attributes of each decision, indexed by QGA_EXIT_DECISION.
static const QGA_EXIT_VERDICT QgaExitVerdicts[] =
{
    /* decision                           relaunch  noLaunch  record  fail   error  name */
    { QGA_DECIDE_NOTHING,                 FALSE,    FALSE,    FALSE,  FALSE, FALSE, L"REQUESTED" },
    { QGA_DECIDE_SESSION_END,             FALSE,    TRUE,     FALSE,  FALSE, FALSE, L"SESSIONEND" },
    { QGA_DECIDE_SESSION_END_UNNOTICED,   FALSE,    TRUE,     FALSE,  FALSE, TRUE,  L"SESSIONEND-UNNOTICED" },
    { QGA_DECIDE_SESSION_END_FORCED,      FALSE,    TRUE,     FALSE,  FALSE, TRUE,  L"SESSIONEND-FORCED" },
    { QGA_DECIDE_SESSION_END_REAPED,      FALSE,    TRUE,     FALSE,  FALSE, FALSE, L"SESSIONEND-REAPED" },
    { QGA_DECIDE_NOTICE_MISSED,           FALSE,    TRUE,     FALSE,  FALSE, TRUE,  L"NOTICEMISSED" },
    { QGA_DECIDE_NOGUI_LATCH,             FALSE,    FALSE,    FALSE,  FALSE, FALSE, L"NOGUIDOMAIN" },
#ifdef QGA_LIFECYCLE_DEFECT_RECONNECTDEATH
    { QGA_DECIDE_RECONNECT,               TRUE,     FALSE,    TRUE,   FALSE, TRUE,  L"RECONNECT" },   // DEFECT: a reconnect records a death
#else
    { QGA_DECIDE_RECONNECT,               TRUE,     FALSE,    FALSE,  FALSE, FALSE, L"RECONNECT" },
#endif
    { QGA_DECIDE_DEATH_IN_ENDING_SESSION, FALSE,    TRUE,     TRUE,   FALSE, TRUE,  L"DEATH-SESSIONENDING" },
#ifdef QGA_LIFECYCLE_DEFECT_RELAUNCHDEATH
    { QGA_DECIDE_DEATH,                   TRUE,     FALSE,    TRUE,   FALSE, TRUE,  L"DEATH" },   // DEFECT: the service relaunches a dead agent itself (the old loop)
#else
    { QGA_DECIDE_DEATH,                   FALSE,    FALSE,    TRUE,   TRUE,  TRUE,  L"DEATH" },
#endif
};

// The table. Order matters: the defined codes first, then the system's termination status, then everything else.
static const QGA_EXIT_ROW QgaExitRows[] =
{
#ifndef QGA_LIFECYCLE_DEFECT_REQUESTEDDEATH
    { QGA_EXIT_REQUESTED,         QGA_NOTICE_ANY, QGA_DONE_ANY, QGA_DECIDE_NOTHING },
#endif
    { QGA_EXIT_NO_GUI_DOMAIN,     QGA_NOTICE_ANY, QGA_DONE_ANY, QGA_DECIDE_NOGUI_LATCH },
    { QGA_EXIT_SESSION_END,       1,              QGA_DONE_ANY, QGA_DECIDE_SESSION_END },
    { QGA_EXIT_SESSION_END,       0,              QGA_DONE_ANY, QGA_DECIDE_SESSION_END_UNNOTICED },
#ifndef QGA_LIFECYCLE_DEFECT_TERMINATEDDEATH
    // THE SYSTEM'S TERMINATION, split on whether the orderly exit had finished. Before 2026-10-07 both were
    // SESSIONEND-FORCED, so every clean shutdown wrote an ERROR line about an exit that had in fact completed.
    { QGA_EXIT_SYSTEM_TERMINATED, 1,              1,            QGA_DECIDE_SESSION_END_REAPED },
    { QGA_EXIT_SYSTEM_TERMINATED, 1,              0,            QGA_DECIDE_SESSION_END_FORCED },
    { QGA_EXIT_SYSTEM_TERMINATED, 0,              QGA_DONE_ANY, QGA_DECIDE_NOTICE_MISSED },   // DEFECT (knob TERMINATEDDEATH): the measured relaunch into the ending session
#endif
    { QGA_EXIT_RECONNECT,         1,              QGA_DONE_ANY, QGA_DECIDE_SESSION_END },     // a notice always wins: nothing is launched into an ending session
    { QGA_EXIT_RECONNECT,         0,              QGA_DONE_ANY, QGA_DECIDE_RECONNECT },
    { QGA_EXIT_ANY,               1,              QGA_DONE_ANY, QGA_DECIDE_DEATH_IN_ENDING_SESSION },
    { QGA_EXIT_ANY,               QGA_NOTICE_ANY, QGA_DONE_ANY, QGA_DECIDE_DEATH },
};

static __inline QGA_EXIT_VERDICT QgaDecideAgentExit(IN DWORD exitCode, IN BOOL noticeAcked, IN BOOL orderlyDone)
{
    size_t i;
    QGA_EXIT_DECISION d = QGA_DECIDE_DEATH;
    for (i = 0; i < RTL_NUMBER_OF(QgaExitRows); i++)
    {
        const QGA_EXIT_ROW *r = &QgaExitRows[i];
        if (r->ExitCode != QGA_EXIT_ANY && r->ExitCode != exitCode)
            continue;
        if (r->NoticeAcked != QGA_NOTICE_ANY && (r->NoticeAcked != 0) != (noticeAcked != 0))
            continue;
        if (r->OrderlyDone != QGA_DONE_ANY && (r->OrderlyDone != 0) != (orderlyDone != 0))
            continue;
        d = r->Decision;
        break;
    }
    for (i = 0; i < RTL_NUMBER_OF(QgaExitVerdicts); i++)
        if (QgaExitVerdicts[i].Decision == d)
            return QgaExitVerdicts[i];
    return QgaExitVerdicts[RTL_NUMBER_OF(QgaExitVerdicts) - 1];   // unreachable: every decision has a row
}

// ---- THE DECISION ON A HELPER EXIT (agent side: the broker, the notification bridge, the ETW proxy) ----------
// The agent supervises three helpers, and the SAME question arises for each: was this exit a death to report, or the
// session going away underneath it? Pure and table-shaped for the same reason QgaDecideAgentExit is - the offline
// suite holds every row.
//
// THE DEFECT THIS CLOSES (measured 2026-10-10 on win10-acc). All three sites tested only HelpersDisarmed(), which
// becomes true once Windows has told the AGENT its session is ending. Windows ends a session's processes in an order
// we do not control, so a helper can be gone BEFORE that notice arrives. Two Application-log 4003 records for
// notifhost.exe, killed 27 s after boot by an ordinary shutdown, each escalated by guest/qwt-report-death.ps1 into a
// dom0 notification reading "exited unexpectedly ... this is a major error". The agent's own exit already had the
// rule - every 0x40010004 row of QgaDecideAgentExit writes no death record - and the helpers never got it.
//
// QGA_EXIT_SYSTEM_TERMINATED (0x40010004, DBG_TERMINATE_PROCESS) is the fact that settles it without the race: no
// program returns it for itself; the system or a debugger imposes it.
//
// THE TWO EXPECTED ARMS ARE NOT THE SAME and must not collapse into one line - that is what would hide a kill:
//   DISARMED  the agent knew first. Routine teardown: INFO.
//   SYSKILL   the system killed the helper before the agent was told. That ordering is itself the anomaly, and an
//             unexplained system kill with no shutdown in progress lands here too, so: WARNING, with its own grep
//             tag per helper (WGCBROKERSYSKILL / NOTIFBRIDGESYSKILL / ETWPROXYSYSKILL).
// Neither arm writes an Event Log death record, so neither reaches dom0 as a death.
// The knob QGA_LIFECYCLE_DEFECT_HELPERSYSKILLDEATH restores the measured defect and must make the suite FAIL.
typedef enum _QGA_HELPER_EXIT_DECISION
{
    QGA_HELPER_EXPECTED_DISARMED = 0,   // launches disarmed: the session is ending or the agent is leaving. INFO, no record.
    QGA_HELPER_EXPECTED_SYSKILL,        // the system imposed 0x40010004 before the agent was told. WARNING, no record.
    QGA_HELPER_DEATH                    // anything else: a death. ERROR + one Event Log record (the dom0 route's input).
} QGA_HELPER_EXIT_DECISION;

// `exited` is FALSE for the broker's hang arm, where no exit code exists to read - a hang is never excused here.
// AN NTSTATUS ERROR-SEVERITY CODE IS A CRASH, whatever else was happening. The top two bits of an
// NTSTATUS are its severity, and 3 (0xC.......) is STATUS_SEVERITY_ERROR: the whole crash family lands
// there - 0xC0000005 an access violation, 0xC0000409 a fast-fail abort, 0xC0000374 heap corruption,
// 0xC00000FD a stack overflow, 0xC000001D an illegal instruction - and so do the codes nobody has
// enumerated yet, which is why this is a severity test and not a list (Jev: ntstatus-severity 0.69 over
// an explicit list). 0xE06D7363 (C++) and 0xE0434352 (.NET) are customer-flagged and caught by the same
// test. QGA_EXIT_SYSTEM_TERMINATED (0x40010004) is severity 1, so it is NOT a crash by this test, which
// is the whole point: Windows' own teardown code must stay excusable while a crash never is.
static __inline BOOL QgaExitIsCrash(IN DWORD exitCode)
{
    return (BOOL)(((exitCode >> 30) & 3u) == 3u);
}

// A REAL ABNORMAL TERMINATION IS ALWAYS LOUD - owner, 2026-10-10: "yet, a REAL abnormal termination
// should be always reported loudly", after "if we shut down the guest, none of its components should
// complain about the fact". Both halves live here: the disarm (the session is ending, so we expect our
// helpers to go) excuses only the two ends that ARE the shutdown - Windows' teardown code, and a clean
// exit 0 - and excuses nothing else. A helper that takes an access violation while the guest is going
// down is a defect of ours and is reported like any other death.
// THE DEFECT THIS CLOSES, and it was flagged when the disarm landed: `if (disarmed) return EXPECTED`
// returned BEFORE the exit code was read, so from the first moment of a shutdown every helper exit was
// expected - a crash included. Jev scored that `hides_a_real_kill` 0.70 at the time and
// `crash_must_be_loud` 0.95 now; it is a NARROWING of the excuse, not a reversal of it
// (`reverses_cd73a56` 0.26) - the teardown case this file exists for stays silent.
// WHAT IS NOT SETTLED, recorded rather than decided quietly: for a clean exit 0 during the disarm Jev
// split warn 0.48 / silent 0.47, and for an arbitrary nonzero it leaned loud at only 0.53. Exit 0 is
// treated as expected (it is normal for a teardown) and any other nonzero as a death (his rule's plain
// reading). The sweep now counts both, which is the data a later round needs.
static __inline QGA_HELPER_EXIT_DECISION QgaDecideHelperExit(IN BOOL disarmed, IN BOOL exited, IN DWORD exitCode)
{
#ifndef QGA_LIFECYCLE_DEFECT_DISARMHIDESCRASH
    // BOTH of these are the narrowing, so the knob has to drop BOTH - with only the first one guarded
    // the second still caught the crash and the knob changed nothing, which the suite reported as
    // "that guard is decoration". A knob that cannot restore the defect proves nothing.
    if (disarmed && exited && QgaExitIsCrash(exitCode))
        return QGA_HELPER_DEATH;   // the disarm never excuses a crash
    if (disarmed && exited && exitCode != QGA_EXIT_SYSTEM_TERMINATED && exitCode != 0)
        return QGA_HELPER_DEATH;   // its own failure exit, or an external kill: still a death
#endif
    if (disarmed)
        return QGA_HELPER_EXPECTED_DISARMED;
#ifndef QGA_LIFECYCLE_DEFECT_HELPERSYSKILLDEATH
    if (exited && exitCode == QGA_EXIT_SYSTEM_TERMINATED)
        return QGA_HELPER_EXPECTED_SYSKILL;
#else
    (void)exited; (void)exitCode;   /* the knob drops both facts: that IS the defect */
#endif
    return QGA_HELPER_DEATH;   // DEFECT knob: the system's own kill falls through to here and is written up as a death
}

// ---- THE GRADE ON A HELPER'S BOUNDED STOP WAIT (agent side: BrokerShutdown, NotifBridgeShutdown) ---------------
// On its way out the agent asks each resident helper to leave (the broker: its shutdown flag + ctl event; the bridge:
// its stop file), waits a bounded time on the helper's validated process handle, then deletes the helper's task -
// which ENDS an instance that did not leave. The wait is a failure detector: a helper that ignores its stop while
// the session is fine is an ERROR. Pure like the decisions above; the offline suite holds the rows.
//
// THE DEFECT THIS CLOSES (measured 2026-10-10 on win10-acc, agent instance 3936, 4.3.36.915):
//   10:49:13.652  QGAENDSESSION: Windows is ending this session -> HelpersDisarm -> the service acknowledges -> the
//                 agent leaves with QGA_EXIT_SESSION_END
//   10:49:14.141  HelperTaskDisarm, then "NOTIFBRIDGE stop file written"
//   10:49:17.166  ERROR "the bridge did not leave within 3 s of the stop file"
// and bridge.log for that instance stops at 10:49:13 with no stop-file line and no exit line: the stop file was
// written into a user-session process Windows was already tearing down, which was never in a position to act on
// it. Nothing was wrong, and an ERROR sent the next reader hunting. The bridge's own stop loop is correct for the
// normal case (stale-stop delete, directory watch, read at the loop top, delete on exit) and is not touched.
//   LEFT      the wait was satisfied: the helper left on our request. Nothing to say.
//   TEARDOWN  the wait EXPIRED while the session is ending: Windows ends the helper, not us. INFO naming the
//             teardown, with the facts (the wait result, the elapsed ms, the helper's pid, the flag).
//   EXPIRED   the wait expired with no teardown, or the wait itself FAILED: ERROR, exactly as before - the task
//             delete ends the helper. A failed wait (WAIT_FAILED: a bad handle) is a defect of ours and is never
//             excused by a teardown.
// The next start's restore sweep already owns the banner markers a bridge ended this way leaves; no recovery here.
// The knob QGA_LIFECYCLE_DEFECT_HELPERSTOPTEARDOWN restores the measured defect and must make the suite FAIL.
typedef enum _QGA_HELPER_STOP_OUTCOME
{
    QGA_HELPER_STOP_LEFT = 0,   // WAIT_OBJECT_0: the helper left on request
    QGA_HELPER_STOP_TEARDOWN,   // WAIT_TIMEOUT while the session is ending: INFO
    QGA_HELPER_STOP_EXPIRED     // WAIT_TIMEOUT with no teardown, or a failed wait: ERROR
} QGA_HELPER_STOP_OUTCOME;

static __inline QGA_HELPER_STOP_OUTCOME QgaHelperStopOutcome(IN BOOL sessionEnding, IN DWORD waitResult)
{
    if (waitResult == WAIT_OBJECT_0)
        return QGA_HELPER_STOP_LEFT;
#ifndef QGA_LIFECYCLE_DEFECT_HELPERSTOPTEARDOWN
    if (waitResult == WAIT_TIMEOUT && sessionEnding)
        return QGA_HELPER_STOP_TEARDOWN;
#else
    (void)sessionEnding;   /* the knob drops the one fact that tells a teardown from a miss: that IS the defect */
#endif
    return QGA_HELPER_STOP_EXPIRED;   // DEFECT knob: a teardown falls through to here and is written up as a miss
}

// ---- THE VERDICT ON A SESSION LOCK (agent side: the secure-desktop block of ProcessNewFrame) ------------------
// A locked guest is reported to dom0 at once and once per boot (owner 2026-10-09: "if it is locked it should be
// detected fast"; 2026-10-10: "if there IS locked guest we need to report it, ONCE, but NEVER assume there is as
// based on inaccurate measurements"). The fact is READ from Windows - WTSSessionInfoEx -> WTSINFOEX_LEVEL1.SessionFlags
// - and this is the pure part: the facts as read in, one of five arms out, so the branch is held offline.
//
// THE DEFECT THIS CLOSES (measured 2026-10-10 on win10-acc, 4.3.36.915, gui-agent-20261010.log:569-570 and
// :827-828): two agent instances, each ~2 s after its own start, on the FIRST secure frame it ever saw, elapsed on
// the secure desktop 0 s, no Default -> secure transition observed - "QGADESKSTUCK the session is LOCKED after 0 s on
// the secure desktop" and a dom0 notification from each. Owner, who saw it: "it was NEVER a locked guest. it was
// secure-desktop detected on display reattach before main desktop owns it." The predicate excluded one thing (a
// session that is ending) and had no notion of whether this agent had ever seen the user's desktop at all.
//
// THE GATE (Jev 2026-10-10, shell-seen-since-start 0.73 at confidence 0.68): no lock is asserted on the WTS
// reading alone until THIS agent instance has observed a shell window (GetShellWindow() non-NULL) at least once
// since it started - the phase gate the helpers' launch sites and the Mode-1 fullscreen guard already apply, and
// the question docs/ADR-uac.md's own flowchart asks first ("is the Windows shell up?").
//
// AND THE ABSENCE OF THAT WINDOW IS NOT THE ABSENCE OF A SESSION (the review's one objection, latch-never-set, 0.75
// of the mass). GetShellWindow() is per DESKTOP: the shell registers its window on Default, so a thread attached
// to the Winlogon desktop - which is where this agent sits throughout a secure episode - reads NULL even while
// explorer is alive on Default. A fresh agent whose first frame is secure therefore never latches, and on the
// window alone a guest that really IS locked at that moment would be graded PRE_SHELL and degrade to the 30 s
// path's 10-minute backstop - the "report it, ONCE" half of the owner's rule traded away for the "never assume"
// half, which he stated in one sentence. So when the reading says LOCK and no window was seen, the verdict turns
// on ONE desktop-independent fact: whether a shell PROCESS runs in the console session (main.c reads it from the
// same Toolhelp pass that finds LogonUI/consent, matched to the console session). Present -> a real lock met by
// a fresh agent, reported; absent -> PRE_SHELL; unreadable -> NO_FACT, said at WARNING - never silently "absent",
// never a lock. A user-name read would not do: WTSUserName is set at logon success, before any shell exists.
//   NO_FACT    a fact the verdict needs could not be read: the query failed, a level the union does not document,
//              a flags value that is neither LOCK nor UNLOCK, or - on the LOCK-without-window path - the process
//              list. Never a lock. WTS_SESSIONSTATE_LOCK is 0, so a ZEROED buffer reads as a lock unless the level
//              is checked - this arm is what stands between that and a dom0 notification.
//   TEARDOWN   the session is ending: a teardown, whatever the flags say (a console user is present, LogonUI is up).
//   UNLOCKED   read: WTS_SESSIONSTATE_UNLOCK. A fact, and not a lock (a logon in progress, a UAC prompt).
//   PRE_SHELL  read: LOCK, no shell window seen by this instance AND no shell process in the session: the session
//              has not reached a shell. LOGGED with its facts, never reported.
//   LOCKED     read: LOCK, no teardown, and a shell either SEEN (the window) or FOUND (the process). The one arm
//              that reports - at once, once per boot (the once is notifyerr.c's persisted marker, not this header's).
// `shellProcess` is read ONLY on the LOCK-without-window path and is UNREAD everywhere else (the earlier arms
// return first, so an UNREAD value is never consulted where it was not needed). `infoLevel` is WTSINFOEX.Level as
// read, 0 when nothing was read; `sessionFlags` likewise, UNKNOWN when nothing was.
// Two knobs, each the measured or the reviewed defect, each required to make the suite FAIL:
//   QGA_LIFECYCLE_DEFECT_PRESHELLLOCK     a LOCK reading is asserted with no shell seen or found (the measured one)
//   QGA_LIFECYCLE_DEFECT_SHELLPROCSILENT  an UNREADABLE process list is read as "no shell" (the silent-skip shape)
#define QGA_WTS_SESSIONSTATE_LOCK     0x00000000UL   // wtsapi32.h's WTS_SESSIONSTATE_LOCK, mirrored so this header
#define QGA_WTS_SESSIONSTATE_UNLOCK   0x00000001UL   // stays pure and the suite builds without that header;
#define QGA_WTS_SESSIONSTATE_UNKNOWN  0xFFFFFFFFUL   // main.c C_ASSERTs each against the real one
typedef enum _QGA_SHELL_PROCESS
{
    QGA_SHELLPROC_UNREAD = 0,   // not read: not needed on this path, or the process list could not be snapshotted
    QGA_SHELLPROC_ABSENT,       // the list was read: no shell process runs in the console session
    QGA_SHELLPROC_PRESENT       // the list was read: a shell process runs in the console session
} QGA_SHELL_PROCESS;
typedef enum _QGA_LOCK_VERDICT
{
    QGA_LOCK_NO_FACT = 0,
    QGA_LOCK_TEARDOWN,
    QGA_LOCK_UNLOCKED,
    QGA_LOCK_PRE_SHELL,
    QGA_LOCK_LOCKED
} QGA_LOCK_VERDICT;

static __inline QGA_LOCK_VERDICT QgaLockVerdict(IN BOOL shellSeen, IN QGA_SHELL_PROCESS shellProcess,
                                                IN BOOL sessionEnding, IN DWORD infoLevel, IN DWORD sessionFlags)
{
    if (sessionEnding)
        return QGA_LOCK_TEARDOWN;
    if (infoLevel != 1)
        return QGA_LOCK_NO_FACT;
    if (sessionFlags == QGA_WTS_SESSIONSTATE_UNLOCK)
        return QGA_LOCK_UNLOCKED;
    if (sessionFlags != QGA_WTS_SESSIONSTATE_LOCK)
        return QGA_LOCK_NO_FACT;
    if (shellSeen)
        return QGA_LOCK_LOCKED;        // the window was seen on Default by this instance: a lock after a shell
#ifdef QGA_LIFECYCLE_DEFECT_PRESHELLLOCK
    (void)shellProcess;   /* the knob asserts a lock on a desktop this agent cannot place: the measured defect */
    return QGA_LOCK_LOCKED;
#else
    if (shellProcess == QGA_SHELLPROC_PRESENT)
        return QGA_LOCK_LOCKED;        // no window from the Winlogon desktop, but the session HAS a shell: a real lock
    if (shellProcess == QGA_SHELLPROC_ABSENT)
        return QGA_LOCK_PRE_SHELL;     // the session has not reached a shell: said, not reported
#ifdef QGA_LIFECYCLE_DEFECT_SHELLPROCSILENT
    return QGA_LOCK_PRE_SHELL;         /* the knob reads an unreadable list as "no shell": the silent-skip defect */
#else
    return QGA_LOCK_NO_FACT;           // the one fact that would decide could not be read: said at WARNING
#endif
#endif
}

// ---- THE AGENT'S OWN READING OF ITS EXIT CODE (WinMain) ------------------------------------------------------
// An expected exit - requested, session end, reconnect, no GUI domain - is logged at INFO naming the reason; anything
// else is a failure, logged at ERROR with the code that caused it. "WinMain: WatchForEvents failed with error 0xb7"
// (and "error 0x0: The operation completed successfully") at ERROR on a requested stop was a stale GetLastError
// (measured 2026-10-07). Held by watchdog/lifecycle_test.c; the knob QGA_LIFECYCLE_DEFECT_REQUESTEDERROR makes an
// expected exit read as a failure and must make that suite fail.
static __inline BOOL QgaExitIsExpected(IN DWORD code)
{
#ifdef QGA_LIFECYCLE_DEFECT_REQUESTEDERROR
    return code == QGA_EXIT_NO_GUI_DOMAIN;   // DEFECT: a requested, session-end or reconnect exit reads as a failure
#else
    return code == QGA_EXIT_REQUESTED || code == QGA_EXIT_SESSION_END || code == QGA_EXIT_RECONNECT ||
           code == QGA_EXIT_NO_GUI_DOMAIN;
#endif
}
static __inline const WCHAR *QgaExitReasonName(IN DWORD code)
{
    switch (code)
    {
    case QGA_EXIT_REQUESTED:         return L"requested through Global\\QGA_SHUTDOWN";
    case QGA_EXIT_SESSION_END:       return L"the session is ending (WM_ENDSESSION)";
    case QGA_EXIT_RECONNECT:         return L"reconnect: dom0's gui-daemon went away or never came";
    case QGA_EXIT_NO_GUI_DOMAIN:     return L"no GUI domain this boot";
    case QGA_EXIT_SYSTEM_TERMINATED: return L"ended by the system (DBG_TERMINATE_PROCESS)";
    default:                         return L"a failure (a Win32 error, or a code no path defines)";
    }
}

/* ---------------------------------------------------------------------------------------------
 * IS THE "DE-SLICE BROKER DOWN" REPORT DUE?  (QGADESLICEDOWN, and the dom0 toast that rides it)
 *
 * THE DEFECT THIS EXISTS TO END, measured 2026-10-11 on win11-app, two cold boots:
 *     00:25:43.341  agent starts, System uptime: 46.250 seconds
 *     00:25:43.811  QGADESLICEDOWN "not running for 46 s" -> toast sent to dom0 (#1 this boot)
 *     00:25:43-44   the same line 29 more times, 28 of them "not sent: suppressed:duplicate"
 *     00:25:59.623  QGAHELPERTASK Qubes-WgcBroker registered and started  <- 16 s AFTER the complaint
 * The broker had not failed; it had not been launched yet. 8168655 (2026-10-08) added a hold for
 * exactly that window - it zeroes both the down-clock and the next-warn tick - but the firing test
 * was `now >= g_BrokerNextWarn`, and zero is in the past, so the hold GUARANTEED the report instead
 * of deferring it. The duration printed was `now - 0`: the system uptime, not an outage. Then the
 * hold re-zeroed on the next pass, so hold and fire ping-ponged and the loop stayed awake through it.
 * The dom0 text was "The notification and menu capture helper is not running ... has not run for over
 * 30 s", which was false on every count, and it also wrote DesliceBrokerDown=1 - the flag
 * health-check and acceptance FAIL on - onto healthy guests.
 *
 * A ZEROED CLOCK MEANS NOT ARMED, NEVER "DUE NOW". A broker that genuinely never becomes ready is
 * still reported: the clock is armed at the launch (g_WgcLastLaunch), so its duration is true, and a
 * guest with no shell at all is QGADESKSTUCK's condition, not this one.
 */
static __inline BOOL QgaDesliceWarnDue(IN unsigned long long downSince, IN unsigned long long nextWarn,
                                       IN unsigned long long now)
{
#ifdef QGA_LIFECYCLE_DEFECT_ZERODUE
    (void)downSince;     /* DEFECT: a zeroed clock reads as "due now" - the false toast per cold boot */
#else
    if (downSince == 0 || nextWarn == 0)
        return FALSE;    /* held: the broker could not have been launched yet - nothing to report */
#endif
    return (BOOL)(now >= nextWarn);
}
/* ---------------------------------------------------------------------------------------------
 * IS THIS RUNNING IMAGE THE SESSION'S SHELL?
 *
 * Owner, 2026-10-11: "can you just match 'it is our shell', not by the name?" The shell fact used to
 * compare against the literal explorer.exe, which is Winlogon's DEFAULT Shell value and not a fact
 * about this guest: a kiosk guest (Shell Launcher / assigned access sets Winlogon\Shell to a single
 * app) runs a shell that is not explorer, read as ABSENT, and the window-hold report would then blame
 * a missing desktop for a real display fault. Open-Shell is NOT this case - it only adds a Start
 * menu and explorer keeps running.
 *
 * So the test is "the image is the CONFIGURED shell, or it is explorer". Both, deliberately: the
 * configured value can be empty, unreadable, or carry arguments and a quoted path, and on a stock
 * guest explorer IS the shell - so the default must keep matching rather than depending on a registry
 * read succeeding. A failed read therefore narrows nothing and can never mute a report.
 *
 * Case-insensitive the portable way: Windows file names are case-insensitive, and this header is
 * compiled by MSVC for the agent and by gcc for the offline suite, where _wcsicmp does not exist.
 */
static __inline int QgaWcsIEqAscii(IN const wchar_t *a, IN const wchar_t *b)
{
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++)
    {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb - L'A' + L'a');
        if (ca != cb)
            return 0;
    }
    return *a == 0 && *b == 0;
}

/* `image` is a running process's image name (no path); `configured` is the basename of the session's
 * Winlogon\Shell value, or NULL/empty when it could not be read. */
static __inline BOOL QgaIsSessionShellImage(IN const wchar_t *image, IN const wchar_t *configured)
{
#ifndef QGA_LIFECYCLE_DEFECT_SHELLBYNAME
    if (configured && configured[0] && QgaWcsIEqAscii(image, configured))
        return TRUE;                       /* the shell this guest is CONFIGURED to run */
#else
    (void)configured;   /* DEFECT: only the literal default counts, so a kiosk guest reads ABSENT */
#endif
    return (BOOL)QgaWcsIEqAscii(image, L"explorer.exe");   /* the default shell, always still a shell */
}
