/*
 * pcmfoldtest.c -- the 24-bit to 16-bit fold.
 *
 * This arithmetic shipped in 0803 and ran on every 24-bit file since
 * without a test, because it was a static inside decoder.c and decoder.c
 * does not build on a host. 0003 moved it to pcmfold.h so netdec.c could
 * use it too, and a header is testable, so here is the test it should
 * have had.
 *
 * What is actually at risk in forty lines of shifting:
 *
 *   - SIGN. The sample is little-endian with the sign in the third
 *     byte. Getting it wrong turns quiet negative audio into loud
 *     positive audio, which is a sound you hear once.
 *   - ROUNDING. Truncation biases every sample toward zero. The bias is
 *     what the test can see; the DC it produces is what a listener
 *     eventually hears.
 *   - THE CLAMP. Rounding up within 128 of full scale wraps the sign
 *     without it. Full-scale positive becoming full-scale negative is
 *     the single worst output this function can produce.
 *   - THE IN-PLACE DIRECTION. Three bytes read for every two written.
 *     If the destination ever caught the source it would eat its own
 *     input, and the corruption would start partway through a buffer,
 *     which is the hardest kind to see in a log.
 *
 * 6150 added the two 32-bit folds, integer and float, and the checks
 * at the end of main() for them. The same four risks apply, plus two of
 * float's own: a sample outside +-1.0, which is legal and must clamp
 * rather than reach an out-of-range conversion, and NaN.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pcmfold.h"

static int checks, failures;

#define CHECK(cond, ...) do {                           \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

/* One little-endian 24-bit sample into a byte buffer. */
static void put24(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

/* Fold exactly one sample and return it. */
static int16_t fold1(int32_t v)
{
    int16_t buf[4];
    memset(buf, 0, sizeof(buf));
    put24((uint8_t *)buf, v);
    const int n = pcmfold_24_to_16(buf, 3);
    if (n != 1) return 0;
    return buf[0];
}

int main(void)
{
    printf("pcmfoldtest\n");

    printf("  sign and magnitude\n");
    {
        CHECK(fold1(0) == 0, "zero folded to %d", fold1(0));

        /* Full scale both ways. 24-bit positive full scale is 0x7FFFFF
         * and negative is -0x800000. */
        CHECK(fold1(0x7FFFFF) == 32767, "positive full scale gave %d",
              fold1(0x7FFFFF));
        CHECK(fold1(-0x800000) == -32768, "negative full scale gave %d",
              fold1(-0x800000));

        /* THE CLAMP, which is the reason full scale above is not just a
         * boundary case. 0x7FFFFF + 128 is 0x800000, which is negative
         * in 24 bits: without the clamp this returns -32768 and the
         * loudest possible sample comes out inverted. */
        for (int32_t v = 0x7FFF80; v <= 0x7FFFFF; v++) {
            CHECK(fold1(v) == 32767, "0x%06X should clamp, gave %d",
                  (unsigned)v, fold1(v));
        }

        /* A negative sample stays negative, and the top byte is what
         * carries that. 0xFF0000 is -65536. */
        CHECK(fold1(-65536) == -256, "-65536 gave %d", fold1(-65536));
        CHECK(fold1(-256) == -1, "-256 gave %d", fold1(-256));

        /* Exactly one 16-bit LSB is 256 in 24-bit terms. */
        CHECK(fold1(256) == 1, "256 gave %d", fold1(256));
        CHECK(fold1(65536) == 256, "65536 gave %d", fold1(65536));
    }

    printf("  rounding, not truncation\n");
    {
        /* Half an LSB up rounds up; just under stays. */
        CHECK(fold1(128) == 1, "half an LSB should round up, gave %d",
              fold1(128));
        CHECK(fold1(127) == 0, "just under half should stay, gave %d",
              fold1(127));
        CHECK(fold1(384) == 2, "1.5 LSB should round to 2, gave %d",
              fold1(384));

        /*
         * THE BIAS, which is the property the rounding exists for and
         * the one a truncating implementation fails. Every 24-bit value
         * in a range, folded, summed against the ideal: truncation
         * drifts steadily negative, rounding does not.
         */
        long err = 0;
        const int32_t lo = -20000, hi = 20000;
        for (int32_t v = lo; v <= hi; v++) {
            const double ideal = (double)v / 256.0;
            err += (long)((double)fold1(v) - ideal > 0 ? 1 : -1);
        }
        /* Over a symmetric range the up-roundings and down-roundings
         * should very nearly cancel. Truncation gives a count of about
         * -40000 here; rounding gives something near zero. */
        CHECK(err > -2000 && err < 2000,
              "rounding is biased: %ld over %d samples", err, hi - lo + 1);
    }

    printf("  in place, forward, nothing eaten\n");
    {
        /*
         * A whole buffer at once, checked against fold1() sample by
         * sample. This is what catches the destination catching up with
         * the source: fold1() only ever folds one sample, so it cannot
         * have the bug, and a mismatch partway through is exactly the
         * shape that failure takes.
         */
        enum { N = 1024 };
        static int16_t buf[N * 2];
        static int32_t src[N];
        uint8_t *p = (uint8_t *)buf;

        for (int i = 0; i < N; i++) {
            /* A sweep that covers both signs, the clamp region and the
             * low bits that rounding acts on. */
            src[i] = (int32_t)((i * 16411) % 0xFFFFFF) - 0x800000;
            put24(p + i * 3, src[i]);
        }

        const int n = pcmfold_24_to_16(buf, N * 3);
        CHECK(n == N, "folded %d of %d samples", n, N);
        int bad = -1;
        for (int i = 0; i < N; i++) {
            if (buf[i] != fold1(src[i])) { bad = i; break; }
        }
        CHECK(bad < 0, "sample %d differs: %d, expected %d",
              bad, bad >= 0 ? buf[bad] : 0, bad >= 0 ? fold1(src[bad]) : 0);
    }

    printf("  byte counts\n");
    {
        int16_t buf[16];
        memset(buf, 0, sizeof(buf));
        CHECK(pcmfold_24_to_16(buf, 0) == 0, "zero bytes produced samples");

        /* A partial sample is left unread rather than read past. Nine
         * bytes is three samples; ten and eleven are still three. */
        memset(buf, 0, sizeof(buf));
        CHECK(pcmfold_24_to_16(buf, 9) == 3, "9 bytes is 3 samples");
        CHECK(pcmfold_24_to_16(buf, 10) == 3, "10 bytes is still 3 samples");
        CHECK(pcmfold_24_to_16(buf, 11) == 3, "11 bytes is still 3 samples");

        /*
         * THE SIZING PROMISE the callers rely on: a buffer of N int16
         * holds N*2 bytes, and folding N*2 bytes of 24-bit yields
         * (N*2)/3 samples, which is fewer than N. Both decoder.c and
         * netdec.c pass a buffer sized in int16 and hand back the
         * return value, so if this were ever false they would both
         * report more audio than they hold.
         */
        for (int cap = 1; cap < 4096; cap++) {
            const uint32_t bytes = (uint32_t)cap * 2;
            CHECK((int)(bytes / 3) <= cap, "a %d-int16 buffer overflows", cap);
        }
    }

    printf("  32-bit integer\n");
    {
        int16_t buf[16];
        const int32_t in[] = { 0, 0x7FFFFFFF, INT32_MIN, 0x00008000,
                               0x00007FFF, -0x00008000, -0x00008001,
                               0x7FFF8000 };
        const int16_t want[] = { 0, 32767, -32768, 1, 0, 0, -1, 32767 };
        memcpy(buf, in, sizeof(in));   /* the same bytes on a LE host */
        const int n = pcmfold_s32_to_16(buf, sizeof(in));
        CHECK(n == 8, "folded %d of 8", n);
        for (int i = 0; i < 8; i++) {
            CHECK(buf[i] == want[i], "s32 0x%08X gave %d, expected %d",
                  (unsigned)in[i], buf[i], want[i]);
        }

        /* A 24-bit sample shifted left by 8 must fold to what the 24-bit
         * fold gives it: one rounding rule, not two. */
        int bad = -1;
        for (int32_t v = -0x800000; v <= 0x7FFFFF; v += 37) {
            int16_t b[2];
            const uint32_t u = (uint32_t)v << 8;
            memcpy(b, &u, 4);
            pcmfold_s32_to_16(b, 4);
            if (b[0] != fold1(v)) { bad = v; break; }
        }
        CHECK(bad < 0, "24-in-32 sample %d folded unlike the 24-bit fold", bad);
    }

    printf("  32-bit float\n");
    {
        int16_t buf[32];
        const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f,
                             1.0f / 32768.0f, -1.0f / 32768.0f,
                             2.0f, -2.0f, 1e30f, -1e30f,
                             (float)INFINITY, -(float)INFINITY, NAN };
        const int16_t want[] = { 0, 32767, -32768, 16384, -16384, 1, -1,
                                 32767, -32768, 32767, -32768,
                                 32767, -32768, 0 };
        enum { K = sizeof(in) / sizeof(in[0]) };
        memcpy(buf, in, sizeof(in));
        const int n = pcmfold_f32_to_16(buf, sizeof(in));
        CHECK(n == K, "folded %d of %d", n, (int)K);
        for (int i = 0; i < K; i++) {
            CHECK(buf[i] == want[i], "float %g gave %d, expected %d",
                  (double)in[i], buf[i], want[i]);
        }
    }

    printf("  32-bit in place, whole buffer\n");
    {
        enum { N = 1024 };
        static int16_t buf[N * 2], ref[N];
        static int32_t si[N];
        static float sf[N];
        for (int i = 0; i < N; i++) {
            si[i] = (int32_t)((uint32_t)i * 2654435761u);
            sf[i] = (float)((i * 7919) % 4001 - 2000) / 1500.0f;
        }
        memcpy(buf, si, sizeof(si));
        CHECK(pcmfold_s32_to_16(buf, sizeof(si)) == N, "s32 count");
        for (int i = 0; i < N; i++) {
            int16_t one[2]; memcpy(one, &si[i], 4);
            pcmfold_s32_to_16(one, 4); ref[i] = one[0];
        }
        CHECK(memcmp(buf, ref, sizeof(ref)) == 0, "s32 buffer differs from one-at-a-time");

        memcpy(buf, sf, sizeof(sf));
        CHECK(pcmfold_f32_to_16(buf, sizeof(sf)) == N, "f32 count");
        for (int i = 0; i < N; i++) {
            int16_t one[2]; memcpy(one, &sf[i], 4);
            pcmfold_f32_to_16(one, 4); ref[i] = one[0];
        }
        CHECK(memcmp(buf, ref, sizeof(ref)) == 0, "f32 buffer differs from one-at-a-time");

        /* Partial samples are left unread, as with 24. */
        CHECK(pcmfold_s32_to_16(buf, 7) == 1, "7 bytes is 1 sample");
        CHECK(pcmfold_f32_to_16(buf, 3) == 0, "3 bytes is no sample");
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
