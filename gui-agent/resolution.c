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
#include <assert.h>
#include <setupapi.h>
#include <strsafe.h>

#include "common.h"
#include "main.h"
#include "resolution.h"
#include "workarea.h"
#include "send.h"
#include "util.h"

#include <config.h>
#include <log.h>

// Hardware id of the Qubes IDD device, matched case-insensitively when replugging
// it so the driver re-reads REG_QUBES_IDD_KEY\REG_QUBES_IDD_MODES_VALUE.
#define QUBES_IDD_HARDWARE_ID L"root\\iddsampledriver"

// How long to wait for a replugged IDD to start offering a freshly requested mode.
#define EXACT_MODE_WAIT_TIMEOUT_MS 12000
#define EXACT_MODE_WAIT_STEP_MS    250

// parameters for the resolution change thread
typedef struct _RESOLUTION_THREAD_PARAMS
{
    HANDLE Event; // event to wait on
    LONG Width; // requested resolution
    LONG Height;
    const WCHAR* Source; // origin of the request, for instrumentation logging
} RESOLUTION_THREAD_PARAMS;

struct SUPPORTED_MODES
{
    DWORD Count;
    POINT* Dimensions;
} g_SupportedModes;

void InitVideoModes()
{
    DEVMODEW mode;

    for (g_SupportedModes.Count = 0; ; g_SupportedModes.Count++)
    {
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(NULL, g_SupportedModes.Count, &mode))
            break;
    }

    LogDebug("Enumerated %u supported modes", g_SupportedModes.Count);
    g_SupportedModes.Dimensions = (POINT*)malloc(g_SupportedModes.Count * sizeof(POINT));
    if (!g_SupportedModes.Dimensions)
        exit(ERROR_OUTOFMEMORY);

    for (DWORD i = 0; i < g_SupportedModes.Count; i++)
    {
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(NULL, i, &mode))
        {
            LogWarning("Failed to get display settings for mode %u", i);
            win_perror("EnumDisplaySettingsW");
            g_SupportedModes.Dimensions[i].x = 0;
            g_SupportedModes.Dimensions[i].y = 0;
            continue;
        }

        g_SupportedModes.Dimensions[i].x = mode.dmPelsWidth;
        g_SupportedModes.Dimensions[i].y = mode.dmPelsHeight;
        LogDebug("mode %u: %ux%u %u bpp @ %u, flags 0x%x", i,
            mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel, mode.dmDisplayFrequency, mode.dmDisplayFlags);
    }

    LogDebug("Initialized %u supported modes", g_SupportedModes.Count);
}

static ULONG SetVideoModeInternal(IN ULONG width, IN ULONG height)
{
    if (!IS_RESOLUTION_VALID(width, height))
    {
        LogError("Resolution is invalid: %lu x %lu", width, height);
        return ERROR_INVALID_PARAMETER;
    }

    LogInfo("New resolution: %lu x %lu", width, height);
    // ChangeDisplaySettings fails if thread's desktop != input desktop...
    // This can happen on "quick user switch".
    AttachToInputDesktop();

    DEVMODE devMode;
    ZeroMemory(&devMode, sizeof(DEVMODE));
    devMode.dmSize = sizeof(DEVMODE);
    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

    ULONG status = ChangeDisplaySettings(&devMode, 0);
    if (DISP_CHANGE_SUCCESSFUL != status)
    {
        LogError("ChangeDisplaySettings failed: 0x%x", status);
    }

    SetLastError(status);
    return status;
}

// return best-matching supported mode index for desired resolution
DWORD SelectSupportedMode(IN DWORD width, IN DWORD height)
{
    DWORD mode = 0;
    float sim = 0;

    LogVerbose("Host screen dimensions: %ux%u", g_HostScreenWidth, g_HostScreenHeight);
    for (DWORD i = 0; i < g_SupportedModes.Count; i++)
    {
        DWORD w = g_SupportedModes.Dimensions[i].x;
        DWORD h = g_SupportedModes.Dimensions[i].y;

        // TODO: filter these when constructing supported mode list
        if (w > g_HostScreenWidth || h > g_HostScreenHeight || w == 0 || h == 0)
            continue;

        if (w == width && h == height)
        {
            LogDebug("Returning exact mode %u (%ux%u) for %ux%u", i, w, h, width, height);
            return i;
        }

        float area_cur = w * (float)h;
        float area_req = width * (float)height;
        float inter = min(w, width) * (float)min(h, height);
        float similarity = inter / (float)(area_cur + area_req - inter);

        if (similarity > sim)
        {
            sim = similarity;
            mode = i;
        }
    }

    LogDebug("Returning mode %u (%ux%u) for %ux%u", mode,
        g_SupportedModes.Dimensions[mode].x, g_SupportedModes.Dimensions[mode].y, width, height);
    return mode;
}

// CDS_TEST the exact size directly with a DEVMODE. Deliberately does NOT consult
// the Init-time g_SupportedModes cache: it goes stale across IDD replugs.
static BOOL IsExactModeAvailable(IN ULONG width, IN ULONG height)
{
    DEVMODE devMode;
    ZeroMemory(&devMode, sizeof(devMode));
    devMode.dmSize = sizeof(devMode);
    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

    return DISP_CHANGE_SUCCESSFUL == ChangeDisplaySettings(&devMode, CDS_TEST);
}

// Publish the requested exact mode where the Qubes IDD driver picks it up on
// monitor arrival (see REG_QUBES_IDD_KEY in include/common.h).
static DWORD WriteRequestedIddMode(IN ULONG width, IN ULONG height)
{
    WCHAR modes[32]; // REG_MULTI_SZ: "WIDTHxHEIGHT\0\0"
    ZeroMemory(modes, sizeof(modes));
    if (FAILED(StringCchPrintf(modes, RTL_NUMBER_OF(modes) - 1, L"%lux%lu", width, height)))
        return ERROR_INVALID_PARAMETER;

    HKEY key;
    DWORD status = RegCreateKeyEx(HKEY_LOCAL_MACHINE, REG_QUBES_IDD_KEY, 0, NULL, 0,
        KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "creating IDD modes registry key");

    status = RegSetValueEx(key, REG_QUBES_IDD_MODES_VALUE, 0, REG_MULTI_SZ,
        (const BYTE*)modes, (DWORD)((wcslen(modes) + 2) * sizeof(WCHAR)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "writing IDD modes registry value");

    return ERROR_SUCCESS;
}

// Restart the Qubes IDD device in-process (what `devcon restart` does) so the
// driver re-reads the modes key. Returns TRUE if a matching device was restarted.
static BOOL RestartQubesIddDevice(void)
{
    HDEVINFO devList = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devList == INVALID_HANDLE_VALUE)
    {
        win_perror("SetupDiGetClassDevs");
        return FALSE;
    }

    BOOL restarted = FALSE;
    SP_DEVINFO_DATA device;
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devList, i, &device); i++)
    {
        WCHAR hardwareIds[512]; // REG_MULTI_SZ
        ZeroMemory(hardwareIds, sizeof(hardwareIds)); // guarantee multi-sz termination
        if (!SetupDiGetDeviceRegistryProperty(devList, &device, SPDRP_HARDWAREID, NULL,
            (BYTE*)hardwareIds, (DWORD)(sizeof(hardwareIds) - 2 * sizeof(WCHAR)), NULL))
            continue;

        BOOL match = FALSE;
        for (const WCHAR* id = hardwareIds; *id; id += wcslen(id) + 1)
        {
            if (0 == _wcsicmp(id, QUBES_IDD_HARDWARE_ID))
            {
                match = TRUE;
                break;
            }
        }

        if (!match)
            continue;

        SP_PROPCHANGE_PARAMS propChange;
        ZeroMemory(&propChange, sizeof(propChange));
        propChange.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        propChange.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        propChange.StateChange = DICS_PROPCHANGE;
        propChange.Scope = DICS_FLAG_GLOBAL;
        propChange.HwProfile = 0;

        if (!SetupDiSetClassInstallParams(devList, &device, &propChange.ClassInstallHeader, sizeof(propChange)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devList, &device))
        {
            win_perror("restarting Qubes IDD device");
            continue;
        }

        LogInfo("replugged Qubes IDD device (index %lu)", i);
        restarted = TRUE;
    }

    SetupDiDestroyDeviceInfoList(devList);
    return restarted;
}

// dom0's window size is the single source of truth: any applied size other than
// the exact requested one makes gui-daemon resize the user's window to match
// ("resize-to-viewport" - forbidden). Either the exact size is applied, if needed
// after obtaining it from the Qubes IDD, or the current resolution is kept as-is.
// Runs only on the resolution-change thread (the sole dom0-sourced call site goes
// through RequestResolutionChange), so the replug+wait never blocks the vchan loop.
static ULONG SetVideoModeExact(IN ULONG width, IN ULONG height)
{
    if (!IS_RESOLUTION_VALID(width, height))
    {
        LogError("Resolution is invalid: %lu x %lu", width, height);
        return ERROR_INVALID_PARAMETER;
    }

    if (width == g_ScreenWidth && height == g_ScreenHeight)
    {
        LogInfo("RESNOOP %lux%lu", width, height);
        return ERROR_SUCCESS;
    }

    BOOL replugged = FALSE;
    if (!IsExactModeAvailable(width, height))
    {
        // try to obtain the exact mode from the Qubes IDD: publish it in the
        // driver's registry key, replug the device, wait for the mode to appear
        if (WriteRequestedIddMode(width, height) != ERROR_SUCCESS)
        {
            LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=modes-key-write-failed",
                width, height, g_ScreenWidth, g_ScreenHeight);
            return ERROR_SUCCESS;
        }

        if (!RestartQubesIddDevice())
        {
            LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=idd-not-present",
                width, height, g_ScreenWidth, g_ScreenHeight);
            return ERROR_SUCCESS;
        }

        DWORD waited;
        for (waited = 0; waited < EXACT_MODE_WAIT_TIMEOUT_MS; waited += EXACT_MODE_WAIT_STEP_MS)
        {
            Sleep(EXACT_MODE_WAIT_STEP_MS);
            if (IsExactModeAvailable(width, height))
                break;
        }

        if (waited >= EXACT_MODE_WAIT_TIMEOUT_MS)
        {
            LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=mode-never-appeared",
                width, height, g_ScreenWidth, g_ScreenHeight);
            return ERROR_SUCCESS;
        }

        replugged = TRUE;
    }

    ULONG status = SetVideoModeInternal(width, height);
    if (status != ERROR_SUCCESS)
    {
        LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=apply-failed-0x%lx",
            width, height, g_ScreenWidth, g_ScreenHeight, status);
        return status;
    }

    // readback-verify (A2-style): re-read what Windows actually applied
    DEVMODE appliedMode;
    ZeroMemory(&appliedMode, sizeof(appliedMode));
    appliedMode.dmSize = sizeof(appliedMode);
    if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &appliedMode))
    {
        LogWarning("EnumDisplaySettings(ENUM_CURRENT_SETTINGS) failed, cannot verify applied resolution");
    }
    else if (appliedMode.dmPelsWidth != width || appliedMode.dmPelsHeight != height)
    {
        LogWarning("RESAPPLIED-MISMATCH applied=%lux%lu expected=%lux%lu",
            appliedMode.dmPelsWidth, appliedMode.dmPelsHeight, width, height);
    }

    LogInfo("RESEXACT %lux%lu replug=%d", width, height, replugged ? 1 : 0);

    g_ScreenWidth = width;
    g_ScreenHeight = height;
    // save last-set resolution to use on next startup
    CfgWriteDword(NULL, REG_CONFIG_FULLSCREEN_WIDTH_VALUE, g_ScreenWidth, NULL);
    CfgWriteDword(NULL, REG_CONFIG_FULLSCREEN_HEIGHT_VALUE, g_ScreenHeight, NULL);
    // resolution changed: recompute the guest work area against the new screen
    WorkAreaApply();

    return ERROR_SUCCESS;
}

ULONG SetVideoMode(IN ULONG width, IN ULONG height, IN const WCHAR* source)
{
    LogVerbose("%lu x %lu", width, height);

    // instrumentation (log-only): what was requested, before any snapping
    LogInfo("RESREQ %lux%lu src=%s", width, height, source);

    // dom0-sourced requests must never be snapped to a different mode:
    // exact size or keep the current one (see SetVideoModeExact)
    if (source && 0 == wcscmp(source, L"dom0"))
        return SetVideoModeExact(width, height);

    DWORD mode = SelectSupportedMode(width, height);

    // instrumentation (log-only): what SelectSupportedMode chose,
    // marked SNAPPED when it differs from the request
    if ((ULONG)g_SupportedModes.Dimensions[mode].x != width || (ULONG)g_SupportedModes.Dimensions[mode].y != height)
        LogInfo("RESSNAP %ldx%ld SNAPPED", g_SupportedModes.Dimensions[mode].x, g_SupportedModes.Dimensions[mode].y);
    else
        LogInfo("RESSNAP %ldx%ld", g_SupportedModes.Dimensions[mode].x, g_SupportedModes.Dimensions[mode].y);

    width = g_SupportedModes.Dimensions[mode].x;
    height = g_SupportedModes.Dimensions[mode].y;

    if (width == g_ScreenWidth && height == g_ScreenHeight)
    {
        LogInfo("RESNOOP %lux%lu", width, height);
        return ERROR_SUCCESS;
    }

    ULONG status = SetVideoModeInternal(width, height);
    if (ERROR_SUCCESS != status)
    {
        g_SeamlessMode = FALSE;

        LogDebug("SetVideoModeInternal failed: 0x%x, keeping original resolution %lux%lu", status, g_ScreenWidth, g_ScreenHeight);
    }
    else
    {
        // instrumentation (log-only): re-read what Windows ACTUALLY applied.
        // Do NOT feed this back into g_ScreenWidth/Height here (that is fix A2).
        DEVMODE appliedMode;
        ZeroMemory(&appliedMode, sizeof(appliedMode));
        appliedMode.dmSize = sizeof(appliedMode);
        if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &appliedMode))
        {
            LogInfo("RESAPPLIED %lux%lu", appliedMode.dmPelsWidth, appliedMode.dmPelsHeight);
            if (appliedMode.dmPelsWidth != width || appliedMode.dmPelsHeight != height)
                LogInfo("RESAPPLIED-MISMATCH applied=%lux%lu expected=%lux%lu",
                    appliedMode.dmPelsWidth, appliedMode.dmPelsHeight, width, height);
        }
        else
        {
            LogWarning("EnumDisplaySettings(ENUM_CURRENT_SETTINGS) failed, cannot verify applied resolution");
        }

        g_ScreenWidth = width;
        g_ScreenHeight = height;
        // save last-set resolution to use on next startup
        CfgWriteDword(NULL, REG_CONFIG_FULLSCREEN_WIDTH_VALUE, g_ScreenWidth, NULL);
        CfgWriteDword(NULL, REG_CONFIG_FULLSCREEN_HEIGHT_VALUE, g_ScreenHeight, NULL);
        // resolution changed: recompute the guest work area against the new screen
        WorkAreaApply();
    }

    return status;
}

// This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
// after last screen resize message received from gui daemon.
// This is to not change resolution on every such message (multiple times per second).
static DWORD WINAPI ResolutionChangeThread(void *param)
{
    DWORD waitResult;
    RESOLUTION_THREAD_PARAMS* args = (RESOLUTION_THREAD_PARAMS*)param;

    while (TRUE)
    {
        // Wait indefinitely for an initial "change resolution" event.
        WaitForSingleObject(args->Event, INFINITE);
        LogDebug("resolution change requested: %dx%d", args->Width, args->Height);

        do
        {
            // If event is signaled again before timeout expires: ignore and wait for another one.
            waitResult = WaitForSingleObject(args->Event, RESOLUTION_CHANGE_TIMEOUT);
            LogVerbose("second wait result: %lu", waitResult);
        } while (waitResult == WAIT_OBJECT_0);

        // If we're here, that means the wait finally timed out.
        // We can change the resolution now.
        LogInfo("resolution change: %dx%d", args->Width, args->Height);

        SetVideoMode(args->Width, args->Height, args->Source);
    }
    return ERROR_SUCCESS;
}

DWORD RequestResolutionChange(IN LONG width, IN LONG height, IN const WCHAR* source)
{
    static RESOLUTION_THREAD_PARAMS threadArgs = { 0 };

    // This thread triggers resolution change if RESOLUTION_CHANGE_TIMEOUT passes
    // after last screen resize message received from gui daemon.
    // This is to not change resolution on every such message (multiple times per second).
    static HANDLE thread = NULL;

    LogVerbose("%dx%d", width, height);

    if (!threadArgs.Event)
        threadArgs.Event = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!threadArgs.Event)
        return win_perror("creating resolution change request event");

    if (!thread)
        thread = CreateThread(NULL, 0, ResolutionChangeThread, &threadArgs, 0, 0);

    if (!thread)
        return win_perror("creating resolution change thread");

    threadArgs.Width = width;
    threadArgs.Height = height;
    threadArgs.Source = source;
    if (!SetEvent(threadArgs.Event))
        return win_perror("signaling resolution change request event");

    return ERROR_SUCCESS;
}
