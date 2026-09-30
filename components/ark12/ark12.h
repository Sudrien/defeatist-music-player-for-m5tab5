/*
 * ark12.h -- Ark Pixel Font, 12px, as deduplicated 6x6 tiles.
 *
 * GENERATED FILE. Do not edit. Regenerate with:
 *     ./tools/gen_ark12.py
 * from ark-pixel-font @ a local checkout.
 *
 * Ark Pixel Font
 * https://github.com/TakWolf/ark-pixel-font
 * Copyright (c) 2021, TakWolf (https://takwolf.com).
 * Licensed under the SIL Open Font License, Version 1.1.
 *
 * NOTE ON LICENSING: the rest of this project is MIT. This file is not,
 * and cannot be. It is a format conversion of OFL-licensed Font Software,
 * which makes it a Modified Version under the OFL, and OFL section 5
 * requires Modified Versions to stay under the OFL. The full text is in
 * components/ark12/LICENSE-OFL and must ship with any redistribution of
 * this file or of a binary containing it. Ark Pixel declares no Reserved
 * Font Name, so this derivative does not have to be renamed -- but it
 * also must not be sold on its own, and this header must stay attached.
 *
 * Every glyph is 12 rows tall and either 6px (Latin and its
 * relatives -- one cell) or 12px (CJK-adjacent scripts and fullwidth forms -- two cells, the
 * same shape a CJK terminal font calls fullwidth) wide. ark12_glyph()
 * hands back a glyph as 12 rows, each one value with bit 0 leftmost --
 * the shape gfx.c draws from.
 *
 * STORED AS TILES (5247). A full-width glyph is four 6x6 tiles (top
 * left, top right, bottom left, bottom right) and a half-width one two
 * (top, bottom). Across the font 81614 tiles are only 33735
 * distinct -- blank corners, shared radicals, repeated strokes -- so each
 * distinct tile is stored once, 36 bits packed end to end in
 * ark12_tiles, and a glyph is its tiles' numbers in ark12_tix. The
 * codepoints are runs of consecutive glyphs of one width (ark12_runs),
 * so there is no table per glyph at all: a glyph's tile numbers start
 * at its run's `tix` plus its place in the run times its tile count.
 * 341268 bytes, where one uint16_t row per scanline and a codepoint and
 * a width per glyph took 558063.
 *
 * SPDX-License-Identifier: OFL-1.1
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARK12_H       12
#define ARK12_HALF_W  6
#define ARK12_FULL_W  12
#define ARK12_COUNT   20669
#define ARK12_TILES   33735
#define ARK12_RUNS    3278

/* 8 bytes a run: the width is the top bit of `count`, since a run is
 * never 32768 long -- a separate byte would pad the struct to 12, and
 * there are 3278 runs (the CJK blocks are not drawn whole). */
#define ARK12_RUN_FULL  0x8000u
typedef struct {
    uint16_t first;     /* the run's first codepoint */
    uint16_t count;     /* how many consecutive codepoints | ARK12_RUN_FULL */
    uint32_t tix;       /* where its tile numbers start in ark12_tix */
} ark12_run_t;

extern const ark12_run_t ark12_runs[ARK12_RUNS];
extern const uint16_t    ark12_tix[];
/* ARK12_TILES tiles of 36 bits, row 0 in the low 6 bits, bit 0 of a row
 * leftmost; then 8 bytes of padding so a tile can be read as 8. */
extern const uint8_t     ark12_tiles[];

static inline uint32_t ark12_tile_row(uint16_t t, int r)
{
    const uint32_t bit = (uint32_t)t * 36u + (uint32_t)r * 6u;
    const uint8_t *p = ark12_tiles + (bit >> 3);
    const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    return (v >> (bit & 7u)) & 0x3Fu;
}

/*
 * Fills rows[] with the glyph for `cp` and returns true, with *w_out its
 * width. False for a codepoint outside the subset -- callers decide what
 * that means; gfx.c draws a notdef box sized from the codepoint's own
 * expected width. On false neither rows[] nor *w_out is written.
 */
static inline bool ark12_glyph(uint32_t cp, int *w_out, uint16_t rows[ARK12_H])
{
    if (cp > 0xFFFFu) return false;
    int lo = 0, hi = ARK12_RUNS - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const ark12_run_t *r = &ark12_runs[mid];
        if (cp < r->first) { hi = mid - 1; continue; }
        const bool full = (r->count & ARK12_RUN_FULL) != 0;
        if (cp >= (uint32_t)r->first + (r->count & ~ARK12_RUN_FULL)) { lo = mid + 1; continue; }
        const int n = full ? 4 : 2;
        const uint16_t *t = ark12_tix + r->tix + (cp - r->first) * (uint32_t)n;
        for (int y = 0; y < ARK12_H; y++) {
            const int half = y / 6, ty = y % 6;
            if (n == 4)
                rows[y] = (uint16_t)(ark12_tile_row(t[half * 2], ty) |
                                     (ark12_tile_row(t[half * 2 + 1], ty) << 6));
            else
                rows[y] = (uint16_t)ark12_tile_row(t[half], ty);
        }
        *w_out = full ? ARK12_FULL_W : ARK12_HALF_W;
        return true;
    }
    return false;
}
