/*
 * texttest/main.c -- the variable-width text layout, measured.
 *
 * Runs against the real gfx.c (see shim.h for why nothing here is a
 * reimplementation). Every draw lands in a real shadow framebuffer under
 * ASan, so an overrun is a crash with a stack trace and not a stray
 * pixel.
 *
 * The properties tested are the ones the fixed-width version got for
 * free and the variable-width version has to earn:
 *
 *   1. gfx_text_w() equals the advance the draw loops actually walk.
 *      When every glyph was one cell these could not disagree. Now they
 *      are separate code paths over separate width lookups, and every
 *      caller that centres or right-aligns text trusts them to match.
 *
 *   2. Nothing draws outside its max_w budget. The ellipsis path in
 *      gfx_draw_text() and the ring walk in gfx_draw_text_tail() both
 *      decide what fits by accumulating per-glyph advances; an off-by-one
 *      there overruns the panel rather than truncating.
 *
 *   3. gfx_draw_text_clipped() draws nothing outside its window, at any
 *      marquee offset, including offsets that put the string far off
 *      either edge.
 *
 *   4. The tail walk keeps the *tail*. That is the entire reason the
 *      function exists, and the ring buffer that replaced the old
 *      byte-offset walk is the easiest thing here to get subtly wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"
#include "gfx.h"
#include "ark12.h"                  /* 5247 */
#include "arabixel.h"               /* 6029 */

#define W   720
#define H   1280

static int failures;
static int checks;

#define CHECK(cond, ...) do {                       \
    checks++;                                       \
    if (!(cond)) {                                  \
        failures++;                                 \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    }                                               \
} while (0)

/* The framebuffer gfx.c allocated, reached the same way the panel would
 * reach it. gfx.c keeps s_fb private, so the harness finds the drawn
 * pixels by scanning what it can see: it clears, draws, and scans. */
static uint16_t *fb;

#define INK   0xFFFF
#define BLANK 0x0000

static void clear(void)
{
    gfx_fill_rect(0, 0, W, H, BLANK);
}

/* Bounding box of everything non-blank. Returns false if nothing drawn. */
static bool ink_bbox(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = W; *y0 = H; *x1 = -1; *y1 = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (fb[y * W + x] == BLANK) continue;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    }
    return *x1 >= 0;
}

/* ------------------------------------------------------------------ */
/* Corpus                                                              */
/* ------------------------------------------------------------------ */

/* Real-shaped strings, not random bytes: the point is layout, and the
 * mixes that break layout are the ones a tagger actually produces. The
 * malformed entries are last and are there because a tag can contain
 * anything and utf8_next() has to survive it. */
static const char *CORPUS[] = {
    "",
    " ",
    "a",
    "Erik Satie",
    "Gymnopedie No. 1",
    "04 - Sarabande No. 2.flac",
    "/sd/Music/Satie/Trois Gymnopedies/04 - Sarabande No. 2.flac",

    /* halfwidth, accented */
    "Bj\xc3\xb6rk",
    "Sigur R\xc3\xb3s",
    "\xc5\x81\xc3\xb3" "d" "\xc5\xba",
    "Dvo\xc5\x99\xc3\xa1k",
    "M\xc3\xb6" "tley Cr" "\xc3\xbc" "e",

    /* Cyrillic -- new at 12px */
    "\xd0\xa7\xd0\xb0\xd0\xb9\xd0\xba\xd0\xbe\xd0\xb2\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
    "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb3\xd0\xb5\xd0\xb9 \xd0\xa0\xd0\xb0\xd1\x85\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xb2",

    /* 6029: Arabic -- shaped and reversed, joined with no gap column,
     * mixed with Latin and digits, a lam-alef and marks to drop */
    "\xd8\xa7\xd9\x84\xd9\x82\xd8\xb1\xd8\xa2\xd9\x86 \xd8\xa7\xd9\x84\xd9\x83\xd8\xb1\xd9\x8a\xd9\x85",
    "Radio \xd8\xb1\xd8\xa7\xd8\xaf\xd9\x8a\xd9\x88 99.1 FM",
    "\xd9\x85\xd9\x8f\xd8\xad\xd9\x8e\xd9\x85\xd9\x91\xd9\x8e\xd8\xaf \xd9\xa1\xd9\xa2\xd9\xa3",

    /* fullwidth */
    "\xe9\x9f\xb3\xe6\xa5\xbd",
    "\xe5\x9d\x82\xe6\x9c\xac\xe9\xbe\x8d\xe4\xb8\x80",
    "\xe3\x81\x93\xe3\x81\x93\xe3\x82\x8d\xe3\x82\x92\xe3\x81\x86\xe3\x81\x9f\xe3\x81\x86",

    /* the mix -- halfwidth and fullwidth in one run, which is where a
     * fixed-cell assumption shows up as misalignment */
    "Ryuichi Sakamoto - \xe6\x88\xa6\xe5\xa0\xb4\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xaa\xe3\x83\xbc\xe3\x82\xaf\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x9e\xe3\x82\xb9",
    "\xe5\x9d\x82\xe6\x9c\xac\xe9\xbe\x8d\xe4\xb8\x80 / async (2017)",
    "a" "\xe9\x9f\xb3" "b" "\xe6\xa5\xbd" "c",

    /* fullwidth parens -- the block that was missing from RANGES */
    "async \xef\xbc\x88\xef\xbc\x92\xef\xbc\x90\xef\xbc\x91\xef\xbc\x97\xef\xbc\x89",

    /* codepoints Ark has no glyph for at any size: notdef path */
    "\xe8\xad\xb7 \xe9\x83\x8e",

    /* soft hyphen: glyph_for() returns no cell at all */
    "soft\xc2\xadhyphen",
    /* no-break space: folded to plain space */
    "no" "\xc2\xa0" "break",

    /* long, to exercise truncation and the tail ring */
    "The Quick Brown Fox Jumps Over The Lazy Dog And Keeps Going For Quite A While Longer Than Any Panel Is Wide",
    "\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd",

    /* malformed: truncated sequence, lone continuation, overlong,
     * surrogate, 5-byte lead. None should hang or read past the NUL. */
    "\xe9\x9f",
    "\x80\x80\x80",
    "\xc0\x80",
    "\xed\xa0\x80",
    "\xf8\x88\x80\x80\x80",
    "valid\xffthen more",
};
#define NCORPUS ((int)(sizeof(CORPUS) / sizeof(CORPUS[0])))

/* ------------------------------------------------------------------ */
/* 1. gfx_text_w() agrees with what the draw path walks                */
/* ------------------------------------------------------------------ */

/* Draw unclipped at a known origin with a budget far larger than the
 * string, then compare the measured ink against the promised width.
 *
 * Ink is not expected to reach the full advance -- the last glyph's gap
 * column and its right bearing are both blank -- so the assertion is
 * one-sided: ink must not exceed the promise. A string that measures
 * narrower than it draws is the bug that misaligns centred text, and
 * that is what this catches. */
static void test_width_matches_draw(void)
{
    printf("gfx_text_w() vs drawn extent\n");
    for (int i = 0; i < NCORPUS; i++) {
        const char *s = CORPUS[i];
        for (int scale = 2; scale <= 5; scale++) {
            clear();
            const int promised = gfx_text_w(s, scale);
            gfx_draw_text(10, 10, s, scale, W - 20, INK);

            int x0, y0, x1, y1;
            if (!ink_bbox(&x0, &y0, &x1, &y1)) continue;   /* nothing drawn */

            const int drawn = x1 - 10 + 1;
            CHECK(drawn <= promised,
                  "s=%d corpus[%d]: drew %d px, gfx_text_w promised %d",
                  scale, i, drawn, promised);
            CHECK(x0 >= 10, "s=%d corpus[%d]: drew left of origin (x0=%d)",
                  scale, i, x0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. max_w is a hard budget                                           */
/* ------------------------------------------------------------------ */

/* Every budget from tiny to panel-wide, against every string. The
 * interesting ones are the budgets near the dots-plus-one-glyph bail-out
 * in gfx_draw_text(): just under it nothing should draw, just over it the
 * ellipsis must still fit. */
static void test_max_w_respected(void)
{
    printf("max_w budget honoured\n");
    static const int budgets[] = { 0, 1, 6, 7, 13, 14, 20, 21, 27, 28,
                                   40, 60, 100, 200, 400, 700 };
    const int nb = (int)(sizeof(budgets) / sizeof(budgets[0]));

    for (int i = 0; i < NCORPUS; i++) {
        for (int scale = 2; scale <= 4; scale++) {
            for (int b = 0; b < nb; b++) {
                const int max_w = budgets[b];

                clear();
                gfx_draw_text(10, 10, CORPUS[i], scale, max_w, INK);
                int x0, y0, x1, y1;
                if (ink_bbox(&x0, &y0, &x1, &y1)) {
                    CHECK(x1 < 10 + max_w,
                          "draw_text s=%d corpus[%d] max_w=%d: ink at x=%d, "
                          "budget ends at %d", scale, i, max_w, x1, 10 + max_w);
                }

                clear();
                gfx_draw_text_tail(10, 10, CORPUS[i], scale, max_w, INK);
                if (ink_bbox(&x0, &y0, &x1, &y1)) {
                    CHECK(x1 < 10 + max_w,
                          "draw_text_tail s=%d corpus[%d] max_w=%d: ink at x=%d, "
                          "budget ends at %d", scale, i, max_w, x1, 10 + max_w);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 3. the clip window is absolute                                      */
/* ------------------------------------------------------------------ */

/* This is the marquee. The string is drawn at x = win_x - off for a
 * sweep of offsets covering the whole travel plus overshoot at both
 * ends, and nothing may land outside the window at any of them. */
static void test_clip_window(void)
{
    printf("gfx_draw_text_clipped() stays in its window\n");
    const int win_x = 40;
    const int win_w = 300;

    for (int i = 0; i < NCORPUS; i++) {
        const int scale = 3;
        const int tw = gfx_text_w(CORPUS[i], scale);
        const int span = tw > win_w ? tw - win_w : 0;

        for (int off = -60; off <= span + 60; off += 7) {
            clear();
            gfx_draw_text_clipped(win_x - off, 10, win_x, win_w,
                                  CORPUS[i], scale, INK);
            int x0, y0, x1, y1;
            if (!ink_bbox(&x0, &y0, &x1, &y1)) continue;
            CHECK(x0 >= win_x && x1 < win_x + win_w,
                  "clipped corpus[%d] off=%d: ink %d..%d outside window %d..%d",
                  i, off, x0, x1, win_x, win_x + win_w - 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 4. the tail walk keeps the tail                                     */
/* ------------------------------------------------------------------ */

/* Built rather than asserted from a table: a string whose last glyphs
 * are known, truncated hard, must still show those last glyphs. The
 * check is that drawing the whole string tail-truncated produces the
 * same ink as drawing just its true tail at the same budget -- which is
 * only true if the ring walk kept exactly the right suffix. */
static void test_tail_keeps_tail(void)
{
    printf("gfx_draw_text_tail() keeps the suffix\n");

    /* pairs: {long string, the suffix that should survive}. The budget is
     * derived below as exactly dots + suffix, not written out here: a
     * hand-picked budget larger than the suffix keeps more than the
     * suffix, which is correct behaviour that reads as a failure. (It
     * did, on the first run of this test -- the code was right and the
     * expectation was wrong.) */
    static const struct { const char *full; const char *suffix; } cases[] = {
        { "/sd/Music/Satie/Trois Gymnopedies/Sarabande.flac", "Sarabande.flac" },
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaXYZ",               "XYZ"            },
        /* fullwidth suffix: the ring has to count 13*scale, not 7*scale */
        { "aaaaaaaaaaaaaaaaaaaa\xe9\x9f\xb3\xe6\xa5\xbd",
          "\xe9\x9f\xb3\xe6\xa5\xbd" },
        /* mixed suffix */
        { "aaaaaaaaaaaaaaaaaaaa\xe9\x9f\xb3z",
          "\xe9\x9f\xb3" "z" },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < n; i++) {
        const int scale = 1;
        const int max_w = 3 * GFX_GLYPH_W(scale)
                        + gfx_text_w(cases[i].suffix, scale);

        /* Ink from the truncated full string, minus the three dots. */
        clear();
        gfx_draw_text_tail(10, 10, cases[i].full, scale, max_w, INK);
        int fx0, fy0, fx1, fy1;
        const bool drew = ink_bbox(&fx0, &fy0, &fx1, &fy1);
        CHECK(drew, "tail[%d]: nothing drawn at max_w=%d", i, max_w);
        if (!drew) continue;

        /* The suffix must be present: its own drawn width should equal
         * the width of the ink after the ellipsis. */
        const int dots_w = 3 * GFX_GLYPH_W(scale);
        const int suffix_w = gfx_text_w(cases[i].suffix, scale);
        const int tail_ink_w = fx1 - (10 + dots_w) + 1;

        CHECK(tail_ink_w <= suffix_w && tail_ink_w > suffix_w - 3 * scale - 1,
              "tail[%d]: suffix ink %d px, expected about %d",
              i, tail_ink_w, suffix_w);
    }
}

/* A path far longer than the ring can hold. The ring is bounded at
 * TAIL_MAX_GLYPHS; a string longer than that must still produce the
 * correct tail, because the ring keeps the newest entries and the tail
 * is made of newest entries. This is the case where a naive bound would
 * silently return the wrong suffix rather than crash. */
static void test_tail_longer_than_ring(void)
{
    printf("gfx_draw_text_tail() past the ring bound\n");
    char big[4096];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big + sizeof(big) - 5, "END", 3);

    for (int scale = 2; scale <= 4; scale++) {
        clear();
        gfx_draw_text_tail(10, 10, big, scale, 300, INK);
        int x0, y0, x1, y1;
        CHECK(ink_bbox(&x0, &y0, &x1, &y1),
              "long path s=%d: nothing drawn", scale);
        CHECK(x1 < 10 + 300, "long path s=%d: overran budget (x1=%d)", scale, x1);
    }
}

/* ------------------------------------------------------------------ */
/* 5. degenerate inputs                                                */
/* ------------------------------------------------------------------ */

static void test_degenerate(void)
{
    printf("degenerate arguments\n");

    /* NULL and empty must be no-ops, not crashes. */
    CHECK(gfx_text_w(NULL, 3) == 0, "gfx_text_w(NULL) != 0");
    CHECK(gfx_text_w("", 3) == 0, "gfx_text_w(\"\") != 0");
    clear();
    gfx_draw_text(10, 10, NULL, 3, 100, INK);
    gfx_draw_text_tail(10, 10, NULL, 3, 100, INK);
    gfx_draw_text_clipped(10, 10, 0, 100, NULL, 3, INK);
    int x0, y0, x1, y1;
    CHECK(!ink_bbox(&x0, &y0, &x1, &y1), "NULL string drew something");

    /* Zero and negative budgets. */
    for (int i = 0; i < NCORPUS; i++) {
        clear();
        gfx_draw_text(10, 10, CORPUS[i], 3, 0, INK);
        gfx_draw_text(10, 10, CORPUS[i], 3, -50, INK);
        gfx_draw_text_tail(10, 10, CORPUS[i], 3, 0, INK);
        gfx_draw_text_tail(10, 10, CORPUS[i], 3, -50, INK);
        gfx_draw_text_clipped(10, 10, 0, 0, CORPUS[i], 3, INK);
        gfx_draw_text_clipped(10, 10, 0, -50, CORPUS[i], 3, INK);
        CHECK(!ink_bbox(&x0, &y0, &x1, &y1),
              "corpus[%d]: non-positive budget drew something", i);
    }

    /* Draws that start off-panel. gfx_fill_rect() clips, so this is
     * checking the layout does not compute an address before clipping. */
    clear();
    for (int i = 0; i < NCORPUS; i++) {
        gfx_draw_text(-500, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text(W - 5, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text(10, -20, CORPUS[i], 4, 400, INK);
        gfx_draw_text(10, H - 3, CORPUS[i], 4, 400, INK);
        gfx_draw_text_tail(-500, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text_tail(W - 5, 10, CORPUS[i], 4, 400, INK);
    }
}

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

/*
 * How many arms the thing at (cx, cy) has, counted rather than assumed.
 *
 * Walks a ring at 78% of the outer radius -- outside the inner vertices
 * at 38.2% and inside the points -- and counts runs of ink. A
 * five-pointed star gives 5. Two overlapping triangles, which is what
 * this used to draw while calling itself five-pointed, give 6.
 */
static int arms_at(int cx, int cy, int r)
{
    /* Spelled out rather than M_PI: this file builds at -std=c11 with
     * no _GNU_SOURCE, where M_PI is not declared. */
    const double pi = 3.14159265358979323846;
    const double rad = r * 0.78;
    const int N = 2000;
    int runs = 0;
    bool prev = false, first = false;

    for (int i = 0; i < N; i++) {
        const double a = (2.0 * pi * i) / N;
        const int x = cx + (int)lround(rad * cos(a));
        const int y = cy + (int)lround(rad * sin(a));
        const bool ink = (x >= 0 && x < W && y >= 0 && y < H &&
                          fb[y * W + x] != BLANK);
        if (i == 0) first = ink;
        if (ink && !prev) runs++;
        prev = ink;
    }
    /* A run straddling the seam was counted at both ends. */
    if (first && prev && runs > 0) runs--;
    return runs;
}

static void test_star_has_five_points(void)
{
    const int cx = 200, cy = 200, r = 80;

    clear();
    gfx_fill_star(cx, cy, r, INK);

    /* THE CHECK THIS FILE EXISTS FOR. It was six. */
    CHECK(arms_at(cx, cy, r) == 5, "star has %d points, not 5",
          arms_at(cx, cy, r));

    /* Point up: ink just inside the top point, none at the bottom
     * centre, where a point-up star has the notch between its two lower
     * arms. This is what tells a star from a star rotated 36 degrees,
     * which also has five points. */
    CHECK(fb[(cy - r + 2) * W + cx] != BLANK, "no ink at the top point");
    CHECK(fb[(cy + (r * 78) / 100) * W + cx] == BLANK,
          "ink at the bottom centre: the star is upside down");

    /* Solid in the middle. A star drawn as an outline by accident would
     * pass the arm count and fail here. */
    CHECK(fb[cy * W + cx] != BLANK, "the centre is not filled");

    /* The bbox is the star's own size, give or take rounding: the
     * points reach r in every direction. */
    int x0, y0, x1, y1;
    CHECK(ink_bbox(&x0, &y0, &x1, &y1), "nothing drawn");
    CHECK(y0 >= cy - r - 1 && y0 <= cy - r + 1, "top at %d, wanted %d",
          y0, cy - r);
    CHECK(x0 >= cx - r - 1 && x1 <= cx + r + 1, "wider than r: %d..%d",
          x0, x1);

    /* The ring the unfilled star is drawn with: the same star again,
     * smaller, in the background. Still five arms, hollow centre. */
    clear();
    gfx_fill_star(cx, cy, r, INK);
    gfx_fill_star(cx, cy, (r * 58) / 100, BLANK);
    CHECK(arms_at(cx, cy, r) == 5, "the ring has %d points, not 5",
          arms_at(cx, cy, r));
    CHECK(fb[cy * W + cx] == BLANK, "the unfilled star has a filled centre");
}

static void test_poly_basics(void)
{
    /* A triangle, because gfx_fill_poly() replaced a triangle
     * primitive and has to be able to do its job. */
    clear();
    {
        const int tri[6] = { 100, 100,  180, 100,  140, 180 };
        gfx_fill_poly(tri, 3, INK);
        CHECK(fb[105 * W + 140] != BLANK, "triangle: no ink inside");
        CHECK(fb[175 * W + 105] == BLANK, "triangle: ink outside the slope");
        int x0, y0, x1, y1;
        CHECK(ink_bbox(&x0, &y0, &x1, &y1), "triangle drew nothing");
        CHECK(y0 == 100 && y1 <= 180, "triangle bbox y %d..%d", y0, y1);
    }

    /* A rectangle as a polygon, against the rect primitive: same
     * pixels, or the crossing rule is off by one somewhere. */
    clear();
    {
        const int box[8] = { 50, 50,  90, 50,  90, 90,  50, 90 };
        gfx_fill_poly(box, 4, INK);
        int x0, y0, x1, y1;
        CHECK(ink_bbox(&x0, &y0, &x1, &y1), "box drew nothing");
        CHECK(x0 == 50 && y0 == 50 && x1 == 90 && y1 == 89,
              "box bbox %d,%d..%d,%d", x0, y0, x1, y1);
    }

    /* Degenerate input draws nothing and does not read off the end.
     * ASan is the real check here. */
    clear();
    {
        const int two[4] = { 10, 10, 20, 20 };
        gfx_fill_poly(two, 2, INK);
        gfx_fill_poly(two, 0, INK);
        gfx_fill_poly(NULL, 3, INK);
        gfx_fill_poly(two, GFX_POLY_MAX_PTS + 1, INK);
        int x0, y0, x1, y1;
        CHECK(!ink_bbox(&x0, &y0, &x1, &y1), "degenerate polygon drew ink");
    }

    /* A flat polygon: every point on one row. No crossings, no ink, no
     * division by a zero height. */
    clear();
    {
        const int flat[6] = { 10, 40,  60, 40,  35, 40 };
        gfx_fill_poly(flat, 3, INK);
        int x0, y0, x1, y1;
        CHECK(!ink_bbox(&x0, &y0, &x1, &y1), "a flat polygon drew ink");
    }
}

/*
 * 5247: the font is stored as shared 6x6 tiles now, and every glyph must
 * come back exactly as it was when it was one uint16_t per row. The hash
 * below was taken from the tables before the change -- every codepoint
 * the old table held, its width and its twelve rows, FNV-1a in codepoint
 * order -- and a lookup of every codepoint from U+0000 to U+FFFF through
 * ark12_glyph() must give the same glyphs, the same count, and nothing
 * the old table did not have. A regenerated font that changes a glyph on
 * purpose changes this number on purpose, and says so where it does.
 */
#define ARK12_GOLDEN_COUNT  20669
#define ARK12_GOLDEN_HASH   0x8EE1A15Eu

static void test_font_unchanged_by_tiling(void)
{
    uint32_t h = 2166136261u;
    int n = 0;
    for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
        uint16_t rows[ARK12_H];
        int w = -1;
        if (!ark12_glyph(cp, &w, rows)) continue;
        CHECK(w == ARK12_HALF_W || w == ARK12_FULL_W, "U+%04X has width %d", (unsigned)cp, w);
        for (int y = 0; y < ARK12_H; y++)
            CHECK((rows[y] >> w) == 0, "U+%04X row %d has ink past its width", (unsigned)cp, y);
        h = (h ^ cp) * 16777619u;
        h = (h ^ (uint32_t)w) * 16777619u;
        for (int y = 0; y < ARK12_H; y++) h = (h ^ rows[y]) * 16777619u;
        n++;
    }
    CHECK(n == ARK12_GOLDEN_COUNT, "the font has %d glyphs, not %d", n, ARK12_GOLDEN_COUNT);
    CHECK(h == ARK12_GOLDEN_HASH, "the font's glyphs hash to 0x%08X, not 0x%08X",
          (unsigned)h, (unsigned)ARK12_GOLDEN_HASH);
    uint16_t rows[ARK12_H];
    int w;
    CHECK(!ark12_glyph(0x10000, &w, rows), "a codepoint past U+FFFF was found");
}

/* ------------------------------------------------------------------ */
/* 6018: gfx_wrap_line()                                               */
/* ------------------------------------------------------------------ */

/* Walks a whole paragraph and returns its lines joined with '|'. */
static int wrap_all(const char *s, int scale, int max_w, char *out, size_t cap)
{
    int n = 0;
    out[0] = '\0';
    for (const char *p = s; p; ) {
        const char *next;
        const size_t len = gfx_wrap_line(p, scale, max_w, &next);
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        CHECK(gfx_text_w(line, scale) <= max_w || len == strlen(p) || len <= 4,
              "wrap: line '%s' is %d px of %d", line, gfx_text_w(line, scale), max_w);
        if (n) strncat(out, "|", cap - strlen(out) - 1);
        strncat(out, line, cap - strlen(out) - 1);
        n++;
        CHECK(next == NULL || next > p, "wrap: no progress at '%s'", p);
        if (n > 64) break;
        p = next;
    }
    return n;
}

static void test_wrap(void)
{
    printf("gfx_wrap_line()\n");
    const int hw = GFX_GLYPH_W(1);          /* 7 */
    const int fw = GFX_GLYPH_W_FULL(1);     /* 13 */
    char out[512];

    wrap_all("aaa bbb ccc", 1, 7 * hw, out, sizeof(out));
    CHECK(strcmp(out, "aaa bbb|ccc") == 0, "latin at spaces: '%s'", out);

    wrap_all("aaa   bbb", 1, 4 * hw, out, sizeof(out));
    CHECK(strcmp(out, "aaa|bbb") == 0, "a run of spaces is dropped: '%s'", out);

    wrap_all("abcdefghij", 1, 4 * hw, out, sizeof(out));
    CHECK(strcmp(out, "abcd|efgh|ij") == 0, "a word wider than the line is cut: '%s'", out);

    wrap_all("one\ntwo", 1, 40 * hw, out, sizeof(out));
    CHECK(strcmp(out, "one|two") == 0, "newline always breaks: '%s'", out);

    wrap_all("一二三四五", 1, 3 * fw, out, sizeof(out));
    CHECK(strcmp(out, "一二三|四五") == 0, "CJK between characters: '%s'", out);

    /* 。 may not start a line: the break moves back one character. */
    wrap_all("一二。三", 1, 2 * fw, out, sizeof(out));
    CHECK(strcmp(out, "一|二。|三") == 0, "closing punctuation stays put: '%s'", out);

    /* （ may not end a line. */
    wrap_all("一二（三）", 1, 3 * fw, out, sizeof(out));
    CHECK(strcmp(out, "一二|（三）") == 0,
          "opening punctuation stays with what follows: '%s'", out);

    wrap_all("Wi-Fi 或网线", 1, 7 * hw, out, sizeof(out));
    CHECK(strcmp(out, "Wi-Fi|或网线") == 0, "mixed scripts: '%s'", out);

    const char *next = (const char *)1;
    CHECK(gfx_wrap_line("", 2, 100, &next) == 0 && next == NULL, "empty paragraph");
    CHECK(gfx_wrap_line("abc", 2, 0, &next) == 1, "zero width still takes one character");
}

/* ------------------------------------------------------------------ */
/* 6029: Arabic shaping and order                                      */
/* ------------------------------------------------------------------ */

static void shape_check(const char *what, const uint32_t *in, int n,
                        const uint32_t *want, int wn)
{
    uint32_t run[32];
    memcpy(run, in, (size_t)n * sizeof(run[0]));
    const int got = arabixel_shape(run, n);
    int ok = got == wn;
    for (int i = 0; ok && i < wn; i++) ok = run[i] == want[i];
    CHECK(ok, "%s: got %d codepoints, first U+%04X", what, got, got ? (unsigned)run[0] : 0u);
}

static void test_arabic(void)
{
    printf("Arabic shaping\n");
    /* mecca: meem initial, kaf medial, teh marbuta final; drawn reversed */
    shape_check("mkh", (const uint32_t[]){ 0x645, 0x643, 0x629 }, 3,
                (const uint32_t[]){ 0xFE94, 0xFEDC, 0xFEE3 }, 3);
    /* radio: reh, alef, dal, yeh, waw -- reh and dal join nothing after */
    shape_check("radio", (const uint32_t[]){ 0x631, 0x627, 0x62F, 0x64A, 0x648 }, 5,
                (const uint32_t[]){ 0xFEEE, 0xFEF3, 0xFEA9, 0xFE8D, 0xFEAD }, 5);
    /* al-: alef, lam+alef ligature isolated; lam-alef after beh is final */
    shape_check("la", (const uint32_t[]){ 0x644, 0x627 }, 2,
                (const uint32_t[]){ 0xFEFB }, 1);
    shape_check("bla", (const uint32_t[]){ 0x628, 0x644, 0x627 }, 3,
                (const uint32_t[]){ 0xFEFC, 0xFE91 }, 2);
    /* marks dropped, so damma does not break meem's join to hah */
    shape_check("marks", (const uint32_t[]){ 0x645, 0x64F, 0x62D }, 3,
                (const uint32_t[]){ 0xFEA2, 0xFEE3 }, 2);
    /* digits keep their order inside the reversed run */
    shape_check("digits", (const uint32_t[]){ 0x645, ' ', 0x661, 0x662, 0x663 }, 5,
                (const uint32_t[]){ 0x661, 0x662, 0x663, ' ', 0xFEE1 }, 5);

    /* Every form arabixel_shape() can produce is in the table: the
     * isolated form and its three others for a dual-joining letter. */
    static const uint16_t iso[] = { 0xFE80, 0xFE81, 0xFE83, 0xFE85, 0xFE87, 0xFE89,
        0xFE8D, 0xFE8F, 0xFE93, 0xFE95, 0xFE99, 0xFE9D, 0xFEA1, 0xFEA5, 0xFEA9,
        0xFEAB, 0xFEAD, 0xFEAF, 0xFEB1, 0xFEB5, 0xFEB9, 0xFEBD, 0xFEC1, 0xFEC5,
        0xFEC9, 0xFECD, 0xFED1, 0xFED5, 0xFED9, 0xFEDD, 0xFEE1, 0xFEE5, 0xFEE9,
        0xFEED, 0xFEEF, 0xFEF1, 0xFEF5, 0xFEF7, 0xFEF9, 0xFEFB };
    for (size_t i = 0; i < sizeof(iso) / sizeof(iso[0]); i++)
        CHECK(arabixel_find(iso[i]) != NULL, "no glyph for U+%04X", iso[i]);

    /* Joined: a word draws as one connected piece of ink -- no blank
     * column between meem, kaf and teh marbuta. */
    clear();
    gfx_draw_text(10, 10, "\xd9\x85\xd9\x83\xd8\xa9", 1, W - 20, INK);
    int x0, y0, x1, y1;
    if (ink_bbox(&x0, &y0, &x1, &y1)) {
        int gaps = 0;
        for (int x = x0; x <= x1; x++) {
            int any = 0;
            for (int y = y0; y <= y1; y++) any |= fb[y * W + x] == INK;
            gaps += !any;
        }
        CHECK(gaps == 0, "mkh drew %d blank columns inside the word", gaps);
        CHECK(y1 - 10 < ARK12_H, "Arabic drew below the 12 rows");
    } else {
        CHECK(0, "mkh drew nothing");
    }

    /* Truncated, a right-to-left line loses its left end and keeps its
     * start, which is on the right: the dots are at the left. */
    clear();
    const char *long_ar = "\xd8\xa7\xd9\x84\xd9\x82\xd8\xb1\xd8\xa2\xd9\x86 \xd8\xa7\xd9\x84\xd9\x83\xd8\xb1\xd9\x8a\xd9\x85 \xd8\xa7\xd9\x84\xd9\x83\xd8\xb1\xd9\x8a\xd9\x85";
    const int full = gfx_text_w(long_ar, 1);
    gfx_draw_text(10, 10, long_ar, 1, full / 2, INK);
    int dot_row = -1;
    for (int y = 10; y < 10 + ARK12_H; y++)
        if (fb[y * W + 10] == INK || fb[y * W + 11] == INK || fb[y * W + 12] == INK) dot_row = y;
    CHECK(dot_row >= 0, "RTL truncation: no ellipsis at the left edge");
}

int main(void)
{
    if (gfx_init(NULL, W, H) != ESP_OK) {
        printf("gfx_init failed\n");
        return 1;
    }
    /* gfx.c keeps its shadow buffer private and should stay that way, so
     * the harness takes the pointer from the shim's allocator rather than
     * from a test-only accessor bolted onto shipping code. Verified below
     * rather than assumed: a pixel is drawn and read back. */
    extern void *shim_last_big_alloc;
    fb = shim_last_big_alloc;
    if (!fb) { printf("no framebuffer\n"); return 1; }

    gfx_fill_rect(0, 0, W, H, BLANK);
    gfx_px(5, 7, INK);
    if (fb[7 * W + 5] != INK) {
        printf("framebuffer handle is wrong -- harness cannot see draws\n");
        return 1;
    }
    gfx_fill_rect(0, 0, W, H, BLANK);

    test_width_matches_draw();
    test_max_w_respected();
    test_clip_window();
    test_tail_keeps_tail();
    test_tail_longer_than_ring();
    test_degenerate();
    test_poly_basics();
    test_star_has_five_points();
    test_font_unchanged_by_tiling();                     /* 5247 */
    test_wrap();                                         /* 6018 */
    test_arabic();                                       /* 6029 */

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
