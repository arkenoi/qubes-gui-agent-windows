/*
 * notifytexts.h - every dom0 error notification the gui-agent and notifhost send, as data
 * (notifyerr.h has the shape and the rules; docs/DESIGN-error-notify.md section 8 the design).
 *
 * One row per notification: the route component and id, the header, line 1 (what it means and
 * what happens next), line 2 (the cause) and the technical line's fixed pieces. The callers
 * (main.c, notifhost.cpp) look a row up by its key and hand it to QerrReportText; the offline
 * render test (notifyrender_test.c, gcc on the dev qube via the main repo's
 * tools/tests/notify-render-selftest.sh) renders every row and holds it to the rules - header at
 * most 60 characters with no code, file name or count in it, a body of 2 to 4 lines, line 1 at
 * most 120 characters and the cause at most 100, the technical line present and shaped with the
 * pid, the code and the build, the code named by its own source's table and appearing ONCE (in
 * the technical line, never repeated in the cause), one pointer in the evidence, none of the
 * phrases the owner struck, the route's redaction accepting the text. A text that lives in a
 * call site cannot be rendered without running the agent; a text that lives here can.
 *
 * THE STYLE (owner 2026-10-09 "way too many words", 2026-10-10 "too much prose"): one short clause
 * for the condition (the header), one for the consequence (line 1), then the facts. What a line
 * used to explain - why the fault happens, that it is not the user's doing, what the retry
 * schedule is - is in the comment beside the row now, not in the text; the Evidence names the log
 * and the line tag, so the cause does not repeat them.
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
    "Seamless mode shows nothing while this lasts: arm autologon (the token is in the registry)."
#else
#define QERR_TXT_DESKTOP_STUCK_NEXT \
    "Seamless mode shows nothing while this lasts: arm autologon, or switch this qube to the windowed desktop."
#endif

#ifdef NOTIFYTEXT_DEFECT_WRONGSOURCE
#define QERR_TXT_LISTENER_DENIED_CODE  "Windows error 2"
#define QERR_TXT_LISTENER_DENIED_CAUSE "Cause: Windows error 2 (the system cannot find the file specified)."
#else
#define QERR_TXT_LISTENER_DENIED_CODE  "exit code 2"
#define QERR_TXT_LISTENER_DENIED_CAUSE "Cause: Windows refused toast-listener access for this user."
#endif

#define QERR_GUI_AGENT_LOG "gui-agent log in Qubes Logs"
/* bridge.log is in the COMMON log directory, not the bridge's state directory. It moved there in
 * 7e349bac and these two rows kept naming ProgramData\qubes-toast-bridge, so a user following the
 * notification opened a directory with no log in it. The state directory still holds the bridge's
 * CONTROL surfaces - stop file, heartbeat, banner markers - which is what its ACLs are for. */
#define QERR_BRIDGE_LOG "bridge.log in Qubes Logs"

static const QerrText QerrTexts[] = {
    /* --- the agent's own faults (component gui-agent, one per (component, id) per boot) --- */
    /* wgcbroker.exe is not next to gui-agent.exe at start: a packaging gap, not a runtime fault (log line
     * QGABROKERMISSING). Nothing of ours relaunches it; only a reinstall of the package puts it there. */
    { "broker-missing", "gui-agent", "broker-missing", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      QERR_TXT_BROKER_MISSING_HEADER,
      "Menus, modern apps and notifications do not appear in dom0 until the package is reinstalled.",
      "Cause: wgcbroker.exe is missing from the install directory.",
      QERR_GUI_AGENT_LOG ", line QGABROKERMISSING", NULL },

    /* Windows.Graphics.Capture delivers no new frames of that window, even after the capture was torn down
     * and re-created (log line QGAWGCDEAF); dom0 keeps the last frame it got. The app's image name (<= 32
     * chars, [A-Za-z0-9._-], ".exe" stripped) fills the %s; the id is capture-deaf-<app>, so each deaf app
     * is reported once per boot. */
    { "capture-deaf", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "A %s window stopped updating",
      "dom0 keeps that window's last picture; close and reopen the window.",
      "Cause: Windows delivers no new pictures of the window, even to a re-created capture.",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", "reported once per boot for this app" },

    /* the same fault when the app's name cannot be used (the route refused it) */
    { "capture-deaf-generic", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "An app window stopped updating",
      "dom0 keeps that window's last picture; close and reopen the window.",
      "Cause: Windows delivers no new pictures of the window, even to a re-created capture.",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", NULL },

    /* DEGRADED on purpose: below the route's ACTION threshold, so it stays in the log (Task Scheduler's
     * restart-on-failure may bring it back; deslice-down escalates 30 s later if not). Wired so the threshold
     * is exercised by a real site and a promotion is a one-word change. Task Scheduler restarts it on failure
     * at most three times, a minute apart; the GUI agent relaunches nothing (docs/ADR-supervision.md 4-5).
     * Log line QGABROKERDIED. */
    { "broker-died", "gui-agent", "broker-died", QERR_SEV_DEGRADED, "gui-agent.exe", NULL,
      "The notification and menu capture helper stopped serving",
      "Task Scheduler restarts it (up to three times); until then menus, modern apps and notifications do not appear in dom0.",
      "Cause: the broker exited or stopped answering after it was ready.",
      QERR_GUI_AGENT_LOG ", line QGABROKERDIED", NULL },

    /* wgcbroker.exe is installed but has not been running for over 30 s on a system that needs it (log line
     * QGADESLICEDOWN): the de-slice path is down and nothing of ours relaunches it. The gui-agent and
     * wgcbroker logs have the sequence. */
    /* A window was launched into a guest that has no desktop yet, so the de-slice broker could not be
     * started and the window waits (log line QGANOSHELLHOLD). Not a display fault: the per-window path
     * is fine, there is nothing to run the helper in. */
    { "no-shell-hold", "gui-agent", "no-shell-hold", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The guest has no desktop yet",
      "The window appears as soon as the desktop finishes starting.",
      "Cause: no shell is running in the guest's session, so the capture helper cannot start.",
      ", line QGANOSHELLHOLD" },
    { "deslice-down-present", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The notification and menu capture helper is not running",
      "Menus, modern apps and notifications do not appear in dom0 until it is back.",
      "Cause: wgcbroker.exe is installed but has not run for over 30 s.",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    /* the same escalation when the binary is not in the install directory at all: a packaging gap, not a
     * runtime fault (log line QGADESLICEDOWN) */
    { "deslice-down-missing", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The notification and menu capture helper is missing",
      "Menus, modern apps and notifications do not appear in dom0 until the package is reinstalled.",
      "Cause: wgcbroker.exe is missing from the install directory.",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    /* A secure desktop (sign-in, lock, UAC) has been the input desktop for over 30 s (log line QGADESKSTUCK).
     * Seamless mode never maps a secure surface - each would be a standalone dom0 window indistinguishable from
     * dom0's own UI (CLAUDE.md, secure desktop) - so the frame path freezes until it goes away: autologon
     * answers the sign-in screen by itself; the windowed desktop shows it. */
    { "desktop-stuck", "gui-agent", "desktop-stuck", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The guest is waiting at the sign-in or lock screen",
      QERR_TXT_DESKTOP_STUCK_NEXT,
      "Cause: a secure desktop has been up for over 30 s.",
      QERR_GUI_AGENT_LOG ", line QGADESKSTUCK", NULL },

    /* docs/ADR-uac.md section 7. Windows parks an elevation prompt it did not raise behind a flashing taskbar
     * button, and the agent maps a taskbar only once the shell exists - so during startup the prompt blocks
     * the program that asked and nothing leads the user to it (log line QGAUACPENDING). ACTION: only a human
     * can answer a consent prompt, and nothing in the guest will resolve it. */
    { "uac-pending", "gui-agent", "uac-pending", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "A Windows permission prompt is waiting, unseen",
      "The program that asked stays blocked until it is answered; switch this qube to the windowed desktop to answer it.",
      "Cause: Windows parked the prompt behind a taskbar button; none exists before the shell starts.",
      QERR_GUI_AGENT_LOG ", line QGAUACPENDING", NULL },

    /* Owner, 2026-10-08: "normally we dont do this at all: our session is single builtin user ... now we make
     * sure session stays", and then "but if it happens, it needs to happen loud". On these guests - one
     * built-in user, autologon - the console session is not supposed to change at all, so a change is not a
     * condition to work around quietly: it goes to dom0 (log line QGANOTIFSESSION). ACTION, because nothing
     * in the guest will put it right and only a human can find out what signed out, switched user or started
     * a second session. The bridge keeps serving the session it was started in, so toasts fall back to the
     * plain window path until the qube is restarted. */
    { "session-changed", "gui-agent", "session-changed", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "The guest's sign-in session changed unexpectedly",
      "Guest notifications show as plain windows until this qube is restarted; find out what signed out or switched user.",
      "Cause: the sign-in session the bridge was serving was replaced while the qube ran.",
      QERR_GUI_AGENT_LOG ", line QGANOTIFSESSION", NULL },

    /* --- the notification bridge's own faults (notifhost.cpp ReportErrorSelf) --------------- */
    /* its exit codes are its own (BridgeMain): 2 = listener access denied, 3 = listener init threw.
     * Both recur for the life of the guest: Task Scheduler restarts the bridge on failure, at most three times
     * a minute apart, and it dies the same way each time; the GUI agent relaunches nothing. Denied access is
     * Windows' notification-access setting for this user (Settings > Privacy > Notifications). */
    { "listener-denied", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe", QERR_TXT_LISTENER_DENIED_CODE,
      "The notification bridge may not read toasts",
      "Bridged apps show as plain windows: allow notification access for this user, or turn service.notify-bridge off.",
      QERR_TXT_LISTENER_DENIED_CAUSE,
      QERR_BRIDGE_LOG, NULL },

    { "listener-init", "notifhost", "listener-init", QERR_SEV_ACTION, "notifhost.exe", "exit code 3",
      "The notification bridge could not start",
      "Bridged apps show as plain windows; Task Scheduler restarts the bridge (up to three times).",
      "Cause: the toast listener threw while starting.",
      QERR_BRIDGE_LOG, NULL },
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
