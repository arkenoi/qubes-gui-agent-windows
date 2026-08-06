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
#include "resolution.h"

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
    INPUT inputEvent;
    RECT rect = { 0 };

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &buttonMsg, sizeof(buttonMsg), L"msg_button"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    if (window)
        GetWindowRect(window, &rect);

    /* TODO: send to correct window */

    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.dwFlags = 0;
    inputEvent.mi.time = 0;
    inputEvent.mi.mouseData = 0;
    inputEvent.mi.dwExtraInfo = 0;
    /* pointer coordinates must be 0..65535, which covers the whole screen -
    * regardless of resolution */
    inputEvent.mi.dx = (buttonMsg.x + rect.left) * 65535 / g_ScreenWidth;
    inputEvent.mi.dy = (buttonMsg.y + rect.top) * 65535 / g_ScreenHeight;
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

    LogDebug("window 0x%x, (%d,%d), flags 0x%x", window, buttonMsg.x, buttonMsg.y, inputEvent.mi.dwFlags);
    InjectInput(&inputEvent, "SendInput");

    return ERROR_SUCCESS;
}

static DWORD HandleMotion(IN HWND window)
{
    struct msg_motion motionMsg;
    INPUT inputEvent;

    LogVerbose("0x%x", window);
    if (!VchanReceiveBuffer(g_Vchan, &motionMsg, sizeof(motionMsg), L"msg_motion"))
    {
        LogError("VchanReceiveBuffer failed"); // KEEP-FATAL: vchan broken/EOF - case (a), see NEVEREXIT policy above
        return ERROR_UNIDENTIFIED_ERROR;
    }

    int32_t x = motionMsg.x;
    int32_t y = motionMsg.y;

    if (motionMsg.is_hint)
    {
        LogDebug("0x%x: ignoring motion hint (%d,%d)", window, x, y);
        return ERROR_SUCCESS;
    }

    if (window)
    {
        const WINDOW_DATA* data = FindWindowByHandle(window);
        if (data)
        {
            x += data->X;
            y += data->Y;
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
                win_perror("GetWindowRect");
                return ERROR_SUCCESS; // ignore
            }
        }

        LogVerbose("0x%x: (%d,%d)", window, x, y);
    }

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
        // post request without waiting to not block and don't change the Z-order
        UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOZORDER;
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
            if (data->IsIconic)
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
            else
            {
                if (data->X == configureMsg.x && data->Y == configureMsg.y)
                {
                    flags |= SWP_NOMOVE;
                    LogVerbose("SWP_NOMOVE");
                }

                if (data->Width == configureMsg.width && data->Height == configureMsg.height)
                {
                    flags |= SWP_NOSIZE;
                    LogVerbose("SWP_NOSIZE");
                }

                if (SetWindowPos(window, NULL, configureMsg.x, configureMsg.y, configureMsg.width, configureMsg.height, flags))
                {
                    // update expected pos/size of the tracked window without waiting for actual change
                    // since we use SWP_ASYNCWINDOWPOS
                    // TODO: improve further, somehow window data via UpdateWindowData() later after SetWindowPos below
                    // is still different from what was passed via configureMsg - DWM API needs a while to properly update
                    // the window state after changes?
                    if (!(flags & SWP_NOMOVE))
                    {
                        LogVerbose("Updating position of 0x%x: (%d,%d) -> (%d,%d)", window, data->X, data->Y,
                            configureMsg.x, configureMsg.y);
                        data->X = configureMsg.x;
                        data->Y = configureMsg.y;
                    }

                    if (!(flags & SWP_NOSIZE))
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
                }
                else
                {
                    win_perror("SetWindowPos");
                }
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
        else
        {
            if (data->IsIconic)
                ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            //BringWindowToTop(window);
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
