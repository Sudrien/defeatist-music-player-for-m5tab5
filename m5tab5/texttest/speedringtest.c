/*
 * speedringtest.c -- the writer's speed stage against a ring (6075).
 *
 * TRANSCRIBED from player.c -- ring_dry() and vs_read() -- over a fake
 * StreamBuffer that, like the real one, cannot be peeked or pushed
 * back. Checks the arithmetic, not the board; like the other
 * transcribed tests here it can drift from the code it describes, so
 * an edit to either is an edit to both.
 *
 * What is claimed:
 *   - whatever sizes the decode writes in, the writer's output is the
 *     same as varispeed_run() over the whole track in one call --
 *     nothing dropped at a ring wrap, nothing doubled;
 *   - every frame the decode wrote is consumed (the window's last two
 *     excepted, as varispeed.h says);
 *   - ring_dry() is not true while the stage still holds audio, so the
 *     handoff cannot leave a track's end behind in the stage;
 *   - the writer never spins: every pass either writes, or the ring
 *     and stage are both empty.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "varispeed.h"

#define PCM_CHUNK_BYTES     (4 * 1024)
#define PCM_BYTES_PER_FRAME (4)

/* --- a StreamBuffer: a byte FIFO, receive only ------------------- */
typedef struct { uint8_t *b; size_t head, tail, cap; } fifo_t;
static size_t fifo_avail(const fifo_t *f) { return f->tail - f->head; }
static bool   fifo_empty(const fifo_t *f) { return f->tail == f->head; }
static size_t fifo_recv(fifo_t *f, void *dst, size_t n)
{
    const size_t a = fifo_avail(f);
    if (n > a) n = a;
    memcpy(dst, f->b + f->head, n);
    f->head += n;
    return n;
}
static void fifo_send(fifo_t *f, const void *src, size_t n)
{
    memcpy(f->b + f->tail, src, n);
    f->tail += n;
}

/* --- transcribed ------------------------------------------------- */
static fifo_t      s_ring;
static varispeed_t s_vs;
static uint8_t     s_stage[PCM_CHUNK_BYTES];
static size_t      s_stage_n;

static bool ring_dry(void)
{
    return fifo_empty(&s_ring) && s_stage_n < PCM_BYTES_PER_FRAME;
}

static size_t vs_read(uint32_t step, uint8_t *dst, size_t cap)
{
    uint8_t *st = s_stage;
    size_t have = s_stage_n;
    if (have < PCM_CHUNK_BYTES) {
        have += fifo_recv(&s_ring, st + have, PCM_CHUNK_BYTES - have);
    }
    size_t used = 0;
    const size_t out = varispeed_run(&s_vs, step, (const int16_t *)st,
                                     have / PCM_BYTES_PER_FRAME, &used,
                                     (int16_t *)dst, cap / PCM_BYTES_PER_FRAME);
    const size_t ub = used * PCM_BYTES_PER_FRAME;
    memmove(st, st + ub, have - ub);
    s_stage_n = have - ub;
    return out * PCM_BYTES_PER_FRAME;
}
/* ----------------------------------------------------------------- */

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

#define N 30000

int main(void)
{
    int16_t *track = malloc(N * 4);
    for (int k = 0; k < N; k++) {
        track[2 * k]     = (int16_t)lrint(9000.0 * sin(k * 0.05));
        track[2 * k + 1] = (int16_t)lrint(9000.0 * cos(k * 0.031));
    }
    s_ring.b = malloc(N * 4 + 16);
    s_ring.cap = N * 4 + 16;

    const size_t big = 8 * N * 4;
    int16_t *ref = malloc(big), *got = malloc(big);
    uint8_t *chunk = malloc(PCM_CHUNK_BYTES);

    srand(6075);
    for (unsigned p = 0; p < VARISPEED_RPMS; p++) {
        for (unsigned c = 0; c < VARISPEED_RPMS; c++) {
            const uint32_t step = varispeed_step(p, c);

            /* The reference: the whole track in one call. */
            varispeed_t v;
            varispeed_reset(&v);
            size_t ru;
            const size_t rn = varispeed_run(&v, step, track, N, &ru, ref, big / 4);

            /* The writer: decode sends in odd sizes (whole frames, as
             * the decode loop does), writer reads 4 KB at a time. */
            s_ring.head = s_ring.tail = 0;
            s_stage_n = 0;
            varispeed_reset(&s_vs);
            size_t sent = 0, outn = 0;
            int idle = 0, spins = 0;
            bool dry_early = false;
            while (1) {
                if (sent < N) {
                    size_t f = 1 + (size_t)(rand() % 700);
                    if (f > N - sent) f = N - sent;
                    fifo_send(&s_ring, track + 2 * sent, f * 4);
                    sent += f;
                }
                const size_t g = vs_read(step, chunk, PCM_CHUNK_BYTES);
                memcpy((uint8_t *)got + outn * 4, chunk, g);
                outn += g / 4;
                if (!g) {
                    if (sent == N && ring_dry()) break;
                    if (!fifo_empty(&s_ring) || s_stage_n >= 16) spins++;
                    if (++idle > 100000) break;
                }
                if (sent == N && ring_dry() && outn < rn - 1) dry_early = true;
            }
            char what[96];
            snprintf(what, sizeof what, "%s/%s: same as one call, %zu frames",
                     varispeed_rpm_label[p], varispeed_rpm_label[c], rn);
            ck(what, outn == rn && memcmp(got, ref, rn * 4) == 0);
            snprintf(what, sizeof what, "%s/%s: all input consumed, no spin, not dry early",
                     varispeed_rpm_label[p], varispeed_rpm_label[c]);
            ck(what, s_stage_n == 0 && fifo_empty(&s_ring) && spins == 0 &&
                     !dry_early);
        }
    }

    /* The handoff question directly: stage holding audio is not dry. */
    s_ring.head = s_ring.tail = 0;
    s_stage_n = 8;
    ck("stage holding two frames: not dry", !ring_dry());
    s_stage_n = 3;
    ck("stage holding less than a frame: dry", ring_dry());

    free(track); free(s_ring.b); free(ref); free(got); free(chunk);
    printf("%s\n", fails ? "speedringtest: FAILED" : "speedringtest: all ok");
    return fails != 0;
}
