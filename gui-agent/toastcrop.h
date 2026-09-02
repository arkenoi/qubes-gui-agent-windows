/*
 * toastcrop - measure the visible card inside a Windows shell toast window.
 *
 * Windows Action Center toasts (ShellExperienceHost) draw their drop shadow INSIDE the
 * window: probed on the guest, GetWindowRect equals DWMWA_EXTENDED_FRAME_BOUNDS with a
 * delta of ZERO, so no Win32 or DWM API can tell the agent where the card actually is.
 * The margin around the card is transparent XAML, and the window is slice-fed from the
 * composited desktop, so dom0 announces - and borders - a rectangle whose margin shows
 * the wallpaper behind the toast.
 *
 * Measured live 2026-08-11: a 396x332 "Turn On Windows Backup" toast carried a 377x287
 * card, i.e. ~2 left / 31 top / 17 right / 14 bottom of dead margin. An earlier 396x133
 * collapsed toast measured 16/30/16/13. The insets are PER TOAST - they must be measured,
 * never assumed - and UI Automation is the only API that sees the card (the XAML element
 * FlexibleToastView / ToastView). The same run measured the Start menu at exactly 858x890
 * announced with the card filling it, which is why the crop is classifier-gated and never
 * applied to popups in general.
 *
 * Toasts are REQUIRED-KEPT (CLAUDE.md 2A-chrome 3c). This module only ever SHRINKS the
 * geometry of a window the agent has already accepted; it never feeds a rejection. Every
 * failure path - not a toast, COM/UIA unavailable, element not found, implausible
 * measurement, cropped size below the AddWindow floor - yields ZERO insets, which is
 * exactly today's uncropped-but-visible behaviour.
 *
 * Cost: this runs in the per-frame tracking pass, so UIA is touched only on a cache miss,
 * keyed by (hwnd, raw size) - one call when the card is found, at most three spaced apart
 * when it is not, per size the toast takes. Once an entry is resolved a lookup is an array
 * scan under a private lock: no allocation, no COM.
 *
 * Registry (module key HKLM\Software\Invisible Things Lab\Qubes Tools\gui-agent, read
 * exactly like perf.c reads its own):
 *   ToastCropDisable  DWORD 0/1 - bypass the module entirely (escape hatch: undocumented
 *                     XAML class names are field-fragile).
 *   ToastCropL/T/R/B  DWORD px  - force these insets instead of measuring. Still subject
 *                     to the plausibility and floor guards.
 */

#pragma once
#include <windows.h>
#include "main.h"

// TRUE for a shell toast banner: the narrow signature from the guest probe (class
// Windows.UI.Core.CoreWindow, WS_POPUP, TOPMOST|NOREDIRECTIONBITMAP, NOT TRANSPARENT,
// NOT TOOLWINDOW, unowned, visible/uncloaked, owned by ShellExperienceHost.exe, under
// the size ceiling that keeps the Action Center flyout out). Deliberately disjoint from
// the shell-overlay and Office-shadow REJECT rules in ShouldAcceptWindow(), which all
// require WS_EX_TRANSPARENT and/or WS_EX_TOOLWINDOW.
BOOL IsShellToastWindow(
    IN const WINDOW_DATA* data
    );

// TRUE for a Win11 WinUI windowed popup menu/flyout (class *PopupWindowSiteBridge /
// *Xaml_WindowedPopup*, override-redirect). Same "shadow inside the window" shape as toasts, so
// the same UIA card crop squares off the transparent (black-when-materialized) margin. Classic
// GDI menus (#32768) are already tight and excluded.
BOOL IsMenuPopupWindow(
    IN const WINDOW_DATA* data
    );

// Which shell-host process a classified surface belongs to. The per-surface ShellManaged
// policy needs the distinction: the GWeck goal state makes START WM-managed (movable,
// size-locked) while toasts stay override-redirect corner popups (user spec 2026-08-12).
typedef enum _SHELL_SURFACE_KIND
{
    ShellSurfaceNone = 0,
    ShellSurfaceToast,    // ShellExperienceHost.exe (banners, Action Center)
    ShellSurfaceStart,    // StartMenuExperienceHost.exe
    ShellSurfaceSearch,   // SearchHost.exe
} SHELL_SURFACE_KIND;

// ShellSurfaceNone unless the window passes the full toast-window classifier; then the
// kind of the owning shell host. IsShellToastWindow(data) == (kind != None).
SHELL_SURFACE_KIND ShellSurfaceKind(
    IN const WINDOW_DATA* data
    );

// TRUE when this window is the START surface and we do NOT know where its card is
// (never measured, or the measurement finished finding nothing, and no sticky last-good).
// StartMenuExperienceHost keeps a top-level surface alive while Start is CLOSED; announcing
// that phantom puts a menu-less window on the dom0 screen at whatever rect it reports
// (measured 1201x919, and x=6050 on a 5120-wide screen), which then vanishes - the
// user-reported "window at random position, then dead". Callers refuse to map it.
// TOASTS ARE EXEMPT BY DESIGN: notifications are REQUIRED-kept, so a toast whose card
// cannot be measured is still announced, just uncropped.
BOOL ShellSurfaceCardless(
    IN const WINDOW_DATA* data
    );

// Insets to subtract from this window's raw rect, from the (hwnd, raw size) cache;
// measures via UIA on a miss. Always writes *insets (all zero = do not crop) and returns
// TRUE only when a nonzero, validated crop applies. Safe to call for any window.
BOOL ToastCropLookup(
    IN const WINDOW_DATA* data,
    OUT RECT* insets
    );

// One UIA measurement of `window`'s card against its raw rect, no caching. Always writes
// *insets; zeroes them on any failure. Returns ERROR_SUCCESS when a nonzero crop was
// measured, otherwise a status that the caller is expected to IGNORE (the zeroed insets
// are the whole contract).
ULONG ToastCropQuery(
    IN HWND window,
    IN RECT raw,
    OUT RECT* insets
    );

// Drop this window's cache slot. Windows recycles HWND values, so a stale hit would apply
// one window's insets to an unrelated popup; called from RemoveWindow.
void ToastCropEvict(
    IN HWND window
    );
