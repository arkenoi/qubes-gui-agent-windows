/*
 * The exit codes the GUI agent and its watchdog service agree on.
 *
 * AN EXIT CODE IS A CONTRACT (docs/ADR-supervision.md section 5 in the main repo, 2026-10-07). Measured on 2026-10-07: a requested
 * stop exited 0x0 once and 0xb7 once ("WinMain: WatchForEvents failed with error 0xb7" - a stale GetLastError), so the service could
 * not tell a requested exit from a crash, and it relaunched a session-1 agent that Windows' session teardown had just terminated
 * (0x40010004) into the SAME ending session, three instances per shutdown. WinMain now returns one of these on every path it
 * decides, never a stale error; the service decides on the code with QgaDecideAgentExit (include/qga-lifecycle.h) and nothing else.
 *
 * A START-TIME CONDITION IS NOT A DEATH (Jev 0.94, 2026-10-03): QGA_EXIT_NO_GUI_DOMAIN. Bit 29 (the "customer" bit) marks an
 * application-defined code, so none of ours can be mistaken for a Win32 error, an NTSTATUS crash code or a C runtime exit.
 */
#pragma once

/* The qube has no GUI domain this boot: qubesdb /qubes-gui-domain-xid is absent (qvm-prefs guivm is ''). dom0 writes that key at VM
 * start when there is a GUI domain, and a guivm change takes effect only at the next start - so relaunching the agent would fail the
 * same way for as long as the qube runs (measured 2026-10-03: 47 relaunches in one boot, then one a minute). */
#define QGA_EXIT_NO_GUI_DOMAIN 0x20514701UL

/* The agent was ASKED to exit through its own stop interface (Global\QGA_SHUTDOWN: the service stopping, an installer's quiesce).
 * A requested exit is not a death: no record, no relaunch - the service waits for its next launch trigger. */
#define QGA_EXIT_REQUESTED     0x20514702UL

/* Windows is ending the agent's session (WM_ENDSESSION with TRUE: a logoff or a shutdown) and the agent left through its orderly
 * exit - vchan closed and its announcement withdrawn, helpers told to leave with their relaunch disarmed first. Not a death; the
 * service never launches into that session again (gui-agent/lifecycle.c, watchdog/watchdog.c). */
#define QGA_EXIT_SESSION_END   0x20514703UL

/* dom0's gui-daemon went away or never came (the vchan closed under us, the handshake failed, no client inside the first-boot
 * budget): a fresh agent re-announcing its vchan is the reconnect. A protocol event, not a death - no record; the ONE relaunch
 * the service performs at once (Jev 1.0, 2026-10-07). */
#define QGA_EXIT_RECONNECT     0x20514704UL

/* NOT ours: DBG_TERMINATE_PROCESS, the status Windows' session teardown leaves in a process it ended itself (measured at every
 * guest shutdown, 2026-10-07). The service reads it as the system's own record of the end of that session. */
#define QGA_EXIT_SYSTEM_TERMINATED 0x40010004UL

/* The watchdog SERVICE's own service-specific exit codes (SERVICE_STATUS.dwServiceSpecificExitCode, System event 7024). The agent
 * it started died without being asked to: the service writes Application event 4001 and ends ITSELF with this code, so that the
 * SCM's recovery actions (armed by the installer: restart after 5 s, 15 s, 60 s) restart the service, which launches a new agent.
 * The death reporter joins the 7024 that carries this code to the agent's 4001 - one notification per death. */
#define QGA_SVC_EXIT_AGENT_DIED    0x20514710UL
/* The agent could not be launched at all (CreateProcessAsUser failed): nothing ran, so there is no 4001; the service ends with this
 * code and the SCM's recovery retries the launch by restarting the service. */
#define QGA_SVC_EXIT_LAUNCH_FAILED 0x20514711UL
