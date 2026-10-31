/*
 * varispeedtest.c -- turntable speed, off the board.
 *
 * What it claims, and what is checked:
 *   - ×1.00 is a copy, bit for bit, whatever the block sizes;
 *   - equal fractions are ×1.00 (33⅓/33⅓ and 45/45 alike);
 *   - 45/33⅓ moves a tone up by 1.35 and eats the input 1.35 times as fast;
 *   - the output does not depend on how the writer cuts its blocks,
 *     including blocks shorter than the 4 frames the cubic needs and
 *     steps longer than the block (78/16⅔);
 *   - the slide lands exactly on its target.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "varispeed.h"

static int fails;
static void ck(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

#define N 44100

static int16_t *tone(double hz)
{
    int16_t *s = malloc(N * 4);
    for (int k = 0; k < N; k++) {
        const int16_t x = (int16_t)lrint(12000.0 * sin(2 * M_PI * hz * k / 44100.0));
        s[2 * k] = x;
        s[2 * k + 1] = (int16_t)-x;
    }
    return s;
}

/* Feed `in` through in blocks of in_blk input / out_blk output. */
static size_t play(uint32_t step, const int16_t *in, size_t in_n,
                   size_t in_blk, size_t out_blk, int16_t *out, size_t out_max,
                   size_t *consumed)
{
    varispeed_t v;
    varispeed_reset(&v);
    size_t pos = 0, n = 0;
    for (int guard = 0; guard < 10000000; guard++) {
        size_t avail = in_n - pos;
        if (avail > in_blk) avail = in_blk;
        size_t cap = out_max - n;
        if (cap > out_blk) cap = out_blk;
        if (!cap) break;
        size_t used = 0;
        const size_t got = varispeed_run(&v, step, in + 2 * pos, avail, &used,
                                         out + 2 * n, cap);
        pos += used;
        n += got;
        if (!got && !used) break;     /* starved: end of input */
    }
    *consumed = pos;
    return n;
}

static int crossings(const int16_t *s, size_t n)
{
    int c = 0;
    for (size_t k = 1; k < n; k++)
        if ((s[2 * (k - 1)] < 0) != (s[2 * k] < 0)) c++;
    return c;
}

int main(void)
{
    int16_t *in = tone(1000.0);
    int16_t *a = calloc(4 * N, 4), *b = calloc(4 * N, 4);
    size_t used, na, nb;

    /* --- unity --- */
    ck("33/33 is unity", varispeed_step(1, 1) == VARISPEED_UNITY);
    ck("45/45 is unity", varispeed_step(2, 2) == VARISPEED_UNITY);
    ck("78/78 is unity", varispeed_step(3, 3) == VARISPEED_UNITY);
    ck("out of range reads as unity", varispeed_step(9, 1) == VARISPEED_UNITY);

    /* The window holds the last two frames back: N - 2 come out. */
    na = play(VARISPEED_UNITY, in, N, 4096, 4096, a, 4 * N, &used);
    ck("unity: every frame in, all but the last two out", na == N - 2 && used == N);
    ck("unity: bit for bit", memcmp(a, in, (size_t)(N - 2) * 4) == 0);
    nb = play(VARISPEED_UNITY, in, N, 7, 3, b, 4 * N, &used);
    ck("unity: bit for bit in 7-in / 3-out blocks",
       nb == N - 2 && memcmp(b, in, (size_t)(N - 2) * 4) == 0);
    nb = play(VARISPEED_UNITY, in, N, 1, 1, b, 4 * N, &used);
    ck("unity: one frame at a time still moves",
       nb == N - 2 && memcmp(b, in, (size_t)(N - 2) * 4) == 0);

    {
        varispeed_t v;
        varispeed_reset(&v);
        ck("fresh state is not active", !varispeed_active(&v));
        size_t u;
        int16_t o[8];
        varispeed_run(&v, VARISPEED_UNITY, in, 2, &u, o, 4);
        ck("one call makes it active", varispeed_active(&v));
        ck("two frames offered, two taken, none out yet", u == 2);
    }

    /* --- 45/33 --- */
    const uint32_t s45 = varispeed_step(2, 1);
    ck("45/33 step is 1.35", s45 == (uint32_t)((135ull << 16) / 100));
    na = play(s45, in, N, 4096, 4096, a, 4 * N, &used);
    ck("45/33: input eaten 1.35x as fast",
       fabs((double)used / (double)na - 1.35) < 0.001);
    {
        const double hz = crossings(a, na) / 2.0 / ((double)na / 44100.0);
        printf("    45/33 tone: %.1f Hz from 1000\n", hz);
        ck("45/33: 1000 Hz comes out at 1350", fabs(hz - 1350.0) < 3.0);
    }

    /* --- block independence --- */
    static const struct { unsigned p, c; const char *name; } pairs[] = {
        { 2, 1, "45/33" }, { 0, 1, "16/33" }, { 3, 0, "78/16 (4.68)" },
        { 0, 3, "16/78 (0.21)" }, { 1, 2, "33/45" },
    };
    static const size_t blk[][2] = { { 1, 1 }, { 2, 5 }, { 3, 64 }, { 1000, 17 }, { 4096, 4096 } };
    for (size_t p = 0; p < sizeof pairs / sizeof *pairs; p++) {
        const uint32_t st = varispeed_step(pairs[p].p, pairs[p].c);
        size_t ref_used;
        const size_t ref = play(st, in, N, 100000, 100000, a, 4 * N, &ref_used);
        int same = 1;
        for (size_t q = 0; q < sizeof blk / sizeof *blk; q++) {
            size_t u;
            const size_t got = play(st, in, N, blk[q][0], blk[q][1], b, 4 * N, &u);
            if (got != ref || memcmp(a, b, ref * 4) != 0) {
                printf("    %s differs at blocks %zu/%zu: %zu vs %zu frames\n",
                       pairs[p].name, blk[q][0], blk[q][1], got, ref);
                same = 0;
            }
        }
        char what[80];
        snprintf(what, sizeof what, "%s: same output whatever the blocks", pairs[p].name);
        ck(what, same);
        const double want = (double)st / 65536.0;
        snprintf(what, sizeof what, "%s: speed %.3f", pairs[p].name, want);
        ck(what, fabs((double)ref_used / (double)ref - want) < 0.01);
    }

    /* --- clipping stays in range --- */
    {
        int16_t sq[2 * 64];
        for (int k = 0; k < 64; k++) sq[2 * k] = sq[2 * k + 1] = (k / 4) % 2 ? 32767 : -32768;
        int16_t o[2 * 256];
        varispeed_t v;
        varispeed_reset(&v);
        size_t u;
        const size_t n = varispeed_run(&v, varispeed_step(0, 1), sq, 64, &u, o, 256);
        ck("full-scale square: overshoot clipped, not wrapped", n > 0);
    }

    /* --- the slide --- */
    ck("slide starts at from", varispeed_slide(s45, VARISPEED_UNITY, 0, 1000) == s45);
    ck("slide ends at unity", varispeed_slide(s45, VARISPEED_UNITY, 1000, 1000) == VARISPEED_UNITY);
    ck("slide past its end stays at unity",
       varispeed_slide(s45, VARISPEED_UNITY, 5000, 1000) == VARISPEED_UNITY);
    {
        const long half = (long)varispeed_slide(s45, VARISPEED_UNITY, 500, 1000);
        const long want = ((long)VARISPEED_UNITY + (long)s45) / 2;
        ck("slide halfway is halfway (to the last bit)", labs(half - want) <= 1);
    }
    ck("zero-length slide is the target",
       varispeed_slide(s45, VARISPEED_UNITY, 0, 0) == VARISPEED_UNITY);

    free(in); free(a); free(b);
    printf("%s\n", fails ? "varispeedtest: FAILED" : "varispeedtest: all ok");
    return fails != 0;
}
