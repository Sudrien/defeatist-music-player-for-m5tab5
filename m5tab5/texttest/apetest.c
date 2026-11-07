/*
 * apetest.c -- main/ape.h against test_audio_files/48-56.
 *
 * The files are lossless, so the test is exactness, and it has two
 * independent witnesses: the CRC32 each frame carries of its own decoded
 * bytes (every file, every frame), and for the files that are the whole
 * minute of 01, 01's own samples (48-52 and 56). 53-55 have no reference
 * file, only the CRC; 54 is 24-bit and its CRC is over the 24-bit
 * samples, before they are folded to 16.
 *
 * Also: a seek to a frame lands on the same samples as playing up to it;
 * the probe names what it refuses; a damaged file ends the decode and
 * does not run on.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APE_CHECK_CRC 1
#include "ape.h"

static int64_t seek_n;

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
    seek_n++;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
}

size_t storage_io_fread(void *dst, size_t len, FILE *f, storage_io_class_t cls)
{
    (void)cls;
    return fread(dst, 1, len, f);
}

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

#define DIR "../test_audio_files/"

static int16_t *ref;            /* file 01's samples, stereo interleaved */
static size_t ref_n;

static void load_ref(void)
{
    FILE *f = fopen(DIR "01 wav-pcm16.wav", "rb");
    if (!f) return;
    long pos = 12;
    for (;;) {
        uint8_t c[8];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(c, 1, 8, f) != 8) break;
        const uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "data", 4)) {
            ref = malloc(len);
            ref_n = fread(ref, 1, len, f) / 2;
            break;
        }
        pos += 8 + (long)len + (len & 1);
    }
    fclose(f);
}

typedef struct {
    const char *name;
    uint32_t level;
    int bits, channels;
    uint32_t rate;
    int vs01;               /* compare with 01 */
} fx_t;

static const fx_t fx[] = {
    { "48 ape-normal.ape",       2000, 16, 2, 44100, 1 },
    { "49 ape-fast.ape",         1000, 16, 2, 44100, 1 },
    { "50 ape-high.ape",         3000, 16, 2, 44100, 1 },
    { "51 ape-extrahigh.ape",    4000, 16, 2, 44100, 1 },
    { "52 ape-insane.ape",       5000, 16, 2, 44100, 1 },
    { "53 ape-insane-noisy.ape", 5000, 16, 2, 44100, 0 },
    { "54 ape-24bit.ape",        2000, 24, 2, 44100, 0 },
    { "55 ape-mono-22k.ape",     2000, 16, 1, 22050, 0 },
    { "56 ape-apev2-tag.ape",    2000, 16, 2, 44100, 1 },
};

/* All of it, through ape_next() in pieces of an odd size. */
static int16_t *decode_all(FILE *f, const ape_t *a, ape_dec_t **dout, uint64_t *got)
{
    ape_dec_t *d = ape_dec_new(a, STORAGE_IO_PLAYBACK);
    if (!d) return NULL;
    int16_t *out = malloc((size_t)a->blocks * a->channels * sizeof(int16_t) + 64);
    uint64_t n = 0;
    if (!ape_frame_open(d, f, 0)) { ape_dec_free(d); free(out); return NULL; }
    for (;;) {
        const uint32_t g = ape_next(d, f, out + n * a->channels, 3001);
        if (!g) break;
        n += g;
    }
    *got = n;
    *dout = d;
    return out;
}

int main(void)
{
    load_ref();
    CHECK(ref != NULL, "01 is missing");

    for (size_t k = 0; k < sizeof fx / sizeof fx[0]; k++) {
        const fx_t *x = &fx[k];
        printf("%s\n", x->name);
        char path[256];
        snprintf(path, sizeof path, DIR "%s", x->name);
        FILE *f = fopen(path, "rb");
        if (!f) { CHECK(0, "missing"); continue; }
        fseek(f, 0, SEEK_END);
        const int64_t size = ftell(f);

        ape_t a;
        const ape_status_t st = ape_probe(f, size, &a, STORAGE_IO_PLAYBACK);
        CHECK(st == APE_OK, "probe says %d (%s)", st, a.why ? a.why : "");
        if (st != APE_OK) { fclose(f); continue; }
        CHECK(a.version == 3990 && a.level == x->level, "version %u level %u",
              a.version, a.level);
        CHECK(a.bits == x->bits && a.channels == x->channels && a.rate == x->rate,
              "%u bit %u ch %u Hz", a.bits, a.channels, a.rate);
        CHECK(a.data_end <= size && a.data_end > 0, "data_end %lld of %lld",
              (long long)a.data_end, (long long)size);
        if (k == 8) {
            CHECK(a.data_end < size - 100, "the tag is counted as audio: data_end %lld of %lld",
                  (long long)a.data_end, (long long)size);
        }

        ape_dec_t *d = NULL;
        uint64_t got = 0;
        int16_t *pcm = decode_all(f, &a, &d, &got);
        CHECK(pcm != NULL, "no decode");
        if (!pcm) { ape_free(&a); fclose(f); continue; }
        CHECK(got == a.blocks, "%llu blocks, wanted %llu",
              (unsigned long long)got, (unsigned long long)a.blocks);
        CHECK(!d->err, "decoder error");
        CHECK(d->crc_bad == 0, "%u frames fail their CRC of %u", d->crc_bad, a.total_frames);
        if (x->vs01 && ref) {
            const size_t n = (size_t)got * a.channels;
            CHECK(n <= ref_n && !memcmp(pcm, ref, n * 2), "differs from 01");
        }

        /* A seek to a frame, then on from it, lands on the same samples. */
        const uint32_t fr[] = { 1, a.total_frames / 2, a.total_frames - 1 };
        for (int j = 0; j < 3; j++) {
            if (fr[j] >= a.total_frames) continue;
            int16_t *tmp = malloc((size_t)a.blocks_per_frame * a.channels * 2 + 64);
            ape_dec_t *e = ape_dec_new(&a, STORAGE_IO_PLAYBACK);
            CHECK(ape_frame_open(e, f, fr[j]), "open frame %u", fr[j]);
            uint32_t m = 0, g;
            while ((g = ape_next(e, f, tmp + (size_t)m * a.channels,
                                 a.blocks_per_frame - m)) > 0 && m + g <= a.blocks_per_frame) {
                m += g;
                if (m >= a.blocks_per_frame) break;
            }
            const uint64_t at = (uint64_t)fr[j] * a.blocks_per_frame;
            CHECK(m == a.blocks_per_frame || fr[j] == a.total_frames - 1,
                  "frame %u: %u blocks", fr[j], m);
            CHECK(!memcmp(tmp, pcm + at * a.channels, (size_t)m * a.channels * 2),
                  "frame %u after a seek differs", fr[j]);
            ape_dec_free(e);
            free(tmp);
        }

        ape_dec_free(d);
        free(pcm);
        ape_free(&a);
        fclose(f);
    }

    /* What the probe refuses, and says why. */
    {
        FILE *f = fopen(DIR "01 wav-pcm16.wav", "rb");
        ape_t a;
        fseek(f, 0, SEEK_END);
        CHECK(ape_probe(f, ftell(f), &a, STORAGE_IO_PLAYBACK) == APE_NOT_APE, "a wav is not an ape");
        fclose(f);

        /* A copy of 48 with the version field made old, then with the
         * level, bit depth and channel count made impossible. */
        f = fopen(DIR "48 ape-normal.ape", "rb");
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        uint8_t *b = malloc((size_t)n);
        fseek(f, 0, SEEK_SET);
        CHECK(fread(b, 1, (size_t)n, f) == (size_t)n, "read");
        fclose(f);
        struct { size_t at; uint8_t v; const char *what; ape_status_t want; } m[] = {
            { 4,       0x4E, "version 3.97", APE_UNSUPPORTED },   /* 3990 -> 3918 low byte: 0x0f4e */
            { 52,      0x17, "level 2007",   APE_UNSUPPORTED },
            { 52 + 16, 8,    "8 bit",        APE_UNSUPPORTED },
            { 52 + 18, 6,    "6 channels",   APE_UNSUPPORTED },
        };
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
            uint8_t save = b[m[i].at];
            b[m[i].at] = m[i].v;
            FILE *t = tmpfile();
            fwrite(b, 1, (size_t)n, t);
            ape_t q;
            const ape_status_t st = ape_probe(t, n, &q, STORAGE_IO_PLAYBACK);
            CHECK(st == m[i].want && q.why, "%s: status %d", m[i].what, st);
            if (st == APE_OK) ape_free(&q);
            fclose(t);
            b[m[i].at] = save;
        }

        /* Damage inside the audio: the CRC catches it or the decode stops,
         * and nothing runs on past the end of the file. */
        for (int trial = 0; trial < 6; trial++) {
            uint8_t *c = malloc((size_t)n);
            memcpy(c, b, (size_t)n);
            ape_t q;
            FILE *t = tmpfile();
            fwrite(c, 1, (size_t)n, t);
            ape_probe(t, n, &q, STORAGE_IO_PLAYBACK);
            const long at = (long)q.seek[1] + 5000 + trial * 997;
            c[at] ^= (uint8_t)(0x10 << (trial & 3));
            fclose(t);
            t = tmpfile();
            fwrite(c, 1, (size_t)n, t);
            uint64_t got = 0;
            ape_dec_t *d = NULL;
            int16_t *pcm = decode_all(t, &q, &d, &got);
            CHECK(pcm != NULL && got <= q.blocks && (d->err || d->crc_bad > 0),
                  "damage %d went unnoticed", trial);
            if (pcm) { ape_dec_free(d); free(pcm); }
            ape_free(&q);
            fclose(t);
            free(c);
        }
        free(b);
    }

    free(ref);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
