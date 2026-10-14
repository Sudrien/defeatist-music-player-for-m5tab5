/*
 * fx.c -- see fx.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fx.h"

#include <math.h>
#include <string.h>

const char *const fx_tone_label[FX_STEPS] = {
    "½", "⅔", "1", "\uE000", "2"
};
/* 6100: three halves is U+E000, a glyph feckless-graphics-handler
 * v0.2.3 draws into Ark12 (its fonts/ark12/README.md notes) -- stacked
 * like ½ and ⅔. "3⁄2" with U+2044 drew 3, a missing-glyph box, 2. A
 * Private Use codepoint means nothing outside this font, so the log
 * prints the gain as a number instead (player.c). */
const float fx_tone_gain[FX_STEPS] = { 0.5f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f };

const uint16_t fx_edge_ms[FX_STEPS] = { 0, 10, 50, 250, 1000 };

/* ------------------------------------------------------------------ */

static float onepole(float hz, uint32_t rate)
{
    /* Kept under Nyquist: 3.5 kHz at an 8 kHz device rate is fine, but
     * a corner above rate / 2 is not a corner. */
    const float f = hz < 0.45f * (float)rate ? hz : 0.45f * (float)rate;
    return 1.0f - expf(-2.0f * (float)M_PI * f / (float)rate);
}

void fx_tone_reset(fx_tone_t *t)
{
    memset(t, 0, sizeof *t);
    t->gb = t->gt = 1.0f;
}

bool fx_tone_flat(const fx_tone_t *t)
{
    return t->gb == 1.0f && t->gt == 1.0f;
}

static inline int16_t clip16(float v)
{
    if (v >  32767.0f) return  32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

void fx_tone_run(fx_tone_t *t, int16_t *pcm, size_t frames,
                 unsigned bass, unsigned treble, uint32_t rate)
{
    if (!frames || !rate) return;
    if (rate != t->rate) {
        t->rate = rate;
        t->ab = onepole(FX_BASS_HZ, rate);
        t->at = onepole(FX_TREBLE_HZ, rate);
    }
    const float gb1 = fx_tone_gain[bass < FX_STEPS ? bass : FX_TONE_FLAT];
    const float gt1 = fx_tone_gain[treble < FX_STEPS ? treble : FX_TONE_FLAT];
    const float gb0 = t->gb, gt0 = t->gt;
    const bool touch = !(gb0 == 1.0f && gt0 == 1.0f &&
                         gb1 == 1.0f && gt1 == 1.0f);

    /* Headroom: 1 over the largest gain, at each end of the chunk and
     * ramped between them with the gains it answers. */
    const float h0 = 1.0f / (gb0 > gt0 ? (gb0 > 1.0f ? gb0 : 1.0f)
                                      : (gt0 > 1.0f ? gt0 : 1.0f));
    const float h1 = 1.0f / (gb1 > gt1 ? (gb1 > 1.0f ? gb1 : 1.0f)
                                      : (gt1 > 1.0f ? gt1 : 1.0f));

    const float ab = t->ab, at = t->at;
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;      /* ends exactly on the new gains */
        const float gb = gb0 + (gb1 - gb0) * u;
        const float gt = gt0 + (gt1 - gt0) * u;
        const float h  = h0  + (h1  - h0)  * u;
        for (int c = 0; c < 2; c++) {
            const float x = (float)pcm[2 * i + (size_t)c];
            t->lb[c] += ab * (x - t->lb[c]);
            t->lt[c] += at * (x - t->lt[c]);
            if (touch) {
                const float hi = x - t->lt[c];
                const float y = x + (gb - 1.0f) * t->lb[c] + (gt - 1.0f) * hi;
                pcm[2 * i + (size_t)c] = clip16(y * h);
            }
        }
    }
    /* Denormals: a filter left listening to digital silence decays into
     * them, and on a soft-float path that is slow. */
    for (int c = 0; c < 2; c++) {
        if (fabsf(t->lb[c]) < 1e-20f) t->lb[c] = 0.0f;
        if (fabsf(t->lt[c]) < 1e-20f) t->lt[c] = 0.0f;
    }
    t->gb = gb1;
    t->gt = gt1;
}

/* ------------------------------------------------------------------ */

void fx_edge_reset(fx_edge_t *e)
{
    e->g = FX_EDGE_ONE;
    e->up = true;
    e->step = 0;
}

void fx_edge_go(fx_edge_t *e, bool up, uint32_t ms, uint32_t rate)
{
    if (!rate) rate = 44100;
    const uint64_t frames = (uint64_t)rate * ms / 1000u;
    e->up = up;
    if (!frames) {
        e->g = up ? FX_EDGE_ONE : 0;
        e->step = 0;
        return;
    }
    e->step = (uint32_t)(FX_EDGE_ONE / frames);
    if (!e->step) e->step = 1;
}

void fx_edge_from_zero(fx_edge_t *e, uint32_t ms, uint32_t rate)
{
    e->g = 0;
    fx_edge_go(e, true, ms, rate);
}

void fx_edge_run(fx_edge_t *e, int16_t *pcm, size_t frames)
{
    if (fx_edge_unity(e)) return;
    for (size_t i = 0; i < frames; i++) {
        if (e->up) {
            const uint32_t n = e->g + e->step;
            e->g = (e->step == 0 || n >= FX_EDGE_ONE) ? FX_EDGE_ONE : n;
        } else {
            e->g = (e->step == 0 || e->g <= e->step) ? 0 : e->g - e->step;
        }
        const int64_t g = (int64_t)e->g;
        pcm[2 * i]     = (int16_t)(((int64_t)pcm[2 * i]     * g) >> 30);
        pcm[2 * i + 1] = (int16_t)(((int64_t)pcm[2 * i + 1] * g) >> 30);
    }
}
