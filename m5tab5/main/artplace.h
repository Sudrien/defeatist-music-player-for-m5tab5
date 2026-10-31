/*
 * artplace.h -- where the cover sits in its box, a little off centre.
 *
 * The companion to the keystone (albumart.c): that leans the sleeve, this
 * puts it down somewhere other than dead centre, so a cover looks propped
 * rather than laid out. Both are drawn once when the picture is placed and
 * neither animates.
 *
 * ONE AXIS, AND IT IS THE ONE THE BAND LEAVES SPARE. The fit makes the
 * picture fill its box in one direction exactly, so the room is on the
 * other -- in portrait the art band is wider than it is tall and a square
 * cover leaves room at the sides; in landscape it is a tall column and the
 * room is above and below. "Horizontal in portrait, vertical in landscape"
 * is therefore not a rule this file knows: it falls out of taking whichever
 * axis has the slack, and it keeps being right if the bands are ever
 * reshaped.
 *
 * SEEDED PER TRACK, not per picture, because albumart.c hands in
 * s_keystone_seed and that is the track's key when there is one. Every
 * track on an album sits its own way, and a repaint of the same track --
 * closing the chooser, a card going away -- puts it back in the same
 * place. A different multiplier from the lean's, so the two are not one
 * coin read twice: a cover that always leaned left AND sat left would look
 * like a single mistake rather than two independent choices.
 *
 * Header-only and pure, so texttest compiles it. It does no drawing, reads
 * no globals and logs nothing -- albumart.c logs what it returns.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARTPLACE 0 leaves the cover centred, which is what it was before this
 * existed and what a board with a complaint about it should be built
 * with to check.
 */
#ifndef ARTPLACE
#define ARTPLACE            1
#endif

/*
 * How far off centre, as a fraction of the slack, either side.
 *
 * 400 per mille means the picture sits somewhere between 10% and 90% of
 * the way across the spare room, so there is always a margin on both
 * sides. At 500 it could finish flush against one edge, which stops
 * looking casual and starts looking like a layout fault -- the same
 * reasoning that caps the keystone at 8%.
 */
#define ARTPLACE_MAX_PM     (400)

/*
 * At or below 500 per mille the nudge cannot take the cover out of its
 * box: the centred origin is slack/2 and the shift is at most
 * slack * pm / 1000, so the result stays within [0, slack]. The clamps
 * below are therefore UNREACHABLE while this holds, and they are kept
 * anyway -- they cost two comparisons once per cover and they are what
 * makes the bound true rather than merely likely.
 *
 * Said out loud because three deliberate bugs planted in this file
 * survived the test suite: removing either clamp, and removing the
 * small-slack early-out. All three are dead code at pm 400, so the
 * suite was right to pass and the mutations proved nothing. This assert
 * is what actually holds the property, and a future session raising the
 * constant past 500 will be stopped here rather than by a cover drawn
 * over the transport bar.
 */
_Static_assert(ARTPLACE_MAX_PM <= 500,
               "a larger nudge can place the cover outside its box");

typedef struct {
    int  dx, dy;        /* where to put the cover's top-left in the box */
    int  shift;         /* what was added, signed; 0 when centred */
    bool horizontal;    /* which axis `shift` was applied to */
    int  slack;         /* the room that axis had */
} artplace_t;

/*
 * The signed nudge for one axis, within +/- ARTPLACE_MAX_PM of its slack.
 * 0 for a slack too small to divide -- which `m <= 0` already answers at
 * pm 400, so the explicit `slack <= 2` is the same belt-and-braces the
 * assert above describes.
 */
static inline int artplace_nudge(int slack, uint32_t bits)
{
    if (slack <= 2) return 0;
    const int m = (int)(((int64_t)slack * ARTPLACE_MAX_PM) / 1000);
    if (m <= 0) return 0;
    return (int)(bits % (uint32_t)(2 * m + 1)) - m;
}

/*
 * Centre the fitted cover in the box, then nudge it along whichever axis
 * has the room.
 *
 * Both axes can have room at once, but only through the whole-pixel
 * enlargement of a small picture (art_int_scale()), which can leave it
 * short of the box in both directions. The larger slack wins there rather
 * than both being nudged: two offsets read as a picture dropped in a
 * corner, where one reads as a print set down a little off centre.
 *
 * The result is always inside the box -- 0 <= dx <= box_w - cw, and the
 * same for dy -- so a caller that trusted the old centring cannot be
 * handed a rectangle that runs off an edge.
 */
static inline artplace_t artplace(int box_w, int box_h, int cw, int ch,
                                  uint32_t seed)
{
    artplace_t p;
    const int sx = box_w - cw > 0 ? box_w - cw : 0;
    const int sy = box_h - ch > 0 ? box_h - ch : 0;

    p.dx = sx / 2;
    p.dy = sy / 2;
    p.shift = 0;
    p.horizontal = (sx >= sy);
    p.slack = p.horizontal ? sx : sy;

#if ARTPLACE
    const uint32_t h = seed * 2246822519u;      /* not the keystone's */
    const int n = artplace_nudge(p.slack, h >> 7);
    if (n) {
        int *const axis = p.horizontal ? &p.dx : &p.dy;
        *axis += n;
        if (*axis < 0) *axis = 0;
        if (*axis > p.slack) *axis = p.slack;
        p.shift = n;
    }
#endif
    return p;
}

#ifdef __cplusplus
}
#endif
