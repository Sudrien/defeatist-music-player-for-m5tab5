/*
 * imaadpcmtest.c -- IMA ADPCM WAV, as decoder.c's BACKEND_ADPCM reads it.
 *
 * The reference is ffmpeg: imaadpcm.h rounds the way ffmpeg does (see
 * ima_nibble()), so files 28 and 29 must decode to ffmpeg's samples
 * exactly. The hashes below are FNV-1a 64 over ffmpeg's s16le output,
 * cut at `fact`:
 *
 *   ffmpeg -i "28 wav-ima-adpcm.wav" -f s16le - | head -c $((5292000*2))
 *
 * What else is at risk:
 *
 *   - THE INTERLEAVE. Stereo blocks alternate 4-byte groups, not bytes
 *     or nibbles; mono has none. One file of each.
 *   - THE LAST BLOCK, short and padded. `fact` cuts it; 29 ends mid-block.
 *   - INDEPENDENCE. A seek decodes from a block in the middle of the
 *     file; that must give the same samples as decoding up to it.
 *   - A DAMAGED BLOCK, a step index past the table, must be refused
 *     rather than read past the table's end.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imaadpcm.h"

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

static uint64_t fnv(uint64_t h, const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint16_t v = (uint16_t)s[i];
        h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
        h = (h ^ (v >> 8)) * 0x100000001b3ull;
    }
    return h;
}

/* Decode the whole file the way adpcm_read() does: block by block, cut
 * at `total`. Returns the hash and the count. */
static uint64_t decode_file(FILE *f, const imawav_t *w, uint64_t first_block,
                            uint64_t *count)
{
    uint8_t *blk = malloc(w->block_align);
    int16_t *pcm = malloc((size_t)w->spb * w->channels * sizeof(int16_t));
    uint64_t h = 0xcbf29ce484222325ull, done = first_block * w->spb;
    int64_t at = (int64_t)first_block * w->block_align;

    fseek(f, (long)(w->data_start + at), SEEK_SET);
    while (at < w->data_len && done < w->total) {
        uint32_t want = w->block_align;
        if (w->data_len - at < want) want = (uint32_t)(w->data_len - at);
        const size_t got = fread(blk, 1, want, f);
        at += want;
        uint32_t n = ima_decode_block(blk, (uint32_t)got, w->channels, pcm);
        if (!n) break;
        if (done + n > w->total) n = (uint32_t)(w->total - done);
        h = fnv(h, pcm, (size_t)n * w->channels);
        done += n;
    }
    free(blk);
    free(pcm);
    *count = done - first_block * w->spb;
    return h;
}

static void check_file(const char *path, uint16_t ch, uint32_t rate,
                       uint64_t total, uint64_t want_hash)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  (skipped: %s not found)\n", path);
        return;
    }
    imawav_t w;
    CHECK(imawav_probe(f, &w), "%s did not probe", path);
    CHECK(w.channels == ch && w.sample_rate == rate,
          "%s: %u ch %u Hz", path, w.channels, w.sample_rate);
    CHECK(w.total == total, "%s: total %llu, fact says %llu", path,
          (unsigned long long)w.total, (unsigned long long)total);
    CHECK(w.spb == imawav_block_samples(w.block_align, w.channels),
          "%s: samples per block", path);

    uint64_t n = 0;
    const uint64_t h = decode_file(f, &w, 0, &n);
    CHECK(n == total, "%s: decoded %llu of %llu", path,
          (unsigned long long)n, (unsigned long long)total);
    CHECK(h == want_hash, "%s: hash %016llx, ffmpeg's is %016llx", path,
          (unsigned long long)h, (unsigned long long)want_hash);

    /* INDEPENDENCE: hash from block 0 the long way, then compare the
     * tail decoded from a mid-file block with the same tail of a full
     * decode. Done by decoding both into memory. */
    const uint64_t mid = (w.total / w.spb) / 2;
    uint8_t *blk = malloc(w.block_align);
    int16_t *pcm = malloc((size_t)w.spb * w.channels * sizeof(int16_t));
    int16_t *full = malloc((size_t)w.spb * w.channels * sizeof(int16_t));
    fseek(f, (long)(w.data_start + (int64_t)mid * w.block_align), SEEK_SET);
    size_t got = fread(blk, 1, w.block_align, f);
    const uint32_t a = ima_decode_block(blk, (uint32_t)got, w.channels, pcm);
    /* The same block, reached by reading every block before it. */
    fseek(f, (long)w.data_start, SEEK_SET);
    uint32_t b = 0;
    for (uint64_t i = 0; i <= mid; i++) {
        got = fread(blk, 1, w.block_align, f);
        b = ima_decode_block(blk, (uint32_t)got, w.channels, full);
    }
    CHECK(a == b && a && memcmp(pcm, full, (size_t)a * w.channels * 2) == 0,
          "%s: block %llu differs when sought to", path, (unsigned long long)mid);
    free(blk);
    free(pcm);
    free(full);
    fclose(f);
}

int main(void)
{
    printf("imaadpcmtest\n");

    printf("  block arithmetic\n");
    {
        CHECK(imawav_block_samples(1024, 2) == 1017, "1024 stereo is 1017");
        CHECK(imawav_block_samples(1024, 1) == 2041, "1024 mono is 2041");
        CHECK(imawav_block_samples(4, 1) == 1, "a header alone is one sample");
        CHECK(imawav_block_samples(3, 1) == 0, "less than a header is none");
        CHECK(imawav_block_samples(10, 1) == 9, "a part group past a whole one is not decoded");
        CHECK(imawav_block_samples(7, 1) == 1, "nor a part group alone");
        CHECK(imawav_block_samples(8, 2) == 1, "stereo headers alone");
        CHECK(imawav_block_samples(16, 2) == 9, "stereo, one group each");
    }

    printf("  one block by hand\n");
    {
        /* Mono: first sample 1000, index 0 (step 7), then nibbles 0..7
         * in byte order low-then-high. ffmpeg's rounding:
         * diff = ((2d + 1) * step) >> 3. */
        const uint8_t blk[8] = { 0xE8, 0x03, 0, 0, 0x10, 0x32, 0x54, 0x76 };
        int16_t out[9];
        CHECK(ima_decode_block(blk, 8, 1, out) == 9, "nine samples");
        int32_t p = 1000;
        int idx = 0;
        int16_t want[9];
        want[0] = 1000;
        for (int n = 0; n < 8; n++) {
            const int32_t step = k_ima_step[idx];
            p += ((2 * n + 1) * step) >> 3;
            idx += k_ima_index[n];
            if (idx < 0) idx = 0;
            want[n + 1] = (int16_t)p;
        }
        CHECK(memcmp(out, want, sizeof(want)) == 0,
              "nibble order or rounding: got %d %d %d, want %d %d %d",
              out[1], out[2], out[8], want[1], want[2], want[8]);
    }

    printf("  stereo interleave\n");
    {
        /* Left starts at 100, right at -100, both at step index 40 so
         * that step/8 is not zero; one group each, nibble 0 (up by
         * step/8) on the left and 8 (down by it) on the right. Output
         * must alternate L R L R, not run L L L L. */
        const uint8_t blk[16] = { 100, 0, 40, 0, 0x9C, 0xFF, 40, 0,
                                  0, 0, 0, 0, 0x88, 0x88, 0x88, 0x88 };
        int16_t out[18];
        CHECK(ima_decode_block(blk, 16, 2, out) == 9, "nine per channel");
        CHECK(out[0] == 100 && out[1] == -100, "headers: %d %d", out[0], out[1]);
        CHECK(out[2] > 100 && out[3] < -100,
              "left rises and right falls: %d %d", out[2], out[3]);
        CHECK(out[16] > out[2] && out[17] < out[3], "and keep going");
    }

    printf("  clamp and damage\n");
    {
        /* Top of the range, the largest step, all +7: must clamp, not wrap. */
        const uint8_t hi[8] = { 0xFF, 0x7F, 88, 0, 0x77, 0x77, 0x77, 0x77 };
        int16_t out[9];
        CHECK(ima_decode_block(hi, 8, 1, out) == 9, "decodes");
        CHECK(out[8] == 32767, "clamped high, got %d", out[8]);
        const uint8_t lo[8] = { 0x00, 0x80, 88, 0, 0xFF, 0xFF, 0xFF, 0xFF };
        ima_decode_block(lo, 8, 1, out);
        CHECK(out[8] == -32768, "clamped low, got %d", out[8]);

        const uint8_t bad[8] = { 0, 0, 89, 0, 0, 0, 0, 0 };
        CHECK(ima_decode_block(bad, 8, 1, out) == 0, "index 89 refused");
    }

    printf("  files 28 and 29 against ffmpeg\n");
    check_file("../test_audio_files/28 wav-ima-adpcm.wav", 2, 44100,
               2646000, 0x25d9176ac9e65637ull);
    check_file("../test_audio_files/29 wav-ima-adpcm-mono.wav", 1, 22050,
               160965, 0x7a22bdf12c8ae538ull);

    printf("  not IMA\n");
    {
        FILE *f = fopen("../test_audio_files/01 wav-pcm16.wav", "rb");
        if (f) {
            imawav_t w;
            CHECK(!imawav_probe(f, &w), "PCM was taken for IMA");
            fclose(f);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
