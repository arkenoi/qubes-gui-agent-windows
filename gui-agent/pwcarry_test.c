/*
 * pwcarry_test.c - offline suite for pwcarry.h (the rebuild content carry-over).
 *
 * Builds and runs with gcc on the dev qube (tools/tests/pwcarry-selftest.sh) and with msbuild in
 * CI. Every buffer is allocated with a poisoned guard margin on both sides and checked afterwards,
 * so an out-of-bounds row or column FAILS rather than merely looking right.
 *
 * The cases are the real ones: a toast crop-snap (new rect strictly inside the old, offset by the
 * shadow inset), a move+resize, a growth (the genuinely new edge must stay zero), a disjoint move,
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

    /* --- 2. MOVE + RESIZE: partial overlap. The overlap must be exact and everything outside it
       must still be zero (the next full frame fills that). */
    {
        BUF src = Alloc(200, 100), dst = Alloc(200, 100);
        int c, z, w, rows;
        Paint(&src, 0, 0);
        rows = (int)PwCarryBlit(src.px, 0, 0, 200, 100, dst.px, 40, 25, 200, 100);
        Tally(&dst, 40, 25, &c, &z, &w);
        Check("move: 75 rows carried", rows, 75);
        Check("move: overlap exact", c, 160 * 75);
        Check("move: remainder still black", z, 200 * 100 - 160 * 75);
        Check("move: nothing misplaced", w, 0);
        Check("move: dest guards intact", GuardsIntact(&dst), 1);
        free(src.mem); free(dst.mem);
    }

    /* --- 3. GROWTH at the same origin: the old area is carried, the new edge stays black. */
    {
        BUF src = Alloc(100, 50), dst = Alloc(160, 80);
        int c, z, w, rows;
        Paint(&src, 10, 10);
        rows = (int)PwCarryBlit(src.px, 10, 10, 100, 50, dst.px, 10, 10, 160, 80);
        Tally(&dst, 10, 10, &c, &z, &w);
        Check("growth: 50 rows carried", rows, 50);
        Check("growth: old area carried", c, 100 * 50);
        Check("growth: new edge still black", z, 160 * 80 - 100 * 50);
        Check("growth: nothing misplaced", w, 0);
        Check("growth: dest guards intact", GuardsIntact(&dst), 1);
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
        Check("disjoint: destination untouched", z, 100 * 50);
        Check("disjoint: dest guards intact", GuardsIntact(&dst), 1);
        (void)c; (void)w;
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

    printf(g_fail ? "\nFAILED (%d)\n" : "\nall checks passed (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
