/*
 * errbox.h - THE ERROR WINDOW, shown when dom0 cannot be told (docs/ADR-supervision.md section 6, main repo; owner
 * 2026-10-07: "if dom0 notification for a loud error is not available we should emit an error window (if the gui
 * agent itself is alive). if it dies, error notification should be the first thing it shows").
 *
 * Mechanism (Jev 1.0): WTSSendMessage on the console session - a Windows-native message box that exists on the
 * user's desktop whether or not the GUI agent is alive, drawn by the system's own process in that session, not by
 * ours: the SYSTEM death reporter in session 0 shows it while the agent is dead, a live agent maps it like any
 * window, and a restarted agent finds it waiting at its first enumeration and maps it FIRST (main.c
 * AddAllWindows, ErrBoxIsSystemBox). Non-blocking: bWait FALSE, no timeout, so the box stays until a human reads it.
 *
 * WHEN it is shown is the route's decision, made in ONE pure place (notifyerr.h QerrWindowWanted): after the
 * route's own per-boot dedupe and cap, so a window never storms and never doubles a dom0 notification. This file
 * is only the Win32 call, shared by the agent (notifyerr.c) and notifhost (ReportErrorSelf).
 *
 * THE TITLE is prefixed with the product name: that prefix, the dialog class (#32770) and the owning system process
 * are what the agent recognizes the box by. Keep QERR_BOX_TITLE_PREFIX and main.c's ERRBOX_TITLE_PREFIX identical.
 */
#pragma once
#include <windows.h>
#include <wtsapi32.h>
#include <strsafe.h>

#ifdef _MSC_VER
#pragma comment(lib, "wtsapi32.lib")
#endif

#define QERR_BOX_TITLE_PREFIX L"Qubes Windows Tools"
#define QERR_BOX_TITLE_MAX    200
#define QERR_BOX_TEXT_MAX     1200

// Shows the route's text as a message box on the console session's desktop. header: the notification's header
// (becomes the title after the prefix); textUtf8: the whole notification text (header, what happens next, cause,
// technical line - the same words dom0 would have shown). Returns TRUE iff WTSSendMessage accepted it; every
// failure is one win_perror-style GetLastError the caller logs. Never waits: the box outlives the caller.
static __inline BOOL QerrShowErrorBox(IN const char *headerUtf8, IN const char *textUtf8, OUT DWORD *lastError)
{
    WCHAR title[QERR_BOX_TITLE_MAX];
    WCHAR header[QERR_BOX_TITLE_MAX];
    WCHAR text[QERR_BOX_TEXT_MAX];
    DWORD session, response = 0;
    size_t titleLen = 0, textLen = 0;

    if (lastError) *lastError = ERROR_SUCCESS;
    session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF)
    {
        if (lastError) *lastError = ERROR_NO_SUCH_LOGON_SESSION;   // no console session: nobody to show it to
        return FALSE;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, textUtf8 ? textUtf8 : "", -1, text, (int)RTL_NUMBER_OF(text)))
    {
        if (lastError) *lastError = GetLastError();
        return FALSE;
    }
    header[0] = 0;
    if (headerUtf8 && *headerUtf8)
        MultiByteToWideChar(CP_UTF8, 0, headerUtf8, -1, header, (int)RTL_NUMBER_OF(header));
    if (header[0])
        StringCchPrintfW(title, RTL_NUMBER_OF(title), QERR_BOX_TITLE_PREFIX L" - %s", header);
    else
        StringCchCopyW(title, RTL_NUMBER_OF(title), QERR_BOX_TITLE_PREFIX);
    StringCchLengthW(title, RTL_NUMBER_OF(title), &titleLen);
    StringCchLengthW(text, RTL_NUMBER_OF(text), &textLen);
    // Lengths are in BYTES (the API's contract); Style MB_OK | MB_ICONERROR; Timeout 0 = none; bWait FALSE.
    if (!WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session, title, (DWORD)(titleLen * sizeof(WCHAR)),
                         text, (DWORD)(textLen * sizeof(WCHAR)), MB_OK | MB_ICONERROR, 0, &response, FALSE))
    {
        if (lastError) *lastError = GetLastError();
        return FALSE;
    }
    return TRUE;
}
