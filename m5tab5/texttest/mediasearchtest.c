/*
 * mediasearchtest.c -- mediasearch.h: the folding, the line, the match.
 *
 * Expected lines are written out by hand here rather than produced by
 * the encoder and then blessed, which is texttest/README.md's rule: a
 * check that records what the code did agrees with it by construction.
 * Where a property is easier to state than an example -- "folding never
 * changes a string's length", "a byte over 0x7F is untouched" -- it is
 * checked as a property over every byte instead.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/mediasearch.h"

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

static mediacat_rec_t rec(const char *path, const char *title,
                          const char *artist, const char *album)
{
    mediacat_rec_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.path, sizeof(r.path), "%s", path);
    snprintf(r.title, sizeof(r.title), "%s", title);
    snprintf(r.artist, sizeof(r.artist), "%s", artist);
    snprintf(r.album, sizeof(r.album), "%s", album);
    r.clock = MIDX_CLOCK_SYNCED;
    return r;
}

int main(void)
{
    char line[MEDIASEARCH_LINE_MAX];
    char fold[MEDIASEARCH_LINE_MAX];

    /* ---- folding ---------------------------------------------------- */
    {
        CHECK(mediasearch_fold("ABBA", fold, sizeof(fold)) == 4 &&
              strcmp(fold, "abba") == 0, "ABBA folded to \"%s\"", fold);

        CHECK(mediasearch_fold("Kid A", fold, sizeof(fold)) == 5 &&
              strcmp(fold, "kid a") == 0, "got \"%s\"", fold);

        /* The separators are the format and cannot survive in a tag. */
        mediasearch_fold("a\tb\nc\rd", fold, sizeof(fold));
        CHECK(strcmp(fold, "a b c d") == 0, "separators: got \"%s\"", fold);

        /* Digits and punctuation are left alone. */
        mediasearch_fold("01. Track [Remix] (2019)", fold, sizeof(fold));
        CHECK(strcmp(fold, "01. track [remix] (2019)") == 0,
              "punctuation: got \"%s\"", fold);

        CHECK(mediasearch_fold("", fold, sizeof(fold)) == 0 && fold[0] == 0,
              "the empty string did not fold to itself");

        /* A property, not an example: every single byte folds to exactly
         * one byte -- a lone high byte is not UTF-8 and passes through --
         * so a byte is never turned into a separator. (5243: a whole code
         * point can get shorter, İ to i, and never longer.) */
        for (int c = 1; c < 256; c++) {
            const char in[2] = { (char)c, 0 };
            char out[4];
            CHECK(mediasearch_fold(in, out, sizeof(out)) == 1,
                  "byte 0x%02x did not fold to one byte", c);
        }

        /* 5243: casefold.h. The board's case: "BÔA" must find "Bôa".
         * 5256: the wants are decomposed (NFD) -- ô is o and U+0302 --
         * which is what the search file now holds. */
        static const struct { const char *in, *want; } uc[] = {
            { "Bôa", "bôa" }, { "BÔA", "bôa" },
            { "Björk", "björk" }, { "SIGUR RÓS", "sigur rós" },
            { "MÖTLEY CRÜE", "mötley crüe" }, { "Ÿ", "ÿ" },
            { "ŁÓDŹ", "łódź" }, { "ŽELJKO ČAĆIĆ", "željko čaćić" },
            { "İSTANBUL", "istanbul" }, { "ſ", "s" },
            { "ΜΆΝΟΣ ΧΑΤΖΙΔΆΚΙΣ", "μάνοσ χατζιδάκισ" }, { "ΟΔΟΣ", "οδοσ" },
            { "odoς", "odoσ" },
            { "ЖАННА АГУЗАРОВА", "жанна агузарова" }, { "ЁЛКА", "ёлка" },
            { "ՀԱՅԱՍՏԱՆ", "հայաստան" },
            { "ĐÀM VĨNH HƯNG", "đàm vĩnh hưng" }, { "Ơ", "ơ" },
            { "µ-Ziq", "μ-ziq" },
            { "ß", "ß" }, { "日本語", "日本語" }, { "Ƞ", "Ƞ" },
        };
        for (size_t i = 0; i < sizeof(uc) / sizeof(uc[0]); i++) {
            const int n = mediasearch_fold(uc[i].in, fold, sizeof(fold));
            CHECK(n == (int)strlen(uc[i].want) && strcmp(fold, uc[i].want) == 0,
                  "\"%s\" folded to \"%s\", wanted \"%s\"", uc[i].in, fold, uc[i].want);
        }

        /* Invalid UTF-8 passes through byte for byte: a Latin-1 "é"
         * (0xE9 alone), a truncated sequence, an overlong '/', a
         * surrogate. None may become a different character. */
        static const char *const raw[] = {
            "caf\xe9", "x\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xff\xfe",
        };
        for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
            const int n = mediasearch_fold(raw[i], fold, sizeof(fold));
            CHECK(n == (int)strlen(raw[i]) && memcmp(fold, raw[i], (size_t)n) == 0,
                  "invalid UTF-8 %zu was changed by folding", i);
        }

        /* The buffer bound holds for multi-byte output. */
        /* 5256: À is a and U+0300, three bytes. */
        CHECK(mediasearch_fold("ÀÀ", fold, 6) == -1, "6 bytes of output fitted in 6");
        CHECK(mediasearch_fold("ÀÀ", fold, 7) == 6 && strcmp(fold, "a\xcc\x80" "a\xcc\x80") == 0,
              "two À in 7 bytes: \"%s\"", fold);

        /* Folding is idempotent: what is folded folds to itself. */
        for (size_t i = 0; i < sizeof(uc) / sizeof(uc[0]); i++) {
            char again[64];
            mediasearch_fold(uc[i].want, again, sizeof(again));
            CHECK(strcmp(again, uc[i].want) == 0, "\"%s\" is not a fixed point", uc[i].want);
        }

        /* And nothing above ASCII is corrupted into a different
         * character: "Björk" survives, as b, j, o, U+0308, r, k. */
        const char bj[] = "Bj\xc3\xb6rk";
        mediasearch_fold(bj, fold, sizeof(fold));
        CHECK(strcmp(fold, "bjo\xcc\x88rk") == 0, "utf-8 mangled");     /* 5256: ö as o U+0308 */
        for (int c = 0x80; c < 256; c++) {
            const char in[2] = { (char)c, 0 };
            char out[4];
            mediasearch_fold(in, out, sizeof(out));
            CHECK((unsigned char)out[0] == (unsigned char)c,
                  "byte 0x%02x was changed to 0x%02x", c,
                  (unsigned char)out[0]);
        }

        /* 5256: normalisation. Each pair must fold to the same bytes:
         * a precomposed letter and its decomposed spelling, marks in
         * either order, Greek Extended with its capital, a Hangul
         * syllable and its jamo. And accents are kept: é is not e. */
        static const struct { const char *a, *b; bool same; } nf[] = {
            { "Béla", "Be\xcc\x81la", true },                   /* é, e U+0301 */
            { "BÉLA", "be\xcc\x81la", true },
            { "Tiến", "Tie\xcc\x82\xcc\x81n", true },         /* ế, e ^ ´ */
            { "ệ", "e\xcc\xa3\xcc\x82", true },               /* dot below, then ^ */
            { "ệ", "e\xcc\x82\xcc\xa3", true },               /* ^, then dot below */
            { "ᾏ", "ᾇ", true },                                /* Greek Extended fold */
            { "Ω", "Ω", true },                                /* U+2126, U+03A9 */
            { "Å", "Å", true },                                /* U+212B, U+00C5 */
            { "한", "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab", true },   /* 한 as jamo */
            { "Béla", "Bela", false },
            { "İ", "i", true },
        };
        for (size_t i = 0; i < sizeof(nf) / sizeof(nf[0]); i++) {
            char fa[64], fb[64];
            const int na = mediasearch_fold(nf[i].a, fa, sizeof(fa));
            const int nb = mediasearch_fold(nf[i].b, fb, sizeof(fb));
            CHECK(na >= 0 && nb >= 0 && (strcmp(fa, fb) == 0) == nf[i].same,
                  "normalisation pair %zu (%s / %s): %s", i, nf[i].a, nf[i].b,
                  nf[i].same ? "differ" : "the same");
        }

        /* No room is a refusal, not a truncation. */
        char tiny[4];
        CHECK(mediasearch_fold("abcdef", tiny, sizeof(tiny)) < 0,
              "a fold that did not fit was accepted");
    }

    /* ---- the line --------------------------------------------------- */
    {
        mediacat_rec_t r = rec("Radiohead/Kid A/01 Everything.flac",
                               "Everything In Its Right Place",
                               "Radiohead", "Kid A");

        const int n = mediasearch_encode(&r, 0x4a1f7, line, sizeof(line));
        CHECK(n > 0, "encode refused a good record");

        /* Written out by hand: eight lower-case hex digits, then nine
         * tab-separated folded fields, then a newline. 5264: genre, date,
         * album artist, track and disc between the album and the path --
         * the album artist, untagged, falling back to the artist. */
        const char *want =
            "0004a1f7\t"
            "everything in its right place\t"
            "radiohead\t"
            "kid a\t"
            "\t\t"
            "radiohead\t"
            "\t\t"
            "radiohead/kid a/01 everything.flac\n";
        CHECK(n == (int)strlen(want) && strcmp(line, want) == 0,
              "line was\n  \"%s\"\nwanted\n  \"%s\"", line, want);

        /* A tombstone has no line at all. */
        mediacat_rec_t d = r;
        d.deleted_at = 1790525389;
        CHECK(mediasearch_encode(&d, 0, line, sizeof(line)) < 0,
              "a buried track was given a search line");

        /* Paths the index refuses, refused here too. */
        mediacat_rec_t bad = r;
        bad.path[0] = '/';
        CHECK(mediasearch_encode(&bad, 0, line, sizeof(line)) < 0,
              "an absolute path was accepted");
        mediacat_rec_t empty = r;
        empty.path[0] = '\0';
        CHECK(mediasearch_encode(&empty, 0, line, sizeof(line)) < 0,
              "an empty path was accepted");

        /* Missing tags are empty fields, not a missing line: a track
         * with no tags is still indexed (medialib.c) and must still be
         * findable by its path. */
        mediacat_rec_t bare = rec("Odd/file.mp3", "", "", "");
        const int m = mediasearch_encode(&bare, 0, line, sizeof(line));
        CHECK(m > 0 && strcmp(line, "00000000\t\t\t\t\t\t\t\t\todd/file.mp3\n") == 0,
              "untagged: got \"%s\"", line);

        /* 5264: the five, folded like the rest, and an album artist of
         * its own rather than the fallback. */
        mediacat_rec_t ex = r;
        snprintf(ex.x.genre, sizeof(ex.x.genre), "Alternative ROCK");
        snprintf(ex.x.date, sizeof(ex.x.date), "2000-10-02");
        snprintf(ex.x.albumartist, sizeof(ex.x.albumartist), "Various Artists");
        snprintf(ex.x.track, sizeof(ex.x.track), "1/10");
        snprintf(ex.x.disc, sizeof(ex.x.disc), "1");
        const int q = mediasearch_encode(&ex, 1, line, sizeof(line));
        CHECK(q > 0 && strcmp(line, "00000001\teverything in its right place\tradiohead\tkid a\t"
                                    "alternative rock\t2000-10-02\tvarious artists\t1/10\t1\t"
                                    "radiohead/kid a/01 everything.flac\n") == 0,
              "5264 extras: got \"%s\"", line);
        {
            mediasearch_line_t pe;
            char nd[64];
            mediasearch_fold("Rock", nd, sizeof(nd));
            CHECK(mediasearch_parse(line, strlen(line), &pe) &&
                  mediasearch_match(&pe, MEDIASEARCH_GENRE, nd) &&
                  !mediasearch_match(&pe, MEDIASEARCH_TITLE, nd),
                  "5264: a genre search found the wrong field");
            mediasearch_fold("various", nd, sizeof(nd));
            CHECK(mediasearch_match(&pe, MEDIASEARCH_ALBUMARTIST, nd) &&
                  !mediasearch_match(&pe, MEDIASEARCH_ARTIST, nd),
                  "5264: album artist and artist are separate fields");
        }

        /* No room is a refusal. */
        char small[16];
        CHECK(mediasearch_encode(&r, 0, small, sizeof(small)) < 0,
              "a line that did not fit was accepted");
    }

    /* ---- parsing ---------------------------------------------------- */
    {
        const char *l = "0004a1f7\tthe title\tthe artist\tthe album\tg\td\taa\tt\tdi\tthe/path.flac\n";
        mediasearch_line_t p;
        CHECK(mediasearch_parse(l, strlen(l), &p), "a good line was refused");
        CHECK(p.cat_off == 0x4a1f7, "offset read as %u", p.cat_off);
        CHECK(p.len[0] == 9 && memcmp(p.f[0], "the title", 9) == 0, "title");
        CHECK(p.len[1] == 10 && memcmp(p.f[1], "the artist", 10) == 0, "artist");
        CHECK(p.len[2] == 9 && memcmp(p.f[2], "the album", 9) == 0, "album");
        CHECK(p.len[3] == 1 && p.f[3][0] == 'g', "genre (5264)");
        CHECK(p.len[5] == 2 && memcmp(p.f[5], "aa", 2) == 0, "album artist (5264)");
        CHECK(p.len[7] == 2 && memcmp(p.f[7], "di", 2) == 0, "disc (5264)");
        CHECK(p.len[8] == 13 && memcmp(p.f[8], "the/path.flac", 13) == 0, "path");

        /* With and without the newline, and with CRLF. */
        char noeol[128];
        snprintf(noeol, sizeof(noeol), "%.*s", (int)(strlen(l) - 1), l);
        CHECK(mediasearch_parse(noeol, strlen(noeol), &p),
              "a line without its newline was refused");
        char crlf[160];
        snprintf(crlf, sizeof(crlf), "%s\r\n", noeol);
        CHECK(mediasearch_parse(crlf, strlen(crlf), &p), "CRLF was refused");

        /* Empty fields are fields. */
        const char *e = "00000000\t\t\t\t\t\t\t\t\tp\n";
        CHECK(mediasearch_parse(e, strlen(e), &p) && p.len[0] == 0 &&
              p.len[8] == 1, "all-empty tags were refused");

        /* Not ours. */
        const char *bad[] = {
            "0004a1f\ta\tb\tc\t\t\t\t\t\td\n",       /* seven digits */
            "0004A1F7\ta\tb\tc\t\t\t\t\t\td\n",      /* upper case hex */
            "0004a1g7\ta\tb\tc\t\t\t\t\t\td\n",      /* not hex */
            "0004a1f7\ta\tb\tc\td\n",             /* four fields: an sr3 line */
            "0004a1f7\ta\tb\tc\t\t\t\t\td\n",        /* eight fields */
            "0004a1f7\ta\tb\tc\t\t\t\t\t\td\te\n",   /* ten fields */
            "0004a1f7 a\tb\tc\t\t\t\t\t\td\n",       /* space where a tab goes */
            "\n",
            "",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK(!mediasearch_parse(bad[i], strlen(bad[i]), &p),
                  "accepted a line that is not ours: \"%s\"", bad[i]);
        }
    }

    /* ---- matching --------------------------------------------------- */
    {
        mediacat_rec_t r = rec("Radiohead/Kid A/01 Everything.flac",
                               "Everything In Its Right Place",
                               "Radiohead", "Kid A");
        mediasearch_encode(&r, 7, line, sizeof(line));

        mediasearch_line_t p;
        CHECK(mediasearch_parse(line, strlen(line), &p), "parse of own line");

        /* Substring, and case-insensitive by virtue of both sides being
         * folded -- which is what MPD's `search` is. */
        mediasearch_fold("RIGHT place", fold, sizeof(fold));
        CHECK(mediasearch_match(&p, MEDIASEARCH_TITLE, fold),
              "a folded query did not match the title");

        mediasearch_fold("radiohead", fold, sizeof(fold));
        CHECK(mediasearch_match(&p, MEDIASEARCH_ARTIST, fold), "artist");
        CHECK(!mediasearch_match(&p, MEDIASEARCH_ALBUM, fold),
              "artist text matched the album field");
        CHECK(mediasearch_match(&p, MEDIASEARCH_FILE, fold),
              "the path holds the artist name and should match");
        CHECK(mediasearch_match(&p, MEDIASEARCH_ANY, fold), "any");

        /* A directory query, which is why the path is on the line. */
        mediasearch_fold("kid a/", fold, sizeof(fold));
        CHECK(mediasearch_match(&p, MEDIASEARCH_FILE, fold),
              "a folder prefix did not match the path");

        /* No match is no match. */
        mediasearch_fold("pablo honey", fold, sizeof(fold));
        CHECK(!mediasearch_match(&p, MEDIASEARCH_ANY, fold),
              "a needle that is not there matched");

        /* An unfolded query with capitals finds nothing, which is the
         * caller's bug and worth pinning down so it is not mistaken for
         * a matching bug later. */
        CHECK(!mediasearch_match(&p, MEDIASEARCH_ARTIST, "Radiohead"),
              "an unfolded query matched; folding is now optional?");

        /* The empty needle matches everything -- MPD's `search title ""`
         * is "every track that has a title field", which every line
         * has. */
        CHECK(mediasearch_match(&p, MEDIASEARCH_TITLE, ""),
              "the empty needle did not match");

        /* A field number that is not a field. */
        CHECK(!mediasearch_match(&p, MEDIASEARCH_FIELDS, "a"),
              "a bad field number matched");
    }

    /* ---- a scan over several lines ---------------------------------- */
    {
        static const struct { const char *path, *title, *artist, *album; } t[] = {
            { "A/One/01 a.flac",  "Alpha",  "Artist One", "One"   },
            { "A/One/02 b.flac",  "Beta",   "Artist One", "One"   },
            { "B/Two/01 c.flac",  "Gamma",  "Artist Two", "Two"   },
            { "B/Two/02 d.flac",  "Alpha",  "Artist Two", "Two"   },
        };

        /* Build the file the way medialib.c will: encode each record in
         * turn into one buffer. */
        char file[MEDIASEARCH_LINE_MAX * 8];
        size_t used = 0;
        uint32_t offs[4];
        for (int i = 0; i < 4; i++) {
            mediacat_rec_t r = rec(t[i].path, t[i].title, t[i].artist,
                                   t[i].album);
            offs[i] = (uint32_t)(100 + i);
            const int n = mediasearch_encode(&r, offs[i], file + used,
                                             sizeof(file) - used);
            CHECK(n > 0, "encode %d", i);
            used += (size_t)n;
        }

        /* Scan it as a reader would: split on '\n', parse, match. */
        mediasearch_fold("alpha", fold, sizeof(fold));
        int hits = 0;
        uint32_t hit_off[4] = { 0, 0, 0, 0 };
        const char *q = file;
        while (q < file + used) {
            const char *nl = strchr(q, '\n');
            const size_t len = nl ? (size_t)(nl - q + 1) : strlen(q);
            mediasearch_line_t p;
            if (mediasearch_parse(q, len, &p) &&
                mediasearch_match(&p, MEDIASEARCH_TITLE, fold)) {
                if (hits < 4) hit_off[hits] = p.cat_off;
                hits++;
            }
            q += len;
        }
        CHECK(hits == 2, "title=alpha matched %d lines, wanted 2", hits);
        CHECK(hit_off[0] == offs[0] && hit_off[1] == offs[3],
              "offsets came back %u,%u, wanted %u,%u",
              hit_off[0], hit_off[1], offs[0], offs[3]);

        /* And a whole-album query, which is the one a client asks to
         * fill a queue. */
        mediasearch_fold("two", fold, sizeof(fold));
        hits = 0;
        q = file;
        while (q < file + used) {
            const char *nl = strchr(q, '\n');
            const size_t len = nl ? (size_t)(nl - q + 1) : strlen(q);
            mediasearch_line_t p;
            if (mediasearch_parse(q, len, &p) &&
                mediasearch_match(&p, MEDIASEARCH_ALBUM, fold)) hits++;
            q += len;
        }
        CHECK(hits == 2, "album=two matched %d lines, wanted 2", hits);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
