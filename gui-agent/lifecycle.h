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
void LifecycleExitDone(void);

// Defined in main.c; called from the end-session handler, in this order: disarm FIRST, then the service is told.
void HelpersDisarm(IN const WCHAR *why);
void HelpersRearm(void);
