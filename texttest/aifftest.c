/*
 * aifftest.c -- AIFF and AIFF-C, as decoder.c's BACKEND_AIFF reads them
 * (6154): aiff.h finds the frames, pcmconv.h turns them into int16.
 *
 * Files 32 to 36 are file 01's audio written by ffmpeg in five layouts.
 * 16-bit audio widened to 24 bits or to float is exact in both, so 32 to
 * 35 must convert back to file 01's samples with no sample differing --
 * whatever the byte order or the width, which is the point of having
 * them. 36 is 8-bit mono at 22.05 kHz, a different signal, checked for
 * its shape and for the 8-bit arithmetic.
 *
 * Also: the 80-bit rate field, every layout pcmconv.h takes against a
 * hand-written sample, and a refusal for a compressed AIFF-C.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aiff.h"

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
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

static int16_t *ref;            /* file 01's samples */
static size_t ref_n;

static void load_ref(void)
{
    FILE *f = fopen(DIR "01 wav-pcm16.wav", "rb");
    if (!f) return;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12) { fclose(f); return; }
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

/* Decode a whole file the way aiff_read() does, in chunks. */
static int16_t *decode(const char *path, aiff_t *a, size_t *n)
{
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const int64_t size = ftell(f);
    if (!aiff_probe(f, size, a, STORAGE_IO_PLAYBACK)) { fclose(f); return NULL; }
    int16_t *out = malloc((size_t)a->frames * a->channels * sizeof(int16_t));
    uint8_t *buf = malloc((size_t)1024 * a->frame_bytes);
    fseek(f, (long)a->data_start, SEEK_SET);
    uint64_t done = 0;
    while (done < a->frames) {
        uint64_t want = a->frames - done;
        if (want > 1024) want = 1024;
        const size_t got = fread(buf, 1, (size_t)want * a->frame_bytes, f);
        const uint32_t fr = (uint32_t)(got / a->frame_bytes);
        if (!fr) break;
        pcm_conv(buf, fr * a->channels, a->layout, out + done * a->channels);
        done += fr;
    }
    free(buf);
    fclose(f);
    *n = (size_t)done * a->channels;
    return out;
}

static void against_01(const char *name, uint64_t frames, const char *kind)
{
    aiff_t a;
    size_t n;
    char path[256];
    snprintf(path, sizeof(path), DIR "%s", name);
    int16_t *s = decode(path, &a, &n);
    if (!s && !fopen(path, "rb")) {
        printf("  (skipped: %s not found)\n", name);
        return;
    }
    CHECK(s != NULL, "%s did not probe", name);
    if (!s) return;
    CHECK(a.channels == 2 && a.sample_rate == 44100, "%s: %u ch %u Hz",
          name, a.channels, a.sample_rate);
    CHECK(a.frames == frames, "%s: %llu frames, want %llu", name,
          (unsigned long long)a.frames, (unsigned long long)frames);
    CHECK(memcmp(a.kind, kind, 4) == 0, "%s: kind %.4s, want %s", name, a.kind, kind);
    size_t bad = 0;
    for (size_t i = 0; i < n && i < ref_n; i++) if (s[i] != ref[i]) bad++;
    CHECK(n == (size_t)frames * 2 && bad == 0, "%s: %zu samples, %zu differ from 01",
          name, n, bad);

    /* A seek is data_start + frame * frame_bytes: decode from frame
     * 33333 that way and it must be the same samples. */
    FILE *f = fopen(path, "rb");
    uint8_t buf[64];
    const uint64_t at = 33333 < a.frames ? 33333 : a.frames / 2;
    fseek(f, (long)(a.data_start + (int64_t)at * a.frame_bytes), SEEK_SET);
    const size_t got = fread(buf, 1, (size_t)a.frame_bytes * 4, f);
    int16_t four[16];
    pcm_conv(buf, (uint32_t)(got / a.layout.bytes), a.layout, four);
    CHECK(memcmp(four, s + at * 2, 8 * sizeof(int16_t)) == 0,
          "%s: frame %llu read by offset differs", name, (unsigned long long)at);
    fclose(f);
    free(s);
}

/* The 80-bit field for a whole-number rate, written as an encoder would. */
static void ext80(uint32_t rate, uint8_t *e)
{
    memset(e, 0, 10);
    int exp = 31;
    while (!(rate & (1u << exp))) exp--;
    const int biased = exp + 16383;
    e[0] = (uint8_t)(biased >> 8);
    e[1] = (uint8_t)biased;
    const uint64_t mant = (uint64_t)rate << (63 - exp);
    for (int i = 0; i < 8; i++) e[2 + i] = (uint8_t)(mant >> (56 - 8 * i));
}

int main(void)
{
    printf("aifftest\n");

    printf("  the 80-bit rate\n");
    {
        const uint32_t rates[] = { 8000, 11025, 22050, 32000, 44100, 48000,
                                   88200, 96000, 176400, 192000, 384000 };
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            uint8_t e[10];
            ext80(rates[i], e);
            CHECK(aiff_rate(e) == rates[i], "%u read back as %u", rates[i], aiff_rate(e));
        }
        /* 44100 as every encoder writes it, byte for byte. */
        const uint8_t k44[10] = { 0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0 };
        CHECK(aiff_rate(k44) == 44100, "the canonical 44100 gave %u", aiff_rate(k44));
        uint8_t neg[10];
        ext80(44100, neg);
        neg[0] |= 0x80;
        CHECK(aiff_rate(neg) == 0, "a negative rate was taken");
    }

    printf("  every layout, one sample\n");
    {
        /* -12345 in each layout pcmconv.h takes. 16-bit and wider are
         * exact; the float forms are -12345 / 32768. */
        struct { pcm_layout_t l; uint8_t b[8]; int16_t want; } t[] = {
            { { 2, true,  false, false }, { 0xCF, 0xC7 }, -12345 },
            { { 2, false, false, false }, { 0xC7, 0xCF }, -12345 },
            { { 3, true,  false, false }, { 0xCF, 0xC7, 0x00 }, -12345 },
            { { 3, false, false, false }, { 0x00, 0xC7, 0xCF }, -12345 },
            { { 4, true,  false, false }, { 0xCF, 0xC7, 0x00, 0x00 }, -12345 },
            { { 1, true,  false, false }, { 0xCF }, (int16_t)(0xCF00) },
            { { 1, true,  false, true  }, { 0x4F }, (int16_t)(0xCF00) },
            { { 4, true,  true,  false }, { 0xBE, 0xC0, 0xE4, 0x00 }, -12345 },
            { { 4, false, true,  false }, { 0x00, 0xE4, 0xC0, 0xBE }, -12345 },
            { { 8, true,  true,  false }, { 0xBF, 0xD8, 0x1C, 0x80, 0, 0, 0, 0 }, -12345 },
        };
        for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            int16_t o = 0;
            CHECK(pcm_layout_ok(t[i].l), "layout %zu refused", i);
            pcm_conv(t[i].b, 1, t[i].l, &o);
            CHECK(o == t[i].want, "layout %zu gave %d, want %d", i, o, t[i].want);
        }
        /* The extremes: the top of 24 bits rounds and clamps rather than
         * wrapping, as the WAV fold does. */
        const pcm_layout_t l24 = { 3, true, false, false };
        const uint8_t top[3] = { 0x7F, 0xFF, 0xFF }, bot[3] = { 0x80, 0, 0 };
        int16_t o;
        pcm_conv(top, 1, l24, &o);
        CHECK(o == 32767, "24-bit top gave %d", o);
        pcm_conv(bot, 1, l24, &o);
        CHECK(o == -32768, "24-bit bottom gave %d", o);
        const pcm_layout_t bad = { 8, true, false, false };
        CHECK(!pcm_layout_ok(bad), "an 8-byte integer was taken");
    }

    load_ref();
    if (!ref) {
        printf("  (skipped files: 01 not found)\n");
    } else {
        printf("  files 32 to 35 against file 01\n");
        against_01("32 aiff-pcm16.aiff", 2646000, "AIFF");
        against_01("33 aifc-sowt.aifc", 220500, "sowt");
        against_01("34 aiff-pcm24.aiff", 220500, "AIFF");
        against_01("35 aifc-fl32.aifc", 220500, "fl32");
    }

    printf("  36, 8-bit mono\n");
    {
        aiff_t a;
        size_t n;
        int16_t *s = decode(DIR "36 aiff-mono-8bit.aiff", &a, &n);
        if (!s) {
            printf("  (skipped or failed: 36)\n");
            CHECK(fopen(DIR "36 aiff-mono-8bit.aiff", "rb") == NULL, "36 did not probe");
        } else {
            CHECK(a.channels == 1 && a.sample_rate == 22050 && a.frames == 110250,
                  "36: %u ch %u Hz %llu frames", a.channels, a.sample_rate,
                  (unsigned long long)a.frames);
            size_t low = 0;
            for (size_t i = 0; i < n; i++) if (s[i] & 0xFF) low++;
            CHECK(low == 0, "8-bit samples with low bits set: %zu", low);
            free(s);
        }
    }

    printf("  refused\n");
    {
        /* A minimal AIFF-C saying ima4. */
        static const uint8_t ima4[] = {
            'F','O','R','M', 0,0,0,46, 'A','I','F','C',
            'C','O','M','M', 0,0,0,22, 0,2, 0,0,0,1, 0,16,
            0x40,0x0E,0xAC,0x44,0,0,0,0,0,0, 'i','m','a','4',
            'S','S','N','D', 0,0,0,10, 0,0,0,0, 0,0,0,0, 0,0 };
        FILE *f = tmpfile();
        fwrite(ima4, 1, sizeof(ima4), f);
        aiff_t a;
        CHECK(!aiff_probe(f, sizeof(ima4), &a, STORAGE_IO_PLAYBACK), "ima4 was taken");
        fclose(f);
    }

    free(ref);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
