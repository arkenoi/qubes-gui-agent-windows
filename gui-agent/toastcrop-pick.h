/*
 * toastcrop-pick - the PURE card-selection rule behind toastcrop.c's UIA walk.
 *
 * toastcrop.c walks a live XAML tree over cross-process RPC and feeds every element's
 * bounding rect through TcCardAccumulate(); afterwards TcCardPick() turns the accumulator
 * into "the card": the UNION of every qualifying element, for every surface. Nothing here
 * touches COM, UIA, or any agent state, so the rule can be unit-tested offline against a
 * recorded tree (toastcrop_pick_test.c).
 *
 * WHY THE UNION, FOR TOASTS TOO (2026-11-24). Several toasts STACK as several cards in the one
 * banner window, which is normal and required (owner 2026-11-24, docs/ADR-toasts.md 13): dom0
 * must be sent every card. Measured on win11r 26300 (guest/toast-uia-tree.ps1 -Hwnd, raw view):
 * one card = one FlexibleToastView 364x157 whose every child ends inside it (rightmost 5097 <
 * card right 5104), so the union IS the card; three cards = three FlexibleToastViews 364x157
 * sharing x=4740, walked OLDEST FIRST, union 364x495. The rule this replaces, the single
 * LARGEST qualifier, tied the equal cards and kept the first-seen - the oldest - so the newest
 * toast was cut off (findings/issues.md TOASTSTACKNOTSHOWN). Over every toast-rule walk ever
 * logged on this rig (536 lines) the union equals the largest on every one-card banner.
 * The 2026-09-06 overcrop (a toast cut to ~214 wide) was the CONTROL view's union - a text
 * column, the card element absent; toasts walk the RAW view, where the card is present.
 * WinUI menu bodies have no single card element (the presenter spans the full height), so
 * the union of the rows is their drawn extent too (toastcrop.c, TcFindCardRect header).
 * The rule is GEOMETRIC. No class name is consulted (FlexibleToastView is cited above as
 * evidence only): class names are undocumented XAML internals that changed between builds
 * once already, which is why the class-keyed finder was retired.
 *
 * Qualification (unchanged since the geometric finder was written):
 * an element counts iff it is fully INSIDE the raw window rect and STRICTLY SMALLER than
 * it in both dimensions, with positive width and height. The shadow margin contains no
 * elements, so it never qualifies; a container spanning the window's full width or height
 * (the toast's ScrollViewer, a menu's presenter) is excluded by the strictness test, so
 * neither rule can ever wash out to the whole window.
 *
 * Portable on purpose: Windows types are taken from <windows.h> where it exists and shimmed
 * otherwise, so the offline suite compiles with gcc as well as MSVC.
 */

#pragma once

#ifdef _WIN32
#include <windows.h>
#else
// Minimal shims so the pure rule (and its test) build on a non-Windows host.
typedef long LONG;
typedef int  BOOL;
typedef struct tagRECT { LONG left; LONG top; LONG right; LONG bottom; } RECT;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#endif

typedef struct _TC_CARD_ACC
{
    RECT Union;          // running union of every qualifier; valid iff Count > 0
    RECT Largest;        // the qualifier with the greatest area; valid iff Count > 0 - logged next to the
                         // union (toastcrop.c's card-walk line) so a field log shows both, never picked
    LONG LargestArea;
    int  Count;          // qualifiers seen so far
} TC_CARD_ACC;

static __inline void TcCardAccInit(TC_CARD_ACC* acc)
{
    acc->Union.left = acc->Union.top = acc->Union.right = acc->Union.bottom = 0;
    acc->Largest = acc->Union;
    acc->LargestArea = 0;
    acc->Count = 0;
}

// TRUE iff `r` is a card candidate inside `raw`: fully inside, strictly smaller in BOTH
// dimensions, and non-empty.
static __inline BOOL TcCardQualifies(RECT raw, RECT r)
{
    LONG w = r.right - r.left;
    LONG h = r.bottom - r.top;
    BOOL inside = (r.left >= raw.left && r.top >= raw.top &&
                   r.right <= raw.right && r.bottom <= raw.bottom);
    BOOL strictlySmaller = (w < (raw.right - raw.left)) && (h < (raw.bottom - raw.top));
    return inside && strictlySmaller && w > 0 && h > 0;
}

// Feed one element rect. Returns TRUE iff it qualified (and was accumulated).
static __inline BOOL TcCardAccumulate(RECT raw, RECT r, TC_CARD_ACC* acc)
{
    if (!TcCardQualifies(raw, r))
        return FALSE;

    LONG area = (r.right - r.left) * (r.bottom - r.top);

    if (acc->Count == 0)
    {
        acc->Union = r;
        acc->Largest = r;
        acc->LargestArea = area;
    }
    else
    {
        if (r.left   < acc->Union.left)   acc->Union.left   = r.left;
        if (r.top    < acc->Union.top)    acc->Union.top    = r.top;
        if (r.right  > acc->Union.right)  acc->Union.right  = r.right;
        if (r.bottom > acc->Union.bottom) acc->Union.bottom = r.bottom;
        // Strictly greater: on a tie the first-seen (shallower, since the walk visits a
        // node before its children) element wins, which is the container, not a child
        // that happens to fill it.
        if (area > acc->LargestArea)
        {
            acc->Largest = r;
            acc->LargestArea = area;
        }
    }
    acc->Count++;
    return TRUE;
}

// The card: the union of every qualifier. FALSE (and *out untouched) when nothing qualified.
static __inline BOOL TcCardPick(const TC_CARD_ACC* acc, RECT* out)
{
    if (acc->Count == 0)
        return FALSE;
    *out = acc->Union;
    return TRUE;
}

// Convenience for a recorded tree: accumulate `count` rects (in walk order) and pick.
static __inline BOOL TcPickCard(RECT raw, const RECT* rects, int count, RECT* out)
{
    TC_CARD_ACC acc;
    TcCardAccInit(&acc);
    for (int i = 0; i < count; i++)
        TcCardAccumulate(raw, rects[i], &acc);
    return TcCardPick(&acc, out);
}

// ---- mid-slide guard --------------------------------------------------------------------
//
// A shell toast SLIDES IN FROM THE RIGHT, and the slide is a XAML translate of the card
// INSIDE a window that does not move (client-area animation). A UIA read taken mid-slide
// therefore sees a card whose right edge is already at its resting margin while its left
// edge is still far right of it - and the window-rect recheck in toastcrop.c cannot notice,
// because the WINDOW is stable. The insets that come out have a signature no resting card
// produces: a tiny inset on one side and a large one on the other. Measured 2026-09-06
// (win11 24H2, complex 3-image toast, LogLevel=3 TcApplyResult lines):
//   396x216 -> l=209 t=30 r=1 b=13      396x573 -> l=53 t=30 r=1 b=13
//   396x200 -> l=105 t=30 r=1 b=13
// and the same toast with client-area animation off, three fires in a row: l=16 r=16.
// The resting cards ever measured are near-symmetric horizontally: 16/16 (twice), 2/17
// (2026-08-11; 15 px apart). The rule: a horizontal left/right difference beyond
// TOAST_CROP_MAX_LR_ASYMMETRY is a mid-slide read and must be RETRIED, never latched -
// the slot's retry budget, then the last-good insets, then uncropped, are the fall-throughs.
// Vertical asymmetry is NOT tested: the slide is horizontal, and a resting card's top/bottom
// margins legitimately differ (30/13). Applied to TOASTS only - a WinUI menu does not slide
// and its raw-view padding container makes its own asymmetry story (toastcrop.c).
#define TOAST_CROP_MAX_LR_ASYMMETRY 32

// TRUE iff `insets` carry the mid-slide signature for a window `rawWidth` wide.
//
// This ONE predicate decides both things toastcrop.c does with a toast read:
//   FALSE -> ACCEPT IMMEDIATELY (the fast path: no wait, no second walk). With client-area
//            animation off this is every toast, and it must be as fast as ever, because the
//            toast is on screen uncropped - black margins - until the crop lands.
//   TRUE  -> REJECT and retry; on the worker thread a 50 ms settle re-walk first says whether
//            the card was moving (log detail only - the read is rejected either way).
// The offline suite asserts the routing on the measured shapes (toastcrop_pick_test.c).
static __inline BOOL TcInsetsMidSlide(LONG rawWidth, const RECT* insets)
{
    (void)rawWidth;
    LONG diff = insets->left - insets->right;
    if (diff < 0)
        diff = -diff;
    return diff > TOAST_CROP_MAX_LR_ASYMMETRY;
}
