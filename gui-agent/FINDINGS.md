
## 2026-08-16 — guest title-bar hiding: BLOCKED, the agent cannot restyle user windows

Implemented as asked (default ON, `service.guestTitleBar` knob, inset-based discriminator), and it
does not work. Flipped to default OFF with the reason in the source.

**The discriminator is right and is proven**, from the agent's own log:

    HideGuestCaption: 0x20244: own-frame app (top inset 0), keeping its caption   <- Explorer, skipped
    HideGuestCaption: 0x1b0320: guest caption hidden (was inset 51)               <- Notepad, acted on

So Edge/Explorer/UWP (inset 0, but all carrying WS_CAPTION) are correctly excluded and only
OS-drawn captions are touched. That part stands and is reusable.

**The mechanism is refused.** After adding a read-back (the first build logged success while the
window kept its caption - intent, not outcome):

    caption strip DID NOT STICK (style 0x14cf0000 -> 0x14cf0000, prev 0, err 5)

`err 5` = ERROR_ACCESS_DENIED, on every window. Identities:

    agent   session=1 user=NT AUTHORITY\SYSTEM
    notepad session=1 user=WIN-IDD-TEST\user

**Control that identifies the cause**: the same `SetWindowLong` from a process running as
`WIN-IDD-TEST\user` succeeds (it is how the manual experiments earlier today stripped Notepad's
caption). Same session, same desktop - the difference is the account.

The agent is SYSTEM by DESIGN: `watchdog/watchdog.c:105-132` duplicates its own SYSTEM token,
sets `TokenSessionId` to the console session, and `CreateProcessAsUser`s the agent, so it can
attach to the input desktop. Running it as the user would break that.

**Routes, none taken:**
1. Impersonate the user around the call (`WTSQueryUserToken` + `ImpersonateLoggedOnUser`). Cheap to
   try, but USER32 validates cross-process window access against the PROCESS, not the thread token,
   so this is likely a no-op. UNTESTED - do not assume either way.
2. A per-user helper process the agent asks to restyle. New process, IPC and lifecycle - a real
   architectural addition for a cosmetic feature.
3. **CROP the caption out of what we announce** instead of touching the app: shift the announced
   rect down by the caption height, offset the capture, and translate injected input by the same
   amount. Needs no permissions we lack, cannot be fought by the app, and works uniformly.
   `toastcrop.c` is in-tree precedent for cropping a window's visible rect. This is the recommended
   route and it is a real piece of work, not a tweak.

The knob and the inset discriminator stay wired so route 3 (or a different identity) can reuse them.

## 2026-08-16 — OPEN: a synthesized menu falls off synthesis when its owner is dragged

Owner: "if a synthetic menu is open, it stays in place during the drag and falls off synthesis" -
and "not always but in notepad it does".

**Why Notepad specifically**: its File menu is one of the few popups that measures `synth=yes`
(contained inside the owner - verified earlier today, `#32768` at 408,350 229x196 inside a Notepad at
400,300 800x600). Menus that fall OUTSIDE the owner - Edge's 3-dot menu, most context menus - are
never synthesized, so they cannot fall off. The bug is reachable only where synthesis is reachable.

**Mechanism, from the code (`main.c:3578-3595`)**: when the owner's geometry changes, every
synthesized child is re-tested and materialized once containment breaks:

    if (!SynthQualifies(c, &stillOwner))
    {
        LogInfo("0x%x: owner geometry changed, materializing child", c->Handle);
        SynthDeactivate(c);
        c->DeletePending = TRUE;
    }

A menu is a SEPARATE top-level window with its own screen position. Dragging the owner does not move
it - it stays where it was opened. So during a drag:

1. Early in the drag the menu is still inside the owner's rect, so it stays SYNTHESIZED - painted
   into the owner's buffer. And because a drag FREEZES the owner's content (`PwDragFrozen`, no
   recapture until settle), the menu is baked into the frozen bitmap and TRAVELS WITH THE WINDOW,
   while the real menu is stationary.
2. The owner keeps moving, containment fails, the child materializes and is announced as its own
   window - at its true, stationary position.
3. The frozen owner bitmap still contains the painted menu until the settle recapture, so for a
   moment BOTH are visible.

That also explains the earlier "moments of distortion, self-heals" report: the settle-time full
recapture is what clears the stale painted copy.

**Proposed fix, NOT implemented**: materialize a dragged window's synthesized children at DRAG
START, from the input-drag latch, before any movement - so they are announced at their true screen
positions and are never baked into a frozen bitmap that moves. The machinery already exists
(`SynthDeactivate` + `DeletePending`, used by the containment path above); this only moves the
trigger earlier, from "containment broke" to "the owner is about to move".

NOT ATTEMPTED because it cannot be honestly tested from here: it needs a real dom0-driven drag with
a menu held open, and dom0 pointer injection is not available to this repo (a guest-side SendInput
drag bypasses the dom0 motion path entirely). Needs a hand test on the rig.
