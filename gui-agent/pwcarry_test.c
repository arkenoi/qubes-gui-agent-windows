/*
 * pwcarry_test.c - offline suite for pwcarry.h (the rebuild content carry-over).
 *
 * Builds and runs with gcc on the dev qube (tools/tests/pwcarry-selftest.sh) and with msbuild in
 * CI. Every buffer is allocated with a poisoned guard margin on both sides and checked afterwards,
 * so an out-of-bounds row or column FAILS rather than merely looking right.
 *
 * The cases are the real ones: a toast crop-snap (new rect strictly inside the old, offset by the
 * shadow inset), a move+resize, a growth (the newly exposed edge must be the surface's own
 * background, never black), a genuinely dark source that must invent nothing, a disjoint move,
 * an identical rect, the same-slab refusal, and degenerate dimensions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pwcarry.h"

static int g_fail = 0;

static void Check(const char* what, int got, int want)
{
    if (got == want) { printf("ok    %s\n", what); }
    else { printf("FAIL  %s (got %d, want %d)\n", what, got, want); g_fail++; }
}

#define GUARD 64
#define POISON 0xA5

/* A buffer of w*h BGRA pixels with a poisoned guard on each side. */
typedef struct { unsigned char* mem; unsigned char* px; unsigned w, h; } BUF;

static BUF Alloc(unsigned w, unsigned h)
{
    BUF b;
    size_t n = (size_t)w * h * 4;
    b.mem = (unsigned char*)malloc(n + 2 * GUARD);
    memset(b.mem, POISON, n + 2 * GUARD);
    b.px = b.mem + GUARD;
    memset(b.px, 0, n);          /* a fresh slab is ZEROED (PwSlabAcquire) */
    b.w = w; b.h = h;
    return b;
}

static int GuardsIntact(const BUF* b)
{
    size_t n = (size_t)b->w * b->h * 4, i;
    for (i = 0; i < GUARD; i++)
        if (b->mem[i] != POISON || b->mem[GUARD + n + i] != POISON)
            return 0;
    return 1;
}

/* Fill every pixel with a value derived from its SCREEN coordinate, so a misplaced copy is
 * detectable: the destination must end up holding the value for its own screen position. */
static unsigned char Val(int sx, int sy) { return (unsigned char)(1 + ((sx * 7 + sy * 13) & 0x7E)); }

static void Paint(BUF* b, int ox, int oy)
{
    unsigned x, y;
    for (y = 0; y < b->h; y++)
        for (x = 0; x < b->w; x++)
            memset(b->px + ((size_t)y * b->w + x) * 4, Val(ox + (int)x, oy + (int)y), 4);
}

/* Count destination pixels that hold the right screen-derived value, and those still zero. */
static void Tally(const BUF* d, int ox, int oy, int* correct, int* zero, int* wrong)
{
    unsigned x, y;
    *correct = *zero = *wrong = 0;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++)
        {
            unsigned char v = d->px[((size_t)y * d->w + x) * 4];
            if (v == Val(ox + (int)x, oy + (int)y)) (*correct)++;
            else if (v == 0) (*zero)++;
            else (*wrong)++;
        }
}

/* Mismatches strictly INSIDE the carried region, in destination-local coordinates. Counting
 * "correct" over the WHOLE destination stopped working once the uncovered area is background-
 * filled: Val() is one byte, so a filled pixel coincides with its screen-derived value now and
 * then (measured: 124 of 20000). That is a property of the test's own pattern, not of the blit -
 * so assert the overlap exactly, and let the separate background checks own the rest. */
static int MismatchesIn(const BUF* d, int ox, int oy, unsigned rx, unsigned ry,
                        unsigned rw, unsigned rh)
{
    unsigned x, y; int bad = 0;
    for (y = ry; y < ry + rh; y++)
        for (x = rx; x < rx + rw; x++)
            if (d->px[((size_t)y * d->w + x) * 4] != Val(ox + (int)x, oy + (int)y))
                bad++;
    return bad;
}

int main(void)
{
    /* --- 1. TOAST CROP-SNAP: the new rect is strictly inside the old, offset by the shadow
       inset. Every destination pixel must come out correct, and none may be zero or wrong.
       This is the case a copy-from-(0,0) version gets subtly wrong. */
    {
        BUF src = Alloc(396, 200), dst = Alloc(364, 157);
        int c, z, w, rows;
        Paint(&src, 1000, 500);
        rows = (int)PwCarryBlit(src.px, 1000, 500, 396, 200, dst.px, 1016, 521, 364, 157);
        Tally(&dst, 1016, 521, &c, &z, &w);
        Check("crop-snap: 157 rows carried", rows, 157);
        Check("crop-snap: every pixel correct", c, 364 * 157);
        Check("crop-snap: no pixel left black", z, 0);
        Check("crop-snap: no pixel misplaced", w, 0);
        Check("crop-snap: source guards intact", GuardsIntact(&src), 1);
        Check("crop-snap: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 2. MOVE + RESIZE: partial overlap. The overlap must be exact, and everything OUTSIDE it
       must be the sampled background - never black. (Before the background fill this asserted
       "remainder still black", which was the defect, not the contract.) */
    {
        BUF src = Alloc(200, 100), dst = Alloc(200, 100);
        int c, z, w, rows, filled = 0;
        unsigned bg, x, y;
        Paint(&src, 0, 0);
        bg = PwCarrySampleBg(src.px, 200, 40, 25, 160, 75);
        rows = (int)PwCarryBlit(src.px, 0, 0, 200, 100, dst.px, 40, 25, 200, 100);
        Tally(&dst, 40, 25, &c, &z, &w);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
            {
                unsigned v;
                memcpy(&v, dst.px + ((size_t)y * dst.w + x) * 4, 4);
                if (v == bg) filled++;
            }
        Check("move: 75 rows carried", rows, 75);
        Check("move: overlap exact", MismatchesIn(&dst, 40, 25, 0, 0, 160, 75), 0);
        Check("move: carried at least the overlap", c >= 160 * 75, 1);
        Check("move: NOTHING left black", z, 0);
        Check("move: a background was sampled", bg != 0, 1);
        Check("move: remainder is the sampled background",
              filled >= 200 * 100 - 160 * 75, 1);
        Check("move: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 3. GROWTH at the same origin: the old area is carried, and the newly exposed edge is
       the sampled background rather than a black hole. */
    {
        BUF src = Alloc(100, 50), dst = Alloc(160, 80);
        int c, z, w, rows, filled = 0;
        unsigned bg, x, y;
        Paint(&src, 10, 10);
        bg = PwCarrySampleBg(src.px, 100, 0, 0, 100, 50);
        rows = (int)PwCarryBlit(src.px, 10, 10, 100, 50, dst.px, 10, 10, 160, 80);
        Tally(&dst, 10, 10, &c, &z, &w);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
            {
                unsigned v;
                memcpy(&v, dst.px + ((size_t)y * dst.w + x) * 4, 4);
                if (v == bg) filled++;
            }
        Check("growth: 50 rows carried", rows, 50);
        Check("growth: old area carried", MismatchesIn(&dst, 10, 10, 0, 0, 100, 50), 0);
        Check("growth: carried at least the old area", c >= 100 * 50, 1);
        Check("growth: NOTHING left black", z, 0);
        Check("growth: new edge is the sampled background",
              filled >= 160 * 80 - 100 * 50, 1);
        Check("growth: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 3b. GROWTH, BACKGROUND-FILLED: the newly-exposed area must show the surface's own
       background, not black. This is the owner's "second one in the same window gets black for a
       moment" - the real geometry measured on win11-up, where a second toast grows the shell's
       toast host 364x157 -> 364x326 and moves it UP, exposing 169 rows at the top. */
    {
        BUF src = Alloc(364, 157), dst = Alloc(364, 326);
        unsigned x, y, zero = 0, bg = 0, rows;
        const unsigned char CARD = 0x2B;              /* dark-theme toast card */
        for (y = 0; y < src.h; y++)
            for (x = 0; x < src.w; x++)
                memset(src.px + ((size_t)y * src.w + x) * 4, CARD, 4);
        rows = PwCarryBlit(src.px, 4740, 1222, 364, 157, dst.px, 4740, 1053, 364, 326);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
            {
                unsigned char v = dst.px[((size_t)y * dst.w + x) * 4];
                if (v == 0) zero++;
                else if (v == CARD) bg++;
            }
        Check("toast growth: 157 rows carried", (int)rows, 157);
        Check("toast growth: NOTHING left black", (int)zero, 0);
        Check("toast growth: whole card is the sampled background", (int)bg, 364 * 326);
        Check("toast growth: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 3c. A GENUINELY DARK SOURCE INVENTS NOTHING. Same discipline as MenuFillNearBlack: if
       no sample is bright enough the surface really is that dark, so skip the fill rather than
       paint a colour that was never on screen. */
    {
        BUF src = Alloc(364, 157), dst = Alloc(364, 326);
        unsigned x, y, zero = 0, rows;
        for (y = 0; y < src.h; y++)
            for (x = 0; x < src.w; x++)
                memset(src.px + ((size_t)y * src.w + x) * 4, 8, 4);   /* below the 24 ceiling */
        rows = PwCarryBlit(src.px, 4740, 1222, 364, 157, dst.px, 4740, 1053, 364, 326);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
                if (dst.px[((size_t)y * dst.w + x) * 4] == 0) zero++;
        Check("dark source: rows still carried", (int)rows, 157);
        Check("dark source: no colour invented (uncovered stays zero)", (int)zero, 364 * (326 - 157));
        free(src.mem); free(dst.mem);
    }

    /* --- 4. SHRINK INSIDE, no move: whole destination covered. */
    {
        BUF src = Alloc(160, 80), dst = Alloc(100, 50);
        int c, z, w, rows;
        Paint(&src, -5, -5);
        rows = (int)PwCarryBlit(src.px, -5, -5, 160, 80, dst.px, -5, -5, 100, 50);
        Tally(&dst, -5, -5, &c, &z, &w);
        Check("shrink: 50 rows carried", rows, 50);
        Check("shrink: fully covered", c, 100 * 50);
        Check("shrink: nothing black", z, 0);
        Check("shrink: nothing misplaced", w, 0);
        free(src.mem); free(dst.mem);
    }

    /* --- 5. DISJOINT: the window moved clear of its old rect - carry nothing, touch nothing. */
    {
        BUF src = Alloc(100, 50), dst = Alloc(100, 50);
        int c, z, w, rows;
        Paint(&src, 0, 0);
        rows = (int)PwCarryBlit(src.px, 0, 0, 100, 50, dst.px, 500, 500, 100, 50);
        Tally(&dst, 500, 500, &c, &z, &w);
        Check("disjoint: nothing carried", rows, 0);
        Check("disjoint: destination NOT left black", z, 0);
        Check("disjoint: dest guards intact", GuardsIntact(&dst), 1);
        (void)c; (void)w;
        free(src.mem); free(dst.mem);
    }

    /* --- 5b. DISJOINT, the real one: the toast host jumps clear of its old rect
       (364x326@4740,1053 -> 364x338@4740,687 - measured on win11-up). Nothing overlaps, so
       nothing is carried, but the destination must still be the surface's background rather
       than a black hole - that is the owner's "short black flash sometimes". */
    {
        BUF src = Alloc(364, 326), dst = Alloc(364, 338);
        unsigned x, y, zero = 0, bg = 0, rows;
        const unsigned char CARD = 0x2B;
        for (y = 0; y < src.h; y++)
            for (x = 0; x < src.w; x++)
                memset(src.px + ((size_t)y * src.w + x) * 4, CARD, 4);
        rows = PwCarryBlit(src.px, 4740, 1053, 364, 326, dst.px, 4740, 687, 364, 338);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
            {
                unsigned char v = dst.px[((size_t)y * dst.w + x) * 4];
                if (v == 0) zero++; else if (v == CARD) bg++;
            }
        Check("toast jump: nothing carried (rects are disjoint)", (int)rows, 0);
        Check("toast jump: NOTHING left black", (int)zero, 0);
        Check("toast jump: whole buffer is the sampled background", (int)bg, 364 * 338);
        Check("toast jump: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 6. IDENTICAL RECT (a rebuild that did not actually change geometry). */
    {
        BUF src = Alloc(64, 64), dst = Alloc(64, 64);
        int c, z, w, rows;
        Paint(&src, 7, 9);
        rows = (int)PwCarryBlit(src.px, 7, 9, 64, 64, dst.px, 7, 9, 64, 64);
        Tally(&dst, 7, 9, &c, &z, &w);
        Check("identical: 64 rows carried", rows, 64);
        Check("identical: byte-for-byte", c, 64 * 64);
        Check("identical: nothing black", z, 0);
        free(src.mem); free(dst.mem);
    }

    /* --- 7. SAME BUFFER: the pool handed the slab straight back. It already holds the pixels;
       a memcpy onto itself with a different stride would shred them. */
    {
        BUF b = Alloc(64, 64);
        int c, z, w, rows;
        Paint(&b, 0, 0);
        rows = (int)PwCarryBlit(b.px, 0, 0, 64, 64, b.px, 0, 4, 64, 64);
        Tally(&b, 0, 0, &c, &z, &w);
        Check("same buffer: refused", rows, 0);
        Check("same buffer: content untouched", c, 64 * 64);
        free(b.mem);
    }

    /* --- 8. DEGENERATE inputs: never a copy, never a crash. */
    {
        BUF src = Alloc(16, 16), dst = Alloc(16, 16);
        Paint(&src, 0, 0);
        Check("null source", (int)PwCarryBlit(NULL, 0, 0, 16, 16, dst.px, 0, 0, 16, 16), 0);
        Check("null dest", (int)PwCarryBlit(src.px, 0, 0, 16, 16, NULL, 0, 0, 16, 16), 0);
        Check("zero source width", (int)PwCarryBlit(src.px, 0, 0, 0, 16, dst.px, 0, 0, 16, 16), 0);
        Check("zero source height", (int)PwCarryBlit(src.px, 0, 0, 16, 0, dst.px, 0, 0, 16, 16), 0);
        Check("zero dest width", (int)PwCarryBlit(src.px, 0, 0, 16, 16, dst.px, 0, 0, 0, 16), 0);
        Check("zero dest height", (int)PwCarryBlit(src.px, 0, 0, 16, 16, dst.px, 0, 0, 16, 0), 0);
        Check("degenerate: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 9. REMEMBERED BACKGROUND. A rebuild whose OUTGOING buffer is itself dark - a broker
       re-registration publishes a black first frame - has nothing to sample, so without a
       remembered background it hands dom0 a zeroed slab. That is the residual "small black flash
       still happens". With one, the surface is painted. */
    {
        BUF src = Alloc(364, 157), dst = Alloc(364, 326);
        unsigned x, y, zero = 0, hit = 0, used = 0;
        const unsigned REMEMBERED = 0xFF2B2B2Bu;
        for (y = 0; y < src.h; y++)                     /* source is BLACK: nothing to sample */
            for (x = 0; x < src.w; x++)
                memset(src.px + ((size_t)y * src.w + x) * 4, 0, 4);
        (void)PwCarryBlit2(src.px, 4740, 1222, 364, 157, dst.px, 4740, 1053, 364, 326,
                           REMEMBERED, &used);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
            {
                unsigned v;
                memcpy(&v, dst.px + ((size_t)y * dst.w + x) * 4, 4);
                if (v == 0) zero++; else if (v == REMEMBERED) hit++;
            }
        Check("remembered bg: reported back to the caller", used == REMEMBERED, 1);
        Check("remembered bg: NOTHING left black", (int)zero, 0);
        Check("remembered bg: whole buffer painted with it", (int)hit, 364 * 326);
        free(src.mem); free(dst.mem);
    }

    /* --- 9b. AND STILL INVENTS NOTHING when there is neither a sample nor a memory. */
    {
        BUF src = Alloc(364, 157), dst = Alloc(364, 326);
        unsigned x, y, zero = 0, used = 0xDEADBEEF;
        for (y = 0; y < src.h; y++)
            for (x = 0; x < src.w; x++)
                memset(src.px + ((size_t)y * src.w + x) * 4, 0, 4);
        (void)PwCarryBlit2(src.px, 4740, 1222, 364, 157, dst.px, 4740, 1053, 364, 326, 0, &used);
        for (y = 0; y < dst.h; y++)
            for (x = 0; x < dst.w; x++)
                if (dst.px[((size_t)y * dst.w + x) * 4] == 0) zero++;
        Check("no sample, no memory: nothing invented", (int)zero, 364 * 326);
        Check("no sample, no memory: reports no background", (int)used, 0);
        free(src.mem); free(dst.mem);
    }

    printf(g_fail ? "\nFAILED (%d)\n" : "\nall checks passed (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
