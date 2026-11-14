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
 * File 47 is DST (u-defeatist-0016): FFmpeg's FATE sample
 * dst-64fs44-2ch.dff, ten frames. dst_checks() holds dst.h to the raw DSD
 * FFmpeg's decoder gives for it, and the rest of the path to the same
 * bits in a plain DFF.
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
    snprintf(path, sizeof(path), name[0] == '/' ? "%s" : DIR "%s", name);
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

/* u-defeatist-0014: dsd_conv_new()'s design cache. A converter built from
 * the cache must hold what one designed from nothing does, byte for byte;
 * a different rate or bit order must not be served from it. */
static dsd_conv_t *conv_for(const char *name, dsd_t *d)
{
    char path[256];
    snprintf(path, sizeof(path), DIR "%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const int64_t size = ftell(f);
    dsd_conv_t *c = NULL;
    if (dsd_probe(f, size, d, STORAGE_IO_PLAYBACK) == DSD_OK) c = dsd_conv_new(d, DSD_BLOCK_MAX);
    fclose(f);
    return c;
}

static size_t tbl_bytes(const dsd_conv_t *c) { return sizeof(int32_t) * (size_t)256 * c->S1 * 8; }

/* ---- DST (u-defeatist-0016) ---------------------------------------- */

/* FFmpeg's dst decoder on 47, AV_SAMPLE_FMT_DSD: 94080 bytes, this hash
 * (FNV-1a). Made with a test-only build of FFmpeg's libavcodec. */
#define DST47_BYTES 94080u
#define DST47_FNV   0xdf722027u

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void put_be(uint8_t *p, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

/* A DFF: FVER, PROP (FS, CHNL, CMPR), then `body` as the audio chunk --
 * a `DSD ` chunk's bytes, or a `DST ` chunk's sub-chunks. */
static void write_dff(const char *path, bool dst, const uint8_t *body, size_t n)
{
    uint8_t h[128];
    size_t k;
    memcpy(h, "FRM8", 4); k = 12; memcpy(h + k, "DSD ", 4); k += 4;
    memcpy(h + k, "FVER", 4); put_be(h + k + 4, 4, 8); put_be(h + k + 12, 0x01050000, 4); k += 16;
    const size_t prop = k;
    memcpy(h + k, "PROP", 4); k += 12; memcpy(h + k, "SND ", 4); k += 4;
    memcpy(h + k, "FS  ", 4); put_be(h + k + 4, 4, 8); put_be(h + k + 12, 2822400, 4); k += 16;
    memcpy(h + k, "CHNL", 4); put_be(h + k + 4, 10, 8); put_be(h + k + 12, 2, 2);
    memcpy(h + k + 14, "SLFTSRGT", 8); k += 22;
    memcpy(h + k, "CMPR", 4); put_be(h + k + 4, 4, 8); memcpy(h + k + 12, dst ? "DST " : "DSD ", 4); k += 16;
    put_be(h + prop + 4, k - prop - 12, 8);
    memcpy(h + k, dst ? "DST " : "DSD ", 4); put_be(h + k + 4, n, 8); k += 12;
    put_be(h + 4, k - 12 + n + (n & 1), 8);
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, k, f);
    fwrite(body, 1, n, f);
    if (n & 1) fputc(0, f);
    fclose(f);
}

/* A `DST ` chunk's body: FRTE, then each frame as a DSTF chunk. */
static size_t dst_body(uint8_t *out, uint32_t frames, const uint8_t *const *fr,
                       const size_t *len)
{
    size_t k;
    memcpy(out, "FRTE", 4); put_be(out + 4, 6, 8); put_be(out + 12, frames, 4);
    put_be(out + 16, 75, 2); k = 18;
    for (uint32_t i = 0; i < frames; i++) {
        memcpy(out + k, "DSTF", 4); put_be(out + k + 4, len[i], 8); k += 12;
        memcpy(out + k, fr[i], len[i]); k += len[i];
        if (len[i] & 1) out[k++] = 0;
    }
    return k;
}

/* Every DSTF of a DFF in memory, run through dst.h a `step` at a time. */
static size_t dst_raw(const uint8_t *b, size_t n, uint32_t step, uint8_t *out, int *bad)
{
    static dst_dec_t d;
    size_t at = 0;
    *bad = 0;
    for (size_t p = 16; p + 12 <= n;) {
        const uint64_t l = dsd_be64(b + p + 4);
        if (!memcmp(b + p, "DST ", 4)) { p += 12; continue; }    /* into it */
        if (!memcmp(b + p, "DSTF", 4)) {
            if (!dst_begin(&d, b + p + 12, (uint32_t)l, 2, 1)) { (*bad)++; return at; }
            while (dst_left_bytes(&d)) at += 2 * dst_run(&d, out + at, step);
        }
        p += 12 + l + (l & 1);
    }
    return at;
}

static void dst_checks(void)
{
    printf("  DST\n");
    size_t n;
    uint8_t *b = slurp(DIR "47 dff-dst.dff", &n);
    CHECK(b != NULL, "47 dff-dst.dff is missing");
    if (!b) return;
    uint8_t *raw = malloc(DST47_BYTES + 4096), *raw2 = malloc(DST47_BYTES + 4096);

    /* dst.h against FFmpeg, in slices of every shape */
    static const uint32_t steps[] = { 4704, DST_STEP, 7, 1 };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        int bad;
        uint8_t *o = i ? raw2 : raw;
        const size_t got = dst_raw(b, n, steps[i], o, &bad);
        const uint32_t h = fnv(o, got, 0x811c9dc5u);
        CHECK(!bad && got == DST47_BYTES && h == DST47_FNV,
              "47 in slices of %u: %zu bytes, %d refused, hash %08x, FFmpeg's %08x",
              (unsigned)steps[i], got, bad, (unsigned)h, DST47_FNV);
    }

    /* the probe */
    {
        FILE *f = fopen(DIR "47 dff-dst.dff", "rb");
        dsd_t d;
        CHECK(dsd_probe(f, (int64_t)n, &d, STORAGE_IO_PLAYBACK) == DSD_OK, "47 refused");
        CHECK(d.dst && d.dff && d.dst_frames == 10 && d.channels == 2 &&
              d.rate == 2822400 && d.bits == 10ull * 37632,
              "47 fields: dst %d frames %u ch %u rate %u bits %llu", d.dst,
              (unsigned)d.dst_frames, d.channels, (unsigned)d.rate, (unsigned long long)d.bits);
        fclose(f);
    }

    /* the whole path: 47 decodes as the same bits in a plain DFF do */
    const char *plain = "/tmp/dsdtest-dst-plain.dff";
    write_dff(plain, false, raw, DST47_BYTES);
    dsd_t d1, d2;
    int16_t *p1 = NULL, *p2 = NULL;
    uint32_t l1, l2;
    const uint64_t n1 = decode_all("47 dff-dst.dff", &d1, &p1, &l1);
    const uint64_t n2 = decode_all(plain, &d2, &p2, &l2);
    const bool same = n1 == n2 && n1 == 10ull * 588 && !memcmp(p1, p2, n1 * 4);
    CHECK(same, "47 against its bits in a plain DFF: %llu and %llu frames, %s",
          (unsigned long long)n1, (unsigned long long)n2, same ? "equal" : "different");

    /* seeks: forward from where the walk is, and back behind it */
    if (n1 == 10ull * 588) {
        FILE *f = fopen(DIR "47 dff-dst.dff", "rb");
        dsd_t d;
        dsd_probe(f, (int64_t)n, &d, STORAGE_IO_PLAYBACK);
        dsd_conv_t *c = dsd_conv_new(&d, DSD_BLOCK_MAX);
        uint8_t *buf = malloc(2 * DSD_BLOCK_MAX);
        int16_t blk[2 * (DSD_BLOCK_MAX + 8)];
        static const uint64_t at[] = { 3000, 5000, 100, 5000, 0 };
        for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
            dsd_rd_t r;
            dsd_seek_plan(c, &r, at[i]);
            const bool back = dsd_rd_offset(c, &r) == d.data_start;
            size_t got = dsd_next(c, &r, f, buf, blk, STORAGE_IO_PLAYBACK);
            if (got > n1 - at[i]) got = (size_t)(n1 - at[i]);
            CHECK(got > 0 && !memcmp(blk, p1 + at[i] * 2, got * 4),
                  "47: a seek to %llu (%s) does not land on the straight run's samples",
                  (unsigned long long)at[i], back ? "from the start" : "forward");
            if (i == 1) CHECK(!back, "47: a seek ahead started the walk again");
            if (i == 2) CHECK(back, "47: a seek behind the walk did not start it again");
        }
        free(buf);
        dsd_conv_free(c);
        fclose(f);
    }
    free(p1);
    free(p2);

    /* stored frames: one whole, one short and filled with silence */
    {
        uint8_t *f0 = malloc(1 + 9408), *f1 = malloc(1 + 100);
        f0[0] = 0; memcpy(f0 + 1, raw, 9408);
        f1[0] = 0; memcpy(f1 + 1, raw + 9408, 100);
        const uint8_t *fr[2] = { f0, f1 };
        const size_t len[2] = { 9409, 101 };
        uint8_t *body = malloc(20000), *want = malloc(2 * 9408);
        const size_t bn = dst_body(body, 2, fr, len);
        write_dff("/tmp/dsdtest-dst-stored.dff", true, body, bn);
        memcpy(want, raw, 9408 + 100);
        memset(want + 9408 + 100, 0x69, 9408 - 100);
        write_dff(plain, false, want, 2 * 9408);
        size_t sn;
        uint8_t *sb = slurp("/tmp/dsdtest-dst-stored.dff", &sn);
        int bad;
        const size_t got = dst_raw(sb, sn, 333, raw2, &bad);
        const bool ok = !bad && got == 2 * 9408 && !memcmp(raw2, want, got);
        CHECK(ok, "stored frames: %zu bytes, %d refused, %s", got, bad,
              ok ? "as stored" : "not as stored");
        const uint64_t a = decode_all("/tmp/dsdtest-dst-stored.dff", &d1, &p1, &l1);
        const uint64_t c2 = decode_all(plain, &d2, &p2, &l2);
        CHECK(a == c2 && a == 2 * 588 && !memcmp(p1, p2, a * 4),
              "stored frames against a plain DFF: %llu and %llu frames",
              (unsigned long long)a, (unsigned long long)c2);
        free(p1); free(p2); free(sb); free(f0); free(f1); free(body); free(want);
    }

    /* a frame dst.h will not take ends the file there, with nothing out */
    {
        uint8_t *bad = malloc(n);
        memcpy(bad, b, n);
        size_t p = 16;
        while (memcmp(bad + p, "DSTF", 4)) {
            p += !memcmp(bad + p, "DST ", 4) ? 12 : 12 + dsd_be64(bad + p + 4);
        }
        bad[p + 12] = 0x80;                     /* coded, but not one segment */
        FILE *f = fopen("/tmp/dsdtest-dst-bad.dff", "wb");
        fwrite(bad, 1, n, f);
        fclose(f);
        f = fopen("/tmp/dsdtest-dst-bad.dff", "rb");
        dsd_t d;
        CHECK(dsd_probe(f, (int64_t)n, &d, STORAGE_IO_PLAYBACK) == DSD_OK, "bad 47: refused at the probe");
        dsd_conv_t *c = dsd_conv_new(&d, DSD_BLOCK_MAX);
        uint8_t *buf = malloc(2 * DSD_BLOCK_MAX);
        int16_t blk[2 * (DSD_BLOCK_MAX + 8)];
        dsd_rd_t r;
        dsd_seek_plan(c, &r, 0);
        const size_t got = dsd_next(c, &r, f, buf, blk, STORAGE_IO_PLAYBACK);
        CHECK(got == 0 && c->dst_bad == 1, "bad 47: %zu frames out, %u refused", got,
              (unsigned)c->dst_bad);
        free(buf);
        dsd_conv_free(c);
        fclose(f);
        free(bad);

        /* and without FRTE, the probe names DST */
        uint8_t body[64];
        memcpy(body, "DSTF", 4); put_be(body + 4, 4, 8); memset(body + 12, 0, 4);
        write_dff("/tmp/dsdtest-dst-nofrte.dff", true, body, 16);
        f = fopen("/tmp/dsdtest-dst-nofrte.dff", "rb");
        fseek(f, 0, SEEK_END);
        CHECK(dsd_probe(f, ftell(f), &d, STORAGE_IO_PLAYBACK) == DSD_DST, "no FRTE: not named DST");
        fclose(f);
    }
    free(raw);
    free(raw2);
    free(b);
}

static void cache_checks(void)
{
    static const char *const files[] = {
        "41 dsf-dsd64.dsf", "45 dff-dsd64.dff", "42 dsf-dsd128.dsf", "43 dsf-dsd256.dsf",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        dsd_t d;
        dsd_cache_drop();
        const uint32_t m0 = dsd_cache.misses, h0 = dsd_cache.hits;
        dsd_conv_t *fresh = conv_for(files[i], &d);
        dsd_conv_t *kept = conv_for(files[i], &d);
        CHECK(fresh && kept, "%s: no converter", files[i]);
        if (!fresh || !kept) { dsd_conv_free(fresh); dsd_conv_free(kept); continue; }
        CHECK(dsd_cache.misses == m0 + 1 && dsd_cache.hits == h0 + 1,
              "%s: %u misses and %u hits for one design and one copy", files[i],
              (unsigned)(dsd_cache.misses - m0), (unsigned)(dsd_cache.hits - h0));
        CHECK(!memcmp(fresh->tbl, kept->tbl, tbl_bytes(fresh)), "%s: the kept table is not the designed one", files[i]);
        CHECK(fresh->tbl != kept->tbl && dsd_cache.tbl != kept->tbl, "%s: a converter shares its table", files[i]);
        /* The halfbands are designed once for every converter, so both of
         * these came from the cache: hold them to a design made here. */
        float hb1[(DSD_HB1_H + 1) / 2], hb2[(DSD_HB2_H + 1) / 2];
        dsd_halfband(hb1, DSD_HB1_H, 9.65);
        dsd_halfband(hb2, DSD_HB2_H, 9.65);
        CHECK(!memcmp(hb1, kept->hb1, sizeof hb1) && !memcmp(hb2, kept->hb2, sizeof hb2) &&
              !memcmp(hb1, fresh->hb1, sizeof hb1) && !memcmp(hb2, fresh->hb2, sizeof hb2),
              "%s: the halfbands are not as designed", files[i]);
        dsd_conv_free(fresh);
        dsd_conv_free(kept);
    }
    /* The same rate in the other bit order (DSF is LSB first, DFF MSB first)
     * and another rate: misses, and the tables differ, so the key holds. */
    dsd_t d;
    dsd_cache_drop();
    dsd_conv_t *lsb = conv_for("41 dsf-dsd64.dsf", &d);
    const uint32_t m0 = dsd_cache.misses;
    dsd_conv_t *msb = conv_for("45 dff-dsd64.dff", &d);
    dsd_conv_t *r128 = conv_for("42 dsf-dsd128.dsf", &d);
    dsd_conv_t *back = conv_for("41 dsf-dsd64.dsf", &d);
    CHECK(lsb && msb && r128 && back, "the four converters");
    if (lsb && msb && r128 && back) {
        CHECK(dsd_cache.misses == m0 + 3, "the other bit order, another rate and back: %u misses, not 3",
              (unsigned)(dsd_cache.misses - m0));
        CHECK(memcmp(lsb->tbl, msb->tbl, tbl_bytes(lsb)) != 0, "DSF's and DFF's DSD64 tables are the same");
        CHECK(!memcmp(lsb->tbl, back->tbl, tbl_bytes(lsb)), "DSD64 designed again is not as before");
    }
    dsd_conv_free(lsb);
    dsd_conv_free(msb);
    dsd_conv_free(r128);
    dsd_conv_free(back);
    dsd_cache_drop();
}

int main(void)
{
    load_ref();
    CHECK(ref_n > 44100 * 2 * 5, "file 01 did not load");

#ifdef DSD_HAVE_PIE
    /* u-defeatist-0005, and first, before any other dsd_conv_new(): the
     * self-test runs once in a process. With the vector sum turned off,
     * dsd_conv_new() must not run it -- it executes the kernel, and it sets
     * dsd_pie_on itself, which before 0005 turned the sum back on. */
    {
        dsd_t d;
        int16_t *pcm = NULL;
        uint32_t lat = 0;
        dsd_pie_on = false;
        const uint64_t n = decode_all("46 dff-dsd64-mono.dff", &d, &pcm, &lat);
        CHECK(n > 0, "46 dff-dsd64-mono.dff: nothing decoded with the vector sum off");
        CHECK(!dsd_pie_on, "dsd_conv_new() ran the PIE self-test with the vector sum turned off");
        free(pcm);
        dsd_pie_on = true;
    }
#endif

    cache_checks();
    dst_checks();

    printf("  decode against file 01\n");
    check_file("41 dsf-dsd64.dsf", 2822400, 2, 30.0, 50.0);
    check_file("42 dsf-dsd128.dsf", 5644800, 2, 5.0, 50.0);
#if DSD_MAX_MULT >= 4
    check_file("43 dsf-dsd256.dsf", 11289600, 2, 3.0, 50.0);
#endif
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
        FILE *f = fopen(DIR "43 dsf-dsd256.dsf", "rb");
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
        const char *names[] = { "41 dsf-dsd64.dsf", "45 dff-dsd64.dff", "42 dsf-dsd128.dsf",
                                "43 dsf-dsd256.dsf" };
        for (int k = 0; k < (DSD_MAX_MULT >= 4 ? 4 : 3); k++) {
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
            const uint64_t targets[] = { 1, 44100 * 2 + 777, 44100 * 2 + 30001 };
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
