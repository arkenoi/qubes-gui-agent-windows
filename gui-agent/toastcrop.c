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
// is not always populated the first time the agent sees the window; the retry budget must
// cover the slide-in animation (~0.5-1 s) end to end. With the async worker an attempt is
// counted when a MEASUREMENT COMPLETES (TcApplyResult), not when a lookup fires - the old
// inline design spent an attempt per lookup and only kept up with the animation because
// the blocking RPC itself stretched the pacing; measured 2026-08-12, the async version
// burned all three attempts in ~450 ms and every surface stayed uncropped. Six completed
// no-card measurements at >=250 ms spacing give ~1.5 s of coverage at worker speed.
#define TOAST_CROP_MAX_ATTEMPTS 6
#define TOAST_CROP_RETRY_MS     250
// Menus only, LEFT/RIGHT only: back the measured horizontal crop off by this many px per side so
// the dom0 window's outer edge lands on the menu's OUTER rounded frame rather than the item-content
// (control-view) edge - the user wants the menu's own left/right border kept, not shaved, and said
// the vertical crop is already right (2026-09-03), so top/bottom are left exactly as measured.
// Toasts are NOT backed off at all: their card edge IS the visible edge. Clamped at 0.
#define TOAST_CROP_MENU_BORDER 4
// Crop-before-show: how many measurement attempts a toast/menu's map is DEFERRED for while its
// crop resolves (CropPending). Shorter than MAX_ATTEMPTS on purpose - once this many attempts
// have completed without a card the surface maps UNCROPPED (and the remaining attempts, up to
// MAX_ATTEMPTS, still land the crop async), so a slow/unmeasurable surface is hidden briefly,
// never for the full budget. Each attempt pokes a re-examination (TcApplyResult) so the defer
// releases within ~attempts x measurement-time, not the 2 s resync interval.
#define TOAST_CROP_DEFER_ATTEMPTS 3

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
    SHELL_SURFACE_KIND Kind;   // which g_TcShellHostImages entry matched (None = no match)
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

// LRU clock, shared by the slot cache and the last-good table below.
static ULONGLONG g_TcClock = 0;

// Last-good insets per window (STICKY CROP). A shell surface's card margins are stable
// across moves and minor size changes, but the async UIA measurement is flaky (pre-XAML
// timing, cross-process RPC races) and a fresh measure can spuriously find no card - which
// used to drop an ALREADY-CROPPED, now WM-managed Start back to uncropped, announcing its
// full transparent-shadow-margined window whose margins composite the desktop behind it
// (user-reported 2026-08-12: "a peek into the underlying desktop"). Once we have seen a
// card for a window we keep those insets and fall back to them whenever a later measure
// yields nothing, so a managed shell surface is never announced uncropped. Keyed by hwnd,
// LRU like the slot cache. g_TcLock held.
static struct { HWND Window; RECT Insets; ULONGLONG LastUse; } g_TcLastGood[TOAST_PID_CACHE_SIZE];

static void TcRememberLastGood(IN HWND window, IN const RECT* insets)
{
    int victim = 0;
    for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcLastGood); i++)
    {
        if (g_TcLastGood[i].Window == window)  { victim = i; goto set; }
        if (g_TcLastGood[i].LastUse < g_TcLastGood[victim].LastUse) victim = i;
    }
set:
    g_TcLastGood[victim].Window = window;
    g_TcLastGood[victim].Insets = *insets;
    g_TcLastGood[victim].LastUse = ++g_TcClock;
}

// TRUE and fills *insets if we have a remembered card for this window. g_TcLock held.
static BOOL TcRecallLastGood(IN HWND window, OUT RECT* insets)
{
    for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcLastGood); i++)
        if (g_TcLastGood[i].Window == window &&
            (g_TcLastGood[i].Insets.left || g_TcLastGood[i].Insets.top ||
             g_TcLastGood[i].Insets.right || g_TcLastGood[i].Insets.bottom))
        {
            *insets = g_TcLastGood[i].Insets;
            g_TcLastGood[i].LastUse = ++g_TcClock;
            return TRUE;
        }
    return FALSE;
}

static IUIAutomation* g_TcUia = NULL;   // created on first use, kept for the process life
static TOAST_CROP_ENTRY g_TcCache[TOAST_CROP_CACHE_SIZE];
static TOAST_PID_ENTRY g_TcPidCache[TOAST_PID_CACHE_SIZE];
// ---- async measurement worker ----
//
// Every UIA call is a synchronous cross-process RPC into a shell host process, bounded
// only by the 500 ms connection timeout - and ToastCropLookup used to make it INLINE on
// the single WatchForEvents thread, which also dispatches MSG_MOTION/MSG_BUTTON/
// MSG_CONFIGURE. One busy shell process therefore stalled input and window tracking for
// hundreds of ms per attempt (adversarially verified 2026-08-12), felt as "all windows
// react weirdly to drag" whenever a toast/Start surface had an unresolved slot. The
// measurement now runs on this dedicated thread, which owns its own IUIAutomation
// instance and holds NO shared lock during the RPC; the tracking path only ever reads
// the cache. Until the worker resolves a slot the window is announced uncropped - the
// same fail-soft the module already had for a failed measurement.
#define TC_QUEUE_SIZE 8
typedef struct _TC_QUERY_REQ
{
    HWND  Window;
    RECT  Raw;
    DWORD RawWidth;
    DWORD RawHeight;
    BOOL  Valid;
} TC_QUERY_REQ;

static TC_QUERY_REQ g_TcQueue[TC_QUEUE_SIZE];   // guarded by g_TcLock
static HANDLE g_TcWorkQueued = NULL;             // auto-reset, signaled on enqueue
static HANDLE g_TcWorkerThread = NULL;
static BOOL   g_TcWorkerOk = FALSE;              // FALSE -> fall back to the inline query

static DWORD WINAPI TcWorkerThread(IN void* param);

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

    // The async measurement worker. If it cannot start, lookups fall back to the inline
    // synchronous query - the crop still works, only with the old stall risk, and the log
    // says so once.
    if (!g_TcDisabled && !g_TcForced)
    {
        g_TcWorkQueued = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (g_TcWorkQueued)
            g_TcWorkerThread = CreateThread(NULL, 0, TcWorkerThread, NULL, 0, NULL);
        g_TcWorkerOk = (g_TcWorkerThread != NULL);
        if (!g_TcWorkerOk)
            LogWarning("QGATOASTCROP worker unavailable (%lu) - measurements stay inline",
                GetLastError());
    }

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

// Creates a fresh automation object for the calling thread. No shared state touched, so
// no lock is needed; the worker owns its instance outright.
static IUIAutomation* TcCreateAutomation(void)
{
    IUIAutomation* uia = NULL;

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

        hr = IUIAutomation2_QueryInterface(uia2, &g_TcIidUIAutomation, (void**)&uia);
        IUIAutomation2_Release(uia2);

        if (SUCCEEDED(hr) && uia)
        {
            LogInfo("TOASTCROP UIA ready with %u ms timeouts (conn=0x%x, tx=0x%x)",
                (unsigned)TOAST_CROP_UIA_TIMEOUT_MS, hrConn, hrTx);
            return uia;
        }

        uia = NULL;
        LogWarning("TOASTCROP QueryInterface(IUIAutomation) failed: 0x%x, retrying untimed", hr);
    }

    // Fallback: no deadline available. Still bounded in practice by the retry cap, and a crop
    // that never happens is better than a toast that never appears.
    LogWarning("TOASTCROP CUIAutomation8 unavailable (0x%x) - falling back to untimed UIA", hr);
    hr = CoCreateInstance(&g_TcClsidUIAutomation, NULL, CLSCTX_INPROC_SERVER,
        &g_TcIidUIAutomation, (void**)&uia);
    if (FAILED(hr) || !uia)
    {
        // Not an error for the agent: without UIA the toast is announced uncropped, which
        // is what it does today.
        LogWarning("CoCreateInstance(CUIAutomation) failed: 0x%x, toasts stay uncropped", hr);
        uia = NULL;
    }

    return uia;
}

// g_TcLock must be held.
static IUIAutomation* TcGetAutomation(void)
{
    if (!g_TcUia)
        g_TcUia = TcCreateAutomation();
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

    // The ratio guard protects NORMAL-SIZE hosts (a toast banner whose UIA hit was some
    // inner element). It must NOT apply to a workarea-sized host: 25H2's Start card is
    // 858x874 inside a 5120x1384 window - 17% of the width - and rejecting that re-creates
    // the fullscreen white Start window (measured 2026-08-12). For oversize hosts (>=90%
    // of the guest screen in either dimension) the protection is the absolute floor above
    // plus a stricter one: a card that small inside a huge host would be a mismeasure.
    BOOL oversizeHost = (g_ScreenWidth > 0 && g_ScreenHeight > 0 &&
        ((ULONG)rawWidth * 100 >= g_ScreenWidth * 90UL ||
         (ULONG)rawHeight * 100 >= g_ScreenHeight * 90UL));

    if (oversizeHost)
    {
        if (width < 300 || height < 200)
        {
            LogWarning("0x%x: card %dx%d too small for a %dx%d host - mismeasure, leaving uncropped",
                window, width, height, rawWidth, rawHeight);
            ZeroMemory(insets, sizeof(*insets));
        }
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

// Depth-limited search for the drawn card: the UNION of every descendant fully inside `raw`
// and strictly smaller than it in both dimensions. Returns TRUE and the union rect in *best.
//
// Why the union and not the single largest such element (which is what this did until
// 2026-09-03): a Win11 WinUI context-menu BODY (class Microsoft.UI.Content.PopupWindowSiteBridge)
// has no single element that is the whole card - its MenuFlyoutPresenter spans the full window
// HEIGHT, so it is NOT strictly smaller in both dims and is excluded, and the largest element
// that IS strictly smaller is one menu ROW. That produced a card 90% wide but 10% tall - an
// absurd bottom inset the plausibility guard then rejected - so the body was left uncropped and
// its transparent bottom shadow rendered as a black band in dom0 (measured live 2026-09-03:
// single B=287 rejected, union B=22 accepted). The union of the menu-item rects is the true
// drawn extent. For a toast / Start / the menu's command-bar sub-window the whole card IS one
// element and every other qualifier nests inside it, so the union EQUALS that card - byte for
// byte the old result (verified: command bar single==union L10 T2 R10 B18). The shadow contains
// no elements so it never enters the union, and a full-window-spanning container is still
// excluded by the strict-smaller test, so it can never wash the union out to the whole window.
//
// Depth is capped because this walks a live XAML tree over cross-process RPC: the toast tree is
// 4 levels deep, Start's is deeper but its card is near the root, and an unbounded walk on a
// pathological tree would cost exactly the per-frame stall the timeouts exist to prevent.
static BOOL TcFindCardRect(IN IUIAutomationTreeWalker* walker, IN IUIAutomationElement* element,
    IN RECT raw, IN int depth, IN ULONGLONG deadline, OUT RECT* best, IN OUT LONG* bestArea)
{
    IUIAutomationElement* child = NULL;
    BOOL found = FALSE;

    // The walker is CALLER-OWNED (control view preferred, raw view as fallback - see
    // TcQueryCore). Passing it in, rather than creating a RawViewWalker here at every node,
    // is what lets the caller pick the view: the raw view includes a structural padding
    // container ~5 px outside the drawn card, which left a thin black strip along the crop.
    if (!walker || depth > TOAST_CROP_MAX_DEPTH)
        return FALSE;

    // Whole-walk deadline: the depth cap bounds the tree SHAPE but not the RPC time - a
    // busy shell host can take the full per-call timeout at every node. Past the deadline
    // the walk stops taking new RPCs and reports whatever it has; the attempt then counts
    // against the retry cap like any other miss.
    if (GetTickCount64() > deadline)
        return FALSE;

    if (FAILED(IUIAutomationTreeWalker_GetFirstChildElement(walker, element, &child)) || !child)
        return FALSE;

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

            if (inside && strictlySmaller && w > 0 && h > 0)
            {
                // Accumulate the UNION of every qualifying element rather than the single
                // largest one (see the function header for why). *bestArea is repurposed as
                // an "is the union non-empty yet" flag: 0 until the first qualifier seeds
                // *best, nonzero once it holds the running union.
                if (*bestArea == 0)
                {
                    *best = r;
                }
                else
                {
                    if (r.left   < best->left)   best->left   = r.left;
                    if (r.top    < best->top)    best->top    = r.top;
                    if (r.right  > best->right)  best->right  = r.right;
                    if (r.bottom > best->bottom) best->bottom = r.bottom;
                }
                *bestArea = 1;
                found = TRUE;
            }
        }

        if (TcFindCardRect(walker, child, raw, depth + 1, deadline, best, bestArea))
            found = TRUE;

        if (FAILED(IUIAutomationTreeWalker_GetNextSiblingElement(walker, child, &next)))
            next = NULL;
        IUIAutomationElement_Release(child);
        child = next;
    }

    return found;
}

// The measurement itself, against a CALLER-OWNED automation object. Holds no module lock:
// every call in here is a synchronous cross-process RPC that can take up to the UIA
// timeout, and holding g_TcLock across it would stall the tracking path's cache reads for
// exactly as long - the main-thread stall this module must never cause.
static ULONG TcQueryCore(IN IUIAutomation* uia, IN HWND window, IN RECT raw, OUT RECT* insets)
{
    IUIAutomationElement* windowElement = NULL;
    ULONG status = ERROR_NOT_FOUND;
    HRESULT hr;
    RECT cardRect = { 0, 0, 0, 0 };   // written by TcFindCardRect when it returns TRUE; the
                                      // control-then-raw two-call path defeats the compiler's
                                      // "found => written" flow analysis, so seed it explicitly.

    ZeroMemory(insets, sizeof(*insets));

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
    // The rule: among all descendants, take the UNION of those that are fully inside the window
    // and strictly smaller in BOTH dimensions. That is the drawn card by construction - the shadow
    // is painted by the window itself and contains no elements, so nothing lives outside the card,
    // while containers that span the full window width (the toast's ScrollViewer, 396 wide in a
    // 396-wide window) are excluded by the strictness requirement. The UNION (not the single
    // largest) is what makes a list/menu flyout whose presenter spans the full window height still
    // resolve to its item extent instead of collapsing onto one row - see TcFindCardRect's header.
    {
        LONG bestArea = 0;
        // 2 s covers a healthy walk (4-6 levels, tens of RPCs) many times over while
        // bounding a pathological one to a small multiple of the per-call timeout.
        ULONGLONG deadline = GetTickCount64() + 2000;
        IUIAutomationTreeWalker* walker = NULL;
        BOOL found = FALSE;

        // Prefer the CONTROL view. The RAW view includes a structural padding container that
        // sits ~5 px outside the drawn card (measured on a WinUI menu body 2026-09-03: raw
        // union L=11 R=11 B=19, control L=16 R=16 B=22 = the visible card), which is exactly
        // what left a thin black strip down the crop's left/right edge and along the bottom.
        // Fall back to the RAW view if the control view finds no card, so a surface whose card
        // is raw-only still crops (this is the pre-2026-09-03 behaviour): the required-kept
        // toasts can never regress below what they crop to today.
        if (SUCCEEDED(IUIAutomation_get_ControlViewWalker(uia, &walker)) && walker)
        {
            found = TcFindCardRect(walker, windowElement, raw, 0, deadline, &cardRect, &bestArea);
            IUIAutomationTreeWalker_Release(walker);
            walker = NULL;
        }
        if (!found)
        {
            bestArea = 0;
            deadline = GetTickCount64() + 2000;
            if (SUCCEEDED(IUIAutomation_get_RawViewWalker(uia, &walker)) && walker)
            {
                found = TcFindCardRect(walker, windowElement, raw, 0, deadline, &cardRect, &bestArea);
                IUIAutomationTreeWalker_Release(walker);
                walker = NULL;
            }
        }
        if (!found)
        {
            // Expected while the XAML tree is still being built; the caller retries a bounded
            // number of times and then leaves the window uncropped.
            LogDebug("0x%x: no card element yet", window);
            status = ERROR_NOT_FOUND;
            goto end;
        }
    }

    // NOTE: cardRect is filled by TcFindCardRect above. There is deliberately no
    // get_CurrentBoundingRectangle call here: `card` is never assigned any more, and calling
    // through it crashed the agent with an access violation at address 0 on every startup
    // (measured on win11-fresh 2026-08-11 - the guest crash-looped, one agent log every ~6 s,
    // and dom0 lost every window of the qube). It compiled cleanly, which is precisely why an
    // artefact must be run on a guest before it is called working.

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
    if (windowElement)
        IUIAutomationElement_Release(windowElement);
    return status;
}

// Public synchronous query - the fallback when the worker thread is unavailable, and the
// entry point external callers keep. Uses the shared automation object under g_TcLock.
ULONG ToastCropQuery(IN HWND window, IN RECT raw, OUT RECT* insets)
{
    IUIAutomation* uia = NULL;
    ULONG status;

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
        LeaveCriticalSection(&g_TcLock);
        return ERROR_NOT_SUPPORTED;
    }
    status = TcQueryCore(uia, window, raw, insets);
    LeaveCriticalSection(&g_TcLock);
    return status;
}

// g_TcLock must be held. Find-only counterpart of TcGetSlot: the worker must never
// resurrect a slot that ToastCropEvict cleared while its query was in flight.
static TOAST_CROP_ENTRY* TcFindSlotLocked(IN HWND window, IN DWORD rawWidth, IN DWORD rawHeight)
{
    for (int i = 0; i < TOAST_CROP_CACHE_SIZE; i++)
    {
        if (g_TcCache[i].Window == window &&
            g_TcCache[i].RawWidth == rawWidth &&
            g_TcCache[i].RawHeight == rawHeight)
            return &g_TcCache[i];
    }
    return NULL;
}

// Applies one finished measurement to its slot, with exactly the resolution semantics the
// old inline path had. g_TcLock must NOT be held by the caller.
static void TcApplyResult(IN const TC_QUERY_REQ* req, IN const RECT* insets)
{
    BOOL resolvedNonZero = FALSE;
    BOOL attemptDone = FALSE;   // an attempt was processed on THIS call (drives the defer poke)

    EnterCriticalSection(&g_TcLock);

    TOAST_CROP_ENTRY* slot = TcFindSlotLocked(req->Window, req->RawWidth, req->RawHeight);
    if (slot && !slot->Resolved)
    {
        slot->Insets = *insets;
        attemptDone = TRUE;

        // The attempt is counted HERE, on a completed measurement - counting at lookup
        // time let event-driven lookups burn the whole budget before the XAML tree
        // could populate (see the constants above).
        slot->Attempts++;

        if (insets->left || insets->top || insets->right || insets->bottom)
        {
            slot->Resolved = TRUE;
            resolvedNonZero = TRUE;
        }
        else if (slot->Attempts >= TOAST_CROP_MAX_ATTEMPTS)
        {
            slot->Resolved = TRUE;
            // Info, not Debug: "measured N times and found no card" is the one line that
            // distinguishes a dead worker from a genuinely card-less surface in a field
            // log at default level.
            LogInfo("0x%x: no card measured for %ux%u after %u attempts, staying uncropped",
                req->Window, req->RawWidth, req->RawHeight, slot->Attempts);
        }

        if (resolvedNonZero)
        {
            TcRememberLastGood(req->Window, insets);
            LogInfo("0x%x: toast card in %ux%u window, insets l=%d t=%d r=%d b=%d",
                req->Window, req->RawWidth, req->RawHeight,
                insets->left, insets->top, insets->right, insets->bottom);
        }
    }

    LeaveCriticalSection(&g_TcLock);

    // Re-examine the surface after EVERY completed attempt, not just the final verdict. Two
    // reasons: (1) it lands the crop within one pass of a successful measurement rather than one
    // animation frame; (2) while a toast/menu is DEFERRED for crop-before-show, the deferred
    // window is otherwise only re-driven by the 2 s resync - poking here re-runs the tracking
    // pass (and thus the next measurement attempt, paced by RetryAt) so the defer resolves in
    // ~attempts x measurement-time and releases (mapped cropped, or uncropped once the defer
    // budget is spent) instead of stalling for seconds.
    if (attemptDone)
        PokeWindowTracking();
}

static DWORD WINAPI TcWorkerThread(IN void* param)
{
    UNREFERENCED_PARAMETER(param);

    if (!TcEnsureCom())
        return 0;

    // Worker-owned instance: never shared, so no lock is ever held across an RPC.
    IUIAutomation* uia = TcCreateAutomation();
    if (!uia)
        return 0;

    while (TRUE)
    {
        WaitForSingleObject(g_TcWorkQueued, INFINITE);

        while (TRUE)
        {
            TC_QUERY_REQ req = { 0 };
            BOOL have = FALSE;

            EnterCriticalSection(&g_TcLock);
            for (int i = 0; i < TC_QUEUE_SIZE; i++)
            {
                if (g_TcQueue[i].Valid)
                {
                    req = g_TcQueue[i];
                    g_TcQueue[i].Valid = FALSE;
                    have = TRUE;
                    break;
                }
            }
            LeaveCriticalSection(&g_TcLock);

            if (!have)
                break;

            RECT insets;
            TcQueryCore(uia, req.Window, req.Raw, &insets); // status is in the insets
            TcApplyResult(&req, &insets);
        }
    }
    // not reached: the worker lives for the process lifetime, like the agent's other threads
}

// g_TcLock must be held. Queues one measurement for the worker; a full queue or an
// already-queued duplicate is dropped silently - the slot's RetryAt pacing re-requests it.
static BOOL TcEnqueueQueryLocked(IN HWND window, IN const RECT* raw, IN DWORD rawWidth, IN DWORD rawHeight)
{
    int freeIdx = -1;

    if (!g_TcWorkerOk)
        return FALSE;

    for (int i = 0; i < TC_QUEUE_SIZE; i++)
    {
        if (g_TcQueue[i].Valid)
        {
            if (g_TcQueue[i].Window == window &&
                g_TcQueue[i].RawWidth == rawWidth && g_TcQueue[i].RawHeight == rawHeight)
                return TRUE; // already pending
        }
        else if (freeIdx < 0)
        {
            freeIdx = i;
        }
    }

    if (freeIdx < 0)
        return FALSE;

    g_TcQueue[freeIdx].Window = window;
    g_TcQueue[freeIdx].Raw = *raw;
    g_TcQueue[freeIdx].RawWidth = rawWidth;
    g_TcQueue[freeIdx].RawHeight = rawHeight;
    g_TcQueue[freeIdx].Valid = TRUE;
    SetEvent(g_TcWorkQueued);
    return TRUE;
}

// g_TcLock must be held. The order of g_TcShellHostImages defines the kind mapping:
// [0] -> Toast, [1] -> Start, [2] -> Search.
static SHELL_SURFACE_KIND TcShellHostKind(IN DWORD processId)
{
    static const SHELL_SURFACE_KIND kindByImage[] =
        { ShellSurfaceToast, ShellSurfaceStart, ShellSurfaceSearch };

    if (processId == 0)
        return ShellSurfaceNone;

    for (int i = 0; i < TOAST_PID_CACHE_SIZE; i++)
    {
        if (g_TcPidCache[i].ProcessId == processId)
        {
            g_TcPidCache[i].LastUse = ++g_TcClock;
            return g_TcPidCache[i].Kind;
        }
    }

    // MAX_PATH is enough for the shell app's own path (under %WINDIR%\SystemApps); a
    // longer one simply fails the query and the window is left uncropped.
    WCHAR path[MAX_PATH];
    DWORD size = RTL_NUMBER_OF(path);
    SHELL_SURFACE_KIND match = ShellSurfaceNone;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
    {
        if (QueryFullProcessImageName(process, 0, path, &size))
        {
            const WCHAR* image = wcsrchr(path, L'\\');
            image = image ? image + 1 : path;
            for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcShellHostImages); i++)
                if (0 == _wcsicmp(image, g_TcShellHostImages[i]))
                {
                    match = kindByImage[i];
                    break;
                }
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
    g_TcPidCache[victim].Kind = match;
    g_TcPidCache[victim].LastUse = ++g_TcClock;
    return match;
}

BOOL IsShellToastWindow(IN const WINDOW_DATA* data)
{
    return ShellSurfaceKind(data) != ShellSurfaceNone;
}

// A Win11 WinUI windowed popup MENU/flyout (context menus, app menus). Like the shell surfaces
// above, these draw their own drop shadow INSIDE their window rect, so the o-r window is larger
// than the visible menu and the transparent margin shows as black when the menu MATERIALIZES as its
// own dom0 window (i.e. is not composited into an owner by synthesis). The same TcFindCardRect crop
// squares them to the visible card. Identified by CLASS (process-agnostic - any app's WinUI menu):
// classic GDI menus (#32768) are already tight and deliberately excluded (nothing to crop).
BOOL IsMenuPopupWindow(IN const WINDOW_DATA* data)
{
    // Keyed on CLASS + VISIBILITY, deliberately NOT on data->IsOverrideRedirect. This runs from
    // the crop gate in GetWindowData(), which ZeroMemory()s the entry at entry and only assigns
    // IsOverrideRedirect much further down (post-crop, by design - the cropped size has to clear
    // the popup guard). Reading IsOverrideRedirect here therefore always saw FALSE, so the menu
    // crop NEVER ran (found 2026-09-03: no crop measurement was ever logged for a materialized
    // WinUI menu body, black shadow band left uncropped). These two classes are XAML-island
    // windowed-popup hosts - always override-redirect transient popups (context menus / flyouts),
    // never normal top-level windows - so the class alone is a sound and sufficient signal. Both
    // fields ARE populated before the gate (Class at GetClassName, IsVisible just above it).
    // Classic GDI menus (#32768) are already tight and deliberately excluded (nothing to crop).
    if (!data || !data->IsVisible || !data->Class)
        return FALSE;
    return wcsstr(data->Class, L"PopupWindowSiteBridge") != NULL ||   // Microsoft.UI.Content.PopupWindowSiteBridge
           wcsstr(data->Class, L"Xaml_WindowedPopup")   != NULL;      // older WinUI windowed popup class
}

SHELL_SURFACE_KIND ShellSurfaceKind(IN const WINDOW_DATA* data)
{
    if (!data || !data->IsVisible)
        return ShellSurfaceNone;

    // GetWindowData() folds DWM cloaking into IsVisible, so an uncloaked check is implied
    // by the test above.
    if (0 != wcscmp(data->Class, g_TcToastWindowClass))
        return ShellSurfaceNone;

    if (!HasFlags(data->Style, WS_POPUP))
        return ShellSurfaceNone;

    if (!HasFlags(data->ExStyle, WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP))
        return ShellSurfaceNone;

    // Both would make this one of the windows ShouldAcceptWindow() REJECTS (shell drag
    // overlays, Office shadow strips). Requiring their absence is what keeps this
    // classifier provably disjoint from those rules.
    if (data->ExStyle & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW))
        return ShellSurfaceNone;

    // The banner belongs to no other window; owned CoreWindows are somebody's real UI.
    if (data->Owner != NULL)
        return ShellSurfaceNone;

    // No size ceiling AT ALL any more. The old 90%-of-screen exclusion assumed an oversize
    // CoreWindow is "not a popup" and safe to leave alone - measured wrong on 25H2 at
    // 5120x1440: StartMenuExperienceHost hosts the Start CARD (858x874, found by the
    // geometric search) inside a WORKAREA-SIZED 5120x1384 window, and leaving that host
    // uncropped mapped an opaque near-fullscreen white window over the whole dom0 screen
    // (fullshot 2026-08-12). The card search plus TcValidateInsets' guards (absolute floor,
    // and the ratio guard for normal-size hosts) are the protection; a fullscreen surface
    // with no card inside simply measures nothing and stays uncropped, as before.

    TcInit();

    EnterCriticalSection(&g_TcLock);
    SHELL_SURFACE_KIND kind = TcShellHostKind(data->ProcessId);
    LeaveCriticalSection(&g_TcLock);

    return kind;
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

    // Shell surfaces (toasts/Start/Search) AND Win11 WinUI menus: all draw a shadow inside the
    // window rect, all measured + cropped by the same generic TcFindCardRect card finder.
    if (!IsShellToastWindow(data) && !IsMenuPopupWindow(data))
        return FALSE;

    EnterCriticalSection(&g_TcLock);

    TOAST_CROP_ENTRY* slot = TcGetSlot(data->Handle, data->Width, data->Height);
    slot->LastUse = ++g_TcClock;

    if (!slot->Resolved && GetTickCount64() >= slot->RetryAt)
    {
        // RetryAt paces REQUESTS; Attempts counts completed MEASUREMENTS (worker side,
        // TcApplyResult) so the budget spans real answers, not event bursts.
        slot->RetryAt = GetTickCount64() + TOAST_CROP_RETRY_MS;

        if (g_TcForced)
        {
            slot->Insets = g_TcForcedInsets;
            TcValidateInsets(data->Handle, (LONG)data->Width, (LONG)data->Height, &slot->Insets);
            slot->Resolved = TRUE;

            if (slot->Insets.left || slot->Insets.top || slot->Insets.right || slot->Insets.bottom)
            {
                TcRememberLastGood(data->Handle, &slot->Insets);
                LogInfo("0x%x: toast card in %ux%u window, insets l=%d t=%d r=%d b=%d",
                    data->Handle, data->Width, data->Height,
                    slot->Insets.left, slot->Insets.top, slot->Insets.right, slot->Insets.bottom);
            }
        }
        else
        {
            RECT raw;
            raw.left = data->X;
            raw.top = data->Y;
            raw.right = data->X + (LONG)data->Width;
            raw.bottom = data->Y + (LONG)data->Height;

            // The measurement is a cross-process UIA RPC and this lookup sits on the
            // window-tracking pass of the thread that also dispatches input, so it must
            // never wait for one: hand the request to the worker and answer from the
            // cache. Until the worker resolves the slot the window is announced
            // UNCROPPED (the module's normal fail-soft), and TcApplyResult pokes the
            // tracking pass so the crop lands within one pass of the answer. Attempt
            // pacing (Attempts/RetryAt above) is unchanged: a lost or unanswered request
            // is simply re-queued at the next retry tick.
            if (!TcEnqueueQueryLocked(data->Handle, &raw, data->Width, data->Height))
            {
                // Worker unavailable (thread failed to start, or the queue is full with
                // other windows). Fall back to the old inline query rather than never
                // measuring; the stall risk this reintroduces is bounded by the same
                // attempt cap that always bounded it, and the common path never gets
                // here.
                LeaveCriticalSection(&g_TcLock);

                RECT measured;
                ToastCropQuery(data->Handle, raw, &measured);

                EnterCriticalSection(&g_TcLock);
                slot = TcFindSlotLocked(data->Handle, data->Width, data->Height);
                if (slot && !slot->Resolved)
                {
                    slot->Insets = measured;
                    slot->Attempts++; // inline fallback measures right here, so count here
                    if (measured.left || measured.top || measured.right || measured.bottom)
                    {
                        slot->Resolved = TRUE;
                        TcRememberLastGood(data->Handle, &slot->Insets);
                        LogInfo("0x%x: toast card in %ux%u window, insets l=%d t=%d r=%d b=%d",
                            data->Handle, data->Width, data->Height,
                            measured.left, measured.top, measured.right, measured.bottom);
                    }
                    else if (slot->Attempts >= TOAST_CROP_MAX_ATTEMPTS)
                    {
                        slot->Resolved = TRUE;
                        LogDebug("0x%x: no card measured for %ux%u after %u attempts, staying uncropped",
                            data->Handle, data->Width, data->Height, slot->Attempts);
                    }
                }
                if (!slot)
                {
                    // Evicted while unlocked: report uncropped this pass.
                    LeaveCriticalSection(&g_TcLock);
                    return FALSE;
                }
            }
        }
    }

    *insets = slot->Insets;

    // STICKY CROP: a fresh measure found no card, but we have cropped this window before -
    // reuse the remembered insets rather than announcing a WM-managed shell surface
    // uncropped (which shows the desktop through its transparent margins). Re-validate
    // against the CURRENT size so a genuinely-resized surface cannot get an oversize crop.
    if (!(insets->left || insets->top || insets->right || insets->bottom))
    {
        RECT lastGood;
        if (TcRecallLastGood(data->Handle, &lastGood))
        {
            TcValidateInsets(data->Handle, (LONG)data->Width, (LONG)data->Height, &lastGood);
            if (lastGood.left || lastGood.top || lastGood.right || lastGood.bottom)
            {
                *insets = lastGood;
                LogDebug("0x%x: fresh measure found no card, using last-good insets l=%d t=%d r=%d b=%d",
                    data->Handle, lastGood.left, lastGood.top, lastGood.right, lastGood.bottom);
            }
        }
    }

    LeaveCriticalSection(&g_TcLock);

    // Menus: align the crop to the menu's OUTER frame, not the item-content edge. Back the
    // LEFT/RIGHT insets off by TOAST_CROP_MENU_BORDER (vertical is already right per the user, so
    // top/bottom are untouched). The reduced insets flow on to entry->CropLeft/Right and thus to
    // the broker's ReqCropX, so the sub-rect the broker lifts widens to match - no shift. Toasts
    // are left tight.
    if (IsMenuPopupWindow(data) && (insets->left || insets->right))
    {
        insets->left  = (insets->left  > TOAST_CROP_MENU_BORDER) ? insets->left  - TOAST_CROP_MENU_BORDER : 0;
        insets->right = (insets->right > TOAST_CROP_MENU_BORDER) ? insets->right - TOAST_CROP_MENU_BORDER : 0;
    }

    return (insets->left || insets->top || insets->right || insets->bottom);
}

BOOL ShellSurfaceCardless(IN const WINDOW_DATA* data)
{
    if (!data)
        return FALSE;

    TcInit();
    if (g_TcDisabled || g_TcForced)
        return FALSE;                       // measurement disabled/forced: never gate on it

    // START ONLY. StartMenuExperienceHost keeps a top-level surface alive while Start is
    // CLOSED, and that phantom has no card - announcing it puts a window with no menu in
    // it on the dom0 screen, at whatever rect the surface reports (measured 1201x919, and
    // x=6050 on a 5120-wide screen), which then vanishes: the user's "window at random
    // position, then dead". Gating Start on "we know where its card is" removes the
    // phantom AND the uncropped flash before the crop resolves.
    //
    // TOASTS ARE DELIBERATELY EXEMPT: CLAUDE.md makes notifications REQUIRED-kept, and the
    // module's contract is that any measurement failure yields zero insets and the toast is
    // announced UNCROPPED rather than lost. A toast we cannot measure must still be seen.
    if (ShellSurfaceKind(data) != ShellSurfaceStart)
        return FALSE;

    // PARKED OFF-SCREEN = CLOSED. Windows keeps the Start menu's window alive and parks it
    // OUTSIDE the desktop while it is closed - measured on this guest: announced at
    // x=6063 on a 5120-wide screen (the user's "window at random position"; its content is
    // whatever the slice reads there, i.e. nothing). A surface with no pixel on the desktop
    // is not presenting a menu, whatever its card cache says.
    {
        RECT screen = { 0, 0, (LONG)g_ScreenWidth, (LONG)g_ScreenHeight };
        RECT win = { data->X, data->Y,
                     data->X + (LONG)data->Width, data->Y + (LONG)data->Height };
        RECT hit;
        if (!IntersectRect(&hit, &screen, &win))
            return TRUE;    // entirely off-screen: parked, not open
    }

    BOOL noCard = TRUE;
    EnterCriticalSection(&g_TcLock);
    {
        RECT lastGood;
        TOAST_CROP_ENTRY* slot = TcFindSlotLocked(data->Handle, data->Width, data->Height);
        if (slot && (slot->Insets.left || slot->Insets.top ||
                     slot->Insets.right || slot->Insets.bottom))
            noCard = FALSE;                 // measured, card known
        else if (TcRecallLastGood(data->Handle, &lastGood))
            noCard = FALSE;                 // sticky card from an earlier open
    }
    LeaveCriticalSection(&g_TcLock);
    return noCard;
}

BOOL CropPending(IN const WINDOW_DATA* data)
{
    if (!data)
        return FALSE;

    TcInit();
    if (g_TcDisabled || g_TcForced)
        return FALSE;                       // measurement not the async path: never defer

    // CROP BEFORE SHOW. A TOAST or WinUI MENU popup whose shadow-crop has not resolved yet is
    // NOT mapped, so it appears already cropped instead of flashing its uncropped transparent
    // (black/wallpaper) shadow margin for a frame and then re-announcing at the cropped size.
    // BOUNDED, and DIFFERENT from ShellSurfaceCardless (Start): the moment the measurement
    // RESOLVES - a card found, OR given up after MAX_ATTEMPTS - this returns FALSE and the
    // surface maps (cropped, or uncropped as the required-kept fallback so a toast/menu is
    // NEVER lost). Start/Search keep the opposite policy (suppress the card-less phantom).
    BOOL isToast = (ShellSurfaceKind(data) == ShellSurfaceToast);
    if (!isToast && !IsMenuPopupWindow(data))
        return FALSE;

    BOOL pending = TRUE;
    EnterCriticalSection(&g_TcLock);
    {
        RECT lastGood;
        TOAST_CROP_ENTRY* slot = TcFindSlotLocked(data->Handle, data->Width, data->Height);
        if (slot && (slot->Resolved || slot->Attempts >= TOAST_CROP_DEFER_ATTEMPTS))
            pending = FALSE;                // resolved, or defer budget spent: map it now
        else if (TcRecallLastGood(data->Handle, &lastGood))
            pending = FALSE;                // sticky card from an earlier open of this hwnd
    }
    LeaveCriticalSection(&g_TcLock);
    return pending;
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
    for (int i = 0; i < (int)RTL_NUMBER_OF(g_TcLastGood); i++)
    {
        if (g_TcLastGood[i].Window == window)
            ZeroMemory(&g_TcLastGood[i], sizeof(g_TcLastGood[i]));
    }

    // Also drop any not-yet-run measurement request; a query already in flight is
    // harmless (TcApplyResult only writes into a still-matching slot).
    for (int i = 0; i < TC_QUEUE_SIZE; i++)
    {
        if (g_TcQueue[i].Valid && g_TcQueue[i].Window == window)
            g_TcQueue[i].Valid = FALSE;
    }

    LeaveCriticalSection(&g_TcLock);
}
