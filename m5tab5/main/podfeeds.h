/*
 * podfeeds.h -- Podcast Index's podcasts/trending, as rows.
 *
 * 8018. The reply is
 *
 *   {"status":"true","feeds":[{"id":75075,"url":"...","title":"...",
 *     "description":"...","author":"...","image":"...",
 *     "categories":{"104":"Tv","105":"Film"}, ...}, ...],
 *    "count":40,"max":"40","since":...,"description":"..."}
 *
 * Each feed has an object inside it ("categories"), which podcats.h's
 * walker stops at by design, and its strings are whatever a podcast's
 * publisher typed: any language, any length, escaped however the server
 * escapes. So this is one step past podcats.h, and still not a parser:
 *
 *  - It tracks depth, quote-aware, so a feed's end is its own closing
 *    brace and a key is read only at the feed's top level -- a
 *    "title" inside a nested object is not the feed's title.
 *  - Strings are unescaped whole: \" \\ \/ \b \f \n \r \t, and \uXXXX
 *    to UTF-8, surrogate pairs joined. A lone surrogate becomes U+FFFD.
 *    Control characters (a \n in a title) become spaces, since a row is
 *    one line.
 *  - A string longer than the buffer is CUT, at a character boundary,
 *    not refused: a long title is still the podcast's title. jsonpick.h
 *    refuses instead, which is right for a URL and wrong for this.
 *
 * Anything that is not that shape ends the walk; what was read before
 * it is kept. A reply cut short by the body buffer ends at the last
 * whole feed.
 *
 * NO FREERTOS AND NO ALLOCATION, so podfeedstest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PODFEEDS_MAX       60       /* the request asks for 40 */
#define PODFEEDS_TITLE_MAX 128      /* bytes, UTF-8, cut at a boundary */

typedef struct {
    int64_t id;
    char    title[PODFEEDS_TITLE_MAX];
} podfeed_t;

static inline bool podfeeds_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* The closing quote of the string opening at `p`, or NULL. */
static inline const char *podfeeds_str_end(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p;
    }
    return NULL;
}

/* The end of the value starting at `p` (one past it), or NULL: a string,
 * an object or array (nested, quote-aware), or a bare word or number. */
static inline const char *podfeeds_skip(const char *p, const char *end)
{
    if (p >= end) return NULL;
    if (*p == '"') { const char *q = podfeeds_str_end(p, end); return q ? q + 1 : NULL; }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < end; p++) {
            if (*p == '"') { p = podfeeds_str_end(p, end); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !podfeeds_ws(*p)) p++;
    return p;
}

static inline int podfeeds_hex4(const char *p, const char *end)
{
    if (end - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* Append code point `cp` to out[*n], if all of it fits in `cap` - 1. */
static inline bool podfeeds_put(char *out, size_t *n, size_t cap, uint32_t cp)
{
    char b[4];
    size_t k;
    if (cp < 0x80)         { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800)   { b[0] = (char)(0xC0 | cp >> 6);  b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63));
                             b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else                   { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63));
                             b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4; }
    if (*n + k + 1 > cap) return false;
    memcpy(out + *n, b, k);
    *n += k;
    return true;
}

/*
 * The string at `p` (its opening quote), unescaped into `out`, cut at a
 * character boundary when it does not fit. False if it is not a whole
 * string. Raw UTF-8 bytes are copied by sequence, so a cut never splits
 * one.
 */
static inline bool podfeeds_str(const char *p, const char *end, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (p >= end || *p != '"') return false;
    const char *q = podfeeds_str_end(p, end);
    if (!q) return false;
    size_t n = 0;
    bool full = false;
    for (p++; p < q && !full; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' && p + 1 < q) {
            const char e = p[1];
            uint32_t cp;
            p += 2;
            switch (e) {
            case 'n': case 'r': case 't': case 'b': case 'f': cp = ' '; break;
            case 'u': {
                const int h = podfeeds_hex4(p, q);
                if (h < 0) { cp = 0xFFFD; break; }
                p += 4;
                cp = (uint32_t)h;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    const int lo = (q - p >= 6 && p[0] == '\\' && p[1] == 'u') ? podfeeds_hex4(p + 2, q) : -1;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                        p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                } else if (cp < 0x20) {
                    cp = ' ';
                }
                break;
            }
            default: cp = (unsigned char)e; break;      /* \" \\ \/ and the rest */
            }
            full = !podfeeds_put(out, &n, cap, cp);
            continue;
        }
        if (c < 0x20) { full = !podfeeds_put(out, &n, cap, ' '); p++; continue; }
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (p + k > q) k = (size_t)(q - p);
        if (n + k + 1 > cap) { full = true; break; }
        memcpy(out + n, p, k);
        n += k;
        p += k;
    }
    out[n] = '\0';
    return true;
}

/* The value of top-level key `key` in the object [obj, end): a pointer
 * to its first character, or NULL. */
static inline const char *podfeeds_find(const char *obj, const char *end, const char *key)
{
    const size_t kl = strlen(key);
    const char *p = obj + 1;                    /* past the '{' */
    for (;;) {
        while (p < end && (podfeeds_ws(*p) || *p == ',')) p++;
        if (p >= end || *p != '"') return NULL; /* '}' or not this shape */
        const char *q = podfeeds_str_end(p, end);
        if (!q) return NULL;
        const bool hit = (size_t)(q - p - 1) == kl && memcmp(p + 1, key, kl) == 0;
        p = q + 1;
        while (p < end && podfeeds_ws(*p)) p++;
        if (p >= end || *p != ':') return NULL;
        p++;
        while (p < end && podfeeds_ws(*p)) p++;
        if (hit) return p;
        p = podfeeds_skip(p, end);
        if (!p) return NULL;
    }
}

/*
 * The feeds in `body`, in the reply's order. Returns how many; a feed
 * without a positive id or a non-empty title is skipped.
 */
static inline int podfeeds_parse(const char *body, size_t n, podfeed_t *out, int max)
{
    if (!body || !out || max <= 0) return 0;
    const char *end = body + n;
    const char *p = body;
    while (p < end && podfeeds_ws(*p)) p++;
    if (p >= end || *p != '{') return 0;
    const char *arr = podfeeds_find(p, end, "feeds");
    if (!arr || *arr != '[') return 0;
    p = arr + 1;
    int count = 0;
    while (p < end && count < max) {
        while (p < end && (podfeeds_ws(*p) || *p == ',')) p++;
        if (p >= end || *p != '{') break;
        const char *close = podfeeds_skip(p, end);
        if (!close) break;                      /* cut short: keep what came before */
        const char *idv = podfeeds_find(p, close, "id");
        int64_t id = 0;
        if (idv) {
            while (idv < close && *idv >= '0' && *idv <= '9' && id < 100000000000LL) id = id * 10 + (*idv++ - '0');
        }
        const char *tv = podfeeds_find(p, close, "title");
        if (id > 0 && tv && podfeeds_str(tv, close, out[count].title, sizeof(out[count].title))
            && out[count].title[0]) {
            out[count].id = id;
            count++;
        }
        p = close;
    }
    return count;
}
