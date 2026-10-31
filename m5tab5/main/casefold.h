/*
 * casefold.h -- simple case folding for the scripts that have case, as
 * ranges rather than a table. 5243.
 *
 * mediasearch.h folded ASCII only, on the grounds that anything more
 * needed a Unicode case map this player had no room for. The board run
 * that found it wanted was a USB drive of real music: "Bôa", searched
 * as MPD clients search -- case-insensitively, as "BÔA" -- found
 * nothing, where MPD (which folds with ICU or GLib) finds it.
 *
 * NO TABLE IS NEEDED for the scripts a music library is written in. In
 * each, upper and lower case are a fixed distance apart or alternate
 * odd/even across a block, so the fold is a few range tests:
 *
 *   Basic Latin         A-Z                      +0x20
 *   Latin-1             U+00C0-00DE but U+00D7   +0x20  (ÀÉÑÖØÞ...); µ as μ
 *   Latin Extended-A    U+0100-017F              pairs, parity per run
 *   Latin Extended-B    U+0180-024F              the regular runs, and
 *                                                Ơ Ư (Vietnamese)
 *   Greek               U+0386-03AB              +0x20 and the tonos
 *                                                letters; ς as σ
 *   Cyrillic            U+0400-042F              +0x50 / +0x20
 *                       U+0460-04FF              pairs
 *   Armenian            U+0531-0556              +0x30
 *   Latin Ext. Addl.    U+1E00-1EFF              pairs (Vietnamese)
 *   Greek Extended, Georgian, Cherokee, fullwidth Latin: not folded.
 *
 * SIMPLE FOLDING, ONE CODE POINT TO ONE: ß stays ß (full folding makes
 * it "ss"), and İ folds to i. That is Unicode's "simple" case folding
 * (CaseFolding.txt status C and S) for the ranges above, which is what
 * MPD's GLib fallback does and close to what ICU gives for these.
 *
 * UTF-8 IN, UTF-8 OUT, and bytes that are not valid UTF-8 pass through
 * exactly, one at a time -- a Latin-1 tag that m3u_line_clean() or the
 * tag reader did not convert must not become a different character. A
 * folded code point can be shorter than the original (İ, two bytes, to
 * i, one) and never longer.
 *
 * PURE and header-only but for 5256's tables (casefold_tab.c): the
 * index writer, the search, and the filter expressions all fold with
 * this, so they cannot disagree. Host-tested
 * in texttest/mediasearchtest.c and mpdfiltertest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One code point's simple case fold (lower case). */
static inline uint32_t casefold_cp(uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 0x20 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c == 0xB5) return 0x3BC;                            /* µ, as μ */
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';                         /* İ */
        if (c == 0x178) return 0xFF;                        /* Ÿ */
        if (c == 0x17F) return 's';                         /* ſ */
        if ((c <= 0x12F || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) &&
            !(c & 1)) return c + 1;
        if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1))
            return c + 1;
        return c;
    }
    if (c >= 0x180 && c <= 0x24F) {
        /* The regular runs of Latin Extended-B: Ǎ..ǜ (odd upper), Ǟ..ǯ,
         * Ǹ..ȳ and Ɇ..ɏ (even upper). The irregular letters between are
         * left as they are. */
        if (c == 0x1A0 || c == 0x1AF) return c + 1;         /* Ơ Ư, Vietnamese */
        if (c >= 0x1CD && c <= 0x1DC && (c & 1)) return c + 1;
        if (((c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) ||
             (c >= 0x222 && c <= 0x233) || (c >= 0x246 && c <= 0x24F)) && !(c & 1))
            return c + 1;       /* not U+0220, whose lower case is U+019E */
        return c;
    }
    if (c >= 0x370 && c <= 0x3FF) {
        if (c == 0x386) return 0x3AC;                       /* Ά */
        if (c >= 0x388 && c <= 0x38A) return c + 0x25;      /* Έ Ή Ί */
        if (c == 0x38C) return 0x3CC;                       /* Ό */
        if (c == 0x38E || c == 0x38F) return c + 0x3F;      /* Ύ Ώ */
        if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 0x20;
        if (c == 0x3C2) return 0x3C3;                       /* final sigma */
        return c;
    }
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;          /* Ѐ..Џ */
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;          /* А..Я */
    if (c >= 0x460 && c <= 0x4FF) {
        if (c == 0x4C0) return 0x4CF;                       /* Ӏ */
        if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) ||
             (c >= 0x4D0 && c <= 0x4FF)) && !(c & 1)) return c + 1;
        if (c >= 0x4C1 && c <= 0x4CE && (c & 1)) return c + 1;
        return c;
    }
    if (c >= 0x531 && c <= 0x556) return c + 0x30;          /* Armenian */
    if (c >= 0x1E00 && c <= 0x1EFF) {
        if (c == 0x1E9E) return 0xDF;                       /* ẞ */
        if (((c >= 0x1E00 && c <= 0x1E95) || c >= 0x1EA0) && !(c & 1)) return c + 1;
        return c;
    }
    return c;
}

/*
 * The next code point of `*p`, advancing it, or the next byte alone when
 * it does not start a valid, shortest-form UTF-8 sequence -- returned as
 * 0x110000 + byte, a value no code point has, so it folds to itself and
 * encodes back as that one byte. 0 at the end of the string.
 */
static inline uint32_t casefold_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    const unsigned char b = s[0];
    if (b == 0) return 0;
    if (b < 0x80) { *p += 1; return b; }
    int n = 0;
    uint32_t c = 0, min = 0;
    if ((b & 0xE0) == 0xC0)      { n = 1; c = b & 0x1F; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { n = 2; c = b & 0x0F; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { n = 3; c = b & 0x07; min = 0x10000; }
    for (int i = 1; n && i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = 0; break; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (!n || c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *p += 1;
        return 0x110000u + b;
    }
    *p += n + 1;
    return c;
}

/* `c` (from casefold_next()) as UTF-8 into `out`; its length. */
static inline int casefold_put(uint32_t c, char *out)
{
    unsigned char *o = (unsigned char *)out;
    if (c >= 0x110000u) { o[0] = (unsigned char)(c - 0x110000u); return 1; }
    if (c < 0x80)    { o[0] = (unsigned char)c; return 1; }
    if (c < 0x800)   { o[0] = (unsigned char)(0xC0 | (c >> 6));
                       o[1] = (unsigned char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (unsigned char)(0xE0 | (c >> 12));
                       o[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
                       o[2] = (unsigned char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (unsigned char)(0xF0 | (c >> 18));
    o[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (c & 0x3F));
    return 4;
}

/* ---- 5256: normalisation -------------------------------------------------- */

/* The tables, generated by tools/gen_casefold.py into casefold_tab.c. */
#define CASEFOLD_NDEC   (810)
#define CASEFOLD_NCCC   (252)
extern const uint16_t casefold_dec_cp[CASEFOLD_NDEC];   /* ascending */
extern const uint16_t casefold_dec_at[CASEFOLD_NDEC];   /* pool index << 2 | (length - 1) */
extern const uint16_t casefold_dec_pool[];
extern const uint16_t casefold_ccc_cp[CASEFOLD_NCCC];   /* marks, ascending */
extern const uint8_t  casefold_ccc[CASEFOLD_NCCC];      /* their combining classes */

/*
 * MPD 0.24 compares case-insensitive filters after Unicode normalisation,
 * so "é" as one code point and as e + U+0301 are one letter. What this
 * produces is NFD, case-folded: every code point is decomposed (the
 * generated table, and Hangul by arithmetic), each piece folded by
 * casefold_cp(), and a run of combining marks put in canonical order,
 * lowest class first, stably -- so "e + dot below + circumflex" and
 * "e + circumflex + dot below" are both ệ. Accents are KEPT: é is not e,
 * in MPD or here.
 *
 * AN ITERATOR, because one code point can become up to four and the
 * marks after it have to be gathered to be ordered. It reads the input
 * a starter and its marks at a time into `buf`, and hands them out one
 * by one. A copy of an iterator is a bookmark: mpdfilter's substring
 * search restarts from one at each position. A mark run longer than the
 * buffer is ordered in pieces -- not canonical, and not a thing a tag
 * contains.
 */

#define CASEFOLD_BUF    (12)

typedef struct {
    const char *p;
    uint32_t    buf[CASEFOLD_BUF];
    uint8_t     n, i;
} casefold_it_t;

static inline int casefold_ccc_of(uint32_t c)
{
    if (c < 0x300 || c > 0xFFFF) return 0;
    int lo = 0, hi = CASEFOLD_NCCC - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (casefold_ccc_cp[m] == c) return casefold_ccc[m];
        if (casefold_ccc_cp[m] < c) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

/* `c` decomposed and folded, appended to it->buf; false when it will
 * not fit (nothing appended). */
static inline bool casefold_push_(casefold_it_t *it, uint32_t c)
{
    uint32_t d[4];
    int k = 0;
    if (c >= 0xAC00 && c <= 0xD7A3) {                   /* Hangul, by arithmetic */
        const uint32_t s = c - 0xAC00;
        d[k++] = 0x1100 + s / 588;
        d[k++] = 0x1161 + (s % 588) / 28;
        if (s % 28) d[k++] = 0x11A7 + s % 28;
    } else {
        int lo = 0, hi = CASEFOLD_NDEC - 1, at = -1;
        if (c >= 0xC0 && c <= 0xFFFF) {
            while (lo <= hi) {
                const int m = (lo + hi) / 2;
                if (casefold_dec_cp[m] == c) { at = m; break; }
                if (casefold_dec_cp[m] < c) lo = m + 1; else hi = m - 1;
            }
        }
        if (at >= 0) {
            const uint16_t o = casefold_dec_at[at];
            for (int j = 0; j <= (o & 3); j++) d[k++] = casefold_dec_pool[(o >> 2) + j];
        } else {
            d[k++] = c;
        }
    }
    if (it->n + k > CASEFOLD_BUF) return false;
    for (int j = 0; j < k; j++) it->buf[it->n++] = casefold_cp(d[j]);
    return true;
}

static inline void casefold_begin(casefold_it_t *it, const char *s)
{
    it->p = s ? s : "";
    it->n = it->i = 0;
}

/* The next code point, normalised and folded; 0 at the end. Bytes that
 * are not UTF-8 come as casefold_next() gives them, and pass through. */
static inline uint32_t casefold_get(casefold_it_t *it)
{
    if (it->i < it->n) return it->buf[it->i++];
    it->n = it->i = 0;
    const uint32_t c = casefold_next(&it->p);
    if (!c) return 0;
    if (!casefold_push_(it, c)) return casefold_cp(c);     /* cannot happen: n was 0 */
    /* The marks that follow, gathered while they fit. */
    for (;;) {
        const char *q = it->p;
        const uint32_t m = casefold_next(&q);
        if (!m || !casefold_ccc_of(m)) break;
        if (!casefold_push_(it, m)) break;
        it->p = q;
    }
    /* Canonical order: a stable insertion sort of every mark by class. A
     * starter (class 0) stays where it is and nothing crosses it. */
    for (int a = 1; a < it->n; a++) {
        const uint32_t x = it->buf[a];
        const int cx = casefold_ccc_of(x);
        if (!cx) continue;
        int b = a - 1;
        while (b >= 0) {
            const int cb = casefold_ccc_of(it->buf[b]);
            if (!cb || cb <= cx) break;
            it->buf[b + 1] = it->buf[b];
            b--;
        }
        it->buf[b + 1] = x;
    }
    return it->buf[it->i++];
}

#ifdef __cplusplus
}
#endif
