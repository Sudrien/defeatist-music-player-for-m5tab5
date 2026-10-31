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
podindex_cats_t podindex_feeds(podfeed_t *out, int max, int *n, int *cat, uint32_t *gen);
uint32_t podindex_feeds_gen(void);

/* 8019: one podcast's episodes (episodes/byfeedid), newest first, the
 * last podcast asked for. As the feeds, and under the same one-fetch
 * rule. Each carries its audio's URL. */
bool podindex_eps_fetch(int64_t feed);
podindex_cats_t podindex_eps(podep_t *out, int max, int *n, int64_t *feed, uint32_t *gen);
uint32_t podindex_eps_gen(void);

#ifdef __cplusplus
}
#endif
