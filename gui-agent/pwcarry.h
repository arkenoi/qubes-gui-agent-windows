#pragma once
/*
 * pwcarry.h - carry a per-window buffer's pixels across a rebuild (perwindow.c: PwResizeWindow).
 *
 * WHY THIS IS ITS OWN HEADER. PwSlabAcquire zeroes every slab it hands out, so a detach/re-attach
 * hands dom0 a BLACK buffer; for a slice-fed window nothing prefills it, and the window stays
 * black until its next full copy (one composite frame on the win10 path, but a broker
 * re-registration plus a published frame on de-slice - hundreds of ms to seconds). That is the
 * black blink the owner reported on 2026-09-13. The fix copies the surviving pixels over before
 * the new buffer is announced, and the part that can be silently WRONG is the geometry: the two
 * buffers are indexed window-relative to DIFFERENT origins, so a naive copy from (0,0) - the
 * obvious version - lands the old pixels shifted by the crop inset and trades a black flash for a
 * jittering one. Pure, offline-testable, with the near-miss versions provable as defects:
 * tools/tests/pwcarry-selftest.sh.
 *
 * All rects are in SCREEN space; both buffers are tightly packed BGRA (pitch = width * 4).
 */

#include <string.h>

/* Deliberate defect injection - the selftest builds these and REQUIRES the suite to fail.
 *   PWCARRY_DEFECT_ZEROOFFSET   copy from the source's (0,0) instead of the screen-space overlap
 *   PWCARRY_DEFECT_NOINTERSECT  copy min(w)/min(h) from (0,0) without intersecting the rects
 *   PWCARRY_DEFECT_SAMEBUFFER   skip the src==dst guard (a pool that returned the same slab)
 *   PWCARRY_DEFECT_NOFILL       leave the newly-exposed area zeroed instead of filling it with
 *                               the surface's own background (the owner's "second one in the same
 *                               window gets black for a moment")
 */

/* THE UNCOVERED AREA MUST NOT BE BLACK EITHER (owner, 2026-09-13: "second one in the same window
 * gets black for a moment ... can we fill it with a normal background color instead of black
 * first?").
 *
 * A GROWTH carries only what existed before. Measured live on win11-up: a second toast makes the
 * shell's toast host grow 364x157 -> 364x326 and move UP (y 1222 -> 1053), so the carry preserves
 * the first toast at the bottom and the 169 newly-exposed rows at the top - where the new toast is
 * about to be drawn - stay zeroed until its first frame arrives. Zeroed is BLACK.
 *
 * So sample the surface's own background out of the region we are carrying and paint the whole
 * destination with it before the blit. The discipline is copied from MenuFillNearBlack (main.c),
 * including its most important rule: if nothing bright enough is found, the surface really is that
 * dark - SKIP rather than invent a colour. Sampling the carried pixels is the honest source; there
 * is no way to ask a WinUI toast what its background is.
 *
 * Returns the BGRA background, or 0 when the source has nothing bright enough to sample. */
static __inline unsigned PwCarrySampleBg(
    const unsigned char* src, unsigned srcW, int sx, int sy, int sw, int sh)
{
    int probe, py, px;
    if (!src || sw < 4 || sh < 4)
        return 0;
    /* Top edge of the carried region: chrome above any text, same as the menu sampler. */
    for (probe = 0; probe < 3; probe++)
    {
        px = sx + ((probe == 0) ? (sw / 2) : (probe == 1) ? 2 : (sw - 3));
        for (py = sy + 1; py < sy + 5 && py < sy + sh; py++)
        {
            const unsigned char* q = src + ((size_t)py * srcW + px) * 4;
            if (q[0] >= 24 || q[1] >= 24 || q[2] >= 24)
            {
                unsigned v;
                memcpy(&v, q, 4);
                return v;
            }
        }
    }
    return 0;
}

/* Copy the overlapping region of the OLD window rect into the NEW one.
 * Returns the number of rows copied; 0 means nothing was carried (disjoint, degenerate, or the
 * same buffer). Never reads or writes outside either buffer. */
static __inline unsigned PwCarryBlit(
    const unsigned char* src, int srcX, int srcY, unsigned srcW, unsigned srcH,
    unsigned char* dst, int dstX, int dstY, unsigned dstW, unsigned dstH)
{
    int l, t, r, b, y;
    size_t bytes;
    const unsigned char* s;
    unsigned char* d;

    if (!src || !dst)
        return 0;
#ifndef PWCARRY_DEFECT_SAMEBUFFER
    if (src == dst)
        return 0;               /* the pool handed back the same slab: it already holds the pixels */
#endif
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0)
        return 0;

#ifdef PWCARRY_DEFECT_NOINTERSECT
    l = 0; t = 0;
    r = (int)(srcW < dstW ? srcW : dstW);
    b = (int)(srcH < dstH ? srcH : dstH);
    srcX = srcY = dstX = dstY = 0;
#else
    /* Screen-space intersection of the two window rects. */
    l = srcX > dstX ? srcX : dstX;
    t = srcY > dstY ? srcY : dstY;
    r = (srcX + (int)srcW) < (dstX + (int)dstW) ? (srcX + (int)srcW) : (dstX + (int)dstW);
    b = (srcY + (int)srcH) < (dstY + (int)dstH) ? (srcY + (int)srcH) : (dstY + (int)dstH);
#endif
    if (r <= l || b <= t)
    {
        /* DISJOINT, AND STILL NOT ALLOWED TO BE BLACK (owner, 2026-09-13: "there is still a short
         * black flash sometimes"). Measured on win11-up minutes later, the exact event:
         *
         *   PWCARRY nothing carried (364x326@4740,1053 -> 364x338@4740,687)
         *
         * The toast host jumped far enough up that the new rect (687..1025) clears the old one
         * (1053..1379) completely. No overlap means nothing to copy - but the old buffer still
         * holds the surface's pixels, so its BACKGROUND is still knowable. Paint the new buffer
         * with it. A card-coloured window for one frame is the same surface; a black one is a
         * hole. Sampling is from the whole source here, since no sub-rect survives.
         * The "invent nothing" rule is unchanged: no bright sample, no fill. */
#ifndef PWCARRY_DEFECT_NOFILL
        {
            unsigned bg = PwCarrySampleBg(src, srcW, 0, 0, (int)srcW, (int)srcH);
            if (bg)
            {
                unsigned x, y2;
                for (y2 = 0; y2 < dstH; y2++)
                {
                    unsigned char* row = dst + (size_t)y2 * dstW * 4;
                    for (x = 0; x < dstW; x++, row += 4)
                        memcpy(row, &bg, 4);
                }
            }
        }
#endif
        return 0;               /* nothing CARRIED - the caller logs that, and it is still true */
    }

    /* Paint the surface's own background first, so any part of the new buffer the carry does not
     * cover shows the card instead of a black hole. Only when the destination is actually bigger
     * than the overlap - an exact or shrinking rebuild has nothing uncovered to fill. */
#ifndef PWCARRY_DEFECT_NOFILL
    if ((unsigned)(r - l) < dstW || (unsigned)(b - t) < dstH)
    {
        unsigned bg = PwCarrySampleBg(src, srcW, l - srcX, t - srcY, r - l, b - t);
        if (bg)
        {
            unsigned x, y2;
            for (y2 = 0; y2 < dstH; y2++)
            {
                unsigned char* row = dst + (size_t)y2 * dstW * 4;
                for (x = 0; x < dstW; x++, row += 4)
                    memcpy(row, &bg, 4);
            }
        }
    }
#endif

    bytes = (size_t)(r - l) * 4;
#ifdef PWCARRY_DEFECT_ZEROOFFSET
    s = src;
#else
    s = src + (size_t)(t - srcY) * (size_t)srcW * 4 + (size_t)(l - srcX) * 4;
#endif
    d = dst + (size_t)(t - dstY) * (size_t)dstW * 4 + (size_t)(l - dstX) * 4;
    for (y = t; y < b; y++)
    {
        memcpy(d, s, bytes);
        s += (size_t)srcW * 4;
        d += (size_t)dstW * 4;
    }
    return (unsigned)(b - t);
}
