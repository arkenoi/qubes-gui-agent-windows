/*
 * errbox_order_test.c - offline suite for errbox-order.h: the order a restarted agent announces the windows it finds
 * (docs/ADR-supervision.md section 6, main repo). The error window the route showed while no agent was alive must be
 * the FIRST window dom0 gets. Driven by tools/tests/errbox-selftest.sh, which also builds it with ERRBOX_DEFECT_NOFIRST
 * and requires the suite to FAIL. Prints "ok <case>" / "FAIL <case>" lines; exit 0 iff no FAIL.
 */
#include <stdio.h>
#include <string.h>

#include "errbox-order.h"

static int g_run, g_fail;
static void check(const char *name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}
static int sameOrder(const unsigned *got, const unsigned *want, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) if (got[i] != want[i]) return 0;
    return 1;
}

int main(void)
{
    unsigned order[16];
    /* enumeration is TOP-FIRST: index 0 is the topmost window */
    {
        const int isBox[5] = { 0, 0, 1, 0, 0 };          /* the box is third from the top */
        const unsigned want[5] = { 2, 4, 3, 1, 0 };       /* the box first, then the rest bottom-first */
        check("one box in the middle: announced first, the rest bottom-first", ErrBoxAnnounceOrder(isBox, 5, order) == 5 && sameOrder(order, want, 5));
    }
    {
        const int isBox[4] = { 1, 0, 0, 0 };              /* the box is the topmost window: bottom-first alone would announce it LAST */
        const unsigned want[4] = { 0, 3, 2, 1 };
        check("the box on top (where a fresh box sits): still first, not last", ErrBoxAnnounceOrder(isBox, 4, order) == 4 && sameOrder(order, want, 4));
    }
    {
        const int isBox[4] = { 0, 0, 0, 1 };
        const unsigned want[4] = { 3, 2, 1, 0 };
        check("the box at the bottom: the order equals plain bottom-first", ErrBoxAnnounceOrder(isBox, 4, order) == 4 && sameOrder(order, want, 4));
    }
    {
        const int isBox[6] = { 0, 1, 0, 1, 0, 0 };
        const unsigned want[6] = { 3, 1, 5, 4, 2, 0 };
        check("two boxes: both first, bottom-first among themselves, then the rest bottom-first", ErrBoxAnnounceOrder(isBox, 6, order) == 6 && sameOrder(order, want, 6));
    }
    {
        const int isBox[3] = { 0, 0, 0 };
        const unsigned want[3] = { 2, 1, 0 };
        check("no box: plain bottom-first, unchanged from before", ErrBoxAnnounceOrder(isBox, 3, order) == 3 && sameOrder(order, want, 3));
    }
    check("empty enumeration: nothing to order", ErrBoxAnnounceOrder(NULL, 0, order) == 0);
    {
        const int isBox[5] = { 1, 1, 1, 1, 1 };
        const unsigned want[5] = { 4, 3, 2, 1, 0 };
        check("every window a box: bottom-first, every index exactly once", ErrBoxAnnounceOrder(isBox, 5, order) == 5 && sameOrder(order, want, 5));
    }
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
