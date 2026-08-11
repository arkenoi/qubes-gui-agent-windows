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

// XAML depth to search for the card. 4 covers the measured toast tree; Start's card sits
// near its root. Deeper costs cross-process RPC for no gain.
#define TOAST_CROP_MAX_DEPTH 6

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

// NOTE: the card is no longer identified by class name. FlexibleToastView/ToastView were the
// names on one build for one surface; TcFindCardRect finds the card geometrically instead, which
// is what makes the same code fix the 25H2 Start menu. The registry escape hatch in toastcrop.h
// remains for the case where even the geometric rule picks wrong on some future shell.

// Every shell surface that draws its own shadow INSIDE its window rect. Measured on the guests
// 2026-08-11: a ShellExperienceHost toast announces 396x332 for a 364x289 card, and on 25H2 a
// StartMenuExperienceHost Start menu announces 858x890 for an 832x874 card (13/3/13/13). On 24H2
// the same Start menu had NO margin - which is the whole reason this cannot be a table of
// constants: the same surface gains a shadow between Windows builds.
static const WCHAR* const g_TcShellHostImages[] = {
    L"ShellExperienceHost.exe",       // notification banners, Action Center
    L"StartMenuExperienceHost.exe",   // Start menu (25H2 shadow margin)
    L"SearchHost.exe",                // search flyout, same XAML shell
};
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

// Depth-limited search for the drawn card: the largest descendant fully inside `raw` and
// strictly smaller than it in both dimensions. Returns TRUE and the winning rect in *best.
//
// Depth is capped because this walks a live XAML tree over cross-process RPC: the toast tree is
// 4 levels deep, Start's is deeper but its card is near the root, and an unbounded walk on a
// pathological tree would cost exactly the per-frame stall the timeouts exist to prevent.
static BOOL TcFindCardRect(IN IUIAutomation* uia, IN IUIAutomationElement* element,
    IN RECT raw, IN int depth, OUT RECT* best, IN OUT LONG* bestArea)
{
    IUIAutomationTreeWalker* walker = NULL;
    IUIAutomationElement* child = NULL;
    BOOL found = FALSE;

    if (depth > TOAST_CROP_MAX_DEPTH)
        return FALSE;

    if (FAILED(IUIAutomation_get_RawViewWalker(uia, &walker)) || !walker)
        return FALSE;

    if (FAILED(IUIAutomationTreeWalker_GetFirstChildElement(walker, element, &child)) || !child)
    {
        IUIAutomationTreeWalker_Release(walker);
        return FALSE;
    }

    while (child)
    {
        IUIAutomationElement* next = NULL;
        RECT r;

        if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(child, &r)))
        {
            LONG w = r.right - r.left;
            LONG h = r.bottom - r.top;

            BOOL inside = (r.left >= raw.left && r.top >= raw.top &&
                           r.right <= raw.right && r.bottom <= raw.bottom);
            BOOL strictlySmaller = (w < (raw.right - raw.left)) && (h < (raw.bottom - raw.top));

            if (inside && strictlySmaller && w > 0 && h > 0 && w * h > *bestArea)
            {
                *bestArea = w * h;
                *best = r;
                found = TRUE;
            }
        }

        if (TcFindCardRect(uia, child, raw, depth + 1, best, bestArea))
            found = TRUE;

        if (FAILED(IUIAutomationTreeWalker_GetNextSiblingElement(walker, child, &next)))
            next = NULL;
        IUIAutomationElement_Release(child);
        child = next;
    }

    IUIAutomationTreeWalker_Release(walker);
    return found;
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

    // Find the card GEOMETRICALLY, not by class name. Class names are undocumented XAML
    // internals that Microsoft renames and restructures between builds - keying on
    // FlexibleToastView worked for a notification banner and would never have matched the 25H2
    // Start menu, whose card has the identical problem (announced 858x890 for an 832x874 card,
    // measured 2026-08-11; on 24H2 the same menu had no margin at all).
    //
    // The rule: among all descendants, take the LARGEST one that is fully inside the window and
    // strictly smaller in BOTH dimensions. That is the drawn card by construction - the shadow
    // is painted by the window itself and contains no elements, so nothing lives outside the
    // card, while containers that span the full window width (the toast's ScrollViewer, 396 wide
    // in a 396-wide window) are excluded by the strictness requirement. Picking the LARGEST also
    // means a list flyout yields its outer card, never one item inside it.
    {
        LONG bestArea = 0;
        if (!TcFindCardRect(uia, windowElement, raw, 0, &cardRect, &bestArea))
        {
            // Expected while the XAML tree is still being built; the caller retries a bounded
            // number of times and then leaves the window uncropped.
            LogDebug("0x%x: no card element yet", window);
            status = ERROR_NOT_FOUND;
            goto end;
        }
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
            for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcShellHostImages) && !match; i++)
                match = (0 == _wcsicmp(image, g_TcShellHostImages[i]));
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

    // No fixed size ceiling. It used to exclude the Action Center flyout, but (a) the 25H2 Start
    // menu is 858x890 and would have been excluded with it, which is exactly the surface this
    // has to fix, and (b) cropping the flyout to ITS card is correct too - the card-selection
    // rule below picks the outermost card, never one notification inside a list. What must stay
    // excluded is a surface so large it is not a popup at all; IsPopup's 90%-of-screen rule
    // already demotes those, and a demoted window never reaches this classifier as a popup.
    if (g_HostScreenWidth > 0 && g_HostScreenHeight > 0 &&
        (ULONGLONG)data->Width * (ULONGLONG)data->Height * 100ULL >
        (ULONGLONG)g_HostScreenWidth * (ULONGLONG)g_HostScreenHeight * 90ULL)
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
