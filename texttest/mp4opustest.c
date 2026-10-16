/*
 * mp4opustest.c -- Opus in MP4, sent to the Ogg parser as pages (6152).
 *
 * mp4seek.c turns each MP4 sample into one Ogg page and puts OpusHead
 * and OpusTags in front. The Ogg parser on the board is a precompiled
 * archive, so what can be checked here is that the stream it is handed
 * is a correct Ogg Opus stream, built from file 30:
 *
 *   - every page starts "OggS", has a CRC that an independent
 *     implementation agrees with, and a sequence number one more than
 *     the last;
 *   - the first page is BOS and holds OpusHead built from `dOps`
 *     (version 1, two channels, ffmpeg's 312-sample pre-skip, 48 kHz
 *     little-endian), the second holds OpusTags, the last is EOS;
 *   - granule positions rise and the last one is the track's end;
 *   - the packets, laced back together, are the MP4's samples byte for
 *     byte, in order -- so a read buffer that splits a run cannot drop
 *     or duplicate one;
 *   - a seek starts a fresh stream: headers again, sequence from 0.
 *
 * Decoding the result is the other half, and is not done here because
 * there is no Opus decoder in the host tests. It was done by hand for
 * 6152: ffmpeg decodes the stream to the same 2880000 samples as it
 * decodes the .m4a, none differing. See ARCHITECTURE.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define FILE30 "../test_audio_files/30 m4a-opus.m4a"

/* Table-driven, unlike mp4seek.c's bitwise one, so the two are not the
 * same code agreeing with itself. */
static uint32_t crc_tab[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        crc_tab[i] = c;
    }
}

static uint32_t crc_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = (i >= 22 && i < 26) ? 0 : p[i];
        c = (c << 8) ^ crc_tab[(c >> 24) ^ b];
    }
    return c;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Read the whole stream out through `cap`-byte reads. */
static uint8_t *drain(FILE *f, mp4_t *m, size_t cap, size_t *len)
{
    size_t room = 1 << 20, used = 0;
    uint8_t *all = malloc(room);
    uint8_t *buf = malloc(cap);
    size_t n;
    while ((n = mp4_read(f, m, buf, cap)) > 0) {
        if (used + n > room) {
            room = (used + n) * 2;
            all = realloc(all, room);
        }
        memcpy(all + used, buf, n);
        used += n;
    }
    free(buf);
    *len = used;
    return all;
}

typedef struct {
    int pages;
    uint32_t packets;           /* audio packets, after the two headers */
    uint64_t last_granule;
    bool bad_crc, bad_seq, granule_fell, eos_last, bos_first;
    bool head_ok, tags_ok;
    uint8_t *payload;           /* audio packets concatenated */
    size_t payload_len;
} walk_t;

static void walk(const uint8_t *s, size_t len, walk_t *w)
{
    memset(w, 0, sizeof(*w));
    w->payload = malloc(len);
    size_t p = 0;
    uint64_t prev = 0;
    while (p + 27 <= len) {
        if (memcmp(s + p, "OggS", 4) != 0) { w->bad_seq = true; break; }
        const uint8_t flags = s[p + 5];
        uint64_t g = 0;
        for (int i = 7; i >= 0; i--) g = (g << 8) | s[p + 6 + i];
        const uint32_t seq = le32(s + p + 18);
        const uint8_t segs = s[p + 26];
        size_t body = 0;
        for (int i = 0; i < segs; i++) body += s[p + 27 + i];
        const size_t hdr = 27 + segs;
        if (p + hdr + body > len) { w->bad_seq = true; break; }

        if (crc_of(s + p, hdr + body) != le32(s + p + 22)) w->bad_crc = true;
        if (seq != (uint32_t)w->pages) w->bad_seq = true;
        if (w->pages == 0) {
            w->bos_first = (flags & 0x02) != 0;
            const uint8_t *h = s + p + hdr;
            w->head_ok = body == 19 && memcmp(h, "OpusHead", 8) == 0 &&
                         h[8] == 1 && h[9] == 2 &&
                         (h[10] | (h[11] << 8)) == 312 &&
                         le32(h + 12) == 48000 && h[18] == 0;
        } else if (w->pages == 1) {
            w->tags_ok = body >= 16 && memcmp(s + p + hdr, "OpusTags", 8) == 0;
        } else {
            if (g < prev) w->granule_fell = true;
            prev = g;
            memcpy(w->payload + w->payload_len, s + p + hdr, body);
            w->payload_len += body;
            w->packets++;
        }
        w->eos_last = (flags & 0x04) != 0;
        w->last_granule = g;
        w->pages++;
        p += hdr + body;
    }
}

int main(void)
{
    printf("mp4opustest\n");
    crc_init();

    FILE *f = fopen(FILE30, "rb");
    if (!f) {
        printf("  (skipped: %s not found)\n", FILE30);
        return 0;
    }

    static mp4_t m;
    CHECK(mp4_probe(f, &m), "file 30 did not probe");
    CHECK(m.opus && !m.alac, "not taken as Opus");
    CHECK(m.channels == 2 && m.sample_rate == 48000, "%u ch %u Hz",
          m.channels, m.sample_rate);
    CHECK(mp4_duration_sec(&m) == 60, "duration %u", mp4_duration_sec(&m));

    /* The MP4's own samples, end to end, to compare against. */
    size_t want_len = 0;
    for (uint32_t i = 0; i < m.count; i++) want_len += m.size[i];
    uint8_t *want = malloc(want_len);
    size_t at = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        storage_io_read_at(f, m.offset[i], want + at, m.size[i], STORAGE_IO_PLAYBACK);
        at += m.size[i];
    }

    /* The decoder's window offers at least half of its 8 KB. The third
     * size is the tightest that still holds the largest packet and its
     * header, which splits runs at nearly every packet. */
    printf("  whole stream, at three read sizes\n");
    const size_t caps[] = { 8192, 2048, 27 + m.max_size / 255 + 1 + m.max_size };
    for (int c = 0; c < 3; c++) {
        mp4_seek_cs(&m, 0);
        size_t len;
        uint8_t *s = drain(f, &m, caps[c], &len);
        walk_t w;
        walk(s, len, &w);
        CHECK(!w.bad_crc, "cap %zu: a CRC disagrees", caps[c]);
        CHECK(!w.bad_seq, "cap %zu: page sequence or framing broken", caps[c]);
        CHECK(w.bos_first && w.head_ok, "cap %zu: OpusHead page wrong", caps[c]);
        CHECK(w.tags_ok, "cap %zu: OpusTags page wrong", caps[c]);
        CHECK(w.eos_last, "cap %zu: last page is not EOS", caps[c]);
        CHECK(!w.granule_fell, "cap %zu: granule went backwards", caps[c]);
        CHECK(w.packets == m.count, "cap %zu: %u packets, %u samples",
              caps[c], w.packets, m.count);
        CHECK(w.payload_len == want_len &&
              memcmp(w.payload, want, want_len) == 0,
              "cap %zu: packets are not the MP4's samples", caps[c]);
        /* The end: total time in the table, at 48 kHz. */
        uint64_t total = 0;
        for (int e = 0; e < m.stts_n; e++)
            total += (uint64_t)m.stts_count[e] * m.stts_delta[e];
        CHECK(w.last_granule == total * 48000 / m.timescale,
              "cap %zu: last granule %llu, table says %llu", caps[c],
              (unsigned long long)w.last_granule,
              (unsigned long long)(total * 48000 / m.timescale));
        free(w.payload);
        free(s);
    }

    printf("  a seek starts a new stream\n");
    {
        const uint32_t cs = mp4_seek_cs(&m, 30);
        CHECK(cs == 3000, "landed at %u cs", cs);
        const uint32_t first = m.cur;
        size_t len;
        uint8_t *s = drain(f, &m, 8192, &len);
        walk_t w;
        walk(s, len, &w);
        CHECK(!w.bad_crc && !w.bad_seq, "after a seek: framing broken");
        CHECK(w.bos_first && w.head_ok && w.tags_ok, "after a seek: no headers");
        CHECK(w.packets == m.count - first, "after a seek: %u packets", w.packets);
        /* The first audio page's granule is the end of sample `first`. */
        size_t skip = 0;
        for (uint32_t i = 0; i < first; i++) skip += m.size[i];
        CHECK(w.payload_len == want_len - skip &&
              memcmp(w.payload, want + skip, w.payload_len) == 0,
              "after a seek: the wrong packets");
        free(w.payload);
        free(s);
    }

    free(want);
    mp4_free(&m);
    fclose(f);

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
