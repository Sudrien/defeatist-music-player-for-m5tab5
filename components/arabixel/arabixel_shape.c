/*
 * arabixel_shape.c -- contextual forms and visual order for a run of
 * Arabic, and the glyph lookup. What a shaping engine does for Arabic,
 * cut to what one line of a station or track name needs: the joining
 * types of the 28 letters, hamza forms, teh marbuta and alef maksura,
 * the four lam-alef ligatures, and reversal. No GSUB, no bidi algorithm
 * -- a run is right to left inside a line that stays left to right.
 *
 * SPDX-License-Identifier: MIT
 */
#include "arabixel.h"

#include <stddef.h>

const arabixel_glyph_t *arabixel_find(uint32_t cp)
{
    unsigned lo = 0, hi = arabixel_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (arabixel_glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return (lo < arabixel_count && arabixel_glyphs[lo].cp == cp)
           ? &arabixel_glyphs[lo] : NULL;
}

/* Joining type, Unicode's ArabicShaping.txt for these letters. */
enum { U = 0, R, D, C };

/* U+0621..U+064A: the isolated form in Presentation Forms-B (final is
 * +1; for D, initial +2 and medial +3) and the joining type. 0 where
 * there is no letter. */
static const struct { uint16_t iso; uint8_t type; } LETTERS[0x64A - 0x621 + 1] = {
    [0x621 - 0x621] = { 0xFE80, U },   /* hamza */
    [0x622 - 0x621] = { 0xFE81, R },   /* alef madda */
    [0x623 - 0x621] = { 0xFE83, R },   /* alef hamza above */
    [0x624 - 0x621] = { 0xFE85, R },   /* waw hamza */
    [0x625 - 0x621] = { 0xFE87, R },   /* alef hamza below */
    [0x626 - 0x621] = { 0xFE89, D },   /* yeh hamza */
    [0x627 - 0x621] = { 0xFE8D, R },   /* alef */
    [0x628 - 0x621] = { 0xFE8F, D },   /* beh */
    [0x629 - 0x621] = { 0xFE93, R },   /* teh marbuta */
    [0x62A - 0x621] = { 0xFE95, D },   /* teh */
    [0x62B - 0x621] = { 0xFE99, D },   /* theh */
    [0x62C - 0x621] = { 0xFE9D, D },   /* jeem */
    [0x62D - 0x621] = { 0xFEA1, D },   /* hah */
    [0x62E - 0x621] = { 0xFEA5, D },   /* khah */
    [0x62F - 0x621] = { 0xFEA9, R },   /* dal */
    [0x630 - 0x621] = { 0xFEAB, R },   /* thal */
    [0x631 - 0x621] = { 0xFEAD, R },   /* reh */
    [0x632 - 0x621] = { 0xFEAF, R },   /* zain */
    [0x633 - 0x621] = { 0xFEB1, D },   /* seen */
    [0x634 - 0x621] = { 0xFEB5, D },   /* sheen */
    [0x635 - 0x621] = { 0xFEB9, D },   /* sad */
    [0x636 - 0x621] = { 0xFEBD, D },   /* dad */
    [0x637 - 0x621] = { 0xFEC1, D },   /* tah */
    [0x638 - 0x621] = { 0xFEC5, D },   /* zah */
    [0x639 - 0x621] = { 0xFEC9, D },   /* ain */
    [0x63A - 0x621] = { 0xFECD, D },   /* ghain */
    [0x640 - 0x621] = { 0,      C },   /* tatweel: joins both ways, no forms */
    [0x641 - 0x621] = { 0xFED1, D },   /* feh */
    [0x642 - 0x621] = { 0xFED5, D },   /* qaf */
    [0x643 - 0x621] = { 0xFED9, D },   /* kaf */
    [0x644 - 0x621] = { 0xFEDD, D },   /* lam */
    [0x645 - 0x621] = { 0xFEE1, D },   /* meem */
    [0x646 - 0x621] = { 0xFEE5, D },   /* noon */
    [0x647 - 0x621] = { 0xFEE9, D },   /* heh */
    [0x648 - 0x621] = { 0xFEED, R },   /* waw */
    [0x649 - 0x621] = { 0xFEEF, R },   /* alef maksura: D in Unicode, but
                                          Forms-B has only its two forms */
    [0x64A - 0x621] = { 0xFEF1, D },   /* yeh */
};

static int type_of(uint32_t cp)
{
    return (cp >= 0x621 && cp <= 0x64A) ? LETTERS[cp - 0x621].type : U;
}

static bool is_mark(uint32_t cp)
{
    return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670;
}

static bool is_digit(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') ||
           (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9);
}

/* lam + this alef: the isolated ligature, final is +1. */
static uint16_t lam_alef(uint32_t cp)
{
    switch (cp) {
    case 0x622: return 0xFEF5;
    case 0x623: return 0xFEF7;
    case 0x625: return 0xFEF9;
    case 0x627: return 0xFEFB;
    default:    return 0;
    }
}

static void reverse(uint32_t *a, int n)
{
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        const uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

int arabixel_shape(uint32_t *run, int n)
{
    /* Marks out first, so a letter's neighbours are letters. */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!is_mark(run[i])) run[m++] = run[i];
    n = m;

    /* Forms, in logical order. Done into the same array: run[i] is read
     * before it is written, and its neighbours' joining types are taken
     * from `prev`, the letter as it was, and run[i + 1], not yet
     * written. */
    int out = 0, prev_type = U;
    for (int i = 0; i < n; i++) {
        const uint32_t cp = run[i];
        const int t = type_of(cp);
        const bool joins_prev = (t == D || t == R || t == C) &&
                                (prev_type == D || prev_type == C);
        uint32_t next = i + 1 < n ? run[i + 1] : 0;

        const uint16_t la = cp == 0x644 ? lam_alef(next) : 0;
        if (la) {
            run[out++] = la + (joins_prev ? 1 : 0);
            i++;                    /* the alef is in the ligature */
            prev_type = R;          /* and, like alef, joins nothing after */
            continue;
        }

        const int nt = type_of(next);
        const bool joins_next = (t == D || t == C) &&
                                (nt == D || nt == R || nt == C);
        const uint16_t iso = (cp >= 0x621 && cp <= 0x64A) ? LETTERS[cp - 0x621].iso : 0;
        uint32_t g = cp;
        if (iso) {
            if (t == D)
                g = iso + (joins_prev ? (joins_next ? 3 : 1) : (joins_next ? 2 : 0));
            else
                g = iso + (joins_prev ? 1 : 0);
        }
        run[out++] = g;
        prev_type = t;
    }
    n = out;

    /* Visual order: right to left, but numbers read left to right. */
    reverse(run, n);
    for (int i = 0; i < n; ) {
        if (!is_digit(run[i])) { i++; continue; }
        int j = i;
        while (j < n && is_digit(run[j])) j++;
        reverse(run + i, j - i);
        i = j;
    }
    return n;
}
