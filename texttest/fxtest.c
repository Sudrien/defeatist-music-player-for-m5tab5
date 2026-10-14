/*
 * fxtest.c -- main/fx.c, the Fx sheet's tone and edges (6097).
 *
 * What is claimed:
 *   - BASS 1 and TREBLE 1 leave every sample as it was, bit for bit,
 *     however long the filters have been running;
 *   - each band reaches its gain where it should: BASS 2 doubles 40 Hz
 *     against 1 kHz, TREBLE ½ halves 12 kHz against 1 kHz, within a
 *     first-order shelf's tolerance;
 *   - the headroom works: a full-scale tone at any setting comes out at
 *     no more than full scale, without a limiter;
 *   - a change of gain is a ramp, not a step;
 *   - an edge takes the time it says, and a release turned round
 *     mid-way comes back up from where it got to.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fx.h"

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

#define RATE 44100
#define N    (RATE / 2)

static int16_t buf[2 * N];

static void tone(double hz, double amp)
{
    for (int k = 0; k < N; k++) {
        const int16_t v = (int16_t)lrint(amp * sin(2 * M_PI * hz * k / RATE));
        buf[2 * k] = buf[2 * k + 1] = v;
    }
}

/* RMS of the left channel over the last half, after settling. */
static double rms(void)
{
    double s = 0;
    for (int k = N / 2; k < N; k++) s += (double)buf[2 * k] * buf[2 * k];
    return sqrt(s / (N / 2));
}

static double gain_at(double hz, unsigned b, unsigned t)
{
    fx_tone_t st;
    fx_tone_reset(&st);
    tone(hz, 8000);
    const double in = rms();
    /* Settled at the gain first, so the ramp is not in the measurement. */
    for (int k = 0; k < N; k += 1024) {
        const int n = (N - k) < 1024 ? (N - k) : 1024;
        fx_tone_run(&st, buf + 2 * k, (size_t)n, b, t, RATE);
    }
    return rms() / in;
}

int main(void)
{
    char what[96];

    /* Flat is untouched. */
    {
        fx_tone_t st;
        fx_tone_reset(&st);
        srand(6097);
        int16_t *a = malloc(sizeof buf), *c = malloc(sizeof buf);
        for (int k = 0; k < 2 * N; k++) a[k] = (int16_t)(rand() - RAND_MAX / 2);
        memcpy(c, a, sizeof buf);
        for (int k = 0; k < N; k += 700) {
            const int n = (N - k) < 700 ? (N - k) : 700;
            fx_tone_run(&st, a + 2 * k, (size_t)n, FX_TONE_FLAT, FX_TONE_FLAT, RATE);
        }
        ck("1/1: bit for bit", memcmp(a, c, sizeof buf) == 0);
        free(a); free(c);
    }

    /* The bands, relative to 1 kHz, which both leave nearly alone. */
    {
        const double mid = gain_at(1000, 4, FX_TONE_FLAT);
        const double lo  = gain_at(40, 4, FX_TONE_FLAT);
        snprintf(what, sizeof what, "BASS 2: 40 Hz over 1 kHz is %.2f (want ~2)", lo / mid);
        ck(what, lo / mid > 1.7 && lo / mid < 2.1);
        const double lo2 = gain_at(40, 0, FX_TONE_FLAT);
        const double mid2 = gain_at(1000, 0, FX_TONE_FLAT);
        snprintf(what, sizeof what, "BASS ½: 40 Hz over 1 kHz is %.2f (want ~0.5)", lo2 / mid2);
        ck(what, lo2 / mid2 > 0.45 && lo2 / mid2 < 0.62);
        /* Against 300 Hz: a first-order shelf at 3.5 kHz has already
         * moved 1 kHz a little. */
        const double hi = gain_at(12000, FX_TONE_FLAT, 0);
        const double m3 = gain_at(300, FX_TONE_FLAT, 0);
        snprintf(what, sizeof what, "TREBLE ½: 12 kHz over 300 Hz is %.2f (want ~0.5)", hi / m3);
        ck(what, hi / m3 > 0.45 && hi / m3 < 0.65);
        const double hi2 = gain_at(12000, FX_TONE_FLAT, 4);
        const double m4 = gain_at(300, FX_TONE_FLAT, 4);
        snprintf(what, sizeof what, "TREBLE 2: 12 kHz over 300 Hz is %.2f (want ~2)", hi2 / m4);
        ck(what, hi2 / m4 > 1.7 && hi2 / m4 < 2.1);
    }

    /* Headroom: full scale in, never over full scale out. */
    {
        const double hz[] = { 30, 100, 1000, 5000, 15000 };
        int worst = 0;
        bool clipped = false;
        for (unsigned b = 0; b < FX_STEPS; b++)
            for (unsigned t = 0; t < FX_STEPS; t++)
                if (b != FX_TONE_FLAT || t != FX_TONE_FLAT)   /* 1/1 is the input */
                for (size_t f = 0; f < sizeof hz / sizeof hz[0]; f++) {
                    fx_tone_t st;
                    fx_tone_reset(&st);
                    tone(hz[f], 32767);
                    for (int k = 0; k < N; k += 1024) {
                        const int n = (N - k) < 1024 ? (N - k) : 1024;
                        fx_tone_run(&st, buf + 2 * k, (size_t)n, b, t, RATE);
                    }
                    for (int k = N / 2; k < N; k++) {
                        const int a = abs(buf[2 * k]);
                        if (a > worst) worst = a;
                        if (a >= 32767) clipped = true;
                    }
                }
        snprintf(what, sizeof what, "headroom: full-scale tones peak at %d off 1/1", worst);
        ck(what, !clipped);
    }

    /* A change is a ramp: no sample-to-sample jump bigger than the
     * signal's own when BASS goes 1 -> 2 on a 40 Hz tone. */
    {
        fx_tone_t st;
        fx_tone_reset(&st);
        tone(40, 8000);
        fx_tone_run(&st, buf, 1024, FX_TONE_FLAT, FX_TONE_FLAT, RATE);
        fx_tone_run(&st, buf + 2048, 1024, 4, FX_TONE_FLAT, RATE);
        int jump = 0;
        for (int k = 1; k < 2048; k++) {
            const int d = abs(buf[2 * k] - buf[2 * k - 2]);
            if (d > jump) jump = d;
        }
        snprintf(what, sizeof what, "1 -> 2 mid-tone: biggest step %d (40 Hz moves ~46)", jump);
        ck(what, jump < 100);
    }

    /* Edges. */
    {
        fx_edge_t e;
        fx_edge_reset(&e);
        tone(1000, 8000);
        int16_t copy[2048];
        memcpy(copy, buf, sizeof copy);
        fx_edge_run(&e, buf, 1024);
        ck("edge at 1, still: untouched", memcmp(copy, buf, sizeof copy) == 0);

        fx_edge_go(&e, false, 50, RATE);
        size_t n = 0;
        while (!fx_edge_silent(&e) && n < 10 * RATE) {
            int16_t f[2] = { 1000, 1000 };
            fx_edge_run(&e, f, 1);
            n++;
        }
        snprintf(what, sizeof what, "release 50 ms: silent after %zu frames (want 2205)", n);
        ck(what, n >= 2200 && n <= 2210);

        fx_edge_reset(&e);
        fx_edge_go(&e, false, 250, RATE);
        for (int i = 0; i < 5000; i++) { int16_t f[2] = { 0, 0 }; fx_edge_run(&e, f, 1); }
        const uint32_t mid = e.g;
        fx_edge_go(&e, true, 10, RATE);
        int16_t f[2] = { 10000, 10000 };
        fx_edge_run(&e, f, 1);
        const double fm = (double)mid / FX_EDGE_ONE;
        snprintf(what, sizeof what, "release turned round at %.3f: goes up from there", fm);
        ck(what, e.g > mid && (double)(e.g - mid) / FX_EDGE_ONE < 0.003 &&
                 fm > 0.5 && fm < 0.6);

        fx_edge_from_zero(&e, 0, RATE);
        ck("attack 0 ms: straight to 1", fx_edge_unity(&e));
        fx_edge_go(&e, false, 0, RATE);
        ck("release 0 ms: straight to 0", fx_edge_silent(&e));
    }

    printf("%s\n", fails ? "fxtest: FAILED" : "fxtest: all ok");
    return fails != 0;
}
