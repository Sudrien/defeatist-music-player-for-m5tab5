/*
 * tsdemuxtest.c -- tsdemux.h: the audio out of an MPEG-2 TS segment.
 *
 * The segments are built here the way a broadcast packager builds them:
 * PAT, PMT naming an AAC stream and a timed-ID3 stream, PES packets
 * with PTS, adaptation-field stuffing at the end of each PES, null
 * packets between. Then fed whole, split at every byte, and a byte at a
 * time, and the output must be exactly the audio every way.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/tsdemux.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define PMT_PID   0x1000
#define AUDIO_PID 0x0101
#define ID3_PID   0x0102

static uint8_t ts[188 * 400];
static size_t  tsn;
static uint8_t want[188 * 400];
static size_t  wantn;
static int     cc_of[0x2000];

/* One packet: header, optional adaptation field of `af` total bytes
 * (0 = none), then payload. */
static void packet(int pid, bool pusi, size_t af, const uint8_t *pay, size_t pn)
{
    uint8_t *p = ts + tsn;
    memset(p, 0xFF, 188);
    p[0] = 0x47;
    p[1] = (uint8_t)((pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F));
    p[2] = (uint8_t)(pid & 0xFF);
    const int afc = (af ? 2 : 0) | (pn ? 1 : 0);
    p[3] = (uint8_t)((afc << 4) | (cc_of[pid] & 0x0F));
    if (pn) cc_of[pid]++;
    size_t o = 4;
    if (af) {
        p[4] = (uint8_t)(af - 1);
        if (af > 1) p[5] = 0x00;            /* flags; the rest stuffing */
        o += af;
    }
    if (o + pn != 188) { printf("bad packet build\n"); exit(2); }
    memcpy(p + o, pay, pn);
    tsn += 188;
}

static void psi(int pid, const uint8_t *sec, size_t len)
{
    uint8_t pay[184];
    memset(pay, 0xFF, sizeof(pay));
    pay[0] = 0;                             /* pointer field */
    memcpy(pay + 1, sec, len);
    packet(pid, true, 0, pay, 184);
}

static void pat_pmt(uint8_t audio_type)
{
    /* PAT: one network entry (prog 0) to prove it is skipped, then prog 1. */
    const uint8_t pat[] = { 0x00, 0xB0, 17, 0x00, 0x01, 0xC1, 0x00, 0x00,
                            0x00, 0x00, 0xE0, 0x10,
                            0x00, 0x01, 0xE0 | (PMT_PID >> 8), PMT_PID & 0xFF,
                            0xDE, 0xAD, 0xBE, 0xEF };
    psi(0, pat, sizeof(pat));
    /* PMT: PCR on audio, no program info, ID3 stream first, then audio. */
    const uint8_t pmt[] = { 0x02, 0xB0, 26, 0x00, 0x01, 0xC1, 0x00, 0x00,
                            0xE0 | (AUDIO_PID >> 8), AUDIO_PID & 0xFF, 0xF0, 0x00,
                            0x15, 0xE0 | (ID3_PID >> 8), ID3_PID & 0xFF, 0xF0, 0x00,
                            audio_type, 0xE0 | (AUDIO_PID >> 8), AUDIO_PID & 0xFF, 0xF0, 0x03,
                            0x0A, 0x01, 0x00,       /* a 3-byte descriptor */
                            0xDE, 0xAD, 0xBE, 0xEF };
    psi(PMT_PID, pmt, sizeof(pmt));
}

/* A PES of `n` audio bytes, PTS in its header, split into packets with
 * the last one stuffed. `first_af` forces an adaptation field of that
 * size on the first packet, to push the PES header across packets. */
static void pes(int pid, const uint8_t *audio, size_t n, size_t first_af, bool record)
{
    uint8_t buf[188 * 64];
    const uint8_t hdr[14] = { 0, 0, 1, 0xC0, 0, 0, 0x80, 0x80, 5,
                              0x21, 0x00, 0x01, 0x00, 0x01 };
    memcpy(buf, hdr, 14);
    memcpy(buf + 14, audio, n);
    size_t total = 14 + n, off = 0;
    bool first = true;
    while (off < total) {
        size_t room = 184;
        size_t af = 0;
        if (first && first_af) { af = first_af; room = 184 - af; }
        size_t take = total - off < room ? total - off : room;
        if (take < room) af = room - take + af;   /* stuff the last */
        packet(pid, first, af, buf + off, take);
        off += take;
        first = false;
    }
    if (record) { memcpy(want + wantn, audio, n); wantn += n; }
}

static void null_packet(void)
{
    uint8_t z[184] = { 0 };
    packet(0x1FFF, false, 0, z, 184);
}

static void build(uint8_t audio_type, size_t first_af)
{
    tsn = wantn = 0;
    memset(cc_of, 0, sizeof(cc_of));
    pat_pmt(audio_type);
    uint8_t a[2000];
    for (int f = 0; f < 6; f++) {
        const size_t n = 300 + (size_t)f * 211;
        for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(i == 0 ? 0xFF : i == 1 ? 0xF1 : (f * 31 + i) & 0xFF);
        pes(AUDIO_PID, a, n, f == 2 ? first_af : 0, true);
        if (f == 1) {
            const uint8_t id3[] = "ID3\x04\x00\x00\x00\x00\x00\x05PRIVx";
            pes(ID3_PID, id3, sizeof(id3) - 1, 0, false);
            null_packet();
        }
    }
}

static uint8_t out[188 * 400 + TSDEMUX_OUT_SLACK];

static size_t feed_cut(tsdemux_t *d, const uint8_t *in, size_t n, size_t cut)
{
    tsdemux_init(d);
    size_t o = 0;
    if (cut == (size_t)-1) {
        for (size_t i = 0; i < n; i++) o += tsdemux_feed(d, in + i, 1, out + o);
    } else {
        o += tsdemux_feed(d, in, cut, out + o);
        o += tsdemux_feed(d, in + cut, n - cut, out + o);
    }
    return o;
}

static void every_way(const char *what)
{
    tsdemux_t d;
    int bad = 0;
    for (size_t cut = 0; cut <= tsn; cut += (cut < 600 ? 1 : 7)) {
        const size_t o = feed_cut(&d, ts, tsn, cut);
        if ((o != wantn || memcmp(out, want, wantn) != 0 || d.err) && !bad++)
            printf("  %s: cut %zu gave %zu bytes (want %zu), err %d\n", what, cut, o, wantn, (int)d.err);
    }
    const size_t o = feed_cut(&d, ts, tsn, (size_t)-1);
    if ((o != wantn || memcmp(out, want, wantn) != 0) && !bad++)
        printf("  %s: byte at a time gave %zu (want %zu)\n", what, o, wantn);
    CHECK(!bad, "%s: %d ways wrong", what, bad);
}

int main(void)
{
    tsdemux_t d;

    build(0x0F, 0);
    every_way("aac");
    feed_cut(&d, ts, tsn, tsn);
    CHECK(d.audio == TSDEMUX_AAC && d.audio_pid == AUDIO_PID && d.pmt_pid == PMT_PID,
          "found AAC on 0x%x via PMT 0x%x", d.audio_pid, d.pmt_pid);
    CHECK(d.cc_gaps == 0 && d.resyncs == 0 && d.bytes_lost == 0, "clean: gaps %u resyncs %u lost %u",
          (unsigned)d.cc_gaps, (unsigned)d.resyncs, (unsigned)d.bytes_lost);

    build(0x03, 0);
    every_way("mp3");
    feed_cut(&d, ts, tsn, tsn);
    CHECK(d.audio == TSDEMUX_MP3, "mp3 type");

    /* PES header split across packets: 180 bytes of adaptation field
     * leave 4 bytes of payload in the first packet of that PES. */
    build(0x0F, 180);
    every_way("pes header across packets");

    /* Garbage in front: found again, counted, output unchanged. */
    build(0x0F, 0);
    memmove(ts + 37, ts, tsn);
    for (int i = 0; i < 37; i++) ts[i] = (uint8_t)(i * 13 + 1) == 0x47 ? 0 : (uint8_t)(i * 13 + 1);
    tsn += 37;
    {
        const size_t o = feed_cut(&d, ts, tsn, tsn);
        CHECK(o == wantn && !memcmp(out, want, wantn) && d.resyncs >= 1 && d.bytes_lost == 37,
              "leading garbage: %zu of %zu, resyncs %u, lost %u", o, wantn,
              (unsigned)d.resyncs, (unsigned)d.bytes_lost);
    }

    /* A lost audio packet: counted, the rest kept. */
    build(0x0F, 0);
    {
        /* Find the 4th audio packet and cut it out. */
        int seen = 0;
        for (size_t p = 0; p < tsn; p += 188) {
            const int pid = ((ts[p + 1] & 0x1F) << 8) | ts[p + 2];
            if (pid == AUDIO_PID && ++seen == 4) {
                memmove(ts + p, ts + p + 188, tsn - p - 188);
                tsn -= 188;
                break;
            }
        }
        const size_t o = feed_cut(&d, ts, tsn, tsn);
        CHECK(d.cc_gaps == 1 && o < wantn && o > wantn - 185, "one gap, one packet short: gaps %u, %zu of %zu",
              (unsigned)d.cc_gaps, o, wantn);
    }

    /* A duplicated audio packet is dropped. */
    build(0x0F, 0);
    {
        for (size_t p = 0; p < tsn; p += 188) {
            const int pid = ((ts[p + 1] & 0x1F) << 8) | ts[p + 2];
            if (pid == AUDIO_PID && !(ts[p + 1] & 0x40)) {
                memmove(ts + p + 188, ts + p, tsn - p);
                tsn += 188;
                break;
            }
        }
        const size_t o = feed_cut(&d, ts, tsn, tsn);
        CHECK(o == wantn && !memcmp(out, want, wantn) && d.cc_gaps == 0, "duplicate dropped (%zu of %zu)", o, wantn);
    }

    /* Refusals. */
    build(0x11, 0);
    feed_cut(&d, ts, tsn, tsn);
    CHECK(d.err == TSDEMUX_ERR_LATM, "LATM refused (%d)", (int)d.err);
    build(0x81, 0);
    {
        const size_t o = feed_cut(&d, ts, tsn, tsn);
        CHECK(d.err == TSDEMUX_ERR_AC3 && o == 0, "AC-3 refused, nothing out");
    }
    build(0x1B, 0);                         /* H.264 where audio was */
    feed_cut(&d, ts, tsn, tsn);
    CHECK(d.err == TSDEMUX_ERR_NO_AUDIO, "no audio (%d)", (int)d.err);

    /* A PMT that says it is longer than its packet. */
    build(0x0F, 0);
    ts[188 + 4 + 2] = 0xB0 | 0x0F;      /* section_length high nibble */
    feed_cut(&d, ts, tsn, tsn);
    CHECK(d.err == TSDEMUX_ERR_SECTION, "oversize section refused (%d)", (int)d.err);

    /* Audio before the PMT (joined mid-stream): ignored until it is named. */
    build(0x0F, 0);
    {
        /* Put a copy of the first audio packet in front of everything. */
        memmove(ts + 188, ts, tsn);
        memcpy(ts, ts + 188 * 3, 188);
        tsn += 188;
        const size_t o = feed_cut(&d, ts, tsn, tsn);
        CHECK(o == wantn && !memcmp(out, want, wantn), "early audio ignored (%zu of %zu)", o, wantn);
    }

    /* Random bytes never write past the slack. */
    {
        static uint8_t junk[5000];
        uint32_t x = 12345;
        for (int round = 0; round < 200; round++) {
            for (size_t i = 0; i < sizeof(junk); i++) {
                x = x * 1103515245u + 12345u;
                junk[i] = (uint8_t)(x >> 16);
                if ((i % 188) == 0 && (x & 1)) junk[i] = 0x47;
            }
            tsdemux_init(&d);
            d.pmt_pid = (int)(x & 0x1FFF);
            d.audio_pid = (int)((x >> 3) & 0x1FFF);
            size_t o = 0;
            for (size_t i = 0; i < sizeof(junk); i += 700) {
                const size_t n = sizeof(junk) - i < 700 ? sizeof(junk) - i : 700;
                const size_t w = tsdemux_feed(&d, junk + i, n, out);
                if (w > n + TSDEMUX_OUT_SLACK) o = (size_t)-1;
            }
            if (o == (size_t)-1) { CHECK(0, "junk wrote past slack"); break; }
        }
        CHECK(1, "junk");
    }

    printf("tsdemuxtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
