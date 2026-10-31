/*
 * mp4mp3test.c -- MP3 in MP4, framed by the sample table and decoded by
 * minimp3 one frame at a time (6153), the way decoder.c's
 * mp4_mp3_read() does it.
 *
 * File 31 is file 01's minute, LAME at 128k, in an MP4. What is checked:
 *
 *   - the probe takes it as MP3 (esds object type 0x6B), not AAC, and
 *     reads rate and channels from the sample entry;
 *   - every sample is a frame minimp3 accepts, and the decode is the
 *     minute: frames x 1152 samples per channel;
 *   - the audio is the audio: against file 01's PCM, after finding
 *     LAME's delay, the signal-to-noise ratio is that of a 128k encode
 *     rather than of noise or of a shifted copy;
 *   - a seek lands on a frame boundary and, after a reset decoder skips
 *     the frames whose bit reservoir is gone, carries on in step with a
 *     decode from the start.
 *
 * Needs components/minimp3/minimp3.h, which cmake/vendored.cmake or
 * tools/fetch_vendored.sh puts there.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "../main/mp4seek.c"

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
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

#define FILE31 "../test_audio_files/31 m4a-mp3.m4a"
#define FILE01 "../test_audio_files/01 wav-pcm16.wav"

static mp3dec_t dec;

/*
 * mp4_mp3_read(), without the decoder_t around it: since 6156 a window
 * filled by mp4_read_raw() in whole runs, with minimp3 taking a frame
 * at a time out of it. The window is deliberately small here, to make
 * the refill and the carry-over happen often.
 */
static uint8_t win[6000];
static int win_len, win_pos;
static bool win_eof;
static int refills;
static uint32_t win_frames;     /* frames consumed since the reset */

static void win_reset(void) { win_len = win_pos = 0; win_eof = false; win_frames = 0; }

static int read_frame(FILE *f, mp4_t *m, uint8_t *in, size_t cap, int16_t *out)
{
    (void)in; (void)cap;
    for (;;) {
        if (!win_eof && win_len - win_pos < 2881) {
            const int keep = win_len - win_pos;
            memmove(win, win + win_pos, (size_t)keep);
            win_pos = 0;
            win_len = keep;
            const size_t got = mp4_read_raw(f, m, win + keep, sizeof(win) - (size_t)keep);
            if (!got) win_eof = true;
            win_len += (int)got;
            refills++;
        }
        if (win_pos >= win_len) return 0;
        mp3dec_frame_info_t fi;
        memset(&fi, 0, sizeof(fi));
        const int spc = mp3dec_decode_frame(&dec, win + win_pos, win_len - win_pos, out, &fi);
        if (fi.frame_bytes <= 0) {
            if (win_eof) return 0;
            win_pos++;
            continue;
        }
        win_pos += fi.frame_bytes;
        win_frames++;
        if (spc <= 0 || fi.channels <= 0) continue;
        return spc * fi.channels;
    }
}

int main(void)
{
    printf("mp4mp3test\n");

    FILE *f = fopen(FILE31, "rb");
    if (!f) {
        printf("  (skipped: %s not found)\n", FILE31);
        return 0;
    }

    static mp4_t m;
    CHECK(mp4_probe(f, &m), "file 31 did not probe");
    CHECK(m.mp3 && !m.opus && !m.alac, "not taken as MP3");
    CHECK(m.channels == 2 && m.sample_rate == 44100, "%u ch %u Hz",
          m.channels, m.sample_rate);
    CHECK(m.max_size <= 2881, "a sample larger than any MP3 frame: %u", m.max_size);

    uint8_t *in = malloc(m.max_size);
    const size_t total_cap = (size_t)m.count * 1152 * 2 + 4096;
    int16_t *pcm = malloc(total_cap * sizeof(int16_t));
    size_t got = 0;

    printf("  the whole minute\n");
    mp3dec_init(&dec);
    win_reset();
    for (;;) {
        const int n = read_frame(f, &m, in, m.max_size, pcm + got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    CHECK(got == (size_t)m.count * 1152 * 2, "decoded %zu, frames say %zu",
          got, (size_t)m.count * 1152 * 2);
    /* 2298 frames of 418 bytes through a 6000-byte window: a read every
     * dozen frames or so, not every frame. */
    printf("    %d reads for %u frames\n", refills, m.count);
    CHECK(refills < (int)m.count / 4, "%d reads for %u frames", refills, m.count);

    printf("  against file 01\n");
    {
        FILE *w = fopen(FILE01, "rb");
        if (!w) {
            printf("  (skipped: %s not found)\n", FILE01);
        } else {
            fseek(w, 0, SEEK_END);
            const long wl = ftell(w);
            int16_t *ref = malloc((size_t)wl);
            fseek(w, 0, SEEK_SET);
            const size_t rn = fread(ref, 1, (size_t)wl, w) / 2;
            fclose(w);
            /* The data chunk: walk for it rather than assume 44. */
            const uint8_t *rb = (const uint8_t *)ref;
            size_t data = 12;
            while (data + 8 < rn * 2 && memcmp(rb + data, "data", 4) != 0) {
                data += 8 + (rb[data + 4] | rb[data + 5] << 8 |
                             rb[data + 6] << 16 | (size_t)rb[data + 7] << 24);
            }
            const int16_t *r = (const int16_t *)(rb + data + 8);
            const size_t rlen = (rn * 2 - data - 8) / 2;

            /* LAME's delay, found rather than assumed: the shift (in
             * stereo frames) that best matches a stretch near the start. */
            int best = -1;
            double best_err = 1e300;
            for (int sh = 0; sh < 3000; sh++) {
                double e = 0;
                for (size_t i = 40000; i < 60000; i++) {
                    const double d = (double)pcm[i + (size_t)sh * 2] - r[i];
                    e += d * d;
                }
                if (e < best_err) { best_err = e; best = sh; }
            }
            double sig = 0, err = 0;
            const size_t n = (got - (size_t)best * 2 < rlen ? got - (size_t)best * 2 : rlen) - 4096;
            for (size_t i = 0; i < n; i++) {
                const double s = r[i], d = (double)pcm[i + (size_t)best * 2] - s;
                sig += s * s;
                err += d * d;
            }
            const double snr = 10 * log10(sig / (err > 0 ? err : 1));
            printf("    delay %d samples, SNR %.1f dB over %zu samples\n",
                   best, snr, n / 2);
            CHECK(best > 500 && best < 2500, "a delay of %d is not LAME's", best);
            /* 26 dB on the board's landmark at 128k: pure tones are
             * LAME's worst case. A misaligned or wrong decode is near 0
             * or below, so the line is drawn well clear of both. */
            CHECK(snr > 15.0, "SNR %.1f dB: not the same audio", snr);
            free(ref);
        }
    }

    printf("  a seek\n");
    {
        const uint32_t cs = mp4_seek_cs(&m, 30);
        CHECK(cs == (uint32_t)((uint64_t)m.cur * 1152 * 100 / 44100),
              "landed %u cs, sample %u starts at %u cs", cs, m.cur,
              (uint32_t)((uint64_t)m.cur * 1152 * 100 / 44100));
        const uint32_t first = m.cur;
        mp3dec_init(&dec);
        win_reset();
        static int16_t buf[1152 * 2];
        /* Frames whose reservoir reaches before the seek decode to
         * nothing; read_frame() skips them. The first frame that does
         * decode is reported, not checked: it has no previous frame to
         * overlap-add with, so it differs. From the second on, the
         * output must be the full decode's exactly. */
        const int n = read_frame(f, &m, in, m.max_size, buf);
        const uint32_t landed = first + win_frames - 1;
        CHECK(n == 1152 * 2, "first frame after the seek gave %d", n);
        CHECK(landed - first <= 2, "%u frames lost to the reservoir", landed - first);
        const int16_t *want = pcm + (size_t)landed * 1152 * 2;
        int bad = 0;
        for (int i = 0; i < n; i++) if (abs(buf[i] - want[i]) > 2) bad++;
        /* The first frame after a reset may differ a little (the
         * overlap-add has no previous frame); the second must not. */
        const int n2 = read_frame(f, &m, in, m.max_size, buf);
        int bad2 = 0;
        for (int i = 0; i < n2; i++) if (buf[i] != want[1152 * 2 + i]) bad2++;
        printf("    landed on frame %u (asked %u); first frame %d off, second %d\n",
               landed, first, bad, bad2);
        CHECK(n2 == 1152 * 2 && bad2 == 0, "second frame after a seek differs in %d", bad2);
    }

    free(in);
    mp4_free(&m);
    fclose(f);

    /*
     * 6155: file 40 is MPEG-1 Layer II, which decoder.c sends to the
     * same minimp3 as .mp3. Every frame must decode, as layer 2, and
     * the minute must be file 01's audio.
     */
    printf("  40, layer II\n");
    FILE *g = fopen("../test_audio_files/40 mp2.mp2", "rb");
    if (!g) {
        printf("  (skipped: 40 not found)\n");
    } else {
        fseek(g, 0, SEEK_END);
        const long gl = ftell(g);
        uint8_t *mp2 = malloc((size_t)gl);
        fseek(g, 0, SEEK_SET);
        const size_t have = fread(mp2, 1, (size_t)gl, g);
        fclose(g);
        mp3dec_init(&dec);
        size_t at2 = 0, out2 = 0;
        int frames = 0, not2 = 0;
        while (at2 < have) {
            mp3dec_frame_info_t fi;
            const int spc = mp3dec_decode_frame(&dec, mp2 + at2, (int)(have - at2),
                                                pcm + out2, &fi);
            if (fi.frame_bytes <= 0) break;
            at2 += (size_t)fi.frame_bytes;
            if (spc > 0) {
                frames++;
                if (fi.layer != 2) not2++;
                out2 += (size_t)spc * fi.channels;
            }
        }
        CHECK(frames > 2000 && not2 == 0, "%d frames, %d not layer II", frames, not2);
        CHECK(out2 >= 2646000 * 2, "only %zu samples", out2);
        free(mp2);
    }
    free(pcm);
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
