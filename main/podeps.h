/*
 * podeps.h -- Podcast Index's episodes/byfeedid, as rows.
 *
 * 8019. The reply is
 *
 *   {"status":"true","items":[{"id":4235,"title":"...","link":"...",
 *     "description":"...","datePublished":1700000000,
 *     "enclosureUrl":"https://.../ep.mp3","enclosureType":"audio/mpeg",
 *     "duration":3021,"persons":[{...}], ...}, ...],
 *    "count":40,"query":"6676981","description":"..."}
 *
 * The same walk as podfeeds.h, whose helpers this uses -- depth-aware,
 * top-level keys only -- over "items" instead of "feeds", keeping four
 * more things: the audio's URL, its length in seconds, its date, and
 * (8024) its size in bytes as the feed declares it.
 *
 * A title too long is cut (podfeeds_str()); a URL too long is DROPPED,
 * and the episode with it, because a cut URL is a different address.
 * So is a URL that is not http(s), and an enclosure with no URL at all.
 *
 * NO FREERTOS AND NO ALLOCATION, so podepstest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "podfeeds.h"

#define PODEPS_MAX       60         /* the request asks for 40 */
#define PODEPS_TITLE_MAX PODFEEDS_TITLE_MAX
#define PODEPS_URL_MAX   512

typedef struct {
    int64_t id;
    int64_t published;              /* unix seconds, 0 if not given */
    int32_t duration;               /* seconds, 0 if not given */
    int64_t size;                   /* 8024: enclosureLength, bytes, 0 if not given */
    char    title[PODEPS_TITLE_MAX];
    char    url[PODEPS_URL_MAX];
} podep_t;

/* A non-negative integer value at `v`, or 0. */
static inline int64_t podeps_num(const char *v, const char *end)
{
    int64_t n = 0;
    if (!v) return 0;
    while (v < end && *v >= '0' && *v <= '9' && n < 100000000000LL) n = n * 10 + (*v++ - '0');
    return n;
}

/* The URL at `v` into `out`, true only if it fit whole and is http(s). */
static inline bool podeps_url(const char *v, const char *end, char *out, size_t cap)
{
    if (!v || !podfeeds_str(v, end, out, cap)) return false;
    const size_t n = strlen(out);
    /* podfeeds_str() cuts rather than fails; a string that filled the
     * buffer may have been cut, so it is not trusted as an address. */
    if (n == 0 || n + 1 >= cap) { out[0] = '\0'; return false; }
    if (strncmp(out, "http://", 7) != 0 && strncmp(out, "https://", 8) != 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/*
 * The episodes in `body`, in the reply's order (newest first). Returns
 * how many; one without a positive id, a non-empty title or a usable
 * URL is skipped.
 */
static inline int podeps_parse(const char *body, size_t n, podep_t *out, int max)
{
    if (!body || !out || max <= 0) return 0;
    const char *end = body + n;
    const char *p = body;
    while (p < end && podfeeds_ws(*p)) p++;
    if (p >= end || *p != '{') return 0;
    const char *arr = podfeeds_find(p, end, "items");
    if (!arr || *arr != '[') return 0;
    p = arr + 1;
    int count = 0;
    while (p < end && count < max) {
        while (p < end && (podfeeds_ws(*p) || *p == ',')) p++;
        if (p >= end || *p != '{') break;
        const char *close = podfeeds_skip(p, end);
        if (!close) break;                      /* cut short: keep what came before */
        podep_t *e = &out[count];
        e->id = podeps_num(podfeeds_find(p, close, "id"), close);
        const char *tv = podfeeds_find(p, close, "title");
        const bool titled = tv && podfeeds_str(tv, close, e->title, sizeof(e->title)) && e->title[0];
        const bool linked = podeps_url(podfeeds_find(p, close, "enclosureUrl"), close,
                                       e->url, sizeof(e->url));
        if (e->id > 0 && titled && linked) {
            e->duration = (int32_t)podeps_num(podfeeds_find(p, close, "duration"), close);
            e->published = podeps_num(podfeeds_find(p, close, "datePublished"), close);
            e->size = podeps_num(podfeeds_find(p, close, "enclosureLength"), close);
            count++;
        }
        p = close;
    }
    return count;
}
