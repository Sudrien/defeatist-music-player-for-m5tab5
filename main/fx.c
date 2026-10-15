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

/* ------------------------------------------------------------------ */
/* 6101: VOX                                                          */
/* ------------------------------------------------------------------ */

const char *const fx_vox_label[FX_STEPS] = { "0", "\u00bd", "1", "\uE000", "2" };
const float fx_vox_gain[FX_STEPS] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };

void fx_vox_reset(fx_vox_t *v)
{
    memset(v, 0, sizeof *v);
    v->gv = 1.0f;
}

void fx_vox_run(fx_vox_t *v, int16_t *pcm, size_t frames,
                unsigned idx, uint32_t rate)
{
    if (!frames || !rate) return;
    if (rate != v->rate) {
        v->rate = rate;
        v->alo = onepole(FX_VOX_LO_HZ, rate);
        v->ahi = onepole(FX_VOX_HI_HZ, rate);
    }
    const float g1 = fx_vox_gain[idx < FX_STEPS ? idx : FX_VOX_FLAT];
    const float g0 = v->gv;
    const bool touch = !(g0 == 1.0f && g1 == 1.0f);
    const float h0 = 1.0f / (g0 > 1.0f ? g0 : 1.0f);
    const float h1 = 1.0f / (g1 > 1.0f ? g1 : 1.0f);
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;
        const float g = g0 + (g1 - g0) * u;
        const float h = h0 + (h1 - h0) * u;
        const float l = (float)pcm[2 * i], r = (float)pcm[2 * i + 1];
        const float c = 0.5f * (l + r);
        v->hi += v->ahi * (c - v->hi);          /* below 8 kHz */
        v->lo += v->alo * (c - v->lo);          /* below 150 Hz */
        if (touch) {
            const float band = v->hi - v->lo;   /* between the two */
            pcm[2 * i]     = clip16((l + (g - 1.0f) * band) * h);
            pcm[2 * i + 1] = clip16((r + (g - 1.0f) * band) * h);
        }
    }
    if (fabsf(v->lo) < 1e-20f) v->lo = 0.0f;
    if (fabsf(v->hi) < 1e-20f) v->hi = 0.0f;
    v->gv = g1;
}

/* ------------------------------------------------------------------ */
/* 6102: ECHO                                                         */
/* ------------------------------------------------------------------ */

const char *const fx_echo_label[FX_ECHO_STEPS] = {
    "0", "\u215b", "\u00bc", "\u2153", "\u00bd", "\u2154", "\u00be", "\u215e"
};
const float fx_echo_gain[FX_ECHO_STEPS] = {
    0.0f, 0.125f, 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 0.875f
};
const uint16_t fx_echo_ms[FX_STEPS] = { 60, 125, 250, 500, 1000 };

void fx_echo_init(fx_echo_t *e, float *line)
{
    memset(e, 0, sizeof *e);
    e->line = line;
}

void fx_echo_run(fx_echo_t *e, int16_t *pcm, size_t frames,
                 unsigned echo, unsigned time, uint32_t rate)
{
    if (!frames || !rate || !e->line) return;
    const float g1 = fx_echo_gain[echo < FX_ECHO_STEPS ? echo : 0];
    const float g0 = e->g;
    if (g0 == 0.0f && g1 == 0.0f) return;           /* off: untouched */
    if (g0 == 0.0f) {
        /* On from off: nothing in the line is wanted. */
        memset(e->line, 0, sizeof(float) * 2 * FX_ECHO_FRAMES);
        e->pos = 0;
    }
    uint32_t d = (uint32_t)((uint64_t)rate *
                            fx_echo_ms[time < FX_STEPS ? time : 2] / 1000u);
    if (d < 1) d = 1;
    if (d > FX_ECHO_FRAMES - 1) d = FX_ECHO_FRAMES - 1;
    float *L = e->line;
    uint32_t pos = e->pos;
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;
        const float g = g0 + (g1 - g0) * u;
        const uint32_t rd = (pos + FX_ECHO_FRAMES - d) % FX_ECHO_FRAMES;
        for (int c = 0; c < 2; c++) {
            const float y = (float)pcm[2 * i + (size_t)c] + g * L[2 * rd + (uint32_t)c];
            L[2 * pos + (uint32_t)c] = y;
            pcm[2 * i + (size_t)c] = clip16(y * (1.0f - g));
        }
        if (++pos == FX_ECHO_FRAMES) pos = 0;
    }
    e->pos = pos;
    e->g = g1;
}

/* ------------------------------------------------------------------ */
/* 6103: LCUT and HCUT                                                */
/* ------------------------------------------------------------------ */

const uint16_t fx_lcut_hz[FX_STEPS] = { 0, 40, 80, 150, 300 };
const uint16_t fx_hcut_hz[FX_STEPS] = { 0, 12000, 8000, 4000, 3000 };

static fx_bq_t bq_make(bool hp, unsigned hz, uint32_t rate)
{
    fx_bq_t k = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };     /* off: a wire */
    if (!hz || !rate || (float)hz >= 0.45f * (float)rate) return k;
    const double w = 2.0 * M_PI * hz / rate;
    const double cs = cos(w), al = sin(w) / (2.0 * 0.70710678);
    const double a0 = 1.0 + al;
    if (hp) {
        k.b0 = (float)((1.0 + cs) / 2.0 / a0);
        k.b1 = (float)(-(1.0 + cs) / a0);
        k.b2 = k.b0;
    } else {
        k.b0 = (float)((1.0 - cs) / 2.0 / a0);
        k.b1 = (float)((1.0 - cs) / a0);
        k.b2 = k.b0;
    }
    k.a1 = (float)(-2.0 * cs / a0);
    k.a2 = (float)((1.0 - al) / a0);
    return k;
}

static inline float bq_step(const fx_bq_t *k, float z[2], float x)
{
    const float y = k->b0 * x + z[0];
    z[0] = k->b1 * x - k->a1 * y + z[1];
    z[1] = k->b2 * x - k->a2 * y;
    return y;
}

void fx_cut_reset(fx_cut_t *c)
{
    memset(c, 0, sizeof *c);
    c->k.b0 = 1.0f;
}

void fx_cut_run(fx_cut_t *c, bool hp, int16_t *pcm, size_t frames,
                unsigned idx, uint32_t rate)
{
    if (!frames || !rate) return;
    if (idx >= FX_STEPS) idx = 0;
    const uint16_t *tab = hp ? fx_lcut_hz : fx_hcut_hz;
    if (idx == 0 && c->idx == 0) {
        c->rate = rate;
        return;                                     /* off: untouched */
    }
    if (idx == c->idx && rate == c->rate) {
        for (size_t i = 0; i < frames; i++)
            for (int ch = 0; ch < 2; ch++)
                pcm[2 * i + (size_t)ch] =
                    clip16(bq_step(&c->k, c->z[ch], (float)pcm[2 * i + (size_t)ch]));
        return;
    }
    /* A change: the old filter and the new one side by side, faded
     * from one to the other. The new one starts from the old one's
     * state, which for a corner moved a step is close; off is a wire
     * whose state is zero. */
    const fx_bq_t nk = bq_make(hp, tab[idx], rate);
    float nz[2][2];
    memcpy(nz, c->z, sizeof nz);
    if (c->idx == 0) memset(nz, 0, sizeof nz);
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;
        for (int ch = 0; ch < 2; ch++) {
            const float x = (float)pcm[2 * i + (size_t)ch];
            const float yo = bq_step(&c->k, c->z[ch], x);
            const float yn = bq_step(&nk, nz[ch], x);
            pcm[2 * i + (size_t)ch] = clip16(yo + (yn - yo) * u);
        }
    }
    c->k = nk;
    memcpy(c->z, nz, sizeof nz);
    c->idx = idx;
    c->rate = rate;
}

/* ------------------------------------------------------------------ */
/* 6104: DUST                                                         */
/* ------------------------------------------------------------------ */

const uint16_t fx_dust_cps[FX_STEPS] = { 0, 1, 4, 12, 40 };

/* Per step, of full scale: the loudest a tick can be, and the hiss. */
static const float k_dust_tick[FX_STEPS] = { 0.0f, 0.10f, 0.12f, 0.15f, 0.20f };
static const float k_dust_hiss[FX_STEPS] = { 0.0f, 0.0008f, 0.0015f, 0.0025f, 0.0045f };
#define FX_DUST_HISS_HZ   (5000.0f)   /* hiss through a one-pole, not white */
#define FX_DUST_TICK_MS_LO (0.08f)
#define FX_DUST_TICK_MS_HI (0.6f)

static inline uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* -1 .. 1 and 0 .. 1 */
static inline float rnd_bi(uint32_t *s) { return (float)(int32_t)xs32(s) * (1.0f / 2147483648.0f); }
static inline float rnd_un(uint32_t *s) { return (float)(xs32(s) >> 8) * (1.0f / 16777216.0f); }

void fx_dust_reset(fx_dust_t *d, uint32_t seed)
{
    memset(d, 0, sizeof *d);
    d->rng = seed ? seed : 0x9E3779B9u;     /* xorshift's one bad seed */
}

void fx_dust_run(fx_dust_t *d, int16_t *pcm, size_t frames,
                 unsigned idx, uint32_t rate)
{
    if (!frames || !rate) return;
    if (idx >= FX_STEPS) idx = 0;
    if (rate != d->rate) {
        d->rate = rate;
        d->ah = onepole(FX_DUST_HISS_HZ, rate);
    }
    const float hl0 = d->hl, hl1 = k_dust_hiss[idx] * 32768.0f;
    const float rm0 = d->room;
    const float rm1 = idx ? k_dust_tick[idx] + k_dust_hiss[idx] : 0.0f;
    if (idx == 0 && hl0 == 0.0f && rm0 == 0.0f && d->env == 0.0f)
        return;                                     /* off: untouched */

    /* A tick starts on a frame with probability cps / rate, as a
     * threshold on 32 random bits. */
    const uint32_t thr = (uint32_t)((double)fx_dust_cps[idx] / rate * 4294967296.0);
    const float tick = k_dust_tick[idx] * 32768.0f;
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;
        const float hl = hl0 + (hl1 - hl0) * u;
        const float h = 1.0f - (rm0 + (rm1 - rm0) * u);

        if (thr && xs32(&d->rng) < thr) {
            const float a = rnd_un(&d->rng);
            const float ms = FX_DUST_TICK_MS_LO +
                             (FX_DUST_TICK_MS_HI - FX_DUST_TICK_MS_LO) * rnd_un(&d->rng);
            const float p = rnd_un(&d->rng);        /* 0 left .. 1 right */
            d->env = tick * a * a;
            d->k = expf(-1000.0f / (ms * (float)rate));
            d->pl = 1.0f - 0.6f * p;                /* never wholly one side */
            d->pr = 0.4f + 0.6f * p;
        }
        const float t = d->env * rnd_bi(&d->rng);
        d->env *= d->k;
        if (d->env < 0.5f) d->env = 0.0f;           /* under a bit: done */

        for (int c = 0; c < 2; c++) {
            d->hs[c] += d->ah * (rnd_bi(&d->rng) - d->hs[c]);
            const float n = hl * d->hs[c] * 1.7f + t * (c ? d->pr : d->pl);
            pcm[2 * i + (size_t)c] = clip16((float)pcm[2 * i + (size_t)c] * h + n);
        }
    }
    d->hl = hl1;
    d->room = rm1;
    if (idx == 0) { d->hs[0] = d->hs[1] = 0.0f; }
}

/* ------------------------------------------------------------------ */
/* 6105: WIDE                                                         */
/* ------------------------------------------------------------------ */

const float fx_wide_gain[FX_STEPS] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };

void fx_wide_reset(fx_wide_t *w)
{
    w->g = 1.0f;
}

void fx_wide_run(fx_wide_t *w, int16_t *pcm, size_t frames,
                 unsigned idx, uint32_t rate)
{
    if (!frames || !rate) return;
    const float g1 = fx_wide_gain[idx < FX_STEPS ? idx : FX_WIDE_FLAT];
    const float g0 = w->g;
    if (g0 == 1.0f && g1 == 1.0f) return;           /* flat: untouched */
    const float h0 = g0 > 1.0f ? 2.0f / (1.0f + g0) : 1.0f;
    const float h1 = g1 > 1.0f ? 2.0f / (1.0f + g1) : 1.0f;
    const float inv = 1.0f / (float)frames;
    for (size_t i = 0; i < frames; i++) {
        const float u = (float)(i + 1) * inv;
        const float g = g0 + (g1 - g0) * u;
        const float h = h0 + (h1 - h0) * u;
        const float l = (float)pcm[2 * i], r = (float)pcm[2 * i + 1];
        const float m = 0.5f * (l + r), sd = 0.5f * (l - r) * g;
        pcm[2 * i]     = clip16((m + sd) * h);
        pcm[2 * i + 1] = clip16((m - sd) * h);
    }
    w->g = g1;
}

/* ------------------------------------------------------------------ */
/* 6105: WOW and FLTR                                                 */
/* ------------------------------------------------------------------ */

const char *const fx_wow_label[FX_STEPS]  = { "0", "0.1", "0.25", "0.5", "1" };
const float       fx_wow_dev[FX_STEPS]    = { 0.0f, 0.001f, 0.0025f, 0.005f, 0.01f };
const char *const fx_fltr_label[FX_STEPS] = { "0", "0.05", "0.1", "0.2", "0.5" };
const float       fx_fltr_dev[FX_STEPS]   = { 0.0f, 0.0005f, 0.001f, 0.002f, 0.005f };

void fx_wow_reset(fx_wow_t *w)
{
    w->pos = 0;
    w->aw = w->af = 0.0f;
    w->cw = w->cf = 1.0f;       /* phase 0: 1 - cos is 0 */
    w->sw = w->sf = 0.0f;
    w->live = false;
}

static inline float slew(float a, float to, float step)
{
    if (a < to) return a + step < to ? a + step : to;
    if (a > to) return a - step > to ? a - step : to;
    return a;
}

void fx_wow_run(fx_wow_t *w, int16_t *pcm, size_t frames,
                unsigned wow, unsigned fltr, unsigned rpm_thirds,
                uint32_t rate)
{
    if (!frames || !rate) return;
    if (wow >= FX_STEPS) wow = 0;
    if (fltr >= FX_STEPS) fltr = 0;
    if (!rpm_thirds) rpm_thirds = 100;
    const float fw = (float)rpm_thirds / 180.0f;    /* revolutions a second */
    const float ff = fw * FX_FLTR_PER_REV;
    const float tw = fx_wow_dev[wow] / (2.0f * (float)M_PI * fw);
    const float tf = fx_fltr_dev[fltr] / (2.0f * (float)M_PI * ff);
    if (tw == 0.0f && tf == 0.0f && w->aw == 0.0f && w->af == 0.0f) {
        w->live = false;
        return;                                     /* off: untouched */
    }
    if (!w->live) {
        /* On from off: the delay starts at 0 and grows slower than time
         * passes, so it only ever reads what this run wrote. */
        memset(w->line, 0, sizeof w->line);
        w->pos = 0;
        w->live = true;
    }
    const float step = FX_WOW_SLEW / (float)rate;
    const float rw = 2.0f * (float)M_PI * fw / (float)rate;
    const float rf = 2.0f * (float)M_PI * ff / (float)rate;
    const float kcw = cosf(rw), ksw = sinf(rw), kcf = cosf(rf), ksf = sinf(rf);
    const float fr = (float)rate, most = (float)(FX_WOW_FRAMES - 2);
    float cw = w->cw, sw = w->sw, cf = w->cf, sf = w->sf;
    float aw = w->aw, af = w->af;
    uint32_t pos = w->pos;
    for (size_t i = 0; i < frames; i++) {
        w->line[pos][0] = pcm[2 * i];
        w->line[pos][1] = pcm[2 * i + 1];
        aw = slew(aw, tw, step);
        af = slew(af, tf, step);
        float d = (aw * (1.0f - cw) + af * (1.0f - cf)) * fr;   /* frames */
        if (d > most) d = most;
        if (d > 0.0f) {
            const uint32_t n = (uint32_t)d;
            const float t = d - (float)n;
            const uint32_t a = (pos + FX_WOW_FRAMES - n) % FX_WOW_FRAMES;
            const uint32_t b = (a + FX_WOW_FRAMES - 1) % FX_WOW_FRAMES;
            for (int c = 0; c < 2; c++) {
                const float y = (float)w->line[a][c] +
                                ((float)w->line[b][c] - (float)w->line[a][c]) * t;
                pcm[2 * i + (size_t)c] = clip16(y);
            }
        }
        /* Both phases on a step; a rotation, not a cosf a frame. */
        float x = cw * kcw - sw * ksw; sw = sw * kcw + cw * ksw; cw = x;
        x = cf * kcf - sf * ksf; sf = sf * kcf + cf * ksf; cf = x;
        if (++pos == FX_WOW_FRAMES) pos = 0;
    }
    /* Back onto the unit circle once a chunk; a rotation drifts. */
    float m = 1.0f / sqrtf(cw * cw + sw * sw); cw *= m; sw *= m;
    m = 1.0f / sqrtf(cf * cf + sf * sf); cf *= m; sf *= m;
    w->cw = cw; w->sw = sw; w->cf = cf; w->sf = sf;
    w->aw = aw; w->af = af;
    w->pos = pos;
}
