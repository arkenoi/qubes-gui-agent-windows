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
#include <winioctl.h>
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
// STEP is now only the DECAYED (late) poll interval - the wait starts by checking
// immediately and polls tightly for the first two seconds; see the loop in
// SetVideoModeExact for why.
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

// ---------------------------------------------------------------------------------------
// EnsureQubesIddSolo - attach the Qubes IddCx monitor and make it the SOLE active display.
//
// WHY THIS EXISTS (measured 2026-08-07, FINDINGS.md). The installer's /idd path does
// pnputil -> devcon create root devnode -> disable the emulated VGA -> reboot, and assumes
// "the next boot comes up IDD-primary". That assumption is false: an IddCx monitor arrives
// CONNECTED but INACTIVE and is not attached to the desktop until a topology apply names
// its path. Nothing in the package ever performed one. So on Win10 the desktop stayed on
// the emulated VGA - which keeps driving video at full resolution even though its devnode
// reports CM_PROB_DISABLED - and the IDD sat at Availability=8 forever. (The old claim that
// Windows "fell back to ROOT\BASICDISPLAY" was a friendly-name mis-attribution; that device
// is absent from the controller list entirely.) Win11 24H2 performs the apply itself, which
// is the whole of the Win10/Win11 difference - the driver has no OS-build branch at all.
//
// WHY IN THE AGENT and not the installer: this needs the INTERACTIVE session. The
// installer's boot-resume task runs as SYSTEM in session 0, where ChangeDisplaySettingsEx
// returns DISP_CHANGE_FAILED / ERROR_ACCESS_DENIED. The agent already attaches to the input
// desktop and must observe the resulting geometry anyway, so it is the right owner.
//
// WHY EVERY OTHER DISPLAY IS DETACHED, not merely deprioritised: an additional ACTIVE
// display enlarges the desktop bounding box the agent maps as the screen, so Windows can
// place windows in a region dom0 never sees and seamless coordinates break (CLAUDE.md
// Phase 1B). Solo is the requirement, not a nicety.
//
// Mirrors tools/modeprobe --solo, which is the implementation this was proven with on
// win-idd-test (same Win10 19045 build): detach others with CDS_UPDATEREGISTRY|CDS_NORESET,
// set the target primary, then commit once with a NULL ChangeDisplaySettingsEx. Persisting
// to the registry is deliberate - it also overwrites stale GraphicsDrivers\Configuration
// entries so the topology survives the next reboot.
//
// Returns ERROR_SUCCESS only when READBACK confirms the end state. It is a no-op (and NOT
// an error) on a guest with no IDD - the Basic Display Adapter configuration is supported.
#define QUBES_IDD_DEVICE_STRING L"IddSampleDriver Device"

ULONG EnsureQubesIddSolo(void)
{
    AttachToInputDesktop();

    // Enumerate once; DISPLAY_DEVICE.DeviceString identifies the IDD adapter.
    DISPLAY_DEVICEW dev;
    WCHAR iddName[CCHDEVICENAME] = { 0 };
    WCHAR attached[16][CCHDEVICENAME];
    DWORD attachedCount = 0;

    for (DWORD i = 0; ; i++)
    {
        ZeroMemory(&dev, sizeof(dev));
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(NULL, i, &dev, 0))
            break;
        if (dev.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
            continue;

        if (0 == _wcsnicmp(dev.DeviceString, QUBES_IDD_DEVICE_STRING,
                           ARRAYSIZE(QUBES_IDD_DEVICE_STRING) - 1))
        {
            StringCchCopyW(iddName, ARRAYSIZE(iddName), dev.DeviceName);
            LogInfo("IDD solo: found IDD adapter '%s' ('%s'), attached=%d",
                dev.DeviceName, dev.DeviceString,
                (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? 1 : 0);
        }
        else if ((dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
                 attachedCount < ARRAYSIZE(attached))
        {
            StringCchCopyW(attached[attachedCount], CCHDEVICENAME, dev.DeviceName);
            attachedCount++;
        }
    }

    if (!iddName[0])
    {
        // No IDD present. This is the Basic Display Adapter configuration, which is a
        // supported way to run - say so once and leave the topology alone.
        LogDebug("IDD solo: no Qubes IDD adapter present, leaving display topology untouched");
        return ERROR_SUCCESS;
    }

    // Detach every other attached display FIRST. NORESET batches the changes so the
    // desktop is never momentarily headless between the detach and the set-primary.
    for (DWORD i = 0; i < attachedCount; i++)
    {
        DEVMODEW det;
        ZeroMemory(&det, sizeof(det));
        det.dmSize = sizeof(det);
        det.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        LONG rc = ChangeDisplaySettingsExW(attached[i], &det, NULL,
                                           CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        LogInfo("IDD solo: detach '%s' -> %ld", attached[i], rc);
    }

    // Make the IDD primary at (0,0). Width/height are left to the driver's own preferred
    // mode: the mode set is published separately (ResolutionPublishBootModeSet) and forcing
    // a size here would race it.
    DEVMODEW pm;
    ZeroMemory(&pm, sizeof(pm));
    pm.dmSize = sizeof(pm);
    pm.dmFields = DM_POSITION;
    LONG prc = ChangeDisplaySettingsExW(iddName, &pm, NULL,
                                        CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    LONG crc = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    LogInfo("IDD solo: set-primary '%s' -> %ld, commit -> %ld", iddName, prc, crc);

    // READBACK. "The call returned success" is not the criterion - re-enumerate and require
    // the IDD attached+primary with ZERO other attached displays. Without this the function
    // would report success on exactly the failure it exists to fix.
    DWORD othersStillAttached = 0;
    BOOL iddAttached = FALSE, iddPrimary = FALSE;
    for (DWORD i = 0; ; i++)
    {
        ZeroMemory(&dev, sizeof(dev));
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(NULL, i, &dev, 0))
            break;
        if (dev.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
            continue;
        if (!(dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;

        if (0 == _wcsicmp(dev.DeviceName, iddName))
        {
            iddAttached = TRUE;
            iddPrimary = (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? TRUE : FALSE;
        }
        else
        {
            othersStillAttached++;
            LogWarning("IDD solo: '%s' ('%s') is STILL attached", dev.DeviceName, dev.DeviceString);
        }
    }

    if (iddAttached && iddPrimary && othersStillAttached == 0)
    {
        LogInfo("IDD solo: OK - '%s' is the sole active display", iddName);
        return ERROR_SUCCESS;
    }

    LogError("IDD solo: FAILED - idd attached=%d primary=%d, others still attached=%lu",
        iddAttached, iddPrimary, othersStillAttached);
    return ERROR_INVALID_STATE;
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

// ---- M6: computed IDD mode set ---------------------------------------------
// The registry mode list is a computed SET, not just the single requested entry,
// so the user's habitual sizes (maximize, tile-half - both derived from the dom0
// work-area feed, never fabricated from inference) are already offered and switch
// with replug=0. In WINDOWED (non-seamless) use host-screen sizes are
// deliberately NOT added by the builder: they may only enter through the target
// slot as an actual dom0 request. Seamless mode is the one exception (entry a1
// below): there the desktop is the host-sized surface by definition.
#define IDD_MODE_SET_MAX 8
#define IDD_MODES_CCH 256

typedef struct { ULONG w, h; } IDD_MODE;

static SRWLOCK g_IddModeSetLock = SRWLOCK_INIT;
static WCHAR g_IddModeSetLast[IDD_MODES_CCH]; // last multi-sz written to the key
static DWORD g_IddModeSetLastCch;             // WCHARs incl. final terminator; 0 = never

// Append WxH if valid, in bounds and not already present. Returns TRUE if the
// size is IN the set after the call (freshly added or already there), FALSE if
// it was rejected as invalid or the set is full - callers that must not point at
// an unpublished mode (the snap candidates) depend on that distinction, which
// only became reachable once the M7 LRU started competing for the 8 slots.
static BOOL IddModeSetAdd(IN OUT IDD_MODE* modes, IN OUT DWORD* count, IN LONG w, IN LONG h)
{
    if (w <= 0 || h <= 0 || !IS_RESOLUTION_VALID((ULONG)w, (ULONG)h))
        return FALSE;
    for (DWORD i = 0; i < *count; i++)
        if (modes[i].w == (ULONG)w && modes[i].h == (ULONG)h)
            return TRUE; // dedupe: already published
    if (*count >= IDD_MODE_SET_MAX)
        return FALSE;
    modes[*count].w = (ULONG)w;
    modes[*count].h = (ULONG)h;
    (*count)++;
    return TRUE;
}

// ---- M7: recent-size LRU ---------------------------------------------------
// Every novel size costs an IDD reload (monitor departure/arrival), and rapid
// display-topology churn is what livelocks the guest (FINDINGS 2026-08-05 cont 9,
// 2026-08-06 cont 4). Publishing the last N distinct dom0-requested sizes makes
// "go back to a size I just had" - a large fraction of real resizing - cost
// replug=0, which removes the reload entirely instead of merely spacing it out.
// Memory only: the LRU is never persisted anywhere except inside the mode set it
// helps compute, so an agent restart starts from an empty LRU.
// Only sizes that were APPLIED successfully are recorded (never a RESKEEP), so a
// size the driver or Windows refused can never enter the set through here.
// Host-screen-sized entries CAN appear in the LRU, but only because dom0 asked
// for them through the target slot - the same condition under which the builder
// is already allowed to publish them (user rule 2: the builder still never
// fabricates a host size on its own).
#define RECENT_SIZE_MAX 4

static SRWLOCK g_RecentLock = SRWLOCK_INIT;
static IDD_MODE g_RecentSizes[RECENT_SIZE_MAX]; // [0] = most recent
static DWORD g_RecentCount;

// Move WxH to the front of the LRU (insert + evict oldest when full).
// Called from the resolution thread (successful exact apply) and once from the
// main loop thread (boot publish); readers take the lock shared.
static void RecentSizeNote(IN ULONG w, IN ULONG h)
{
    if (!IS_RESOLUTION_VALID(w, h))
        return;

    AcquireSRWLockExclusive(&g_RecentLock);
    DWORD at = g_RecentCount; // insertion point when not already present
    for (DWORD i = 0; i < g_RecentCount; i++)
    {
        if (g_RecentSizes[i].w == w && g_RecentSizes[i].h == h)
        {
            at = i;
            break;
        }
    }
    if (at == g_RecentCount && g_RecentCount < RECENT_SIZE_MAX)
        g_RecentCount++;
    if (at >= RECENT_SIZE_MAX)
        at = RECENT_SIZE_MAX - 1; // full and novel: the oldest entry falls off
    for (DWORD i = at; i > 0; i--)
        g_RecentSizes[i] = g_RecentSizes[i - 1];
    g_RecentSizes[0].w = w;
    g_RecentSizes[0].h = h;
    ReleaseSRWLockExclusive(&g_RecentLock);
}

// Build the multi-sz mode set for a target size. desc receives the M6SET log tail
// ("n=<count> target=WxH lru=<n> [max=WxH] [half=WxH]", absent entries omitted;
// lru = how many of the 8 slots the M7 recent-size LRU contributed); callers
// log it when they actually write. Returns the multi-sz length in WCHARs
// (including both terminators), 0 on failure.
// Habitual-size snap candidates (work-area-derived entries of the published
// set ONLY - never the previous target, so small deliberate adjustments are
// not undone). User rule 2026-08-05: a dom0 request within 15 px of one of
// these snaps to it (blink-free, since it is published).
static ULONG g_SnapCandidates[4][2];
static DWORD g_SnapCandidateCount;
#define HABITUAL_SNAP_PX 15

static DWORD BuildIddModeSet(IN ULONG targetW, IN ULONG targetH,
    OUT WCHAR* multiSz, IN DWORD cchMax, OUT WCHAR* desc, IN DWORD cchDesc)
{
    IDD_MODE modes[IDD_MODE_SET_MAX];
    DWORD count = 0;

    // a. the target, always first (dom0's request slot)
    IddModeSetAdd(modes, &count, (LONG)targetW, (LONG)targetH);

    // a1. SEAMLESS: while seamless mode is active the guest desktop IS the
    // host-sized surface - SetSeamlessMode forces g_HostScreen* on every
    // StartFrameProcessing - so the host size must be OFFERED or that force
    // fails with DISP_CHANGE_BADMODE and seamless is dead (measured
    // 2026-08-06: host 5120x1440 absent from the computed set, work area and
    // capture failed behind it). This is consistent with the user's rule that
    // host-size modes appear only when the window is effectively fullscreen:
    // in seamless the whole host screen IS the window. Placed right after the
    // target so the 8-entry cap can never drop it, and deliberately NOT a snap
    // candidate - snapping a windowed resize toward the host size is exactly
    // what the user forbade.
    if (g_SeamlessMode && IS_RESOLUTION_VALID(g_HostScreenWidth, g_HostScreenHeight))
    {
        if (IddModeSetAdd(modes, &count, (LONG)g_HostScreenWidth, (LONG)g_HostScreenHeight))
            LogInfo("M6SEAMLESS host %lux%lu added to set", g_HostScreenWidth, g_HostScreenHeight);
        else
            LogWarning("M6SEAMLESS host %lux%lu NOT in set - seamless force will fail",
                g_HostScreenWidth, g_HostScreenHeight);
    }

    // a2. recently applied dom0 sizes (M7 LRU), most recent first, right after
    // the target: returning to one of them then needs no reload at all.
    DWORD lru = 0;
    AcquireSRWLockShared(&g_RecentLock);
    for (DWORD i = 0; i < g_RecentCount; i++)
    {
        DWORD before = count;
        IddModeSetAdd(modes, &count, (LONG)g_RecentSizes[i].w, (LONG)g_RecentSizes[i].h);
        if (count > before)
            lru++; // carried by the LRU (deduped-away entries are not counted)
    }
    ReleaseSRWLockShared(&g_RecentLock);

    // b+c. habitual sizes from the dom0 work-area feed. Frame extents are
    // WINDOW-STATE-DEPENDENT: this WM hides borders when maximized (measured:
    // real maximized client = waW x (waH - title) while normal windows carry
    // 5/5/25/5) - so publish BOTH variants of maximize and tile-half; dedupe
    // collapses them when extents are zero anyway. These entries double as the
    // snap candidates (user rule: requests within 15 px of a habitual size
    // snap to it - see SetVideoModeExact).
    int x, y, w, h, fl, fr, ft, fb;
    g_SnapCandidateCount = 0;
    if (WorkAreaGetDom0Raw(&x, &y, &w, &h, &fl, &fr, &ft, &fb))
    {
        LONG cand[4][2] = {
            { w,           h - ft },           // maximize, borderless-maximized WM
            { w - fl - fr, h - ft - fb },      // maximize, bordered
            { w / 2,           h - ft },       // tile half, borderless
            { w / 2 - fl - fr, h - ft - fb },  // tile half, bordered
        };
        for (int i = 0; i < 4; i++)
        {
            // A snap candidate MUST be in the published set - snapping to a mode
            // the driver does not offer would cost a replug, the exact opposite
            // of the snap's purpose. With the M7 LRU competing for the 8 slots a
            // habitual size can now be squeezed out, so record only what was
            // actually published.
            BOOL published = IddModeSetAdd(modes, &count, cand[i][0], cand[i][1]);
            // Snap targets are the BORDERED variants only (odd indices): a
            // maximize gesture emits the borderless size EXACTLY (measured) and
            // needs no snap, while any window close-but-not-equal is a normal
            // bordered window - snapping it to a borderless height overflows
            // the work area and pushes the bottom border off-screen (measured:
            // taskbar at the last screen row, border invisible).
            if ((i % 2) == 1 && published &&
                g_SnapCandidateCount < RTL_NUMBER_OF(g_SnapCandidates) &&
                IS_RESOLUTION_VALID((ULONG)cand[i][0], (ULONG)cand[i][1]))
            {
                g_SnapCandidates[g_SnapCandidateCount][0] = (ULONG)cand[i][0];
                g_SnapCandidates[g_SnapCandidateCount][1] = (ULONG)cand[i][1];
                g_SnapCandidateCount++;
            }
        }
    }

    // d. safety-net fallback
    IddModeSetAdd(modes, &count, 1024, 768);

    if (count == 0)
        return 0;

    DWORD used = 0;
    for (DWORD i = 0; i < count; i++)
    {
        if (cchMax - used < 13) // "99999x99999" + NUL + final multi-sz NUL
            return 0;
        if (FAILED(StringCchPrintf(multiSz + used, cchMax - used - 1, L"%lux%lu",
            modes[i].w, modes[i].h)))
            return 0;
        used += (DWORD)wcslen(multiSz + used) + 1;
    }
    multiSz[used++] = L'\0'; // multi-sz double terminator

    StringCchPrintf(desc, cchDesc, L"n=%lu target=%lux%lu lru=%lu", count, targetW, targetH, lru);
    WCHAR item[48];
    if (g_SnapCandidateCount >= 2)
    {
        StringCchPrintf(item, RTL_NUMBER_OF(item), L" max=%lux%lu/%lux%lu",
            g_SnapCandidates[0][0], g_SnapCandidates[0][1],
            g_SnapCandidates[1][0], g_SnapCandidates[1][1]);
        StringCchCat(desc, cchDesc, item);
    }
    if (g_SnapCandidateCount >= 4)
    {
        StringCchPrintf(item, RTL_NUMBER_OF(item), L" half=%lux%lu/%lux%lu",
            g_SnapCandidates[2][0], g_SnapCandidates[2][1],
            g_SnapCandidates[3][0], g_SnapCandidates[3][1]);
        StringCchCat(desc, cchDesc, item);
    }

    return used;
}

// Write a built multi-sz to the key the Qubes IDD driver reads on monitor
// arrival / IOCTL reload (REG_QUBES_IDD_KEY in include/common.h) and remember it
// for change detection. Logs M6SET on success.
static DWORD WriteIddModeSetRaw(IN const WCHAR* multiSz, IN DWORD cch, IN const WCHAR* desc)
{
    HKEY key;
    DWORD status = RegCreateKeyEx(HKEY_LOCAL_MACHINE, REG_QUBES_IDD_KEY, 0, NULL, 0,
        KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "creating IDD modes registry key");

    status = RegSetValueEx(key, REG_QUBES_IDD_MODES_VALUE, 0, REG_MULTI_SZ,
        (const BYTE*)multiSz, cch * sizeof(WCHAR));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "writing IDD modes registry value");

    AcquireSRWLockExclusive(&g_IddModeSetLock);
    memcpy(g_IddModeSetLast, multiSz, cch * sizeof(WCHAR));
    g_IddModeSetLastCch = cch;
    ReleaseSRWLockExclusive(&g_IddModeSetLock);

    LogInfo("M6SET %s", desc);
    return ERROR_SUCCESS;
}

// Publish the computed mode set for a target size (unconditional write; callers
// on the obtain path reload the driver afterwards).
static DWORD WriteIddModeSet(IN ULONG targetW, IN ULONG targetH)
{
    WCHAR multiSz[IDD_MODES_CCH];
    WCHAR desc[128];
    DWORD cch = BuildIddModeSet(targetW, targetH, multiSz, RTL_NUMBER_OF(multiSz),
        desc, RTL_NUMBER_OF(desc));
    if (cch == 0)
        return ERROR_INVALID_PARAMETER;

    return WriteIddModeSetRaw(multiSz, cch, desc);
}

// Ask the RUNNING Qubes IDD driver to re-read the modes key via
// IOCTL_QIDD_RELOAD_MODES (monitor-level departure/arrival, no PnP restart;
// see QIDD_INTERFACE_GUID_INIT in include/common.h). Returns TRUE if at least
// one interface instance accepted the IOCTL.
static BOOL QiddReloadModes(void)
{
    static const GUID qiddInterfaceGuid = QIDD_INTERFACE_GUID_INIT;

    HDEVINFO devList = SetupDiGetClassDevs(&qiddInterfaceGuid, NULL, NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devList == INVALID_HANDLE_VALUE)
    {
        win_perror("SetupDiGetClassDevs(QIDD interface)");
        return FALSE;
    }

    BOOL reloaded = FALSE;
    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devList, NULL, &qiddInterfaceGuid, i, &iface); i++)
    {
        BYTE detailBuffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + MAX_PATH * sizeof(WCHAR)];
        SP_DEVICE_INTERFACE_DETAIL_DATA* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA*)detailBuffer;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(devList, &iface, detail, sizeof(detailBuffer), NULL, NULL))
        {
            win_perror("SetupDiGetDeviceInterfaceDetail(QIDD)");
            continue;
        }

        HANDLE device = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (device == INVALID_HANDLE_VALUE)
        {
            win_perror("opening QIDD device interface");
            continue;
        }

        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device, IOCTL_QIDD_RELOAD_MODES, NULL, 0, NULL, 0, &returned, NULL);
        CloseHandle(device);
        if (!ok)
        {
            win_perror("IOCTL_QIDD_RELOAD_MODES");
            continue;
        }

        reloaded = TRUE;
    }

    SetupDiDestroyDeviceInfoList(devList);
    return reloaded;
}

// ---- M7: reload rate limiter -----------------------------------------------
// A reload is a display-topology change (monitor departure + arrival). Two of
// them in quick succession is the pattern under which the guest livelocks
// against the Xen PV machinery (FINDINGS 2026-08-05 cont 9, 2026-08-06 cont 4),
// and the dom0-side fixes are gated on upstream review, so the guest enforces a
// minimum spacing itself.
//
// The wait happens ONLY on the obtain path in SetVideoModeExact, INSIDE the
// branch that has already established that the requested mode is not offered and
// a reload is therefore unavoidable. Every replug=0 route (RESNOOP, the habitual
// snap onto a published size, an LRU/published size that IsExactModeAvailable
// accepts) returns or falls through without ever reaching this call, so nothing
// that would not have replugged anyway can be delayed by it.
//
// Sleeping there is safe: the obtain runs on the resolution thread, which is
// debounced and latest-wins - it holds no vchan state and a newer dom0 request
// simply supersedes this one. ResolutionPublishBootModeSet runs on the main loop
// thread instead and therefore only STAMPS the tick, never waits.
#define MIN_REPLUG_INTERVAL_MS 2500

// GetTickCount64 of the last reload we issued; 0 = none this process.
// Written on the resolution thread and once on the main loop thread (boot
// publish, which cannot race it - the connect sequence precedes any obtain).
static ULONGLONG g_LastReloadTick;

static void NoteIddReload(void)
{
    g_LastReloadTick = GetTickCount64();
}

// Block until MIN_REPLUG_INTERVAL_MS has passed since the last reload.
// Resolution thread only.
static void ThrottleBeforeIddReload(void)
{
    if (g_LastReloadTick == 0)
        return;

    ULONGLONG now = GetTickCount64();
    ULONGLONG since = now - g_LastReloadTick;
    if (since >= MIN_REPLUG_INTERVAL_MS)
        return;

    ULONGLONG wait = MIN_REPLUG_INTERVAL_MS - since;
    LogInfo("M7THROTTLE waiting %I64u ms before reload", wait);
    Sleep((DWORD)wait);
}

// Restart the Qubes IDD device in-process (what `devcon restart` does) so the
// driver re-reads the modes key. Returns TRUE if a matching device was restarted.
// Fallback only: the PnP replug disturbs the Xen platform device (grant-revoke
// spins, lost event-channel delivery) - prefer QiddReloadModes().
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

// Is at least one QIDD device interface present? Guard for the boot publish so
// non-IDD systems are left alone entirely.
static BOOL QiddInterfacePresent(void)
{
    static const GUID qiddInterfaceGuid = QIDD_INTERFACE_GUID_INIT;

    HDEVINFO devList = SetupDiGetClassDevs(&qiddInterfaceGuid, NULL, NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devList == INVALID_HANDLE_VALUE)
        return FALSE;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    BOOL present = SetupDiEnumDeviceInterfaces(devList, NULL, &qiddInterfaceGuid, 0, &iface);
    SetupDiDestroyDeviceInfoList(devList);
    return present;
}

// One-shot startup publish: arm the driver's mode list with the full computed set
// and make it live with a single reload, so the habitual sizes switch blink-free
// from first use. Called from the connect sequence after HandleXconf/SetVideoMode
// and WorkAreaInit/WorkAreaApply, i.e. when g_Screen* is final and the dom0
// work-area feed (if any) has been read. Every failure is non-fatal (NEVEREXIT):
// log and continue - the exact-obtain path still works without it.
void ResolutionPublishBootModeSet(void)
{
    static BOOL done; // connect sequence runs on the main loop thread only
    if (done)
        return;
    done = TRUE;

    if (!IS_RESOLUTION_VALID(g_ScreenWidth, g_ScreenHeight))
    {
        LogWarning("M6BOOT skipped: no valid screen size (%lux%lu)",
            g_ScreenWidth, g_ScreenHeight);
        return;
    }

    if (!QiddInterfacePresent())
    {
        LogInfo("M6BOOT skipped: QIDD interface not present");
        return;
    }

    // Seed the M7 LRU with the size the desktop is actually running at (dom0's
    // xconf / the last size that stuck), so the first resize away from it can
    // come back for free. It is an applied size by definition, not a request.
    RecentSizeNote(g_ScreenWidth, g_ScreenHeight);

    if (WriteIddModeSet(g_ScreenWidth, g_ScreenHeight) != ERROR_SUCCESS)
    {
        LogWarning("M6BOOT modes-key write failed - continuing"); // non-fatal
        return;
    }

    // No throttle wait here (main loop thread must not sleep), but the boot
    // reload still counts as a topology change: stamp it so an obtain arriving
    // right after connect is spaced away from it by the limiter.
    NoteIddReload();
    if (QiddReloadModes())
        LogInfo("M6BOOT target=%lux%lu reloaded", g_ScreenWidth, g_ScreenHeight);
    else
        LogWarning("M6BOOT reload failed - set published, live on next obtain");
}

// Recompute the published set when the dom0 work-area feed changes (called from
// WorkAreaSetDom0, i.e. the qubesdb watcher and MSG_WORKAREA paths). Registry
// rewrite ONLY, and only on an actual change - no reload: a dom0 panel move must
// not blink the screen; the next natural obtain makes the new set live.
void ResolutionRecomputeIddModeSet(void)
{
    // Target slot = the currently applied screen size (the last dom0 request that
    // stuck). Before the first successful mode set there is no target yet; the
    // boot publish covers first arming.
    ULONG targetW = g_ScreenWidth, targetH = g_ScreenHeight;
    if (!IS_RESOLUTION_VALID(targetW, targetH))
        return;

    WCHAR multiSz[IDD_MODES_CCH];
    WCHAR desc[128];
    DWORD cch = BuildIddModeSet(targetW, targetH, multiSz, RTL_NUMBER_OF(multiSz),
        desc, RTL_NUMBER_OF(desc));
    if (cch == 0)
        return;

    AcquireSRWLockShared(&g_IddModeSetLock);
    BOOL same = (g_IddModeSetLastCch == cch &&
        0 == memcmp(g_IddModeSetLast, multiSz, cch * sizeof(WCHAR)));
    ReleaseSRWLockShared(&g_IddModeSetLock);
    if (same)
        return;

    if (WriteIddModeSetRaw(multiSz, cch, desc) == ERROR_SUCCESS)
        LogInfo("M6DEFER registry updated, live on next obtain");
    // failure already logged by WriteIddModeSetRaw; non-fatal by design
}

// dom0's window size is the single source of truth: any applied size other than
// the exact requested one makes gui-daemon resize the user's window to match
// ("resize-to-viewport" - forbidden). Either the exact size is applied, if needed
// after obtaining it from the Qubes IDD, or the current resolution is kept as-is.
// Runs only on the resolution-change thread (the sole dom0-sourced call site goes
// through RequestResolutionChange), so the replug+wait never blocks the vchan loop.
// Stale-echo filter state: during the replug's capture outage gui-daemon's window
// still has the PREVIOUS size and echoes it back as MSG_CONFIGURE; treating that
// echo as fresh dom0 intent reverted a just-applied exact size (user-reported
// 2026-08-05). Remember what size an exact apply REPLACED and when; a dom0 request
// for exactly that size arriving within the echo window is dropped. Genuine user
// intent survives: a real drag back keeps requesting past the window and the
// debounced latest-wins path re-delivers it.
static ULONG g_EchoPrevW, g_EchoPrevH;
static ULONGLONG g_EchoWindowEnd; // GetTickCount64 deadline; 0 = no window
#define EXACT_ECHO_WINDOW_MS 4000

// While an exact-obtain (replug + apply) is in flight the desktop TRANSITS through
// intermediate real modes (monitor re-arrival default, e.g. 1920x1080); the daemon
// must never learn about them (they echo back as fake dom0 intent and the window
// visibly twitches - user-reported spontaneous revert, FINDINGS 2026-08-05).
// Written on the resolution thread only; read cross-thread as a benign racy hint.
static volatile LONG g_ExactInFlight;
static ULONG g_ExactTargetW, g_ExactTargetH;

// M0BLINK: GetTickCount64 stamp taken at obtain-start (just before the registry
// write of a novel-size obtain). Shared with the A6ACKREPAINT site in
// vchan-handlers.c, which read-and-clears it to log repaint latency relative to
// the obtain. 0 = no obtain awaiting its repaint.
volatile LONG64 g_M0BlinkObtainStart;
// Same stamp, consumed by the optimistic post-dump repaint in main.c (see
// resolution.h): that repaint, not the ack-gated one, is when pixels reach dom0.
volatile LONG64 g_M0BlinkFirstPaintStart;

LONG64 ResolutionM0BlinkObtainStart(void)
{
    return g_M0BlinkObtainStart;
}

BOOL ResolutionExactObtainInFlight(void)
{
    return g_ExactInFlight != 0;
}

// May the recovery path announce this geometry to the daemon (MSG_CONFIGURE w0)?
// The capture thread's recovery cycle LAGS the resolution thread's apply: a
// transit mode observed during the replug can surface up to ~100 ms after the
// in-flight flag clears (measured 75 ms - the transit leaked, the daemon echoed
// it, wrong size applied). So the gate covers the whole settle window: while it
// is open, ONLY the exact target may be announced.
BOOL ResolutionShouldAnnounceGeometry(IN ULONG width, IN ULONG height)
{
    if (g_ExactInFlight)
        return width == g_ExactTargetW && height == g_ExactTargetH;
    if (g_EchoWindowEnd != 0 && GetTickCount64() < g_EchoWindowEnd)
        return width == g_ExactTargetW && height == g_ExactTargetH;
    return TRUE;
}

// Sizes the desktop transited during the current/last obtain (recorded by the
// recovery path when it suppresses A6CONFIGURE). Only THESE, plus the pre-apply
// size, are echo suspects - anything else inside the settle window is genuine
// user intent and must be applied (a settled drag's request never re-arrives, so
// dropping it loses it: measured 2026-08-05, RESECHO ate a real 2055x1308).
#define ECHO_SUSPECT_MAX 6
static ULONG g_EchoSuspectW[ECHO_SUSPECT_MAX], g_EchoSuspectH[ECHO_SUSPECT_MAX];
static DWORD g_EchoSuspectCount;

void ResolutionNoteTransitSize(IN ULONG width, IN ULONG height)
{
    for (DWORD i = 0; i < g_EchoSuspectCount; i++)
        if (g_EchoSuspectW[i] == width && g_EchoSuspectH[i] == height)
            return;
    if (g_EchoSuspectCount < ECHO_SUSPECT_MAX)
    {
        g_EchoSuspectW[g_EchoSuspectCount] = width;
        g_EchoSuspectH[g_EchoSuspectCount] = height;
        g_EchoSuspectCount++;
    }
}

static BOOL IsEchoSuspect(IN ULONG width, IN ULONG height)
{
    if (width == g_EchoPrevW && height == g_EchoPrevH)
        return TRUE;
    for (DWORD i = 0; i < g_EchoSuspectCount; i++)
        if (g_EchoSuspectW[i] == width && g_EchoSuspectH[i] == height)
            return TRUE;
    return FALSE;
}

// allowSnap: apply the 15-px habitual-size snap below. TRUE for dom0 window
// follows (its whole point), FALSE for the seamless force - that one must land
// on the host size EXACTLY, and a work-area-derived habitual size can sit within
// 15 px of it (thin dom0 panel), which would silently mis-size the seamless
// desktop.
static ULONG SetVideoModeExact(IN ULONG width, IN ULONG height, IN BOOL allowSnap)
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

    // Echo handling inside the settle window after an apply. Only KNOWN-SUSPECT
    // sizes (the pre-apply size and sizes the desktop transited during the
    // obtain) are treated as possible daemon echoes - and even those are
    // DEFERRED to the window's end rather than dropped, then applied: a settled
    // drag's request never re-arrives, so a dropped genuine request is lost
    // forever (measured: a real 2055x1308 was eaten by the drop version).
    // Anything else is fresh user intent and applies immediately. Worst case for
    // a residual echo: one late bounce, which converges (each apply is exact and
    // the daemon follows it).
    if (g_EchoWindowEnd != 0 && width != g_ExactTargetW && IsEchoSuspect(width, height))
    {
        ULONGLONG now = GetTickCount64();
        if (now < g_EchoWindowEnd)
        {
            LogInfo("RESECHO %lux%lu suspect - deferring %I64u ms (settle window after %lux%lu)",
                width, height, g_EchoWindowEnd - now, g_ExactTargetW, g_ExactTargetH);
            Sleep((DWORD)(g_EchoWindowEnd - now));
            // latest-wins still holds: if a newer request arrived meanwhile, the
            // resolution thread's event is set and it will supersede this apply.
        }
    }

    // Habitual-size snap (user rule): a request within 15 px of a published
    // work-area-derived mode takes that mode - blink-free and intentional; the
    // daemon then nudges the window to match (sanctioned resize-to-viewport,
    // bounded by 15 px and only toward tiles/maximize).
    for (DWORD i = 0; allowSnap && i < g_SnapCandidateCount; i++)
    {
        LONG dw = (LONG)width - (LONG)g_SnapCandidates[i][0];
        LONG dh = (LONG)height - (LONG)g_SnapCandidates[i][1];
        if (dw > -HABITUAL_SNAP_PX && dw < HABITUAL_SNAP_PX &&
            dh > -HABITUAL_SNAP_PX && dh < HABITUAL_SNAP_PX &&
            (dw != 0 || dh != 0))
        {
            LogInfo("RESSNAP15 %lux%lu -> %lux%lu (habitual size within %d px)",
                width, height, g_SnapCandidates[i][0], g_SnapCandidates[i][1],
                HABITUAL_SNAP_PX);
            width = g_SnapCandidates[i][0];
            height = g_SnapCandidates[i][1];
            if (width == g_ScreenWidth && height == g_ScreenHeight)
            {
                LogInfo("RESNOOP %lux%lu (post-snap)", width, height);
                return ERROR_SUCCESS;
            }
            break;
        }
    }

    // Decide ONCE whether this apply needs a reload, BEFORE anything is armed.
    // Everything the driver already offers - the target of the last obtain, the
    // M7 LRU entries, the habitual sizes - answers TRUE here and takes the
    // replug=0 route below without ever touching the rate limiter.
    // This block deliberately precedes the in-flight/target arming: while the
    // limiter waits, the desktop is still at the PREVIOUS target and must keep
    // streaming to dom0. Arming first would make ResolutionShouldAnnounceGeometry
    // hold every frame for the length of the wait (visible freeze).
    BOOL needObtain = !IsExactModeAvailable(width, height);
    if (needObtain)
    {
        // A reload is unavoidable, so it is now legal to spend time spacing it
        // away from the previous one.
        ThrottleBeforeIddReload();
        // The wait is long enough that a reload issued just before this request
        // (the boot publish, or a preceding obtain) may have landed meanwhile;
        // re-check rather than force a topology change nobody needs any more.
        needObtain = !IsExactModeAvailable(width, height);
        if (!needObtain)
            LogInfo("M7THROTTLE mode became available while waiting - no reload");
        // latest-wins still holds across the wait: a newer dom0 request has
        // already re-signalled the resolution thread's event and supersedes this
        // apply once it returns (same contract as the RESECHO defer above).
    }

    BOOL replugged = FALSE;
    ULONGLONG m0Start = 0; // M0BLINK obtain-start stamp; 0 = no obtain needed
    g_ExactTargetW = width;
    g_ExactTargetH = height;
    g_EchoSuspectCount = 0; // fresh obtain, fresh transit list
    InterlockedExchange(&g_ExactInFlight, 1);

    if (needObtain)
    {
        // try to obtain the exact mode from the Qubes IDD: publish the computed
        // mode set (target first) in the driver's registry key, make the driver
        // reload it (IOCTL preferred, PnP replug as fallback), wait for the mode
        // to appear
        m0Start = GetTickCount64();
        InterlockedExchange64(&g_M0BlinkObtainStart, (LONG64)m0Start);
        InterlockedExchange64(&g_M0BlinkFirstPaintStart, (LONG64)m0Start);
        LogInfo("M0BLINK obtain-start %lux%lu t=%I64u", width, height, m0Start);
        if (WriteIddModeSet(width, height) != ERROR_SUCCESS)
        {
            LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=modes-key-write-failed",
                width, height, g_ScreenWidth, g_ScreenHeight);
            InterlockedExchange64(&g_M0BlinkObtainStart, 0); // obtain aborted, no repaint expected
            InterlockedExchange64(&g_M0BlinkFirstPaintStart, 0);
            InterlockedExchange(&g_ExactInFlight, 0);
            return ERROR_SUCCESS;
        }

        if (QiddReloadModes())
        {
            LogInfo("QIDD ioctl-reload ok");
        }
        else
        {
            LogWarning("QIDD ioctl-reload unavailable, falling back to device restart (PnP)");
            if (!RestartQubesIddDevice())
            {
                LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=idd-not-present",
                    width, height, g_ScreenWidth, g_ScreenHeight);
                InterlockedExchange64(&g_M0BlinkObtainStart, 0); // obtain aborted, no repaint expected
                InterlockedExchange64(&g_M0BlinkFirstPaintStart, 0);
                InterlockedExchange(&g_ExactInFlight, 0);
                return ERROR_SUCCESS;
            }
        }

        // A topology change was issued (either branch above returned early on
        // failure): stamp it so the next one is spaced by the M7 limiter.
        NoteIddReload();

        {
            ULONGLONG now = GetTickCount64();
            LogInfo("M0BLINK reload-returned t=%I64u sinceobtain=%I64u ms", now, now - m0Start);
        }

        // Poll cadence. The old loop SLEPT A FULL 250 ms STEP BEFORE ITS FIRST CHECK,
        // so the mode-offered phase could never cost less than 250 ms no matter how
        // fast the driver was - and the measurement says the driver is fast:
        // reload-returned at 16 ms, mode-offered at 281 ms, polls=1. That is one
        // quantum of sleeping followed by a check that passed on its first try; the
        // mode was already being offered and nothing was looking. Check immediately,
        // then poll tightly, then decay to the old step so a driver that genuinely
        // takes seconds costs the same handful of wakeups it always did. The probe is
        // ChangeDisplaySettings(CDS_TEST) - a query, it never changes the mode.
        ULONGLONG waitStart = GetTickCount64();
        ULONG polls = 0;
        BOOL offered = FALSE;
        for (;;)
        {
            polls++;
            if (IsExactModeAvailable(width, height))
            {
                offered = TRUE;
                break;
            }
            ULONGLONG elapsed = GetTickCount64() - waitStart;
            if (elapsed >= EXACT_MODE_WAIT_TIMEOUT_MS)
                break;
            Sleep(elapsed < 500 ? 15 : (elapsed < 2000 ? 50 : EXACT_MODE_WAIT_STEP_MS));
        }

        if (!offered)
        {
            LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=mode-never-appeared",
                width, height, g_ScreenWidth, g_ScreenHeight);
            InterlockedExchange64(&g_M0BlinkObtainStart, 0); // obtain aborted, no repaint expected
            InterlockedExchange64(&g_M0BlinkFirstPaintStart, 0);
            InterlockedExchange(&g_ExactInFlight, 0);
            return ERROR_SUCCESS;
        }

        {
            ULONGLONG now = GetTickCount64();
            LogInfo("M0BLINK mode-offered t=%I64u polls=%lu sinceobtain=%I64u ms",
                now, polls, now - m0Start);
        }

        replugged = TRUE;
    }

    ULONG status = SetVideoModeInternal(width, height);
    if (status != ERROR_SUCCESS)
    {
        LogWarning("RESKEEP %lux%lu-unavailable keeping %lux%lu reason=apply-failed-0x%lx",
            width, height, g_ScreenWidth, g_ScreenHeight, status);
        InterlockedExchange64(&g_M0BlinkObtainStart, 0); // no repaint expected for this obtain
        InterlockedExchange64(&g_M0BlinkFirstPaintStart, 0);
        InterlockedExchange(&g_ExactInFlight, 0);
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

    if (m0Start != 0)
    {
        ULONGLONG now = GetTickCount64();
        LogInfo("M0BLINK applied %lux%lu t=%I64u sinceobtain=%I64u ms", width, height, now, now - m0Start);
    }

    InterlockedExchange(&g_ExactInFlight, 0);

    // A display mode change makes Windows reload the cursor scheme, undoing
    // HideCursors()' blanked system cursors - the guest "shadow cursor" returns
    // (user-reported after resizes). Re-blank after every applied mode change;
    // HideCursors self-guards on DisableCursor and is idempotent.
    HideCursors();

    // M7: this size is now APPLIED (every RESKEEP path returned above), so it
    // earns its place in the LRU and will be published with the next mode set -
    // coming back to it later is then a replug=0 apply.
    RecentSizeNote(width, height);

    // arm the stale-echo filter with the size this apply REPLACED
    g_EchoPrevW = g_ScreenWidth;
    g_EchoPrevH = g_ScreenHeight;
    g_EchoWindowEnd = GetTickCount64() + EXACT_ECHO_WINDOW_MS;

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
        return SetVideoModeExact(width, height, TRUE);

    // Seamless needs the HOST size EXACTLY (the framebuffer the daemon composits
    // from is host-sized). On an IDD-backed desktop the offered modes are the
    // computed set, so SelectSupportedMode's boot-time g_SupportedModes cache
    // cannot contain the host size and its "closest" pick is a mode the driver
    // no longer offers -> ChangeDisplaySettings BADMODE, no seamless (measured
    // 2026-08-06). Route the force through the exact-follow path instead, which
    // publishes the set (host entry included by BuildIddModeSet, see a1),
    // reloads the driver through QiddReloadModes and waits for the mode - all
    // under the M7 replug rate limiter. Only when the Qubes IDD is actually
    // present: on a fixed-mode adapter the legacy snap is still the better
    // answer, and stock behaviour must not change.
    if (source && 0 == wcscmp(source, L"seamless-force") && QiddInterfacePresent())
        return SetVideoModeExact(width, height, FALSE); // never snapped

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

        HideCursors(); // mode change reloads the cursor scheme - re-blank (see exact path)
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
