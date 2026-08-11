/*
 * toastcrop - measure the visible card inside a Windows shell toast window.
 * See toastcrop.h for what this exists to fix and why UIA is the only instrument.
 */

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wchar.h>

// The MIDL-generated UIA client interfaces are used through their C vtable macros, like
// the DXGI/D3D11 ones in capture.h.
#define COBJMACROS
#include <uiautomation.h>

#include "main.h"
#include "toastcrop.h"

#include <log.h>
#include <config.h>

// Registry values (module key: HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent),
// read exactly as perf.c reads its own.
#define REG_CONFIG_TOASTCROP_DISABLE_VALUE L"ToastCropDisable"
#define REG_CONFIG_TOASTCROP_LEFT_VALUE    L"ToastCropL"
#define REG_CONFIG_TOASTCROP_TOP_VALUE     L"ToastCropT"
#define REG_CONFIG_TOASTCROP_RIGHT_VALUE   L"ToastCropR"
#define REG_CONFIG_TOASTCROP_BOTTOM_VALUE  L"ToastCropB"

// Only ShellExperienceHost banners are cropped. The Action Center flyout comes from the
// same process with the same styles, so it is excluded by size: measured toasts are
// 396 px wide and at most a few hundred tall (396x133 collapsed, 396x332 expanded), the
// flyout is a full-height column. Generous on purpose - the ceiling only decides whether
// UIA is asked, and a window that has no toast card measures no insets anyway.
#define TOAST_MAX_RAW_WIDTH  1000u
#define TOAST_MAX_RAW_HEIGHT 600u

// Absolute floor for a cropped window, used with the SM_CXMIN/SM_CYMIN floor below. The
// XAML card of a real toast is hundreds of px wide; anything this small is a mismeasure.
#define TOAST_CROP_FLOOR_WIDTH  32
#define TOAST_CROP_FLOOR_HEIGHT 24

// A card must keep at least this percentage of each raw dimension. The whole failure mode
// this module must never have is a SILENT OVERCROP that clips the toast's action buttons,
// and a UIA hit on some unrelated inner element would look exactly like a valid crop.
// Measured cards: 364x90 in 396x133 (92% x 68%), 377x287 in 396x332 (95% x 86%).
#define TOAST_CROP_MIN_PERCENT 40

// UIA attempts per (hwnd, raw size) key, and the quiet time between them. The XAML tree
// is not always populated the first time the agent sees the window, but an unbounded
// retry would put a COM call in the per-frame tracking pass for every toast that has no
// card - and back-to-back retries would spend all of them inside two frames, before the
// tree could possibly have appeared. Three attempts ~150 ms apart cover the slide-in
// while keeping the cross-process calls off the input-rate path.
#define TOAST_CROP_MAX_ATTEMPTS 3
#define TOAST_CROP_RETRY_MS     150

#define TOAST_CROP_CACHE_SIZE 16
#define TOAST_PID_CACHE_SIZE  8

extern DWORD g_MinWindowWidth;  // main.c
extern DWORD g_MinWindowHeight; // main.c
BOOL HasFlags(DWORD value, DWORD flags); // main.c

typedef struct _TOAST_CROP_ENTRY
{
    HWND      Window;
    DWORD     RawWidth;
    DWORD     RawHeight;
    RECT      Insets;
    BOOL      Resolved;   // measured, or retries exhausted - no more UIA for this key
    UINT      Attempts;
    ULONGLONG RetryAt;    // GetTickCount64() before which no further UIA call is made
    ULONGLONG LastUse;    // g_TcClock stamp, for eviction when the cache is full
} TOAST_CROP_ENTRY;

typedef struct _TOAST_PID_ENTRY
{
    DWORD     ProcessId;
    BOOL      IsShellExperienceHost;
    ULONGLONG LastUse;
} TOAST_PID_ENTRY;

static INIT_ONCE g_TcInitOnce = INIT_ONCE_STATIC_INIT;

// Guards everything below. GetWindowData() runs on both the window-event thread and the
// main loop, so the module cannot rely on the caller's g_csWatchedWindows; taking our own
// lock inside theirs is safe because nothing here ever calls back into main.c.
static CRITICAL_SECTION g_TcLock;

static BOOL g_TcDisabled = FALSE;
static BOOL g_TcForced = FALSE;         // registry insets replace the UIA measurement
static RECT g_TcForcedInsets;

static IUIAutomation* g_TcUia = NULL;   // created on first use, kept for the process life
static TOAST_CROP_ENTRY g_TcCache[TOAST_CROP_CACHE_SIZE];
static TOAST_PID_ENTRY g_TcPidCache[TOAST_PID_CACHE_SIZE];
static ULONGLONG g_TcClock = 0;

// COM is per thread, so this tracks the calling thread, not the module.
static __declspec(thread) BOOL g_TcComReady = FALSE;

// CLSID_CUIAutomation / IID_IUIAutomation live in uuid.lib. Defining them here instead
// keeps the agent's link line free of an ordering assumption about a library it does not
// otherwise need.
static const CLSID g_TcClsidUIAutomation =
    { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const IID g_TcIidUIAutomation =
    { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
// CUIAutomation8 / IUIAutomation2 exist only so the cross-process calls can be given a
// DEADLINE. Every UIA call below is a synchronous RPC into ShellExperienceHost; without a
// timeout a hung or suspended shell process would block the window-tracking pass, turning a
// cosmetic crop into a frozen desktop - the exact failure class the vchan work just removed
// from the send path. Absent on very old builds, hence the fallback to plain CUIAutomation.
static const CLSID g_TcClsidUIAutomation8 =
    { 0xe22ad333, 0xb25f, 0x460c, { 0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9 } };
static const IID g_TcIidUIAutomation2 =
    { 0x34723aff, 0x0c9d, 0x49d0, { 0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a } };

// Milliseconds. Generous enough that a merely busy shell still answers, short enough that a
// dead one cannot stall a frame for a human-visible time.
#define TOAST_CROP_UIA_TIMEOUT_MS 500

// The XAML element that IS the visible card. Both names are shipped by different Windows
// builds; neither is documented, hence the registry escape hatch in toastcrop.h.
static const WCHAR* const g_TcCardClasses[] = { L"FlexibleToastView", L"ToastView" };

static const WCHAR g_TcShellHostImage[] = L"ShellExperienceHost.exe";
static const WCHAR g_TcToastWindowClass[] = L"Windows.UI.Core.CoreWindow";

static BOOL WINAPI TcInitOnceCallback(PINIT_ONCE initOnce, PVOID parameter, PVOID* context)
{
    UNREFERENCED_PARAMETER(initOnce);
    UNREFERENCED_PARAMETER(parameter);
    UNREFERENCED_PARAMETER(context);

    InitializeCriticalSection(&g_TcLock);

    WCHAR moduleName[CFG_MODULE_MAX];
    DWORD value;

    if (ERROR_SUCCESS == CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName)))
    {
        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTCROP_DISABLE_VALUE, &value, NULL))
            g_TcDisabled = (value != 0);

        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTCROP_LEFT_VALUE, &value, NULL))
        {
            g_TcForcedInsets.left = (LONG)value;
            g_TcForced = TRUE;
        }
        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTCROP_TOP_VALUE, &value, NULL))
        {
            g_TcForcedInsets.top = (LONG)value;
            g_TcForced = TRUE;
        }
        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTCROP_RIGHT_VALUE, &value, NULL))
        {
            g_TcForcedInsets.right = (LONG)value;
            g_TcForced = TRUE;
        }
        if (ERROR_SUCCESS == CfgReadDword(moduleName, REG_CONFIG_TOASTCROP_BOTTOM_VALUE, &value, NULL))
        {
            g_TcForcedInsets.bottom = (LONG)value;
            g_TcForced = TRUE;
        }
    }

    // Logged unconditionally: a captured log must state which condition produced it, the
    // same reason perf.c logs its switches whether or not they are on.
    if (g_TcDisabled)
        LogInfo("QGATOASTCROP off (ToastCropDisable)");
    else if (g_TcForced)
        LogInfo("QGATOASTCROP on, forced insets l=%d t=%d r=%d b=%d",
            g_TcForcedInsets.left, g_TcForcedInsets.top, g_TcForcedInsets.right, g_TcForcedInsets.bottom);
    else
        LogInfo("QGATOASTCROP on, measuring with UIA");

    return TRUE;
}

static void TcInit(void)
{
    InitOnceExecuteOnce(&g_TcInitOnce, TcInitOnceCallback, NULL, NULL);
}

// COM is per thread and the agent's threads never initialize it for themselves, so the
// first UIA call on each thread does. Never uninitialized: the automation object outlives
// any single query.
static BOOL TcEnsureCom(void)
{
    if (g_TcComReady)
        return TRUE;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means the thread already lives in an STA - COM is usable, it is
    // just not ours to configure.
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LogWarning("CoInitializeEx failed: 0x%x, no toast crop on this thread", hr);
        return FALSE;
    }

    g_TcComReady = TRUE;
    return TRUE;
}

// g_TcLock must be held.
static IUIAutomation* TcGetAutomation(void)
{
    if (g_TcUia)
        return g_TcUia;

    // Preferred path: CUIAutomation8 gives an IUIAutomation2 whose connection/transaction
    // timeouts bound every later call. If anything about it fails, fall through to the plain
    // object rather than losing the crop entirely.
    IUIAutomation2* uia2 = NULL;
    HRESULT hr = CoCreateInstance(&g_TcClsidUIAutomation8, NULL, CLSCTX_INPROC_SERVER,
        &g_TcIidUIAutomation2, (void**)&uia2);
    if (SUCCEEDED(hr) && uia2)
    {
        // Best-effort: a build that rejects the property still gives a usable object.
        HRESULT hrConn = IUIAutomation2_put_ConnectionTimeout(uia2, TOAST_CROP_UIA_TIMEOUT_MS);
        HRESULT hrTx = IUIAutomation2_put_TransactionTimeout(uia2, TOAST_CROP_UIA_TIMEOUT_MS);

        hr = IUIAutomation2_QueryInterface(uia2, &g_TcIidUIAutomation, (void**)&g_TcUia);
        IUIAutomation2_Release(uia2);

        if (SUCCEEDED(hr) && g_TcUia)
        {
            LogInfo("TOASTCROP UIA ready with %u ms timeouts (conn=0x%x, tx=0x%x)",
                (unsigned)TOAST_CROP_UIA_TIMEOUT_MS, hrConn, hrTx);
            return g_TcUia;
        }

        g_TcUia = NULL;
        LogWarning("TOASTCROP QueryInterface(IUIAutomation) failed: 0x%x, retrying untimed", hr);
    }

    // Fallback: no deadline available. Still bounded in practice by the retry cap, and a crop
    // that never happens is better than a toast that never appears.
    LogWarning("TOASTCROP CUIAutomation8 unavailable (0x%x) - falling back to untimed UIA", hr);
    hr = CoCreateInstance(&g_TcClsidUIAutomation, NULL, CLSCTX_INPROC_SERVER,
        &g_TcIidUIAutomation, (void**)&g_TcUia);
    if (FAILED(hr) || !g_TcUia)
    {
        // Not an error for the agent: without UIA the toast is announced uncropped, which
        // is what it does today.
        LogWarning("CoCreateInstance(CUIAutomation) failed: 0x%x, toasts stay uncropped", hr);
        g_TcUia = NULL;
    }

    return g_TcUia;
}

// Zeroes *insets unless they describe a plausible card inside a rawWidth x rawHeight
// window. Zero insets are the module's fail-soft output, never a rejection.
static void TcValidateInsets(IN HWND window, IN LONG rawWidth, IN LONG rawHeight, IN OUT RECT* insets)
{
    if (insets->left < 0 || insets->top < 0 || insets->right < 0 || insets->bottom < 0)
    {
        ZeroMemory(insets, sizeof(*insets));
        return;
    }

    LONG width = rawWidth - insets->left - insets->right;
    LONG height = rawHeight - insets->top - insets->bottom;

    if (rawWidth <= 0 || rawHeight <= 0 || width <= 0 || height <= 0)
    {
        ZeroMemory(insets, sizeof(*insets));
        return;
    }

    // FLOOR GUARD. AddWindow drops an override-redirect window below g_MinWindow* on
    // purpose (Win11 Alt-nav keytip badges - main.c:1282-1295, "not appearing beats
    // appearing wrong"). That drop must stay exactly as it is and must stay UNREACHABLE
    // from here: a crop may never turn a visible toast into a dropped one.
    LONG floorWidth = (LONG)g_MinWindowWidth;
    LONG floorHeight = (LONG)g_MinWindowHeight;
    if (floorWidth < TOAST_CROP_FLOOR_WIDTH)
        floorWidth = TOAST_CROP_FLOOR_WIDTH;
    if (floorHeight < TOAST_CROP_FLOOR_HEIGHT)
        floorHeight = TOAST_CROP_FLOOR_HEIGHT;

    if (width < floorWidth || height < floorHeight)
    {
        LogWarning("0x%x: cropped size %dx%d below the %dx%d floor, leaving the toast uncropped",
            window, width, height, floorWidth, floorHeight);
        ZeroMemory(insets, sizeof(*insets));
        return;
    }

    if (width * 100 < rawWidth * TOAST_CROP_MIN_PERCENT ||
        height * 100 < rawHeight * TOAST_CROP_MIN_PERCENT)
    {
        // The continuation literal carries its own L, like the split formats in perf.c:
        // the log macro only prefixes the first one.
        LogWarning("0x%x: cropped size %dx%d is under %d%% of the %dx%d window - measured the "
            L"wrong element, leaving the toast uncropped",
            window, width, height, TOAST_CROP_MIN_PERCENT, rawWidth, rawHeight);
        ZeroMemory(insets, sizeof(*insets));
        return;
    }
}

ULONG ToastCropQuery(IN HWND window, IN RECT raw, OUT RECT* insets)
{
    IUIAutomationElement* windowElement = NULL;
    IUIAutomationElement* card = NULL;
    IUIAutomation* uia = NULL;
    ULONG status = ERROR_NOT_FOUND;
    HRESULT hr;
    RECT cardRect;

    if (!insets)
        return ERROR_INVALID_PARAMETER;

    ZeroMemory(insets, sizeof(*insets));

    TcInit();

    if (!TcEnsureCom())
        return ERROR_NOT_SUPPORTED;

    EnterCriticalSection(&g_TcLock);

    uia = TcGetAutomation();
    if (!uia)
    {
        status = ERROR_NOT_SUPPORTED;
        goto end;
    }

    hr = IUIAutomation_ElementFromHandle(uia, window, &windowElement);
    if (FAILED(hr) || !windowElement)
    {
        LogDebug("0x%x: ElementFromHandle failed: 0x%x", window, hr);
        status = ERROR_NOT_FOUND;
        goto end;
    }

    for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcCardClasses) && !card; i++)
    {
        IUIAutomationCondition* condition = NULL;
        VARIANT className;

        VariantInit(&className);
        className.vt = VT_BSTR;
        className.bstrVal = SysAllocString(g_TcCardClasses[i]);
        if (!className.bstrVal)
        {
            status = ERROR_NOT_ENOUGH_MEMORY;
            goto end;
        }

        hr = IUIAutomation_CreatePropertyCondition(uia, UIA_ClassNamePropertyId, className, &condition);
        VariantClear(&className);
        if (FAILED(hr) || !condition)
        {
            LogDebug("0x%x: CreatePropertyCondition failed: 0x%x", window, hr);
            continue;
        }

        // FindAll, not FindFirst: the count is the discriminator. A toast BANNER contains
        // exactly one card, while the Action Center / notification-centre flyout is a LIST of
        // them - and the size ceiling alone cannot separate the two, because the guest desktop
        // follows dom0's viewport, so a small dom0 window yields a guest screen on which the
        // flyout fits under the ceiling. Cropping the flyout to its first card would hide every
        // other notification, so more than one card means "not a banner": leave it uncropped.
        IUIAutomationElementArray* matches = NULL;
        hr = IUIAutomationElement_FindAll(windowElement, TreeScope_Descendants, condition, &matches);
        IUIAutomationCondition_Release(condition);
        if (FAILED(hr) || !matches)
        {
            LogDebug("0x%x: FindAll(%s) failed: 0x%x", window, g_TcCardClasses[i], hr);
            continue;
        }

        int found = 0;
        hr = IUIAutomationElementArray_get_Length(matches, &found);
        if (FAILED(hr))
            found = 0;

        if (found == 1)
        {
            hr = IUIAutomationElementArray_GetElement(matches, 0, &card);
            if (FAILED(hr))
                card = NULL;
        }
        else if (found > 1)
        {
            LogInfo("TOASTCROP 0x%x: %d '%s' cards - notification list, not a banner; left uncropped",
                window, found, g_TcCardClasses[i]);
            IUIAutomationElementArray_Release(matches);
            status = ERROR_NOT_FOUND;
            goto end;
        }

        IUIAutomationElementArray_Release(matches);
    }

    if (!card)
    {
        // Expected while the XAML tree is still being built; the caller retries a bounded
        // number of times and then leaves the toast uncropped.
        LogDebug("0x%x: no toast card element yet", window);
        status = ERROR_NOT_FOUND;
        goto end;
    }

    hr = IUIAutomationElement_get_CurrentBoundingRectangle(card, &cardRect);
    if (FAILED(hr))
    {
        LogDebug("0x%x: get_CurrentBoundingRectangle failed: 0x%x", window, hr);
        status = ERROR_NOT_FOUND;
        goto end;
    }

    // The card rect and `raw` were sampled at DIFFERENT times, and the toast slide-in is a
    // POSITION-ONLY animation - so the window may have moved between the two reads, and the
    // difference would then be latched as a permanent inset keyed by (hwnd, size), silently
    // cropping the wrong part of the card for the rest of its life. Re-read the window rect
    // and discard the measurement unless it is unchanged; a discarded attempt just becomes
    // another bounded retry, and the toast stays uncropped in the meantime.
    {
        // GetRealWindowRect, not GetWindowRect: `raw` is built by the caller from the tracked
        // X/Y/Width/Height, which come from GetRealWindowRect. Comparing against a different
        // rect source would mismatch on every window that has a DWM frame trim and turn the
        // race guard into a permanent "never crop".
        RECT recheck;
        if (ERROR_SUCCESS != GetRealWindowRect(window, &recheck))
        {
            LogDebug("0x%x: GetRealWindowRect recheck failed", window);
            status = ERROR_NOT_FOUND;
            goto end;
        }
        if (recheck.left != raw.left || recheck.top != raw.top ||
            recheck.right != raw.right || recheck.bottom != raw.bottom)
        {
            LogDebug("0x%x: window moved during measurement ((%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)) - retrying",
                window, raw.left, raw.top, raw.right, raw.bottom,
                recheck.left, recheck.top, recheck.right, recheck.bottom);
            status = ERROR_NOT_FOUND;
            goto end;
        }
    }

    // UIA reports physical screen pixels while `raw` is the DPI-adjusted rect from
    // GetRealWindowRect. The two coincide at 100% scaling, which is what QWT guests run:
    // the agent drives the guest resolution to dom0's viewport, it does not scale it. At
    // any other scale the insets are off by that factor - bounded, and still filtered by
    // the plausibility guard below, so the failure mode stays "uncropped", never "clipped".
    insets->left = cardRect.left - raw.left;
    insets->top = cardRect.top - raw.top;
    insets->right = raw.right - cardRect.right;
    insets->bottom = raw.bottom - cardRect.bottom;

    // A card edge outside the window edge is not a crop, it is nothing to do on that side.
    if (insets->left < 0)
        insets->left = 0;
    if (insets->top < 0)
        insets->top = 0;
    if (insets->right < 0)
        insets->right = 0;
    if (insets->bottom < 0)
        insets->bottom = 0;

    TcValidateInsets(window, raw.right - raw.left, raw.bottom - raw.top, insets);

    status = (insets->left || insets->top || insets->right || insets->bottom)
        ? ERROR_SUCCESS : ERROR_NOT_FOUND;

end:
    if (card)
        IUIAutomationElement_Release(card);
    if (windowElement)
        IUIAutomationElement_Release(windowElement);
    LeaveCriticalSection(&g_TcLock);
    return status;
}

// g_TcLock must be held.
static BOOL TcIsShellExperienceHost(IN DWORD processId)
{
    if (processId == 0)
        return FALSE;

    for (int i = 0; i < TOAST_PID_CACHE_SIZE; i++)
    {
        if (g_TcPidCache[i].ProcessId == processId)
        {
            g_TcPidCache[i].LastUse = ++g_TcClock;
            return g_TcPidCache[i].IsShellExperienceHost;
        }
    }

    // MAX_PATH is enough for the shell app's own path (under %WINDIR%\SystemApps); a
    // longer one simply fails the query and the window is left uncropped.
    WCHAR path[MAX_PATH];
    DWORD size = RTL_NUMBER_OF(path);
    BOOL match = FALSE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
    {
        if (QueryFullProcessImageName(process, 0, path, &size))
        {
            const WCHAR* image = wcsrchr(path, L'\\');
            image = image ? image + 1 : path;
            match = (0 == _wcsicmp(image, g_TcShellHostImage));
        }
        CloseHandle(process);
    }

    // PIDs are recycled, so a stale hit can misclassify an unrelated process. Harmless by
    // construction: a false positive only asks UIA for a toast card that is not there.
    int victim = 0;
    for (int i = 1; i < TOAST_PID_CACHE_SIZE; i++)
    {
        if (g_TcPidCache[i].LastUse < g_TcPidCache[victim].LastUse)
            victim = i;
    }

    g_TcPidCache[victim].ProcessId = processId;
    g_TcPidCache[victim].IsShellExperienceHost = match;
    g_TcPidCache[victim].LastUse = ++g_TcClock;
    return match;
}

BOOL IsShellToastWindow(IN const WINDOW_DATA* data)
{
    if (!data || !data->IsVisible)
        return FALSE;

    // GetWindowData() folds DWM cloaking into IsVisible, so an uncloaked check is implied
    // by the test above.
    if (0 != wcscmp(data->Class, g_TcToastWindowClass))
        return FALSE;

    if (!HasFlags(data->Style, WS_POPUP))
        return FALSE;

    if (!HasFlags(data->ExStyle, WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP))
        return FALSE;

    // Both would make this one of the windows ShouldAcceptWindow() REJECTS (shell drag
    // overlays, Office shadow strips). Requiring their absence is what keeps this
    // classifier provably disjoint from those rules.
    if (data->ExStyle & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW))
        return FALSE;

    // The banner belongs to no other window; owned CoreWindows are somebody's real UI.
    if (data->Owner != NULL)
        return FALSE;

    if (data->Width > TOAST_MAX_RAW_WIDTH || data->Height > TOAST_MAX_RAW_HEIGHT)
        return FALSE;

    TcInit();

    EnterCriticalSection(&g_TcLock);
    BOOL isToast = TcIsShellExperienceHost(data->ProcessId);
    LeaveCriticalSection(&g_TcLock);

    return isToast;
}

// g_TcLock must be held.
static TOAST_CROP_ENTRY* TcGetSlot(IN HWND window, IN DWORD rawWidth, IN DWORD rawHeight)
{
    int victim = 0;

    for (int i = 0; i < TOAST_CROP_CACHE_SIZE; i++)
    {
        if (g_TcCache[i].Window == window &&
            g_TcCache[i].RawWidth == rawWidth &&
            g_TcCache[i].RawHeight == rawHeight)
            return &g_TcCache[i];

        if (g_TcCache[i].LastUse < g_TcCache[victim].LastUse)
            victim = i;
    }

    ZeroMemory(&g_TcCache[victim], sizeof(g_TcCache[victim]));
    g_TcCache[victim].Window = window;
    g_TcCache[victim].RawWidth = rawWidth;
    g_TcCache[victim].RawHeight = rawHeight;
    return &g_TcCache[victim];
}

BOOL ToastCropLookup(IN const WINDOW_DATA* data, OUT RECT* insets)
{
    if (!insets)
        return FALSE;

    ZeroMemory(insets, sizeof(*insets));

    if (!data)
        return FALSE;

    TcInit();

    if (g_TcDisabled)
        return FALSE;

    if (!IsShellToastWindow(data))
        return FALSE;

    EnterCriticalSection(&g_TcLock);

    TOAST_CROP_ENTRY* slot = TcGetSlot(data->Handle, data->Width, data->Height);
    slot->LastUse = ++g_TcClock;

    if (!slot->Resolved && GetTickCount64() >= slot->RetryAt)
    {
        slot->Attempts++;
        slot->RetryAt = GetTickCount64() + TOAST_CROP_RETRY_MS;

        if (g_TcForced)
        {
            slot->Insets = g_TcForcedInsets;
            TcValidateInsets(data->Handle, (LONG)data->Width, (LONG)data->Height, &slot->Insets);
            slot->Resolved = TRUE;
        }
        else
        {
            RECT raw;
            raw.left = data->X;
            raw.top = data->Y;
            raw.right = data->X + (LONG)data->Width;
            raw.bottom = data->Y + (LONG)data->Height;

            ToastCropQuery(data->Handle, raw, &slot->Insets);

            // Resolved once something was measured, or once the retries are spent: after
            // that a lookup is a pure array scan, which is what keeps COM out of the
            // per-frame tracking pass.
            if (slot->Insets.left || slot->Insets.top || slot->Insets.right || slot->Insets.bottom)
                slot->Resolved = TRUE;
            else if (slot->Attempts >= TOAST_CROP_MAX_ATTEMPTS)
            {
                slot->Resolved = TRUE;
                LogDebug("0x%x: no card measured for %ux%u after %u attempts, staying uncropped",
                    data->Handle, data->Width, data->Height, slot->Attempts);
            }
        }

        if (slot->Resolved &&
            (slot->Insets.left || slot->Insets.top || slot->Insets.right || slot->Insets.bottom))
        {
            LogInfo("0x%x: toast card in %ux%u window, insets l=%d t=%d r=%d b=%d",
                data->Handle, data->Width, data->Height,
                slot->Insets.left, slot->Insets.top, slot->Insets.right, slot->Insets.bottom);
        }
    }

    *insets = slot->Insets;

    LeaveCriticalSection(&g_TcLock);

    return (insets->left || insets->top || insets->right || insets->bottom);
}

void ToastCropEvict(IN HWND window)
{
    TcInit();

    EnterCriticalSection(&g_TcLock);

    for (int i = 0; i < TOAST_CROP_CACHE_SIZE; i++)
    {
        if (g_TcCache[i].Window == window)
            ZeroMemory(&g_TcCache[i], sizeof(g_TcCache[i]));
    }

    LeaveCriticalSection(&g_TcLock);
}
