/*
 * m3ulinetest.c -- m3uline.c: a playlist line written by something else.
 *
 * Written from m3uline.h's rules: .m3u8 is UTF-8 by name; #EXTENC
 * declares; otherwise valid UTF-8 stays and anything else is Latin-1;
 * a run of backslashes is one '/'; a line that will not fit is refused.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/m3uline.h"

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

static void clean(const char *in, m3u_enc_t enc, const char *want)
{
    char b[256];
    snprintf(b, sizeof(b), "%s", in);
    const int n = m3u_line_clean(b, sizeof(b), enc);
    CHECK(n >= 0 && strcmp(b, want) == 0 && (size_t)n == strlen(want),
          "[%s] enc %d gave [%s] (%d), want [%s]", in, (int)enc, b, n, want);
}

int main(void)
{
    /* Names. */
    CHECK(m3u_is_name("a.m3u") && m3u_is_name("a.M3U8") && m3u_is_name("x y.m3u8"), "m3u names");
    CHECK(!m3u_is_name("a.m3") && !m3u_is_name(".m3u") && !m3u_is_name("a.m3u9") &&
          !m3u_is_name("m3u") && !m3u_is_name(NULL), "not m3u names");
    CHECK(m3u_enc_of_name("a.m3u8") == M3U_ENC_UTF8, ".m3u8 is utf-8");
    CHECK(m3u_enc_of_name("a.m3u") == M3U_ENC_UNKNOWN, ".m3u is undeclared");

    /* Directives. */
    m3u_enc_t e = M3U_ENC_UNKNOWN;
    CHECK(m3u_directive("#EXTENC: UTF-8", &e) && e == M3U_ENC_UTF8, "EXTENC utf-8");
    CHECK(m3u_directive("#extenc:iso-8859-1", &e) && e == M3U_ENC_LATIN1, "EXTENC latin-1");
    e = M3U_ENC_UTF8;
    CHECK(m3u_directive("#EXTENC: KOI8-R", &e) && e == M3U_ENC_UTF8,
          "an unknown EXTENC changed the encoding");
    CHECK(!m3u_directive("#EXTINF:123,x", &e) && !m3u_directive("Album/x.mp3", &e),
          "not directives");

    /* Endings and the BOM. */
    clean("Album/01.mp3\r\n", M3U_ENC_UNKNOWN, "Album/01.mp3");
    clean("Album/01.mp3  \n", M3U_ENC_UNKNOWN, "Album/01.mp3");
    clean("\xEF\xBB\xBF" "Album/01.mp3", M3U_ENC_UNKNOWN, "Album/01.mp3");

    /* Backslashes: one, escaped, mixed. */
    clean("Album\\01.mp3", M3U_ENC_UNKNOWN, "Album/01.mp3");
    clean("Album\\\\01.mp3", M3U_ENC_UNKNOWN, "Album/01.mp3");
    clean("A\\B/C\\\\\\\\d.flac", M3U_ENC_UNKNOWN, "A/B/C/d.flac");

    /* Encoding. Valid UTF-8 stays whatever the guess. */
    clean("Caf\xC3\xA9/T\xC3\xB6rn.mp3", M3U_ENC_UNKNOWN, "Caf\xC3\xA9/T\xC3\xB6rn.mp3");
    clean("Caf\xC3\xA9.mp3", M3U_ENC_UTF8, "Caf\xC3\xA9.mp3");
    /* Latin-1 bytes are not UTF-8, so the guess converts them. */
    clean("Caf\xE9/T\xF6rn.mp3", M3U_ENC_UNKNOWN, "Caf\xC3\xA9/T\xC3\xB6rn.mp3");
    /* Declared Latin-1 converts even bytes that happen to be valid UTF-8. */
    clean("\xC3\xA9", M3U_ENC_LATIN1, "\xC3\x83\xC2\xA9");
    /* Declared UTF-8 is trusted, invalid or not. */
    clean("Caf\xE9.mp3", M3U_ENC_UTF8, "Caf\xE9.mp3");
    /* Overlong and surrogate encodings are not UTF-8. */
    clean("\xC0\xAF", M3U_ENC_UNKNOWN, "\xC3\x80\xC2\xAF");
    clean("\xED\xA0\x80", M3U_ENC_UNKNOWN, "\xC3\xAD\xC2\xA0\xC2\x80");
    /* A sequence cut off at the end. */
    clean("x\xE2\x82", M3U_ENC_UNKNOWN, "x\xC3\xA2\xC2\x82");

    /* Too long once converted: refused, not cut. */
    {
        char b[8] = "\xE9\xE9\xE9\xE9";         /* 4 bytes, needs 8 + NUL */
        CHECK(m3u_line_clean(b, sizeof(b), M3U_ENC_LATIN1) == -1, "an overlong conversion fitted");
        char c[9] = "\xE9\xE9\xE9\xE9";
        CHECK(m3u_line_clean(c, sizeof(c), M3U_ENC_LATIN1) == 8, "an exact conversion was refused");
    }
    CHECK(m3u_line_clean(NULL, 4, M3U_ENC_UNKNOWN) == -1, "NULL line");

    /* Every byte string: the result is valid UTF-8 unless declared UTF-8. */
    {
        unsigned seed = 5198u;
        for (int it = 0; it < 20000; it++) {
            char b[64];
            const int len = (int)(seed % 20);
            for (int i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                b[i] = (char)((seed >> 16) & 0xFF);
                if (!b[i]) b[i] = 'a';
            }
            b[len] = '\0';
            seed = seed * 1103515245u + 12345u;
            const int n = m3u_line_clean(b, sizeof(b), M3U_ENC_UNKNOWN);
            checks++;
            if (n < 0 || strchr(b, '\\')) { failures++; printf("FAIL fuzz: n %d\n", n); continue; }
            /* re-cleaning a clean line changes nothing */
            char again[64];
            memcpy(again, b, (size_t)n + 1);
            const int n2 = m3u_line_clean(again, sizeof(again), M3U_ENC_UNKNOWN);
            checks++;
            if (n2 != n || memcmp(again, b, (size_t)n + 1) != 0) {
                failures++;
                printf("FAIL fuzz: cleaning twice changed the line\n");
            }
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
