/*
 * tagextra.h -- 5262. The tags a library record keeps beyond title,
 * artist and album. Pure: no IDF, so cuedir.h and mediawalk.h can take it
 * without albumart.h's display types, and the host tests with them.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

/*
 * The tags the library keeps beyond title, artist and album -- for MPD's Genre,
 * Date, AlbumArtist, Track and Disc. A struct of its own rather than more
 * fields on id3_tags_t, which player.c keeps as stack locals on the decode
 * path and mediacache.c stores per slot: those want the three strings and
 * nothing else, and 128 more bytes on each would be paid for nothing.
 * Only the indexer asks for these.
 *
 * Strings, as MPD sends them: a track is "3" or "3/12", a date "1999" or
 * "1999-04-01", a genre whatever the tagger wrote -- except an ID3v1
 * genre number, which is turned into its name (id3_genre_name()).
 */
#define TAG_GENRE_LEN   (32)
#define TAG_DATE_LEN    (16)
#define TAG_NUM_LEN     (12)   /* "65535/65535" */
typedef struct {
    char genre[TAG_GENRE_LEN];
    char date[TAG_DATE_LEN];
    char albumartist[64];
    char track[TAG_NUM_LEN];
    char disc[TAG_NUM_LEN];
} tag_extra_t;


/* 5262: ID3v1's genre list, 0-79 as the spec numbers it and 80-191 as
 * Winamp extended it -- the numbers TCON's "(17)" and MP4's gnre name.
 * NULL past the end. */
const char *id3_genre_name(unsigned n);

/* 5262: a genre string as a tagger left it, into its name: "(17)" and
 * "17" become "Rock", "(17)Rock" becomes "Rock", and a name is left
 * alone. In place. */
void tag_genre_fix(char *g, size_t cap);
