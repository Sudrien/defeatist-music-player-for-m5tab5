/*
 * mpdfiltertest.c -- main/mpdfilter.c, MPD 0.21's filter expressions.
 * 5238. The real parser and evaluator, against expressions shaped like
 * the ones clients send and the ones MPD's doc/protocol.rst gives, and
 * against every way one can be malformed.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mpdfilter.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static mpdfilter_t F;

/* One song: title, artist, album, uri. */
static const char *const SONG[MPDF_NFIELDS] = {
    "Something", "The Beatles", "Abbey Road", "sd/Beatles/Abbey Road/02 Something.flac",
};
static const char *const BARE[MPDF_NFIELDS] = { "", "", "", "sd/Recordings/x.flac" };

static void match(const char *expr, const char *const *song, bool fold, bool want)
{
    const bool ok = mpdfilter_parse(expr, &F);
    CHECK(ok, "[%s] did not parse: %s", expr, F.err ? F.err : "?");
    if (!ok) return;
    const bool got = mpdfilter_eval(&F, song, fold);
    CHECK(got == want, "[%s] fold=%d on [%s]: got %d, want %d", expr, fold, song[0], got, want);
}

static void bad(const char *expr, bool unsupported)
{
    const bool ok = mpdfilter_parse(expr, &F);
    CHECK(!ok, "[%s] parsed and should not have", expr);
    if (ok) return;
    CHECK(F.err && F.err[0], "[%s] failed with no message", expr);
    CHECK(F.unsupported == unsupported, "[%s] unsupported=%d, want %d (%s)", expr,
          F.unsupported, unsupported, F.err ? F.err : "");
}

int main(void)
{
    /* ---- comparisons, exact (find) and folded (search) ---------------- */
    match("(Artist == 'The Beatles')", SONG, false, true);
    match("(artist == 'the beatles')", SONG, false, false);
    match("(artist == 'the beatles')", SONG, true, true);
    match("(ARTIST == \"The Beatles\")", SONG, false, true);
    match("(Artist != 'The Beatles')", SONG, false, false);
    match("(Artist != 'Stones')", SONG, false, true);
    match("(album contains 'Road')", SONG, false, true);
    match("(album contains 'road')", SONG, false, false);
    match("(album contains 'road')", SONG, true, true);
    match("(album contains '')", SONG, false, true);
    match("(title == 'Something')", SONG, false, true);
    match("(AlbumArtist == 'The Beatles')", SONG, false, true);   /* as artist */
    match("(file == 'sd/Beatles/Abbey Road/02 Something.flac')", SONG, false, true);
    match("(file contains 'Abbey')", SONG, false, true);
    match("(any == 'Abbey Road')", SONG, false, true);
    match("(any contains 'beat')", SONG, true, true);
    match("(any == 'nothing')", SONG, false, false);

    /* A tag no song here has: == and contains never, != always. */
    match("(genre == 'Rock')", SONG, false, false);
    match("(genre contains '')", SONG, false, false);
    match("(genre != 'Rock')", SONG, false, true);

    /* An empty field is "" -- a recording with no tags. */
    match("(artist == '')", BARE, false, true);
    match("(artist != '')", BARE, false, false);

    /* ---- base ---------------------------------------------------------- */
    match("(base 'sd/Beatles')", SONG, false, true);
    match("(base 'sd/Beatles/Abbey Road')", SONG, false, true);
    match("(base 'sd/Beat')", SONG, false, false);         /* a folder, not a prefix */
    match("(base 'sd/beatles')", SONG, true, false);       /* never folded */
    match("(base '')", SONG, false, true);

    /* ---- NOT, AND, nesting -------------------------------------------- */
    match("(!(artist == 'The Beatles'))", SONG, false, false);
    match("(!(artist == 'Stones'))", SONG, false, true);
    match("((artist == 'The Beatles') AND (album == 'Abbey Road'))", SONG, false, true);
    match("((artist == 'The Beatles') AND (album == 'Let It Be'))", SONG, false, false);
    match("((artist == 'The Beatles') AND (!(title == 'Something')))", SONG, false, false);
    match("((a == 'x') AND (b == 'y') AND (title contains 'thing'))", SONG, false, false);
    match("((artist contains 'Beat') AND (album contains 'Abbey') AND (title contains 'thing'))",
          SONG, false, true);
    match("((artist == 'The Beatles'))", SONG, false, true);          /* one, nested */
    match("(!(!(artist == 'The Beatles')))", SONG, false, true);
    match("  ( artist   ==   'The Beatles' )  ", SONG, false, true);

    /* ---- escapes: MPD's doc example, as it arrives after the protocol's
     * own unquoting: (Artist == "foo\'bar\"") is foo'bar" ---------------- */
    {
        static const char *const Q[MPDF_NFIELDS] = { "t", "foo'bar\"", "a", "sd/x" };
        match("(Artist == \"foo\\'bar\\\"\")", Q, false, true);
        match("(Artist == 'foo\\'bar\"')", Q, false, true);
        match("(Artist == \"foo'bar\\\"\")", Q, false, true);
        static const char *const B[MPDF_NFIELDS] = { "t", "back\\slash", "a", "sd/x" };
        match("(artist == 'back\\\\slash')", B, false, true);
    }

    /* ---- syntax errors: ARG, not unsupported --------------------------- */
    bad("", false);
    bad("artist == 'x'", false);
    bad("(artist == 'x'", false);
    bad("(artist == 'x'))", false);
    bad("(artist == x)", false);
    bad("(artist == 'x)", false);
    bad("(artist 'x')", false);
    bad("(artist like 'x')", false);
    bad("((a == 'x') OR (b == 'y'))", false);
    bad("((a == 'x') and (b == 'y'))", false);      /* AND is upper case */
    bad("((a == 'x') AND)", false);
    bad("(!)", false);
    bad("(base)", false);
    bad("()", false);
    bad("(artist == 'x') trailing", false);
    bad("(artist == 'x\\", false);

    /* ---- well-formed and refused: unsupported --------------------------- */
    bad("(artist =~ 'Beat.*')", true);
    bad("(artist !~ 'Beat.*')", true);
    bad("(modified-since '2020-01-01T00:00:00Z')", true);
    bad("(AudioFormat == '44100:16:2')", true);
    bad("((artist == 'x') AND (title =~ 'y'))", true);

    /* ---- limits --------------------------------------------------------- */
    {
        char deep[256] = "";
        for (int i = 0; i < MPDF_MAX_DEPTH + 1; i++) strcat(deep, "(!");
        strcat(deep, "(artist == 'x')");
        for (int i = 0; i < MPDF_MAX_DEPTH + 1; i++) strcat(deep, ")");
        bad(deep, false);

        char wide[2048] = "(";
        for (int i = 0; i < MPDF_MAX_NODES + 2; i++) {
            if (i) strcat(wide, " AND ");
            strcat(wide, "(title == 'x')");
        }
        strcat(wide, ")");
        bad(wide, false);

        char *longv = malloc(MPDF_BUF + 64);
        strcpy(longv, "(title == '");
        memset(longv + strlen(longv), 'a', MPDF_BUF);
        strcpy(longv + 11 + MPDF_BUF, "')");
        bad(longv, false);
        free(longv);
    }

    /* ---- the pre-filter: only what a match cannot do without ------------ */
    {
        int req[8];
        mpdfilter_parse("((artist == 'A') AND (album contains 'B') AND (!(title == 'C')) "
                        "AND (genre == 'D') AND (title != 'E'))", &F);
        const int n = mpdfilter_required(&F, req, 8);
        CHECK(n == 2, "required: %d terms, want 2 (artist ==, album contains)", n);
        for (int i = 0; i < n; i++)
            CHECK(F.node[req[i]].kind == MPDF_N_CMP &&
                  (F.node[req[i]].field == MPDF_ARTIST || F.node[req[i]].field == MPDF_ALBUM),
                  "required term %d is field %d", i, F.node[req[i]].field);
        mpdfilter_parse("(!(artist == 'A'))", &F);
        CHECK(mpdfilter_required(&F, req, 8) == 0, "a NOT contributes nothing");
        mpdfilter_parse("(any == 'A')", &F);
        CHECK(mpdfilter_required(&F, req, 8) == 0, "any is not one field");
        mpdfilter_parse("(base 'sd/x')", &F);
        CHECK(mpdfilter_required(&F, req, 8) == 0, "base is not a field term");
    }

    printf("mpdfiltertest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
