/*
 * varispeed.c -- see varispeed.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "varispeed.h"

#include <string.h>

const uint16_t varispeed_rpm_thirds[VARISPEED_RPMS] = { 50, 100, 135, 234 };
const char *const varispeed_rpm_label[VARISPEED_RPMS] = {
    "16⅔", "33⅓", "45", "78"
};

uint32_t varispeed_step(unsigned played, unsigned cut)
{
    if (played >= VARISPEED_RPMS || cut >= VARISPEED_RPMS || played == cut)
        return VARISPEED_UNITY;
    /* Largest is 234/50 = 4.68, so the product stays inside 32 bits. */
    return (uint32_t)(((uint64_t)varispeed_rpm_thirds[played] << 16)
                      / varispeed_rpm_thirds[cut]);
}

void varispeed_reset(varispeed_t *v)
{
    memset(v, 0, sizeof *v);
    v->have = 1;          /* win[0], the frame before the first: silence */
}

uint32_t varispeed_slide(uint32_t from, uint32_t to, uint32_t pos, uint32_t len)
{
    if (len == 0 || pos >= len) return to;
    const int64_t d = (int64_t)to - (int64_t)from;
    return (uint32_t)((int64_t)from + d * (int64_t)pos / (int64_t)len);
}

static inline int16_t clip(float x)
{
    if (x >  32767.0f) return  32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)(x < 0 ? x - 0.5f : x + 0.5f);
}

size_t varispeed_run(varispeed_t *v, uint32_t step,
                     const int16_t *in, size_t in_frames, size_t *in_used,
                     int16_t *out, size_t out_cap)
{
    size_t used = 0, n = 0;
    v->active = true;

    for (;;) {
        if (v->adv) {
            if (v->have) {                    /* move the read point on */
                memmove(v->win[0], v->win[1], sizeof v->win[0] * 3);
                v->have--;
            } else if (used < in_frames) {    /* a long step: skip input */
                used++;
            } else {
                break;
            }
            v->adv--;
            continue;
        }
        if (v->have < 4) {
            if (used == in_frames) break;
            v->win[v->have][0] = in[2 * used];
            v->win[v->have][1] = in[2 * used + 1];
            v->have++;
            used++;
            continue;
        }
        if (n == out_cap) break;

        const float t = (float)v->frac * (1.0f / 65536.0f);
        for (int ch = 0; ch < 2; ch++) {
            const float xm = v->win[0][ch], x0 = v->win[1][ch];
            const float x1 = v->win[2][ch], x2 = v->win[3][ch];
            /* Catmull-Rom: passes through every input sample, so t == 0
             * gives x0 back exactly. */
            const float y = x0 + 0.5f * t * (x1 - xm
                          + t * (2.0f * xm - 5.0f * x0 + 4.0f * x1 - x2
                          + t * (3.0f * (x0 - x1) + x2 - xm)));
            out[2 * n + (size_t)ch] = clip(y);
        }
        n++;
        const uint32_t f = v->frac + step;
        v->adv = f >> 16;
        v->frac = f & 0xffffu;
    }
    *in_used = used;
    return n;
}
