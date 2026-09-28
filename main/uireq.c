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

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    ui_action_t    act;
    uint32_t       seq;
    uireq_source_t src;
} req_t;

static SemaphoreHandle_t s_mu;
static req_t             s_ring[UIREQ_DEPTH];      /* under s_mu */
static unsigned          s_head, s_n;               /* under s_mu */
static unsigned          s_waiting[UIREQ_SOURCES];  /* under s_mu */
static uint32_t          s_seq;                     /* under s_mu */
static uint32_t          s_taken;   /* under s_mu: the last one taken */
static uint32_t          s_done;    /* under s_mu: taken AND published */

static char              s_open_path[UIREQ_PATH_MAX];  /* under s_mu */
static bool              s_open_folder;                /* under s_mu */
static volatile bool     s_open_pending;

void uireq_init(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
}

uint32_t uireq_press(uireq_source_t src, const ui_action_t *act)
{
    if (!s_mu || !act || (unsigned)src >= UIREQ_SOURCES) return 0;
    uint32_t seq = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_waiting[src] < UIREQ_PER_SOURCE && s_n < UIREQ_DEPTH) {
        if (++s_seq == 0) s_seq = 1;        /* 0 is "did not fit" */
        seq = s_seq;
        s_ring[(s_head + s_n) % UIREQ_DEPTH] = (req_t){ .act = *act, .seq = seq, .src = src };
        s_n++;
        s_waiting[src]++;
    }
    xSemaphoreGive(s_mu);
    return seq;
}

bool uireq_take_press(ui_action_t *out)
{
    if (!s_mu || !out) return false;
    bool had = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_n) {
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
