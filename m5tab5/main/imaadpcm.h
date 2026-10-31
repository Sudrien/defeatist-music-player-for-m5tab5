/*
 * imaadpcm.h -- IMA ADPCM in WAV (format tag 0x0011), decoded here.
 *
 * WHY NOT esp_audio_codec
 *
 * It has an IMA ADPCM decoder, and decoder.c says why it is not used:
 * _ADPCM is one of the simple decoder's one-frame-per-call types, and
 * the sliding window cannot promise it a frame. A framing layer could,
 * but IMA ADPCM is the rare codec where the framing layer is most of
 * the work and the decoder is twenty lines -- a step table, an index
 * table and an add -- so this is both, in one header, host-tested in
 * texttest/imaadpcmtest.c against ffmpeg's decoder, which a library
 * archive could never be.
 *
 * And it is exactly seekable for free. Every block starts from a stated
 * sample and step index, so a block is independently decodable and
 * block N starts at sample N * samples_per_block: a seek is one
 * multiplication and a discard of the samples before the target.
 *
 * THE BLOCK, per Microsoft's layout (which is what "IMA ADPCM WAV"
 * means in practice -- Apple's IMA4 is a different container):
 *
 *   per channel, 4 bytes:  int16 first sample, uint8 step index, uint8 0
 *   then, repeating:       4 bytes (8 nibbles) of channel 0,
 *                          4 bytes of channel 1, ...
 *
 * low nibble first. The header sample is the block's first output
 * sample, so a block of B bytes holds (B / ch - 4) * 2 + 1 samples per
 * channel. The last block of a file may be short; it is decoded as far
 * as its whole 4-byte groups go.
 *
 * Mono and stereo only. The player is stereo, and a layout with more
 * channels is not something an ADPCM WAV turns out to be in practice.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef IMAADPCM_H
#define IMAADPCM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A block larger than this is refused rather than allocated: real
 * encoders write 256 to 2048 bytes a channel, and 36 KB would be 72 K
 * samples a block, which nothing writes. */
#define IMAWAV_MAX_BLOCK    (16 * 1024)

typedef struct {
    int64_t  data_start;    /* first byte of the first block */
    int64_t  data_len;      /* bytes of block data */
    uint32_t sample_rate;
    uint32_t byte_rate;     /* as stated, for the bitrate shown */
    uint16_t channels;      /* 1 or 2 */
    uint16_t block_align;   /* bytes per block, all channels */
    uint32_t spb;           /* samples per block, per channel */
    uint64_t total;         /* samples per channel: `fact`, or the blocks' */
} imawav_t;

static inline uint32_t imawav_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Samples per channel in a block of `bytes`, the short last one too. */
static inline uint32_t imawav_block_samples(uint32_t bytes, uint16_t ch)
{
    if (!ch || bytes < 4u * ch) return 0;
    const uint32_t groups = (bytes - 4u * ch) / (4u * ch);
    return 1 + groups * 8;
}

/*
 * Walk the RIFF chunks and fill `w`. True only for an IMA ADPCM WAV this
 * can play; false for every other WAV, which then goes the ordinary way.
 * The handle's position is not restored: the caller seeks to data_start.
 *
 * Chunks are walked rather than assumed adjacent, for the reason
 * cbrseek.c gives -- LIST/INFO lands between `fmt ` and `data`.
 */
static inline bool imawav_probe(FILE *f, imawav_t *w)
{
    memset(w, 0, sizeof(*w));
    uint8_t h[12];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12) return false;
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    uint64_t fact = 0;
    int64_t pos = 12;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t c[8];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(c, 1, 8, f) != 8) break;
        const uint32_t len = imawav_le32(c + 4);

        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t fm[20];
            if (len < 16) return false;
            const size_t want = len >= 20 ? 20 : 16;
            if (fread(fm, 1, want, f) != want) return false;
            if ((fm[0] | (fm[1] << 8)) != 0x0011) return false;   /* not IMA */
            w->channels = (uint16_t)(fm[2] | (fm[3] << 8));
            w->sample_rate = imawav_le32(fm + 4);
            w->byte_rate = imawav_le32(fm + 8);
            w->block_align = (uint16_t)(fm[12] | (fm[13] << 8));
            if ((fm[14] | (fm[15] << 8)) != 4) return false;      /* 3-bit IMA exists */
            have_fmt = true;
        } else if (memcmp(c, "fact", 4) == 0 && len >= 4) {
            uint8_t fc[4];
            if (fread(fc, 1, 4, f) == 4) fact = imawav_le32(fc);
        } else if (memcmp(c, "data", 4) == 0) {
            if (!have_fmt) return false;
            w->data_start = pos + 8;
            w->data_len = len;
            /* Streamed WAVs write 0 or 0xFFFFFFFF: "to the end". */
            if (fseek(f, 0, SEEK_END) == 0) {
                const long end = ftell(f);
                if (end > 0 && (len == 0 || len == 0xFFFFFFFFu ||
                                (int64_t)len > end - w->data_start)) {
                    w->data_len = end - w->data_start;
                }
            }
            break;
        }
        pos += 8 + (int64_t)len + (len & 1);
    }

    if (!have_fmt || !w->data_start || w->data_len <= 0) return false;
    if (w->channels < 1 || w->channels > 2 || !w->sample_rate) return false;
    if (w->block_align < 4u * w->channels + 4u * w->channels ||
        w->block_align > IMAWAV_MAX_BLOCK || w->block_align % (4u * w->channels)) {
        return false;
    }
    w->spb = imawav_block_samples(w->block_align, w->channels);

    const uint64_t blocks = (uint64_t)w->data_len / w->block_align;
    const uint32_t tail = (uint32_t)((uint64_t)w->data_len % w->block_align);
    w->total = blocks * w->spb + imawav_block_samples(tail, w->channels);
    /* `fact` is the exact count, without the last block's padding. It
     * only ever shortens: a fact larger than the blocks is a lie. */
    if (fact && fact < w->total) w->total = fact;
    return w->total > 0;
}

static const int16_t k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t k_ima_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

/*
 * One nibble.
 *
 * WHICH ROUNDING. The IMA recommendation sums truncated terms -- step>>3,
 * then step, step>>1, step>>2 by bit -- and ffmpeg computes the same
 * quantity in one go, ((2d + 1) * step) >> 3, which truncates once. They
 * differ by up to a few LSB a sample, and the difference carries to the
 * end of the block (on file 01 converted, at most 73 LSB, -53 dB). This
 * uses ffmpeg's: it is what VLC, mpv and every other ffmpeg-based player
 * produce for the same file, so "sounds like on the computer" is a
 * comparison this wins exactly, and imaadpcmtest checks it to the
 * sample. Either form resynchronises at every block header.
 */
static inline int16_t ima_nibble(int32_t *pred, int *idx, unsigned n)
{
    const int32_t step = k_ima_step[*idx];
    const int32_t diff = ((2 * (int32_t)(n & 7) + 1) * step) >> 3;
    int32_t p = (n & 8) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    else if (p < -32768) p = -32768;
    *pred = p;
    int i = *idx + k_ima_index[n & 15];
    *idx = i < 0 ? 0 : (i > 88 ? 88 : i);
    return (int16_t)p;
}

/*
 * Decode one block of `bytes` into `out`, interleaved. Returns samples
 * per channel written, or 0 if the block is unusable (a step index past
 * the table is a damaged block, not a quiet one). `out` must hold
 * imawav_block_samples(bytes, ch) * ch int16.
 */
static inline uint32_t ima_decode_block(const uint8_t *blk, uint32_t bytes,
                                        uint16_t ch, int16_t *out)
{
    const uint32_t n = imawav_block_samples(bytes, ch);
    if (!n) return 0;

    int32_t pred[2];
    int idx[2];
    for (unsigned c = 0; c < ch; c++) {
        const uint8_t *h = blk + 4 * c;
        pred[c] = (int16_t)(uint16_t)(h[0] | (h[1] << 8));
        idx[c] = h[2];
        if (idx[c] > 88) return 0;
        out[c] = (int16_t)pred[c];
    }

    const uint8_t *p = blk + 4u * ch;
    const uint32_t groups = (n - 1) / 8;
    for (uint32_t g = 0; g < groups; g++) {
        for (unsigned c = 0; c < ch; c++) {
            int16_t *o = out + (1 + g * 8) * ch + c;
            for (int b = 0; b < 4; b++) {
                const uint8_t v = *p++;
                o[0] = ima_nibble(&pred[c], &idx[c], v & 0x0F);
                o += ch;
                o[0] = ima_nibble(&pred[c], &idx[c], v >> 4);
                o += ch;
            }
        }
    }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* IMAADPCM_H */
