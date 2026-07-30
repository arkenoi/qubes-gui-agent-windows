# Phase 2A — event-driven window tracking in the Windows gui-agent

Replaces the per-frame `EnumWindows()` pass in `ProcessNewFrame()` with
`SetWinEventHook`-driven tracking, plus a cache of ineligible windows and a periodic
full resync as a safety net.

## Why

Measured on `win-idd-test` (Win10 LTSC 2021 19044, 4 vCPU) with the Phase 1A QGAPERF
instrumentation, 499 frames over 75 s of scripted drag/scroll/typing:

| phase | mean | p50 | max | share |
|---|---|---|---|---|
| `enu` — `AddAllWindows()` / `EnumWindows` | 31.8 ms | 23.1 ms | 177 ms | **97.5%** |
| `upd` — `UpdateWindowData()` over tracked windows | 0.58 ms | 0.37 ms | 5.6 ms | 1.8% |
| `drq`+`dmg` — damage extraction and intersection | 84 µs | 56 µs | 2.1 ms | 0.3% |
| `snd` — vchan writes | 92 µs | 58 µs | 1.5 ms | 0.3% |

The guest has ~70 top-level windows and normally exactly one of them is eligible.
`AddWindowsProc()` early-outs only for windows *already in the watched list*
(`FindWindowByHandle`), so all ~69 rejected windows fell through to
`GetWindowData()` + `ShouldAcceptWindow()` again on **every frame, forever** —
each one a `malloc`, a synchronous cross-process `WM_GETTEXT` (`GetWindowText`),
`GetClassName`, `IsWindowVisible` and a `DwmGetWindowAttribute(DWMWA_CLOAKED)` RPC to
DWM, ~340 µs apiece. Rejections were never remembered. The agent got 6.6 fps and spent
essentially all of it deciding, again, that the same 69 windows are not interesting.

Move rects are **always empty** on this path (0 in all 499 agent frames and in a
separate 25 s `ddaprobe` drag), so no move-rect handling was added — see
`instrumentation/PHASE1A-RESULT.md` in the parent repo.

## Threading decision (the load-bearing one)

`WatchForEvents()` is a `WaitForMultipleObjects(..., INFINITE)` loop, **not** a
`GetMessage` loop, and the agent has no message pump anywhere. Out-of-context WinEvent
hooks (`WINEVENT_OUTOFCONTEXT`) are delivered by the system only while the hook-owning
thread retrieves messages, so a pump had to come from somewhere. Two options:

1. **Convert the main loop to `MsgWaitForMultipleObjects`** and own the hooks there.
   Rejected. The main thread blocks for long stretches: inside `ProcessNewFrame()`
   (holding `g_csWatchedWindows`, doing DWM queries and vchan writes) and inside
   `HandleServerData()`. While it is blocked it retrieves no messages, so callbacks
   would be delayed by exactly the work we are trying to make cheap, and the system
   coalesces/drops events for a hook client that falls behind. It would also entangle
   the rework with the main loop's control flow, which is the part upstream will read
   most carefully.

2. **A dedicated hook thread** (chosen). `WindowEventThreadProc()` does nothing but
   attach to the input desktop, set the hooks, and pump; the callback appends handles
   to a bounded queue and signals the main loop. Nothing slow can ever get between an
   event and the pump. It also makes the desktop lifecycle explicit: WinEvent hooks
   only see the desktop their owning thread is attached to, and the agent re-attaches
   on every `StartFrameProcessing()` (i.e. after a desktop switch shows up as a capture
   error), so `RearmWindowEvents()` is called from there and the thread unhooks,
   re-attaches and re-hooks, then forces a resync.

The hook thread waits with `MsgWaitForMultipleObjects(2, {stop, rearm}, ..., QS_ALLINPUT)`
and drains with `PeekMessage(PM_REMOVE)`. It never touches `g_csWatchedWindows`, only
its own `g_csWindowEvents`, so there is no lock-order relationship with the main loop
(the main loop takes watched → events, never the reverse) and the callback can never
block on frame processing.

### Why the callback does no real work

`WindowEventProc()` filters (`OBJID_WINDOW`/`CHILDID_SELF`, parented to the desktop,
seamless mode on) and appends an `HWND`. No `GetWindowText`, no DWM call, no vchan
write — all of those are what made the old path expensive, and any of them in the
callback would stall the event stream for every other window. The interrogation happens
on the main loop, which is also the only thread allowed to touch the watched list.

The `objectId != OBJID_WINDOW || childId != CHILDID_SELF` test is what makes the
callback affordable at all: `EVENT_OBJECT_LOCATIONCHANGE` fires for carets and the
mouse cursor at input rate.

### Hooked events

| range | why |
|---|---|
| `EVENT_SYSTEM_MINIMIZESTART..MINIMIZEEND` | minimize/restore (`WINDOW_FLAG_MINIMIZE`) |
| `EVENT_SYSTEM_DESKTOPSWITCH` | forces a full resync |
| `EVENT_OBJECT_CREATE..HIDE` | create/destroy/show/hide — list membership |
| `EVENT_OBJECT_STATECHANGE..NAMECHANGE` | includes `LOCATIONCHANGE` (moves/resizes) and `NAMECHANGE` (`MSG_WMNAME`) |
| `EVENT_OBJECT_CLOAKED..UNCLOAKED` | `GetWindowData()` treats DWM-cloaked windows as invisible, and cloaking changes nothing else observable |

## Cache of ineligible windows, and its invalidation

`g_RejectedWindows[]` remembers windows that `ShouldAcceptWindow()` refused, together
with their creator process/thread ids, styles and `GetWindowRect()`. It is consulted
**only on the resync path**; the per-frame path does not enumerate at all, so a rejected
window is never re-interrogated because of a frame. An entry is dropped when:

* an event names the window — `ExamineWindow()` evicts before interrogating, so
  SHOW/HIDE/NAMECHANGE/LOCATIONCHANGE/STATECHANGE/CLOAKED/UNCLOAKED/CREATE/DESTROY all
  re-admit it;
* `IsWindowRejected()` finds the creator thread or process id changed (window destroyed,
  handle recycled — both ids read 0 for a dead window);
* `IsWindowRejected()` finds `GWL_STYLE`, `GWL_EXSTYLE` or the window rect changed. This
  is the backstop for a lost event: becoming visible (`WS_VISIBLE`), growing past
  `g_MinWindowWidth/Height` or changing extended styles all show up here, and all three
  checks are in-process reads, no cross-process call;
* `ResetWatch()` runs (seamless/fullscreen switch, resolution change) — the whole cache
  is cleared.

Two classes of window are **never** cached, because interrogating them has a side effect
on global state that `AddAllWindows()` recomputes from scratch each resync: the UAC
placeholder window (sets `g_ShowTaskbar`) and the Start/Search windows (set
`g_StartVisible`, which gates Search's acceptance). Caching them would freeze that state.

**HWND reuse** is handled by the creator id check above, plus the fact that
`EVENT_OBJECT_DESTROY` and `EVENT_OBJECT_CREATE` both evict. A handle recycled by the
same thread *and* reused for a window with identical styles and identical rect would
inherit the entry, but only until the next event for it, and it would have to already be
ineligible-looking on all three counts.

## Resync safety net

`TakePendingWindows()` forces a full `AddAllWindows()` when:

* it is the first pass (`g_ResyncRequested` starts `TRUE`);
* the hook thread (re)armed the hooks or saw `EVENT_SYSTEM_DESKTOPSWITCH`;
* the pending queue overflowed (`PENDING_WINDOWS_MAX` = 256 distinct handles between two
  drains) — events are never silently dropped, an overflow degrades to a resync;
* events were discarded because we were not streaming (`DiscardWindowEvents()`);
* `WINDOW_RESYNC_INTERVAL_MS` (**2000 ms**) has elapsed since the last resync.

2 s is picked so that a lost event is a glitch, not breakage, while the cost stays
negligible: a resync now interrogates only tracked windows plus windows whose cheap
signature changed, so the ~70-window scan is a few `GetWindowLong`/`GetWindowRect` calls
each (tens of µs total) instead of ~23 ms. Even in the worst case where the cache is
useless, it is one 23 ms frame every 2 s (1.2%) rather than 23 ms on every frame.
Because the resync also runs `UpdateWindowData()` over every tracked window, a missed
`LOCATIONCHANGE` can leave a window's geometry stale for at most that interval.

## Instrumentation

`enu` still measures the tracking phase (admitting new windows / the resync
enumeration), `upd` still measures refreshing tracked windows, `rem` removals — the
Phase 1A analysis and `analyze-perf.py` keep working unchanged. Added, record version
bumped to 2:

* `iwn` — windows whose state was actually queried in this frame (the direct
  before/after number: it was ~70 every frame, it should now be 0–1 on most frames);
* `wev` — window events applied in this frame (0 on a resync frame).

Tracking done *between* frames (the new `case 6` wakeup) is accumulated in
`g_Tracked*` and folded into the next frame's record, so no tracking cost can hide
outside the QGAPERF accounting.

## Behaviour deltas (deliberate)

* `MSG_CONFIGURE`/`MSG_MAP`/`MSG_WMNAME` are now sent when the event arrives, not when
  the next frame is captured — window moves reach the gui daemon at input rate. This is
  the point of `case 6` in the main loop.
* `g_ShowTaskbar` (UAC prompt → show the taskbar) is recomputed on resync rather than
  per frame, so it can lag by up to 2 s.
* Fixed on the way past: the old `AddWindowsProc()` leaked the `WINDOW_DATA` allocated
  by `GetWindowData()` for every rejected window — ~69 allocations per frame, ~7 MB/s at
  the measured frame rate. `ExamineWindow()` frees it.
* `AddAllWindows()` no longer reports a bogus `EnumWindows` error when enumeration was
  stopped by the callback because of a vchan failure; it reports the real status.

## Not verified here

This qube has no MSVC and the guest was not rebuilt in this session. Specifically:

* **It has not been compiled.** Written to be warning-clean under `/W4 /permissive- /sdl`
  with `TreatWarningAsError`, but that needs the CI job to confirm.
* **It has not been run.** No before/after numbers yet; rerun `drag-harness.ps1` +
  `analyze-perf.py` and compare `enu`, `tot`, `iwn`.
* Whether `MsgWaitForMultipleObjects` + `PeekMessage` delivers WinEvent callbacks
  reliably in this guest (it is the standard pattern, and the system also processes
  sent messages inside the wait itself, but it is untested here).
* Whether `GetAncestor(w, GA_PARENT) == GetDesktopWindow()` accepts exactly the set
  `EnumWindows()` offers on this guest. If it is stricter, the 2 s resync covers the
  difference — that would show up as windows appearing with up to 2 s delay.
* Whether any relevant transition produces *no* event at all (the cheap-signature check
  and the resync are both aimed at this, but the failure mode to watch for is "window
  visible in the guest, missing in dom0, appears within ~2 s").
* The UAC-prompt/taskbar path and the Start/Search visibility workaround were reasoned
  about, not exercised.
* `AttachToInputDesktop()` is now also called from the hook thread. It closes the handle
  returned by `GetThreadDesktop()`, which is a pre-existing upstream oddity; a second
  caller has not been observed to cause trouble, but it was not tested either.
