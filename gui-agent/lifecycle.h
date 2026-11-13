/*
 * lifecycle.h - the agent's END-OF-LIFE ORDER (docs/ADR-supervision.md section 5, main repo; include/qga-lifecycle.h
 * has the channel and the service's decision table).
 *
 * Two things live here. (1) THE EXIT CODE LATCH: every path that ends the main loop names WHY it ends
 * (include/qga-exitcodes.h) and the first reason latched wins; WinMain returns it, never a stale GetLastError.
 * (2) THE END-SESSION HANDSHAKE: a hidden top-level window on its own thread - created as the process's FIRST act,
 * before Init, so even an instance still initializing gets Windows' WM_QUERYENDSESSION (a message-only window
 * would not: HWND_MESSAGE windows receive no broadcasts). On it the agent disarms its helpers' relaunch first,
 * tells the QubesGuiWatchdog service "my session is ending" and waits (bounded) for the acknowledgement that no
 * agent is launched into this session again; on WM_ENDSESSION(TRUE) it leaves through the orderly exit with
 * QGA_EXIT_SESSION_END; on WM_ENDSESSION(FALSE) it re-arms and tells the service the session continues.
 */
#pragma once
#include <windows.h>
#include <log.h>   // the degrade macros below emit through _LogFormat

// The process's first act. Opens the service's channel (named by this pid; absent when the agent was started
// outside the service - logged, the handshake is then skipped) and starts the window thread, waiting a bounded
// time for the window to exist. Never fails the agent: a missing window is a loud ERROR and Init proceeds.
void LifecycleStart(void);

// Latch the reason this process is ending (a QGA_EXIT_* code). First writer wins; returns the code in force.
DWORD LifecycleLatchExit(IN DWORD code);
// The latched code, or 0 when nothing has been latched yet.
DWORD LifecycleExitCode(void);
// TRUE from WM_QUERYENDSESSION until a WM_ENDSESSION(FALSE) cancels it.
BOOL LifecycleSessionEnding(void);
// WinMain, right before it returns: the orderly exit has run. The end-session handler waits on this.
// The ORDERLY CONTRACT is satisfied - call it where the last orderly step finishes, not where the
// main thread leaves. Idempotent (the event is manual-reset).
void LifecycleOrderlyComplete(void);

void LifecycleExitDone(void);

// Defined in main.c; called from the end-session handler, in this order: disarm FIRST, then the service is told.
void HelpersDisarm(IN const WCHAR *why);
void HelpersRearm(void);
// Defined in main.c too: TRUE once the relaunch of this session's helpers has been disarmed.
BOOL HelpersDisarmed(void);
// Defined in main.c too: TRUE while the guest is in seamless mode and the session is going down
// (LifecycleSessionEnding: WM_QUERYENDSESSION, not cancelled). In seamless mode the one-shot launches of
// docs/ADR-supervision.md item 2b refuse then - the caption helper, the shadows helper, the two QGA notices and
// the error route's notice; in non-seamless mode the notices keep launching. Not this agent's own exit: an
// agent that leaves for another reason keeps its notices, because the session goes on.
BOOL SeamlessGoingDown(void);
// Defined in main.c too: copies LifecycleSessionEnding() into the toast-hold section's header, where the
// notification bridge reads it (it cannot open the SYSTEM-only going-down latch). Called by the end-session
// handler when the end starts and when it is cancelled; a no-op while the section does not exist.
void NotifHoldPublishGoingDown(void);

// THE GOING-DOWN QUESTION, answered once for the whole process. Every emitter that is about to
// complain that something is absent, lost or could not be recreated asks THIS - never its own flag.
// stopRequested is the caller's own deliberate-stop fact (a retired capture generation, a helper this
// agent stopped); pass FALSE when there is none. The decision itself lives in
// include/qga-lifecycle.h (QgaGoingDownSilences, built on the ADR-supervision section 7 predicate) so
// that the watchdog, which has its own notion of an ending session, judges it by the same rule.
BOOL LifecycleGoingDown(IN BOOL stopRequested);

// DEGRADE IN PLACE: the same line, at INFO, when a going-down is already in progress - the owner's
// "all these messages should be degraded to informational when shutdown is in progress already".
// Use these at a site whose control flow does not change; a site that also has to LEAVE calls
// LifecycleGoingDown directly. The INFO form carries QGAGOINGDOWN so a sweep can tell a degraded line
// from a line that was always informational, and the text is otherwise untouched: this changes a
// LEVEL, never the record. A death or an unexpected fault does NOT belong here - ADR-supervision
// section 1 keeps those loud at ERROR whatever is going down.
#define QGA_GOINGDOWN_TAIL L" [QGAGOINGDOWN expected: a going-down was already in progress]"
#define LogErrorUnlessGoingDown(stopRequested, format, ...)                                              \
    do {                                                                                                 \
        if (LifecycleGoingDown(stopRequested))                                                           \
            _LogFormat(LOG_LEVEL_INFO, FALSE, __FUNCTION__, L##format QGA_GOINGDOWN_TAIL, ##__VA_ARGS__); \
        else                                                                                             \
            _LogFormat(LOG_LEVEL_ERROR, FALSE, __FUNCTION__, L##format, ##__VA_ARGS__);                  \
    } while (0)
#define LogWarningUnlessGoingDown(stopRequested, format, ...)                                            \
    do {                                                                                                 \
        if (LifecycleGoingDown(stopRequested))                                                           \
            _LogFormat(LOG_LEVEL_INFO, FALSE, __FUNCTION__, L##format QGA_GOINGDOWN_TAIL, ##__VA_ARGS__); \
        else                                                                                             \
            _LogFormat(LOG_LEVEL_WARNING, FALSE, __FUNCTION__, L##format, ##__VA_ARGS__);                \
    } while (0)
