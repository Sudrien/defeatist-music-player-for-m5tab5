/*
 * pcmconv.h -- uncompressed PCM in any of the layouts a container states,
 * to the int16 everything downstream is built from (6154).
 *
 * WAV's PCM goes through esp_audio_codec and pcmfold.h, because the WAV
 * parser hands back little-endian samples and says only how wide. The
 * containers this file is for -- AIFF and AIFF-C, and PCM inside an MP4
 * -- are read here instead, so the layout is known exactly from the
 * header: width, byte order, integer or float. There is nothing to
 * guess, which is the condition pcmfold.h sets for touching 32 bits.
 *
 * Every width rounds the way pcmfold.h rounds, by calling it: a sample
 * is left-justified into 32 bits and handed to pcmfold_round32(), and a
 * float to pcmfold_f32_one() or pcmfold_f64_one(). So a 24-bit AIFF and
 * a 24-bit WAV of the same audio fold to the same samples. 16-bit is
 * exact either way; 8-bit is shifted up, which is exact too.
 *
 * Not in place: the source is read into its own buffer, because a
 * 16-bit big-endian sample and its int16 are the same size and the
 * 8-bit one is smaller than its output, so the in-place argument the
 * folds rely on does not hold here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PCMCONV_H
#define PCMCONV_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pcmfold.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t bytes;          /* 1, 2, 3, 4, or 8 (8 only for float) */
    bool    big;            /* byte order */
    bool    fp;             /* IEEE float: bytes 4 or 8 */
    bool    unsig;          /* 8-bit offset binary ('raw ' in AIFF-C) */
    char    law;            /* 'u' or 'a': G.711, one byte a sample (6155) */
} pcm_layout_t;

static inline bool pcm_layout_ok(pcm_layout_t l)
{
    if (l.law) return (l.law == 'u' || l.law == 'a') && l.bytes == 1 && !l.fp;
    if (l.fp) return l.bytes == 4 || l.bytes == 8;
    if (l.unsig) return l.bytes == 1;
    return l.bytes >= 1 && l.bytes <= 4;
}

/*
 * G.711, the ITU's two 8-bit logarithmic codes (6155). These are the
 * reference expansions (Sun's g711.c, which every decoder since copies),
 * so the output is exactly what ffmpeg and sox produce: mu-law spans
 * +-32124 and A-law +-32256, already on the 16-bit scale. Arithmetic
 * rather than two 512-byte tables, for the flash.
 */
static inline int16_t pcm_ulaw(uint8_t b)
{
    const uint8_t u = (uint8_t)~b;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

static inline int16_t pcm_alaw(uint8_t b)
{
    const uint8_t a = (uint8_t)(b ^ 0x55);
    int t = (a & 0x0F) << 4;
    const int seg = (a & 0x70) >> 4;
    if (seg == 0) {
        t += 8;
    } else {
        t += 0x108;
        if (seg > 1) t <<= seg - 1;
    }
    return (int16_t)((a & 0x80) ? t : -t);
}

/* `n` samples (all channels counted) from `src` to `dst`. */
static inline void pcm_conv(const uint8_t *src, uint32_t n, pcm_layout_t l,
                            int16_t *dst)
{
    const unsigned w = l.bytes;
    if (l.law) {
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = l.law == 'u' ? pcm_ulaw(src[i]) : pcm_alaw(src[i]);
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++, src += w) {
        if (l.fp) {
            if (w == 4) {
                const uint32_t u = l.big
                    ? ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                      ((uint32_t)src[2] << 8) | src[3]
                    : ((uint32_t)src[3] << 24) | ((uint32_t)src[2] << 16) |
                      ((uint32_t)src[1] << 8) | src[0];
                float x;
                memcpy(&x, &u, sizeof(x));
                dst[i] = pcmfold_f32_one(x);
            } else {
                uint64_t u = 0;
                for (unsigned k = 0; k < 8; k++) {
                    u = (u << 8) | src[l.big ? k : 7 - k];
                }
                double x;
                memcpy(&x, &u, sizeof(x));
                dst[i] = pcmfold_f64_one(x);
            }
            continue;
        }
        /* Integer: most significant byte first into the top of 32 bits,
         * so every width arrives left-justified and signed alike. */
        uint32_t raw = 0;
        for (unsigned k = 0; k < w; k++) {
            raw |= (uint32_t)src[l.big ? k : w - 1 - k] << (24 - 8 * k);
        }
        if (l.unsig) raw ^= 0x80000000u;
        dst[i] = pcmfold_round32(raw);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PCMCONV_H */
