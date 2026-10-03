/*
 * bidilinetest.c -- bidiline.h: a right-to-left line in drawing order.
 *
 * Written in a shorthand so the expected orders can be read: a capital
 * letter A-Z stands for an Arabic letter (U+0621 + its index), '#' for
 * an Arabic-Indic digit, and everything else is itself -- so Latin in
 * these cases is lower case. Expectations
 * were worked out from UAX #9 by hand for each case, not by running
 * this code.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/bidiline.h"

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

static int enc(const char *t, uint32_t *out)
{
    int n = 0;
    for (; *t; t++) {
        if (*t >= 'A' && *t <= 'Z') out[n++] = 0x0621 + (uint32_t)(*t - 'A');
        else if (*t == '#') out[n++] = 0x0661;
        else out[n++] = (unsigned char)*t;
    }
    return n;
}

static void dec(const uint32_t *s, int n, char *out)
{
    for (int i = 0; i < n; i++) {
        if (s[i] >= 0x0621 && s[i] < 0x0621 + 26) out[i] = (char)('A' + (s[i] - 0x0621));
        else if (s[i] == 0x0661) out[i] = '#';
        else if (s[i] < 0x80) out[i] = (char)s[i];
        else out[i] = '?';
    }
    out[n] = '\0';
}

static void order(const char *logical, const char *want)
{
    uint32_t s[256];
    char got[256];
    const int n = enc(logical, s);
    bidiline_rtl(s, n);
    dec(s, n, got);
    CHECK(strcmp(got, want) == 0, "[%s] drew [%s], want [%s]", logical, got, want);
}

int main(void)
{
    /* The board photo's title, in shorthand: words, a year, a bracketed
     * phrase, a credit. Visual is the logical reversed, the year kept
     * in order, both brackets mirrored. */
    order("JDYD 2021 ZAML ( ARWS ALJN ) ADA ALMNSD",
          "DSNMLA ADA ( NJLA SWRA ) LMAZ 2021 DYDJ");

    /* Arabic alone: plain reversal. */
    order("ABC DEF", "FED CBA");

    /* Latin inside: kept in its own order, spaces between Latin words
     * stay with them. */
    order("AB daft punk CD", "DC daft punk BA");

    /* A Latin title with its own brackets keeps them (N0: L inside, L
     * before the opening bracket). */
    order("AB song (live) CD", "DC song (live) BA");

    /* Brackets around Arabic in an Arabic line mirror, as a pair. */
    order("AB (CD) EF", "FE (DC) BA");

    /* Brackets around Latin right after Arabic: L inside, R before the
     * opening bracket -> the pair is R and mirrors; the Latin stays. */
    order("AB (live) CD", "DC (live) BA");

    /* Digits: European in order; a time keeps its colon (between two
     * L). */
    order("AB 12:30 CD", "DC 12:30 BA");

    /* Leading and trailing neutrals belong to the paragraph, R. */
    order("- AB -", "- BA -");
    order("AB.", ".BA");

    /* An unmatched bracket is only a neutral; it still mirrors at R. */
    order("AB ( CD", "DC ) BA");

    /* Guillemets mirror too; < and > mirror but do not pair. */
    {
        uint32_t s[8] = { 0x00AB, 0x0628, 0x00BB };
        bidiline_rtl(s, 3);
        CHECK(s[0] == 0x00AB && s[1] == 0x0628 && s[2] == 0x00BB, "guillemets mirror as a pair");
    }
    order("A<B", "B>A");

    /* Arabic-Indic digits are R here: the caller has already put a
     * shaped run back in logical order, and this reversal restores
     * what arabixel_shape() drew. */
    order("A ##", "## A");

    /* Nested and mixed brackets. */
    order("AB [x (CD) y] EF", "FE [y (DC) x] BA");

    /* Empty and NULL do nothing. */
    bidiline_rtl(NULL, 3);
    {
        uint32_t s[1] = { 'x' };
        bidiline_rtl(s, 0);
        CHECK(s[0] == 'x', "n == 0 touched the array");
    }

    /* Classes. */
    CHECK(bidi_class(0x0627) == BIDI_R && bidi_class(0xFEFB) == BIDI_R &&
          bidi_class(0x05D0) == BIDI_R, "R");
    CHECK(bidi_class('a') == BIDI_L && bidi_class('7') == BIDI_L &&
          bidi_class(0x00E9) == BIDI_L && bidi_class(0x4E00) == BIDI_L, "L");
    CHECK(bidi_class(' ') == BIDI_N && bidi_class('(') == BIDI_N &&
          bidi_class(0x2014) == BIDI_N && bidi_class(0x00A0) == BIDI_N, "N");

    /* Deep nesting stops pairing rather than overflowing. */
    {
        char deep[64] = "A";
        for (int i = 0; i < 20; i++) strcat(deep, "(");
        strcat(deep, "B");
        uint32_t s[64];
        const int n = enc(deep, s);
        bidiline_rtl(s, n);
        CHECK(1, "deep");
    }

    printf("bidilinetest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
