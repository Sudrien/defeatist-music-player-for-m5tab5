/*
 * aiff.h -- AIFF and AIFF-C, found and described (6154).
 *
 * The decode is pcmconv.h; this is the container. An IFF file is big-
 * endian chunks inside a FORM: `COMM` says the shape, `SSND` holds the
 * frames, and everything else (NAME, ANNO, ID3, MARK, COMT) is skipped.
 * Chunks are padded to an even length and walked, never assumed
 * adjacent, for the reason cbrseek.c gives about WAV.
 *
 * COMM:  channels (2), frames (4), bits per sample (2), the sample rate
 *        as an 80-bit extended float (10); in AIFF-C then a compression
 *        type (4) and a name.
 * SSND:  an offset (4) to the first frame, a block size (4) nobody
 *        uses, then the frames.
 *
 * AIFF-C compression types taken, all uncompressed: NONE and twos (big-
 * endian signed), sowt (little-endian, what macOS writes), in24/in32
 * (QuickTime's names for 24- and 32-bit big-endian), raw (8-bit offset
 * binary), fl32/fl64 in either case, and since 6155 ulaw/alaw (G.711,
 * expanded in pcmconv.h). Anything else compressed -- ima4, MAC3/6 --
 * is refused rather than played as noise.
 *
 * wav_g711_probe() below fills the same struct from a WAV holding G.711
 * (format tags 6 and 7), which esp_audio_codec's WAV parser does not
 * take: one layout, one reader, two containers.
 *
 * Mono and stereo only, for the reason imaadpcm.h gives.
 *
 * Reads go through storage_io_read_at(), so the probe takes its turn
 * with the card like every other read on the open path.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AIFF_H
#define AIFF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pcmconv.h"
#include "storage_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t      data_start;    /* first byte of the first frame */
    uint64_t     frames;        /* per channel, as COMM states, cut to SSND */
    uint32_t     sample_rate;
    uint16_t     channels;
    uint16_t     frame_bytes;   /* bytes per frame, all channels */
    pcm_layout_t layout;
    char         kind[5];       /* "AIFF" or the AIFF-C type, for the log */
} aiff_t;

static inline uint32_t aiff_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/*
 * The 80-bit extended rate: sign, 15-bit exponent biased by 16383, and
 * a 64-bit mantissa with its integer bit explicit. Every real rate is a
 * positive whole number well inside 32 bits, so this answers only that:
 * anything negative, fractional past rounding, or out of range is 0,
 * which the probe refuses.
 */
static inline uint32_t aiff_rate(const uint8_t *e)
{
    if (e[0] & 0x80) return 0;
    const int exp = ((e[0] & 0x7F) << 8 | e[1]) - 16383;
    uint64_t mant = 0;
    for (int i = 0; i < 8; i++) mant = (mant << 8) | e[2 + i];
    if (exp < 0 || exp > 31) return 0;
    const int shift = 63 - exp;
    uint64_t v = mant >> shift;
    if (shift > 0 && (mant >> (shift - 1)) & 1) v++;   /* round */
    return (uint32_t)v;
}

/* AIFF-C compression type to a layout. False for anything compressed. */
static inline bool aiff_layout(const uint8_t *t, uint16_t bits, pcm_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->big = true;
    l->bytes = (uint8_t)((bits + 7) / 8);
    if (!memcmp(t, "NONE", 4) || !memcmp(t, "twos", 4)) return true;
    if (!memcmp(t, "sowt", 4)) { l->big = false; return true; }
    if (!memcmp(t, "in24", 4)) { l->bytes = 3; return true; }
    if (!memcmp(t, "in32", 4)) { l->bytes = 4; return true; }
    if (!memcmp(t, "raw ", 4)) { l->bytes = 1; l->unsig = true; return true; }
    if (!memcmp(t, "fl32", 4) || !memcmp(t, "FL32", 4)) {
        l->fp = true; l->bytes = 4; return true;
    }
    if (!memcmp(t, "fl64", 4) || !memcmp(t, "FL64", 4)) {
        l->fp = true; l->bytes = 8; return true;
    }
    /* G.711 (6155): one byte a sample whatever COMM's width says --
     * Apple writes 16 there, the width it expands to. */
    if (!memcmp(t, "ulaw", 4) || !memcmp(t, "ULAW", 4)) {
        l->law = 'u'; l->bytes = 1; return true;
    }
    if (!memcmp(t, "alaw", 4) || !memcmp(t, "ALAW", 4)) {
        l->law = 'a'; l->bytes = 1; return true;
    }
    return false;
}

/*
 * True for an AIFF or AIFF-C this can play. `size` is the file's length,
 * which bounds the walk and the frame count. The handle's position is
 * not kept.
 */
static inline bool aiff_probe(FILE *f, int64_t size, aiff_t *a,
                              storage_io_class_t cls)
{
    memset(a, 0, sizeof(*a));
    uint8_t h[12];
    if (size < 12 || !storage_io_read_at(f, 0, h, 12, cls)) return false;
    if (memcmp(h, "FORM", 4) != 0) return false;
    const bool aifc = memcmp(h + 8, "AIFC", 4) == 0;
    if (!aifc && memcmp(h + 8, "AIFF", 4) != 0) return false;

    bool have_comm = false;
    uint16_t bits = 0;
    uint64_t frames = 0;
    int64_t pos = 12;
    for (int guard = 0; guard < 64 && pos + 8 <= size; guard++) {
        uint8_t c[8];
        if (!storage_io_read_at(f, pos, c, 8, cls)) return false;
        const uint32_t len = aiff_be32(c + 4);

        if (!memcmp(c, "COMM", 4)) {
            uint8_t m[22];
            if (len < 18) return false;
            const size_t want = aifc && len >= 22 ? 22 : 18;
            if (!storage_io_read_at(f, pos + 8, m, want, cls)) return false;
            a->channels = (uint16_t)(m[0] << 8 | m[1]);
            frames = aiff_be32(m + 2);
            bits = (uint16_t)(m[6] << 8 | m[7]);
            a->sample_rate = aiff_rate(m + 8);
            if (aifc) {
                if (want < 22) return false;
                if (!aiff_layout(m + 18, bits, &a->layout)) return false;
                memcpy(a->kind, m + 18, 4);
            } else {
                aiff_layout((const uint8_t *)"NONE", bits, &a->layout);
                memcpy(a->kind, "AIFF", 4);
            }
            have_comm = true;
        } else if (!memcmp(c, "SSND", 4)) {
            uint8_t s[8];
            if (!have_comm || len < 8) return false;
            if (!storage_io_read_at(f, pos + 8, s, 8, cls)) return false;
            a->data_start = pos + 16 + aiff_be32(s);
            int64_t avail = (int64_t)len - 8 - aiff_be32(s);
            /* A writer that never went back to fix the length leaves 0
             * or less than the file holds; the file is the bound. */
            if (avail <= 0 || a->data_start + avail > size) {
                avail = size - a->data_start;
            }
            if (!pcm_layout_ok(a->layout) || a->channels < 1 ||
                a->channels > 2 || !a->sample_rate || avail <= 0) {
                return false;
            }
            a->frame_bytes = (uint16_t)(a->layout.bytes * a->channels);
            const uint64_t in_file = (uint64_t)avail / a->frame_bytes;
            /* COMM is the count; SSND can only shorten it. A COMM of 0
             * from an unfinished writer means "what is there". */
            a->frames = (frames && frames < in_file) ? frames : in_file;
            return a->frames > 0;
        }
        pos += 8 + (int64_t)len + (len & 1);
    }
    return false;
}

/*
 * A WAV whose format tag is 6 (A-law) or 7 (mu-law), 8 bits, mono or
 * stereo, into an aiff_t (6155). False for every other WAV, which then
 * goes the way it always has. Chunks walked, as everywhere.
 */
static inline bool wav_g711_probe(FILE *f, int64_t size, aiff_t *a,
                                  storage_io_class_t cls)
{
    memset(a, 0, sizeof(*a));
    uint8_t h[12];
    if (size < 12 || !storage_io_read_at(f, 0, h, 12, cls)) return false;
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    int64_t pos = 12;
    for (int guard = 0; guard < 64 && pos + 8 <= size; guard++) {
        uint8_t c[8];
        if (!storage_io_read_at(f, pos, c, 8, cls)) return false;
        const uint32_t len = (uint32_t)c[4] | (uint32_t)c[5] << 8 |
                             (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t m[16];
            if (len < 16 || !storage_io_read_at(f, pos + 8, m, 16, cls)) return false;
            const unsigned tag = m[0] | m[1] << 8;
            if (tag != 6 && tag != 7) return false;
            if ((m[14] | m[15] << 8) != 8) return false;
            a->channels = (uint16_t)(m[2] | m[3] << 8);
            a->sample_rate = (uint32_t)m[4] | (uint32_t)m[5] << 8 |
                             (uint32_t)m[6] << 16 | (uint32_t)m[7] << 24;
            a->layout.bytes = 1;
            a->layout.law = tag == 7 ? 'u' : 'a';
            memcpy(a->kind, tag == 7 ? "ulaw" : "alaw", 4);
            have_fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt || a->channels < 1 || a->channels > 2 || !a->sample_rate) {
                return false;
            }
            a->data_start = pos + 8;
            int64_t avail = len;
            if (!len || len == 0xFFFFFFFFu || a->data_start + avail > size) {
                avail = size - a->data_start;
            }
            a->frame_bytes = a->channels;
            a->frames = avail > 0 ? (uint64_t)avail / a->frame_bytes : 0;
            return a->frames > 0;
        }
        pos += 8 + (int64_t)len + (len & 1);
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* AIFF_H */
