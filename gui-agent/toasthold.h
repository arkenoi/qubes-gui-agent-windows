/*
 * toasthold - keep a shell toast banner UNMAPPED in dom0 until THAT toast's bridge verdict is known.
 *
 * The defect (findings/issues.md P1, owner 2026-09-13 / 2026-10-04): a toast the bridge forwards to
 * dom0 also reached dom0 as the guest's own banner, captured and mapped like any toast window - the
 * first toast of every classifier-routed app showed TWICE. The per-application ShowBanner=0 switch the
 * bridge used to write could not fix it (set after the first forward, kept until the bridge exited, so
 * a later interactive toast of that app was shown NOWHERE). docs/ADR-toasts.md 10 retires the switch:
 * the agent decides per banner, from the toast's CONTENT, whether to map it at all.
 *
 * MECHANISM (the measured facts are in toastident.h):
 *   * the bridge publishes one record per listed notification (identity hashes, verdict pending/bridge/
 *     window, arrival tick) into a section the agent created, and signals an event the agent's main
 *     loop waits on - no polling, no timer at rest;
 *   * when the shell's banner window appears or changes (EVENT_OBJECT_LOCATIONCHANGE / NAMECHANGE,
 *     already hooked), the agent reads the card's identity through UI Automation on the toastcrop
 *     worker thread (bounded by the same 500 ms UIA timeouts; never inline on the tracking thread)
 *     and matches it against the records (toastident.h: content first, FIFO only to break ties);
 *   * verdict=bridge: the banner is never mapped (it times out unseen in the guest); verdict=window:
 *     mapped at once; no verdict / no record / no identity within TH_HOLD_BOUND_MS (3 s): mapped and
 *     the fall-back REPORTED (QGATOASTHOLDLATE) - a double is better than a loss, and neither is the
 *     target; bridge not running or dying mid-hold: mapped at once. A suppressed banner is re-examined
 *     on every pass: it is shown after all when the bridge turns its record to `window` (the forward
 *     failed for good) or dies before dom0 acknowledged the forward (`forwarded`).
 *   * a bridge-bound toast queued behind a mapped window-path banner pre-empts it (unmapped before the
 *     in-place swap can paint; evaluated on every pass, read in flight or not; see toasthold-core.h).
 *   * only the BANNER is held: main.c admits ShellExperienceHost surfaces under a size ceiling, and a
 *     window whose card (NormalToastView) is not found in two reads is a flyout - shown, never held.
 *   * NON-SEAMLESS mode has no hold (the guest draws its banner inside the one desktop window): the
 *     agent publishes the display mode in the shared section's header and the bridge then forwards
 *     nothing - every toast takes the window path (docs/ADR-toasts.md 10).
 *
 * Decided at START (CLAUDE.md): active iff the bridge gate is on, the ToastHoldDisable knob is not
 * set, the IPC objects exist and the UIA worker runs; nothing is re-read at runtime. When inactive,
 * every call here is a no-op and main.c maps toasts exactly as before.
 *
 * Registry (module key HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent, read once at init):
 *   ToastHoldDisable  DWORD 0/1 - escape hatch: banners map as before (bridged toasts show twice).
 *
 * Log tokens (hashes only - never toast text or window titles): QGATOASTHOLD (hold/suppress/show
 * transitions), QGATOASTHOLDLATE (every fail-open, WARNING), QGATOASTPREEMPT (pre-emption start/end),
 * QGATOASTIDENT (identity reads worth a look: partial matches, unreadable cards).
 */
#pragma once
#include <windows.h>
#include "main.h"
#include "toasthold-core.h"

// Resolve the gate once (after the NotifyBridge gate is known) and log the outcome. `bridgeGate` is
// the resolved g_NotifBridge (legacy_toasts already applied).
void ToastHoldInit(IN BOOL bridgeGate);

// Hand over the shared records section (mapped, header initialised by the caller with ThIpcInit) and
// the auto-reset verdict event the bridge signals. Both stay owned by the caller for the process life.
void ToastHoldAttachIpc(IN TH_IPC_HEADER* ipc, IN HANDLE verdictEvent);

// The event for the main loop's wait array (NULL when the hold is inactive).
HANDLE ToastHoldVerdictEvent(void);

// TRUE when the hold governs toast banners on this run (gate + knob + IPC + worker, decided at init).
BOOL ToastHoldActive(void);

// The agent's view of the bridge process: TRUE once its validated handle is held, FALSE on exit.
// Going down fails every held banner open at once (nothing can arrive any more).
void ToastHoldSetBridgeUp(IN BOOL up);

// The decision for a toast banner window on this tracking pass (main-loop thread, g_csWatchedWindows
// held). Starts the hold on first sight, requests identity reads, matches records, logs transitions,
// arms the fail-open deadline. Returns ThDecShow when the hold is inactive.
TH_DECISION ToastHoldDecide(IN const WINDOW_DATA* entry);

// A tracking pass examined this banner because of a window event: its content may have changed
// (a banner arriving in place), so re-read the identity (coalesced: one read in flight per window).
void ToastHoldNoteChanged(IN HWND window);

// Drop the window's state (RemoveWindow). HWNDs are recycled; a stale hold would hide the next one.
void ToastHoldEvict(IN HWND window);

// TRUE when the hold already tracks this window (main.c applies its size rule only to a window it does not
// track yet, so a banner that grows is never released by its own growth).
BOOL ToastHoldTracks(IN HWND window);

// The verdict event fired: re-examine every banner this module tracks (queues a tracking pass each).
void ToastHoldOnSignal(void);

// Earliest deadline this module needs the main loop awake for (0 = nothing armed - the rest state),
// and the sweep that re-examines expired holds. Main-loop thread.
ULONGLONG ToastHoldNextDue(void);
void ToastHoldSweep(void);

// Worker side (toastcrop.c's UIA worker thread, with its own IUIAutomation*): read the banner's card
// identity and apply it to the entry whose incarnation the request named (a window removed and re-added
// meanwhile is a new incarnation: the old reading is dropped). Declared with a void* so this header
// stays free of <uiautomation.h>.
void ToastHoldReadIdentity(IN void* uiaAutomation, IN HWND window, IN LONG incarnation);
