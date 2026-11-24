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
 *     mapped at once. With no verdict the agent waits for as long as the banner lives; at its end
 *     (TH_BANNER_END_MS after first sight) it is NOT mapped - the record is claimed pending -> bridge by
 *     one compare-exchange and the bridge forwards the toast, said as an ERROR (QGATOASTCLAIM); a banner
 *     that ends with no record is remembered and the record listed later is claimed (QGATOASTUNSHOWN).
 *     A toast NO banner showed (banners off, do-not-disturb, Notification Center open): a record no
 *     live banner holds, still pending or window without the shown mark, TH_BANNER_END_MS after its
 *     listing, is claimed for the bridge the same way (QGATOASTNOBANNER, the no-banner scan).
 *     Never lost, never double (ADR-toasts 13, "Claim, no timer"). Bridge not running or dying
 *     mid-hold: mapped at once. A suppressed banner is re-examined on every pass: it is shown after all
 *     when the bridge turns its record to `window` (the forward failed for good) or dies before dom0
 *     acknowledged the forward (`forwarded`).
 *   * a SHOWN banner's new frames reach dom0 only after a read has named the cards they show, at most
 *     3 s later (the settle hold, ADR-toasts 13); a newer record dom0 has, or one still pending, keeps
 *     dom0 on its last frame past that. Nothing is pre-empted or unmapped for a queued toast.
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
 * transitions), QGATOASTHOLDLATE (a fail-open on bridge death, WARNING), QGATOASTCLAIM (every claim the
 * agent makes; ERROR at a banner's end), QGATOASTUNSHOWN (a banner that ended with no record or identity),
 * QGATOASTFWDLATE (no dom0 ack within the forward bound, ERROR), QGATOASTNOBANNER (a toast no banner
 * showed, claimed for the bridge, ERROR), QGATOASTIDENT (identity reads worth a
 * look: partial matches, unreadable cards).
 */
#pragma once
#include <windows.h>
#include "main.h"
#include "toastcrop.h"       // TOAST_CARD_TEXTS / TOAST_CARD_STATUS: the worker's header-free read contract
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

// main.c, at every seamless/non-seamless publish: records listed before this tick are never claimed by the no-banner scan.
void ToastHoldNoteMode(void);

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

// main.c PwConsumeBrokerFrame, BEFORE a toast banner's broker frame is copied into the buffer dom0 maps. TRUE = consume
// it without copying or damaging: a mapped banner's new frame is held until a read names the cards it shows, at most
// 3 s (ADR-toasts 13, the settle hold). An unmapped decided banner with a newer toast queued gets one read per new frame.
// Never a timer: a timed re-read kept the banner on screen (findings/issues.md TOASTBANNERLOSTONDIRECTPATH).
BOOL ToastHoldNoteFrame(IN HWND window, IN BOOL newFrame, IN BOOL mapped);

// Drop the window's state (RemoveWindow). HWNDs are recycled; a stale hold would hide the next one.
// A gate release left dom0 holding a frame from before the hold. Returns TRUE ONCE for the window that
// owes a repaint; the caller must then repaint it from its VISIBLE region, never the whole window (see
// toasthold.c for the occlusion-bleed review finding this exists to respect).
BOOL ToastHoldTakeRepaintOwed(IN HWND window);

void ToastHoldEvict(IN HWND window);

// The crop of a STACKED banner whose cards are not all shown in this window (main.c GetWindowData, before
// the decision): the contiguous run of the cards that belong here, from the last identity read's card
// rects - a card the bridge forwarded, or is still deciding, is kept out (ADR-toasts 13, no doubles).
// FALSE = no override (one card, every card shown, no fresh read): the crop worker's union stands.
BOOL ToastHoldCardInsets(IN const WINDOW_DATA* entry, OUT RECT* insets);

// TRUE when the hold already tracks this window (main.c applies its size rule only to a window it does not
// track yet, so a banner that grows is never released by its own growth).
BOOL ToastHoldTracks(IN HWND window);

// The verdict event fired: re-examine every banner this module tracks (queues a tracking pass each).
void ToastHoldOnSignal(void);

// Earliest deadline this module needs the main loop awake for (0 = nothing armed - the rest state),
// and the sweep that re-examines expired holds. Main-loop thread.
ULONGLONG ToastHoldNextDue(void);
void ToastHoldSweep(void);

// Worker side (toastcrop.c's UIA worker thread, which performed the read - every UIA call lives there):
// apply the card's three raw texts (or the read's failure) to the entry whose incarnation the request
// named; a window removed and re-added meanwhile is a new incarnation and the old reading is dropped.
void ToastHoldApplyIdentity(IN HWND window, IN LONG incarnation, IN TOAST_CARD_STATUS cardStatus,
                            IN const TOAST_CARD_TEXTS* texts);
