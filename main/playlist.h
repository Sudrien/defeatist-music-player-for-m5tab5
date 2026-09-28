/*
 * playlist.h -- one directory's worth of playable files, in order.
 *
 * A folder is the unit rather than a saved list of arbitrary tracks,
 * because a folder is what an album is on disk. Nothing here is
 * persisted: the list is rebuilt from the directory whenever one is
 * chosen, so a file added on a desktop appears the next time that folder
 * is opened rather than after a rescan nobody remembered to run.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAY_ORDER_ONE = 0,     /* stop at the end of the track */
    PLAY_ORDER_ALL,         /* on to the next, stop at the end of the folder */
    PLAY_ORDER_SHUFFLE,     /* random, without repeating until exhausted */
    PLAY_ORDER_REPEAT_ONE,  /* this track again, until told otherwise */
} play_order_t;

/*
 * ONE and REPEAT_ONE are both about one track and they are opposites.
 *
 * ONE stops when the track ends; REPEAT_ONE plays it again. Neither is
 * about the skip button -- pressing next under either still moves to
 * the next track, because a press is not the end of a track. The names
 * are as close as the concepts, so both call sites map them the same
 * way and say why.
 */

/*
 * 5172: the lock, created here. Once, from app_main(), before any task
 * that uses the list. Before it, every function runs unlocked, which is
 * what a single-threaded host test wants.
 */
void playlist_init(void);

/*
 * 5172: hold the lock across several calls, for a reader that needs the
 * list and the cursor to agree -- or that reads mpdqueue.h directly, as
 * mpd.c does, which is reading the list without this file's lock unless
 * it holds this. Recursive, so the playlist_* calls inside are fine. Keep
 * it short: main_task waits on it at every track boundary.
 */
void playlist_lock(void);
void playlist_unlock(void);

/* Replace the list with the playable files in dir, sorted case-insensitively.
 *
 * Directories are not descended. An album that is one folder deep with
 * disc subfolders is two choices rather than one, which is the honest
 * rendering of what is on the card -- a recursive scan of a card root
 * would be a several-thousand-entry list and a long stall on the touch
 * that asked for it. */
esp_err_t playlist_load_dir(const char *dir);

/* Empty the list and forget the folder. */
void playlist_clear(void);

int playlist_count(void);

/*
 * NULL when i is out of range. The returned pointer is owned by the
 * playlist and is invalidated by the next playlist_load_dir().
 *
 * EVERY POINTER OUT OF THIS FILE IS BORROWED, AND THE LOAN IS SHORT.
 * This applies to playlist_path(), playlist_next(), playlist_prev() and
 * playlist_peek_next() alike: what comes back is s_paths[i] itself, and
 * playlist_clear() -- which every load begins with -- free()s it. A
 * caller may read it, and must copy it before doing anything that can
 * block, because what unblocks it may be the load.
 *
 * Every caller but one copies immediately into a request (request_track()
 * and player_loop() both snprintf it and are done). prefetch_next() was
 * the exception and held one across the whole tag-and-cover read; it
 * copies now. The rule is written here rather than only there because
 * the next caller will read the header, not that function -- the same
 * reason NETDEC_MIN_STACK stopped living inside an xTaskCreate call.
 *
 * The loan is only as safe as the mutation rule, which is today "ui_task
 * mutates, at a user's tap". A list that a socket can rewrite mid-track
 * makes a held pointer a use-after-free rather than a stale one.
 *
 * 5172: SO OFF ui_task, COPY UNDER THE LOCK. Every function here takes
 * playlist.c's lock, which keeps the list whole while it is read -- but a
 * borrowed pointer outlives the call, and the lock with it. The *_copy
 * readers below copy before letting go, and are what main_task and
 * media_task use. ui_task, which makes the edits, still borrows at the
 * sites that copy straight into a request; what it races is main_task's
 * load and clear, which is the race a folder tap has always had.
 */
const char *playlist_path(int i);

/* 5172: the entry at `i`, copied into `out`. False, `out` untouched, when
 * there is none. */
bool playlist_path_copy(int i, char *out, size_t size);

/* Index of path in the current list, or -1. Compares whole paths, so a
 * track chosen from a different folder does not match a same-named track
 * in this one. */
int playlist_index_of(const char *path);

/* Where playback is. -1 when the current track is not in the list --
 * which is the normal state for a single file chosen and then played
 * while the list holds something else. */
int playlist_current(void);
void playlist_set_current(int i);

/*
 * Next path to play under this order, or NULL to stop.
 *
 * Shuffle keeps a played-bitmap rather than picking uniformly at random,
 * so a twelve-track album plays twelve different tracks. The bitmap
 * clears when it fills, which makes the next pass a fresh shuffle rather
 * than a repeat of the same order.
 */
const char *playlist_next(play_order_t order);

/* 5172: playlist_next(), copied under the lock. False when it would have
 * returned NULL. */
bool playlist_next_copy(play_order_t order, char *out, size_t size);

/*
 * What playlist_next() would return, without consuming anything.
 *
 * For prefetch. Returns the CURRENT path under PLAY_ORDER_REPEAT_ONE,
 * which is the honest prediction and costs nothing -- the file is
 * already open and its sidecar already held, so priming it again is a
 * cache hit rather than a read. Returns NULL under PLAY_ORDER_ONE
 * (nothing follows) and,
 * deliberately, under PLAY_ORDER_SHUFFLE: shuffle's choice is made by
 * esp_random() at the moment it is asked, so there is no next track to
 * predict. Predicting one would mean fixing the choice early, which
 * turns "random when you get there" into "decided a song ago" and makes
 * the played-bitmap lie if the track is skipped. Shuffle simply does not
 * prefetch, and that is the honest answer rather than a missing feature.
 */
const char *playlist_peek_next(play_order_t order);

/* 5172: playlist_peek_next(), copied under the lock. False when it would
 * have returned NULL. */
bool playlist_peek_next_copy(play_order_t order, char *out, size_t size);

/*
 * Whether pressing next would do anything, for the UI to grey the button.
 *
 * Distinct from playlist_peek_next() != NULL, and the difference is the
 * whole reason it exists: under shuffle there is no predictable next
 * track, but there is certainly a next track, so peek returns NULL and
 * this returns true. Greying the button under shuffle would say the
 * playlist had ended when it had not.
 *
 * PLAY_ORDER_ONE is mapped to ALL, matching what UI_ACTION_NEXT actually
 * does: repeat-one governs what happens when a track ENDS, not what the
 * skip button means. Greying next in repeat-one would be wrong about a
 * button that works.
 */
bool playlist_has_next(play_order_t order);

/* Previous entry in list order, or NULL at the top. Shuffle deliberately
 * does not have a history: a back button that undoes a random choice
 * needs a stack, and the button is there to skip back one track. */
const char *playlist_prev(void);

/* The folder this list came from, or "" -- shown in the chooser. */
const char *playlist_dir(void);

/*
 * 5171: EDITING THE LIST (MPD.md step 6). The list is the queue
 * (mpdqueue.h), and these are the only way to change it besides a load
 * or a clear, because they move the cursor and the shuffle history with
 * the entries -- an mpdq_* call alone would leave both on the slots.
 * ui_task only, like everything else here that changes the list (5172:
 * which is a convention now, not what keeps the list whole -- the lock is).
 *
 * THE ENTRY PLAYING CAN BE REMOVED. It keeps playing; playlist_current()
 * becomes -1, the state for a file played from outside the list; and
 * next, peek_next, has_next and prev act as if the cursor sat in the gap
 * it left -- next is what slid into its place, prev what was before it.
 * Setting the current entry, a load or a clear forget the gap.
 *
 * playlist_dir() is not changed by an edit: it is the folder the list
 * was loaded from, and is only the log's and the chooser's name for it.
 */

/* Insert `path` at `at` (0..count), or at the end for a negative `at`.
 * The position, or -1: full (PLAYLIST_MAX), out of memory, `at` past the
 * end, or a path mpdq_insert() refuses. */
int playlist_add(const char *path, int at);

/* Insert `path` to play next: after the current entry, into the gap left
 * by a removed one, or at the end when there is neither. */
int playlist_add_next(const char *path);

/* Remove the entry at `pos`. False when there is none. */
bool playlist_remove(int pos);

/* Move the entry at `from` to `to`, the others closing up -- mpdq_move()'s
 * meaning. False when either is not a position. */
bool playlist_move(int from, int to);

/*
 * 5175: shuffle the list in place (MPD's `shuffle`). The entry playing
 * stays current wherever it lands; shuffle's played history starts again
 * with only it played; a gap is forgotten.
 */
void playlist_shuffle(void);

/* Largest number of tracks in one folder. A folder past this is truncated
 * and logs; the cap exists because each entry is a strdup of a path up to
 * 512 bytes, and an unbounded readdir on a card root is an unbounded
 * allocation on a touch event. */
#define PLAYLIST_MAX    (1024)

#ifdef __cplusplus
}
#endif
