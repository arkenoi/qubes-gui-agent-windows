/*
 * toastcrop_pick_test - offline suite for the pure card-selection rule (toastcrop-pick.h).
 *
 * Self-contained: no rig, no UIA, no COM - it runs on the CI Windows runner right after
 * msbuild, and (being plain C with shimmed Windows types) on any host with a C compiler:
 *   gcc -Wall -Wextra -I. toastcrop_pick_test.c -o toastcrop_pick_test && ./toastcrop_pick_test
 * Exit 0 = every case matched; nonzero = at least one mismatch.
 *
 * WHAT IT PROVES. The recorded trees below are the two surfaces the per-surface rule was
 * written for, with the rects MEASURED live (UIA both-views dump, animation disabled):
 *   * TOAST (win11 24H2, 2026-09-06): CoreWindow 396x200; the FlexibleToastView card 364x157
 *     at insets 16/30/16/13 (the toastcrop.h baseline); the control-view content - header
 *     text, body text, two action buttons, header buttons - unions to a box well INSIDE the
 *     card. The toast rule (largest qualifier) must yield the card; the menu rule (union of
 *     the control-view qualifiers) yields the narrower content box and cuts the buttons.
 *   * MENU body (2026-09-03): the MenuFlyoutPresenter spans the full window height, so it
 *     is NOT strictly smaller and does not qualify; the largest qualifier is one row. The
 *     menu rule (union of the rows) must yield the item extent; the toast rule would
 *     collapse onto a single row - which is exactly why the union rule exists for menus.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts as evidence only once it has been seen
 * to FAIL). Rebuilding with -DTOASTCROP_PICK_DEFECT_UNION (vcxproj property
 * ToastCropPickDefect, e.g.
 *   msbuild ... /t:Rebuild /p:ToastCropPickDefect=TOASTCROP_PICK_DEFECT_UNION
 * ) makes the toast path use the union rule - the 2026-09-03 finder - and this suite MUST
 * then exit nonzero (the toast case then reports the ~214-wide content box instead of the
 * 364-wide card). CI inverts the exit code under the defect define to enforce it. Note this
 * is the SAME mechanism as the agent's registry knob ToastCropToastUnion=1, which forces the
 * rule at runtime on the rig; the define proves the suite, the knob proves the live crop.
 */

#include "toastcrop-pick.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned g_run = 0, g_fail = 0;

static TC_PICK_MODE MenuRule(void) { return TcPickUnion; }

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static RECT R(LONG l, LONG t, LONG r, LONG b) { RECT x; x.left = l; x.top = t; x.right = r; x.bottom = b; return x; }

static void Check(const char* name, BOOL found, RECT got, BOOL expFound, RECT exp, LONG tol)
{
    g_run++;
    BOOL ok;
    if (!expFound)
        ok = !found;
    else
        ok = found &&
             labs(got.left - exp.left) <= tol && labs(got.top - exp.top) <= tol &&
             labs(got.right - exp.right) <= tol && labs(got.bottom - exp.bottom) <= tol;
    if (!ok) g_fail++;
    printf("%s %-52s expected %s(%ld,%ld) %ldx%ld  got %s(%ld,%ld) %ldx%ld\n",
        ok ? "ok  " : "FAIL", name,
        expFound ? "" : "none ", (long)exp.left, (long)exp.top, (long)(exp.right - exp.left), (long)(exp.bottom - exp.top),
        found ? "" : "none ", (long)got.left, (long)got.top, (long)(got.right - got.left), (long)(got.bottom - got.top));
}

// ---- recorded trees (walk order: a node before its children) ---------------------------

// TOAST: window at (4700,1180), 396x200. Card = FlexibleToastView 364x157 at +16/+30
// (insets 16/30/16/13). Measured 2026-09-06: the shipped (union) finder cropped this toast
// to ~214 wide x 157 tall, cutting both action buttons (OK at x 4756-4918, Later at
// 4926-5088 in that dump) - so the accumulated union was the header/body text column, and
// the buttons did NOT enter it (their rects came back empty, or the walk never reached
// them; the dump does not say which, and the rule below must be right either way).
static const RECT g_ToastRaw = { 4700, 1180, 5096, 1380 };
// Raw view: the card is the container, so it precedes its content in walk order.
static const RECT g_ToastRawTree[] = {
    { 4700, 1180, 5096, 1380 },   // ScrollViewer spanning the whole window: NOT strictly smaller
    { 4716, 1210, 5080, 1367 },   // FlexibleToastView - THE CARD, 364x157
    { 4716, 1210, 5080, 1250 },   // header strip (full card width, shorter): qualifies, inside card
    { 4750, 1216, 4782, 1244 },   // app icon
    { 4790, 1216, 4964, 1244 },   // header text
    { 5040, 1216, 5072, 1244 },   // header gear/close buttons
    { 4750, 1256, 4964, 1300 },   // body text
    { 4756, 1312, 4918, 1352 },   // OK button
    { 4926, 1312, 5088, 1352 },   // Later button (right edge 5088 > card right 5080: overhangs the
                                  // card into the margin, still inside the window -> qualifies, but
                                  // the union over the raw tree is still governed by the card below)
    { 4716, 1200, 4716, 1200 },   // empty rect (zero size): must be ignored
};
// Control view as the 2026-09-03 finder accumulated it for this toast: leaves only (the
// structural containers are AccessibilityView=Raw), the buttons NOT accumulated (see above),
// so the union is the 214-wide text column inside the card. (The dump shows the card class
// in the control view as well; whether the agent's walk reaches it is not what this suite
// decides - it fixes the RULE: with the card accumulated, largest picks it; without it, the
// union can only ever be a content box. Both are covered below.)
static const RECT g_ToastControlTree[] = {
    { 4750, 1216, 4782, 1244 },   // app icon
    { 4790, 1216, 4964, 1244 },   // header text
    { 4750, 1256, 4964, 1300 },   // body text
    { 0, 0, 0, 0 },               // OK button: empty (not accumulated in the measured walk)
    { 0, 0, 0, 0 },               // Later button: empty
};
static const RECT g_ToastCard = { 4716, 1210, 5080, 1367 };   // 364x157, insets 16/30/16/13
static const RECT g_ToastContentBox = { 4750, 1216, 4964, 1300 };   // 214 wide: the measured overcrop

// THE TOAST PATH, mirroring TcQueryCore's per-surface branch: view AND rule together.
//   normal  -> RAW view tree   + LARGEST qualifier   (the fix)
//   defect  -> CONTROL view tree + UNION             (the 2026-09-03 finder; what
//              ToastCropToastUnion=1 forces at runtime)
// `reversed` feeds the tree in reverse walk order, to show the pick is by area, not order.
static BOOL ToastPick(BOOL reversed, RECT* out)
{
#ifdef TOASTCROP_PICK_DEFECT_UNION
    const RECT* tree = g_ToastControlTree; int n = COUNT(g_ToastControlTree); TC_PICK_MODE mode = TcPickUnion;
#else
    const RECT* tree = g_ToastRawTree; int n = COUNT(g_ToastRawTree); TC_PICK_MODE mode = TcPickLargest;
#endif
    RECT buf[32];
    if (n > 32) return FALSE;
    for (int i = 0; i < n; i++) buf[i] = reversed ? tree[n - 1 - i] : tree[i];
    return TcPickCard(g_ToastRaw, buf, n, mode, out);
}

// MENU body (2026-09-03): window 240x300 at (100,100); the presenter spans the full HEIGHT
// (not strictly smaller -> excluded); rows are 208 wide and 36 tall each; the drawn extent
// is the union of the rows = (116,108)-(324,378): L=16 R=16 T=8 B=22 - the measured
// "union B=22 accepted" vs the single-largest "B=287 rejected".
static const RECT g_MenuRaw = { 100, 100, 340, 400 };
static const RECT g_MenuTree[] = {
    { 112, 100, 328, 400 },   // MenuFlyoutPresenter: full window height -> not strictly smaller
    { 116, 108, 324, 144 },   // row 1
    { 116, 144, 324, 180 },   // row 2
    { 116, 180, 324, 216 },   // row 3
    { 116, 216, 324, 252 },   // row 4
    { 116, 252, 324, 288 },   // row 5
    { 116, 288, 324, 324 },   // row 6
    { 116, 324, 324, 360 },   // row 7
    { 116, 360, 324, 378 },   // row 8 (last, shorter)
};
static const RECT g_MenuCard = { 116, 108, 324, 378 };
static const RECT g_MenuOneRow = { 116, 108, 324, 144 };

int main(void)
{
#ifdef TOASTCROP_PICK_DEFECT_UNION
    printf("DEFECT BUILD: TOASTCROP_PICK_DEFECT_UNION - toasts use the union rule; this run MUST fail\n");
#endif
    RECT got = { 0, 0, 0, 0 };
    BOOL found;

    // 1. THE FIX: the toast path (raw view + largest) yields the card, 364x157 +-2. Under the
    //    defect define the same path is control view + union and yields the 214-wide content
    //    box - a MATERIAL width loss, asserted separately so a near-miss cannot pass.
    found = ToastPick(FALSE, &got);
    Check("toast path -> card 364x157 (+-2)", found, got, TRUE, g_ToastCard, 2);
    {
        LONG w = found ? got.right - got.left : 0;
        LONG cardW = g_ToastCard.right - g_ToastCard.left;
        g_run++;
        if (w < cardW - 2) { g_fail++; printf("FAIL toast path width %ld is materially narrower than the card %ld: buttons cut (the overcrop)\n", (long)w, (long)cardW); }
        else printf("ok   toast path width %ld keeps the card width %ld\n", (long)w, (long)cardW);
    }

    // 2. Toast, control-view leaves only, toast rule vs the union: the union is the content
    //    box (the 2026-09-03 overcrop); largest over the same leaves is ONE leaf, which is even
    //    narrower - proving that on a leaves-only tree NO rule can find the card, i.e. the raw
    //    view (where the card element is) is load-bearing for toasts, not just the rule.
    found = TcPickCard(g_ToastRaw, g_ToastControlTree, COUNT(g_ToastControlTree), TcPickUnion, &got);
    Check("toast control-leaves, union -> 214-wide content box (the defect)", found, got, TRUE, g_ToastContentBox, 0);
    {
        LONG w = got.right - got.left;
        g_run++;
        if (w >= (g_ToastCard.right - g_ToastCard.left) - 2) { g_fail++; printf("FAIL union over control leaves is not materially narrower than the card (%ld)\n", (long)w); }
        else printf("ok   union over control leaves is %ld wide < card %ld: the overcrop is reproducible\n", (long)w, (long)(g_ToastCard.right - g_ToastCard.left));
    }

    // 3. Toast, raw tree, the OTHER rule: the union over the raw tree is the card PLUS the
    //    8 px the Later button overhangs into the margin - i.e. even with the card accumulated
    //    the union is not the card once any content pokes past its edge, whereas largest is.
    //    The full-window ScrollViewer must never wash either out.
    found = TcPickCard(g_ToastRaw, g_ToastRawTree, COUNT(g_ToastRawTree), TcPickUnion, &got);
    Check("toast raw-tree, union -> card + button overhang (not the card)", found, got, TRUE, R(4716, 1210, 5088, 1367), 0);

    // 4. Toast path with the tree fed in REVERSE walk order (card last): largest must still
    //    pick it (area, not order).
    found = ToastPick(TRUE, &got);
    Check("toast path, reversed walk order -> card", found, got, TRUE, g_ToastCard, 0);

    // 5. Menu body, menu rule -> union of the rows (must stay exactly today's behaviour).
    found = TcPickCard(g_MenuRaw, g_MenuTree, COUNT(g_MenuTree), MenuRule(), &got);
    Check("menu tree, menu rule -> row union (today's crop)", found, got, TRUE, g_MenuCard, 0);

    // 6. Menu body under the toast rule collapses onto one row - the reason menus keep the
    //    union; guards the per-surface split from being "simplified" to one rule.
    found = TcPickCard(g_MenuRaw, g_MenuTree, COUNT(g_MenuTree), TcPickLargest, &got);
    Check("menu tree, largest -> one row (why menus use union)", found, got, TRUE, g_MenuOneRow, 0);

    // 7. Nothing qualifies: only full-size / outside / empty elements.
    {
        RECT none[] = {
            { 4700, 1180, 5096, 1380 },   // == window
            { 4690, 1180, 5090, 1370 },   // pokes outside on the left
            { 4700, 1180, 5096, 1300 },   // full width, shorter: not strictly smaller
            { 4800, 1200, 4800, 1300 },   // zero width
        };
        got = R(0, 0, 0, 0);
        found = TcPickCard(g_ToastRaw, none, 4, TcPickLargest, &got);
        Check("no qualifier, largest -> not found (stays uncropped)", found, got, FALSE, R(0, 0, 0, 0), 0);
        got = R(0, 0, 0, 0);
        found = TcPickCard(g_ToastRaw, none, 4, MenuRule(), &got);
        Check("no qualifier, union -> not found", found, got, FALSE, R(0, 0, 0, 0), 0);
    }

    // 8. Largest on an area tie keeps the FIRST-seen (the container in walk order).
    {
        RECT tie[] = {
            { 4716, 1210, 5080, 1367 },   // container
            { 4716, 1210, 5080, 1367 },   // a child filling it exactly
        };
        found = TcPickCard(g_ToastRaw, tie, 2, TcPickLargest, &got);
        Check("largest on tie -> first seen", found, got, TRUE, g_ToastCard, 0);
    }

    printf("%u checks, %u failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
