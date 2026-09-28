/*
 * mpdqueue.h -- a list somebody built, with ids and a version.
 *
 * MPD.md step 4, and the thing playlist.c cannot be turned into by
 * adding functions to it. playlist.c holds a FOLDER: load_dir() reads a
 * directory, filters and sorts it, and every load re-sorts, so an order
 * a listener chose cannot survive. This holds a list that was built --
 * tracks from anywhere on either volume, in the order someone put them
 * in, each with an id that survives a move.
 *
 * NOT A PROTOCOL MODULE. MPD is why the shape is this shape, but the
 * queue is the device's: the remote page's add/reorder verbs (MPD.md
 * step 6) go through the same list, and this file names nothing from
 * either protocol. It is a list.
 *
 * WHAT IS NOT HERE: where playback is. That stays with the player and
 * playlist.c's cursor, because the decoder, the three rings and the
 * prefetch all already agree about it and a second opinion is how the
 * 1200 series was spent. MPD's `song` and `songid` are answered by
 * asking the player which entry it is on, not by a field in here.
 *
 * OWNERSHIP: THIS MODULE HAS NO LOCK, deliberately, like playlist.c.
 * One task mutates -- ui_task, draining requests the way it drains
 * remote_take_open() -- and a socket task asks rather than calls. MPD.md
 * has the argument. What this file does provide is the thing that makes
 * that safe to get wrong: mpdq_path() hands out a pointer the queue
 * owns, and the rule is the same as playlist.h's, written below.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The same ceiling as PLAYLIST_MAX, and for the same reasons: PSRAM is
 * already carrying a 256 KB netstream ring, the framebuffer, two cover
 * entries at ~142 KB and the decoders' scratch (MEDIA-INDEX.md,
 * "Memory"). A deeper queue is a number to raise on purpose, not a
 * thing to discover by filling it.
 */
#define MPDQ_MAX        (1024)

/* Longest path the rest of the player allows, mediaindex.h's limit plus
 * a mount. Kept here rather than included from there so this file has
 * no dependency but stdint. */
#define MPDQ_PATH_MAX   (512)

/*
 * Ready the queue. Idempotent, and false only if the array could not be
 * allocated -- a device with no queue, which the caller should report
 * rather than work around.
 */
bool mpdq_init(void);

/* How many entries. 0 before mpdq_init(). */
int mpdq_count(void);

/*
 * The version, which is what makes MPD's `plchanges` answerable.
 *
 * It starts at 1 and rises by one on every change. Each ENTRY also
 * carries the version at which its slot last changed, so a client that
 * saw version V asks for the entries with a later one.
 *
 * THE SUBTLE PART, AND THE ONE AN IMPLEMENTATION GETS WRONG: an entry's
 * version moves when its POSITION changes, not only when it is the
 * entry being touched. Removing the first of ten tracks moves nine
 * others, and a client told only that entry 0 changed draws nine rows
 * in the wrong places. So insert, remove and move stamp everything they
 * shifted.
 */
uint32_t mpdq_version(void);

/* Empty it. The version still rises: a client that saw the old list has
 * to be told the new one is empty. */
void mpdq_clear(void);

/*
 * Add at the end, or at `pos` (0..count, where count is "at the end").
 *
 * Return the position, or -1 when the queue is full, the path is empty
 * or too long, or `pos` is not a position. `id_out` may be NULL.
 *
 * IDS ARE NEVER REUSED WITHIN A BOOT. MPD's songid means "this entry,
 * wherever it goes", so it survives a move and a neighbour's removal,
 * and no other entry inherits it. Across a reboot the queue is gone
 * anyway, so a 32-bit counter is not a thing that wraps.
 */
int mpdq_append(const char *path, uint32_t *id_out);
int mpdq_insert(int pos, const char *path, uint32_t *id_out);

/* Remove by position or by id. False when there is no such entry. */
bool mpdq_remove(int pos);
bool mpdq_remove_id(uint32_t id);

/*
 * Move the entry at `from` so that it sits at `to`, the others closing
 * up behind it -- MPD's `move`, which is a move and not a swap. False
 * when either is not a position.
 */
bool mpdq_move(int from, int to);

/* Shuffle. A permutation: the same entries with the same ids, in a new
 * order, so a client's songids stay meaningful across it. */
void mpdq_shuffle(void);

/*
 * Sort by path with `cmp` (strcasecmp's shape), for a list that was
 * loaded from a folder and not built by a person (5165: playlist.c fills
 * the queue with a directory and wants it in the order the glass has
 * always shown). A permutation, like mpdq_shuffle(): entries keep their
 * ids. Only entries that actually moved get a new version, and a sort
 * that moves nothing leaves the version alone -- the range rule
 * bump_range() exists for. Not stable, as qsort is not: two paths `cmp`
 * calls equal come out in either order, which is also what playlist.c's
 * qsort did.
 */
void mpdq_sort(int (*cmp)(const char *a, const char *b));

/* The position of `id`, or -1. */
int mpdq_find_id(uint32_t id);

/*
 * The entry at `pos`.
 *
 * THE PATH IS BORROWED AND THE LOAN IS SHORT, exactly as in playlist.h:
 * what comes back is the queue's own copy, and anything that removes or
 * replaces that entry frees it. A caller may read it, and must copy it
 * before doing anything that can block -- because what unblocks it may
 * be the removal. 5126 is what that mistake looks like when the list
 * can only be changed by a tap; a list a socket can rewrite makes it a
 * use-after-free rather than a stale read.
 */
const char *mpdq_path(int pos);
uint32_t    mpdq_id(int pos);
uint32_t    mpdq_entry_version(int pos);

#ifdef __cplusplus
}
#endif
