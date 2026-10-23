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

#ifdef __cplusplus
}
#endif
