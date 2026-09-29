/*
 * micpcmtest.c -- the microphone conversions in micpcm.h (5207).
 *
 * What is at risk:
 *
 *   - THE TDM SLOT. Slot 3 of four is the headset microphone; reading
 *     slot 2 is the right array microphone and sounds like a working
 *     recording of the wrong thing. And in place: an unpack that writes
 *     over a raw frame before reading it starts corrupting partway
 *     through a buffer.
 *   - THE CARRY. The UAC driver returns what it had at its timeout,
 *     which need not be whole frames. Dropping the partial frame shifts
 *     every later sample by one channel -- a stereo file swaps sides
 *     from there -- and it happens once in a long recording, which is
 *     the hardest place to find it.
 *   - SIGN. Little-endian int16 widened to int32 has to stay negative.
 *   - THE MONO FOLD. The mean of two samples, rounded the same way for
 *     both signs, with no overflow at full scale.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "micpcm.h"

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

static void put16(uint8_t *p, int16_t v) { memcpy(p, &v, 2); }

/* A sample value that says which frame and slot it came from, and is
 * negative for odd frames so sign extension is exercised throughout. */
static int16_t tag(size_t frame, unsigned slot)
{
    const int v = (int)((frame * 8 + slot) % 30000) + 1;
    return (int16_t)((frame & 1) ? -v : v);
}

static void test_tdm(void)
{
    printf("  tdm slot, in place\n");
    enum { N = 961 };                       /* odd, and more than one DMA buffer */
    for (unsigned slots = 2; slots <= 8; slots += 2) {
        for (unsigned slot = 0; slot < slots; slot++) {
            /* Sized as the capture buffer is: n frames of 2 x int32, or
             * n raw frames of `slots` int16 -- whichever is larger. */
            const size_t bytes = N * (slots * 2 > 8 ? slots * 2 : 8);
            int32_t *buf = malloc(bytes);
            uint8_t *raw = (uint8_t *)buf;
            for (size_t i = 0; i < N; i++)
                for (unsigned s = 0; s < slots; s++)
                    put16(raw + (i * slots + s) * 2, tag(i, s));
            const size_t n = micpcm_tdm_slot(buf, N, slots, slot);
            CHECK(n == N, "slots %u: %zu frames back", slots, n);
            size_t bad = 0;
            for (size_t i = 0; i < N; i++) if (buf[i] != tag(i, slot)) bad++;
            CHECK(bad == 0, "slots %u slot %u: %zu samples wrong", slots, slot, bad);
            free(buf);
        }
    }
    /* The one the board uses: 4 slots, slot 3, extremes. */
    {
        int32_t buf[4];
        uint8_t *raw = (uint8_t *)buf;
        const int16_t a[8] = { 1, 2, 3, -32768, 5, 6, 7, 32767 };
        for (int i = 0; i < 8; i++) put16(raw + i * 2, a[i]);
        CHECK(micpcm_tdm_slot(buf, 2, 4, 3) == 2, "2 frames");
        CHECK(buf[0] == -32768 && buf[1] == 32767, "full scale: %d %d",
              (int)buf[0], (int)buf[1]);
    }
    CHECK(micpcm_tdm_slot(NULL, 5, 1, 0) == 0, "1 slot must be refused");
    CHECK(micpcm_tdm_slot(NULL, 5, 4, 4) == 0, "slot past the end must be refused");
}

/* A stream of `frames` interleaved int16 frames, fed through
 * micpcm_s16_take() in chunks whose sizes come from `chunks` (cycled),
 * none aligned to a frame. The output must be the stream, exactly. */
static void run_carry(unsigned ch, const size_t *chunks, size_t nchunks, size_t frames)
{
    const size_t fb = ch * 2;
    uint8_t *stream = malloc(frames * fb);
    for (size_t i = 0; i < frames; i++)
        for (unsigned c = 0; c < ch; c++)
            put16(stream + i * fb + c * 2, tag(i, c));

    uint8_t *raw = malloc(4096 + fb);
    int32_t *out = malloc(frames * ch * sizeof(int32_t));
    int32_t *tmp = malloc(4096 / 2 * sizeof(int32_t) + 16);
    size_t carry = 0, pos = 0, outn = 0, k = 0;
    while (pos < frames * fb) {
        size_t got = chunks[k++ % nchunks];
        if (got > frames * fb - pos) got = frames * fb - pos;
        memcpy(raw + carry, stream + pos, got);
        pos += got;
        const size_t n = micpcm_s16_take(raw, carry, got, ch, tmp, &carry);
        CHECK(carry < fb, "carry %zu not under a frame (%zu)", carry, fb);
        memcpy(out + outn * ch, tmp, n * ch * sizeof(int32_t));
        outn += n;
    }
    CHECK(outn == frames, "%u ch: %zu of %zu frames", ch, outn, frames);
    CHECK(carry == 0, "%u ch: %zu bytes left at the end", ch, carry);
    size_t bad = 0;
    for (size_t i = 0; i < outn; i++)
        for (unsigned c = 0; c < ch; c++)
            if (out[i * ch + c] != tag(i, c)) bad++;
    CHECK(bad == 0, "%u ch: %zu samples wrong (a channel shift?)", ch, bad);
    free(stream); free(raw); free(out); free(tmp);
}

static void test_carry(void)
{
    printf("  16-bit take, with a partial frame carried\n");
    const size_t odd[] = { 1, 3, 7, 1000, 5, 2, 1, 999 };
    const size_t whole[] = { 960 };
    const size_t tiny[] = { 1 };
    for (unsigned ch = 1; ch <= 2; ch++) {
        run_carry(ch, odd, sizeof(odd) / sizeof(odd[0]), 4801);
        run_carry(ch, whole, 1, 4800);
        run_carry(ch, tiny, 1, 97);
    }
    /* Nothing new: whatever was carried stays carried. */
    {
        uint8_t raw[8] = { 0x34, 0x12, 0x78 };
        int32_t out[4];
        size_t carry = 99;
        CHECK(micpcm_s16_take(raw, 3, 0, 2, out, &carry) == 0 && carry == 3,
              "3 bytes of a 4-byte frame: %zu carried", carry);
        CHECK(raw[0] == 0x34 && raw[1] == 0x12 && raw[2] == 0x78, "carry moved");
        CHECK(micpcm_s16_take(raw, 0, 4, 0, out, &carry) == 0 && carry == 0,
              "0 channels takes nothing");
    }
}

static void test_mono(void)
{
    printf("  mono fold\n");
    int32_t f[] = { 100, 200,  -1, 0,  -3, -4,  8388607, 8388607,
                    -8388608, -8388608,  32767, -32768,  2147483647, 2147483647 };
    const size_t n = sizeof(f) / sizeof(f[0]) / 2;
    micpcm_mono(f, n);
    CHECK(f[0] == 150, "100,200 -> %d", (int)f[0]);
    CHECK(f[1] == -1, "-1,0 -> %d (floor)", (int)f[1]);
    CHECK(f[2] == -4, "-3,-4 -> %d (floor)", (int)f[2]);
    CHECK(f[3] == 8388607, "24-bit +FS -> %d", (int)f[3]);
    CHECK(f[4] == -8388608, "24-bit -FS -> %d", (int)f[4]);
    CHECK(f[5] == -1, "opposite 16-bit rails -> %d", (int)f[5]);
    CHECK(f[6] == 2147483647, "no overflow at int32 max: %d", (int)f[6]);
}

int main(void)
{
    printf("micpcmtest\n");
    test_tdm();
    test_carry();
    test_mono();
    printf("micpcmtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
