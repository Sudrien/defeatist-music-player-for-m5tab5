/*
 * artplacetest.c -- artplace.h: which axis, how far, and never off an edge.
 *
 * The properties here are written from what the feature must not do --
 * leave the box, move between repaints of one track, pick the axis with no
 * room -- rather than from what the arithmetic happens to produce, which is
 * texttest/README.md's rule. The bounds are the ones worth having: a cover
 * placed one pixel outside its box writes into whatever is beside it, and
 * the band sits next to the transport bar.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/artplace.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* The two real band shapes, from albumart.c's own comment: portrait is
 * the full 720 width, landscape is a 560-wide column. */
#define PORTRAIT_W  720
#define PORTRAIT_H  560
#define LAND_W      560
#define LAND_H      720

int main(void)
{
    /* ---- the axis is the one with the room ------------------------- */
    {
        /* Portrait: a square cover fits the height, so the room is at the
         * sides and the offset must be horizontal. */
        for (uint32_t seed = 1; seed <= 200; seed++) {
            const artplace_t p = artplace(PORTRAIT_W, PORTRAIT_H, 560, 560, seed);
            CHECK(p.horizontal, "portrait seed %u went vertical", seed);
            CHECK(p.dy == 0, "portrait seed %u moved dy to %d", seed, p.dy);
        }

        /* Landscape: the column is taller than wide, so the room is above
         * and below and the offset must be vertical. */
        for (uint32_t seed = 1; seed <= 200; seed++) {
            const artplace_t p = artplace(LAND_W, LAND_H, 560, 560, seed);
            CHECK(!p.horizontal, "landscape seed %u went horizontal", seed);
            CHECK(p.dx == 0, "landscape seed %u moved dx to %d", seed, p.dx);
        }
    }

    /* ---- never off an edge ----------------------------------------- */
    {
        /* Over both band shapes, a spread of cover sizes and many seeds:
         * the placed rectangle stays wholly inside the box. This is the
         * check that matters -- one pixel outside and the write lands in
         * the transport bar. */
        const int boxes[2][2] = { { PORTRAIT_W, PORTRAIT_H },
                                  { LAND_W, LAND_H } };
        for (int b = 0; b < 2; b++) {
            const int bw = boxes[b][0], bh = boxes[b][1];
            for (int cw = 1; cw <= bw; cw += 37) {
                for (int ch = 1; ch <= bh; ch += 41) {
                    for (uint32_t seed = 0; seed < 64; seed++) {
                        const artplace_t p = artplace(bw, bh, cw, ch, seed);
                        CHECK(p.dx >= 0 && p.dy >= 0,
                              "box %dx%d cover %dx%d seed %u: negative "
                              "origin %d,%d", bw, bh, cw, ch, seed, p.dx, p.dy);
                        CHECK(p.dx + cw <= bw,
                              "box %dx%d cover %dx%d seed %u: runs %d px "
                              "past the right edge",
                              bw, bh, cw, ch, seed, p.dx + cw - bw);
                        CHECK(p.dy + ch <= bh,
                              "box %dx%d cover %dx%d seed %u: runs %d px "
                              "past the bottom edge",
                              bw, bh, cw, ch, seed, p.dy + ch - bh);
                    }
                }
            }
        }
    }

    /* ---- a cover that exactly fills its box does not move ---------- */
    {
        for (uint32_t seed = 0; seed < 100; seed++) {
            const artplace_t p = artplace(560, 560, 560, 560, seed);
            CHECK(p.dx == 0 && p.dy == 0, "an exact fit moved to %d,%d",
                  p.dx, p.dy);
            CHECK(p.shift == 0, "an exact fit reported a shift of %d", p.shift);
        }

        /* And one bigger than its box is clamped to the origin rather
         * than given a negative one. */
        const artplace_t big = artplace(560, 560, 800, 800, 12345);
        CHECK(big.dx == 0 && big.dy == 0, "an oversize cover went to %d,%d",
              big.dx, big.dy);
    }

    /* ---- the same track lands in the same place -------------------- */
    {
        /* The whole point of seeding per track: closing the chooser or
         * dismissing a card repaints the cover, and it must not jump. */
        for (uint32_t seed = 1; seed <= 50; seed++) {
            const artplace_t a = artplace(PORTRAIT_W, PORTRAIT_H, 500, 500, seed);
            const artplace_t b = artplace(PORTRAIT_W, PORTRAIT_H, 500, 500, seed);
            CHECK(a.dx == b.dx && a.dy == b.dy,
                  "seed %u placed twice at %d,%d then %d,%d",
                  seed, a.dx, a.dy, b.dx, b.dy);
        }
    }

    /* ---- different tracks land in different places ----------------- */
    {
        /*
         * Not a distribution test -- just that the seed reaches the
         * result. A constant offset would satisfy every bound above and
         * every repaint check, and would be invisible in a log.
         */
        int seen[PORTRAIT_W];
        memset(seen, 0, sizeof(seen));
        int distinct = 0;
        for (uint32_t seed = 1; seed <= 400; seed++) {
            const artplace_t p = artplace(PORTRAIT_W, PORTRAIT_H, 460, 460, seed);
            if (p.dx >= 0 && p.dx < PORTRAIT_W && !seen[p.dx]++) distinct++;
        }
        CHECK(distinct > 20, "400 seeds produced only %d distinct positions",
              distinct);
    }

    /* ---- the offset keeps a margin on both sides ------------------- */
    {
        /*
         * ARTPLACE_MAX_PM is 400 per mille, so the cover sits between 10%
         * and 90% of the way across the slack and is never flush against
         * an edge. Flush reads as a layout fault rather than as a print
         * set down casually, which is the whole intent.
         */
        const int slack = PORTRAIT_W - 460;          /* 260 */
        for (uint32_t seed = 1; seed <= 500; seed++) {
            const artplace_t p = artplace(PORTRAIT_W, PORTRAIT_H, 460, 460, seed);
            CHECK(p.dx > 0, "seed %u put the cover flush left", seed);
            CHECK(p.dx < slack, "seed %u put the cover flush right", seed);
        }
    }

    /* ---- both axes with room: the larger one wins ------------------- */
    {
        /*
         * Only reachable through the whole-pixel enlargement of a small
         * picture, which can leave it short of the box in both directions.
         * One offset, not two -- two reads as a picture dropped in a
         * corner.
         */
        for (uint32_t seed = 1; seed <= 100; seed++) {
            /* 300 wide in a 720 box (420 spare), 400 tall in a 560 box
             * (160 spare): horizontal has more room. */
            const artplace_t p = artplace(720, 560, 300, 400, seed);
            CHECK(p.horizontal, "seed %u nudged the tighter axis", seed);
            CHECK(p.dy == (560 - 400) / 2,
                  "seed %u moved dy off centre to %d as well", seed, p.dy);
        }
        for (uint32_t seed = 1; seed <= 100; seed++) {
            /* The other way round: 500 wide in 560 (60 spare), 200 tall
             * in 720 (520 spare). */
            const artplace_t p = artplace(560, 720, 500, 200, seed);
            CHECK(!p.horizontal, "seed %u nudged the tighter axis", seed);
            CHECK(p.dx == (560 - 500) / 2,
                  "seed %u moved dx off centre to %d as well", seed, p.dx);
        }
    }

    /* ---- a slack too small to divide ------------------------------- */
    {
        for (int slack = 0; slack <= 2; slack++) {
            for (uint32_t seed = 0; seed < 32; seed++) {
                const artplace_t p = artplace(560 + slack, 560, 560, 560, seed);
                CHECK(p.shift == 0, "slack %d was nudged by %d",
                      slack, p.shift);
                CHECK(p.dx == slack / 2, "slack %d placed at %d", slack, p.dx);
            }
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
