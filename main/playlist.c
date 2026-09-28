/*
 * playlist.c
 *
 * 5165, MPD.md step 4b: THE LIST IS THE QUEUE NOW. The paths live in
 * mpdqueue.c, and this file is what it always was apart from that -- the
 * folder half (read, filter, sort) and the cursor (where playback is,
 * the shuffle history, the folder's name), which mpdqueue.h keeps out of
 * the queue on purpose. A folder tap therefore replaces the queue, which
 * is what MPD.md asked a tap to mean, and the glass cannot tell:
 * texttest/playlisttest.c was written against the old file (5164) and is
 * passed unchanged.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "cuedir.h"
#include "decoder.h"
#include "mpdqueue.h"
#include "playlist.h"
#include "storage.h"

static const char *TAG = "tab5_playlist";

/* The queue is the list (5165); it must hold a whole folder. */
_Static_assert(PLAYLIST_MAX <= MPDQ_MAX, "a folder must fit in the queue");

static uint8_t *s_played;           /* shuffle bitmap, one bit per entry */
static int s_current = -1;
/*
 * 5171: where the list resumes after the entry playing was removed from
 * under it -- the position its successor slid into -- or -1. Only ever
 * set while s_current is -1; see playlist_remove().
 */
static int s_gap = -1;
static char s_dir[512];

/* The count and the paths are the queue's. Named so the logic below
 * reads as it did when they were this file's own array. */
#define s_count         (mpdq_count())
#define s_paths_at(i)   (mpdq_path(i))

void playlist_clear(void)
{
    mpdq_clear();
    s_current = -1;
    s_gap = -1;
    s_dir[0] = '\0';
    if (s_played) memset(s_played, 0, (PLAYLIST_MAX + 7) / 8);
}

esp_err_t playlist_load_dir(const char *dir)
{
    if (!dir || !*dir) return ESP_ERR_INVALID_ARG;

    if (!s_played) {
        s_played = heap_caps_calloc((PLAYLIST_MAX + 7) / 8, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mpdq_init() || !s_played) {
            ESP_LOGE(TAG, "out of memory for the track list");
            return ESP_ERR_NO_MEM;
        }
    }

    playlist_clear();

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "cannot open %s", dir);
        return ESP_ERR_NOT_FOUND;
    }

    /* Cue sheets: their tracks join the list as "X.cue#NN", and the
     * audio they cover leaves it -- see cuedir.h. NULL, the usual case,
     * changes nothing below. */
    cuedir_t *cues = cuedir_load(dir, STORAGE_IO_BACKGROUND);

    struct dirent *e;
    bool truncated = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        /* Before decoder_supports(), because the entries this catches
         * would pass it: an AppleDouble sidecar is called ._Track.mp3
         * and holds a resource fork. The chooser has always hidden
         * these; this list did not, so playing a folder written on a Mac
         * queued eighteen entries for nine tracks and every other one
         * decoded to nothing. */
        if (storage_is_hidden(e->d_name)) continue;
        if (!decoder_supports(e->d_name)) continue;
        if (cuedir_hides(cues, e->d_name)) continue;
        if (s_count >= PLAYLIST_MAX) { truncated = true; break; }

        char full[512];
        if (!storage_join_path(full, sizeof(full), dir, e->d_name)) {
            ESP_LOGW(TAG, "path too long, skipping: %s/%s", dir, e->d_name);
            continue;
        }
        /* The queue copies it, where strdup() did; -1 is its "no memory"
         * here, the other refusals being ruled out above. */
        if (mpdq_append(full, NULL) < 0) break;
    }
    closedir(d);

    for (int i = 0; i < cuedir_count(cues); i++) {
        if (s_count >= PLAYLIST_MAX) { truncated = true; break; }
        char full[512];
        if (!storage_join_path(full, sizeof(full), dir, cuedir_name(cues, i))) continue;
        if (mpdq_append(full, NULL) < 0) break;
    }
    cuedir_free(cues);

    if (truncated) {
        ESP_LOGW(TAG, "%s has more than %d tracks; the rest are ignored",
                 dir, PLAYLIST_MAX);
    }

    /* FatFs hands entries back in directory order, which is creation
     * order on most cards -- so an album copied track by track is roughly
     * right and an album copied by a tool that parallelises is not. Sort
     * rather than trust it. */
    mpdq_sort(strcasecmp);

    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    ESP_LOGI(TAG, "%s: %d track%s", dir, s_count, s_count == 1 ? "" : "s");
    return s_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int playlist_count(void) { return s_count; }

const char *playlist_path(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return s_paths_at(i);
}

int playlist_index_of(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_paths_at(i), path) == 0) return i;
    }
    return -1;
}

int playlist_current(void) { return s_current; }

void playlist_set_current(int i)
{
    s_current = (i >= 0 && i < s_count) ? i : -1;
    s_gap = -1;
    if (s_current >= 0 && s_played) {
        s_played[s_current / 8] |= (uint8_t)(1 << (s_current % 8));
    }
}

const char *playlist_dir(void) { return s_dir; }

static bool shuffle_seen(int i)
{
    return s_played && (s_played[i / 8] & (1 << (i % 8)));
}

const char *playlist_peek_next(play_order_t order)
{
    if (s_count <= 0) return NULL;
    if (order == PLAY_ORDER_REPEAT_ONE) {
        return (s_current >= 0) ? s_paths_at(s_current) : NULL;
    }
    if (order != PLAY_ORDER_ALL) return NULL;   /* see the header */

    /* 5171: the playing entry was removed; its successor is next. */
    if (s_current < 0 && s_gap >= 0) return s_gap < s_count ? s_paths_at(s_gap) : NULL;
    const int n = s_current + 1;
    if (s_current < 0 || n >= s_count) return NULL;
    return s_paths_at(n);
}

bool playlist_has_next(play_order_t order)
{
    if (s_count <= 0) return false;
    if (order == PLAY_ORDER_SHUFFLE) return true;   /* see the header */
    /* Repeat-one always has a next track: itself. The skip button is
     * mapped to ALL by the caller, so what this really answers is
     * "would pressing next do anything", and it would. */
    if (order == PLAY_ORDER_REPEAT_ONE) return s_current >= 0;
    if (s_current < 0 && s_gap >= 0) return s_gap < s_count;     /* 5171 */
    return s_current >= 0 && s_current + 1 < s_count;
}

const char *playlist_next(play_order_t order)
{
    if (s_count <= 0) return NULL;

    if (order == PLAY_ORDER_ONE) return NULL;

    /*
     * The same file again, without touching s_current.
     *
     * Returning the path rather than restarting the decoder from here
     * keeps this function what it is -- a question about the list --
     * and lets the player treat the repeat as an ordinary track change:
     * the sidecar is already held, the cover already cached, and 0702's
     * refusal to crossfade a track with itself is already in place for
     * exactly this case.
     */
    if (order == PLAY_ORDER_REPEAT_ONE) {
        return (s_current >= 0) ? s_paths_at(s_current) : NULL;
    }

    if (order == PLAY_ORDER_SHUFFLE) {
        int remaining = 0;
        for (int i = 0; i < s_count; i++) if (!shuffle_seen(i)) remaining++;

        if (remaining == 0) {
            /* Exhausted: start a fresh pass rather than replaying the
             * same order. Everything except the track just played, so the
             * wrap does not repeat it back to back. */
            memset(s_played, 0, (size_t)(PLAYLIST_MAX + 7) / 8);
            if (s_current >= 0 && s_count > 1) {
                s_played[s_current / 8] |= (uint8_t)(1 << (s_current % 8));
                remaining = s_count - 1;
            } else {
                remaining = s_count;
            }
        }

        int pick = (int)(esp_random() % (uint32_t)remaining);
        for (int i = 0; i < s_count; i++) {
            if (shuffle_seen(i)) continue;
            if (pick-- == 0) {
                playlist_set_current(i);
                return s_paths_at(i);
            }
        }
        return NULL;
    }

    /* 5171: after the playing entry was removed, its successor. */
    const int next = (s_current < 0 && s_gap >= 0) ? s_gap : s_current + 1;
    if (next >= s_count) return NULL;        /* stop at the end of the folder */
    playlist_set_current(next);
    return s_paths_at(next);
}

const char *playlist_prev(void)
{
    /* 5171: after the playing entry was removed, what was before it. */
    if (s_current < 0 && s_gap > 0 && s_gap <= s_count) {
        playlist_set_current(s_gap - 1);
        return s_paths_at(s_current);
    }
    if (s_count <= 0 || s_current <= 0) return NULL;
    playlist_set_current(s_current - 1);
    return s_paths_at(s_current);
}

/* ---- 5171: editing the list, keeping the cursor ------------------------ */

/*
 * The queue can be edited now (MPD.md step 6), and the queue's positions
 * are this file's cursor and shuffle bitmap. An edit through mpdq_*
 * alone would leave both pointing at whatever slid into their slots: the
 * glass would mark the wrong row as playing, next would skip or repeat,
 * and shuffle would call a new entry played. So every edit comes through
 * here, and the cursor and the bitmap follow the ENTRIES, not the slots.
 */

static bool played_at(int i)
{
    return s_played && (s_played[i / 8] & (1 << (i % 8)));
}

static void played_put(int i, bool on)
{
    if (!s_played) return;
    if (on) s_played[i / 8] |= (uint8_t)(1 << (i % 8));
    else    s_played[i / 8] &= (uint8_t)~(1 << (i % 8));
}

/* Where an entry at `i` is after one at `from` moves to `to`. */
static int moved_index(int i, int from, int to)
{
    if (i == from) return to;
    if (from < i && i <= to) return i - 1;
    if (to <= i && i < from) return i + 1;
    return i;
}

static bool edit_ready(void)
{
    if (!s_played) {
        s_played = heap_caps_calloc((PLAYLIST_MAX + 7) / 8, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return mpdq_init() && s_played;
}

int playlist_add(const char *path, int at)
{
    if (!edit_ready() || s_count >= PLAYLIST_MAX) return -1;
    const int n = s_count;
    if (at < 0) at = n;
    if (at > n) return -1;
    if (mpdq_insert(at, path, NULL) < 0) return -1;
    for (int i = n; i > at; i--) played_put(i, played_at(i - 1));
    played_put(at, false);
    if (s_current >= at) s_current++;
    /* Inserted AT the gap, it is on the far side of the boundary: the
     * entry that plays next. */
    if (s_gap > at) s_gap++;
    return at;
}

int playlist_add_next(const char *path)
{
    if (s_current >= 0) return playlist_add(path, s_current + 1);
    if (s_gap >= 0) return playlist_add(path, s_gap);
    return playlist_add(path, -1);
}

bool playlist_remove(int pos)
{
    const int n = s_count;
    if (pos < 0 || pos >= n) return false;
    if (!mpdq_remove(pos)) return false;
    for (int i = pos; i < n - 1; i++) played_put(i, played_at(i + 1));
    played_put(n - 1, false);
    if (s_current == pos) {
        /* The track keeps playing -- it is open, and stopping it is a
         * different request -- but it is no longer in the list, which is
         * the state playlist.h already names for a file played from
         * outside it. What followed it has slid into `pos`, and is what
         * next means now. */
        s_current = -1;
        s_gap = pos;
    } else {
        if (s_current > pos) s_current--;
        if (s_gap > pos) s_gap--;
    }
    return true;
}

bool playlist_move(int from, int to)
{
    const int n = s_count;
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    if (!mpdq_move(from, to)) return false;
    const bool b = played_at(from);
    if (from < to) for (int i = from; i < to; i++) played_put(i, played_at(i + 1));
    else           for (int i = from; i > to; i--) played_put(i, played_at(i - 1));
    played_put(to, b);
    if (s_current >= 0) s_current = moved_index(s_current, from, to);
    /* The gap is a boundary, not an entry: it is "before the entry now at
     * s_gap". It follows that entry, unless that entry is the one moving,
     * in which case the boundary stays where it was. */
    if (s_gap >= 0 && s_gap < n && s_gap != from) s_gap = moved_index(s_gap, from, to);
    return true;
}
