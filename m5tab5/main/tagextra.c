/*
 * tagextra.c -- 5262. ID3v1's genre names, and turning a tagger's genre
 * number into one. Pure, host-tested in texttest/tagextratest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tagextra.h"

static const char *const s_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
    "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
    "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
    "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock",
    /* 80-: Winamp's */
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebob",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
    "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock",
    "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech",
    "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass",
    "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
    "Duet", "Punk Rock", "Drum Solo", "A capella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore Techno",
    "Terror", "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat",
    "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
    "Thrash Metal", "Anime", "Jpop", "Synthpop", "Abstract", "Art Rock",
    "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo",
    "Dub", "EBM", "Eclectic", "Electro", "Electroclash", "Emo",
    "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth",
    "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock",
    "New Romantic", "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance",
    "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast",
    "Indie Rock", "G-Funk", "Dubstep", "Garage Rock", "Psybient",
};

const char *id3_genre_name(unsigned n)
{
    return n < sizeof(s_genres) / sizeof(s_genres[0]) ? s_genres[n] : NULL;
}

void tag_genre_fix(char *g, size_t cap)
{
    if (!g || !g[0] || !cap) return;
    const char *p = g;
    const bool paren = (*p == '(');
    if (paren) p++;
    if (*p < '0' || *p > '9') return;
    unsigned n = 0;
    while (*p >= '0' && *p <= '9' && n < 1000) n = n * 10 + (unsigned)(*p++ - '0');
    if (paren) { if (*p != ')') return; p++; }
    /* A bare number is a genre number only as the whole string: "80s
     * Pop" is a name. "(17)Rock" is a number and then the tagger's own
     * words, which are kept. */
    if (*p) {
        if (paren) memmove(g, p, strlen(p) + 1);
        return;
    }
    const char *name = id3_genre_name(n);
    if (name) snprintf(g, cap, "%s", name);
}

