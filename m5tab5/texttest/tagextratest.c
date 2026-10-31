/*
 * tagextratest.c -- 5262: the ID3v1 genre table and tag_genre_fix().
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/tagextra.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *fix(const char *in)
{
    static char g[TAG_GENRE_LEN];
    snprintf(g, sizeof g, "%s", in);
    tag_genre_fix(g, sizeof g);
    return g;
}

int main(void)
{
    /* The table's anchors: the spec's first and last, Winamp's first,
     * and the last this table has. */
    CHECK(!strcmp(id3_genre_name(0), "Blues"), "0 is %s", id3_genre_name(0));
    CHECK(!strcmp(id3_genre_name(17), "Rock"), "17 is %s", id3_genre_name(17));
    CHECK(!strcmp(id3_genre_name(79), "Hard Rock"), "79 is %s", id3_genre_name(79));
    CHECK(!strcmp(id3_genre_name(80), "Folk"), "80 is %s", id3_genre_name(80));
    CHECK(!strcmp(id3_genre_name(191), "Psybient"), "191 is %s", id3_genre_name(191));
    CHECK(id3_genre_name(192) == NULL, "192 is past the end");

    CHECK(!strcmp(fix("(17)"), "Rock"), "(17) -> %s", fix("(17)"));
    CHECK(!strcmp(fix("17"), "Rock"), "17 -> %s", fix("17"));
    CHECK(!strcmp(fix("(17)Rock"), "Rock"), "(17)Rock -> %s", fix("(17)Rock"));
    CHECK(!strcmp(fix("(4)Disco Inferno"), "Disco Inferno"), "words kept: %s", fix("(4)Disco Inferno"));
    CHECK(!strcmp(fix("Rock"), "Rock"), "a name is left alone: %s", fix("Rock"));
    CHECK(!strcmp(fix("(999)"), "(999)"), "an unknown number is left: %s", fix("(999)"));
    CHECK(!strcmp(fix("(17"), "(17"), "an unclosed paren is left: %s", fix("(17"));
    CHECK(!strcmp(fix("80s Pop"), "80s Pop"), "a name that starts with a number: %s", fix("80s Pop"));
    CHECK(!strcmp(fix("2 Tone"), "2 Tone"), "and another: %s", fix("2 Tone"));
    CHECK(!strcmp(fix(""), ""), "empty stays empty");
    {
        char g[4] = "17";
        tag_genre_fix(g, sizeof g);
        CHECK(!strcmp(g, "Roc"), "cut to the field: %s", g);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAILURES" : "all passed",
           checks, failures);
    return failures ? 1 : 0;
}
