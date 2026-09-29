/*
 * mediasearch.h -- the derived search file: one line per live track.
 *
 * MEDIA-INDEX.md point 2, and the thing that note's closing paragraph
 * says the whole index was justified by: "the index is justified by
 * search, not by browsing." medialist.h answers lsinfo from the index a
 * folder at a time; MPD's `search` and `find` are whole-card questions
 * and there is nothing to seek into for them, because the answer is not
 * ordered by anything the query knows.
 *
 * So: a flat file, scanned start to finish, with no JSON parser and no
 * seeking. 20 000 tracks at ~200 bytes is 4 MB read sequentially, which
 * is what the arbiter is good at and what a per-track catalog decode is
 * not.
 *
 * DERIVED, LIKE THE INDEX. Rebuilt from the catalog in the same pass
 * that writes .ix2, never appended to, never migrated -- the version is
 * in the filename (medialib.h explains why) and a build that does not
 * know a file deletes it and writes its own.
 *
 * LIVE RECORDS ONLY. The index keeps tombstones because reconcile needs
 * to see the dead to revive them; a search does not, and a buried track
 * must not come back as a result. Nothing here has a dead flag because
 * nothing dead gets a line.
 *
 * HEADER-ONLY AND PURE, for the same reason medialist.h is: the format
 * and the matching are host-testable without a card, and the only thing
 * left for medialib.c is the fwrite.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "casefold.h"           /* 5243 */
#include "mediacat.h"
#include "mediaindex.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIASEARCH_NAME        ".defeatist.sr2"
#define MEDIASEARCH_TEMP_NAME   ".defeatist.srn"

/*
 * Search files an earlier build wrote, removed on the next run, the way
 * MEDIALIB_OLD_INDEX_NAMES works.
 *
 * sr1 folded ASCII only (5129-5138); sr2 folds with casefold.h (5243).
 * The lines are written folded, so an sr1 read by this build would miss
 * every accented match -- a new name is the rebuild, as for the index.
 * The automatic reindex after each mount writes the sr2.
 */
#define MEDIASEARCH_OLD_NAMES   { ".defeatist.sr1" }

/*
 * A line: the catalog offset, then four folded fields, tab-separated.
 *
 *     0004a1f7<TAB>title<TAB>artist<TAB>album<TAB>path<NL>
 *
 * Eight hex digits and not a decimal, so the offset is fixed width and
 * the fields start at a known column; lower case hex so the whole line
 * is one alphabet.
 *
 * WHY THE PATH IS IN HERE at all, when the offset would fetch it: MPD
 * searches `file` as a tag like any other, and a query that names a
 * directory ("everything under Live Albums/") is the common case for a
 * client browsing by path. Reading the catalog per line to answer it
 * would make the scan a random read per track, which is the thing this
 * file exists to avoid.
 */
#define MEDIASEARCH_LINE_MAX    (1024)
#define MEDIASEARCH_OFF_DIGITS  (8)

typedef enum {
    MEDIASEARCH_ANY = 0,        /* MPD's "any": every field */
    MEDIASEARCH_TITLE,
    MEDIASEARCH_ARTIST,
    MEDIASEARCH_ALBUM,
    MEDIASEARCH_FILE,
    MEDIASEARCH_FIELDS          /* not a field; the count */
} mediasearch_field_t;

/*
 * Folding: simple case folding by casefold.h, and tabs and newlines
 * turned into spaces.
 *
 * 5243: NOT ASCII ONLY ANY MORE. This said there was no case table for
 * anything else on this device and "Ä" would fold to itself; a board run
 * with a real music drive searched for "BÔA" and found no "Bôa". The
 * scripts a library is written in fold by ranges, not a table, and
 * casefold.h has them: Latin (Vietnamese included), Greek, Cyrillic,
 * Armenian. What this note warned against still holds -- byte-wise
 * |= 0x20 over UTF-8 corrupts it -- so the fold decodes code points,
 * and a byte that is not valid UTF-8 passes through as it is.
 *
 * The separator bytes have to go because they are the format: a tab in
 * a tag would add a field and a newline would add a record. Neither
 * belongs in a tag and both turn up in files written by hand.
 *
 * Returns the length written, or -1 if it would not fit.
 */
static inline int mediasearch_fold(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) return -1;
    size_t n = 0;
    const char *p = in;
    for (;;) {
        uint32_t c = casefold_next(&p);
        if (!c) break;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        char u[4];
        const int k = casefold_put(casefold_cp(c), u);
        if (n + (size_t)k + 1 > out_size) return -1;
        memcpy(out + n, u, (size_t)k);
        n += (size_t)k;
    }
    out[n] = '\0';
    return (int)n;
}

/*
 * The line for one live catalog record, '\n' included. Returns its
 * length, or -1 for a record that has no line: a tombstone, a path the
 * index would refuse, or a line that will not fit.
 */
static inline int mediasearch_encode(const mediacat_rec_t *r, uint32_t cat_off,
                                     char *out, size_t out_size)
{
    if (!r || !out) return -1;
    if (r->deleted_at) return -1;                  /* live records only */
    if (!r->path[0] || r->path[0] == '/') return -1;
    if (strlen(r->path) > MIDX_PATH_MAX) return -1;

    static const char hex[] = "0123456789abcdef";
    if (out_size < MEDIASEARCH_OFF_DIGITS + 1) return -1;
    for (int i = 0; i < MEDIASEARCH_OFF_DIGITS; i++) {
        out[i] = hex[(cat_off >> (4 * (MEDIASEARCH_OFF_DIGITS - 1 - i))) & 0xF];
    }

    size_t n = MEDIASEARCH_OFF_DIGITS;
    const char *const field[4] = { r->title, r->artist, r->album, r->path };

    for (int f = 0; f < 4; f++) {
        if (n + 1 >= out_size) return -1;
        out[n++] = '\t';
        const int w = mediasearch_fold(field[f], out + n, out_size - n);
        if (w < 0) return -1;
        n += (size_t)w;
    }

    if (n + 2 > out_size) return -1;
    out[n++] = '\n';
    out[n] = '\0';
    return (int)n;
}

/* One line, taken apart in place. Pointers into `line`, not copies. */
typedef struct {
    uint32_t    cat_off;
    const char *f[4];           /* title, artist, album, path */
    size_t      len[4];
    const char *body;           /* the fields, for MEDIASEARCH_ANY */
    size_t      body_len;
} mediasearch_line_t;

/*
 * Split a line. False for anything this format did not write: a short
 * offset, a non-hex digit, the wrong number of fields. A line that
 * fails is skipped by the caller and the file is not condemned for it
 * -- unlike the index, a search file with one bad line still answers
 * every other query correctly, and it is rebuilt on the next reconcile
 * anyway.
 *
 * `line` may or may not carry its '\n'; both are accepted, because a
 * reader that splits on newlines and one that hands over a whole line
 * are both reasonable and neither should have to care.
 */
static inline bool mediasearch_parse(const char *line, size_t len,
                                     mediasearch_line_t *out)
{
    if (!line || !out) return false;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len < MEDIASEARCH_OFF_DIGITS + 4) return false;   /* offset + 4 tabs */

    uint32_t off = 0;
    for (int i = 0; i < MEDIASEARCH_OFF_DIGITS; i++) {
        const char c = line[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return false;                 /* upper case hex is not ours */
        off = (off << 4) | d;
    }

    size_t p = MEDIASEARCH_OFF_DIGITS;
    out->body     = line + p + 1;
    out->body_len = len - p - 1;

    for (int f = 0; f < 4; f++) {
        if (p >= len || line[p] != '\t') return false;
        p++;
        const char *start = line + p;
        while (p < len && line[p] != '\t') p++;
        out->f[f]   = start;
        out->len[f] = (size_t)(line + p - start);
    }
    if (p != len) return false;            /* a fifth field is not ours */

    out->cat_off = off;
    return true;
}

/* Substring, over a length rather than a NUL. */
static inline bool mediasearch_contains_(const char *hay, size_t hn,
                                         const char *needle, size_t nn)
{
    if (nn == 0) return true;
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0) return true;
    }
    return false;
}

/*
 * Does this line match? `needle` must ALREADY BE FOLDED -- run the
 * query through mediasearch_fold() once, not per line.
 *
 * SUBSTRING, WHICH IS MPD's `search` AND NOT ITS `find`. The two differ:
 * search is a case-insensitive substring, find is an exact
 * case-sensitive equality. A folded file can only answer the first.
 *
 * So this is a FILTER AND NOT AN ANSWER for `find`: it narrows the card
 * to the handful of lines that could match, and the caller reads those
 * records out of the catalog at cat_off and compares the unfolded tags
 * itself. That two-stage shape is the reason the offset is on every
 * line, and it is why folding away case here costs nothing: nothing
 * that needs case is decided here.
 *
 * MEDIASEARCH_ANY matches the fields as one run, so a needle straddling
 * a tab boundary could match text that is in no single field. Accepted:
 * `any` is a client's "search everything" and a false positive there is
 * a row the user did not expect rather than a wrong answer, while the
 * alternative is four searches per line.
 */
static inline bool mediasearch_match(const mediasearch_line_t *l,
                                     mediasearch_field_t field,
                                     const char *needle)
{
    if (!l || !needle) return false;
    const size_t nn = strlen(needle);

    if (field == MEDIASEARCH_ANY) {
        return mediasearch_contains_(l->body, l->body_len, needle, nn);
    }
    if (field <= MEDIASEARCH_ANY || field >= MEDIASEARCH_FIELDS) return false;

    const int i = (int)field - 1;
    return mediasearch_contains_(l->f[i], l->len[i], needle, nn);
}

#ifdef __cplusplus
}
#endif
