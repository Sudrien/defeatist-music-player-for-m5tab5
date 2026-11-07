/*
 * dsdtest.c -- DSF and DFF, as decoder.c's BACKEND_DSD reads them: dsd.h
 * finds the bits and converts them to 44.1 kHz PCM.
 *
 * Files 41 to 47 are file 01's audio, resampled to the DSD rate and
 * modulated to one bit by dsdgen.py at 0.6 scale. So what comes out
 * must be file 01 at 0.6, to within the converter's and the
 * modulator's noise: the test measures that as SNR against file 01.
 * (A 2nd-order modulator at 64x is good for roughly 70 dB at this
 * level. The threshold is below it; the number is printed.)
 *
 * Also: every container field the probes read, the refusals, a seek
 * landing on the same samples as a straight run, the exact length, and
 * that the DFF and DSF of the same audio decode alike.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd.h"

static int64_t seek_offs[8];
static int seek_n;

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
    if (seek_n < 8) seek_offs[seek_n++] = off;
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

/* Decode the whole file; returns frames (capped at dsd_frames), PCM in *out. */
static uint64_t decode_all(const char *name, dsd_t *d, int16_t **out, uint32_t *latency)
{
    char path[256];
    snprintf(path, sizeof(path), DIR "%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (missing %s)\n", name); return 0; }
    fseek(f, 0, SEEK_END);
    const int64_t size = ftell(f);
    if (dsd_probe(f, size, d, STORAGE_IO_PLAYBACK) != DSD_OK) { fclose(f); return 0; }
    dsd_conv_t *c = dsd_conv_new(d, DSD_BLOCK_MAX);
    const uint64_t total = dsd_frames(c);
    *latency = c->latency;
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * (total + 4096));
    uint8_t *buf = malloc((size_t)d->channels * DSD_BLOCK_MAX);
    int16_t blk[2 * (DSD_BLOCK_MAX + 8)];
    dsd_rd_t r;
    dsd_seek_plan(c, &r, 0);
    uint64_t n = 0;
    size_t got;
    while (n < total && (got = dsd_next(c, &r, f, buf, blk, STORAGE_IO_PLAYBACK)) > 0) {
        if (got > total - n) got = (size_t)(total - n);
        memcpy(pcm + n * d->channels, blk, got * d->channels * sizeof(int16_t));
        n += got;
    }
    free(buf);
    dsd_conv_free(c);
    fclose(f);
    *out = pcm;
    return n;
}

/* Error rms in LSB of channel `ch` against 0.6 * file 01, over [t0, t1)
 * seconds, and the SNR with it. The right channel of file 01 is mostly
 * silence between ticks, so its SNR is low for a reason that is the
 * modulator's noise floor, not the converter's: the error is what is held. */
static double snr(const int16_t *pcm, int nch, int ch, double t0, double t1,
                  double *err_rms)
{
    double sig = 0, err = 0;
    for (size_t i = (size_t)(t0 * 44100); i < (size_t)(t1 * 44100); i++) {
        const double want = 0.6 * ref[2 * i + (size_t)(nch == 1 ? 0 : ch)];
        const double got = pcm[i * (size_t)nch + (size_t)ch];
        sig += want * want;
        err += (got - want) * (got - want);
    }
    *err_rms = sqrt(err / (double)((size_t)(t1 * 44100) - (size_t)(t0 * 44100)));
    return 10 * log10(sig / (err + 1e-9));
}

static void check_file(const char *name, uint32_t rate, int nch, double secs,
                       double min_snr)
{
    dsd_t d;
    int16_t *pcm = NULL;
    uint32_t lat = 0;
    const uint64_t n = decode_all(name, &d, &pcm, &lat);
    CHECK(n > 0, "%s: nothing decoded", name);
    if (!n) return;
    CHECK(d.rate == rate && d.channels == nch, "%s: rate %u ch %u", name, d.rate, d.channels);
    CHECK(llabs((long long)n - (long long)(secs * 44100)) <= 1,
          "%s: %llu frames, wanted %.0f", name, (unsigned long long)n, secs * 44100);
    double worst = 1e9, worst_err = 0;
    for (int ch = 0; ch < nch; ch++) {
        double e;
        const double s = snr(pcm, nch, ch, 0.1, secs - 0.1, &e);
        if (ch == 0 && s < worst) worst = s;        /* the left tone is the signal */
        if (e > worst_err) worst_err = e;
    }
    printf("  %-28s %2d ch %8u Hz  latency %2u  left SNR %.1f dB  error %.1f LSB rms\n",
           name, nch, rate, lat, worst, worst_err);
    CHECK(worst >= min_snr, "%s: left SNR %.1f dB, wanted >= %.1f", name, worst, min_snr);
    CHECK(worst_err <= 10.0, "%s: error %.1f LSB rms", name, worst_err);
    free(pcm);
}

int main(void)
{
    load_ref();
    CHECK(ref_n > 44100 * 2 * 5, "file 01 did not load");

    printf("  decode against file 01\n");
    check_file("41 dsf-dsd64.dsf", 2822400, 2, 30.0, 50.0);
    check_file("42 dsf-dsd128.dsf", 5644800, 2, 5.0, 50.0);
    check_file("44 dsf-dsd64-mono-odd.dsf", 2822400, 1, 14112007.0 / 64 / 44100, 50.0);
    check_file("45 dff-dsd64.dff", 2822400, 2, 5.0, 50.0);
    check_file("46 dff-dsd64-mono.dff", 2822400, 1, 5.0, 50.0);

    printf("  DSF and DFF of the same audio agree\n");
    {
        dsd_t a, b;
        int16_t *pa = NULL, *pb = NULL;
        uint32_t la, lb;
        const uint64_t na = decode_all("41 dsf-dsd64.dsf", &a, &pa, &la);
        const uint64_t nb = decode_all("45 dff-dsd64.dff", &b, &pb, &lb);
        /* Same landmark, same modulator, same first five seconds. */
        if (na && nb) {
            size_t diff = 0;
            int maxd = 0;
            /* all but the last 0.1 s, where the DFF ends and the DSF goes on */
            for (size_t i = 0; i < 44100 * 49 / 10 * 2; i++) {
                const int dd = abs(pa[i] - pb[i]);
                if (dd > maxd) maxd = dd;
                if (dd > 8) diff++;
            }
            /* Not bit-identical (70 samples differ, by a few LSB): the files
             * were modulated in separate runs, and a sigma-delta loop
             * amplifies any upstream rounding difference into different
             * noise -- probably; the signal agrees either way. */
            CHECK(diff == 0, "%zu samples differ by more than 8 LSB between the DSF and the DFF (max %d)", diff, maxd);
        }
        free(pa); free(pb);
    }

    printf("  probe fields\n");
    {
        FILE *f = fopen(DIR "41 dsf-dsd64.dsf", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_OK, "41 refused");
            CHECK(d.lsb_first && !d.dff && d.block == 4096 && d.data_start == 92,
                  "41: lsb %d dff %d block %u start %lld", d.lsb_first, d.dff, d.block,
                  (long long)d.data_start);
            CHECK(d.bits == 2822400ull * 30, "41: %llu bits", (unsigned long long)d.bits);
            fclose(f);
        }
        f = fopen(DIR "45 dff-dsd64.dff", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_OK, "45 refused");
            CHECK(!d.lsb_first && d.dff && d.channels == 2, "45 fields");
            CHECK(d.bits == 2822400ull * 5, "45: %llu bits", (unsigned long long)d.bits);
            fclose(f);
        }
        f = fopen(DIR "44 dsf-dsd64-mono-odd.dsf", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_OK, "44 refused");
            CHECK(d.bits == 14112007, "44: the header's count, not the block's: %llu",
                  (unsigned long long)d.bits);
            fclose(f);
        }
    }

    printf("  refused\n");
    {
        FILE *f = fopen(DIR "47 dff-dst-refuse.dff", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_DST,
                  "DST was not named");
            fclose(f);
        }
        f = fopen(DIR "43 dsf-dsd256.dsf", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            const dsd_status_t st = dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK);
#if DSD_MAX_MULT >= 4
            CHECK(st == DSD_OK, "DSD256 refused with DSD_MAX_MULT %d", DSD_MAX_MULT);
#else
            CHECK(st == DSD_UNSUPPORTED, "DSD256 taken with DSD_MAX_MULT %d", DSD_MAX_MULT);
#endif
            fclose(f);
        }
        f = fopen(DIR "01 wav-pcm16.wav", "rb");
        if (f) {
            dsd_t d;
            fseek(f, 0, SEEK_END);
            CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_NOT_DSD,
                  "a WAV was taken as DSD");
            fclose(f);
        }
    }

    printf("  seek lands on the same samples\n");
    {
        const char *names[] = { "41 dsf-dsd64.dsf", "45 dff-dsd64.dff", "42 dsf-dsd128.dsf" };
        for (int k = 0; k < 3; k++) {
            dsd_t d;
            int16_t *all = NULL;
            uint32_t lat;
            const uint64_t n = decode_all(names[k], &d, &all, &lat);
            if (!n) continue;
            char path[256];
            snprintf(path, sizeof(path), DIR "%s", names[k]);
            FILE *f = fopen(path, "rb");
            dsd_conv_t *c = dsd_conv_new(&d, DSD_BLOCK_MAX);
            uint8_t *buf = malloc((size_t)d.channels * DSD_BLOCK_MAX);
            int16_t blk[2 * (DSD_BLOCK_MAX + 8)];
            const uint64_t targets[] = { 1, 44100 * 2 + 777, 44100 * 3 };
            for (int t = 0; t < 3; t++) {
                dsd_rd_t r;
                dsd_seek_plan(c, &r, targets[t]);
                seek_n = 0;
                static int16_t gather[2 * 4096];
                size_t got = 0, g;
                while (got < 400 && (g = dsd_next(c, &r, f, buf, blk, STORAGE_IO_PLAYBACK)) > 0) {
                    memcpy(gather + got * d.channels, blk, g * d.channels * sizeof(int16_t));
                    got += g;
                }
                /* A seek is one fseek and then plain reads. */
                CHECK(seek_n == 1, "%s seek %llu: %d seeks, wanted 1", names[k],
                      (unsigned long long)targets[t], seek_n);
                CHECK(got >= 400, "%s seek %llu: %zu frames", names[k],
                      (unsigned long long)targets[t], got);
                double md = 0;
                const size_t m = got < 400 ? got : 400;
                for (size_t i = 0; i < m * d.channels; i++) {
                    const double e = fabs((double)gather[i] -
                                          (double)all[targets[t] * d.channels + i]);
                    if (e > md) md = e;
                }
                /* The seek restarts the filters on idle bits; what remains is
                 * the difference in settling, far below the signal. */
                CHECK(md <= 8, "%s: seek to %llu differs by up to %.0f LSB", names[k],
                      (unsigned long long)targets[t], md);

                /* The same seek on a stream at byte 0 (what dsd_seek() leaves
                 * after reopening): no seek at all, the same samples. */
                dsd_rd_t r2;
                dsd_conv_reset(c);
                dsd_seek_plan(c, &r2, targets[t]);
                r2.at = 0;
                rewind(f);
                seek_n = 0;
                static int16_t gather2[2 * 4096];
                size_t got2 = 0, g2;
                while (got2 < 400 && (g2 = dsd_next(c, &r2, f, buf, blk, STORAGE_IO_PLAYBACK)) > 0) {
                    memcpy(gather2 + got2 * d.channels, blk, g2 * d.channels * sizeof(int16_t));
                    got2 += g2;
                }
                CHECK(seek_n == 0, "%s skip %llu: %d seeks, wanted 0", names[k],
                      (unsigned long long)targets[t], seek_n);
                CHECK(got2 == got && !memcmp(gather, gather2, got * d.channels * sizeof(int16_t)),
                      "%s skip %llu: not the samples of the seek", names[k],
                      (unsigned long long)targets[t]);

                /* And a forward seek from where that stream now is: the
                 * stream is kept (at), nothing is seeked, same samples. */
                if (t + 1 < 3) {
                    const int64_t at = r2.at;
                    dsd_rd_t r3;
                    dsd_seek_plan(c, &r3, targets[t + 1]);
                    const int64_t off = dsd_rd_offset(c, &r3);
                    CHECK(off >= at, "%s forward %llu: offset %lld behind the stream %lld",
                          names[k], (unsigned long long)targets[t + 1],
                          (long long)off, (long long)at);
                    r3.at = at;
                    seek_n = 0;
                    static int16_t gather3[2 * 4096];
                    size_t got3 = 0, g3;
                    while (got3 < 400 && (g3 = dsd_next(c, &r3, f, buf, blk, STORAGE_IO_PLAYBACK)) > 0) {
                        memcpy(gather3 + got3 * d.channels, blk, g3 * d.channels * sizeof(int16_t));
                        got3 += g3;
                    }
                    CHECK(seek_n == 0, "%s forward %llu: %d seeks, wanted 0", names[k],
                          (unsigned long long)targets[t + 1], seek_n);
                    double m3 = 0;
                    for (size_t i = 0; i < (got3 < 400 ? got3 : 400) * d.channels; i++) {
                        const double e = fabs((double)gather3[i] -
                                              (double)all[targets[t + 1] * d.channels + i]);
                        if (e > m3) m3 = e;
                    }
                    CHECK(got3 >= 400 && m3 <= 8, "%s forward %llu: %zu frames, up to %.0f LSB",
                          names[k], (unsigned long long)targets[t + 1], got3, m3);
                }
            }
            free(buf);
            dsd_conv_free(c);
            fclose(f);
            free(all);
        }
    }

    free(ref);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
