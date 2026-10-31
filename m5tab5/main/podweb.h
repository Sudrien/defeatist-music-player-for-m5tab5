/*
 * podweb.h -- the remote's podcast lists, as JSON, and when to ask.
 *
 * 8033. The remote's page browses what the PODCAST tab browses --
 * categories, a category's podcasts, a podcast's episodes -- and starts
 * the same downloads. podindex.c keeps ONE list of each kind, the last
 * one asked for, and the screen reads the same lists. So a page that
 * fetched whenever its list was not the current one would take the
 * screen's away every time it polled, and the screen, asking again,
 * would take it back: two viewers trading one list once a second.
 *
 * The rule here stops that. The page sends `go` only when the listener
 * opened a list or asked again, never on a poll:
 *
 *  - nothing fetched yet (NONE): fetch, `go` or not -- there is nothing
 *    to take away;
 *  - the current list is another podcast's or category's: fetch with
 *    `go`; without it say OTHER, and the page says the player's screen
 *    asked for another list and offers to ask again;
 *  - the current list is this one: show it, LOADING or READY; FAILED
 *    shows as failed, and is fetched again with `go`.
 *
 * The categories are one list for everyone, so for them `same` is
 * always true.
 *
 * NO FREERTOS AND NO ALLOCATION, so podwebtest covers all of it.
 * remoteproto_json_str() is remoteproto.c's, linked by the test.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "podindex.h"           /* podindex_cats_t, and the three row types */

typedef enum {
    PODWEB_FETCH,               /* ask Podcast Index for it now */
    PODWEB_SHOW,                /* the list there is this one: send it */
    PODWEB_OTHER,               /* the list there is someone else's: say so */
} podweb_do_t;

static inline podweb_do_t podweb_decide(bool go, podindex_cats_t st, bool same)
{
    if (st == PODINDEX_CATS_NONE) return PODWEB_FETCH;
    if (!same) return go ? PODWEB_FETCH : PODWEB_OTHER;
    if (st == PODINDEX_CATS_FAILED && go) return PODWEB_FETCH;
    return PODWEB_SHOW;
}

/* The list's state as the page reads it. */
static inline const char *podweb_state_name(podindex_cats_t st)
{
    return st == PODINDEX_CATS_READY  ? "ready"  :
           st == PODINDEX_CATS_FAILED ? "failed" : "loading";
}

size_t remoteproto_json_str(const char *in, char *out, size_t cap);

/*
 * One row each, with a leading comma unless it is the first. `esc` is
 * scratch for the escaped title, at least PODWEB_ESC_MAX. Returns the
 * length written, 0 when it did not fit -- the caller sends no row then,
 * rather than half of one. `out` at least PODWEB_ROW_MAX holds any row.
 */
#define PODWEB_ESC_MAX (PODFEEDS_TITLE_MAX * 6 + 4)
#define PODWEB_ROW_MAX (PODWEB_ESC_MAX + 160)

static inline size_t podweb_put(char *out, size_t cap, int n)
{
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static inline size_t podweb_cat_row(const podcat_t *c, bool first, char *esc, char *out, size_t cap)
{
    if (!remoteproto_json_str(c->name, esc, PODWEB_ESC_MAX)) return 0;
    return podweb_put(out, cap, snprintf(out, cap, "%s{\"id\":%d,\"t\":%s}",
                                         first ? "" : ",", c->id, esc));
}

static inline size_t podweb_feed_row(const podfeed_t *f, bool first, char *esc, char *out, size_t cap)
{
    if (!remoteproto_json_str(f->title, esc, PODWEB_ESC_MAX)) return 0;
    return podweb_put(out, cap, snprintf(out, cap, "%s{\"id\":%" PRId64 ",\"t\":%s}",
                                         first ? "" : ",", f->id, esc));
}

/* An episode: id, title, date (unix seconds), length (seconds) and size
 * (bytes), each 0 when the feed did not say. Not the description, the
 * pictures or the URL: the page needs none of them to list or to ask,
 * and the description alone would make the reply a hundred kilobytes. */
static inline size_t podweb_ep_row(const podep_t *e, bool first, char *esc, char *out, size_t cap)
{
    if (!remoteproto_json_str(e->title, esc, PODWEB_ESC_MAX)) return 0;
    return podweb_put(out, cap, snprintf(out, cap,
        "%s{\"id\":%" PRId64 ",\"t\":%s,\"d\":%" PRId64 ",\"len\":%" PRId32 ",\"size\":%" PRId64 "}",
        first ? "" : ",", e->id, esc, e->published, e->duration, e->size));
}
