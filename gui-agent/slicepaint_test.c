/*
 * slicepaint_test - offline suite for the pure "buffer actually painted" predicate
 * (slicepaint.h) behind the slice-content map-hold.
 *
 * Self-contained: no rig, no capture, no agent state - it runs on the CI Windows runner right
 * after msbuild, and (plain C with shimmed Windows types) on any host with a C compiler:
 *   gcc -Wall -Wextra -I. slicepaint_test.c -o slicepaint_test && ./slicepaint_test
 * Exit 0 = every case matched; nonzero = at least one mismatch.
 *
 * WHAT IT PROVES. The map-hold's readiness must mean PAINTED, not "a copy happened":
 *   * a zeroed slab / a transparent (black) WGC first frame is NOT painted - the exact input
 *     that released the hold onto a black toast on the rig (2026-09-06, win11 24H2);
 *   * a handful of lit pixels in a black buffer is NOT painted (below 2%);
 *   * a dark-theme toast card (#2B2B2B), a fading-in white card (~10% opacity, premultiplied),
 *     and a raw toast window (black shadow margin around a lit card) ARE painted;
 *   * content darker than the black ceiling (#0C0C0C) is NOT painted - documented reliance on
 *     the bounded timeout release, pinned here so it is a checked fact, not a surprise;
 *   * padded rows (pitch > w*4) are addressed correctly; empty input is never painted.
 *
 * DEFECT RE-INTRODUCTION (CLAUDE.md: a check counts as evidence only once it has been seen to
 * FAIL). Rebuilding with -DSLICEPAINT_DEFECT_ANYCOPY (vcxproj property SlicePaintDefect, e.g.
 *   msbuild ... /t:Rebuild /p:SlicePaintDefect=SLICEPAINT_DEFECT_ANYCOPY
 * ) restores the pre-fix semantics - any non-empty copy counts as content - and this suite MUST
 * then exit nonzero (the all-black cases report "painted"). CI inverts the exit code under the
 * defect define to enforce it.
 */

#include "slicepaint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_run = 0, g_fail = 0;

// A w x h BGRA image with `pad` extra bytes per row, filled with one color.
static BYTE* Image(unsigned w, unsigned h, unsigned pad, BYTE b, BYTE g, BYTE r, size_t* pitch)
{
    *pitch = (size_t)w * 4 + pad;
    BYTE* img = (BYTE*)malloc(*pitch * h);
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
        {
            BYTE* p = img + y * *pitch + (size_t)x * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
        }
    return img;
}

static void Fill(BYTE* img, size_t pitch, unsigned x0, unsigned y0, unsigned w, unsigned h,
                 BYTE b, BYTE g, BYTE r)
{
    for (unsigned y = y0; y < y0 + h; y++)
        for (unsigned x = x0; x < x0 + w; x++)
        {
            BYTE* p = img + y * pitch + (size_t)x * 4;
            p[0] = b; p[1] = g; p[2] = r;
        }
}

static void Check(const char* name, BOOL got, BOOL expected, const SLICE_PAINT_STATS* st)
{
    g_run++;
    BOOL ok = (got != 0) == (expected != 0);
    if (!ok) g_fail++;
    printf("%s %-58s expected %-7s got %-7s (nonblack %lu / sampled %lu)\n",
        ok ? "ok  " : "FAIL", name, expected ? "painted" : "black", got ? "painted" : "black",
        (unsigned long)st->NonBlack, (unsigned long)st->Sampled);
}

int main(void)
{
#ifdef SLICEPAINT_DEFECT_ANYCOPY
    printf("DEFECT BUILD: SLICEPAINT_DEFECT_ANYCOPY - any copy counts as content; this run MUST fail\n");
#endif
    SLICE_PAINT_STATS st;
    size_t pitch;
    BYTE* img;

    // 1. Zeroed slab / transparent WGC first frame, card-sized (364x157): NOT painted. This is
    //    the input that released the hold onto a black toast.
    img = Image(364, 157, 0, 0, 0, 0, &pitch);
    Check("zeroed 364x157 slab (WGC first frame / fresh slab)", SlicePainted(img, pitch, 364, 157, &st), FALSE, &st);

    // 2. A few lit pixels in that black buffer (an icon outline): below 2% -> NOT painted.
    //    Grid points are at multiples of 8; light exactly 4 of the ~920 sampled (0.4%).
    Fill(img, pitch, 0, 0, 1, 1, 255, 255, 255);
    Fill(img, pitch, 8, 0, 1, 1, 255, 255, 255);
    Fill(img, pitch, 0, 8, 1, 1, 255, 255, 255);
    Fill(img, pitch, 8, 8, 1, 1, 255, 255, 255);
    Check("4 lit samples of ~920 (0.4%) in a black 364x157", SlicePainted(img, pitch, 364, 157, &st), FALSE, &st);
    free(img);

    // 3. Dark-theme toast card, solid #2B2B2B: painted (43 > 16 on every sample).
    img = Image(364, 157, 0, 0x2B, 0x2B, 0x2B, &pitch);
    Check("dark-theme card fill #2B2B2B", SlicePainted(img, pitch, 364, 157, &st), TRUE, &st);
    free(img);

    // 4. Fading-in white card at ~10% opacity, premultiplied (25/25/25): visible -> painted.
    img = Image(364, 157, 0, 25, 25, 25, &pitch);
    Check("white card mid fade-in, premultiplied 25/25/25", SlicePainted(img, pitch, 364, 157, &st), TRUE, &st);
    free(img);

    // 5. Content darker than the ceiling (#0C0C0C terminal): NOT painted - the bounded timeout
    //    is what maps such a window. Pinned so the reliance is explicit.
    img = Image(640, 400, 0, 0x0C, 0x0C, 0x0C, &pitch);
    Check("#0C0C0C fill is below the black ceiling (timeout maps it)", SlicePainted(img, pitch, 640, 400, &st), FALSE, &st);
    free(img);

    // 6. Raw toast window 396x200: black shadow margin (16/30/16/13) around a lit card.
    //    Painted - the card is ~73% of the window.
    img = Image(396, 200, 0, 0, 0, 0, &pitch);
    Fill(img, pitch, 16, 30, 364, 157, 0xF3, 0xF3, 0xF3);
    Check("raw 396x200 toast: black margin + lit 364x157 card", SlicePainted(img, pitch, 396, 200, &st), TRUE, &st);
    free(img);

    // 7. Exactly at the 2% threshold: 2% of samples lit -> painted; one fewer -> not.
    //    64x64 on an 8-grid = 64 samples; 2% of 64 = 1.28 -> need NonBlack*1000 >= 20*64 = 1280,
    //    i.e. 2 lit samples pass (2000 >= 1280), 1 fails (1000 < 1280).
    img = Image(64, 64, 0, 0, 0, 0, &pitch);
    Fill(img, pitch, 0, 0, 1, 1, 200, 200, 200);
    Check("64x64: 1 of 64 samples lit (1.6%) -> below threshold", SlicePainted(img, pitch, 64, 64, &st), FALSE, &st);
    Fill(img, pitch, 8, 0, 1, 1, 200, 200, 200);
    Check("64x64: 2 of 64 samples lit (3.1%) -> at/above threshold", SlicePainted(img, pitch, 64, 64, &st), TRUE, &st);
    free(img);

    // 8. Padded rows (pitch = w*4 + 64): the lit column must be found through the pitch, and
    //    the padding bytes (left as malloc garbage) must never be read as pixels. Fill the
    //    padding with 0xFF to make a pitch bug visible: a w*4-stride walk would drift into it.
    img = Image(48, 48, 64, 0, 0, 0, &pitch);
    for (unsigned y = 0; y < 48; y++) memset(img + y * pitch + 48 * 4, 0xFF, 64);
    Check("padded rows, all-black pixels, 0xFF padding -> black", SlicePainted(img, pitch, 48, 48, &st), FALSE, &st);
    Fill(img, pitch, 0, 0, 48, 48, 0x40, 0x40, 0x40);
    Check("padded rows, lit pixels -> painted", SlicePainted(img, pitch, 48, 48, &st), TRUE, &st);
    free(img);

    // 9. Empty / invalid input is never painted (and never dereferenced).
    Check("NULL base", SlicePainted(NULL, 4 * 16, 16, 16, &st), FALSE, &st);
    img = Image(16, 16, 0, 255, 255, 255, &pitch);
    Check("zero width", SlicePainted(img, pitch, 0, 16, &st), FALSE, &st);
    Check("zero height", SlicePainted(img, pitch, 16, 0, &st), FALSE, &st);
    Check("pitch shorter than a row", SlicePainted(img, 16 * 4 - 1, 16, 16, &st), FALSE, &st);
    Check("stats pointer optional", SlicePainted(img, pitch, 16, 16, NULL), TRUE, &st);
    free(img);

    printf("%u cases, %u failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
