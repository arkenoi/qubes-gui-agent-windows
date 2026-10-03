/*
 * The exit codes the GUI agent and its watchdog service agree on.
 *
 * A START-TIME CONDITION IS NOT A DEATH (docs/ADR-supervision.md in the main repo; Jev 0.94, 2026-10-03). The agent exits with one of
 * these when it cannot run THIS BOOT for a reason dom0 fixed at VM start; the watchdog then neither relaunches it before the next boot
 * nor records a death. Bit 29 (the "customer" bit) marks an application-defined code, so none of these can be mistaken for a Win32
 * error, an NTSTATUS crash code or a C runtime exit.
 */
#pragma once

/* The qube has no GUI domain this boot: qubesdb /qubes-gui-domain-xid is absent (qvm-prefs guivm is ''). dom0 writes that key at VM
 * start when there is a GUI domain, and a guivm change takes effect only at the next start - so relaunching the agent would fail the
 * same way for as long as the qube runs (measured 2026-10-03: 47 relaunches in one boot, then one a minute). */
#define QGA_EXIT_NO_GUI_DOMAIN 0x20514701UL
