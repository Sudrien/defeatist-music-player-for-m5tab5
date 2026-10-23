/*
 * podcats.h -- Podcast Index's categories/list, as rows.
 *
 * 8014. The reply is
 *
 *   {"status":"true","feeds":[{"id":1,"name":"Arts"},{"id":2,...}],
 *    "count":112,"description":"..."}
 *
 * -- one array of flat two-field objects. This walks that array and
 * hands each object to jsonpick.h for its name, which is the shape
 * jsonpick.h was written for: ONE flat object at a time. It is not a
 * parser either; it finds the array, splits it at the objects' braces
 * (quote-aware, so a brace inside a name does not split it), and reads
 * two fields from each. Nested objects are not expected and are not
 * handled: a `{` inside an object ends the walk there, and what was
 * read before it is kept.
 *
 * cJSON is in the build and was not used: its nodes come from plain
 * malloc(), which is internal RAM, a hundred-odd small allocations for
 * a list this size -- CLAUDE.md's "internal RAM belongs to the radio".
 *
 * NO FREERTOS AND NO ALLOCATION, so podcatstest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "jsonpick.h"

#define PODCATS_MAX      160    /* 112 in October 2026 */
#define PODCATS_NAME_MAX 48     /* "Religion & Spirituality" is the long one */

typedef struct {
    int  id;
    char name[PODCATS_NAME_MAX];
} podcat_t;

/* The end of the string starting at the quote `p` (the closing quote),
 * or NULL if it runs off `end`. */
static inline const char *podcats_str_end(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p;
    }
    return NULL;
}

/* The number after "id": in [obj, end), or -1. A key is a quoted "id"
 * followed by a colon; strings are skipped whole, so a name holding the
 * text "id" is not read as one. */
static inline int podcats_id(const char *obj, const char *end)
{
    for (const char *p = obj; p < end; p++) {
        if (*p != '"') continue;
        const char *q = podcats_str_end(p, end);
        if (!q) return -1;
        const bool is_id = (q - p == 3) && p[1] == 'i' && p[2] == 'd';
        const char *c = q + 1;
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) c++;
        if (is_id && c < end && *c == ':') {
            c++;
            while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')) c++;
            if (c >= end || *c < '0' || *c > '9') return -1;
            long v = 0;
            while (c < end && *c >= '0' && *c <= '9' && v < 100000000L) v = v * 10 + (*c++ - '0');
            return (int)v;
        }
        p = q;
    }
    return -1;
}

/*
 * The categories in `body`, in the reply's order, into `out`. Returns
 * how many; an object without a positive id or a string name is
 * skipped, and the walk stops at `max`, at the array's end, or at
 * anything that is not the shape above.
 */
static inline int podcats_parse(const char *body, size_t n, podcat_t *out, int max)
{
    if (!body || !out || max <= 0) return 0;
    const char *end = body + n;
    const char *p = body;
    /* "feeds" as a key: quoted, then a colon, then an array. */
    for (;;) {
        p = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!p) return 0;
        const char *q = podcats_str_end(p, end);
        if (!q) return 0;
        const char *c = q + 1;
        while (c < end && (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t')) c++;
        if (q - p == 6 && memcmp(p + 1, "feeds", 5) == 0 && c < end && *c == ':') {
            c++;
            while (c < end && (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t')) c++;
            if (c >= end || *c != '[') return 0;
            p = c + 1;
            break;
        }
        p = q + 1;
    }

    int count = 0;
    while (p < end && count < max) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        if (p >= end || *p != '{') break;         /* ']' or something else */
        const char *obj = p;
        const char *close = NULL;
        for (const char *s = p + 1; s < end; s++) {
            if (*s == '"') { s = podcats_str_end(s, end); if (!s) return count; continue; }
            if (*s == '{') return count;          /* nested: not this shape */
            if (*s == '}') { close = s; break; }
        }
        if (!close) break;
        const size_t len = (size_t)(close + 1 - obj);
        const int id = podcats_id(obj, close);
        if (id > 0 && jsonpick_string(obj, len, "name", out[count].name, sizeof(out[count].name))
            && out[count].name[0]) {
            out[count].id = id;
            count++;
        }
        p = close + 1;
    }
    return count;
}
