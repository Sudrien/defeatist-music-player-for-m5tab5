/*
 * bidiline.h -- a line that reads right to left, put in drawing order.
 *
 * 6029 drew Arabic by reversing each run of Arabic letters and leaving
 * the runs in logical order across the line. Right for a name alone,
 * wrong the moment anything else is in the line. A board photo of the
 * title
 *
 *     جديد 2021 زامل ( عروش الجن ) اداء المنشد قناف المعظي
 *
 * read, from the right, "اداء المنشد قناف المعظي ( عروش الجن ) ز...":
 * the digits and both brackets ended runs, so the phrases came out last
 * first; the brackets faced the wrong way; and the truncation, which
 * keeps the right-hand side of a right-to-left line because that is
 * where it starts, kept the end of the title and cut its first word to
 * one letter.
 *
 * THIS IS THE PART OF THE UNICODE BIDI ALGORITHM (UAX #9) A ONE-LINE
 * LABEL NEEDS, FOR A RIGHT-TO-LEFT PARAGRAPH, AND NO MORE:
 *
 *   - three classes: R (Arabic, including its digits and the
 *     presentation forms shaping produces), L (Latin and other letters,
 *     and European digits -- which in an R paragraph keep their own
 *     left-to-right order, so "2021" and "Daft Punk" read correctly), and
 *     N (spaces and punctuation);
 *   - a run of N between two L is L (so "Song (Live)" stays one piece);
 *     every other N is R, the paragraph's direction (N1, N2);
 *   - the line is reversed, then every maximal L piece is put back in its
 *     own order (L2);
 *   - a bracket at R is drawn mirrored (L4): "(" as ")".
 *
 *   - a bracket pair takes one direction for both halves (N0, simply):
 *     R if there is R inside it; else L if there is L inside and the
 *     strong type before the opening bracket is L; else left to the
 *     rule above. So "Song (Live)" in an Arabic line keeps both its
 *     brackets, and "( عروش الجن )" mirrors both.
 *
 * Not here, because no string this screen draws needs it: explicit
 * embeddings and isolates, weak-type
 * subtleties beyond "digits are L", and lines that start
 * left-to-right -- those keep 6029's behaviour, which is right for a
 * Latin title with an Arabic word in it.
 *
 * In place, no allocation, on an array of codepoints. The caller has
 * already shaped each Arabic run where it sits (see gfx.c) and put the
 * shaped run back in logical order, so the reversal here leaves it the
 * way arabixel_shape() drew it -- including the Arabic-Indic digits,
 * which that function keeps in order.
 *
 * Host-tested: texttest/bidilinetest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { BIDI_N = 0, BIDI_L, BIDI_R } bidi_class_t;

static inline bidi_class_t bidi_class(uint32_t cp)
{
    if ((cp >= 0x0590 && cp <= 0x08FF) ||           /* Hebrew, Arabic, Syriac, ... */
        (cp >= 0xFB1D && cp <= 0xFDFF) ||           /* presentation forms A */
        (cp >= 0xFE70 && cp <= 0xFEFF))             /* presentation forms B */
        return BIDI_R;
    if (cp < 0x80) {
        if ((cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z'))
            return BIDI_L;
        return BIDI_N;
    }
    if (cp <= 0xBF) return BIDI_N;                  /* Latin-1 punctuation, NBSP */
    if (cp == 0xD7 || cp == 0xF7) return BIDI_N;    /* x and divide signs */
    if (cp >= 0x2000 && cp <= 0x2BFF) return BIDI_N;    /* punctuation, symbols, arrows */
    if (cp >= 0x3000 && cp <= 0x303F) return BIDI_N;    /* CJK punctuation */
    if (cp >= 0xFE10 && cp <= 0xFE6F) return BIDI_N;    /* vertical and small forms */
    if (cp >= 0xFF00 && cp <= 0xFF0F) return BIDI_N;    /* fullwidth punctuation */
    return BIDI_L;
}

/* The mirrored glyph for a bracket drawn right to left, else cp. */
static inline uint32_t bidi_mirror(uint32_t cp)
{
    switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;     /* << >> guillemets */
    case 0x00BB: return 0x00AB;
    case 0x2039: return 0x203A;
    case 0x203A: return 0x2039;
    default:     return cp;
    }
}

/* Working marks, above the 21 bits a codepoint uses. */
#define BIDI_MARK_L     0x80000000u     /* resolved L, for the final pass */
#define BIDI_FORCE_L    0x40000000u     /* a bracket N0 resolved to L */
#define BIDI_FORCE_R    0x20000000u     /* a bracket N0 resolved to R */
#define BIDI_CP(x)      ((x) & 0x001FFFFFu)

static inline bidi_class_t bidi_eff(uint32_t x)
{
    if (x & BIDI_FORCE_L) return BIDI_L;
    if (x & BIDI_FORCE_R) return BIDI_R;
    return bidi_class(BIDI_CP(x));
}

/* The direction of the nearest strong codepoint from i, walking by step,
 * or BIDI_R past either end: the paragraph is right to left (sos, eos). */
static inline bidi_class_t bidi_strong_from(const uint32_t *s, int n, int i, int step)
{
    for (int k = i; k >= 0 && k < n; k += step) {
        const bidi_class_t c = bidi_eff(s[k]);
        if (c != BIDI_N) return c;
    }
    return BIDI_R;
}

static inline bool bidi_is_open(uint32_t cp)
{
    return cp == '(' || cp == '[' || cp == '{' || cp == 0x00AB || cp == 0x2039;
}

#define BIDI_BRACKET_DEPTH  (16)

/* N0: pair the brackets and give each pair its direction. */
static inline void bidi_brackets(uint32_t *s, int n)
{
    int stack[BIDI_BRACKET_DEPTH];
    int depth = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t cp = BIDI_CP(s[i]);
        if (bidi_is_open(cp)) {
            if (depth == BIDI_BRACKET_DEPTH) return;    /* BD16: stop pairing */
            stack[depth++] = i;
            continue;
        }
        if (bidi_is_open(bidi_mirror(cp)) && bidi_mirror(cp) != cp) {
            /* A closing bracket: match the nearest open of its kind. */
            int d = depth - 1;
            while (d >= 0 && BIDI_CP(s[stack[d]]) != bidi_mirror(cp)) d--;
            if (d < 0) continue;                        /* unpaired */
            const int o = stack[d];
            depth = d;
            bool r = false, l = false;
            for (int k = o + 1; k < i; k++) {
                const bidi_class_t c = bidi_eff(s[k]);
                r |= c == BIDI_R;
                l |= c == BIDI_L;
            }
            uint32_t force = 0;
            if (r) force = BIDI_FORCE_R;
            else if (l) force = bidi_strong_from(s, n, o - 1, -1) == BIDI_L
                              ? BIDI_FORCE_L : BIDI_FORCE_R;
            s[o] |= force;
            s[i] |= force;
        }
    }
}

/*
 * `s[0..n)` from logical order to drawing order (left to right), for a
 * right-to-left line.
 */
static inline void bidiline_rtl(uint32_t *s, int n)
{
    if (!s || n <= 0) return;
    bidi_brackets(s, n);
    /* Resolve each codepoint to L or R. Neutrals look at their strong
     * neighbours, so the marks go on in a second pass. */
    for (int i = 0; i < n; i++) {
        bidi_class_t c = bidi_eff(s[i]);
        if (c == BIDI_N)
            c = (bidi_strong_from(s, n, i - 1, -1) == BIDI_L &&
                 bidi_strong_from(s, n, i + 1, +1) == BIDI_L) ? BIDI_L : BIDI_R;
        if (c == BIDI_L) s[i] |= BIDI_MARK_L;
    }
    for (int i = 0; i < n; i++) s[i] &= ~(BIDI_FORCE_L | BIDI_FORCE_R);
    /* The whole line reversed... */
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        const uint32_t t = s[a]; s[a] = s[b]; s[b] = t;
    }
    /* ...each L piece put back in its own order, and every R codepoint
     * mirrored. */
    for (int i = 0; i < n; ) {
        if (s[i] & BIDI_MARK_L) {
            int j = i;
            while (j < n && (s[j] & BIDI_MARK_L)) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) {
                const uint32_t t = s[a]; s[a] = s[b]; s[b] = t;
            }
            for (int k = i; k < j; k++) s[k] &= ~BIDI_MARK_L;
            i = j;
        } else {
            s[i] = bidi_mirror(s[i]);
            i++;
        }
    }
}
