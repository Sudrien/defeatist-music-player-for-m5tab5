/*
 * varispeed.h -- turntable speed: play a track faster or slower, pitch
 * and all.
 *
 * The listener sets a fraction, the speed the record is PLAYED at over
 * the speed it was CUT at: 45/33⅓ is a 33 on a deck set to 45. Equal
 * numbers are ×1.00 whatever they are -- 33⅓/33⅓ and 45/45 are the same
 * thing -- and at ×1.00 this file is a copy, bit for bit, so the
 * samples reach the DAC exactly as decoded, as they always have (and a
 * track that never left ×1.00 does not come here at all).
 *
 * Anything else is a variable-rate read of the ring: the output clock
 * stays where the file put it, and the read position moves through the
 * track `step` frames per output frame, interpolated (4-point cubic)
 * between them. That is what a turntable does -- the whole spectrum
 * moves with the speed -- and it is NOT a resampler in rateconv.h's
 * sense: rateconv changes the rate and keeps the pitch; this keeps the
 * rate and changes the pitch.
 *
 * Pull model, for the writer. One call is given what the ring has (a
 * peek, not a take), fills up to `out_cap` frames, and reports how many
 * input frames it is finished with. The caller drops exactly that many
 * from the ring; the rest are offered again next call. So the ring keeps
 * counting TRACK frames, and every position, trim and waveform that is
 * measured from the ring stays in track time.
 *
 * Not band-limited when faster than ×1.00: content above the output's
 * Nyquist / step folds back. At 45/33⅓ that is what was above 16.3 kHz
 * of a 44.1 kHz file. Accepted for now and written down rather than
 * discovered.
 *
 * One state per ring. A crossfade runs two: the outgoing track's, whose
 * step slides back to ×1.00 (VARISPEED_UNITY), and the incoming's.
 *
 * 16-bit stereo interleaved, the only thing the PCM rings hold.
 * Small enough to live in a static; never on a task stack by habit.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A step is input frames per output frame, Q16.16. */
#define VARISPEED_UNITY  (1u << 16)

/*
 * Turntable speeds in thirds of an RPM, so 16⅔ and 33⅓ are integers.
 * The order is the order a drag walks them in.
 */
#define VARISPEED_RPMS   4
extern const uint16_t varispeed_rpm_thirds[VARISPEED_RPMS];  /* 50 100 135 234 */
extern const char *const varispeed_rpm_label[VARISPEED_RPMS]; /* "16⅔" "33⅓" "45" "78" */

/* played/cut as a step. Indexes out of range read as ×1.00. */
uint32_t varispeed_step(unsigned played, unsigned cut);

/*
 * The cubic reads four frames around the read point; win[1] is the
 * frame at it. Every frame offered is taken into this window or dropped
 * by a long step, so a call is never "not enough input" -- a ring that
 * wraps and offers one frame at a time still moves. The price is that
 * the window holds the last two frames of a track: at the end of a
 * track they are not played (45 µs at 44.1 kHz).
 */
typedef struct {
    int16_t  win[4][2];
    uint8_t  have;         /* frames in win */
    uint32_t adv;          /* frames the read point still has to move */
    uint32_t frac;         /* Q16 fraction past the read point */
    bool     active;       /* has run since the last reset */
} varispeed_t;

/* Forget everything: a new track, a seek. Starts from silence history. */
void varispeed_reset(varispeed_t *v);

/*
 * False until the first varispeed_run(). The writer copies a ring
 * straight through while this is false and the step is ×1.00, so a
 * track nobody sped up never passes through here at all. Once a track
 * has, it stays here to its end even back at ×1.00 -- still bit for
 * bit, two frames later -- so turning the speed off mid-track is not a
 * jump.
 */
static inline bool varispeed_active(const varispeed_t *v) { return v->active; }

/*
 * Fill up to `out_cap` frames of `out` from `in` (`in_frames` available)
 * at `step`. Returns frames written; *in_used is how many input frames
 * the caller may now drop, which is all of them unless `out` filled
 * first. A short input returns early -- fewer than out_cap frames --
 * rather than inventing samples.
 *
 * At VARISPEED_UNITY every output frame is an input frame, unchanged.
 */
size_t varispeed_run(varispeed_t *v, uint32_t step,
                     const int16_t *in, size_t in_frames, size_t *in_used,
                     int16_t *out, size_t out_cap);

/*
 * The step a slide has reached: `from` to `to` over `len` frames,
 * `pos` frames in. Linear; clamps at both ends. Used to bring the
 * outgoing track of a crossfade back to ×1.00.
 */
uint32_t varispeed_slide(uint32_t from, uint32_t to, uint32_t pos, uint32_t len);

#ifdef __cplusplus
}
#endif
