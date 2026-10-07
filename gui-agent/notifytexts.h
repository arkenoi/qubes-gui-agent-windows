/*
 * notifytexts.h - every dom0 error notification the gui-agent and notifhost send, as data
 * (notifyerr.h has the shape and the rules; docs/DESIGN-error-notify.md section 8 the design).
 *
 * One row per notification: the route component and id, the header, line 1 (what it means and
 * what happens next), line 2 (the cause with the code) and the technical line's fixed pieces. The
 * callers (main.c, notifhost.cpp) look a row up by its key and hand it to QerrReportText; the
 * offline render test (notifyrender_test.c, gcc on the dev qube via the main repo's
 * tools/tests/notify-render-selftest.sh) renders every row and holds it to the rules - header at
 * most 60 characters with no code, file name or count in it, a body of 2 to 4 lines, the technical
 * line present and shaped, the code named by its own source's table, the route's redaction
 * accepting the text. A text that lives in a call site cannot be rendered without running the
 * agent; a text that lives here can.
 *
 * Human names (the header never says "gui-agent" or "wgcbroker.exe"): the GUI agent, the de-slice
 * broker, the notification bridge, the ETW signal proxy - the same names guest/qwt-report-death.ps1
 * uses for the deaths of these components, so a reader meets one vocabulary.
 *
 * DEFECT KNOBS (render test only; never build them otherwise):
 *   NOTIFYTEXT_DEFECT_HEADER       a header in the old style: product prefix, file name, code, count
 *   NOTIFYTEXT_DEFECT_SECRETWORD   a line carries a credential keyword the route refuses
 *   NOTIFYTEXT_DEFECT_WRONGSOURCE  a process exit code phrased by the Win32 (SCM) table
 */
#ifndef QWT_NOTIFYTEXTS_H
#define QWT_NOTIFYTEXTS_H

#include "notifyerr.h"

#ifdef NOTIFYTEXT_DEFECT_HEADER
#define QERR_TXT_BROKER_MISSING_HEADER \
    "Qubes Windows Tools, gui-agent: wgcbroker.exe is missing (0xC0000135); death 1 this boot"
#else
#define QERR_TXT_BROKER_MISSING_HEADER "The notification and menu capture helper is missing"
#endif

#ifdef NOTIFYTEXT_DEFECT_SECRETWORD
#define QERR_TXT_DESKTOP_STUCK_NEXT \
    "Seamless mode shows nothing while this lasts: arm autologon in the guest (the token is in the registry)."
#else
#define QERR_TXT_DESKTOP_STUCK_NEXT \
    "Seamless mode shows nothing while this lasts: arm autologon in the guest, or switch this qube to the windowed desktop."
#endif

#ifdef NOTIFYTEXT_DEFECT_WRONGSOURCE
#define QERR_TXT_LISTENER_DENIED_CODE  "Windows error 2"
#define QERR_TXT_LISTENER_DENIED_CAUSE "Cause: Windows error 2 (the system cannot find the file specified)."
#else
#define QERR_TXT_LISTENER_DENIED_CODE  "exit code 2"
#define QERR_TXT_LISTENER_DENIED_CAUSE "Cause: Windows refused the bridge's toast-listener access for this user (exit code 2)."
#endif

#define QERR_GUI_AGENT_LOG "gui-agent log in Qubes Logs"

static const QerrText QerrTexts[] = {
    /* --- the agent's own faults (component gui-agent, one per (component, id) per boot) --- */
    { "broker-missing", "gui-agent", "broker-missing", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      QERR_TXT_BROKER_MISSING_HEADER,
      "Menus, modern app windows and notification windows do not appear in dom0 until the package is reinstalled.",
      "Cause: wgcbroker.exe is not next to gui-agent.exe - a packaging gap, not a runtime fault (log line QGABROKERMISSING).",
      QERR_GUI_AGENT_LOG ", line QGABROKERMISSING", NULL },

    /* the app's image name (<= 32 chars, [A-Za-z0-9._-], ".exe" stripped) fills the %s; the id is
     * capture-deaf-<app>, so each deaf app is reported once per boot */
    { "capture-deaf", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "A %s window stopped updating",
      "dom0 keeps showing that window's last picture; close and reopen the window to get a live one.",
      "Cause: Windows delivers no new pictures of that window to the capture, even after the capture was re-created (log line QGAWGCDEAF).",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", "reported once per boot for this app" },

    /* the same fault when the app's name cannot be used (the route refused it) */
    { "capture-deaf-generic", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "An app window stopped updating",
      "dom0 keeps showing that window's last picture; close and reopen the window to get a live one.",
      "Cause: Windows delivers no new pictures of that window to the capture, even after the capture was re-created (log line QGAWGCDEAF).",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", NULL },

    /* DEGRADED on purpose: below the route's ACTION threshold, so it stays in the log (Task Scheduler's
     * restart-on-failure may bring it back; deslice-down escalates 30 s later if not). Wired so the threshold
     * is exercised by a real site and a promotion is a one-word change. */
    { "broker-died", "gui-agent", "broker-died", QERR_SEV_DEGRADED, "gui-agent.exe", NULL,
      "The notification and menu capture helper stopped serving",
      "Task Scheduler restarts it on failure (at most three times, a minute apart); the GUI agent relaunches nothing. Until it is back, menus, modern app windows and notification windows do not appear in dom0.",
      "Cause: the broker process exited or stopped answering after it was ready (log line QGABROKERDIED).",
      QERR_GUI_AGENT_LOG ", line QGABROKERDIED", NULL },

    { "deslice-down-present", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The notification and menu capture helper is not running",
      "Menus, modern app windows and notification windows do not appear in dom0 until it is back; collect the gui-agent and wgcbroker logs.",
      "Cause: wgcbroker.exe is installed but has not been running for over 30 s on a system that needs it (log line QGADESLICEDOWN).",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    { "deslice-down-missing", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The notification and menu capture helper is missing",
      "Menus, modern app windows and notification windows do not appear in dom0 until the package is reinstalled.",
      "Cause: wgcbroker.exe is missing from the install directory - a packaging gap, not a runtime fault (log line QGADESLICEDOWN).",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    { "desktop-stuck", "gui-agent", "desktop-stuck", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The guest is waiting at the sign-in or lock screen",
      QERR_TXT_DESKTOP_STUCK_NEXT,
      "Cause: a secure desktop has been up for over 30 s, and seamless mode never maps one (log line QGADESKSTUCK).",
      QERR_GUI_AGENT_LOG ", line QGADESKSTUCK", NULL },

    /* --- the notification bridge's own faults (notifhost.cpp ReportErrorSelf) --------------- */
    /* its exit codes are its own (BridgeMain): 2 = listener access denied, 3 = listener init threw */
    { "listener-denied", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe", QERR_TXT_LISTENER_DENIED_CODE,
      "The notification bridge may not read toasts",
      "Bridged apps keep the plain window path: allow notification access for this user in Settings, or turn service.notify-bridge off.",
      QERR_TXT_LISTENER_DENIED_CAUSE,
      "bridge.log in ProgramData\\qubes-toast-bridge", NULL },

    { "listener-init", "notifhost", "listener-init", QERR_SEV_ACTION, "notifhost.exe", "exit code 3",
      "The notification bridge could not start",
      "Bridged apps keep the plain window path; Task Scheduler restarts the bridge on failure (at most three times, a minute apart), the GUI agent relaunches nothing.",
      "Cause: the toast listener threw while starting (exit code 3).",
      "bridge.log in ProgramData\\qubes-toast-bridge", NULL },
};
#define QERR_TEXT_COUNT (sizeof(QerrTexts) / sizeof(QerrTexts[0]))

/* The row for a key, or NULL. A NULL at a call site is a bug of ours: QerrReportText logs it. */
static inline const QerrText* QerrTextFind(const char* key)
{
    size_t i;
    if (!key) return NULL;
    for (i = 0; i < QERR_TEXT_COUNT; i++)
        if (strcmp(QerrTexts[i].key, key) == 0) return &QerrTexts[i];
    return NULL;
}

#endif /* QWT_NOTIFYTEXTS_H */
