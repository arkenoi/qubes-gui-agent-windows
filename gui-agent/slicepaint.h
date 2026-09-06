/*
 * slicepaint - the PURE "is this per-window buffer actually painted" predicate behind the
 * slice-content map-hold (main.c: SliceContentReady / PwNoteSliceContent).
 *
 * WHY (rig, 2026-09-06, win11 24H2, a slice-fed toast on a de-slice guest): with SliceMapHold on,
 * the toast mapped at the right geometry but its pixels were BLACK and stayed black. The hold's
 * readiness signal was "a copy into the buffer happened" (the first consumed broker frame or the
 * first composite copy), and the FIRST copy for a shell surface is routinely black: a WGC frame
 * of a window whose visual tree has not rendered yet is transparent (the broker's WGC publish
 * path has no non-black check - only its PrintWindow fallback has one), and the slab itself is
 * zeroed at attach. So "content ready" fired on a black frame, the hold released, and the map
 * showed black. The whole point of the hold is defeated when its predicate means "a copy
 * happened" instead of "the window painted".
 *
 * THE PREDICATE: sample the window's own buffer (BGRA, top-down) on a SLICE_PAINT_STEP grid and
 * call it PAINTED iff at least SLICE_PAINT_MIN_PERMILLE of the sampled pixels are non-black,
 * where non-black = any of B,G,R > SLICE_PAINT_BLACK_MAX. Alpha is ignored on purpose: slice
 * sources are premultiplied or opaque, and a fading-in toast (white card at 10% opacity comes
 * out ~25/25/25) IS visible and must count as painted - the black flash is the thing to hold,
 * not the animation. The thresholds are the broker's own (PublishPrintWindow: >16, 2%), so the
 * two ends of the pipe agree on what "black" is.
 *
 * WHAT IT CANNOT DO, by design: a window whose real content is darker than the black ceiling
 * (a #0C0C0C terminal) never counts as painted. That is fine because the hold is BOUNDED - the
 * crop-before-show release maps the window after CROP_BEFORE_SHOW_TIMEOUT_MS regardless - so the
 * worst case for such a window is a bounded delay, never a window that stays hidden. The suite
 * (slicepaint_test.c) pins that case so the reliance on the timeout is a checked fact.
 *
 * Portable on purpose (mirrors toastcrop-pick.h): Windows types from <windows.h> where it
 * exists, shimmed otherwise, so the offline suite builds with gcc as well as MSVC.
 */

#pragma once

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#else
typedef unsigned char BYTE;
typedef int  BOOL;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#endif

// A channel value above this is "not black". Same ceiling as the broker's PrintWindow test.
#define SLICE_PAINT_BLACK_MAX     16
// Painted iff at least this many per mille of the SAMPLED pixels are non-black (2%, the
// broker's "PrintWindow produced something" threshold).
#define SLICE_PAINT_MIN_PERMILLE  20
// Sampling grid pitch in pixels, both axes: a 364x157 card is ~900 samples, a 4K overlay
// ~130k - cheap enough to run on every copy until the window first counts as painted.
#define SLICE_PAINT_STEP          8

typedef struct _SLICE_PAINT_STATS
{
    size_t Sampled;    // grid points examined
    size_t NonBlack;   // of those, pixels with any of B,G,R > SLICE_PAINT_BLACK_MAX
} SLICE_PAINT_STATS;

// TRUE iff the w x h BGRA image at base (row pitch `pitch` bytes) counts as painted. Zero-sized
// or NULL input is never painted. `stats` (optional) receives the sample counts for logging.
static __inline BOOL SlicePainted(const BYTE* base, size_t pitch, unsigned w, unsigned h,
                                  SLICE_PAINT_STATS* stats)
{
    SLICE_PAINT_STATS st;
    st.Sampled = 0;
    st.NonBlack = 0;
    if (stats)
        *stats = st;
    if (!base || w == 0 || h == 0 || pitch < (size_t)w * 4)
        return FALSE;

#ifdef SLICEPAINT_DEFECT_ANYCOPY
    // DEFECT RE-INTRODUCTION: the pre-fix semantics - any copy into a non-empty buffer counted
    // as content, black or not. Built only by the suite's defect pass, which MUST then fail.
    return TRUE;
#else
    for (unsigned y = 0; y < h; y += SLICE_PAINT_STEP)
    {
        const BYTE* row = base + (size_t)y * pitch;
        for (unsigned x = 0; x < w; x += SLICE_PAINT_STEP)
        {
            const BYTE* p = row + (size_t)x * 4;
            st.Sampled++;
            if (p[0] > SLICE_PAINT_BLACK_MAX || p[1] > SLICE_PAINT_BLACK_MAX ||
                p[2] > SLICE_PAINT_BLACK_MAX)
                st.NonBlack++;
        }
    }
    if (stats)
        *stats = st;
    // per-mille compare without overflow for any realistic buffer (NonBlack <= Sampled)
    return st.Sampled != 0 &&
           st.NonBlack * 1000 >= (size_t)SLICE_PAINT_MIN_PERMILLE * st.Sampled;
#endif
}
