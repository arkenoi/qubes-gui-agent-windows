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
#include <wchar.h>   // wcscmp: QgaHoldReasonIsNormal compares a hold reason
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
/* ---------------------------------------------------------------------------------------------
 * WHERE THE DOWN-CLOCK IS ANCHORED WHEN IT RE-ARMS.
 *
 * The clock is cleared the moment the broker validates ready, and armed again on the first pass
 * that finds it not alive. The question this answers is what tick that second arming counts from.
 *
 * THE DEFECT, MEASURED 2026-10-23 on win11r-sup running 4.3.37.1066, from the agent's own log:
 *     20:57:25  WgcLaunch: WGCBROKER launched via Task Scheduler, user session 1
 *     20:57:39  BrokerSupervise: WGCBROKER ready (pid 3000 validated)
 *     21:00:52.734  QGABROKEREXIT de-slice broker process EXITED - detected by process wait
 *     21:00:52.763  QGADESLICEDOWN "not running for 210 s"      <- 29 ms after the detection
 *     21:02:23  WGCBROKER ready (pid 8124 validated)            <- the relaunch SUCCEEDS
 * The broker had been serving for 193 of those "210 s". The arming branch anchored at
 * g_WgcLastLaunch, which does not slide (one launch per agent life, asserted at its assignment
 * site), so after ANY healthy period it re-armed at a tick minutes in the past and nextWarn was
 * ALREADY EXPIRED. The 30 s grace exists precisely so a relaunch can end the outage without
 * bothering the user, and it was skipped entirely - the complaint fired in the same supervise pass
 * as the detection, printing a duration that counted the time the broker was healthy.
 * With this anchor the report would have been due at 21:01:22, and the relaunch at 21:02:23 would
 * have cleared the clock first, so nothing would have been sent.
 *
 * NOT A REGRESSION FROM A REVERT, checked: the version reverted in 21aec24 anchored on
 * `g_WgcLastLaunch ? g_WgcLastLaunch : now` as well. The staleness is older than both.
 *
 * THE LAUNCH ANCHOR IS STILL RIGHT FOR THE CASE IT WAS ADDED FOR. A broker that is launched and
 * NEVER becomes ready has no healthy period to exclude, so its outage genuinely starts at the
 * launch (measured 2026-10-08: the shell took 27 s, the complaint fired 1 s before ready). That is
 * what `readyAt == 0` distinguishes - g_BrokerReadyAt is set once, when an instance validates, and
 * is never cleared, so non-zero means "this agent has had a working broker".
 * Jev: sound 0.84.
 */
static __inline unsigned long long QgaDesliceAnchor(IN int launched, IN unsigned long long lastLaunch,
                                                    IN unsigned long long readyAt, IN unsigned long long now)
{
#ifdef QGA_LIFECYCLE_DEFECT_STALEANCHOR
    (void)readyAt;   /* DEFECT: re-arm at the original launch even after a healthy run - fires instantly */
    return (launched && lastLaunch) ? lastLaunch : now;
#else
    if (launched && lastLaunch && readyAt == 0)
        return lastLaunch;   /* launched and never ready: the outage does start at the launch */
    return now;              /* it was serving, so THIS outage starts now */
#endif
}
/* ---------------------------------------------------------------------------------------------
 * A GRACEFUL STAND-DOWN IS ULTIMATE UNTIL SOMETHING STARTS IT AGAIN.
 *
 * Owner, 2026-10-24: "the proper fix is when the broker is told to go down, it stays down, no
 * timer, no supervisor until next rearm" and "of course graceful shutdown should be ultimate until
 * something starts it".
 *
 * WHAT WAS WRONG. BrokerSupervise already recognises a deliberate stand-down - QgaDecideHelperExit
 * returns QGA_HELPER_EXPECTED_DISARMED, the caller logs "not a death" and returns - but it returned
 * only from THAT pass. On the next wake g_BrokerReady is already 0, so the whole was-ready branch is
 * skipped, control reaches `else if (!g_BrokerDownSince)`, the clock arms, and the complaint begins
 * against a broker nobody wants running. QgaDesliceTeardownExcused did not cover it either: that
 * holds while the session is ending or helpers are disarmed, and a broker stopped on purpose while
 * the session continues is neither.
 *
 * So the stand-down has to be a LATCH, not a per-pass decision. While it is held there is no clock,
 * no deadline, no re-warn and no dom0 row - supervision is not running at all. It clears on exactly
 * one event: a new launch being issued. That is the "until next rearm" in the owner's words, and it
 * is the only thing that can clear it - not a timer, not the shell appearing, not the broker being
 * seen absent again.
 *
 * WHY A LATCH IS SAFE HERE. A broker that was never stood down is unaffected: stoodDown is 0 and
 * this predicate holds nothing. A broker that genuinely dies is unaffected: QgaDecideHelperExit
 * returns QGA_HELPER_DEATH, the latch is never set, and the death report and the de-slice clock both
 * run as before. The only condition it silences is the one the agent itself asked for, which is the
 * condition that was never a fault.
 *
 * IT IS NOT A MUTE. The caller still logs the stand-down, and it still clears the DesliceBrokerDown
 * config flag that health-check and acceptance read - a flag left at 1 over a deliberate stop is a
 * guest that reads as broken for ever. What goes away is a complaint about an absence that was
 * requested, not the record of it.
 */
static __inline int QgaDesliceSupervisionHeld(IN int stoodDown, IN unsigned long long downSince,
                                              IN unsigned long long now)
{
#ifdef QGA_LIFECYCLE_DEFECT_STANDDOWNEXPIRES
    /* DEFECT: the hold lapses once the first warn interval has passed, so the complaint comes back
     * on its own - which is the shape measured BEFORE the latch, where the stand-down held only for
     * the pass that detected it. */
    return (stoodDown && (downSince == 0 || (now - downSince) < 30000ULL)) ? 1 : 0;
#else
    /* TIME CANNOT END A STAND-DOWN, and taking no account of it is the whole decision: the inputs
     * are accepted and deliberately unused so that a future reader cannot reintroduce a deadline
     * here without deleting these lines. Only an event clears the latch, in the caller - a launch
     * being issued, or the broker being seen alive - never the clock. */
    (void)downSince; (void)now;
    return stoodDown ? 1 : 0;
#endif
}
/* ---------------------------------------------------------------------------------------------
 * ONE SITUATION IS ONE DOM0 NOTIFICATION.
 *
 * When the broker DIED, the death reporter already tells dom0 - by name, with the pid and the exit
 * code - and that row is the actionable one. The de-slice complaint then says the same thing worse
 * ("has not been running for N s"), and the owner's standing rule is "no doubles, nothing lost": a
 * double is P1.
 *
 * MEASURED 2026-10-23, one injected broker death producing TWO dom0 rows two seconds apart:
 *     21:00:52.973  PlatLog: NOTIFYERR gui-agent.deslice-down sent to dom0 (#1 this boot)
 *     21:01:01      qwt-deaths.log: DEATH #1 notified to dom0 as wgcbroker.death-1
 * The owner saw both and asked whether it was expected. Jev, given the two rows and this file's own
 * secure-desktop precedent: suppress the de-slice complaint, confidence 1.00.
 *
 * THIS SUPPRESSES THE NOTIFICATION ONLY. The caller still writes the DesliceBrokerDown config flag
 * that health-check and acceptance read, and still logs QGADESLICEDOWN at ERROR, so the condition
 * stays visible to every instrument - what goes away is the second toast, not the evidence.
 * Same shape as the secure-desktop hold beside it, where QGADESKSTUCK owns the condition.
 */
/* ---------------------------------------------------------------------------------------------
 * A GRACEFUL SHUTDOWN IS NOT A FAULT, AND MUST NOT EMIT AN ERROR.
 *
 * Owner, 2026-10-23: "graceful shutdown should NOT emit error". This file already states the
 * contract at HelpersDisarm(): "once set, no helper is launched into this session any more and a
 * helper's exit while it is set is EXPECTED (the session's teardown, or our own stop) - INFO, not a
 * death". The de-slice report never asked it.
 *
 * MEASURED 2026-10-23 on win11r-sup (4.3.37.1066):
 *     21:00:52.763  QGADESLICEDOWN ... at ERROR, and DesliceBrokerDown=1 written
 *     21:00:52.973  NOTIFYERR gui-agent.deslice-down sent to dom0
 *     21:01:01.766  ControlHandlerEx: preshutdown - the agent will not be restarted from here on
 * i.e. an ERROR, a config flag that health-check and acceptance FAIL on, and a client-visible toast,
 * nine seconds before the machine went down on purpose. The helper was gone because the guest was
 * going away.
 *
 * ALL THREE ARE SKIPPED when this is true, not just the toast: the ERROR line (the owner's words),
 * the DesliceBrokerDown flag (a false 1 on a healthy guest is what makes health-check fail), and the
 * dom0 row. Outside a teardown nothing changes - a real outage on a running guest still reports at
 * ERROR with its flag and its row.
 */
static __inline BOOL QgaDesliceTeardownExcused(IN int helpersDisarmed, IN int sessionEnding)
{
#ifdef QGA_LIFECYCLE_DEFECT_SHUTDOWNERROR
    (void)helpersDisarmed; (void)sessionEnding;
    return FALSE;    /* DEFECT: an ERROR, a flag and a toast while the guest is shutting down */
#else
    return (BOOL)(helpersDisarmed || sessionEnding);
#endif
}
static __inline BOOL QgaDesliceOwnedByDeath(IN unsigned long long diedAt)
{
#ifdef QGA_LIFECYCLE_DEFECT_DOUBLETOAST
    (void)diedAt;
    return FALSE;    /* DEFECT: complain AS WELL AS reporting the death - two toasts for one event */
#else
    return (BOOL)(diedAt != 0);   /* a death is being reported for this outage; that row owns it */
#endif
}
/* The first deadline after a death: the scheduler's whole restart budget while it still owes one, else the usual grace. */
static __inline unsigned long long QgaDesliceDeathGraceMs(IN int restartOwed, IN unsigned long deaths,
                                                          IN unsigned long restartCount,
                                                          IN unsigned long long budgetMs,
                                                          IN unsigned long long firstWarnMs)
{
    return (restartOwed && deaths <= restartCount) ? budgetMs : firstWarnMs;
}
/* A broker start seen inside the window re-anchors the deadline there; downSince is not moved, so the reported duration stays true. */
static __inline unsigned long long QgaDesliceRestartRearm(IN int armed, IN unsigned long long nextWarn,
                                                          IN unsigned long long restartTick,
                                                          IN unsigned long long firstWarnMs)
{
    return armed ? (restartTick + firstWarnMs) : nextWarn;
}
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

/* ---------------------------------------------------------------------------------------------
 * A HOLD WHILE THE BROKER IS STILL STARTING IS NOT A FAULT.
 *
 * Owner, 2026-10-11, on what acceptance means: "acceptance criteria includes absence of
 * notification noise", and his standing rule from 2026-10-08: "no warnings on normal operation.
 * warning means something is not quite normal, yet workable."
 *
 * MEASURED on win11-app, the boot at 03:43:55, with a window opened 3.3 s before the broker was up:
 *   03:44:06.737  W  BROKERHOLD hwnd 0x2002a holding last content ... brokerActive=0 slot=-1
 *   03:44:07.700  W  BROKERHOLD hwnd 0x2002a ... brokerActive=0 slot=-1
 *   03:44:10.526  I  WGCBROKER ready
 *   03:44:10.576  W  BROKERLATEREG hwnd 0x2002a ... withheld until now
 * Three WARNINGs for the designed path. The window was held correctly and delivered with pixels -
 * nothing went wrong - but the log reports every cold boot as something to investigate, and those
 * lines are counted as ours by the sweep.
 *
 * The emitter's own comment carries the wrong premise: "brokerActive=0 is always the latter [a feed
 * loss to investigate]". It is not. The broker's one launch waits for a shell, so on EVERY cold boot
 * there is a window - measured 10.8-11.6 s on an AppVM, 1.4-4.3 s on a StandaloneVM - in which
 * brokerActive=0 means "it has not started yet", which is normal. A broker that WAS ready and went
 * away is the fault, and the agent already distinguishes them.
 */
static __inline BOOL QgaHoldIsStartup(IN BOOL brokerActive, IN BOOL brokerEverReadyThisAgent)
{
    if (brokerActive)
        return FALSE;                       /* not a hold for want of a broker at all */
#ifndef QGA_LIFECYCLE_DEFECT_HOLDALWAYSLOUD
    return (BOOL)(!brokerEverReadyThisAgent);   /* never ready yet = still starting = normal */
#else
    (void)brokerEverReadyThisAgent;         /* DEFECT: every hold is a fault, so normal boots shout */
    return FALSE;
#endif
}

/* ---------------------------------------------------------------------------------------------
 * WHICH WINDOW GETS THE ESCAPE THAT CLOSES THE HIDDEN START MENU?
 *
 * MEASURED 2026-10-17 on win11de-sm (German 25H2 26200) with 4.3.36+agent.0824ac03e2e5 installed and
 * verified: the hidden Start menu opened TWICE and our Escape did not close it either time -
 *   120650.185 I QGASTARTDISMISS 0x10180: ... posted Escape TO THAT WINDOW ... (foreground pid 2920)
 *   120651.216 W QGASTARTDISMISS 0x10180: the hidden Start menu is STILL open 1031 ms after Escape
 *                (foreground pid 2920) - clicks go to a menu the user cannot see
 * and again at 120653.978 / 120655.006. That is the field report this dismissal was written for
 * (GWeck #172), reproducing on the current build; the owner had already retracted a claim that
 * 4.3.35 fixed it. Jev: P1-class 0.88.
 *
 * WHY IT FAILED. The agent's own tracking line in the same run reads
 *   AttachToInputDesktop: desktop: 0x1000c, start: 0x10180, search: 0x0
 * so its `search` handle is 0x0 - never found - while the window that is actually visible,
 * WS_EX_TOPMOST and owned by SearchHost.exe is 0x1018a ('Suche', 800x750, parent 0x1000c). The post
 * went to 0x10180. The GUARD was right and the TARGET was wrong: StartOrSearchHoldsForeground asks
 * only what KIND OF PROCESS holds the foreground, so it correctly reported Search (pid 2920), and
 * the Escape was then posted to a different window of the same hierarchy. PostMessage returned TRUE
 * both times - the Info line is written only when both posts succeed - so nothing failed loudly;
 * the keystroke simply went to a queue whose window does not have the input. Jev: the cause is the
 * wrong target rather than a CoreWindow ignoring a posted key, 0.87.
 *
 * AND THE DELIVERY STAYS TARGETED. This must not become a global SendInput again: that injects into
 * the SYSTEM input queue and was measured closing Open-Shell's OWN menu (3 of 57 Win presses on
 * 4.3.35, 6-15 ms after it took the foreground, against 0 of 36 on 4.3.34; Jev race_is_real 0.96).
 * The emitter's comment is explicit - "there is deliberately NO fallback to a global inject, because
 * a fallback here is the bug" - so this only ever chooses between TWO WINDOWS, never a broadcast.
 * Jev on the direction: post to the focused window 0.99, measure-first 0.00.
 *
 * THE CHOICE IS PURE, so it is decided here and the suite holds both arms: the focused window when
 * the foreground thread has one, and otherwise the tracked Start surface - which is what the agent
 * did before and is no worse than before when focus cannot be read. NEVER NULL, because posting to
 * NULL is a broadcast to every top-level window, i.e. exactly the global inject this forbids.
 */
/* THE KEYSTROKE WAS THE WRONG MECHANISM, and this project's own code said so before it was written.
 *
 * main.c:7832, comment dated 2026-10-04: "A posted WM_KEYDOWN Escape to that window does NOT close
 * Start; a real Escape keystroke does (measured)". The change from SendInput to PostMessage - made
 * to stop a global inject closing Open-Shell's menu - kept the TARGETING property and dropped the
 * only mechanism ever measured to work. A later fix then repaired the TARGET of that posted Escape
 * (it had gone to the tracked surface while SearchHost's window held the input) and was REFUTED on
 * the guest the same day: the Escape lands on the focused window now, and the menu is still open
 * 1000 and 1063 ms later, twice. Targeting was never the defect. So: no keystroke is posted, by
 * this predicate or anywhere below it, and QgaStartDismissTarget - which chose between two windows
 * for a message that cannot work - is gone with its knob.
 *
 * WHAT CLOSES IT INSTEAD. Start is a LIGHT-DISMISS surface: it closes when something else takes
 * activation, which is what a click on another window does. That is Windows' own close path, so the
 * Start app's own state stays consistent - unlike hiding the window (keyboard focus stays in an
 * invisible search box) or closing it (the shell's Start component is destroyed). The agent
 * therefore returns activation to the window dom0 says the user is in, and the guest's shell
 * desktop only when dom0 names none.
 *
 * AND THE CLOSURE IS OBSERVED, ONCE, WITHIN A BOUND - which is the part that was missing. Before
 * this, the two ways an opening ended were both SILENT: ShouldAcceptWindow clearing the tracked
 * handle when the surface was seen cloaked, and the re-arm clearing the flag when the foreground
 * left Start. Nothing ever said "it closed", so a success and a never-attempted dismissal read
 * identically in a log, and only the FAILURE was ever written down.
 *
 * WHY A PREDICATE: eight outcomes over six facts, three of them racing (the surface's visibility,
 * where the foreground sits, whether dom0's window still exists), and the suite has to be able to
 * drive every row including the ones a live guest produces rarely. The settle time is a PARAMETER
 * rather than a constant read inside, so a test can assert the BOUND rather than the literal.
 */
typedef enum {
    QGA_START_LOOK_AGAIN = 0,       /* Start is up but input has not moved to it yet - a later event looks again */
    QGA_START_RETURN_FOCUS,         /* hand activation back to the window dom0 named */
    QGA_START_RETURN_DESKTOP,       /* dom0 named none: the shell's desktop window */
    QGA_START_NO_TARGET,            /* nothing to activate - ERROR, and nothing is touched */
    QGA_START_CLOSING,              /* activation returned, surface not gone yet, still inside the bound */
    QGA_START_CLOSED_CLOAKED,       /* observed closed: the surface itself is no longer visible */
    QGA_START_CLOSED_FG_LEFT,       /* observed closed: the foreground is no longer Start/Search */
    QGA_START_STUCK                 /* past the bound and still holding input - clicks go to a hidden menu */
} QGA_START_VERDICT;

static __inline QGA_START_VERDICT QgaHiddenStartVerdict(
    IN BOOL attempted,              /* activation already returned in THIS opening */
    IN BOOL surfaceVisible,         /* the tracked Start surface is still visible */
    IN BOOL fgIsStartOrSearch,      /* StartMenuExperienceHost or SearchHost holds the foreground */
    IN BOOL dom0FocusAlive,         /* dom0 named a window and it still exists */
    IN BOOL shellWindowPresent,     /* the shell's desktop window exists (it does not at logon/shutdown) */
    IN unsigned long long sinceAttemptMs,
    IN unsigned long long settleMs)
{
    if (attempted)
    {
        /* CLOAKED BEATS FOREGROUND-LEFT when both hold: the surface being gone is the stronger
         * statement, and it is the one the user sees. */
#ifndef QGA_LIFECYCLE_DEFECT_STARTCLOSEUNSAID
        if (!surfaceVisible)
            return QGA_START_CLOSED_CLOAKED;
        if (!fgIsStartOrSearch)
            return QGA_START_CLOSED_FG_LEFT;
#else
        if (!surfaceVisible || !fgIsStartOrSearch)
            return QGA_START_LOOK_AGAIN;    /* DEFECT: both closures stay silent and re-arm, as before -
                                             * a dismissal that worked is indistinguishable from one that
                                             * never ran */
#endif
        if (sinceAttemptMs < settleMs)
            return QGA_START_CLOSING;
#ifndef QGA_LIFECYCLE_DEFECT_STARTNEVERSTUCK
        return QGA_START_STUCK;
#else
        return QGA_START_CLOSING;           /* DEFECT: never stuck, so a menu that stayed open past the
                                             * bound is reported as still closing, for ever */
#endif
    }

    /* 25H2 moves the foreground to Start's search box on its own schedule - measured up to 480 ms
     * after the surface appears, sometimes after that opening's last window event. Not yet is not a
     * failure: EVENT_SYSTEM_FOREGROUND brings us back. */
    if (!fgIsStartOrSearch)
        return QGA_START_LOOK_AGAIN;

    if (dom0FocusAlive)
        return QGA_START_RETURN_FOCUS;
#ifndef QGA_LIFECYCLE_DEFECT_STARTRETURNNULL
    if (shellWindowPresent)
        return QGA_START_RETURN_DESKTOP;
    return QGA_START_NO_TARGET;             /* say so and touch nothing: an activation of NULL is not a
                                             * request, and at logon/shutdown there is no shell window */
#else
    (void)shellWindowPresent;               /* DEFECT: activate the shell desktop even when there is
                                             * none, i.e. activate NULL */
    return QGA_START_RETURN_DESKTOP;
#endif
}

/* ---------------------------------------------------------------------------------------------
 * IS THE HOST'S MENU KEY FORWARDED?  (and the reason a key may not summon the unclosable)
 *
 * The Super/Win key is dropped in seamless mode by default (BlockMenuKey), and dom0 can ask for it
 * to pass (service.enableWinKey). That override exists because a THIRD-PARTY Start replacement
 * wants that key - it is that menu's key, not ours.
 *
 * BUT A KEY MUST NEVER SUMMON A SURFACE WE REFUSE TO SHOW AND CANNOT CLOSE. With SeamlessStart=0
 * the native Start is not presented, and MEASURED on win11de-sm 2026-10-17 the agent cannot close
 * it: a posted Escape leaves it open (1000 and 1063 ms), and returning activation is REFUSED by the
 * foreground lock even with an input nudge that injects cleanly - the surface closes ~2 s later
 * because the foreground leaves, not because of us. A real injected Escape does close it and is
 * refused on different grounds: it rides the SYSTEM input queue and is delivered on dequeue, which
 * is a race, and this project does not ship races.
 * So in that cell the key is dropped and the surface never opens. Suppression then holds BY
 * CONSTRUCTION rather than by a mechanism that has to win something.
 *
 * AND IT IS CONDITIONAL, because dropping it unconditionally would silently break the user's own
 * menu. When a replacement IS present the key is forwarded - the surface it summons is that menu's,
 * which we present like any other window. Jev on the policy: conditional-on-a-replacement 0.99.
 *
 * HOW THE REPLACEMENT IS DETECTED, and why the two obvious bases are wrong. It is NOT the
 * registered shell: a replacement runs ALONGSIDE the default shell, so HKLM Winlogon\Shell still
 * reads explorer.exe with one installed. It is NOT its menu window class either: that window exists
 * only while the menu is OPEN, which is after the key this predicate is deciding about. So the fact
 * is its presence as an installed, running process in the console session - the only thing knowable
 * before the menu exists (Jev: installed-and-running-process 0.98, registered-shell 0.00,
 * menu-window-class 0.00). It is LATCHED when first seen and re-checked on a coarse trigger, never
 * per key event (Jev 1.00). And never by caption: a caption lookup for L"Search" read 0x0 on this
 * German guest while the window's caption was 'Suche', and that bug shipped for months.
 *
 * THE UNKNOWN CASE IS NOT A COIN FLIP. A false negative drops the key and the user's own Start menu
 * stops answering it, silently, where the agent cannot see the user pressing it to no effect. A
 * false positive forwards the key and a suppressed surface holds focus for ~2 s - which the
 * examination path REPORTS, loudly. So the asymmetry decides it: forward unless we positively know
 * no replacement is there (Jev: a silent dead key is worse, 0.74).
 *
 * WHEN IT CANNOT BE DECIDED, THE KEY IS FORWARDED (Jev 0.65). Silently breaking the user's own Start
 * menu is the worse failure and the one we cannot see; a suppressed surface holding focus is loud,
 * because the examination path reports it.
 */
/* Is this process image a third-party START REPLACEMENT - the thing that wants the host's menu key?
 * A NAME LIST, and its limits stated: these products are identified by their own executable because
 * nothing else is knowable before their menu exists (see QgaMenuKeyPolicy). A product not listed
 * here reads as ABSENT, which would drop the key on a guest that has one - so the caller must treat
 * "none found" as decisive only when the enumeration itself SUCCEEDED, and must forward the key
 * whenever it could not look. Extend the list rather than widen the matching. */
static __inline BOOL QgaIsStartReplacementImage(IN const wchar_t *image)
{
    if (!image || !image[0])
        return FALSE;
    return (BOOL)(QgaWcsIEqAscii(image, L"StartMenu.exe")          /* Open-Shell, Classic Shell */
               || QgaWcsIEqAscii(image, L"ClassicStartMenu.exe")    /* Classic Shell, older */
               || QgaWcsIEqAscii(image, L"StartIsBack.exe")
               || QgaWcsIEqAscii(image, L"StartIsBackCfg.exe")
               || QgaWcsIEqAscii(image, L"Start11.exe"));
}

typedef enum {
    QGA_MENUKEY_FORWARD = 0,        /* pass it on */
    QGA_MENUKEY_DROP_BLOCKED,       /* BlockMenuKey is on and dom0 did not override it */
    QGA_MENUKEY_DROP_UNCLOSABLE     /* dom0 asked for it, but it would summon the unclosable */
} QGA_MENUKEY_POLICY;

static __inline QGA_MENUKEY_POLICY QgaMenuKeyPolicy(
    IN BOOL seamless,
    IN BOOL blockMenuKey,          /* the effective setting after any dom0 override */
    IN BOOL seamlessStart,         /* 1 = the native Start IS presented when it opens */
    IN BOOL replacementKnown,      /* could we decide whether a Start replacement is present? */
    IN BOOL replacementPresent)
{
    if (!seamless)
        return QGA_MENUKEY_FORWARD;         /* fullscreen always passes the key */
    if (blockMenuKey)
        return QGA_MENUKEY_DROP_BLOCKED;    /* the existing rule, unchanged */
    if (seamlessStart)
        return QGA_MENUKEY_FORWARD;         /* we present what it summons */
#ifndef QGA_LIFECYCLE_DEFECT_MENUKEYALWAYSFORWARD
    if (replacementKnown && !replacementPresent)
        return QGA_MENUKEY_DROP_UNCLOSABLE; /* nothing may summon a surface we refuse and cannot close */
#else
    (void)replacementKnown; (void)replacementPresent;   /* DEFECT: forward regardless, so the key
                                             * summons a hidden Start that holds the user's focus */
#endif
    return QGA_MENUKEY_FORWARD;             /* a replacement is present, or we could not tell */
}

/* ---------------------------------------------------------------------------------------------
 * IS THIS XENCONTROL LINE WORTH KEEPING?  (the per-notify Debug line, x118 in one run)
 *
 * xencontrol.dll logs through OUR callback (capture.c's XcLogger) at whatever level we hand it, and
 * one of its lines is per-event-channel-notify: "XcEvtchnNotify: LocalPort: 19". MEASURED on the
 * 2026-10-17 Start run: 118 of them, identical, every one at Debug. That is a line per vchan
 * notification - it says the transport worked, which is the definition of normal operation.
 *
 * DROPPED AT THE CALLBACK rather than by capping xencontrol's level: the level cap would also hide
 * its grant and map Debug lines, which a grant-lifetime diagnosis needs. Exactly one (level,
 * function) pair is dropped, and that function at WARNING or above still comes through - if the
 * notify path starts failing, the failure is not suppressed with the heartbeat.
 *
 * No summary line replaces it: the frame heartbeat is already counted in QGAPERF.
 */
static __inline BOOL QgaXcLineKept(IN int level, IN const char* function)
{
    if (!function)
        return TRUE;                        /* no function name: never drop what cannot be identified */
#ifndef QGA_LIFECYCLE_DEFECT_XCNOTIFYKEPT
    /* 4 == LOG_LEVEL_DEBUG (windows-utils log.h); xencontrol's levels are the same as ours. */
    if (level == 4 && strcmp(function, "XcEvtchnNotify") == 0)
        return FALSE;
#else
    (void)level; (void)function;            /* DEFECT: the per-notify Debug line is kept - 118
                                             * identical lines in one run saying the transport works */
#endif
    return TRUE;
}

/* ---------------------------------------------------------------------------------------------
 * IS THIS SHELL SURFACE PRESENTED, AND IF NOT, WHY NOT?  (the settings matrix, as one predicate)
 *
 * Four shipped configurations crossed with the surface kinds, decided in one place so the suite can
 * drive every cell. The axis is SeamlessStart: 0 (the default) means a native Start that opens is
 * not shown, 1 means work on it is enabled and everything is presented.
 *
 * THE COMPANION IS THE HALF THAT WAS MISSING. A Start opening on 25H2 brings up TWO surfaces: the
 * StartMenuExperienceHost one, and a SearchHost one ('Suche' on a German guest, WS_EX_TOPMOST,
 * 800x750) that carries the search box. Refusing only the first leaves the second mapped, so a
 * menu declared "not presented" still puts a slice of itself on dom0's screen.
 *
 * AND THE STOCK RULE FOR THIS CANNOT FIRE OUTSIDE ENGLISH. main.c had
 *     if (g_StartVisible && data->Handle == g_SearchWindow)
 * where g_SearchWindow comes from util.c's FindWindow(L"Windows.UI.Core.CoreWindow", L"Search").
 * MEASURED on the German 25H2 guest, from the agent's own line: "start: 0x10180, search: 0x0",
 * while the window's caption is 'Suche'. A caption lookup in one language is 0x0 everywhere else,
 * and data->Handle is never 0x0, so that rule has never once fired on the configuration the defect
 * was reported from. It is deleted, not fixed: captions are banned as identity in this project.
 *
 * THE DISCRIMINATOR IS hiddenStartOpen, and it is the only fact that separates the two cases a
 * SearchHost window can be in - the companion of a Start opening, or a standalone Win+S Search the
 * user asked for and must get. Measured 2026-10-04 (main.c:7835): 0 Start-surface examinations in
 * 2 Win+S openings, against 2 per Shift+Win opening. Nothing else separates them: the kind, the
 * foreground process, the focused window, the size and the caption are identical in both, and
 * Windows exposes no owner relation (Owner == NULL is part of the classifier).
 */
typedef enum {
    QGA_SURFACE_PRESENT = 0,
    QGA_SURFACE_REFUSE_START,       /* the Start surface itself, in seamless with SeamlessStart=0 */
    QGA_SURFACE_REFUSE_COMPANION    /* the Search surface OF a hidden Start opening */
} QGA_SURFACE_POLICY;

/* kind: mirrors SHELL_SURFACE_KIND (toastcrop.h) as plain ints so this header stays free of the
 * agent's own headers; main.c C_ASSERTs the two agree. */
#define QGA_SHELL_MANAGED_NONE  0u  /* mirrors perf.h's SHELL_MANAGED_*, so this header stays */
#define QGA_SHELL_MANAGED_ALL   1u  /* pure and the offline suite builds without it; main.c    */
#define QGA_SHELL_MANAGED_START 2u  /* C_ASSERTs each against the real one                     */
#define QGA_SURFACE_KIND_NONE   0
#define QGA_SURFACE_KIND_TOAST  1
#define QGA_SURFACE_KIND_START  2
#define QGA_SURFACE_KIND_SEARCH 3

/* ---- MAY THIS SHELL SURFACE BE ANNOUNCED AS A WM-MANAGED (MOVABLE) WINDOW? --------------------
 * The owner's requirement, 2026-10-18: "start menu is either movable and resizeable normal window
 * (like settings), or (last option) override-redirect", then amended - "resizable may drop",
 * "movable is important". So Start is announced or=0 and SIZE-LOCKED; the dom0 WM frames it and
 * drags it, and SendWindowSizeLock's PMinSize==PMaxSize offering no resize handle is now the
 * intended behaviour rather than a contradiction.
 *
 * CROP RESOLUTION IS THE CALLER'S GATE, not a parameter here. It was a parameter in the first
 * draft, and at the only call site it was always TRUE - the site is reached only once a crop has
 * resolved - which makes it an inert argument, the shape this project has been caught by before.
 * The caller keeps that test where it already lived; this predicate decides policy, kind and size.
 *
 * THE GUARD THIS EXISTS FOR. A shell CoreWindow is SCREEN-SIZED while it composes and card-sized
 * once the menu is placed - measured 2026-10-18, the host's window held 5120x1392 at 0,0 for a whole
 * opening while the card it carries is 642x726. Announcing the screen-sized state as a NORMAL window
 * puts a full-screen framed, draggable window on dom0's screen; the 2026-08-12 measurement recorded
 * exactly that ("a shell CoreWindow announced or=0 at 5120x1336"). The existing site gates the flip
 * on the crop having resolved, which is necessary and NOT sufficient: a crop can resolve to the
 * window's own full extent. So a card that spans the screen is refused the frame and stays
 * override-redirect, which is the safe direction - a menu nobody can drag beats a full-screen
 * window the user cannot get rid of.
 *
 * screenW/screenH of 0 mean "not known yet" and disable only the proportion test, never the rest:
 * at startup g_ScreenWidth is 0 until the first xconf, and refusing every shell frame until then
 * would be a different defect.
 * Knob: QGA_LIFECYCLE_DEFECT_GIANTSHELLFRAME removes the proportion test, so a screen-sized shell
 * surface is announced as a normal window - the 2026-08-12 shape. The suite must fail with it. */
static __inline BOOL QgaShellFrameAllowed(
    IN unsigned shellManaged,      /* SHELL_MANAGED_NONE / _ALL / _START */
    IN int kind,                   /* QGA_SURFACE_KIND_* */
    IN unsigned announcedW,
    IN unsigned announcedH,
    IN unsigned screenW,           /* 0 = not known yet */
    IN unsigned screenH)
{
    if (kind == QGA_SURFACE_KIND_NONE)
        return FALSE;
    if (shellManaged == QGA_SHELL_MANAGED_NONE)
        return FALSE;
    if (shellManaged == QGA_SHELL_MANAGED_START && kind != QGA_SURFACE_KIND_START)
        return FALSE;
    if (announcedW == 0 || announcedH == 0)
        return FALSE;
#ifndef QGA_LIFECYCLE_DEFECT_GIANTSHELLFRAME
    /* 90% of either axis. A real card is at most about half the screen; the measured bad state is
     * the full width. Integer, so no float creeps into a decision. */
    if (screenW != 0 && (unsigned long long)announcedW * 10ULL >= (unsigned long long)screenW * 9ULL)
        return FALSE;
    if (screenH != 0 && (unsigned long long)announcedH * 10ULL >= (unsigned long long)screenH * 9ULL)
        return FALSE;
#else
    (void)screenW; (void)screenH;  /* DEFECT: a screen-sized shell surface gets a dom0 frame */
#endif
    return TRUE;
}

static __inline QGA_SURFACE_POLICY QgaShellSurfacePolicy(
    IN BOOL seamless,
    IN BOOL seamlessStart,
    IN int kind,
    IN BOOL hiddenStartOpen)
{
    if (!seamless)
        return QGA_SURFACE_PRESENT;         /* non-seamless shows the whole desktop anyway */
    if (seamlessStart)
        return QGA_SURFACE_PRESENT;         /* work on Start is enabled: present everything */
    if (kind == QGA_SURFACE_KIND_START)
        return QGA_SURFACE_REFUSE_START;
#ifndef QGA_LIFECYCLE_DEFECT_COMPANIONSHOWN
    if (kind == QGA_SURFACE_KIND_SEARCH && hiddenStartOpen)
        return QGA_SURFACE_REFUSE_COMPANION;
#else
    (void)hiddenStartOpen;                  /* DEFECT: the Search companion of a hidden Start is
                                             * presented - a menu declared not-shown still puts a
                                             * slice of itself on the screen */
#endif
#ifdef QGA_LIFECYCLE_DEFECT_SEARCHALWAYSHIDDEN
    if (kind == QGA_SURFACE_KIND_SEARCH)
        return QGA_SURFACE_REFUSE_COMPANION;   /* DEFECT: a standalone Win+S Search the user asked
                                                * for is refused too */
#endif
    return QGA_SURFACE_PRESENT;
}

/* ---------------------------------------------------------------------------------------------
 * WHICH CARD RULE MEASURES THIS SURFACE?  (the crop, decided by KIND)
 *
 * The card walk reports two candidates per measurement - the largest qualifying element and the
 * union of them - and the rule chosen decides which becomes the crop. A Search flyout was being
 * measured by the TOAST rule because the selector was a single BOOL ("is it a menu popup"), so
 * every non-menu surface got the toast rule by construction.
 *
 * SEARCH GETS NO RULE, and that was decided by reading the walks rather than by preference.
 * MEASURED on the German 25H2 guest, the same window at its two raw sizes:
 *     raw 800x798 -> largest=(2364,655) 774x724   union=(2363,655) 775x777
 *     raw 800x750 -> largest=(2196,687) 728x32    union=(2196,687) 728x32
 * At the smaller size BOTH candidates collapse to the same 728x32 sliver - the search input field,
 * not the card - so neither rule is reliable for this surface and a crop from either would clip the
 * flyout. Jev on the walk lines alone: neither-announce-uncropped 0.72 (largest 0.19, union 0.08).
 * The cost is stated rather than hidden: the flyout is announced with its ~13 px shadow margin
 * showing, which is worse pixels than a correct crop and better than a clipped window.
 */
typedef enum {
    QGA_CARD_RULE_NONE = 0,         /* announce uncropped - no reliable card for this kind */
    QGA_CARD_RULE_TOAST_LARGEST,
    QGA_CARD_RULE_MENU_UNION
} QGA_CARD_RULE;

static __inline QGA_CARD_RULE QgaCardRuleForSurface(IN int kind, IN BOOL isMenuPopup)
{
    if (isMenuPopup)
        return QGA_CARD_RULE_MENU_UNION;    /* a real menu popup, whatever process owns it */
#ifdef QGA_LIFECYCLE_DEFECT_SEARCHASTOAST
    if (kind == QGA_SURFACE_KIND_SEARCH)
        return QGA_CARD_RULE_TOAST_LARGEST; /* DEFECT: today's construction - the selector was one
                                             * BOOL, so every non-menu surface got the toast rule
                                             * and a Search flyout was measured as a toast */
#endif
    if (kind == QGA_SURFACE_KIND_TOAST || kind == QGA_SURFACE_KIND_START)
        return QGA_CARD_RULE_TOAST_LARGEST; /* the 25H2 Start card is found by this rule today */
    return QGA_CARD_RULE_NONE;              /* search, and anything unclassified: measured above */
}

/* ---------------------------------------------------------------------------------------------
 * IS THIS HOLD'S REASON A NORMAL ONE?  (BROKERHOLD's level, decided per CAUSE)
 *
 * BrokerFreshFrame refuses a frame for eleven distinct reasons and now names which; this decides
 * which of those names is worth a warning. ONE is excluded, on the owner's reading: a slot the
 * broker has registered but not yet published to.
 *
 * WHY THAT ONE IS NORMAL, end to end. A window appears; the agent decides it needs per-window
 * capture and asks the broker to take it; the broker assigns a slot and starts a WGC session for
 * that HWND, and the agent records sourced=1 slot=N. The session needs a few milliseconds and a
 * compositor tick before it publishes anything. If a damage pass runs in that gap, the slot exists
 * and is ACTIVE but carries FrameWidth=FrameHeight=0, the dims comparison rejects it, and the
 * slice-fed caller HOLDS last content for that one pass. Milliseconds later the first frame lands
 * and the window paints. Nothing failed, and on a guest that opens many short-lived surfaces it
 * happens constantly - which is why the Start-menu reproduction harness carries 18-49 of these per
 * run while an ordinary acceptance cell carries one or two.
 *
 * AND THIS IS THE ONE CASE WHERE THE SAFETY NET REALLY DOES COVER IT. QGADIRECTSUPPRESS reports a
 * window that NEVER RECEIVED A FRAME, on QgaWindowFaultDue's clock below - which is exactly this
 * condition persisting. An earlier attempt to demote this line offered that net for ALL 589
 * healthy-feed holds and was reverted at Jev 0.60 because it does not cover a window that was fed
 * and then stopped. Keyed on this single reason it is the right net, not a borrowed one.
 *
 * EVERY OTHER REASON STAYS LOUD, and they are not the same finding: slot-holds-another-hwnd is the
 * agent and the broker disagreeing about who owns a slot; dims-mismatch-STUCK is a feed lost past
 * its bound; broker-paused is the secure desktop; torn-read-4-retries is contention. A dims
 * mismatch with NON-ZERO dimensions is a real disagreement about size and keeps its warning - only
 * the all-zero case, which means "nothing written yet", is quiet.
 *
 * WHY A PREDICATE AND NOT A STRING TEST AT THE CALL SITE: so the offline suite can drive every
 * reason through it, and so a reason added later is LOUD by default rather than silently matching.
 */
/* HOW LONG A HELD WINDOW MAY SHOW STALE PIXELS BEFORE IT IS A DECLARED FAULT.
 *
 * THIS IS THE DETECTOR THE BROKERHOLD WARNING WAS STANDING IN FOR, badly. That line is one-shot
 * per window, so a window the broker never feeds again showed stale pixels indefinitely with
 * nothing further said - and Jev, asked whether a one-shot warning is a usable starvation
 * detector, said 0.15. The same call said "revert until a starvation detector exists" 0.60 and
 * "stale pixels are user-visible" 0.70. The comment at the emitter has said twice that the real
 * answer is a per-window clock that reports a hold beyond a bound. This is that clock.
 *
 * A FRAME RESETS IT. The clock runs from the LATER of "the hold began" and "painted content last
 * landed", so a window that is fed, holds briefly, and is fed again never declares anything -
 * which is the 589-of-594 healthy-feed population that made the warning noise in the first place.
 * Only a hold that OUTLASTS the bound with no new content is a fault, and then it is an ERROR
 * naming the consequence the user can see.
 *
 * 10 s, the same order as DIRECT_WINDOW_GRACE_MS: long enough that no measured healthy hold on
 * this rig reaches it (cold-boot broker launch is 10.8-11.6 s on an AppVM, but that window is
 * WITHHELD, not held with stale content - it has no painted frame to go stale), short enough that
 * a starved window is reported while the session is still the one that starved it.
 */
#define QGA_HOLD_STARVE_MS 10000u

/* NO DEFECT KNOB HERE, deliberately: the owner's standing rule is to ask before adding one, and
 * both facts this predicate reads are plain arguments the offline suite can drive directly. */
/* WHY THIS HAS A KNOB, added 2026-10-24. QGAHOLDSTARVED has been emitted ZERO times in every capture
 * on this rig, and that zero was about to be used as the evidence that a BROKERHOLD hold is a benign
 * transient - 117 of 117 of them carry why=slot-ack-pending and none ever outlasted this bound. Jev
 * scored that reasoning 0.49: a never-fired detector cannot distinguish a condition that does not
 * happen from a detector that cannot fire. This predicate had NO test at all, so the zero meant
 * nothing. It means something now. */
static __inline BOOL QgaHoldStarveDue(IN unsigned long long holdSince, IN unsigned long long lastContent,
                                      IN unsigned long long now, IN unsigned long long boundMs)
{
#ifdef QGA_LIFECYCLE_DEFECT_STARVENEVERDUE
    /* DEFECT: the inert detector - exactly the shape a zero emitted-count cannot tell apart from a
     * condition that never occurs. */
    (void)holdSince; (void)lastContent; (void)now; (void)boundMs;
    return FALSE;
#else
    unsigned long long from;
    if (holdSince == 0)
        return FALSE;                       /* not holding: no clock, nothing to declare */
    if (lastContent == 0)
        return FALSE;                       /* never had painted content: that is QGADIRECTSUPPRESS's
                                             * population, on QgaWindowFaultDue's clock - a window
                                             * with nothing on screen cannot show STALE pixels */
    from = (lastContent > holdSince) ? lastContent : holdSince;
    return (BOOL)(now > from && (now - from) >= boundMs);
#endif
}

static __inline BOOL QgaHoldReasonIsNormal(IN const WCHAR* why)
{
    if (!why)
        return FALSE;                       /* no reason recorded: never quiet on an unknown */
#ifndef QGA_LIFECYCLE_DEFECT_HOLDREASONALLNORMAL
    return (BOOL)(wcscmp(why, L"first-frame-pending") == 0);
#else
    (void)why;                              /* DEFECT: every reason reads as normal, so a broker
                                             * that dropped a slot is as quiet as a first frame */
    return TRUE;
#endif
}

/* ---------------------------------------------------------------------------------------------
 * WAS THIS LATE REGISTRATION THE DESIGNED COLD-BOOT PATH?  (BROKERLATEREG's level)
 *
 * A window added while the broker had not started yet gets no slot, is withheld, and is registered
 * by the periodic resync the moment the broker becomes ready. That is the designed path and happens
 * on EVERY cold boot, so it may not be a warning.
 *
 * THE MISTAKE THIS REPLACES, worth keeping because it is the third of its kind in two days: the
 * first version asked QgaHoldIsStartup(FALSE, g_BrokerReadyAt != 0) here. Late registration only
 * SUCCEEDS once the broker is ready, so g_BrokerReadyAt is non-zero at every call and the quiet
 * branch was unreachable - the fix read as applied, the guest still logged W, and only the receipt
 * caught it (2026-10-11). A predicate fed a fact that is constant at its call site is inert.
 *
 * The two facts that do vary, both true AT THE TIME the window went without a slot:
 *   refusedByActiveBroker - an ACTIVE broker had no free slot or no arena room for it
 *                           (PwRegFailLogged, snapshotted before the retry clears it). The broker
 *                           was there and said no: that is the capacity fault QGABROKERREGFAIL
 *                           already shouts about, and the late registration is its tail.
 *   everSourcedThisWindow - the window HELD a slot earlier in its life and lost it, which on this
 *                           path means the broker died and was relaunched. A component that was
 *                           working and stopped is a failure, reported loudly, never quietly
 *                           recovered from (CLAUDE.md, capabilities are decided at start).
 * Neither true = the window was never registered and was never refused, so the only thing that was
 * missing is a broker that had not started yet. Quiet.
 */
static __inline BOOL QgaLateRegIsStartup(IN BOOL refusedByActiveBroker, IN BOOL everSourcedThisWindow)
{
#ifndef QGA_LIFECYCLE_DEFECT_LATEREGALWAYSLOUD
    if (refusedByActiveBroker)
        return FALSE;                       /* the broker was up and had no room: a capacity fault */
    if (everSourcedThisWindow)
        return FALSE;                       /* it had a slot and lost it: the broker died */
    return TRUE;                            /* never registered, never refused: still starting */
#else
    (void)refusedByActiveBroker; (void)everSourcedThisWindow;
    return FALSE;                           /* DEFECT: every cold boot warns about correct behaviour */
#endif
}

/* ---------------------------------------------------------------------------------------------
 * IS A SLOW CAPTURE PASS WORTH A WARNING YET?  (CaptureThread's "main loop slow")
 *
 * The capture loop has TWO arms: at CAPTURE_READY_WEDGE_MS (15 s) it declares the loop wedged,
 * releases the frame, signals error_event and exits the thread; below that it logs "main loop
 * slow" once per second. The soft arm warned from the FIRST second.
 *
 * MEASURED over run 38994277148, every occurrence in the campaign:
 *   waited=1000ms x10, waited=2000ms x2, waited=3000ms x1
 * and because the emitter speaks once per second of the SAME pass, those 13 lines are ~9 distinct
 * slow passes - win11-clean 06:02:58+:59 is one pass of ~2 s, win11-reinstall 06:15:52+:53+:54 one
 * of ~3 s. The longest pass in the whole campaign was 3 s against a 15 s wedge, the hard arm fired
 * ZERO times, and every pass recovered on its own. All of them landed inside install or boot
 * activity on six different cells, i.e. while the guest was busy.
 * Jev: graduate the report 0.86 (leave it loud 0.14, RAISE THE THRESHOLD 0.00); is this a product
 * defect 0.23.
 *
 * SO THE WEDGE IS NOT MOVED AND NOTHING IS TUNED. This project refuses "tune the number until it
 * passes", and a timeout is never a fix - so the 15 s arm is untouched and this predicate does not
 * know what the data looked like. What changes is only the LEVEL of the sub-wedge report: the first
 * part of the wait is said at Info, and it becomes a Warning once the pass is closer to being
 * reinitialized than to being brief.
 *
 * THE BOUNDARY IS DERIVED FROM THE WEDGE, NOT FROM THE MEASUREMENT - which is what keeps it from
 * being the tuning this project refuses. Half of the wedge is the point past which a pass has less
 * of its budget left than it has already spent; it would sit at the same place whatever the
 * campaign had shown, and it is expressed as a fraction so that moving the wedge moves it too
 * rather than leaving a stale literal behind. That the measured 3 s falls below it is a
 * CONSEQUENCE, not the reason, and if a pass ever does cross it the line is loud again with no
 * further change.
 */
static __inline BOOL QgaCaptureSlowIsNormal(IN unsigned long waitedMs, IN unsigned long wedgeMs)
{
    if (wedgeMs == 0)
        return FALSE;                       /* no wedge configured: nothing to be a fraction of */
#ifndef QGA_LIFECYCLE_DEFECT_SLOWALWAYSLOUD
    return (BOOL)(waitedMs * 2 < wedgeMs);  /* less than half the budget spent: still brief */
#else
    (void)waitedMs;                         /* DEFECT: every slow pass warns from its first second */
    return FALSE;
#endif
}

/* ---------------------------------------------------------------------------------------------
 * IS THIS WINDOW'S FAULT CLOCK ACTUALLY DUE?  (QGADIRECTSUPPRESS and the dom0 notice it sends)
 *
 * MEASURED on win11-app, 2026-10-11, a window opened ~29 s before the broker became ready:
 *   09:38:09.121  BROKERHOLD hwnd 0x10094 ... brokerActive=0 slot=-1     (held, correctly)
 *   06:38:35.047  E QGADIRECTSUPPRESS hwnd 0x10094 (class Notepad, 1428x783) NOT SHOWN: never
 *                 received a frame ... (sliceFed=1 brokerSourced=1 slot=0 brokerActive=1
 *                 DirectSuppressed=1)
 *   06:38:35.064  DirectSuppressNotifyUser: user notification sent (1 suppressed window(s))
 *   ~11 s later   dom0 shows that window at 1428x783, mean 0.9906 - WHITE, correct, never black
 * So dom0 was told "a window could not be shown - the per-window display path is not working" about
 * a window that was fine, and DirectSuppressed - which acceptance asserts is 0 - was incremented on
 * a healthy guest.
 *
 * WHY. The window's own grace runs "from REGISTRATION", and the window was registered into a broker
 * that had only just become active and was not yet feeding anything. The comment above the old code
 * says the two clocks exist so that "a window freshly registered with a READY broker" is not
 * mistaken for one the broker can never capture - but the registration happened while the broker was
 * still coming up, so the window's 10 s was already spent by the time the broker could feed it.
 *
 * THE RULE: the per-window clock cannot start before the broker is in a position to feed it. It runs
 * from the LATER of the window's registration and the moment the broker became ready. If the broker
 * has never been ready (readyAt == 0), the clock runs from registration exactly as before, so a
 * broker that never arrives still ends in a bounded, loud fault - QGADESLICEDOWN names that case,
 * and nothing here can hide it.
 */
static __inline BOOL QgaWindowFaultDue(IN unsigned long long registeredAt, IN unsigned long long brokerReadyAt,
                                       IN unsigned long long now, IN unsigned long long graceMs)
{
    unsigned long long from;
    if (registeredAt == 0)
        return FALSE;                       /* never registered: no clock, nothing to declare */
#ifndef QGA_LIFECYCLE_DEFECT_FAULTFROMREG
    from = (brokerReadyAt > registeredAt) ? brokerReadyAt : registeredAt;
#else
    (void)brokerReadyAt;                    /* DEFECT: the clock runs from registration even when the
                                             * broker could not feed the window yet - the false
                                             * "a window could not be shown" measured on 2026-10-11 */
    from = registeredAt;
#endif
    return (BOOL)(now > from && (now - from) >= graceMs);
}

/* ---------------------------------------------------------------------------------------------
 * A SYSTEM-TERMINATED HELPER'S VERDICT IS NOT KNOWN AT THE EXIT. Windows kills a session's
 * processes and delivers WM_ENDSESSION to us AFTERWARDS, so "were we told the session was ending?"
 * is false at the moment the exit is seen and the exit reads as an ordering anomaly. MEASURED
 * 2026-10-19, release 4.3.37 acceptance, win10-clean, agent pid 3944:
 *     233257.268  W NOTIFBRIDGESYSKILL Windows killed the bridge pid 6448 before announcing session end
 *     233257.319  I QGAENDSESSION Windows is ending this session (shutdown, lParam 0x40000000)
 * Fifty-one milliseconds, and the site's own comment said "if no shutdown was in progress this is
 * an ordering anomaly worth finding" - it had no way to wait and find out. It fired in 2 of 8 cells
 * and each occurrence writes a DURABLE Event Log record. Jev: defect 0.84, defer_verdict 0.95 (over
 * querying SM_SHUTTINGDOWN, 0.05 - that metric is 0 on a LOGOFF, where the ordering is the same).
 *
 * THREE EXITS, and nothing is reported until one is taken:
 *   PENDING  - still inside the grace and no session end yet: say nothing
 *   TEARDOWN - the session end arrived: ordinary ordering, Info, no death record
 *   ANOMALY  - the grace expired with no session end: the case the warning exists for
 * The caller's fourth exit is its own: an agent that leaves before the grace expires reports
 * nothing, which is correct, because the session was ending.
 */
typedef enum _QGA_SYSKILL_VERDICT {
    QGA_SYSKILL_PENDING  = 0,
    QGA_SYSKILL_TEARDOWN = 1,
    QGA_SYSKILL_ANOMALY  = 2
} QGA_SYSKILL_VERDICT;

static __inline QGA_SYSKILL_VERDICT QgaSysKillVerdict(IN int sessionEnding, IN unsigned long long killedAt,
                                                      IN unsigned long long now, IN unsigned long long graceMs)
{
    if (killedAt == 0)
        return QGA_SYSKILL_PENDING;        /* nothing pending: no verdict to give */
    if (sessionEnding)
        return QGA_SYSKILL_TEARDOWN;       /* the fact that decides it arrived */
#ifdef QGA_LIFECYCLE_DEFECT_SYSKILLNOW
    (void)graceMs; (void)now;              /* DEFECT: decided at the exit, before the session end can
                                            * arrive - the false anomaly measured 2026-10-19 */
    return QGA_SYSKILL_ANOMALY;
#else
    if (now > killedAt && (now - killedAt) >= graceMs)
        return QGA_SYSKILL_ANOMALY;        /* the grace expired and nothing said the session was ending */
    return QGA_SYSKILL_PENDING;
#endif
}
