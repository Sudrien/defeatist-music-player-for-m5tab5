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

        /* 5243: casefold.h. The board's case: "BÔA" must find "Bôa". */
        static const struct { const char *in, *want; } uc[] = {
            { "Bôa", "bôa" }, { "BÔA", "bôa" },
            { "Björk", "björk" }, { "SIGUR RÓS", "sigur rós" },
            { "MÖTLEY CRÜE", "mötley crüe" }, { "Ÿ", "ÿ" },
            { "ŁÓDŹ", "łódź" }, { "ŽELJKO ČAĆIĆ", "željko čaćić" },
            { "İSTANBUL", "istanbul" }, { "ſ", "s" },
            { "ΜΆΝΟΣ ΧΑΤΖΙΔΆΚΙΣ", "μάνοσ χατζιδάκισ" }, { "ΟΔΟΣ", "οδοσ" },
            { "odoς", "odoσ" },
            { "ЖАННА АГУЗАРОВА", "жанна агузарова" }, { "ЁЛКА", "ёлка" },
            { "ՀԱՅԱՍՏԱՆ", "հայաստան" },
            { "ĐÀM VĨNH HƯNG", "đàm vĩnh hưng" }, { "Ơ", "ơ" },
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
        CHECK(mediasearch_fold("ÀÀ", fold, 4) == -1, "4 bytes of output fitted in 4");
        CHECK(mediasearch_fold("ÀÀ", fold, 5) == 4 && strcmp(fold, "àà") == 0,
              "two À in 5 bytes: \"%s\"", fold);

        /* Folding is idempotent: what is folded folds to itself. */
        for (size_t i = 0; i < sizeof(uc) / sizeof(uc[0]); i++) {
            char again[64];
            mediasearch_fold(uc[i].want, again, sizeof(again));
            CHECK(strcmp(again, uc[i].want) == 0, "\"%s\" is not a fixed point", uc[i].want);
        }

        /* And nothing above ASCII is touched, so UTF-8 is not corrupted
         * into a different character. "Björk" must survive byte for
         * byte apart from the B. */
        const char bj[] = "Bj\xc3\xb6rk";
        mediasearch_fold(bj, fold, sizeof(fold));
        CHECK(strcmp(fold, "bj\xc3\xb6rk") == 0, "utf-8 mangled");
        for (int c = 0x80; c < 256; c++) {
            const char in[2] = { (char)c, 0 };
            char out[4];
            mediasearch_fold(in, out, sizeof(out));
            CHECK((unsigned char)out[0] == (unsigned char)c,
                  "byte 0x%02x was changed to 0x%02x", c,
                  (unsigned char)out[0]);
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

        /* Written out by hand: eight lower-case hex digits, then four
         * tab-separated folded fields, then a newline. */
        const char *want =
            "0004a1f7\t"
            "everything in its right place\t"
            "radiohead\t"
            "kid a\t"
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
        CHECK(m > 0 && strcmp(line, "00000000\t\t\t\todd/file.mp3\n") == 0,
              "untagged: got \"%s\"", line);

        /* No room is a refusal. */
        char small[16];
        CHECK(mediasearch_encode(&r, 0, small, sizeof(small)) < 0,
              "a line that did not fit was accepted");
    }

    /* ---- parsing ---------------------------------------------------- */
    {
        const char *l = "0004a1f7\tthe title\tthe artist\tthe album\tthe/path.flac\n";
        mediasearch_line_t p;
        CHECK(mediasearch_parse(l, strlen(l), &p), "a good line was refused");
        CHECK(p.cat_off == 0x4a1f7, "offset read as %u", p.cat_off);
        CHECK(p.len[0] == 9 && memcmp(p.f[0], "the title", 9) == 0, "title");
        CHECK(p.len[1] == 10 && memcmp(p.f[1], "the artist", 10) == 0, "artist");
        CHECK(p.len[2] == 9 && memcmp(p.f[2], "the album", 9) == 0, "album");
        CHECK(p.len[3] == 13 && memcmp(p.f[3], "the/path.flac", 13) == 0, "path");

        /* With and without the newline, and with CRLF. */
        char noeol[128];
        snprintf(noeol, sizeof(noeol), "%.*s", (int)(strlen(l) - 1), l);
        CHECK(mediasearch_parse(noeol, strlen(noeol), &p),
              "a line without its newline was refused");
        char crlf[160];
        snprintf(crlf, sizeof(crlf), "%s\r\n", noeol);
        CHECK(mediasearch_parse(crlf, strlen(crlf), &p), "CRLF was refused");

        /* Empty fields are fields. */
        const char *e = "00000000\t\t\t\tp\n";
        CHECK(mediasearch_parse(e, strlen(e), &p) && p.len[0] == 0 &&
              p.len[3] == 1, "all-empty tags were refused");

        /* Not ours. */
        const char *bad[] = {
            "0004a1f\ta\tb\tc\td\n",         /* seven digits */
            "0004A1F7\ta\tb\tc\td\n",        /* upper case hex */
            "0004a1g7\ta\tb\tc\td\n",        /* not hex */
            "0004a1f7\ta\tb\tc\n",           /* three fields */
            "0004a1f7\ta\tb\tc\td\te\n",     /* five fields */
            "0004a1f7 a\tb\tc\td\n",         /* space where a tab goes */
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
