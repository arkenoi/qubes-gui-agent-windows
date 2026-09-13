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
 */

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
        return 0;               /* disjoint: the window moved clear of its old rect */

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
