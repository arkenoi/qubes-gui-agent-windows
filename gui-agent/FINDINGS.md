
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
