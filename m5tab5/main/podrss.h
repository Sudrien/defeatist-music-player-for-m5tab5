/*
 * podrss.h -- a podcast's own RSS feed, as the rows Podcast Index would give.
 *
 * 8130. For a feed the index does not list, the player reads the feed
 * itself. This is the reading, and it is NOT an XML parser: it walks the
 * tags once, keeps the few it wants, and ignores the rest.
 *
 *  - Tags are matched by their written name ("itunes:image", "pubDate"),
 *    not by namespace, which is how feeds are written in practice.
 *  - The channel's title and picture come from the channel's own children
 *    (an <image> block's <title> is not the podcast's title); an item's
 *    from the item's own. <item>s are read only directly under <channel>.
 *  - Text is read to its end tag, CDATA sections verbatim, the rest with
 *    entities decoded -- and so are attribute values, which is what makes
 *    "&amp;" in an enclosure's address into "&".
 *  - A description is HTML either way (CDATA, or escaped), so it goes
 *    through podhtml_text() once decoded, as the index's do.
 *  - An item needs a title and an http(s) enclosure address that fits
 *    whole; otherwise it is skipped. Its id is a hash of its <guid>, else
 *    of the address, so the same episode has the same id each time.
 *  - pubDate is RFC 822 (the weekday optional, two- or four-digit year,
 *    GMT/UT/Z or +hhmm). itunes:duration is seconds or [HH:]MM:SS.
 *  - Of any number of items the newest `max` are kept, newest first: an
 *    oldest-first feed (a serial) is not cut to its oldest.
 *
 * Atom feeds are not read: there is no <channel>, so there are no items.
 * A reply cut short by the caller's buffer ends at the last whole item.
 *
 * NO FREERTOS AND NO ALLOCATION (the caller gives a scratch podep_t, which
 * is too big for a small stack), so podrsstest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "podeps.h"

typedef struct {
    char title[PODFEEDS_TITLE_MAX];
    char image[PODEPS_URL_MAX];
} podrss_info_t;

/* Feeds that are not the index's get ids from here up, so a feed id of
 * either kind says which it is. The index's are far below it. */
#define PODRSS_FEED_BASE ((int64_t)1 << 41)

static inline bool podrss_feed_id(int64_t id) { return id >= PODRSS_FEED_BASE; }

static inline uint64_t podrss_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* A stable id for a feed address: BASE plus 32 bits of its hash. */
static inline int64_t podrss_feed_for(const char *url)
{
    const uint64_t h = podrss_hash(url);
    return PODRSS_FEED_BASE + (int64_t)((h ^ (h >> 32)) & 0xFFFFFFFFULL);
}

/* An episode's id: 1 .. 2^48-1, from its guid or address. */
static inline int64_t podrss_ep_id(const char *key)
{
    const uint64_t h = podrss_hash(key);
    int64_t id = (int64_t)((h ^ (h >> 48)) & 0xFFFFFFFFFFFFULL);
    return id ? id : 1;
}

/* Drop a trailing half of a UTF-8 letter from out[0..n). Returns n. */
static inline size_t podrss_trim_utf8(char *out, size_t n)
{
    size_t lead = n;
    while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > 0) {
        const unsigned char c = (unsigned char)out[lead - 1];
        const size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n - (lead - 1) < k) n = lead - 1;
    }
    out[n] = '\0';
    return n;
}

/* s[0..len) with entities decoded, into out (NUL-terminated, cut at a
 * letter's edge when it does not fit). How many bytes were written. */
static inline size_t podrss_decode(const char *s, size_t len, char *out, size_t cap)
{
    if (!cap) return 0;
    const char *end = s + len;
    size_t n = 0;
    while (s < end && n + 1 < cap) {
        if (*s == '&') {
            char o[4];
            size_t w = 0;
            const size_t used = podhtml_entity(s, o, &w);
            if (used && s + used <= end) {
                if (n + w + 1 > cap) break;
                memcpy(out + n, o, w);
                n += w;
                s += used;
                continue;
            }
        }
        out[n++] = *s++;
    }
    return podrss_trim_utf8(out, n);
}

static inline const char *podrss_find(const char *p, const char *end, const char *needle)
{
    const size_t k = strlen(needle);
    for (; (size_t)(end - p) >= k; p++) if (!memcmp(p, needle, k)) return p;
    return NULL;
}

/* One past the '>' that ends the tag opening at `p` ('<'), quote-aware. */
static inline const char *podrss_tag_end(const char *p, const char *end)
{
    char q = 0;
    for (p++; p < end; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p + 1;
    }
    return NULL;
}

/* The value of attribute `name` in the tag [tag, tagend), decoded. */
static inline bool podrss_attr(const char *tag, const char *tagend, const char *name, char *out, size_t cap)
{
    const size_t k = strlen(name);
    for (const char *p = tag + 1; p + k < tagend; p++) {
        if (!(p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')) continue;
        if (memcmp(p, name, k) != 0) continue;
        const char *q = p + k;
        while (q < tagend && (*q == ' ' || *q == '\t')) q++;
        if (q >= tagend || *q != '=') continue;
        q++;
        while (q < tagend && (*q == ' ' || *q == '\t')) q++;
        if (q >= tagend || (*q != '"' && *q != '\'')) return false;
        const char *e = memchr(q + 1, *q, (size_t)(tagend - q - 1));
        if (!e) return false;
        podrss_decode(q + 1, (size_t)(e - q - 1), out, cap);
        return true;
    }
    return false;
}

/*
 * The text of the element whose start tag ended at `p`, up to its end tag:
 * CDATA verbatim, the rest entity-decoded, nested tags dropped, comments
 * skipped. Into `out`, cut to fit. One past the end tag, or NULL if the
 * body ends first.
 */
static inline const char *podrss_leaf(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0;
    if (cap) out[0] = '\0';
    while (p < end) {
        if (*p == '<') {
            if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
                const char *e = podrss_find(p + 9, end, "]]>");
                if (!e) return NULL;
                size_t k = (size_t)(e - (p + 9));
                if (cap && n + 1 + k > cap) k = cap - 1 - n;
                if (cap && k) { memcpy(out + n, p + 9, k); n += k; out[n] = '\0'; }
                p = e + 3;
                continue;
            }
            if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
                const char *e = podrss_find(p + 4, end, "-->");
                if (!e) return NULL;
                p = e + 3;
                continue;
            }
            if (p + 1 < end && p[1] == '/') {
                const char *g = memchr(p, '>', (size_t)(end - p));
                if (cap) podrss_trim_utf8(out, n);
                return g ? g + 1 : NULL;
            }
            const char *g = podrss_tag_end(p, end);
            if (!g) return NULL;
            p = g;
            continue;
        }
        const char *q = p;
        while (q < end && *q != '<') q++;
        if (cap && n + 1 < cap) n += podrss_decode(p, (size_t)(q - p), out + n, cap - n);
        p = q;
    }
    return NULL;
}

/* Control characters to spaces, runs of spaces to one, none at the ends. */
static inline void podrss_oneline(char *s)
{
    char *w = s;
    bool sp = true;
    for (const char *r = s; *r; r++) {
        const unsigned char c = (unsigned char)*r;
        if (c <= ' ') { if (!sp) { *w++ = ' '; sp = true; } continue; }
        *w++ = (char)c;
        sp = false;
    }
    if (w > s && w[-1] == ' ') w--;
    *w = '\0';
}

static inline int64_t podrss_days(int y, int m, int d)     /* days since 1970-01-01 */
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "Wed, 04 Nov 2026 08:00:00 +0100" and its relatives, as unix seconds;
 * 0 if it is not one. */
static inline int64_t podrss_date(const char *s)
{
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec" };
    while (*s && !(*s >= '0' && *s <= '9')) s++;
    int d = 0, digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 3) { d = d * 10 + (*s++ - '0'); digits++; }
    while (*s == ' ' || *s == '-') s++;
    int m = 0;
    for (int i = 0; i < 12; i++) if (!strncasecmp(s, mon[i], 3)) { m = i + 1; break; }
    if (!m || d < 1 || d > 31) return 0;
    while (*s && !(*s == ' ' || *s == '-')) s++;
    while (*s == ' ' || *s == '-') s++;
    int y = 0;
    digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 5) { y = y * 10 + (*s++ - '0'); digits++; }
    if (digits == 2) y += y < 70 ? 2000 : 1900;
    else if (digits != 4) return 0;
    int hh = 0, mm = 0, ss = 0;
    while (*s == ' ') s++;
    if (*s >= '0' && *s <= '9') {
        digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) { hh = hh * 10 + (*s++ - '0'); digits++; }
        if (*s == ':') {
            s++;
            while (*s >= '0' && *s <= '9') mm = mm * 10 + (*s++ - '0');
            if (*s == ':') { s++; while (*s >= '0' && *s <= '9') ss = ss * 10 + (*s++ - '0'); }
        }
    }
    if (hh > 23 || mm > 59 || ss > 60) return 0;
    while (*s == ' ') s++;
    int64_t off = 0;
    if ((*s == '+' || *s == '-') && s[1] >= '0' && s[1] <= '9') {
        const int sign = *s == '-' ? -1 : 1;
        int z = 0;
        digits = 0;
        for (s++; *s >= '0' && *s <= '9' && digits < 4; s++, digits++) z = z * 10 + (*s - '0');
        off = sign * ((z / 100) * 3600 + (z % 100) * 60);
    }
    const int64_t t = podrss_days(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss - off;
    return t > 0 ? t : 0;
}

/* "3021", "50:21", "1:02:03" (and a fraction on the seconds) in seconds. */
static inline int32_t podrss_duration(const char *s)
{
    int64_t part[3] = { 0, 0, 0 };
    int k = 0;
    while (*s == ' ') s++;
    for (; *s && k < 3; s++) {
        if (*s >= '0' && *s <= '9') { if (part[k] < 1000000) part[k] = part[k] * 10 + (*s - '0'); }
        else if (*s == ':') k++;
        else if (*s == '.') { while (s[1] >= '0' && s[1] <= '9') s++; }
        else break;
    }
    int64_t t = k == 0 ? part[0] : k == 1 ? part[0] * 60 + part[1] : part[0] * 3600 + part[1] * 60 + part[2];
    return t > 0x7fffffff ? 0x7fffffff : (int32_t)t;
}

static inline bool podrss_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && !strncasecmp(name, want, len);
}

/* An http(s) address that is whole: not empty, not at the buffer's edge. */
static inline bool podeps_url_plain(const char *u)
{
    const size_t n = strlen(u);
    return (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && n > 8 && n + 1 < PODEPS_URL_MAX;
}

#define PODRSS_DEPTH 8

/*
 * The feed in body[0..n): `info` its title and picture, `out` up to `max`
 * newest-first episodes, `scratch` room for one item. Returns how many
 * episodes. A feed with no channel gives 0.
 */
static inline int podrss_parse(const char *body, size_t n, podrss_info_t *info,
                               podep_t *out, int max, podep_t *scratch)
{
    if (!body || !info || !out || max <= 0 || !scratch) return 0;
    info->title[0] = '\0';
    info->image[0] = '\0';
    const char *p = body, *end = body + n;
    const char *nm[PODRSS_DEPTH];
    size_t nl[PODRSS_DEPTH];
    int depth = 0, count = 0;
    bool in_item = false, have_enc = false;
    char guid[160], tmp[96];

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        p = lt;
        if (end - p >= 2 && (p[1] == '?' || p[1] == '!')) {
            const char *e = NULL;
            if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) { e = podrss_find(p + 9, end, "]]>"); e = e ? e + 3 : NULL; }
            else if (end - p >= 4 && !memcmp(p, "<!--", 4)) { e = podrss_find(p + 4, end, "-->"); e = e ? e + 3 : NULL; }
            else if (p[1] == '?') { e = podrss_find(p + 2, end, "?>"); e = e ? e + 2 : NULL; }
            else e = podrss_tag_end(p, end);
            if (!e) break;
            p = e;
            continue;
        }
        if (end - p >= 2 && p[1] == '/') {
            const char *g = memchr(p, '>', (size_t)(end - p));
            if (!g) break;
            const char *name = p + 2;
            size_t len = 0;
            while (name + len < g && name[len] != ' ' && name[len] != '\t' && name[len] != '\n' && name[len] != '\r') len++;
            if (depth > 0) depth--;
            p = g + 1;
            if (in_item && podrss_is(name, len, "item")) {
                in_item = false;
                const bool ok = scratch->title[0] && have_enc && scratch->url[0];
                if (ok) {
                    scratch->id = podrss_ep_id(guid[0] ? guid : scratch->url);
                    podhtml_text(scratch->desc);
                    int slot = -1;
                    if (count < max) slot = count++;
                    else {
                        int oldest = 0;
                        for (int i = 1; i < count; i++) if (out[i].published < out[oldest].published) oldest = i;
                        if (scratch->published > out[oldest].published) slot = oldest;
                    }
                    if (slot >= 0) out[slot] = *scratch;
                }
            }
            continue;
        }
        const char *name = p + 1;
        size_t len = 0;
        while (name + len < end && name[len] != ' ' && name[len] != '\t' && name[len] != '\n' &&
               name[len] != '\r' && name[len] != '/' && name[len] != '>') len++;
        const char *te = podrss_tag_end(p, end);
        if (!te) break;
        const bool selfclose = te - p >= 2 && te[-2] == '/';
        const char *parent = depth > 0 && depth <= PODRSS_DEPTH ? nm[depth - 1] : NULL;
        const size_t pl = depth > 0 && depth <= PODRSS_DEPTH ? nl[depth - 1] : 0;
        const bool in_channel = parent && podrss_is(parent, pl, "channel");
        const char *leaf_to = NULL;
        bool leaf = false;

        if (!in_item) {
            if (in_channel && podrss_is(name, len, "item")) {
                in_item = true;
                have_enc = false;
                guid[0] = '\0';
                memset(scratch, 0, offsetof(podep_t, title));
                scratch->title[0] = scratch->url[0] = scratch->desc[0] = '\0';
                scratch->image[0] = scratch->feed_image[0] = '\0';
            } else if (in_channel && podrss_is(name, len, "title") && !info->title[0]) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, info->title, sizeof(info->title));
                podrss_oneline(info->title);
            } else if (in_channel && podrss_is(name, len, "itunes:image")) {      /* wins over <image> */
                char u[PODEPS_URL_MAX];
                if (podrss_attr(p, te, "href", u, sizeof(u)) && podeps_url_plain(u)) memcpy(info->image, u, strlen(u) + 1);
            } else if (depth >= 2 && depth <= PODRSS_DEPTH && podrss_is(name, len, "url") &&
                       podrss_is(parent, pl, "image") && podrss_is(nm[depth - 2], nl[depth - 2], "channel") &&
                       !info->image[0]) {
                char u[PODEPS_URL_MAX];
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, u, sizeof(u));
                podrss_oneline(u);
                if (podeps_url_plain(u)) memcpy(info->image, u, strlen(u) + 1);
            }
        } else if (parent && podrss_is(parent, pl, "item")) {
            if (podrss_is(name, len, "title") && !scratch->title[0]) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, scratch->title, sizeof(scratch->title));
                podrss_oneline(scratch->title);
            } else if (podrss_is(name, len, "guid")) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, guid, sizeof(guid));
                podrss_oneline(guid);
            } else if (podrss_is(name, len, "pubDate") || podrss_is(name, len, "dc:date")) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, tmp, sizeof(tmp));
                if (!scratch->published) scratch->published = podrss_date(tmp);
            } else if (podrss_is(name, len, "itunes:duration")) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, tmp, sizeof(tmp));
                scratch->duration = podrss_duration(tmp);
            } else if (podrss_is(name, len, "enclosure") && !have_enc) {
                char u[PODEPS_URL_MAX], l[24];
                if (podrss_attr(p, te, "url", u, sizeof(u))) {
                    podrss_oneline(u);
                    if (podeps_url_plain(u)) {
                        memcpy(scratch->url, u, strlen(u) + 1);
                        have_enc = true;
                        scratch->size = podrss_attr(p, te, "length", l, sizeof(l)) ? podeps_num(l, l + strlen(l)) : 0;
                    }
                }
            } else if (podrss_is(name, len, "itunes:image")) {
                char u[PODEPS_URL_MAX];
                if (podrss_attr(p, te, "href", u, sizeof(u)) && podeps_url_plain(u))
                    memcpy(scratch->image, u, strlen(u) + 1);
            } else if ((podrss_is(name, len, "description") || podrss_is(name, len, "itunes:summary")) &&
                       !scratch->desc[0]) {
                leaf = true;
                if (!selfclose) leaf_to = podrss_leaf(te, end, scratch->desc, sizeof(scratch->desc));
            }
        }

        if (leaf) {
            if (selfclose) p = te;
            else if (!leaf_to) break;                 /* cut short in the middle of one */
            else p = leaf_to;
            continue;
        }
        if (!selfclose) {
            if (depth < PODRSS_DEPTH) { nm[depth] = name; nl[depth] = len; }
            depth++;
        }
        p = te;
    }

    for (int i = 0; i < count; i++) memcpy(out[i].feed_image, info->image, strlen(info->image) + 1);
    for (int i = 1; i < count; i++) {                 /* newest first; equal dates keep feed order */
        podep_t *t = scratch;
        *t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].published < t->published) { out[j + 1] = out[j]; j--; }
        out[j + 1] = *t;
    }
    return count;
}
