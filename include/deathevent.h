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

// deathevent.h - the ONE Event Log record a supervisor of ours writes when a process it started exits
// without being asked to (docs/ADR-supervision.md section 2, main repo).
//
// WHY AN EVENT AND NOT A NOTIFICATION. Windows records crashes (Application 1000/1001, .NET Runtime
// 1026), service deaths (System 7031/7034/7023/7024) and task failures (TaskScheduler 201/203) by
// itself. The one death it cannot see is a child that exits CLEANLY and unasked under a supervisor of
// ours: gui-agent.exe under the QubesGuiWatchdog service, and the de-slice broker, the notification
// bridge and the ETW proxy under the agent. Each supervisor writes exactly this record under our
// registered source, and nothing else. ONE event-triggered reporter task (guest/qwt-report-death.ps1,
// main repo) subscribes to every record above, filtered to our components, counts the death and sends
// the dom0 notification. No component sends its own death notification (ADR section 3).
//
// RENDERING WITHOUT A MESSAGE DLL. The installer registers the source DEATHEVENT_SOURCE in the
// Application log with the .NET Framework's EventLogMessages.dll as its message file (every message
// id in it is "%1"), so the FIRST insertion string is the rendered text. The strings after it are
// STRUCTURED fields for the reporter (Event/EventData/Data[2..6]); they do not render, and need not:
// the reporter reads the raw event, never the rendered message. Until the installer has registered
// the source the record is still written - Event Viewer then shows the raw strings with a "description
// not found" preamble, which is loud, not lost.
//
// HEADER-ONLY ON PURPOSE. The watchdog and the agent are separate binaries (vs2022/watchdog compiles
// watchdog.c alone) and include/ is on both projects' include path (common.h comes from there). The
// composer is pure - no I/O - so the offline suite (gui-agent/deathevent_test.c, gcc on the dev qube)
// pins the contract; the writer is three Win32 calls, which that suite stubs.
//
// The exit code is written RAW. Its meaning (0xC0000409 = fast-fail, ...) is decoded in ONE place,
// the reporter, so the text a human reads in dom0 and the text in our own deaths log cannot drift.
//
// THE RENDERED TEXT (%1, what Event Viewer shows) follows the dom0 notification's style (rz39,
// agent notifyerr.h): the component by its human name first, the executable and pid in brackets,
// what happened in words, then the code, the run time and the supervisor's own words.

#include <windows.h>
#include <strsafe.h>
#include <log.h>

#define DEATHEVENT_SOURCE L"Qubes Windows Tools"

// One id per supervised child, so the reporter and a human reading the log can tell them apart
// without parsing text. 4001-4004 are ours; nothing else in the product writes under this source.
#define DEATHEVENT_ID_GUI_AGENT     4001    // the QubesGuiWatchdog service: gui-agent.exe exited unasked
#define DEATHEVENT_ID_WGCBROKER     4002    // gui-agent: the de-slice broker stopped serving (exit or hang)
#define DEATHEVENT_ID_NOTIFBRIDGE   4003    // gui-agent: the notification bridge exited unasked
#define DEATHEVENT_ID_ETWPROXY      4004    // gui-agent: the ETW signal proxy exited unasked

// "I do not know": a supervisor that found a child gone without observing its exit has no exit
// code; one that found it gone without a launch stamp has no run time. Both are written as the
// word "unknown", never as 0.
#define DEATHEVENT_EXIT_UNKNOWN     0xFFFFFFFFUL
#define DEATHEVENT_RAN_UNKNOWN      ((ULONGLONG)-1)
// A HANG IS NOT AN EXIT: a child that was still running but stopped answering, and that the
// supervisor then ended itself, has no exit code of its own - the word "hung" is written in the
// exit-code field, so the reporter renders a hang as a hang and never as "exited, code unknown".
#define DEATHEVENT_EXIT_HUNG        0xFFFFFFFEUL

// The human name of each supervised child (the dom0 notification and the deaths log use the same
// names - guest/qwt-report-death.ps1's table), and who supervises it.
static const WCHAR *DeathEventHumanName(IN DWORD eventId)
{
    switch (eventId)
    {
    case DEATHEVENT_ID_GUI_AGENT:   return L"The GUI agent";
    case DEATHEVENT_ID_WGCBROKER:   return L"The notification and menu capture helper";
    case DEATHEVENT_ID_NOTIFBRIDGE: return L"The notification bridge";
    case DEATHEVENT_ID_ETWPROXY:    return L"The ETW signal proxy";
    default:                        return L"A component";
    }
}
static const WCHAR *DeathEventSupervisorName(IN DWORD eventId)
{
    return eventId == DEATHEVENT_ID_GUI_AGENT ? L"the GUI agent watchdog" : L"the GUI agent";
}

// Insertion strings: %1 renders; 2..6 are the reporter's structured fields, in this fixed order.
#define DEATHEVENT_STRINGS          6
#define DEATHEVENT_TEXT_MAX         640
#define DEATHEVENT_EXE_MAX          64
#define DEATHEVENT_DETAIL_MAX       320

typedef struct _DEATHEVENT
{
    DWORD EventId;
    WORD Type;                                   // EVENTLOG_ERROR_TYPE - a death is a major error, never a warning
    WCHAR Text[DEATHEVENT_TEXT_MAX];             // %1 rendered text
    WCHAR Exe[DEATHEVENT_EXE_MAX];               // %2 image name as the supervisor knows it, e.g. gui-agent.exe
    WCHAR Pid[16];                               // %3 decimal pid
    WCHAR ExitCode[16];                          // %4 0x%08X, or "unknown"
    WCHAR RanMs[24];                             // %5 decimal milliseconds the child ran, or "unknown"
    WCHAR Detail[DEATHEVENT_DETAIL_MAX];         // %6 the supervisor's own words: what it does next, where its log is
    LPCWSTR Strings[DEATHEVENT_STRINGS];
} DEATHEVENT;

// Pure: fills *ev from the arguments, never fails (every field is bounded and truncation is silent
// and safe - StringCch* always terminates). Returns the number of insertion strings.
static WORD DeathEventCompose(
    OUT DEATHEVENT *ev,
    IN DWORD eventId,
    IN const WCHAR *exeName,
    IN DWORD pid,
    IN DWORD exitCode,
    IN ULONGLONG ranMs,
    IN const WCHAR *detail OPTIONAL)
{
    WCHAR ran[64];

    ZeroMemory(ev, sizeof(*ev));
#ifdef DEATHEVENT_DEFECT_SHAREDID
    UNREFERENCED_PARAMETER(eventId);
    ev->EventId = DEATHEVENT_ID_GUI_AGENT;       // DEFECT: every death looks like an agent death
#else
    ev->EventId = eventId;
#endif
#ifdef DEATHEVENT_DEFECT_WARNING
    ev->Type = EVENTLOG_WARNING_TYPE;            // DEFECT: "it is not fucking warning! it is a major error!"
#else
    ev->Type = EVENTLOG_ERROR_TYPE;
#endif
    StringCchCopyW(ev->Exe, RTL_NUMBER_OF(ev->Exe), (exeName && *exeName) ? exeName : L"?");
    StringCchPrintfW(ev->Pid, RTL_NUMBER_OF(ev->Pid), L"%lu", (unsigned long)pid);
#ifdef DEATHEVENT_DEFECT_NOCODE
    exitCode = DEATHEVENT_EXIT_UNKNOWN;          // DEFECT: the one fact a crash leaves behind is dropped
#endif
#ifdef DEATHEVENT_DEFECT_HANGASEXIT
    if (exitCode == DEATHEVENT_EXIT_HUNG)
        exitCode = DEATHEVENT_EXIT_UNKNOWN;      // DEFECT: a hang is written as an exit with no code (rz39 defect 2)
#endif
    if (exitCode == DEATHEVENT_EXIT_UNKNOWN)
        StringCchCopyW(ev->ExitCode, RTL_NUMBER_OF(ev->ExitCode), L"unknown");
    else if (exitCode == DEATHEVENT_EXIT_HUNG)
        StringCchCopyW(ev->ExitCode, RTL_NUMBER_OF(ev->ExitCode), L"hung");
    else
        StringCchPrintfW(ev->ExitCode, RTL_NUMBER_OF(ev->ExitCode), L"0x%08X", (unsigned int)exitCode);
    if (ranMs == DEATHEVENT_RAN_UNKNOWN)
    {
        StringCchCopyW(ev->RanMs, RTL_NUMBER_OF(ev->RanMs), L"unknown");
        StringCchCopyW(ran, RTL_NUMBER_OF(ran), L"an unknown time");
    }
    else
    {
        StringCchPrintfW(ev->RanMs, RTL_NUMBER_OF(ev->RanMs), L"%llu", (unsigned long long)ranMs);
        StringCchPrintfW(ran, RTL_NUMBER_OF(ran), L"%llu:%02u:%02u (%llu ms)",
            (unsigned long long)(ranMs / 3600000ULL), (unsigned int)((ranMs / 60000ULL) % 60ULL),
            (unsigned int)((ranMs / 1000ULL) % 60ULL), (unsigned long long)ranMs);
    }
    StringCchCopyW(ev->Detail, RTL_NUMBER_OF(ev->Detail), detail ? detail : L"");
    if (exitCode == DEATHEVENT_EXIT_HUNG)
        StringCchPrintfW(ev->Text, RTL_NUMBER_OF(ev->Text),
            L"%s (%s, PID %s) stopped answering and was ended by %s - a hang, so there is no exit code - "
            L"after running %s. A Qubes Windows Tools component died; this is a major error. %s",
            DeathEventHumanName(ev->EventId), ev->Exe, ev->Pid, DeathEventSupervisorName(ev->EventId),
            ran, ev->Detail);
    else
        StringCchPrintfW(ev->Text, RTL_NUMBER_OF(ev->Text),
            L"%s (%s, PID %s) exited without being asked to - exit code %s - after running %s. "
            L"A Qubes Windows Tools component died; this is a major error. %s",
            DeathEventHumanName(ev->EventId), ev->Exe, ev->Pid, ev->ExitCode, ran, ev->Detail);
    ev->Strings[0] = ev->Text;
    ev->Strings[1] = ev->Exe;
    ev->Strings[2] = ev->Pid;
    ev->Strings[3] = ev->ExitCode;
    ev->Strings[4] = ev->RanMs;
    ev->Strings[5] = ev->Detail;
    return DEATHEVENT_STRINGS;
}

// Writes the record. Returns TRUE iff the Event Log accepted it; every failure is one win_perror line
// and nothing else - the caller has already logged the death at ERROR and must not be derailed by the
// reporting path. Fire-and-forget: RegisterEventSource/ReportEvent are local RPC calls to the Event
// Log service, bounded by that service, with no dependency on qrexec or on any session.
static BOOL DeathEventReport(
    IN DWORD eventId,
    IN const WCHAR *exeName,
    IN DWORD pid,
    IN DWORD exitCode,
    IN ULONGLONG ranMs,
    IN const WCHAR *detail OPTIONAL)
{
    DEATHEVENT ev;
    HANDLE source;
    WORD count;
    BOOL ok;

    count = DeathEventCompose(&ev, eventId, exeName, pid, exitCode, ranMs, detail);
    source = RegisterEventSourceW(NULL, DEATHEVENT_SOURCE);
    if (!source)
    {
        win_perror("RegisterEventSourceW(" DEATHEVENT_SOURCE L")");
        return FALSE;
    }
    ok = ReportEventW(source, ev.Type, 0, ev.EventId, NULL, count, 0, ev.Strings, NULL);
    if (!ok)
        win_perror("ReportEventW");
    DeregisterEventSource(source);
    return ok;
}
