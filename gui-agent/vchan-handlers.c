/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>

#include "common.h"
#include "main.h"
#include "vchan.h"
#include "capture.h" // CaptureRevokeStaleGrants (A6 window-0 dump ack)
#include "vchan-handlers.h"
#include "send.h"
#include "perwindow.h"
#include "xorg-keymap.h"
#include "util.h" // AttachToInputDesktop (input-injection resilience)
#include "perf.h" // g_ProtoTrace: tag configure ACKs in the trace (they are otherwise
                  // indistinguishable from genuine announces, which cost this project a
                  // whole day of misattributed evidence on the drag replay)
#include "resolution.h"
#include "toastcrop.h" // IsShellToastWindow, for the shell-surface focus quarantine

#include <config.h>
#include <log.h>

// NEVEREXIT policy (user directive 2026-08-05): the agent exits only when (a) the
// vchan is genuinely dead / the daemon disconnected, or (b) it is explicitly told to
// stop (QGA_SHUTDOWN). Every other failure is a logged, degraded, retrying state -
// each needless exit risks killing gui-daemon via the dom0 EOF-on-write bug.
// In this file that means:
//   - Every VchanReceiveBuffer failure stays FATAL. It is not just "vchan probably
//     dead": on a failed/partial receive the stream position is unknown, and a
//     handler that continued would re-parse arbitrary stream bytes as messages -
//     including MSG_KEYPRESS/MSG_BUTTON, i.e. synthesized input into the guest. This
//     fatality is a guest-side input-integrity mechanism; never convert it.
//   - Send failures (SendWindowConfigure ACKs) stay fatal: VchanSendBuffer blocks on
//     a full ring and fails only on a broken vchan - case (a).
//   - Handler failures whose message body was FULLY consumed are converted to
//     log-and-continue (InjectInput below was the prototype; SetVideoMode in
//     HandleXconf and RequestResolutionChange in HandleConfigure follow it).
//
// Input injection must NEVER kill the agent. SendInput fails transiently whenever the
// secure desktop owns input (UAC prompt, idle lock screen): measured 2026-08-05, one
// denied HandleMotion made HandleServerData exit the agent, which closed the vchan and
// took gui-daemon down with it (the user's window vanished). The event is dropped, we
// try to re-attach to the input desktop so the NEXT event can land, and processing
// continues. Dropping is protocol-safe: the message body was fully consumed already.
static DWORD InjectInput(IN INPUT* inputEvent, IN const char* what)
{
    if (SendInput(1, inputEvent, sizeof(*inputEvent)))
        return ERROR_SUCCESS;

    DWORD status = GetLastError();
    UNREFERENCED_PARAMETER(what);
    LogWarning("SendInput failed with error 0x%x - dropping input event (likely secure desktop), re-attaching input desktop", status);
    AttachToInputDesktop(); // best effort - failure means we retry on a later event
    return ERROR_SUCCESS; // deliberately never fatal
}


#include "workarea.h"
#include "perf.h"

// Protocol 1.9 proposal (DESIGN-workarea-propagation.md): daemon-sent work area.
// Defined locally until the vendored qubes-gui-protocol.h carries it; a daemon
// that never sends it costs nothing (the case is simply never dispatched).
#ifndef MSG_WORKAREA
#define QGA_MSG_WORKAREA 150
struct qga_msg_workarea
{
    uint32_t x, y, width, height;
    uint32_t frame_left, frame_right, frame_top, frame_bottom;
};
#else
#define QGA_MSG_WORKAREA MSG_WORKAREA
#define qga_msg_workarea msg_workarea
#endif

static DWORD HandleWorkarea(void)
{
    struct qga_msg_workarea m;
    if (!VchanReceiveBuffer(g_Vchan, &m, sizeof(m), L"msg_workarea"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogInfo("dom0 workarea (%u,%u) %ux%u frame %u/%u/%u/%u",
        m.x, m.y, m.width, m.height,
        m.frame_left, m.frame_right, m.frame_top, m.frame_bottom);
    WorkAreaSetDom0((int)m.x, (int)m.y, (int)m.width, (int)m.height,
        (int)m.frame_left, (int)m.frame_right, (int)m.frame_top, (int)m.frame_bottom);
    WorkAreaApply();
    return ERROR_SUCCESS;
}

// tell helper service to simulate ctrl-alt-del
static void SignalSASEvent(void)
{
    static HANDLE sasEvent = NULL;

    LogVerbose("start");
    if (!sasEvent)
    {
        sasEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, QGA_SAS_EVENT_NAME);
        if (!sasEvent)
            win_perror("OpenEvent(" QGA_SAS_EVENT_NAME L")");
    }

    if (sasEvent)
    {
        LogDebug("Setting SAS event '%s'", QGA_SAS_EVENT_NAME);
        SetEvent(sasEvent);
    }
}

DWORD HandleVersion(void)
{
    DWORD guidVersion;
    if (!VchanReceiveBuffer(g_Vchan, &guidVersion, sizeof(guidVersion), L"version"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogInfo("gui daemon version: 0x%x", guidVersion);
    PwSetDaemonVersion(guidVersion);
    return ERROR_SUCCESS;
}

DWORD HandleXconf(void)
{
    struct msg_xconf xconf;

    LogVerbose("start");
    // KEEP-FATAL: vchan receive failed - stream broken/EOF, msg_xconf body possibly
    // part-consumed; re-parsing a desynced stream is never protocol-safe (case (a)).
    if (!VchanReceiveBuffer(g_Vchan, &xconf, sizeof(xconf), L"msg_xconf"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogInfo("host resolution: %lux%lu, mem: %lu, depth: %lu", xconf.w, xconf.h, xconf.mem, xconf.depth);
    g_HostScreenWidth = xconf.w;
    g_HostScreenHeight = xconf.h;

    // if we have a resolution saved in the registry config, use that instead of xconf value
    // this is to preserve user-chosen resolution, it's saved by SetVideoMode
    DWORD fullscreenWidth = g_HostScreenWidth;
    DWORD fullscreenHeight = g_HostScreenHeight;
    const WCHAR* source = L"xconf"; // instrumentation: where the resolution request came from

    DWORD status = CfgReadDword(NULL, REG_CONFIG_FULLSCREEN_WIDTH_VALUE, &fullscreenWidth, NULL);
    if (status != ERROR_SUCCESS)
    {
        LogDebug("no saved fullscreen width, using host's (%u)", xconf.w);
        goto end;
    }

    source = L"lastapplied"; // saved FullscreenWidth/Height from the registry

    status = CfgReadDword(NULL, REG_CONFIG_FULLSCREEN_HEIGHT_VALUE, &fullscreenHeight, NULL);
    if (status != ERROR_SUCCESS)
        LogDebug("no saved fullscreen height, using host's (%u)", xconf.h);

end:
    // A4CLAMP (A4/M2b-lite): the cached FullscreenWidth/Height can be the HOST
    // screen size (saved while the window really was fullscreen). Applying it at
    // boot makes gui-daemon's dom0 window fullscreen-sized, which the WM treats as
    // maximized - wedging the window. Host-size modes are legitimate ONLY when the
    // window is actually fullscreen, so clamp the boot size to the dom0 work-area
    // maximize ceiling. HandleXconf runs BEFORE WorkAreaInit, so trigger one
    // synchronous feed read first; if the feed is unavailable, do NOT fabricate a
    // ceiling - keep the existing behavior unchanged.
    {
        int wax, way, waw, wah, wafl, wafr, waft, wafb;
        BOOL haveCeiling = FALSE;

        WorkAreaSyncReadDom0(); // result irrelevant: the accessor is the authority
        if (WorkAreaGetDom0Raw(&wax, &way, &waw, &wah, &wafl, &wafr, &waft, &wafb))
        {
            int maxW = waw - wafl - wafr;
            int maxH = wah - waft - wafb;
            if (maxW > 0 && maxH > 0)
            {
                haveCeiling = TRUE;
                if (fullscreenWidth > (DWORD)maxW || fullscreenHeight > (DWORD)maxH)
                {
                    DWORD clampedW = fullscreenWidth > (DWORD)maxW ? (DWORD)maxW : fullscreenWidth;
                    DWORD clampedH = fullscreenHeight > (DWORD)maxH ? (DWORD)maxH : fullscreenHeight;
                    LogInfo("A4CLAMP boot size %ux%u -> %ux%u (work-area ceiling)",
                        fullscreenWidth, fullscreenHeight, clampedW, clampedH);
                    fullscreenWidth = clampedW;
                    fullscreenHeight = clampedH;
                }
            }
        }
        if (!haveCeiling)
            LogDebug("A4CLAMP unavailable (no dom0 work area)");
    }

    // NEVEREXIT (CONVERT, was fatal): a resolution we couldn't set is not a reason to
    // die - the vchan is fine and capture runs at whatever mode is current. This was
    // the one HandleXconf failure that killed the agent at connect time.
    status = SetVideoMode(fullscreenWidth, fullscreenHeight, source);
    if (status != ERROR_SUCCESS)
    {
        LogError("NEVEREXIT SetVideoMode(%ux%u) failed (0x%x) - continuing at current resolution",
            fullscreenWidth, fullscreenHeight, status);
    }

    // OUTSIDE the failure branch deliberately. SetVideoMode can now return ERROR_SUCCESS WITHOUT
    // applying a mode - that is what "no supported mode fits, keep the current one" means - and
    // g_ScreenWidth/Height are written only on a successful set. Leaving this rescue inside the
    // failure branch would let a SUCCESSFUL return walk past it with the sizes still 0, and the
    // input handlers divide by them: HandleMotion's `x * 65535 / g_ScreenWidth` is a division by
    // zero on the first pointer event. The condition below is the real guard; the branch it used
    // to sit in never was.
    {
        // g_ScreenWidth/Height are only ever written on a SUCCESSFUL mode set
        // (resolution.c); at first connect they are still 0 here, and the input
        // handlers (HandleMotion/HandleButton) divide by them. Adopt the mode
        // Windows is actually in, falling back to xconf's values.
        if (g_ScreenWidth == 0 || g_ScreenHeight == 0)
        {
            DEVMODE current;
            ZeroMemory(&current, sizeof(current));
            current.dmSize = sizeof(current);
            if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &current) &&
                current.dmPelsWidth > 0 && current.dmPelsHeight > 0)
            {
                g_ScreenWidth = current.dmPelsWidth;
                g_ScreenHeight = current.dmPelsHeight;
            }
            else
            {
                g_ScreenWidth = xconf.w;
                g_ScreenHeight = xconf.h;
            }
            LogWarning("NEVEREXIT adopted %ux%u as the current screen size",
                g_ScreenWidth, g_ScreenHeight);
        }
    }
    return ERROR_SUCCESS;
}

static int BitSet(IN OUT BYTE *keys, IN int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

static BOOL IsKeyDown(IN int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static DWORD HandleKeymapNotify(void)
{
    int i;
    WORD virtualKey;
    BYTE remoteKeys[32];
    INPUT inputEvent;
    int modifierKeys[] = {
        50 /* VK_LSHIFT   */,
        37 /* VK_LCONTROL */,
        64 /* VK_LMENU    */,
        62 /* VK_RSHIFT   */,
        105 /* VK_RCONTROL */,
        108 /* VK_RMENU    */,
        133 /* VK_LWIN     */,
        134 /* VK_RWIN     */,
        135 /* VK_APPS     */,
        0
    };

    LogVerbose("start");
    if (!VchanReceiveBuffer(g_Vchan, remoteKeys, sizeof(remoteKeys), L"keymap"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    i = 0;
    while (modifierKeys[i])
    {
        virtualKey = g_X11ToVk[modifierKeys[i]];
        if (!BitSet(remoteKeys, i) && IsKeyDown(g_X11ToVk[modifierKeys[i]]))
        {
            inputEvent.type = INPUT_KEYBOARD;
            inputEvent.ki.time = 0;
            inputEvent.ki.wScan = 0; /* TODO? */
            inputEvent.ki.wVk = virtualKey;
            inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
            inputEvent.ki.dwExtraInfo = 0;

            InjectInput(&inputEvent, "SendInput");
            LogDebug("unsetting key VK=0x%x (keycode=0x%x)", virtualKey, modifierKeys[i]);
        }
        i++;
    }
    return ERROR_SUCCESS;
}

// Translates x11 keycode to physical scancode and uses that to synthesize keyboard input.
static DWORD SynthesizeKeycode(IN UINT keycode, IN BOOL release)
{
    WORD scanCode = g_KeycodeToScancode[keycode];
    INPUT inputEvent;

    // If the scancode already has 0x80 bit set, do not send key release.
    if (release && (scanCode & 0x80))
        return ERROR_SUCCESS;

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.dwExtraInfo = 0;
    inputEvent.ki.dwFlags = KEYEVENTF_SCANCODE;
    inputEvent.ki.wVk = 0; // virtual key code is not used
    inputEvent.ki.wScan = scanCode & 0xff;

    LogDebug("%S keycode: 0x%x, scancode: 0x%x, %s", g_KeycodeName[keycode], keycode, scanCode, release ? L"release" : L"press");

    if ((scanCode & 0xff00) == 0xe000) // extended key
    {
        inputEvent.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    if (release)
        inputEvent.ki.dwFlags |= KEYEVENTF_KEYUP;

    InjectInput(&inputEvent, "SendInput");

    return ERROR_SUCCESS;
}

static DWORD HandleKeypress(IN HWND window)
{
    struct msg_keypress keyMsg;
    INPUT inputEvent;
    SHORT localCapslockState;
    DWORD status;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &keyMsg, sizeof(keyMsg), L"msg_keypress"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    /* ignore x, y */
    /* TODO: send to correct window */

    // MENU-KEY BLOCK (seamless only). dom0 owns the Super/Windows key; a forwarded press
    // pops the guest Start over the seamless desktop (GWeck #44 'weird pictures') and on
    // 25H2 triggered the whole S1 garble class. Scancode-matched (keymap-independent of X
    // keycode numbering): drop Super KEY PRESSES, and swallow any key event carrying the
    // Mod4 state bit so whole Win+X chords die statelessly (the 'r' of Win+R arrives with
    // Mod4 set; dropping only the Super events would leak a bare 'r'). Super RELEASES
    // always pass, so a stuck modifier is structurally impossible. The dom0 appmenu
    // shortcut opens Start via guest-local injection and never crosses this path.
    if (g_BlockMenuKey && g_SeamlessMode)
    {
        WORD scan = g_KeycodeToScancode[keyMsg.keycode & 0xff];
        BOOL super = (scan == 0xe05b || scan == 0xe05c);
        if (super && keyMsg.type == KeyPress)
        {
            LogDebug("QGABLOCKWIN dropped Super press (keycode 0x%x)", keyMsg.keycode);
            return ERROR_SUCCESS;
        }
        if (!super && (keyMsg.state & (1u << Mod4MapIndex)))
        {
            LogDebug("QGABLOCKWIN swallowed Mod4 chord key (keycode 0x%x type %u)",
                keyMsg.keycode, keyMsg.type);
            return ERROR_SUCCESS;
        }
    }

    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.time = 0;
    inputEvent.ki.wScan = 0;
    inputEvent.ki.dwExtraInfo = 0;

    localCapslockState = GetKeyState(VK_CAPITAL) & 1;
    // check if remote CapsLock state differs from local
    // other modifiers should be synchronized in MSG_KEYMAP_NOTIFY handler
    if ((!localCapslockState) ^ (!(keyMsg.state & (1 << LockMapIndex))))
    {
        // toggle CapsLock state
        inputEvent.ki.wVk = VK_CAPITAL;
        inputEvent.ki.dwFlags = 0;
        InjectInput(&inputEvent, "SendInput(VK_CAPITAL)");
        inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;
        InjectInput(&inputEvent, "SendInput(KEYEVENTF_KEYUP)");
    }

    // produce the key press/release
    status = SynthesizeKeycode(keyMsg.keycode, keyMsg.type != KeyPress);
    if (ERROR_SUCCESS != status)
        return status;

    // TODO: allow customization of SAS sequence?
    if (IsKeyDown(VK_CONTROL) && IsKeyDown(VK_SHIFT) && IsKeyDown(VK_DELETE))
        SignalSASEvent();

    return ERROR_SUCCESS;
}

static DWORD HandleButton(IN HWND window)
{
    struct msg_button buttonMsg;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &buttonMsg, sizeof(buttonMsg), L"msg_button"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    return ProcessButtonEvent(window, buttonMsg.x, buttonMsg.y, buttonMsg.button, buttonMsg.type);
}

DWORD ProcessButtonEvent(IN HWND window, IN int bx, IN int by, IN unsigned int button,
    IN unsigned int type)
{
    struct msg_button buttonMsg;
    INPUT inputEvent;

    buttonMsg.type = type;
    buttonMsg.x = bx;
    buttonMsg.y = by;
    buttonMsg.button = button;
    buttonMsg.state = 0;

    int32_t x = buttonMsg.x;
    int32_t y = buttonMsg.y;

    // Same origin as HandleMotion, and for the same reason: dom0's coordinates are relative
    // to the rect the agent ANNOUNCED, which is the tracked X/Y - not GetWindowRect, which
    // returns the raw rect. The two differ for every window whose announced rect was
    // adjusted (DWM frame trim, and now the toast crop in toastcrop.c), and the two input
    // paths must not disagree about which space dom0 is speaking in.
    //
    // The lookup AND the field read are under g_csWatchedWindows: RemoveWindow free()s these
    // entries from the window-tracking thread, so touching data->X/Y outside the lock is a
    // use-after-free whenever a popup closes in the instant between dom0 sending the click and
    // the agent handling it - exactly the toast/menu case this coordinate change exists for.
    // HandleConfigure takes the same lock for the same reason. The two values are copied out and
    // the lock dropped at once; nothing below needs the entry itself.
    // Whether the translated (x,y) is trustworthy enough to move the pointer to. Only the
    // TRACKED origin speaks the same coordinate space as dom0 (the announced rect); the raw
    // GetWindowRect fallback is off by the announced-rect adjustment (DWM trim, toast crop
    // insets), so a positioned click through it would land measurably wrong - worse than
    // the historic click-at-last-motion semantics it would replace.
    BOOL positionTrusted = (window == NULL); // screen-relative coords need no translation
    // Function scope: the latch arm below records the exact addend this event used.
    BOOL haveTracked = FALSE;
    int32_t trackedX = 0, trackedY = 0;

    if (window)
    {
        // FROZEN ORIGIN (D1 drag wobble; see g_InputDragOrigin* in main.c). While the
        // latch holds this window every event - critically the RELEASE - translates in
        // the frame the PRESS used: with position announces withheld, dom0's applied
        // origin cannot move mid-drag, so the frozen addend is the exact one, while the
        // live tracked X/Y advances with the dragged window and would land the release
        // up to the in-flight displacement away from where dom0 aimed. A Button1 PRESS
        // never takes this path: the press is what anchors a new drag, and a stale
        // frozen origin from a lost release must not leak into it.
        if (g_InputDragFreeze && window == g_InputDragWindow && g_InputDragOriginValid &&
            !(buttonMsg.button == Button1 && buttonMsg.type == ButtonPress))
        {
            haveTracked = TRUE;
            trackedX = g_InputDragOriginX;
            trackedY = g_InputDragOriginY;
        }
        // LIVE SERVO (D1, live-feedback fix; mechanism comment at g_DragAnnounces in
        // main.c): translate buttons - critically the Button1 RELEASE - against dom0's
        // RECONSTRUCTED applied origin, not the live tracked one. The live origin leads
        // dom0's by the announce round-trip (66-250 ms measured), so a release through
        // it would land up to the in-flight displacement away from where dom0 aimed;
        // the reconstruction is dom0's own applied value to within the transit-time
        // estimate. Full deviation here, unlike the damped motion path: the release
        // must land exactly, and no further motion exists for a damped step to
        // converge through. A Button1 PRESS never takes this path (it anchors a NEW
        // drag; stale state from a lost release must not leak in), and a failed
        // reconstruction falls through to the historic live lookup.
        else if (g_InputDragServo && window == g_InputDragWindow && g_InputDragOriginValid &&
                 !(buttonMsg.button == Button1 && buttonMsg.type == ButtonPress) &&
                 DragAnnounceOriginAt(GetTickCount() - g_InputDragServoTauMs, &trackedX, &trackedY))
        {
            haveTracked = TRUE;
        }
        else
        {
            EnterCriticalSection(&g_csWatchedWindows);
            {
                const WINDOW_DATA* data = FindWindowByHandle(window);
                if (data)
                {
                    haveTracked = TRUE;
                    trackedX = data->X;
                    trackedY = data->Y;
                }
            }
            LeaveCriticalSection(&g_csWatchedWindows);
        }

        if (haveTracked)
        {
            x += trackedX;
            y += trackedY;
            positionTrusted = TRUE;
        }
        else // edge case: window might have got destroyed before we received this message
        {
            RECT rect;
            if (GetWindowRect(window, &rect))
            {
                x += rect.left;
                y += rect.top;
            }
            else
            {
                // Unlike a motion event, a button event is never dropped: swallowing a
                // release would leave the guest with a button held down forever. Inject at
                // the unadjusted origin, exactly what the zero-initialized rect did before.
                win_perror("GetWindowRect");
            }
        }
    }

    /* TODO: send to correct window */

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.dwFlags = 0;
    inputEvent.mi.time = 0;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwExtraInfo = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    // dx/dy were dead here for years: SendInput only reads them when dwFlags carries
    // MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE and the switch below never set either, so a
    // click landed wherever the LAST processed MSG_MOTION put the cursor. Any dropped,
    // stale or re-ordered motion made the click land elsewhere - the reported "toast is
    // not clickable" (docs/TOAST-fix-plan.md). The daemon sends every button with the
    // pointer position it fired at; carrying that position in the click itself makes the
    // click land there by construction. g_ButtonAbsolute=0 (registry "ButtonAbsolute")
    // restores the historic semantics; wheel events keep them unconditionally (a wheel
    // goes to whatever is under the cursor, and yanking the cursor on scroll would be a
    // behavior change nobody asked for).
    inputEvent.mi.dx = x * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = y * 65535 / g_ScreenHeight;
    switch (buttonMsg.button)
    {
    case Button1:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case Button2:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case Button3:
        inputEvent.mi.dwFlags =
            (buttonMsg.type == ButtonPress) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case Button4:
    case Button5:
        inputEvent.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inputEvent.mi.mouseData = (buttonMsg.button == Button4) ? WHEEL_DELTA : -WHEEL_DELTA;
        break;
    default:
        LogWarning("unknown button pressed/released 0x%x", buttonMsg.button);
    }

    if (g_ButtonAbsolute && positionTrusted &&
        (buttonMsg.button == Button1 || buttonMsg.button == Button2 || buttonMsg.button == Button3))
    {
        inputEvent.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    }

    // DRAG LATCH (see g_InputDragWindow in main.c): a held left button on a window means
    // the user may be dragging it, and while that lasts the frame path must not spend a
    // 15-18 ms PrintWindow per frame re-capturing content that cannot have changed. Set on
    // press, cleared on ANY release so the latch can never stick if the press and release
    // land on different windows.
    g_InputDragLastEventTick = GetTickCount();
    if (buttonMsg.button == Button1)
    {
        g_InputDragWindow = (buttonMsg.type == ButtonPress) ? window : NULL;
        // Freeze the origin at the addend THIS press just used: the shared translation
        // makes the grab offset g = r_press cancel exactly, so even a pre-drag announce
        // still in flight only shifts the transient, never the final position. Valid
        // only when the press found the window tracked - the GetWindowRect fallback
        // speaks a different space (missing DWM trim / crop insets) and must not be
        // frozen into a whole drag.
        g_InputDragOriginValid = (buttonMsg.type == ButtonPress) && haveTracked;
        g_InputDragOriginX = trackedX;
        g_InputDragOriginY = trackedY;
        // Live-servo state (see g_DragAnnounces in main.c). The grab offset is dom0's
        // window-relative press position: this press injects the guest cursor at
        // (origin + grab), so the modal move loop's own grab offset equals it by
        // construction. The announce ring is seeded with the origin THIS press
        // translated against - dom0's applied origin cannot differ from it until the
        // first mid-drag announce lands, so early reconstructions are exact. A release
        // (or an untracked press) clears the ring instead: reads are gated on the
        // latch, and a stale history must never survive into the next drag.
        // Seed the cursor servo with the position this press itself injects, so the
        // first motion event walks from a known-exact point (the press translation is
        // exact: no announce has moved dom0's origin yet).
        g_DragLastInjectedX = buttonMsg.x + trackedX;
        g_DragLastInjectedY = buttonMsg.y + trackedY;
        g_DragLastRelX = buttonMsg.x;
        g_DragLastRelY = buttonMsg.y;
        g_InputDragGrabX = buttonMsg.x;
        g_InputDragGrabY = buttonMsg.y;
        if (g_InputDragOriginValid)
            DragAnnounceReset(trackedX, trackedY);
        else
            DragAnnounceClear();

        // The single most important fact about any drag, and the trace had no line for it:
        // did the press ARM the fixed translation law? g_InputDragOriginValid is FALSE
        // whenever the press did not find the window tracked, and that silently disables the
        // whole drag fix for the gesture that follows - every motion event then falls through
        // to the live origin. `armed=0` here and `br=0` on the motion lines are the same fact
        // seen from both ends; either alone would be an inference.
        if (ProtoDragOn())
            LogInfo("QGAPROTO,msg=DRAGLATCH,hwnd=0x%x,ev=%s,armed=%d,ox=%d,oy=%d,gx=%d,gy=%d",
                (uint32_t)(ULONG_PTR)window,
                (buttonMsg.type == ButtonPress) ? L"press" : L"release",
                g_InputDragOriginValid ? 1 : 0, trackedX, trackedY, buttonMsg.x, buttonMsg.y);
    }

    if (ProtoDragOn())
        LogInfo("QGAPROTO,msg=BUTTON,hwnd=0x%x,btn=%u,type=%u,rx=%d,ry=%d,ax=%d,ay=%d",
            (uint32_t)(ULONG_PTR)window, buttonMsg.button, buttonMsg.type,
            buttonMsg.x, buttonMsg.y, x, y);

    LogDebug("window 0x%x, (%d,%d), flags 0x%x", window, buttonMsg.x, buttonMsg.y, inputEvent.mi.dwFlags);
    InjectInput(&inputEvent, "SendInput");

    return ERROR_SUCCESS;
}

// MSG_CROSSING (127): the pointer ENTERED or LEFT this window in dom0.
//
// This message was previously UNHANDLED. It fell to the default branch of the dispatch switch,
// which logged "got unknown msg type 127, ignoring" - at input rate, roughly 10 lines per second
// while the pointer moves. Two costs, both real:
//
//   1. LOST SEMANTICS. Enter/leave is how the agent learns the pointer is no longer over a
//      window. Without it the guest is never told, so state that should be released on leave
//      simply is not. Found 2026-08-30 while the owner reported Explorer showing "occlusion and
//      cursor artifacts"; the log was full of exactly this message.
//   2. FILE I/O ON THE INPUT PATH. A LogWarning per crossing event writes to disk at input rate,
//      which is squarely against Track A's purpose of finding what makes the guest feel slow.
//
// The body was at least DRAINED correctly by the default branch (it consumes untrusted_len), so
// the vchan never desynchronised - checked before writing this, because a stream desync would
// have been a far worse bug than the one being fixed.
//
// WHAT THIS HANDLER DOES, deliberately conservatively: it does not synthesise pointer input.
// Faking a WM_MOUSELEAVE or warping the cursor to force one would be guesswork about what each
// app expects, and would be visible to the user if it moved the cursor. What it DOES do is
// release the drag latch, which is a state machine we own and can reason about exactly:
//
//   The latch (g_InputDragWindow, set in HandleButton on a Button1 press) suppresses the
//   per-frame PrintWindow re-capture for the duration of a drag. It is documented as "cleared on
//   ANY release so the latch can never stick" - but that only holds if the release ARRIVES. A
//   pointer that has left the window cannot still be dragging it, so LeaveNotify is an
//   independent, authoritative signal that the drag is over. This closes the "a Button1 release
//   can be lost, leaving the window frozen until the next Button1 event anywhere" hole already
//   noted in main.c.
//
// Anything further (actual hover/cursor-ownership release) needs measurement of what the guest
// does on leave before it is written - see the FINDINGS entry. Recognising the message and
// dropping the flood is correct and safe on its own.
static DWORD HandleCrossing(IN HWND window)
{
    struct msg_crossing crossingMsg;

    if (!VchanReceiveBuffer(g_Vchan, &crossingMsg, sizeof(crossingMsg), L"msg_crossing"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogVerbose("0x%x: crossing type=%u at (%d,%d) mode=%u detail=%u",
        window, crossingMsg.type, crossingMsg.x, crossingMsg.y, crossingMsg.mode, crossingMsg.detail);

    // MODE MATTERS, and ignoring it was a regression (2026-09-01). X synthesises crossing events
    // for GRAB BOOKKEEPING as well as for real pointer motion, and gui-daemon forwards the mode
    // verbatim (xside.c process_xevent_crossing: `k.mode = ev->mode`) without filtering. A drag
    // IS a pointer grab: activating it delivers LeaveNotify with mode=NotifyGrab to the windows
    // below the grab window, and releasing it delivers the matching NotifyUngrab.
    //
    // Treating those as "the pointer left" tore down the drag state at the very moment a drag
    // began - clearing g_InputDragWindow (so InputDragFreezeContent stopped suppressing the
    // per-frame PrintWindow, bringing back the 193-211 ms startup stall) and calling
    // DragAnnounceClear(), which empties the ring the QUANTISED ORIGIN translates against. With
    // no ring the reconstruction falls back to the live origin - which is precisely the gain-1
    // oscillator that InputDragQuantise exists to remove. Net effect: both shipped drag fixes
    // silently disabled mid-drag, i.e. the wobble back at full strength.
    //
    // Only NotifyNormal means the pointer actually went somewhere. This is the standard X guard.
    //
    // AND IT IS NOT ENOUGH. MEASURED 2026-09-01 on a hand drag, which is the only thing that can
    // produce these events: 569 ms into a 5.0 s guest-native drag, dom0 delivered LeaveNotify
    // with mode=0 (NotifyNormal) - a REAL crossing, not grab bookkeeping - and this handler
    // tore the drag down. The trace shows the consequence exactly:
    //
    //     before the crossing:  50 motion events, 50/50 on the interpolated origin, 8% reversals
    //     after  the crossing: 490 motion events, 489/490 on the LIVE origin,       20% reversals
    //
    // 16-19% is the wobble as originally measured, so this single event restores it in full.
    // It also silently disables InputDragFreezeContent, DragEventPriority and the announce
    // pacing, all of which gate on the same latch - the announce rate went to 33.5/s against
    // the ~14/s the tuning was fitted at.
    //
    // WHY A GENUINE NotifyNormal LEAVE HAPPENS MID-DRAG, and why the premise of this release
    // was wrong: in a guest-native drag the WINDOW is what moves, not the pointer. Every
    // position we announce makes dom0's WM move the frame under a hand that is (relative to the
    // root) barely moving, and a window moving out from under the pointer is exactly what X
    // reports as a normal-mode LeaveNotify. "A pointer that has left the window cannot still be
    // dragging it" is false whenever the window is the thing that left.
    //
    // THE DISCRIMINATOR IS THE BUTTON, and dom0 already sends it: msg_crossing.state carries X's
    // button mask (xside.c process_xevent_crossing: `k.state = ev->state`). While button 1 is
    // held there is no such thing as "the drag ended", whatever the pointer did - so the only
    // case this release was ever written for, a LOST Button1 release, is precisely the case
    // where the mask is clear. A truly stuck latch is still caught twice over: by the next
    // Button1 event anywhere, and by the INPUT_DRAG_STUCK_MS sweep in DaemonSettleSweep.
    // NotifyInferior is excluded for the same reason as the grab modes: the pointer moved to a
    // CHILD window, i.e. it is still inside.
    if (crossingMsg.type == LeaveNotify && crossingMsg.mode == NotifyNormal &&
        crossingMsg.detail != NotifyInferior &&
        !(crossingMsg.state & Button1Mask) &&
        window && window == g_InputDragWindow)
    {
        LogDebug("0x%x: pointer left the window with no button held while the drag latch was "
            L"held - the release was lost; releasing the latch", window);
        if (ProtoDragOn())
            LogInfo("QGAPROTO,msg=DRAGLATCH,hwnd=0x%x,ev=crossing,armed=0,mode=%u,detail=%u,state=0x%x",
                (uint32_t)(ULONG_PTR)window, crossingMsg.mode, crossingMsg.detail,
                crossingMsg.state);
        g_InputDragWindow = NULL;
        g_InputDragOriginValid = FALSE;
        DragAnnounceClear();
    }
    else if (crossingMsg.type == LeaveNotify && window && window == g_InputDragWindow)
    {
        // Not a departure from a drag: a grab/ungrab crossing, a move to a child window, or -
        // the case that cost this project the whole 2026-08-30..09-01 regression - the window
        // moving out from under a hand that is still holding button 1. Logged with the fields
        // that decided it, so the next person suspecting this path can read the reason instead
        // of re-deriving it.
        LogDebug("0x%x: crossing mode=%u detail=%u state=0x%x on the dragged window - not a "
            L"departure (button held / grab bookkeeping / child window), latch KEPT",
            window, crossingMsg.mode, crossingMsg.detail, crossingMsg.state);
        if (ProtoDragOn())
            LogInfo("QGAPROTO,msg=DRAGLATCH,hwnd=0x%x,ev=crossing,armed=1,mode=%u,detail=%u,state=0x%x",
                (uint32_t)(ULONG_PTR)window, crossingMsg.mode, crossingMsg.detail,
                crossingMsg.state);
    }

    return ERROR_SUCCESS;
}

static DWORD HandleMotion(IN HWND window)
{
    struct msg_motion motionMsg;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &motionMsg, sizeof(motionMsg), L"msg_motion"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    return ProcessMotionEvent(window, motionMsg.x, motionMsg.y, motionMsg.is_hint != 0);
}

DWORD ProcessMotionEvent(IN HWND window, IN int mx, IN int my, IN BOOL isHint)
{
    struct msg_motion motionMsg;
    INPUT inputEvent;

    motionMsg.x = mx;
    motionMsg.y = my;
    motionMsg.state = 0;
    motionMsg.is_hint = isHint ? 1 : 0;

    int32_t x = motionMsg.x;
    int32_t y = motionMsg.y;

    // Which translation law this event actually took. The three fixed laws are the drag-wobble
    // fix; PTB_LIVE means the event fell through to the live tracked origin, i.e. the gain-1
    // oscillator - which is what the fix exists to prevent and what a broken latch restores.
    BYTE traceBranch = PTB_LIVE;

    g_InputDragLastEventTick = GetTickCount();
    if (motionMsg.is_hint)
    {
        LogDebug("0x%x: ignoring motion hint (%d,%d)", window, x, y);
        return ERROR_SUCCESS;
    }

    // Same locking rule as HandleButton, and pre-existing here: FindWindowByHandle's result is
    // owned by the window-tracking thread and free()d by RemoveWindow, so it may only be
    // dereferenced under g_csWatchedWindows.
    if (window)
    {
        BOOL haveTracked = FALSE;
        int32_t trackedX = 0, trackedY = 0;

        // FROZEN ORIGIN (D1 drag wobble, see HandleButton): while the latch holds this
        // window, translate against the origin captured at button-down instead of the
        // live tracked X/Y. The live origin follows the dragged window, and with the
        // measured announce-apply lag (66-250 ms vs a ~10 ms event rate) feeding it
        // back through the guest's modal move loop is a divergent oscillator - the
        // measured 40-163 px forth-and-back with 16-19% of announces reversing. With
        // announces withheld dom0's origin equals the frozen one for the whole drag,
        // so the reconstruction is exact, not damped.
        if (g_InputDragFreeze && window == g_InputDragWindow && g_InputDragOriginValid)
        {
            haveTracked = TRUE;
            traceBranch = PTB_FREEZE;
            trackedX = g_InputDragOriginX;
            trackedY = g_InputDragOriginY;
        }
        // QUANTISED ORIGIN: dom0's origin is not unknown - it is the position we last announced.
        // Translate against the newest announce dom0 has certainly applied, so the addend is exact
        // rather than leading (live) or predicted (servo). The event's own announce cannot move the
        // origin the event used, which is precisely what breaks the gain-1 loop.
        // INTERPOLATED ORIGIN: evaluate dom0's origin at (now - measured lag) by ramping between the
        // bracketing announces, rather than holding one announce and switching all at once. Removes
        // the step discontinuity the quantised law leaves behind, without adding lag. Falls through
        // to the quantised branch when off or when the ring cannot answer.
        else if (g_InputDragOriginInterp && window == g_InputDragWindow && g_InputDragOriginValid &&
                 DragAnnounceOriginAt(GetTickCount() - g_InputDragLagMs, &trackedX, &trackedY))
        {
            haveTracked = TRUE;
            traceBranch = PTB_INTERP;
        }
        else if (g_InputDragQuantise && window == g_InputDragWindow && g_InputDragOriginValid &&
                 DragAnnounceAppliedOrigin(g_InputDragAdoptMs, &trackedX, &trackedY))
        {
            haveTracked = TRUE;
            traceBranch = PTB_QUANT;
        }
        else
        {
            EnterCriticalSection(&g_csWatchedWindows);
            {
                const WINDOW_DATA* data = FindWindowByHandle(window);
                if (data)
                {
                    haveTracked = TRUE;
                    trackedX = data->X;
                    trackedY = data->Y;
                }
            }
            LeaveCriticalSection(&g_csWatchedWindows);

            // LIVE SERVO (D1 drag wobble, live-feedback fix; mechanism comment at
            // g_DragAnnounces in main.c). The plain live translation below this branch
            // is A = r + W_live; the app's modal move loop applies W' = A - g, closing
            // a gain-1 servo through the 66-250 ms announce-apply transport lag -
            // structurally oscillatory over the whole measured lag range (roots
            // |z| = 1.00-1.19), the observed 40-163 px forth-and-back. Restructure the
            // law instead of freezing the window: reconstruct the dom0 cursor
            // C_hat = r + D_hat from our own announce history (which removes the lag
            // from the loop equation - single real pole at 1-beta, stable for ANY
            // transport lag), then move the window a FRACTION beta of the remaining
            // deviation per event. The 3 px default deadband absorbs announce
            // quantization so a stationary hand yields a stationary window. Cost of
            // the detune: ~33 px of extra cursor trail at the measured p50 hand speed,
            // on top of the ~83 px irreducible pipeline lag even a perfect estimator
            // shows. A failed reconstruction (no seeded ring) leaves the historic
            // live-origin translation untouched.
            if (haveTracked && g_InputDragServo &&
                window == g_InputDragWindow && g_InputDragOriginValid)
            {
                int dhatX, dhatY;
                if (DragAnnounceOriginAt(GetTickCount() - g_InputDragServoTauMs, &dhatX, &dhatY))
                {
                    if (!DragAnnounceMoved())
                    {
                        // No position announce since the press: dom0's applied origin
                        // IS the press origin, the reconstruction is exact and no
                        // feedback loop exists to damp. This is every client-area drag
                        // (selection, scrollbars - the window never moves, so this
                        // branch carries the whole gesture) and the first ~66 ms of a
                        // title-bar drag. Damping here would be pure harm: a selection
                        // cursor would undershoot by (1-beta) of its pull.
                        trackedX = dhatX;
                        trackedY = dhatY;
                    }
                    else
                    {
                        // SERVO THE CURSOR, NOT THE WINDOW. Deviation between the
                        // reconstructed dom0 cursor and the cursor we last injected;
                        // the injected cursor then walks toward the real one by beta.
                        //
                        // Deliberately NOT expressed against the grab offset: Windows
                        // RE-ANCHORS the modal loop's grab mid-drag (dragging a
                        // maximized or snapped window to restore it re-anchors it
                        // proportionally under the cursor), and a law written against a
                        // grab captured at the press then has a fixed point offset by
                        // (grab - grab_new)*(1/beta - 1) - a constant several-hundred-px
                        // tracking error for a restore gesture (review finding, with an
                        // analytic fixed point and a simulation agreeing at ~390px).
                        // Servoing the cursor removes the grab from the loop entirely,
                        // so whatever grab the modal loop currently holds is applied by
                        // Windows itself and cancels. It also fixes left/top
                        // resize-border drags, which re-anchor the same way.
                        int cHatX = x + dhatX;
                        int cHatY = y + dhatY;
                        int devX = cHatX - g_DragLastInjectedX;
                        int devY = cHatY - g_DragLastInjectedY;
                        int db = (int)g_InputDragServoDeadband;
                        if (devX <= db && devX >= -db)
                            devX = 0;
                        if (devY <= db && devY >= -db)
                            devY = 0;
                        // GAIN SCHEDULING (user request 2026-08-13: "first jump
                        // immediately and then adapt to the speed"). The damping exists
                        // only to absorb prediction error near the settling point, where
                        // an over-correction would ring. A LARGE deviation is not a
                        // prediction error - it is the hand genuinely moving fast, and
                        // damping it just makes the window trail ("slow moves fine, fast
                        // ones look off"). So: apply a large deviation in FULL and reserve
                        // the damped gain for the small ones. Per axis, because a drag is
                        // usually fast on one axis and settling on the other.
                        int gainX = (devX >= (int)g_InputDragServoFastPx ||
                                     devX <= -(int)g_InputDragServoFastPx)
                                    ? (int)g_InputDragServoFastGainPct
                                    : (int)g_InputDragServoGainPct;
                        int gainY = (devY >= (int)g_InputDragServoFastPx ||
                                     devY <= -(int)g_InputDragServoFastPx)
                                    ? (int)g_InputDragServoFastGainPct
                                    : (int)g_InputDragServoGainPct;
                        int stepX = devX * gainX / 100;
                        int stepY = devY * gainY / 100;
                        if (g_InputDragServoClamp)
                        {
                            // NEVER MOVE FURTHER THAN THE HAND DID. dom0's relative
                            // coordinate moved by (x,y) minus the previous relative
                            // position, and no legitimate cursor motion can exceed that
                            // plus the window's own travel since the last event. A wrong
                            // origin reconstruction can therefore make the injected
                            // cursor LAG, but never overshoot - which is what produced
                            // the 'crazy extrapolated jumps' when a bad estimate was
                            // applied at full gain (user, 2026-08-13). The bound is
                            // deliberately generous (2x + 32px) so it never throttles a
                            // genuinely fast hand; it exists to cap nonsense, not to
                            // shape normal motion.
                            int dRelX = x - g_DragLastRelX;
                            int dRelY = y - g_DragLastRelY;
                            if (dRelX < 0) dRelX = -dRelX;
                            if (dRelY < 0) dRelY = -dRelY;
                            int limX = 2 * dRelX + 32;
                            int limY = 2 * dRelY + 32;
                            if (stepX >  limX) stepX =  limX;
                            if (stepX < -limX) stepX = -limX;
                            if (stepY >  limY) stepY =  limY;
                            if (stepY < -limY) stepY = -limY;
                        }
                        g_DragLastInjectedX += stepX;
                        g_DragLastInjectedY += stepY;
                        // Express the target as the addend the shared translation below
                        // applies to the window-relative (x,y).
                        trackedX = g_DragLastInjectedX - x;
                        trackedY = g_DragLastInjectedY - y;
                        traceBranch = PTB_SERVO;
                    }
                }
            }
        }

        if (haveTracked)
        {
            x += trackedX;
            y += trackedY;
        }
        else // edge case: window might have got destroyed before we received this message
        {
            RECT rect;
            if (GetWindowRect(window, &rect))
            {
                x += rect.left;
                y += rect.top;
                trackedX = rect.left;
                trackedY = rect.top;
                traceBranch = PTB_RAWRECT;
            }
            else
            {
                win_perror("GetWindowRect");
                return ERROR_SUCCESS; // ignore
            }
        }

        LogVerbose("0x%x: (%d,%d)", window, x, y);

        // The other half of the drag record. Announces alone say where we PUT the window;
        // this says what dom0 told us and which origin we reconstructed against, which is
        // what makes dom0's true apply lag recoverable offline: replay these events against
        // the announce history at a candidate lag L and the L that minimises the injected
        // path's reversals is dom0's real lag. InputDragAdoptMs (70 ms) was never measured
        // against it - it was chosen conservatively - and the announce pacing is bounded
        // below by that choice, so this is the measurement that could tighten both.
        //
        // TWO FIELDS ADDED 2026-09-01, because the drag question could not be answered from
        // the line as it stood:
        //   br= WHICH TRANSLATION LAW this event actually took (PTB_* in perf.h). `trk` only
        //       said "an origin was found"; it is 1 for the live origin too, and the live
        //       origin IS the gain-1 oscillator the fix exists to remove. Without br a trace
        //       cannot distinguish "the fix applied and still wobbles" from "the fix was
        //       never armed", which is precisely the fork the 2026-09-01 session was stuck on.
        //   wx/wy WHERE THE WINDOW ACTUALLY IS at this event. Announces are paced (50 ms)
        //       while motion arrives at ~45 Hz, so the announce stream alone cannot show a
        //       window moving backwards between announces - and moving backwards is the
        //       symptom being chased.
        if (ProtoDragOn())
        {
            RECT wr = { 0 };
            (void)GetWindowRect(window, &wr);
            LogInfo("QGAPROTO,msg=MOTION,hwnd=0x%x,rx=%d,ry=%d,ox=%d,oy=%d,ax=%d,ay=%d,trk=%d,"
                L"br=%u,wx=%d,wy=%d",
                (uint32_t)(ULONG_PTR)window, motionMsg.x, motionMsg.y,
                haveTracked ? trackedX : 0, haveTracked ? trackedY : 0, x, y, haveTracked ? 1 : 0,
                traceBranch, wr.left, wr.top);
        }
    }

    // Relative coordinates of THIS motion, for the next event's clamp bound.
    g_DragLastRelX = motionMsg.x;
    g_DragLastRelY = motionMsg.y;

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.time = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = x * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = y * 65535 / g_ScreenHeight;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputEvent.mi.dwExtraInfo = 0;

    InjectInput(&inputEvent, "SendInput");

    return ERROR_SUCCESS;
}

static DWORD HandleConfigure(IN HWND window, BOOL replyToMessages)
{
    struct msg_configure configureMsg;

    if (!VchanReceiveBuffer(g_Vchan, &configureMsg, sizeof(configureMsg), L"msg_configure"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogDebug("0x%x: (%d,%d) %dx%d", window, configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height);

    if (window != 0) // 0 is full screen
    {
        // TRUE only when this configure actually carried geometry we act on. Ignored
        // configures (iconic, maximized, byte-identical geometry) must not arm the
        // daemon-drive machinery: holding damage for a window whose configures we ignore
        // buys nothing and costs full-window settle repaints (review finding).
        BOOL geometryDriven = FALSE;
        EnterCriticalSection(&g_csWatchedWindows);
        WINDOW_DATA* data = FindWindowByHandle(window);
        if (data && data->Synthesized)
        {
            // The daemon cannot legitimately reference a synthesized window (never
            // announced); treat it as untracked so no ACK is sent back.
            LogWarning("configure for synthesized window 0x%x, ignoring", window);
            data = NULL;
        }

        if (data != NULL)
        {
            // Is a GUEST-NATIVE drag holding this window right now? Both the trace line and
            // the guard below need it, and it is the fact the inbound path never considered.
            const BOOL dragHeld = (window == g_InputDragWindow && g_InputDragOriginValid);

            // WHAT THE DAEMON JUST TOLD US, next to what we believe. `cx-tx` is the whole
            // story of the suspected defect: during a fast guest-native drag our own announce
            // comes back after the window has already moved on, so the agent sees a position
            // that differs from data->X - reads it as a dom0 ORDER rather than as its own echo
            // - and ApplyPendingDaemonMove then SetWindowPos'es the window BACKWARDS, out from
            // under the user's cursor. Whether that actually happens was unobservable: there
            // was no line here at all, only the ACK line further down, which is emitted for
            // every configure and says nothing about what was decided.
            if (ProtoDragOn())
                LogInfo("QGAPROTO,msg=CONFIGURE-IN,hwnd=0x%x,cx=%d,cy=%d,cw=%u,ch=%u,"
                    L"tx=%d,ty=%d,drag=%d,iconic=%d",
                    (uint32_t)(ULONG_PTR)window, configureMsg.x, configureMsg.y,
                    configureMsg.width, configureMsg.height, data->X, data->Y,
                    dragHeld ? 1 : 0, data->IsIconic ? 1 : 0);

            // INPUT-DRAG CONFIGURE GUARD (InputDragCfgGuard, default off until measured).
            // The mirror of DAEMON-DRIVE SUPPRESSION: whoever owns the drag owns the
            // position. While the user's hand drags this window inside the guest, geometry
            // arriving from the daemon is not an instruction - it is either our own announce
            // coming back late or dom0's WM constraining a position the hand has already left.
            // Applying it fights the modal move loop. The ACK below is still sent, so the
            // daemon's queued-configure bookkeeping clears exactly as before, and the
            // tracking pass announces the true resting place when the drag ends.
            if (g_InputDragCfgGuard && dragHeld)
            {
                LogDebug("0x%x: configure (%d,%d) ignored - a guest-native drag owns this "
                    L"window's position (tracked %d,%d)",
                    window, configureMsg.x, configureMsg.y, data->X, data->Y);
            }
            else if (data->IsIconic)
            {
                LogVerbose("0x%x is minimized, ignoring", window);
            }
            else if (IsZoomed(window))
            {
                // A maximized window is anchored by the guest WM: applying dom0 geometry
                // via SetWindowPos either bounces (the window snaps back) or drags the
                // maximized frame off its anchor - both feed an endless CONFIGURE
                // ping-pong with the dom0 WM, rebuilding the per-window grant on every
                // flip. Ignore the request; the ACK below re-syncs the daemon to the
                // geometry the guest actually has.
                LogDebug("0x%x is maximized, ignoring dom0 configure %dx%d",
                    window, configureMsg.width, configureMsg.height);
                // ...but remember the size the dom0 WM can actually display, so the
                // tracking pass reports/grants exactly that (see DaemonMax* in main.h).
                if (configureMsg.width > 0 && configureMsg.height > 0)
                {
                    data->DaemonMaxValid = TRUE;
                    data->DaemonMaxW = configureMsg.width;
                    data->DaemonMaxH = configureMsg.height;
                }
            }
            else if (data->CropLeft != 0 || data->CropTop != 0 ||
                data->CropRight != 0 || data->CropBottom != 0)
            {
                // These coordinates are in CROPPED space - the rect the agent announced is
                // the visible card, not the window (toastcrop.c).
                //
                // LATEST-WINS, same as the normal branch below: the newest card-space
                // geometry is stashed and ApplyPendingDaemonMove posts at most one async
                // SetWindowPos per window (it adds the crop insets back itself - raw
                // origin = card origin - insets - on top of the announce/SetWindowPos
                // delta, which is zero for frameless shell CoreWindows). A dom0-WM drag
                // of a managed shell surface used to issue one SetWindowPos per configure
                // - the same queued-moves replay the normal branch was cured of (review
                // finding, closed 2026-08-12). SIZE is never applied: the announced size
                // is card size, the HWND's is larger by the insets, and shell surfaces
                // size themselves; the NoSize stash flag keeps ShellExperienceHost's
                // layout intact.
                if (data->X == configureMsg.x && data->Y == configureMsg.y)
                {
                    LogVerbose("0x%x cropped, position unchanged", window);
                }
                else
                {
                    // FROZEN ANCHOR. dom0 has placed this shell surface; from now on dom0
                    // OWNS its position. The guest HWND is deliberately NOT moved and
                    // data->X/Y deliberately NOT updated: X/Y is the slice source (the
                    // window's real position, where the shell actually paints its card)
                    // and the origin every damage rect and injected click is translated
                    // against. Moving it - or merely pretending it moved - is what made a
                    // dragged Start slice bare wallpaper (user-reported 2026-08-12).
                    // Because slice-fed surfaces are copied into their own per-window
                    // buffer, dom0 renders the card correctly at whatever position the
                    // user dragged the frame to.
                    geometryDriven = TRUE;
                    data->DaemonOwnsPos = TRUE;

                    LogDebug("0x%x cropped by %d/%d/%d/%d: dom0 placed it at (%d,%d); "
                        "keeping guest anchor (%d,%d), dom0 owns position",
                        window, data->CropLeft, data->CropTop, data->CropRight, data->CropBottom,
                        configureMsg.x, configureMsg.y, data->X, data->Y);

                    // Record the daemon's own values as last-sent so nothing echoes them
                    // back at it (the normal branch does the same for its dictated move).
                    data->CfgSentValid = TRUE;
                    data->LastCfgX = configureMsg.x;
                    data->LastCfgY = configureMsg.y;
                    data->LastCfgW = (int)data->Width;
                    data->LastCfgH = (int)data->Height;
                    data->LastCfgOvr = data->IsOverrideRedirect;
                }
            }
            else
            {
                BOOL noMove = (data->X == configureMsg.x && data->Y == configureMsg.y);
                BOOL noSize = (data->Width == configureMsg.width && data->Height == configureMsg.height);

                if (noMove && noSize)
                {
                    LogVerbose("0x%x: geometry unchanged", window);
                }
                else
                {
                    // LATEST-WINS: stash the newest daemon geometry instead of issuing a
                    // per-message async SetWindowPos. During a dom0 WM title-bar drag the
                    // daemon streams these at input rate; the per-message flood queued
                    // dozens of async moves the guest window then played back over
                    // seconds after release, and the frame path re-announced every
                    // lagging step - dom0 replayed the whole drag path (user-reproduced
                    // 2026-08-12, 1:1 trace in FINDINGS.md). ApplyPendingDaemonMove posts
                    // at most one in-flight move per window and converts announce-space
                    // coords to SetWindowPos space (the old direct call landed every
                    // move off by the DWM invisible-border delta).
                    geometryDriven = TRUE;
                    if (noMove && data->DaemonMovePending && !data->DaemonMoveNoMove)
                    {
                        // A dictated position is still waiting (in-flight gated, never
                        // posted). noMove was computed against the OPTIMISTIC data->X/Y
                        // (already equal to that pending position), so overwriting the
                        // stash with NoMove would silently drop the un-applied move and
                        // leave the window at its old origin (review finding). Keep the
                        // pending position and its move flag; merge in the newer size.
                        data->DaemonMoveW = configureMsg.width;
                        data->DaemonMoveH = configureMsg.height;
                        data->DaemonMoveNoSize = noSize;
                    }
                    else
                    {
                        data->DaemonMovePending = TRUE;
                        data->DaemonMoveX = configureMsg.x;
                        data->DaemonMoveY = configureMsg.y;
                        data->DaemonMoveW = configureMsg.width;
                        data->DaemonMoveH = configureMsg.height;
                        data->DaemonMoveNoMove = noMove;
                        data->DaemonMoveNoSize = noSize;
                    }

                    // Expected pos/size updated without waiting for the actual change,
                    // as before (the tracking pass self-corrects if the apply diverges).
                    if (!noMove)
                    {
                        LogVerbose("Updating position of 0x%x: (%d,%d) -> (%d,%d)", window, data->X, data->Y,
                            configureMsg.x, configureMsg.y);
                        data->X = configureMsg.x;
                        data->Y = configureMsg.y;
                    }

                    if (!noSize)
                    {
                        LogVerbose("Updating size of 0x%x: %dx%d -> %dx%d", window, data->Width, data->Height,
                            configureMsg.width, configureMsg.height);
                        data->Width = configureMsg.width;
                        data->Height = configureMsg.height;
                    }

                    // Inference sample for the work-area sync: a daemon-dictated
                    // origin reveals the dom0 panel + frame margins.
                    WorkAreaNoteDaemonOrigin(configureMsg.x, configureMsg.y);

                    // The daemon knows this geometry (it dictated it); record it as
                    // last-sent so the tracking pass does not echo it back.
                    data->CfgSentValid = TRUE;
                    data->LastCfgX = data->X;
                    data->LastCfgY = data->Y;
                    data->LastCfgW = (int)data->Width;
                    data->LastCfgH = (int)data->Height;
                    data->LastCfgOvr = data->IsOverrideRedirect;

                    // Apply immediately if nothing is in flight (the common non-drag
                    // case: exactly one configure -> exactly one SetWindowPos, applied
                    // here); during a flood the drain-end/per-frame pass picks it up.
                    ApplyPendingDaemonMove(data);
                }
            }

            // The daemon is actively dictating this window's geometry: hold the
            // tracking/frame paths' position-only announces (see DAEMON-DRIVE
            // SUPPRESSION in SendWindowConfigureIfChanged) - echoing the guest
            // window's lagging position at the daemon is what fought the dom0 WM
            // during drags and replayed the trajectory after release.
            if (geometryDriven)
            {
                DWORD driveNow = GetTickCount();
                // Two geometry-carrying configures within the window = a stream (dom0 WM
                // drag at input rate); only a stream suppresses announces and holds
                // damage. A lone placement configure stamps DriveTick only, so a
                // freshly-mapped window's first paint is never delayed - and ignored
                // configures (iconic/maximized/unchanged) stamp nothing at all.
                if (data->DaemonDriveTick != 0 &&
                    (driveNow - data->DaemonDriveTick) < DAEMON_DRIVE_ACTIVE_MS)
                    data->DaemonStreamTick = driveNow;
                data->DaemonDriveTick = driveNow;
            }
        }
        else
        {
            LogWarning("window 0x%x not tracked", window);
        }

        if (replyToMessages && data != NULL)
        {
            // ACK to the gui daemon so it won't stop sending MSG_CONFIGURE. It MUST
            // byte-echo the daemon's own values: the daemon recognizes the echo and
            // no-ops it. ACKing anything else (tried: the agent's actual geometry for
            // maximized windows) is processed as a real configure request - the daemon
            // moveresizes its X window, the resulting ConfigureNotify emits a fresh
            // MSG_CONFIGURE, and the pair spins at vchan speed. (The daemon ignores the
            // override_redirect field of agent configures - xside.c:2105 - so echoing
            // it is safe.) Sent under g_csWatchedWindows so window removal
            // (UNMAP/DESTROY, also under this lock) cannot interleave: an ACK for a
            // just-destroyed window would hit the daemon's "msg without CREATE"
            // exit(1). For the same reason no ACK at all for untracked windows.
            // KEEP-FATAL (propagated): SendWindowConfigure only fails when the vchan
            // write fails (VchanSendBuffer blocks rather than failing on a full
            // ring), i.e. the vchan is broken - case (a).
            if (ProtoDragOn())
                LogInfo("QGAPROTO,msg=CONFIGURE-ACK,hwnd=0x%x,x=%d,y=%d",
                    (uint32_t)(ULONG_PTR)window, configureMsg.x, configureMsg.y);
            ULONG ackStatus = SendWindowConfigure(window,
                configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height,
                configureMsg.override_redirect);
            LeaveCriticalSection(&g_csWatchedWindows);
            return ackStatus;
        }
        LeaveCriticalSection(&g_csWatchedWindows);
        return ERROR_SUCCESS;
    }
    else
    {
        // gui daemon requests screen resize: possible resolution change
        BOOL valid = TRUE;

        if (g_ScreenWidth == configureMsg.width && g_ScreenHeight == configureMsg.height)
        {
            valid = FALSE;
            // nothing changes, ignore
        }

        if (!IS_RESOLUTION_VALID(configureMsg.width, configureMsg.height))
        {
            LogWarning("Ignoring invalid resolution %ux%u", configureMsg.width, configureMsg.height);
            valid = FALSE;
        }

        if (valid)
        {
            // Remember where dom0's window actually sits: our own w0 configure
            // after a resize must echo THIS position, not (0,0) - sending origin
            // made the daemon move the client to x=0 and pushed the left frame
            // border off-screen (user-reported after every snap).
            g_ScreenWinX = configureMsg.x;
            g_ScreenWinY = configureMsg.y;
            DWORD status = RequestResolutionChange(configureMsg.width, configureMsg.height, L"dom0");
            if (status != ERROR_SUCCESS)
            {
                // NEVEREXIT (CONVERT, was fatal): the msg_configure body is fully
                // consumed, so dropping the request is protocol-safe. Failure here is
                // a local resource problem (event/thread creation) - keep the current
                // resolution, still ACK below, keep running.
                win_perror2(status, "requesting resolution change");
                LogWarning("NEVEREXIT resolution change request %ux%u dropped - keeping current resolution",
                    configureMsg.width, configureMsg.height);
            }
        }
    }

    // Screen (window 0) ACK: the screen window is never destroyed, no ordering hazard.
    // KEEP-FATAL (propagated): send failure = broken vchan, case (a) - see the
    // per-window ACK above.
    if (replyToMessages)
    {
        if (g_ProtoTrace)
            LogInfo("QGAPROTO,msg=CONFIGURE-ACK,hwnd=0x%x,x=%d,y=%d",
                (uint32_t)(ULONG_PTR)window, configureMsg.x, configureMsg.y);
        return SendWindowConfigure(window,
            configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height, configureMsg.override_redirect);
    }

    return ERROR_SUCCESS;
}

static DWORD HandleFocus(IN HWND window)
{
    struct msg_focus focusMsg;

    if (!VchanReceiveBuffer(g_Vchan, &focusMsg, sizeof(focusMsg), L"msg_focus"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }
    LogVerbose("0x%x: type %x, mode %x, detail %x", window, focusMsg.type, focusMsg.mode, focusMsg.detail);

    if (focusMsg.type == 9) // focus gain
    {
        EnterCriticalSection(&g_csWatchedWindows);
        WINDOW_DATA* data = FindWindowByHandle(window);
        if (!data)
        {
            LogWarning("window 0x%x not tracked", window);
        }
        else if (g_ShellManaged != SHELL_MANAGED_NONE &&
                 (data->CropLeft || data->CropTop || data->CropRight ||
                 data->CropBottom || IsShellToastWindow(data)))
        {
            // A WM-managed toast gets X focus the moment the dom0 WM maps or the user
            // clicks its frame; forwarding that as SetForegroundWindow(ShellExperienceHost)
            // would yank guest keyboard focus from whatever the user is typing in every
            // time a notification pops - a regression the movability feature must not
            // introduce. Clicks inside the toast still work: ButtonAbsolute positions each
            // click, and toast buttons respond to mouse without foreground status. (The
            // Start menu keeps focus normally: it is foreground already from opening, and
            // this branch only skips the FORWARDING of dom0 focus, not guest focus itself.)
            LogDebug("0x%x: shell surface, not forwarding dom0 focus", window);
        }
        else
        {
            if (data->IsIconic)
                ShowWindow(window, SW_RESTORE);

            // FOREGROUND LOCK. SetForegroundWindow SILENTLY FAILS - returns FALSE, changes
            // nothing - when another process owns the foreground and Windows' foreground lock is
            // in force. Its result was previously ignored, so the failure was invisible: dom0
            // said "focus this window", the guest quietly did nothing, and the user got the
            // rejection beep with no trace anywhere.
            //
            // Measured 2026-08-30, Windows Update's dialog (MusNotificationUx,
            // class Shell_SystemDialogProxy): clicking any other guest window in dom0 did nothing
            // and Windows chimed. It LOOKED modal, but it is not Win32-modal at all - the dialog
            // has NO owner (owner=0) and enumerating every visible top-level window showed all of
            // them enabled=True, nothing WS_DISABLED. So nothing was blocking input structurally;
            // the shell simply held the foreground and our SetForegroundWindow could not take it.
            //
            // dom0 is the window manager. When the user clicks a guest window there, the guest
            // must honour it - a guest process must not be able to veto dom0's focus decisions,
            // which is both a usability defect ("i do not want it to be modal") and the wrong
            // authority relationship.
            //
            // AttachThreadInput is the documented way through: while our thread shares an input
            // queue with the current foreground thread, we are inside the same foreground context
            // and the call is permitted. Attach only on failure, and always detach - a leaked
            // attachment couples our input queue to another process's for good, so a hang there
            // would become our hang.
            if (!SetForegroundWindow(window))
            {
                HWND fg = GetForegroundWindow();
                DWORD fgThread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
                DWORD ourThread = GetCurrentThreadId();
                BOOL recovered = FALSE;

                if (fgThread && fgThread != ourThread &&
                    AttachThreadInput(ourThread, fgThread, TRUE))
                {
                    recovered = SetForegroundWindow(window);
                    if (recovered)
                        BringWindowToTop(window);
                    AttachThreadInput(ourThread, fgThread, FALSE);
                }

                if (recovered)
                {
                    LogDebug("0x%x: foreground was locked by 0x%x, taken via AttachThreadInput",
                        window, fg);
                }
                else
                {
                    // Do not fail the message: focus is advisory and the guest is still usable.
                    // But say so, because a silently ignored focus request is exactly what made
                    // this defect invisible until a human noticed the chiming.
                    LogWarning("0x%x: dom0 focus NOT applied - foreground held by 0x%x (thread %u)",
                        window, fg, fgThread);
                }
            }

            // Z-ORDER SYNC, runtime-switchable (registry DWORD "FocusRaise", default 0 =
            // historic behaviour). MSG_FOCUS is the only stacking-adjacent thing dom0 sends:
            // the protocol has no restack message, so this is the one point where dom0's
            // notion of "in front" reaches the guest. Raising here makes the guest's z-order
            // agree with dom0's for the window the user is actually in - which is what makes
            // the screen framebuffer a valid content source for it, and therefore what the
            // per-window fast path's hit rate depends on.
            //
            // Kept behind a switch rather than simply uncommented: SetForegroundWindow
            // usually raises already, so the increment may be nil, and the line was commented
            // out originally for a reason nobody recorded. A switch lets both conditions be
            // measured on ONE binary, interleaved, with no reinstall and no doubt about which
            // build produced which number.
            if (g_FocusRaise)
                BringWindowToTop(window);
        }
        LeaveCriticalSection(&g_csWatchedWindows);
    }

    return ERROR_SUCCESS;
}

static DWORD HandleClose(IN HWND window)
{
    LogDebug("0x%x", window);
    PostMessage(window, WM_SYSCOMMAND, SC_CLOSE, 0);

    return ERROR_SUCCESS;
}

static DWORD HandleWindowFlags(IN HWND window)
{
    struct msg_window_flags flagsMsg;

    if (!VchanReceiveBuffer(g_Vchan, &flagsMsg, sizeof(flagsMsg), L"msg_window_flags"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogDebug("0x%x: set 0x%x, unset 0x%x", window, flagsMsg.flags_set, flagsMsg.flags_unset);

    if (flagsMsg.flags_unset & WINDOW_FLAG_MINIMIZE) // restore
    {
        ShowWindowAsync(window, SW_RESTORE);
    }
    else if (flagsMsg.flags_set & WINDOW_FLAG_MINIMIZE) // minimize
    {
        ShowWindowAsync(window, SW_MINIMIZE);
    }

    return ERROR_SUCCESS;
}

static DWORD HandleDestroy(IN HWND window, OUT BOOL* screenDestroyed)
{
    LogDebug("0x%x", window);
    if (window == NULL) // desktop
    {
        *screenDestroyed = TRUE;
    }
    return ERROR_SUCCESS;
}

DWORD HandleServerData(BOOL replyToMessages, IN OUT struct _CAPTURE_CONTEXT* capture OPTIONAL,
    OUT BOOL* screenDestroyed)
{
    struct msg_hdr header;
    BYTE discardBuffer[256];
    int readSize;
    DWORD status = ERROR_SUCCESS;

    if (!VchanReceiveBuffer(g_Vchan, &header, sizeof(header), L"msg_hdr"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    LogVerbose("received message type %d for 0x%x", header.type, header.window);

#pragma warning(push)
#pragma warning(disable:4312)
    switch (header.type)
    {
    case MSG_KEYPRESS:
        status = HandleKeypress((HWND)header.window);
        break;
    case MSG_BUTTON:
        status = HandleButton((HWND)header.window);
        break;
    case MSG_MOTION:
        status = HandleMotion((HWND)header.window);
        break;
    case MSG_CROSSING:
        status = HandleCrossing((HWND)header.window);
        break;
    case MSG_CONFIGURE:
        status = HandleConfigure((HWND)header.window, replyToMessages);
        break;
    case MSG_FOCUS:
        status = HandleFocus((HWND)header.window);
        break;
    case MSG_CLOSE:
        status = HandleClose((HWND)header.window);
        break;
    case MSG_KEYMAP_NOTIFY:
        status = HandleKeymapNotify();
        break;
    case MSG_WINDOW_FLAGS:
        status = HandleWindowFlags((HWND)header.window);
        break;
    case MSG_DESTROY:
        status = HandleDestroy((HWND)header.window, screenDestroyed);
        break;
    case MSG_WINDOW_DUMP_ACK:
        // no body; the daemon has processed a WINDOW_DUMP, so superseded grants for
        // that window can now be revoked
        if (header.window == 0 && capture)
        {
            // A6: dom0 has adopted the new screen dump - handle_window_dump releases
            // the old framebuffer mapping before mapping the new refs - so the parked
            // screen grant(s) can be revoked now.
            // STAGING made the revoke arm of this DORMANT for the screen path: with
            // the persistent staging grant nothing is ever parked (no re-grants), so
            // the sweep below finds an empty list and no-ops. Kept for the direct-map
            // fallback; the per-window path (PwRevokeTick below) is independent.
            LogInfo("A6ACK window-0 dump ack received");
            CaptureRevokeStaleGrants(capture, L"ack");
            // Repaint the whole screen against the mapping dom0 just adopted. Under
            // back-to-back resizes dom0 can be left showing interleaved content
            // generations (sheared bands, measured 2026-08-05); the ack is the one
            // point where the new mapping is provably current on both sides.
            if (!g_SeamlessMode)
            {
                LogInfo("A6ACKREPAINT full damage %ux%u", capture->width, capture->height);
                SendWindowDamageEvent(NULL, 0, 0, capture->width, capture->height);
                // M0BLINK: the ACK-GATED repaint - one vchan round trip AFTER the
                // optimistic post-dump repaint that main.c already sent (logged as
                // "M0BLINK repaint-first"). Time-to-pixels is repaint-first; this
                // line is the ack round-trip cost on top of it. Consume the
                // obtain-start stamp (one line per obtain; later acks stay silent).
                LONG64 m0Start = InterlockedExchange64(&g_M0BlinkObtainStart, 0);
                if (m0Start != 0)
                {
                    ULONGLONG now = GetTickCount64();
                    LogInfo("M0BLINK repaint-sent t=%I64u sinceobtain=%I64u ms",
                        now, now - (ULONGLONG)m0Start);
                }
            }
        }
        PwRevokeTick();
        status = ERROR_SUCCESS;
        break;
    case QGA_MSG_WORKAREA:
        status = HandleWorkarea();
        break;
#pragma warning(pop)
    default:
        LogWarning("got unknown msg type %d, ignoring", header.type);

        /* discard unsupported message body */
        while (header.untrusted_len > 0)
        {
            readSize = min(header.untrusted_len, sizeof(discardBuffer));
            if (!VchanReceiveBuffer(g_Vchan, discardBuffer, readSize, L"discard buffer"))
            {
                LogError("VchanReceiveBuffer failed");
                return ERROR_UNIDENTIFIED_ERROR;
            }
            header.untrusted_len -= readSize;
        }
    }

    if (ERROR_SUCCESS != status)
    {
        // KEEP-FATAL: after the NEVEREXIT conversions (see the policy comment at the
        // top of this file) every status that reaches this point is a vchan-level
        // receive or send failure - the stream is broken or desynced. Case (a).
        LogError("handler failed: 0x%x, exiting (vchan-level failure)", status);
    }

    return status;
}
