/* SPDX-License-Identifier: MIT
 *
 * boxwin_run.c -- the player's own decoders, run for their windows
 * (u-defeatist-0002; boxwin.h).
 *
 * The decoders are the player's files, not copies: ../../main/ape.h,
 * ../../main/dsd.h and the minimp3 that cmake/vendored.cmake fetches. They
 * are the formats the player decodes in its own code. FLAC, AAC, Vorbis,
 * Opus and ALAC are esp_audio_codec's, precompiled and linked into the
 * launcher (8092): in a box they would be host calls, which run in machine
 * mode with interrupts on, and are not windows.
 *
 * ape.h and dsd.h read through storage_io_read_at() and storage_io_fread(),
 * which a box does not answer (feckless_box_calls has fopen/fread/fseek).
 * They are defined here, over stdio, each read a host call with the window
 * closed before it and opened after. Nothing else in this file touches a
 * file, so every host call this code makes ends a window on the record.
 *
 * Set-up (the probe, dsd_conv_new()'s filter design in double) is timed but
 * not recorded as windows: its double arithmetic is thousands of host calls
 * this file cannot see, so what it would record is not a window.
 *
 * No ESP-IDF in this file: test/host.c builds it on a host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "boxwin.h"

/* A box has the P4's PIE unit or it does not, and that is one of the
 * questions. Built in where the target has it; off until
 * bw_pie_selftest(). */
#if defined(__riscv) && !defined(BW_NO_PIE)
#define APE_PIE 1
#define DSD_PIE 1
#endif
#include "ape.h"
#include "dsd.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3_prefix.h"
#include "minimp3.h"

static bw_track_t s_track = { .open_us = -1, .into = NULL };

/* Around every host call this file makes. */
#define HOST_BEGIN()  bw_close(&s_track, bw_now())
#define HOST_END()    bw_open(&s_track, bw_now())

/* Where the player would hand the PCM on (audio_out_write(), a host call):
 * the window ends and the next begins. One clock read, which is itself the
 * host call that ends it. */
static void cut(void)
{
    const int64_t now = bw_now();
    bw_close(&s_track, now);
    bw_open(&s_track, now);
}

/* ---- storage_io's two reads, over a box's stdio ------------------------ */

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
    HOST_BEGIN();
    bw_lease();
    const bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
    bw_unlease();
    HOST_END();
    return ok;
}

size_t storage_io_fread(void *dst, size_t len, FILE *f, storage_io_class_t cls)
{
    (void)cls;
    HOST_BEGIN();
    bw_lease();
    const size_t n = fread(dst, 1, len, f);
    bw_unlease();
    HOST_END();
    return n;
}

static int64_t file_size(FILE *f)
{
    bw_lease();
    const int64_t n = fseek(f, 0, SEEK_END) == 0 ? (int64_t)ftell(f) : -1;
    bw_unlease();
    return n;
}

/* 64-bit division is a host call in a box (libgcc's __udivdi3, answered by
 * feckless_box_calls), so neither of these is used inside a decode loop:
 * the cap is worked out once, in frames, and the length once, at the end. */
static uint32_t ms_of(uint64_t frames, uint32_t rate)
{
    return rate ? (uint32_t)(frames * 1000u / rate) : 0;
}

static uint64_t cap_frames(uint32_t rate)
{
    return (uint64_t)rate * BW_MAX_AUDIO_MS / 1000u;
}

/* ---- MP3 and MP2: minimp3, a frame a window ----------------------------- */

#define MP3_BUF     (16 * 1024)
#define MP3_REFILL  (4 * 1024)      /* above any frame: 1441 bytes at 320 kbit/s */

static void run_mp3(FILE *f, int64_t size, bw_result_t *r)
{
    /* Not on the stack: mp3dec_decode_frame() keeps about 11.6 KB of
     * scratch there itself (CLAUDE.md). */
    static mp3dec_t dec;
    static uint8_t buf[MP3_BUF];
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    /* Past an ID3v2 tag, which can be megabytes of cover: ten bytes say
     * how long it is, syncsafe. */
    int64_t start = 0;
    uint8_t h[10];
    if (size > 10 && storage_io_read_at(f, 0, h, 10, STORAGE_IO_PLAYBACK) &&
        !memcmp(h, "ID3", 3) && !((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        start = 10 + (((int64_t)h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9]);
        if (h[5] & 0x10) start += 10;           /* a footer */
        if (start >= size) start = 0;
    }
    size_t have = 0, pos = 0;
    if (!storage_io_read_at(f, start, buf, 1, STORAGE_IO_PLAYBACK)) {
        r->why = "could not be read";
        return;
    }
    have = 1;

    mp3dec_init(&dec);
    uint64_t frames = 0, cap = UINT64_MAX;
    bool eof = false;
    int layer = 0;
    s_track.into = &r->win;
    cut();
    for (;;) {
        if (!eof && have - pos < MP3_REFILL) {
            memmove(buf, buf + pos, have - pos);
            have -= pos;
            pos = 0;
            const size_t n = storage_io_fread(buf + have, sizeof buf - have, f, STORAGE_IO_PLAYBACK);
            if (!n) eof = true;
            have += n;
        }
        if (have == pos) break;
        mp3dec_frame_info_t info;
        const int s = mp3dec_decode_frame(&dec, buf + pos, (int)(have - pos), pcm, &info);
        cut();
        if (!info.frame_bytes) {
            if (eof) break;
            if (have - pos >= MP3_REFILL) pos = have;   /* nothing in 4 KB: let it go */
            continue;
        }
        pos += (size_t)info.frame_bytes;
        if (s > 0) {
            frames += (uint64_t)s;
            if (r->rate != (uint32_t)info.hz) {         /* the first frame */
                r->rate = (uint32_t)info.hz;
                cap = cap_frames(r->rate);
            }
            r->channels = (uint32_t)info.channels;
            layer = info.layer;
            r->calls++;
            if (frames >= cap) break;
        }
    }
    s_track.into = NULL;
    if (!frames) {
        r->why = "no MPEG audio frame found";
        return;
    }
    snprintf(r->kind, sizeof r->kind, "mp%d", layer == 3 ? 3 : layer == 2 ? 2 : 1);
    r->audio_ms = ms_of(frames, r->rate);
    r->ok = true;
}

/* ---- APE: ape.h, BW_APE_BLOCKS a window --------------------------------- */

static void run_ape(FILE *f, int64_t size, bool pie, bw_result_t *r)
{
    ape_t a;
    memset(&a, 0, sizeof a);
    const ape_status_t st = ape_probe(f, size, &a, STORAGE_IO_PLAYBACK);
    if (st != APE_OK) {
        r->why = a.why ? a.why : st == APE_NOT_APE ? "not an APE" : "a damaged APE";
        ape_free(&a);
        return;
    }
#ifdef APE_HAVE_PIE
    ape_pie_on = pie;
#else
    (void)pie;
#endif
    ape_dec_t *d = ape_dec_new(&a, STORAGE_IO_PLAYBACK);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * BW_APE_BLOCKS);
    if (!d || !pcm || !ape_frame_open(d, f, 0)) {
        r->why = (!d || !pcm) ? "no memory for the decoder" : "its first frame would not open";
        ape_dec_free(d);
        ape_free(&a);
        free(pcm);
        return;
    }
    snprintf(r->kind, sizeof r->kind, "ape %u", (unsigned)a.level);
    r->rate = a.rate;
    r->channels = a.channels;

    uint64_t blocks = 0;
    const uint64_t cap = cap_frames(r->rate);
    s_track.into = &r->win;
    cut();
    for (;;) {
        const uint32_t n = ape_next(d, f, pcm, BW_APE_BLOCKS);
        cut();
        if (!n) break;
        blocks += n;
        r->calls++;
        if (blocks >= cap) break;
    }
    s_track.into = NULL;
    r->audio_ms = ms_of(blocks, r->rate);
    r->ok = blocks > 0 && !d->err;
    if (!r->ok) r->why = "unreadable data";
    ape_dec_free(d);
    ape_free(&a);
    free(pcm);
}

/* ---- DSD: dsd.h, a block a window --------------------------------------- */

static void run_dsd(FILE *f, int64_t size, bool pie, bw_result_t *r)
{
    dsd_t dd;
    const dsd_status_t st = dsd_probe(f, size, &dd, STORAGE_IO_PLAYBACK);
    if (st != DSD_OK) {
        r->why = st == DSD_DST ? "DST-compressed: there is no DST decoder"
               : st == DSD_UNSUPPORTED ? "a DSD this does not play" : "not a DSF or DFF";
        return;
    }
#ifdef DSD_HAVE_PIE
    dsd_pie_on = pie;
#else
    (void)pie;
#endif
    /* As decoder.c's dsd_open() sizes them. */
    dsd_conv_t *c = dsd_conv_new(&dd, DSD_BLOCK_MAX);
    uint8_t *in = malloc((size_t)dd.channels * DSD_BLOCK_MAX);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * (DSD_BLOCK_MAX / 8 + 96));
    if (!c || !in || !pcm) {
        r->why = "no memory for the converter";
        dsd_conv_free(c);
        free(in);
        free(pcm);
        return;
    }
    snprintf(r->kind, sizeof r->kind, "dsd%u %.3s", (unsigned)(dd.rate / DSD_OUT_RATE), dd.kind);
    r->rate = DSD_OUT_RATE;
    r->channels = dd.channels;

    dsd_rd_t rd;
    dsd_seek_plan(c, &rd, 0);
    uint64_t frames = 0;
    const uint64_t cap = cap_frames(r->rate);
    s_track.into = &r->win;
    cut();
    for (;;) {
        const size_t n = dsd_next(c, &rd, f, in, pcm, STORAGE_IO_PLAYBACK);
        cut();
        if (!n) break;
        frames += n;
        r->calls++;
        if (frames >= cap) break;
    }
    s_track.into = NULL;
    r->audio_ms = ms_of(frames, r->rate);
    r->ok = frames > 0;
    if (!r->ok) r->why = "no audio came out";
    dsd_conv_free(c);
    free(in);
    free(pcm);
}

/* ------------------------------------------------------------------------ */

static bool ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot + 1, ext);
}

void bw_run_file(const char *path, bool pie, bw_result_t *out)
{
    memset(out, 0, sizeof *out);
    const bool mp3 = ext_is(path, "mp3") || ext_is(path, "mp2");
    const bool ape = ext_is(path, "ape");
    const bool dsd = ext_is(path, "dsf") || ext_is(path, "dff");
    if (!mp3 && !ape && !dsd) {
        out->skipped = true;
        out->why = "not decoded by the app's own code";
        return;
    }
    bw_lease();
    FILE *f = fopen(path, "rb");
    bw_unlease();
    if (!f) {
        out->why = "would not open";
        return;
    }
    out->pie = pie && !mp3 && bw_pie_built();
    const int64_t t0 = bw_now();
    const int64_t size = file_size(f);
    if (mp3) run_mp3(f, size, out);
    else if (ape) run_ape(f, size, out->pie, out);
    else run_dsd(f, size, out->pie, out);
    /* What was not a recorded window: set-up, and the reads. */
    const int64_t all = bw_now() - t0;
    const int64_t rest = all - (int64_t)out->win.sum_us;
    out->setup_ms = rest > 0 ? (uint32_t)(rest / 1000) : 0;
    bw_lease();
    fclose(f);
    bw_unlease();
}

bool bw_pie_built(void)
{
#if defined(APE_HAVE_PIE) && defined(DSD_HAVE_PIE) && !defined(APE_PIE_MODEL)
    return true;
#else
    return false;
#endif
}

bool bw_pie_selftest(void)
{
#if defined(APE_HAVE_PIE) && defined(DSD_HAVE_PIE)
    const bool a = ape_pie_selftest();
    const bool d = dsd_pie_selftest();
    return a && d;
#else
    return false;
#endif
}

char *bw_format(const char *name, const bw_result_t *r, char *out, size_t len)
{
    if (!r->ok) {
        snprintf(out, len, "%.40s: %s", name, r->why ? r->why : "failed");
        return out;
    }
    const uint32_t cpu_ms = (uint32_t)(r->win.sum_us / 1000u);
    const uint32_t x100 = cpu_ms ? (uint32_t)((uint64_t)r->audio_ms * 100u / cpu_ms) : 0;
    snprintf(out, len,
             "%.40s: %s%s %u Hz x%u, %u ms of audio in %u ms (%u.%02ux); "
             "%u windows, longest %u.%u ms, %u of 5 ms or more, %u of 20 ms or more%s",
             name, r->kind, r->pie ? " PIE" : "", (unsigned)r->rate, (unsigned)r->channels,
             (unsigned)r->audio_ms, (unsigned)cpu_ms, (unsigned)(x100 / 100u), (unsigned)(x100 % 100u),
             (unsigned)r->win.windows, (unsigned)(r->win.max_us / 1000u),
             (unsigned)(r->win.max_us % 1000u / 100u),
             (unsigned)bw_stat_over(&r->win, BW_PANEL_US),
             (unsigned)bw_stat_over(&r->win, BW_AUDIO_US),
             r->win.max_us >= BW_AUDIO_US ? "  ** over the audio budget"
             : r->win.max_us >= BW_PANEL_US ? "  * over the panel's" : "");
    return out;
}
