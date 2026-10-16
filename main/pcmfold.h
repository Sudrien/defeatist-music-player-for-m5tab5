/*
 * pcmfold.h -- 24-bit PCM folded to the 16 bits everything downstream is
 * built from.
 *
 * WHY THIS IS A HEADER AND NOT A STATIC IN decoder.c
 *
 * It was a static in decoder.c, which was right while the file path was
 * the only path that could meet a 24-bit sample. It is not any more:
 * LapFox Radio is 24-bit Ogg FLAC, netdec.c refused it, and the fold
 * that would have played it was forty lines away behind a `static`.
 *
 * Copying it into netdec.c was the other option and is the worse one.
 * The arithmetic below is the kind that is wrong quietly -- a sign
 * extension that depends on the signedness of a right shift, a rounding
 * step that biases every sample toward zero if it is dropped, a clamp
 * that only fires within 128 of full scale -- and two copies means one
 * of them gets fixed. Header-only and inline, so both paths compile the
 * same instructions, and host-tested in texttest/pcmfoldtest.c, which a
 * static in a file full of ESP-IDF includes could never be.
 *
 * WHERE THE CALL BELONGS, in either path: the first moment the samples
 * exist and the last moment before they belong to the rest of the
 * player. The ring is int16, the ReplayGain scale is int16, the
 * crossfade, the envelope, the loudness gate and the volume are all
 * int16, and the I2S slot config is 16-bit. Converting there means none
 * of them learn the source was ever anything else.
 *
 * 32 IS NOT A WIDER 24, AND THE DECODER CANNOT SAY WHICH 32 IT IS.
 * esp_audio_simple_dec_info_t reports a bit count and nothing about how
 * to read it, so a 32-bit stream is integer or IEEE float and the struct
 * cannot say which. Folding float samples as integers is full-scale
 * noise into headphones, and guessing has no safe side. So there are two
 * 32-bit folds below and nothing here chooses between them: the caller
 * has to know from somewhere other than the decoder. Since 6150 the file
 * path does, for WAV -- cbr_probe() reads the format tag (and the
 * extensible subformat) out of `fmt `, which is the one place the file
 * says. The stream path has no such place and still refuses 32. See test
 * files 19 and 20: the same audio in both forms, indistinguishable at
 * the decoder and distinguished by the header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PCMFOLD_H
#define PCMFOLD_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fold `bytes` of little-endian 24-bit PCM in `buf` to int16, in place.
 * Returns the number of int16 samples written.
 *
 * IN PLACE, FORWARD, three bytes read for every two written, so the
 * destination trails the source by a growing margin and never catches
 * it. The count returned shrinks accordingly -- bytes / 3 rather than
 * bytes / 2 -- and is smaller than a buffer sized for int16 at the same
 * byte count, so there is nothing to overflow. A caller that hands over
 * a buffer of N int16 can be handed back at most (N * 2) / 3 samples.
 *
 * ROUNDED, NOT TRUNCATED. Dropping the low byte biases every sample
 * toward zero, which is a DC-ish error that costs exactly as much
 * arithmetic as rounding does. Half an LSB and a clamp: the clamp only
 * fires on a sample within 128 of full scale, where rounding up would
 * wrap the sign, and that is the one input that makes truncation look
 * correct by accident.
 *
 * No dither. The noise it would replace sits at -96 dBFS, under a
 * headphone amp on a tablet, and it would cost two RNG calls a sample
 * at 2.6 million samples a minute to hide something nothing here can
 * resolve.
 *
 * A byte count that is not a multiple of three leaves the remainder
 * unread rather than reading past it: a partial sample is the decoder
 * having produced something this cannot interpret, and a third of a
 * sample is not worth a special case.
 */
static inline int pcmfold_24_to_16(int16_t *buf, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    int16_t *dst = buf;
    const uint32_t n = bytes / 3;

    for (uint32_t i = 0; i < n; i++) {
        /*
         * Little-endian, sign in the top byte.
         *
         * ASSEMBLED UNSIGNED, THEN SIGN-EXTENDED BY SUBTRACTION. The
         * obvious version -- casting the top byte to int8_t and
         * shifting it left by 16 -- is undefined behaviour whenever the
         * sample is negative, because C says nothing about left-shifting
         * a negative value. It shipped that way from 0803 and worked on
         * every compiler it met, which is how that class of bug spends
         * its time; UBSan in this file's own test is what finally said
         * so. Nothing below depends on the signedness of a right shift
         * either, for the same reason.
         */
        const uint32_t raw = ((uint32_t)src[2] << 16) |
                             ((uint32_t)src[1] << 8) | (uint32_t)src[0];
        int32_t s = (int32_t)raw;
        if (s & 0x800000) s -= 0x1000000;
        src += 3;

        s = (s + 128) >> 8;
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        *dst++ = (int16_t)s;
    }
    return (int)n;
}

/*
 * Fold `bytes` of little-endian signed 32-bit integer PCM to int16, in
 * place. Returns the number of int16 samples written.
 *
 * Four bytes read for every two written: the same in-place forward walk
 * as the 24-bit fold, with more margin. Rounded half up and clamped,
 * exactly as the 24-bit fold rounds, so a 32-bit file that is a 24-bit
 * file shifted left by 8 folds to the same samples.
 *
 * NO SIGNED SHIFTS. The sample is moved to offset binary by flipping the
 * sign bit -- 0x80000000 is silence, 0 and 0xFFFFFFFF the extremes --
 * rounded and shifted as an unsigned 64-bit value, and moved back by a
 * subtraction. Nothing is left-shifted while negative and nothing
 * depends on the signedness of a right shift, for the reason the 24-bit
 * fold gives.
 */
static inline int pcmfold_s32_to_16(int16_t *buf, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    int16_t *dst = buf;
    const uint32_t n = bytes / 4;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t raw = ((uint32_t)src[3] << 24) |
                             ((uint32_t)src[2] << 16) |
                             ((uint32_t)src[1] << 8) | (uint32_t)src[0];
        src += 4;
        uint64_t q = (((uint64_t)(raw ^ 0x80000000u)) + 0x8000u) >> 16;
        if (q > 0xFFFFu) q = 0xFFFFu;           /* rounding up off the top */
        *dst++ = (int16_t)((int32_t)q - 32768);
    }
    return (int)n;
}

/*
 * Fold `bytes` of little-endian IEEE 754 single-precision PCM to int16,
 * in place. Returns the number of int16 samples written.
 *
 * Full scale is +-1.0 and is mapped to +-32768, then clamped: the
 * convention every float-to-integer converter worth matching uses, and
 * the one that makes 32767/32768 the top rather than wrapping. Float
 * PCM is allowed to exceed 1.0 -- that is half the point of it -- so the
 * clamp is not a corner case here but the ordinary handling of a loud
 * master.
 *
 * CLAMPED AS A FLOAT, BEFORE THE CONVERSION. Converting a float outside
 * the range of int32 is undefined behaviour in C, and a sample of 1e30
 * is a legal float. NaN compares false with everything, so it is caught
 * by name and becomes silence: there is no right answer for it, and
 * silence is the one that cannot hurt.
 *
 * Read through memcpy, which the compiler turns into a load: the buffer
 * is int16 and reading it as float through a pointer cast is an
 * aliasing violation, the kind that works until an optimiser notices.
 */
static inline int pcmfold_f32_to_16(int16_t *buf, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    int16_t *dst = buf;
    const uint32_t n = bytes / 4;

    for (uint32_t i = 0; i < n; i++) {
        float x;
        memcpy(&x, src, sizeof(x));         /* little-endian host and file */
        src += 4;
        float s = x * 32768.0f;
        if (s != s) s = 0.0f;               /* NaN */
        else if (s > 32767.0f) s = 32767.0f;
        else if (s < -32768.0f) s = -32768.0f;
        /* Half away from zero; the clamp above leaves room for it, since
         * 32767.5 and -32768.5 both truncate back inside. */
        *dst++ = (int16_t)(int32_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
    }
    return (int)n;
}

#ifdef __cplusplus
}
#endif

#endif /* PCMFOLD_H */
