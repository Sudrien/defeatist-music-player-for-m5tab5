/*
 * micpcm.h -- microphone samples into the int32 frames the recorder
 * takes (5207).
 *
 * Three small conversions, each the kind that is wrong quietly: an
 * in-place unpack that overwrites a sample before reading it, a partial
 * frame dropped at a read boundary so a stereo file swaps sides from
 * there on, a sum that rounds one way for positive samples and the other
 * for negative. Header-only and inline for pcmfold.h's reason, and
 * host-tested in texttest/micpcmtest.c, which audio_out.c and uac.c,
 * full of ESP-IDF includes, could never be.
 *
 * Every output sample is an int32 holding a sign-extended value of the
 * source's width. flacenc takes that as it is.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MICPCM_H
#define MICPCM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One slot out of n TDM frames of `slots` little-endian int16 each, in
 * place: frames[i] = slot `slot` of raw frame i.
 *
 * IN PLACE, FORWARD. A raw frame is slots * 2 bytes and an output frame
 * is 4, so for slots >= 2 output i (bytes 4i..4i+3) never reaches a raw
 * frame not yet read (bytes slots*2*i onward, which is >= 4i + 4 for
 * every later frame). slots < 2 would overrun its own input and is
 * refused: 0 is returned. memcpy, not an int16_t pointer into the int32
 * array, which would break strict aliasing.
 */
static inline size_t micpcm_tdm_slot(int32_t *frames, size_t n, unsigned slots, unsigned slot)
{
    if (slots < 2 || slot >= slots) return 0;
    const uint8_t *raw = (const uint8_t *)frames;
    for (size_t i = 0; i < n; i++) {
        int16_t s;
        memcpy(&s, raw + (i * slots + slot) * sizeof(int16_t), sizeof(s));
        frames[i] = s;
    }
    return n;
}

/*
 * 5212: two slots out of n TDM frames of `slots` little-endian int16
 * each, as interleaved stereo, in place: frames[2i] = slot a of raw frame
 * i, frames[2i + 1] = slot b, each shifted left by `shift` (8 carries 16
 * bits to 24-bit scale).
 *
 * IN PLACE, FORWARD. Output frame i is 8 bytes at 8i; raw frame i is
 * slots * 2 bytes at slots * 2 * i. For slots >= 4 output i never
 * reaches past raw frame i, whose two samples are read before either is
 * written. slots < 4, a or b out of range, or a shift over 15 return 0.
 */
static inline size_t micpcm_tdm_pair(int32_t *frames, size_t n, unsigned slots,
                                     unsigned a, unsigned b, unsigned shift)
{
    if (slots < 4 || a >= slots || b >= slots || shift > 15) return 0;
    const uint8_t *raw = (const uint8_t *)frames;
    for (size_t i = 0; i < n; i++) {
        int16_t sa, sb;
        memcpy(&sa, raw + (i * slots + a) * sizeof(int16_t), sizeof(sa));
        memcpy(&sb, raw + (i * slots + b) * sizeof(int16_t), sizeof(sb));
        frames[2 * i]     = (int32_t)sa * (1 << shift);
        frames[2 * i + 1] = (int32_t)sb * (1 << shift);
    }
    return n;
}

/*
 * Interleaved little-endian int16 in `raw`: `carry` bytes left over from
 * the last call, then `got` new ones. Whole frames of `ch` channels are
 * widened into out; the bytes of a partial frame at the end are moved to
 * the front of raw and their count stored in *carry_out, for the next
 * call to put the rest after. Returns the frames written: out must hold
 * ((carry + got) / (ch * 2)) * ch int32. ch 0 returns 0 and keeps
 * nothing.
 */
static inline size_t micpcm_s16_take(uint8_t *raw, size_t carry, size_t got, unsigned ch,
                                     int32_t *out, size_t *carry_out)
{
    if (!ch) { *carry_out = 0; return 0; }
    const size_t fb = (size_t)ch * sizeof(int16_t);
    const size_t have = carry + got;
    const size_t n = have / fb;
    for (size_t i = 0; i < n * ch; i++) {
        int16_t s;
        memcpy(&s, raw + i * sizeof(int16_t), sizeof(s));
        out[i] = s;
    }
    const size_t rest = have - n * fb;
    if (rest) memmove(raw, raw + n * fb, rest);
    *carry_out = rest;
    return n;
}

/*
 * n stereo frames to n mono samples, in place: the mean of left and
 * right, rounded toward negative infinity the same way for every sign
 * (an arithmetic shift of the sum, which GCC and Clang both guarantee on
 * a signed int64). The sum is taken in int64 so no width a microphone
 * produces can overflow it. Output i is written after input 2i and 2i+1
 * are read, and 2i >= i, so forward is safe.
 */
static inline void micpcm_mono(int32_t *frames, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const int64_t sum = (int64_t)frames[2 * i] + (int64_t)frames[2 * i + 1];
        frames[i] = (int32_t)(sum >> 1);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MICPCM_H */
