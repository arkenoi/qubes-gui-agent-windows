/*
 * notifytexts.h - every dom0 error notification the gui-agent and notifhost send, as data
 * (notifyerr.h has the shape and the rules; docs/DESIGN-error-notify.md section 8 the design).
 *
 * One row per notification: the route component and id, the header, line 1 (the effect and the
 * action) and the technical line's fixed pieces. The callers (main.c, notifhost.cpp) look a row up
 * by its key and hand it to QerrReportText. A text that lives in a call site cannot be rendered
 * without running the agent; a text that lives here can (QerrRenderText).
 *
 * THE STYLE is a system message's (owner 2026-11-17: "short and precise, no need for long
 * sentences and detailed explanations"; before that 2026-10-09 "way too many words", 2026-10-10
 * "too much prose"): a short title, one line of effect and action, then the facts. No cause
 * sentence: a fact the reader needs - the missing file, the setting that denies, the threshold -
 * is the technical line's code field, and the reason a fault happens is in the comment beside the
 * row and in the log line the technical line points to.
 *
 * Human names (the header never says "gui-agent" or "wgcbroker.exe"): the GUI agent, the de-slice
 * broker, the notification bridge, the ETW signal proxy - the same names guest/qwt-report-death.ps1
 * uses for the deaths of these components, so a reader meets one vocabulary.
 */
#ifndef QWT_NOTIFYTEXTS_H
#define QWT_NOTIFYTEXTS_H

#include "notifyerr.h"

#define QERR_TXT_BROKER_MISSING_HEADER "Notification and menu capture helper missing"
#define QERR_TXT_CAPTURE_LOST_NEXT "Menus, modern apps and notifications do not reach dom0."
#define QERR_TXT_DESKTOP_STUCK_NEXT "Seamless mode shows nothing. Arm autologon or use the windowed desktop."

/* The bridge's refusal (notifhost.cpp, ListenerDenialReason): ONE notice per boot, the row chosen by what denies
 * access, read at the refusal in the order that decides - a policy (group policy or device management, the same
 * setting) overrides both consent toggles, the device toggle overrides the user's. Each row names its fix; the
 * last is the refusal no setting explains, which is a bug to report, not a choice to accept. The FATAL access
 * line in bridge.log carries every value read. The Settings path is Windows 10's ("Privacy & security" on 11). */
#define QERR_TXT_LISTENER_DENIED_EVIDENCE QERR_BRIDGE_LOG ", line FATAL access"

#define QERR_GUI_AGENT_LOG "gui-agent log"
/* bridge.log is in the COMMON log directory, not the bridge's state directory. It moved there in
 * 7e349bac and these two rows kept naming ProgramData\qubes-toast-bridge, so a user following the
 * notification opened a directory with no log in it. The state directory still holds the bridge's
 * CONTROL surfaces - stop file, heartbeat, banner markers - which is what its ACLs are for. */
#define QERR_BRIDGE_LOG "bridge.log"

static const QerrText QerrTexts[] = {
    /* --- the agent's own faults (component gui-agent, one per (component, id) per boot) --- */
    /* wgcbroker.exe is not next to gui-agent.exe at start: a packaging gap, not a runtime fault (log line
     * QGABROKERMISSING). Nothing of ours relaunches it; only a reinstall of the package puts it there. */
    { "broker-missing", "gui-agent", "broker-missing", QERR_SEV_ACTION, "gui-agent.exe", "wgcbroker.exe not installed",
      QERR_TXT_BROKER_MISSING_HEADER,
      QERR_TXT_CAPTURE_LOST_NEXT " Reinstall Qubes Windows Tools.",
      QERR_GUI_AGENT_LOG ", line QGABROKERMISSING", NULL },

    /* Windows.Graphics.Capture delivers no new frames of that window, even after the capture was torn down
     * and re-created (log line QGAWGCDEAF); dom0 keeps the last frame it got. The app's image name (<= 32
     * chars, [A-Za-z0-9._-], ".exe" stripped) fills the %s; the id is capture-deaf-<app>, so each deaf app
     * is reported once per boot. */
    { "capture-deaf", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "Window stopped updating: %s",
      "dom0 shows its last picture. Close and reopen the window.",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", NULL },

    /* the same fault when the app's name cannot be used (the route refused it) */
    { "capture-deaf-generic", "gui-agent", "capture-deaf", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "App window stopped updating",
      "dom0 shows its last picture. Close and reopen the window.",
      QERR_GUI_AGENT_LOG ", line QGAWGCDEAF", NULL },

    /* DEGRADED on purpose: below the route's ACTION threshold, so it stays in the log; the dom0 row for a broker
     * death is the death reporter's, and deslice-down's is suppressed for that outage (QgaDesliceOwnedByDeath).
     * Wired so the threshold is exercised by a real site and a promotion is a one-word change.
     * Nothing brings the broker back before the next GUI agent start: Task Scheduler's restart-on-failure fires
     * only when the task cannot be launched, and the GUI agent relaunches nothing (docs/ADR-supervision.md 4-5).
     * Log line QGABROKERDIED. */
    { "broker-died", "gui-agent", "broker-died", QERR_SEV_DEGRADED, "gui-agent.exe", NULL,
      "Notification and menu capture helper stopped",
      QERR_TXT_CAPTURE_LOST_NEXT " Restart the qube.",
      QERR_GUI_AGENT_LOG ", line QGABROKERDIED", NULL },

    /* wgcbroker.exe is installed but has not been running for over 30 s on a system that needs it (log line
     * QGADESLICEDOWN): the de-slice path is down. The gui-agent and wgcbroker logs have the sequence.
     *
     * THE NEXT-STEP LINE HAS BEEN WRONG TWICE. It first said "Nothing of ours restarts it; it comes back when
     * the GUI agent next starts" and reached the owner on a guest where the launch succeeded 4 s later
     * (findings/issues.md DESLICEDOWNFIRESWHILETHELAUNCHISINFLIGHT). It was then made "Task Scheduler restarts
     * it up to three times", on docs/ADR-supervision.md 4's word, which nobody had measured. Measured
     * 2026-11-13 (25H2, the live task definition copied verbatim): RestartOnFailure fires only when the task
     * cannot be launched; an instance that ran and exited 2 or 0xC0000005 was not restarted. The line now
     * promises no restart, names the step that always works, and ties the outage to the helper running
     * again rather than to the restart: a retried launch or a new GUI agent can bring it back first.
     *
     * WHEN THIS CAN STILL FIRE, after the stand-down latch (QgaDesliceSupervisionHeld): the broker was
     * launched and never became ready. A deliberate stand-down no longer reaches here at all, and a death
     * is reported by the death reporter instead (QgaDesliceOwnedByDeath). */
    { "deslice-down-present", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", "launched, never ready",
      "Notification and menu capture helper not running",
      QERR_TXT_CAPTURE_LOST_NEXT " Restart the qube.",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    /* the same escalation when the binary is not in the install directory at all: a packaging gap, not a
     * runtime fault (log line QGADESLICEDOWN) */
    { "deslice-down-missing", "gui-agent", "deslice-down", QERR_SEV_ACTION, "gui-agent.exe", "wgcbroker.exe not installed",
      QERR_TXT_BROKER_MISSING_HEADER,
      QERR_TXT_CAPTURE_LOST_NEXT " Reinstall Qubes Windows Tools.",
      QERR_GUI_AGENT_LOG ", line QGADESLICEDOWN", NULL },

    /* A secure desktop (sign-in, lock, UAC) has been the input desktop for over 30 s (log line QGADESKSTUCK).
     * Seamless mode never maps a secure surface - each would be a standalone dom0 window indistinguishable from
     * dom0's own UI (CLAUDE.md, secure desktop) - so the frame path freezes until it goes away: autologon
     * answers the sign-in screen by itself; the windowed desktop shows it. */
    { "desktop-stuck", "gui-agent", "desktop-stuck", QERR_SEV_ACTION, "gui-agent.exe", "secure desktop for over 30 s",
      "Guest waiting at the sign-in or lock screen",
      QERR_TXT_DESKTOP_STUCK_NEXT,
      QERR_GUI_AGENT_LOG ", line QGADESKSTUCK", NULL },

    /* docs/ADR-uac.md section 7. Windows parks an elevation prompt it did not raise behind a flashing taskbar
     * button, and the agent maps a taskbar only once the shell exists - so during startup the prompt blocks
     * the program that asked and nothing leads the user to it (log line QGAUACPENDING). ACTION: only a human
     * can answer a consent prompt, and nothing in the guest will resolve it. */
    { "uac-pending", "gui-agent", "uac-pending", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "Windows permission prompt waiting, unseen",
      "The program that asked is blocked. Answer it in the windowed desktop.",
      QERR_GUI_AGENT_LOG ", line QGAUACPENDING", NULL },

    /* Owner, 2026-10-08: "normally we dont do this at all: our session is single builtin user ... now we make
     * sure session stays", and then "but if it happens, it needs to happen loud". On these guests - one
     * built-in user, autologon - the console session is not supposed to change at all, so a change is not a
     * condition to work around quietly: it goes to dom0 (log line QGANOTIFSESSION). ACTION, because nothing
     * in the guest will put it right and only a human can find out what signed out, switched user or started
     * a second session. The bridge keeps serving the session it was started in, so toasts fall back to the
     * plain window path until the qube is restarted. */
    { "session-changed", "gui-agent", "session-changed", QERR_SEV_ACTION, "gui-agent.exe", NULL,
      "Guest sign-in session changed",
      "Notifications show as plain windows. Restart the qube.",
      QERR_GUI_AGENT_LOG ", line QGANOTIFSESSION", NULL },

    /* --- the notification bridge's own faults (notifhost.cpp ReportErrorSelf) --------------- */
    /* its exit codes are its own (BridgeMain): 2 = listener access denied, 3 = listener init threw.
     * Neither is restarted: Task Scheduler's restart-on-failure fires only when the task cannot be launched,
     * and the GUI agent relaunches nothing, so the bridge starts again with the next GUI agent start. Which
     * setting denies access is read at the refusal; one row per answer. */
    { "listener-denied-policy", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe",
      "exit code 2 (policy: Force Deny)",
      "Notification access blocked by policy",
      "Set \"Let Windows apps access notifications\" to \"User is in control\", then restart the qube.",
      QERR_TXT_LISTENER_DENIED_EVIDENCE, NULL },

    { "listener-denied-device", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe",
      "exit code 2 (device access off)",
      "Notification access off for this device",
      "Allow device access in Settings > Privacy > Notifications, then restart the qube.",
      QERR_TXT_LISTENER_DENIED_EVIDENCE, NULL },

    { "listener-denied-user", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe",
      "exit code 2 (app access off)",
      "Notification access off for this user",
      "Allow app access in Settings > Privacy > Notifications, then restart the qube.",
      QERR_TXT_LISTENER_DENIED_EVIDENCE, NULL },

    { "listener-denied-unknown", "notifhost", "listener-denied", QERR_SEV_ACTION, "notifhost.exe",
      "exit code 2 (no setting denies it)",
      "Notification access refused",
      "Notifications show as plain windows. No setting explains it: please report it.",
      QERR_TXT_LISTENER_DENIED_EVIDENCE, NULL },

    { "listener-init", "notifhost", "listener-init", QERR_SEV_ACTION, "notifhost.exe",
      "exit code 3 (listener start threw)",
      "Notification bridge failed to start",
      "Notifications show as plain windows. Restart the qube.",
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
