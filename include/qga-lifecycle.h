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

#include <windows.h>
#include <strsafe.h>
#include "qga-exitcodes.h"

#define QGA_LIFECYCLE_NAME_FMT      L"Global\\QGA_LIFECYCLE_%lu_%s"
#define QGA_LIFECYCLE_NOTICE        L"notice"
#define QGA_LIFECYCLE_ACK           L"ack"
#define QGA_LIFECYCLE_CONTINUE      L"continue"
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
    QGA_DECIDE_SESSION_END_FORCED,      // the system ended it after an acknowledged notice: the orderly exit did not finish in time - ERROR, no launch into that session
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
    QGA_EXIT_DECISION Decision;
} QGA_EXIT_ROW;
#define QGA_EXIT_ANY    0xFFFFFFFFUL
#define QGA_NOTICE_ANY  (-1)

// The attributes of each decision, indexed by QGA_EXIT_DECISION.
static const QGA_EXIT_VERDICT QgaExitVerdicts[] =
{
    /* decision                           relaunch  noLaunch  record  fail   error  name */
    { QGA_DECIDE_NOTHING,                 FALSE,    FALSE,    FALSE,  FALSE, FALSE, L"REQUESTED" },
    { QGA_DECIDE_SESSION_END,             FALSE,    TRUE,     FALSE,  FALSE, FALSE, L"SESSIONEND" },
    { QGA_DECIDE_SESSION_END_UNNOTICED,   FALSE,    TRUE,     FALSE,  FALSE, TRUE,  L"SESSIONEND-UNNOTICED" },
    { QGA_DECIDE_SESSION_END_FORCED,      FALSE,    TRUE,     FALSE,  FALSE, TRUE,  L"SESSIONEND-FORCED" },
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
    { QGA_EXIT_REQUESTED,         QGA_NOTICE_ANY, QGA_DECIDE_NOTHING },
#endif
    { QGA_EXIT_NO_GUI_DOMAIN,     QGA_NOTICE_ANY, QGA_DECIDE_NOGUI_LATCH },
    { QGA_EXIT_SESSION_END,       1,              QGA_DECIDE_SESSION_END },
    { QGA_EXIT_SESSION_END,       0,              QGA_DECIDE_SESSION_END_UNNOTICED },
#ifndef QGA_LIFECYCLE_DEFECT_TERMINATEDDEATH
    { QGA_EXIT_SYSTEM_TERMINATED, 1,              QGA_DECIDE_SESSION_END_FORCED },
    { QGA_EXIT_SYSTEM_TERMINATED, 0,              QGA_DECIDE_NOTICE_MISSED },   // DEFECT (knob TERMINATEDDEATH): the measured relaunch into the ending session
#endif
    { QGA_EXIT_RECONNECT,         1,              QGA_DECIDE_SESSION_END },     // a notice always wins: nothing is launched into an ending session
    { QGA_EXIT_RECONNECT,         0,              QGA_DECIDE_RECONNECT },
    { QGA_EXIT_ANY,               1,              QGA_DECIDE_DEATH_IN_ENDING_SESSION },
    { QGA_EXIT_ANY,               QGA_NOTICE_ANY, QGA_DECIDE_DEATH },
};

static __inline QGA_EXIT_VERDICT QgaDecideAgentExit(IN DWORD exitCode, IN BOOL noticeAcked)
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
        d = r->Decision;
        break;
    }
    for (i = 0; i < RTL_NUMBER_OF(QgaExitVerdicts); i++)
        if (QgaExitVerdicts[i].Decision == d)
            return QgaExitVerdicts[i];
    return QgaExitVerdicts[RTL_NUMBER_OF(QgaExitVerdicts) - 1];   // unreachable: every decision has a row
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
