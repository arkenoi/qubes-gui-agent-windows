/*
 * errbox-order.h - the ORDER the agent announces the windows it finds at its start (docs/ADR-supervision.md 6,
 * main repo; owner 2026-10-07: "if it dies, error notification should be the first thing it shows").
 *
 * AddAllWindows (main.c) enumerates the top-level windows top-first and announces them BOTTOM-FIRST, so that X's
 * create-on-top semantics rebuild the guest's stacking (see AddWindowsProc). The error window the route showed
 * while the agent was dead is one of those windows; it goes FIRST - all error boxes, bottom-first among
 * themselves, then everything else bottom-first - so a restarted agent's first dom0 window is the error that
 * killed its predecessor. Pure, no Windows types: the offline suite (errbox_order_test.c, gcc on the dev qube via
 * tools/tests/errbox-selftest.sh) holds the order and sees it fail with ERRBOX_DEFECT_NOFIRST.
 */
#ifndef QWT_ERRBOX_ORDER_H
#define QWT_ERRBOX_ORDER_H

/* isBox[i]: window i (in top-first enumeration order) is a system error box. Fills order[0..count-1] with the
 * indices in announce order and returns count. order must hold count entries. */
static __inline unsigned ErrBoxAnnounceOrder(const int *isBox, unsigned count, unsigned *order)
{
    unsigned n = 0, i;
#ifndef ERRBOX_DEFECT_NOFIRST
    /* the error boxes first, bottom-first among themselves */
    for (i = count; i > 0; i--)
        if (isBox[i - 1])
            order[n++] = i - 1;
    /* then the rest, bottom-first */
    for (i = count; i > 0; i--)
        if (!isBox[i - 1])
            order[n++] = i - 1;
#else
    /* DEFECT: plain bottom-first - the box is announced wherever the z-order put it */
    (void)isBox;
    for (i = count; i > 0; i--)
        order[n++] = i - 1;
#endif
    return n;
}

#endif /* QWT_ERRBOX_ORDER_H */
