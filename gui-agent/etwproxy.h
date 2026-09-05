/*
 * etwproxy - launch + supervision of the least-privilege ETW acquisition proxy
 * (`notifhost.exe --etw-proxy`), per docs/DESIGN-p3-classifier-impl.md sec 10.14.
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
 * RegisterWaitForSingleObject on the proxy process handle; the callback schedules a
 * relaunch with exponential backoff via a one-shot timer-queue timer. There is no
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
// by the registered exit-wait. This only covers the two conditions an exit-wait cannot
// see because no process exists yet or the precondition changed under a live one:
//   * first launch: the --client-sid to put on the proxy's command line is the console
//     user's SID, so the launch must wait for a console session to exist;
//   * console user CHANGE: the pipe DACL admits exactly one SID, so a different user
//     logging on needs a proxy restart (TerminateJobObject; the exit-wait relaunches).
void EtwProxyPoke(void);

// Tear down: unregister the wait, cancel any pending relaunch timer, TerminateJobObject
// (KILL_ON_JOB_CLOSE also covers agent crash). Safe to call when never armed.
void EtwProxyShutdown(void);
