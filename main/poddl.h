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
    PODDL_FAILED,       /* 8024: its part file is kept, for the next start */
    PODDL_PAUSED,       /* 8024: stopped on request, part file kept */
} poddl_state_t;

/* Start `url` into `dir` (made if missing) as `base` plus an extension.
 * `id` is the caller's, reported back so a row can tell it is its own.
 * 8021: `feed` and `show` are the podcast's, written beside the episodes
 * as podpath.h's PODPATH_LINK so the folder leads back to them.
 * 8027: `cover` (the podcast's picture) and `own` (the episode's) are
 * URLs or "" / NULL; after the episode is whole, the first is kept as
 * cover.<ext> if the folder has no cover, the second -- when it is not
 * the first -- as folderart.h's ".<file>.<ext>" beside the episode.
 * False if one is running or the task could not start. Any task. */
bool poddl_start(int64_t id, const char *url, const char *dir, const char *base,
                 int64_t feed, const char *show, const char *cover, const char *own);

/*
 * 8024: stop at the next chunk and keep the part file (pause), or stop
 * and remove it (cancel). A start of the same episode -- the same dir
 * and base -- resumes a kept part with an HTTP Range request, after a
 * pause, a failure or a reboot alike. Cancel on a paused or failed
 * download removes its part file at once. Any task.
 */
void poddl_pause(void);
void poddl_cancel(void);

/* 8024: whether <dir>/<base>.part is there, and its size. Touches the
 * filesystem, as poddl_find() does. */
bool poddl_part(const char *dir, const char *base, int64_t *size);

/* The current or last download: its id, bytes so far and in all (-1 if
 * the server did not say), the finished file's path, and a generation
 * that moves with every change worth redrawing (8024: every 0.1 MB). */
poddl_state_t poddl_state(int64_t *id, int64_t *got, int64_t *total,
                          char *path, size_t path_size, uint32_t *gen);

/* The path of an already-downloaded <dir>/<base>.<ext> for any extension
 * podpath.h knows, into `out`. False if there is none. Touches the
 * filesystem: not from a task that must not block on the card. */
bool poddl_find(const char *dir, const char *base, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
