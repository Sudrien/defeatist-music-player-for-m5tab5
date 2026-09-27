/*
 * mpdqueue.c -- see mpdqueue.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpdqueue.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
#endif

typedef struct {
    char    *path;
    uint32_t id;
    uint32_t ver;
} ent_t;

/* The array is PSRAM, its strings the ordinary heap -- playlist.c's
 * split, for playlist.c's reason: 1024 pointers is worth putting in
 * PSRAM and a 200-byte path is not worth the round trip. */
static ent_t   *s_e;
static int      s_n;
static uint32_t s_next_id = 1;
static uint32_t s_ver = 1;

/* mp4seek.c's pattern: PSRAM on the device, malloc on the host, so this
 * file builds into a test without an IDF. */
static void *big_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(n, 1);
#endif
}

static uint32_t rnd(void)
{
#ifdef ESP_PLATFORM
    return esp_random();
#else
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
#endif
}

bool mpdq_init(void)
{
    if (s_e) return true;
    s_e = big_alloc((size_t)MPDQ_MAX * sizeof(ent_t));
    return s_e != NULL;
}

int      mpdq_count(void)   { return s_e ? s_n : 0; }
uint32_t mpdq_version(void) { return s_ver; }

static bool at(int pos) { return s_e && pos >= 0 && pos < s_n; }

const char *mpdq_path(int pos) { return at(pos) ? s_e[pos].path : NULL; }
uint32_t    mpdq_id(int pos)   { return at(pos) ? s_e[pos].id : 0; }

uint32_t mpdq_entry_version(int pos)
{
    return at(pos) ? s_e[pos].ver : 0;
}

void mpdq_clear(void)
{
    if (!s_e) return;
    for (int i = 0; i < s_n; i++) {
        free(s_e[i].path);
        s_e[i].path = NULL;
    }
    s_n = 0;
    s_ver++;           /* an empty list is news too */
}

/*
 * Stamp the entries whose POSITION changed, inclusive, with the new
 * version.
 *
 * This is the function that makes plchanges correct, and the reason it
 * exists rather than each caller stamping the one entry it touched: an
 * insert and a remove change the position of everything after them, and
 * a client told only about the entry named redraws every later row in
 * the wrong place.
 *
 * A RANGE AND NOT A TAIL, which is the other half and the half the
 * first version of this got wrong. A move from 1 to 3 shifts 1, 2 and 3
 * and leaves 4 exactly where it was; stamping to the end reported 4 as
 * changed, and a client then re-reads rows that are already right. Not a
 * wrong list -- just a client doing work on every move for no reason,
 * which is the cost MPD's version scheme exists to avoid. Caught by the
 * test asking MPD's question rather than reading this code.
 */
static void bump_range(int lo, int hi)
{
    s_ver++;
    if (lo < 0) lo = 0;
    if (hi > s_n - 1) hi = s_n - 1;
    for (int i = lo; i <= hi; i++) s_e[i].ver = s_ver;
}

int mpdq_insert(int pos, const char *path, uint32_t *id_out)
{
    if (!s_e || !path || !*path) return -1;
    const size_t len = strlen(path);
    if (len >= MPDQ_PATH_MAX) return -1;
    if (s_n >= MPDQ_MAX) return -1;
    if (pos < 0 || pos > s_n) return -1;

    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, path, len + 1);

    memmove(&s_e[pos + 1], &s_e[pos], (size_t)(s_n - pos) * sizeof(ent_t));
    s_e[pos].path = copy;
    s_e[pos].id   = s_next_id++;
    s_n++;

    if (id_out) *id_out = s_e[pos].id;
    bump_range(pos, s_n - 1);
    return pos;
}

int mpdq_append(const char *path, uint32_t *id_out)
{
    return mpdq_insert(s_e ? s_n : -1, path, id_out);
}

bool mpdq_remove(int pos)
{
    if (!at(pos)) return false;
    free(s_e[pos].path);
    memmove(&s_e[pos], &s_e[pos + 1],
            (size_t)(s_n - pos - 1) * sizeof(ent_t));
    s_n--;
    s_e[s_n].path = NULL;
    bump_range(pos, s_n - 1);
    return true;
}

bool mpdq_remove_id(uint32_t id)
{
    const int pos = mpdq_find_id(id);
    return (pos >= 0) && mpdq_remove(pos);
}

bool mpdq_move(int from, int to)
{
    if (!at(from) || !at(to)) return false;
    if (from == to) return true;            /* no change, no version */

    const ent_t moving = s_e[from];
    if (from < to) {
        memmove(&s_e[from], &s_e[from + 1],
                (size_t)(to - from) * sizeof(ent_t));
    } else {
        memmove(&s_e[to + 1], &s_e[to],
                (size_t)(from - to) * sizeof(ent_t));
    }
    s_e[to] = moving;

    /* Exactly the span between the two ends shifted; nothing outside it
     * did. */
    bump_range(from < to ? from : to, from < to ? to : from);
    return true;
}

void mpdq_shuffle(void)
{
    if (!s_e || s_n < 2) return;
    /* Fisher-Yates over the entries themselves, so an entry keeps its
     * id and a client's songids survive the shuffle. */
    for (int i = s_n - 1; i > 0; i--) {
        const int j = (int)(rnd() % (uint32_t)(i + 1));
        const ent_t t = s_e[i];
        s_e[i] = s_e[j];
        s_e[j] = t;
    }
    bump_range(0, s_n - 1);
}

int mpdq_find_id(uint32_t id)
{
    if (!s_e || id == 0) return -1;
    for (int i = 0; i < s_n; i++) {
        if (s_e[i].id == id) return i;
    }
    return -1;
}
