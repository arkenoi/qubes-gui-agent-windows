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

#pragma once

#define PAGE_SIZE 0x1000

// registry configuration key, user mode and kernel mode names (kernel one currently unused)
#define REG_CONFIG_USER_KEY     L"Software\\Invisible Things Lab\\Qubes Tools"
#define REG_CONFIG_KERNEL_KEY   L"\\Registry\\Machine\\Software\\Invisible Things Lab\\Qubes Tools"

// value names in registry config
#define REG_CONFIG_FPS_VALUE        L"MaxFps"
#define REG_CONFIG_CURSOR_VALUE     L"DisableCursor"
#define REG_CONFIG_SEAMLESS_VALUE   L"SeamlessMode"
#define REG_CONFIG_FULLSCREEN_WIDTH_VALUE   L"FullscreenWidth"
#define REG_CONFIG_FULLSCREEN_HEIGHT_VALUE  L"FullscreenHeight"
// Desktop size to use in NON-SEAMLESS mode (the whole guest desktop inside ONE dom0 window).
// Kept separate from FullscreenWidth/Height because those are overwritten by the seamless
// force-to-host, which left no windowed size to return to - so entering non-seamless
// inherited host geometry and produced a screen-covering window every time.
#define REG_CONFIG_WINDOWED_WIDTH_VALUE     L"WindowedWidth"
#define REG_CONFIG_WINDOWED_HEIGHT_VALUE    L"WindowedHeight"
#define WINDOWED_DEFAULT_WIDTH   1280
#define WINDOWED_DEFAULT_HEIGHT   800
// guest work area override "x,y,w,h" (see gui-agent/workarea.h)
#define REG_CONFIG_WORKAREA_VALUE           L"WorkArea"
// persistent staging framebuffer grant (DWORD, default 1=ON): the screen framebuffer
// is granted to dom0 once per agent lifetime and frames are copied into it; 0 restores
// the legacy direct-map per-geometry grant (A/B switch, see gui-agent/capture.c)
#define REG_CONFIG_STAGING_VALUE            L"StagingGrant"
// P2 probe (DWORD, default 0; needs StagingGrant=1): never grant the desktop framebuffer
// to dom0 and suppress the window-0 MSG_WINDOW_DUMP - dom0 renders exclusively from
// per-window grants. The staging buffer is still allocated and filled (it stays the LOCAL
// pixel source for slice-fed windows, the DDA-owned channel and synth patches). With this
// ON, paths that need dom0 to read the screen image (non-seamless window 0, the daemon-side
// legacy slice for unattached windows) render border-only. See DESIGN-pure-per-window.md P2.
#define REG_CONFIG_NO_SCREEN_GRANT_VALUE    L"SeamlessNoScreenGrant"
// Allow fullscreen-sized windows in seamless mode (DWORD, default 0 = deny). Denying them
// hides the boot/shutdown full-desktop "flash" (a fullscreen window mapped during the seamless
// transition; override-redirect fullscreen is denied unconditionally). The dom0 opt-in feature
// service.gui-fullscreen (guest qubesdb /qubes-service/gui-fullscreen) overrides this
// guest-local base, and dom0 wins. Does not affect true fullscreen MODE.
#define REG_CONFIG_SHOW_FS_VALUE            L"ShowFullscreenScreen"
// How long the agent waits for its FIRST gui-daemon client before deciding the vchan
// server it opened is dead and exiting for the watchdog to respawn it, and how many times
// it may do that before giving up and staying quiet. See the FIRST-BOOT SELF-HEAL note in
// gui-agent/main.c: on the first boot of a freshly created AppVM the daemon never attaches,
// and restarting the agent alone fixes it (measured 3/3, 2026-08-14).
#define VCHAN_FIRST_CLIENT_WAIT_MS      90000
#define VCHAN_FIRST_CLIENT_MAX_RESTARTS 3
#define REG_CONFIG_VCHAN_RESTARTS_VALUE     L"VchanFirstClientRestarts"
// Set once a gui-daemon has ever connected, so a RESPAWNED agent can distinguish "this qube
// never had a GUI" from "its GUI died and dom0's daemon is not coming back".
#define REG_CONFIG_HAD_CLIENT_VALUE         L"VchanHadClient"

// path to the gui agent, launched by the watchdog service
#define REG_CONFIG_AGENT_PATH_VALUE  L"GuiAgentPath"

// Qubes IDD dynamic mode key (under HKLM): the agent publishes exact modes the
// daemon asked for as REG_MULTI_SZ "WIDTHxHEIGHT" entries and replugs the IDD
// device; the D4 driver (t2/d4-registry-modes) reads them at monitor arrival.
#define REG_QUBES_IDD_KEY           L"SOFTWARE\\QubesIDD"
#define REG_QUBES_IDD_MODES_VALUE   L"Modes"

// Qubes IDD control device interface + reload IOCTL (driver branch
// t2/d4v3-ioctl-reload): DeviceIoControl(IOCTL_QIDD_RELOAD_MODES) makes the
// RUNNING driver re-read REG_QUBES_IDD_KEY\REG_QUBES_IDD_MODES_VALUE via a
// monitor-level departure/arrival - no PnP device restart (which disturbs the
// Xen platform device; see FINDINGS 2026-08-05 cont 10).
// Values match the driver (driver/IddSampleDriver/Driver.h on t2/d4v3-ioctl-reload,
// commit ce8edf6): interface GUID {C7817EB4-B2B6-4996-A48C-04EF247952AB}, control
// code (FILE_DEVICE_UNKNOWN, function 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
// = 0x00222000.
#define QIDD_INTERFACE_GUID_INIT \
    { 0xc7817eb4, 0xb2b6, 0x4996, { 0xa4, 0x8c, 0x04, 0xef, 0x24, 0x79, 0x52, 0xab } }
#define IOCTL_QIDD_RELOAD_MODES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// event created by the helper service, trigger to simulate SAS (ctrl-alt-delete)
#define QGA_SAS_EVENT_NAME L"Global\\QGA_SAS_TRIGGER"

// When signaled, causes agent to shutdown gracefully.
#define QGA_SHUTDOWN_EVENT_NAME L"Global\\QGA_SHUTDOWN"
// Single-instance guard. A MUTEX, not an event, deliberately: mutex ownership is released by
// the kernel when the owning process dies, so a crashed agent can never lock out its
// replacement. An event would persist while any handle lived and could strand the qube
// without a GUI - the opposite of what this protects against.
#define QGA_INSTANCE_MUTEX_NAME L"Global\\QGA_SINGLE_INSTANCE"

// these are hardcoded
#define	MIN_RESOLUTION_WIDTH	320UL
#define	MIN_RESOLUTION_HEIGHT	200UL

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && (MIN_RESOLUTION_HEIGHT <= (uHeight)))
