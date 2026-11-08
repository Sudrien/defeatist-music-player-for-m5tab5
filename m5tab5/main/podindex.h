/*
 * podindex.h -- the listener's Podcast Index key: tried, then kept.
 *
 * 8006. The key and secret are typed into the remote's page (remote.c,
 * /podindex). Nothing is saved on the strength of the form: a task asks
 * api.podcastindex.org for categories/list with them, and only a real
 * answer stores them in NVS. From then on the state is WORKING, the
 * page shows the key read-only and the secret not at all, and a second
 * try is refused -- here, not only on the page.
 *
 * podauth.h has the signing and why the key is the listener's own.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "podcats.h"          /* 8014: podcat_t */
#include "podfeeds.h"         /* 8018: podfeed_t */
#include "podeps.h"           /* 8019: podep_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PODINDEX_NONE,          /* nothing saved, nothing tried */
    PODINDEX_CHECKING,      /* the task is asking */
    PODINDEX_WORKING,       /* saved; the form is closed */
    PODINDEX_REFUSED,       /* 401/403, the clock verified */
    PODINDEX_CLOCK,         /* refused, and NTP has not answered this boot */
    PODINDEX_UNREACHABLE,   /* no connection, or a status that is not 200/401/403 */
    PODINDEX_BAD_REPLY,     /* 200, but not "status":"true" */
    PODINDEX_FAILED,        /* no memory, no task, or NVS would not take it */
} podindex_state_t;

typedef enum {
    PODINDEX_TRY_STARTED,
    PODINDEX_TRY_INVALID,   /* podauth_text_ok() said no */
    PODINDEX_TRY_BUSY,      /* already checking */
    PODINDEX_TRY_LOCKED,    /* already WORKING */
    PODINDEX_TRY_FAILED,    /* could not start the task */
} podindex_try_t;

/* Once, on an internal-RAM stack, after nvsns_migrate(): reads NVS. */
void podindex_init(void);

/* The state, the saved key while WORKING (else ""), and the HTTP status
 * of the last refusal or 0. Any task. */
podindex_state_t podindex_state(char *key, size_t key_size, int *http);

/* Tries `key` and `secret`; copies both. Any task. */
podindex_try_t podindex_try(const char *key, const char *secret);

/*
 * 8014: Podcast Index's categories. NONE until something asks; a key
 * that works on the remote keeps the list its check fetched, so READY
 * follows straight on. FAILED keeps the last good list, if there was
 * one.
 */
typedef enum {
    PODINDEX_CATS_NONE,
    PODINDEX_CATS_LOADING,
    PODINDEX_CATS_READY,
    PODINDEX_CATS_FAILED,
} podindex_cats_t;

/* Fetch the list with the saved key, on a task of its own. False when
 * there is no working key or a fetch is already running. Any task. */
bool podindex_cats_fetch(void);

/* Up to `max` categories into `out` (NULL for none), how many in *n,
 * and the generation, which moves with every change to the list or its
 * state. Any task. */
podindex_cats_t podindex_cats(podcat_t *out, int max, int *n, uint32_t *gen);
uint32_t podindex_cats_gen(void);

/*
 * 8018: one category's podcasts (podcasts/trending), the last category
 * asked for. The same states as the categories; asking for another
 * category empties the list until it answers. One fetch of either kind
 * at a time: false while one runs, or with no working key.
 */
bool podindex_feeds_fetch(int cat);

/* 8131: the same list, from a search by name (search/byterm) instead of a
 * category: the "category" is PODINDEX_CAT_SEARCH, and the list is the term's
 * until something else is asked for. podindex_feeds_fetch(PODINDEX_CAT_SEARCH)
 * asks again with the last term. False for a term podfeeds_term_ok()
 * refuses, as the others for no key or a fetch already running. */
#define PODINDEX_CAT_SEARCH (-1)
bool podindex_search(const char *term);
podindex_cats_t podindex_feeds(podfeed_t *out, int max, int *n, int *cat, uint32_t *gen);
uint32_t podindex_feeds_gen(void);

/* 8019: one podcast's episodes (episodes/byfeedid), newest first, the
 * last podcast asked for. As the feeds, and under the same one-fetch
 * rule. Each carries its audio's URL. */
bool podindex_eps_fetch(int64_t feed);
podindex_cats_t podindex_eps(podep_t *out, int max, int *n, int64_t *feed, uint32_t *gen);
uint32_t podindex_eps_gen(void);

/*
 * 8128: one podcast by its feed's address (podcasts/byfeedurl), for a
 * feed a listener typed. One lookup at a time, under the same
 * one-fetch rule. FOUND carries the index's id and title; NOTFOUND is
 * an answer ("the index does not know that address"), FAILED is not.
 * podindex_lookup_take() hands the result over once and goes back to
 * NONE, so asking again starts afresh.
 */
typedef enum {
    PODINDEX_LOOK_NONE,
    PODINDEX_LOOK_LOADING,
    PODINDEX_LOOK_FOUND,
    PODINDEX_LOOK_NOTFOUND,
    PODINDEX_LOOK_FAILED,
} podindex_look_t;

/* False with no working key, another fetch running, or an address
 * podfeeds_url_ok() refuses. Any task. */
bool podindex_lookup(const char *feed_url);

/* The state, and for FOUND/NOTFOUND/FAILED the address asked about
 * (into url, if given). Does not clear it. Any task. */
podindex_look_t podindex_lookup_state(char *url, size_t url_size);

/* A finished result: the feed (FOUND) and the state, which goes back to
 * NONE. LOADING and NONE are returned as they are and left alone. */
podindex_look_t podindex_lookup_take(podfeed_t *out);

/*
 * 8130: a podcast the index does not list, by its feed's address. The
 * address gets an id (podrss.h's range, far above the index's) that
 * podindex_eps_fetch() takes like any other: it reads the feed itself and
 * the list is the same. The id is the address's own, so it is the same
 * after a reboot; what the id stands for is kept for the last few only,
 * so ask for the address again (here) before a fetch by its id.
 *
 * podindex_url_feed(): the id, and remembers the address; 0 for an
 * address podfeeds_url_ok() refuses. podindex_feed_url(): the address and
 * the feed's own title (empty until its episodes have been read) for an
 * id this still knows; false for one it does not. Any task.
 */
int64_t podindex_url_feed(const char *url);
bool podindex_feed_url(int64_t feed, char *url, size_t url_size, char *title, size_t title_size);

#ifdef __cplusplus
}
#endif
