/*
 * etwproxy - launch + supervision of the least-privilege ETW acquisition proxy
 * (`etwproxy.exe`, the GUI-DLL-free console binary split out of notifhost on 2026-09-05),
 * per docs/DESIGN-p3-classifier-impl.md sec 10.14.
 *
 * This is DELIBERATELY a separate translation unit from main.c: main.c currently
 * carries an unrelated uncommitted change (slice-map-hold, CropReadyForMap/AddWindow)
 * and the two must stay segregable at commit time. main.c's only involvement is three
 * one-line calls: EtwProxyInit (after the NotifyBridge gate read), EtwProxyPoke (on the
 * existing supervise call site in the main loop), EtwProxyShutdown (next to
 * NotifBridgeShutdown).
 *
 * The SYSTEM agent's entire involvement with the ETW tier is process lifecycle: it
 * never connects to the proxy's pipe and never reads an event (design sec 10.10.1).
 * Supervision is EXIT-WAIT, not heartbeat-poll (owner directive, sec 10.14.6/10.15):
 * RegisterWaitForSingleObject on the proxy process handle; the callback records the death
 * (ERROR + event 4004) and nothing relaunches it - the proxy is launched ONCE per agent
 * life (owner, 2026-10-07: no hand-written relaunch loops; see etwproxy.c's header for why
 * it cannot become a Task Scheduler task without weakening its sandbox). There is no
 * proxy heartbeat file at all - a hung-but-silent proxy merely leaves the bridge's ETW
 * tier down, and the bridge already degrades to the listener/DB rung (fail-open).
 */
#pragma once
#include <windows.h>

// Read the gate result once (the same g_NotifBridge decision main.c just computed) and
// arm the supervisor. With enabled=FALSE this module is inert for the whole run.
// Never fails, never blocks.
void EtwProxyInit(BOOL bridgeEnabled);

// Cheap, self-throttled (5 s) tick, called from the existing main-loop supervise site
// (next to NotifBridgeSupervise). It is NOT a health poll - process death is detected
// by the registered exit-wait. This only covers the condition an exit-wait cannot see
// because no process exists yet: the first (and only) launch waits for a console session,
// since the --client-sid on the proxy's command line is the console user's SID. A console
// user CHANGE under a running proxy is said once at ERROR and not acted on (no relaunch).
void EtwProxyPoke(void);

// Tear down: unregister the wait, TerminateJobObject (KILL_ON_JOB_CLOSE also covers agent
// crash). Safe to call when never armed.
void EtwProxyShutdown(void);

// The session-end order (main.c HelpersDisarm/HelpersRearm, docs/ADR-supervision.md 5):
// disarmed, nothing is launched and the running proxy's exit is expected (no 4004).
void EtwProxyDisarm(void);
void EtwProxyRearm(void);
