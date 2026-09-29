/*
 * mpdfilter.h -- MPD 0.21's filter expressions, parsed and evaluated.
 * 5238.
 *
 * `find "(Artist == 'Beatles')"`, `search "((album contains 'road') AND
 * (!(title == 'Something')))"`: the form every 0.21 client sends once the
 * greeting says 0.21, and the reason the greeting stayed at 0.20 until
 * now (mpdproto.h, MPDPROTO_VERSION). The older TAG VALUE pairs stay
 * mpd.c's; this is the parenthesised form only.
 *
 * THE GRAMMAR, as MPD 0.21's doc/protocol.rst gives it and
 * src/song/Filter.cxx parses it:
 *
 *   expr   := '(' '!' expr ')'
 *           | '(' expr { 'AND' expr } ')'
 *           | '(' 'base' STRING ')'
 *           | '(' 'modified-since' STRING ')'
 *           | '(' 'AudioFormat' op STRING ')'
 *           | '(' TAG op STRING ')'
 *   op     := '==' | '!=' | 'contains' | '=~' | '!~'
 *   STRING := '"' ... '"' | '\'' ... '\'', a backslash escaping the
 *             next byte, whatever it is
 *
 * Whitespace separates tokens and is otherwise ignored. A word is
 * letters, digits, '-' and '_'. TAG names are matched without regard to
 * case, as MPD's tag_name_parse_i() matches them.
 *
 * WHAT THIS DEVICE CAN ANSWER is what its catalog holds: title, artist,
 * album and the path (MEDIA-INDEX.md). So:
 *   - Title, Artist, Album, `file` and `any` are compared;
 *   - AlbumArtist is compared as Artist, as 5180's pairs do;
 *   - any other tag is one no song has: `==` and `contains` match
 *     nothing, `!=` matches everything -- MPD's answer for a song
 *     without that tag, which here is every song;
 *   - `base` is a folder prefix of the URI, exactly;
 *   - `=~`, `!~`, `modified-since` and `AudioFormat` are REFUSED at parse
 *     time with the reason. There is no regex library on this target,
 *     the catalog keeps no stamps a client could compare against, and
 *     no format. A refusal says so; a match that silently ignored them
 *     would answer a different question.
 *
 * CASE. find's commands compare exactly; search's fold case, as 0.21's
 * doc says ("find commands are case sensitive, which search and related
 * commands ignore case") -- 5244: with casefold.h, the folding the
 * search file is written with (5243), accented letters included.
 *
 * PURE: no ESP-IDF, no allocation. The tree lives in the caller's
 * mpdfilter_t, and the values are unescaped into its own buffer.
 * Host-tested in texttest/mpdfiltertest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fields a song is compared on, in this order in mpdfilter_eval(). */
enum {
    MPDF_TITLE = 0,
    MPDF_ARTIST,
    MPDF_ALBUM,
    MPDF_FILE,              /* the URI */
    MPDF_NFIELDS,
    MPDF_ANY = -1,          /* every field */
    MPDF_NONE = -2,         /* a tag no song here has */
};

typedef enum {
    MPDF_N_CMP,             /* field op value */
    MPDF_N_BASE,            /* the URI is below `value` */
    MPDF_N_NOT,
    MPDF_N_AND,
} mpdf_kind_t;

typedef enum { MPDF_EQ, MPDF_NE, MPDF_CONTAINS } mpdf_op_t;

typedef struct {
    mpdf_kind_t kind;
    mpdf_op_t   op;         /* CMP */
    int         field;      /* CMP: MPDF_TITLE.., MPDF_ANY or MPDF_NONE */
    const char *value;      /* CMP, BASE: into mpdfilter_t.buf */
    int         child;      /* NOT, AND: the first child, or -1 */
    int         next;       /* the next sibling under an AND, or -1 */
} mpdf_node_t;

/*
 * How deep and how wide an expression may be. A client's filter is a
 * handful of terms -- Cantata's widest is an AND of four -- and these
 * bound the static tree, not the language.
 */
#define MPDF_MAX_NODES      (32)
#define MPDF_MAX_DEPTH      (8)
#define MPDF_BUF            (1024)

typedef struct {
    mpdf_node_t node[MPDF_MAX_NODES];
    int         n;
    int         root;
    char        buf[MPDF_BUF];     /* the unescaped values */
    size_t      used;
    /* On failure: a static message, and whether it is a refusal (a
     * feature this device does not have) rather than a syntax error. */
    const char *err;
    bool        unsupported;
} mpdfilter_t;

/*
 * Parse one expression, the whole of `s`. True with `f` ready for
 * mpdfilter_eval(). False with f->err set: a syntax error (MPD answers
 * those with ACK_ERROR_ARG) or, with f->unsupported, a well-formed
 * expression asking for something this device cannot compare.
 */
bool mpdfilter_parse(const char *s, mpdfilter_t *f);

/*
 * Whether a song matches. `field` is MPDF_NFIELDS strings -- title,
 * artist, album, URI -- each "" when the song has none. `fold` is
 * search's case folding; false is find's exact comparison.
 */
bool mpdfilter_eval(const mpdfilter_t *f, const char *const field[MPDF_NFIELDS], bool fold);

/*
 * The terms a match cannot do without, for a cheap first pass: the
 * `==` and `contains` comparisons on a held field reachable from the
 * root through ANDs only, never through a NOT. Each such value must
 * appear, as a folded substring, in that field of any song that
 * matches -- so a line whose search-file field lacks it can be skipped
 * before its catalog record is read. Writes up to `max` node indices
 * into `out` and returns how many. Zero is fine: every song is read.
 */
int mpdfilter_required(const mpdfilter_t *f, int *out, int max);

#ifdef __cplusplus
}
#endif
