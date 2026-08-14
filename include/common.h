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
// guest work area override "x,y,w,h" (see gui-agent/workarea.h)
#define REG_CONFIG_WORKAREA_VALUE           L"WorkArea"
// persistent staging framebuffer grant (DWORD, default 1=ON): the screen framebuffer
// is granted to dom0 once per agent lifetime and frames are copied into it; 0 restores
// the legacy direct-map per-geometry grant (A/B switch, see gui-agent/capture.c)
#define REG_CONFIG_STAGING_VALUE            L"StagingGrant"
// How long the agent waits for its FIRST gui-daemon client before deciding the vchan
// server it opened is dead and exiting for the watchdog to respawn it, and how many times
// it may do that before giving up and staying quiet. See the FIRST-BOOT SELF-HEAL note in
// gui-agent/main.c: on the first boot of a freshly created AppVM the daemon never attaches,
// and restarting the agent alone fixes it (measured 3/3, 2026-08-14).
#define VCHAN_FIRST_CLIENT_WAIT_MS      90000
#define VCHAN_FIRST_CLIENT_MAX_RESTARTS 3
#define REG_CONFIG_VCHAN_RESTARTS_VALUE     L"VchanFirstClientRestarts"

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

// these are hardcoded
#define	MIN_RESOLUTION_WIDTH	320UL
#define	MIN_RESOLUTION_HEIGHT	200UL

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && (MIN_RESOLUTION_HEIGHT <= (uHeight)))
