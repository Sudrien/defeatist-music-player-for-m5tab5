/*
 * poddl.h -- one podcast episode, downloaded to a volume.
 *
 * 8020. The episode's enclosure URL is fetched on a task of its own and
 * written to <dir>/<base>.<ext>.part, then renamed to <base>.<ext> when
 * the last byte is in, so a file without ".part" is always whole. The
 * extension is decided by podpath.h from the URL, else the reply's
 * Content-Type, else mp3. Redirects are followed by hand -- tracking
 * prefixes stack two or three deep on most podcast hosts -- with http
 * allowed at the far end, as netstream allows it for audio (0418).
 *
 * One at a time. A second start while one runs is refused; the first
 * goes on in the background whatever the screen does.
 *
 * Writes go through storage_io under the BACKGROUND lease, one chunk
 * at a time, so playback reading the same card wins every wait.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PODDL_IDLE,
    PODDL_RUNNING,
    PODDL_DONE,         /* the file is whole, at `path` */
    PODDL_FAILED,
} poddl_state_t;

/* Start `url` into `dir` (made if missing) as `base` plus an extension.
 * `id` is the caller's, reported back so a row can tell it is its own.
 * 8021: `feed` and `show` are the podcast's, written beside the episodes
 * as podpath.h's PODPATH_LINK so the folder leads back to them.
 * False if one is running or the task could not start. Any task. */
bool poddl_start(int64_t id, const char *url, const char *dir, const char *base,
                 int64_t feed, const char *show);

/* The current or last download: its id, bytes so far and in all (-1 if
 * the server did not say), the finished file's path, and a generation
 * that moves with every change worth redrawing (about every 1%). */
poddl_state_t poddl_state(int64_t *id, int64_t *got, int64_t *total,
                          char *path, size_t path_size, uint32_t *gen);

/* The path of an already-downloaded <dir>/<base>.<ext> for any extension
 * podpath.h knows, into `out`. False if there is none. Touches the
 * filesystem: not from a task that must not block on the card. */
bool poddl_find(const char *dir, const char *base, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
