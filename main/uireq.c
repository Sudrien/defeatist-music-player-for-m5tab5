/*
 * uireq.c -- see uireq.h. MPD.md step 5.
 *
 * A ring rather than a FreeRTOS queue, for two reasons. The per-source
 * limit needs a count a queue does not keep. And the sequence number has
 * to be given out under the same lock as the slot: with a queue, two
 * tasks could number their presses 5 and 6 and send 6 first, and the
 * pass that took 6 would mark 5 serviced before anyone had taken it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "uireq.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    ui_action_t    act;
    uint32_t       seq;
    uireq_source_t src;
    bool           is_edit;     /* 5173: `e` and `path`, not `act` */
    uireq_edit_t   e;
    char          *path;        /* 5173: an add's, PSRAM; NULL otherwise */
} req_t;

static SemaphoreHandle_t s_mu;
static req_t             s_ring[UIREQ_DEPTH];      /* under s_mu */
static unsigned          s_head, s_n;               /* under s_mu */
static unsigned          s_waiting[UIREQ_SOURCES];  /* under s_mu */
static uint32_t          s_seq;                     /* under s_mu */
static uint32_t          s_taken;   /* under s_mu: the last one taken */
static uint32_t          s_done;    /* under s_mu: taken AND published */

/* 5175: the last UIREQ_OUTCOMES edits' outcomes, by seq % UIREQ_OUTCOMES. */
static struct {
    uint32_t     seq;       /* 0: empty */
    uireq_done_t how;
    uint32_t     id;
} s_done_tab[UIREQ_OUTCOMES];                                /* under s_mu */

static char              s_open_path[UIREQ_PATH_MAX];  /* under s_mu */
static bool              s_open_folder;                /* under s_mu */
static volatile bool     s_open_pending;

void uireq_init(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
}

/* Under s_mu: `r` into the ring with the next number, or 0 for no room.
 * The caller frees r->path on 0. */
static uint32_t push_locked(req_t r)
{
    if (s_waiting[r.src] >= UIREQ_PER_SOURCE || s_n >= UIREQ_DEPTH) return 0;
    if (++s_seq == 0) s_seq = 1;            /* 0 is "did not fit" */
    r.seq = s_seq;
    s_ring[(s_head + s_n) % UIREQ_DEPTH] = r;
    s_n++;
    s_waiting[r.src]++;
    return r.seq;
}

uint32_t uireq_press(uireq_source_t src, const ui_action_t *act)
{
    if (!s_mu || !act || (unsigned)src >= UIREQ_SOURCES) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const uint32_t seq = push_locked((req_t){ .act = *act, .src = src });
    xSemaphoreGive(s_mu);
    return seq;
}

uint32_t uireq_edit(uireq_source_t src, const uireq_edit_t *e, const char *path, size_t len)
{
    if (!s_mu || !e || (unsigned)src >= UIREQ_SOURCES) return 0;
    char *copy = NULL;
    if (e->kind == UIREQ_EDIT_ADD || e->kind == UIREQ_EDIT_ADD_NEXT) {
        if (!path || len == 0 || len >= UIREQ_PATH_MAX) return 0;
        /* Copied before the lock: an allocation is not a thing to do
         * while the other producer waits. */
        copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) return 0;
        memcpy(copy, path, len);
        copy[len] = '\0';
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const uint32_t seq = push_locked((req_t){ .src = src, .is_edit = true, .e = *e, .path = copy });
    xSemaphoreGive(s_mu);
    if (!seq) free(copy);
    return seq;
}

bool uireq_take_edit(uireq_edit_t *out, char *path, size_t size)
{
    if (!s_mu || !out) return false;
    char *held = NULL;
    bool had = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_n && s_ring[s_head].is_edit) {
        const req_t *r = &s_ring[s_head];
        *out = r->e;
        out->seq = r->seq;              /* 5175 */
        held = r->path;
        s_taken = r->seq;
        s_waiting[r->src]--;
        s_head = (s_head + 1) % UIREQ_DEPTH;
        s_n--;
        had = true;
    }
    xSemaphoreGive(s_mu);
    if (path && size) {
        path[0] = '\0';
        if (held) {
            const size_t n = strlen(held);
            const size_t k = n < size - 1 ? n : size - 1;
            memcpy(path, held, k);
            path[k] = '\0';
        }
    }
    free(held);
    return had;
}

void uireq_edit_done(uint32_t seq, uireq_done_t how, uint32_t id)
{
    if (!s_mu || !seq) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_done_tab[seq % UIREQ_OUTCOMES].seq = seq;
    s_done_tab[seq % UIREQ_OUTCOMES].how = how;
    s_done_tab[seq % UIREQ_OUTCOMES].id = id;
    xSemaphoreGive(s_mu);
}

bool uireq_edit_outcome(uint32_t seq, uireq_done_t *how, uint32_t *id)
{
    if (!s_mu || !seq) return false;
    bool have = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_done_tab[seq % UIREQ_OUTCOMES].seq == seq) {
        if (how) *how = s_done_tab[seq % UIREQ_OUTCOMES].how;
        if (id) *id = s_done_tab[seq % UIREQ_OUTCOMES].id;
        have = true;
    }
    xSemaphoreGive(s_mu);
    return have;
}

bool uireq_take_press(ui_action_t *out)
{
    if (!s_mu || !out) return false;
    bool had = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_n && !s_ring[s_head].is_edit) {
        const req_t *r = &s_ring[s_head];
        *out = r->act;
        s_taken = r->seq;
        s_waiting[r->src]--;
        s_head = (s_head + 1) % UIREQ_DEPTH;
        s_n--;
        had = true;
    }
    xSemaphoreGive(s_mu);
    return had;
}

void uireq_published(void)
{
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_done = s_taken;
    xSemaphoreGive(s_mu);
}

bool uireq_serviced(uint32_t seq)
{
    if (!s_mu) return true;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool done = (int32_t)(s_done - seq) >= 0;
    xSemaphoreGive(s_mu);
    return done;
}

bool uireq_open(const char *path, size_t len, bool folder)
{
    if (!s_mu || !path || len >= UIREQ_PATH_MAX) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_open_path, path, len);
    s_open_path[len] = '\0';
    s_open_folder = folder;
    s_open_pending = true;
    xSemaphoreGive(s_mu);
    return true;
}

bool uireq_take_open(char *path, size_t size, bool *folder)
{
    /* Unlocked first look: this is every ui_task pass, and almost always
     * there is nothing. */
    if (!s_open_pending || !s_mu || !path || !size) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool had = s_open_pending;
    if (had) {
        const size_t n = strlen(s_open_path);
        const size_t k = n < size - 1 ? n : size - 1;
        memcpy(path, s_open_path, k);
        path[k] = '\0';
        if (folder) *folder = s_open_folder;
        s_open_pending = false;
    }
    xSemaphoreGive(s_mu);
    return had;
}
