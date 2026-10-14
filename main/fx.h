/*
 * fx.h -- the Fx sheet (6097): tone and the edges of the sound.
 *
 * Six numbers, two kinds.
 *
 * TONE: BASS and TREBLE, each a gain on its band as a fraction --
 * ½, ⅔, 1, 3/2, 2 -- read the way the speed fraction is read: 1 is the
 * track as decoded, bit for bit, and anything else changes it. A
 * multiplied fraction rather than an added one so the steps are even in
 * level: ½ and 2 are -6 and +6 dB, ⅔ and 3/2 are -3.5 and +3.5.
 *
 * Built the way an FM patch stacks carriers side by side: the track AND
 * a low-passed copy of it AND a high-passed one,
 *
 *     y = x + (gb - 1) * low(x) + (gt - 1) * high(x)
 *
 * with low() a one-pole at FX_BASS_HZ and high() x minus a one-pole at
 * FX_TREBLE_HZ. At DC the gain is exactly gb; towards Nyquist it tends
 * to gt; between the two corners it is 1. First order on purpose: a
 * shelf with no resonance cannot ring, and a one-pole has no
 * coefficient that goes wrong at 8 kHz or at 192.
 *
 * HEADROOM, NOT A LIMITER. Aurora Player 1.3.9's Bass+ boosts and then
 * catches the overs with a zero-lookahead limiter, which pumps. Here
 * the whole result is divided by the largest gain chosen, so a boost
 * comes out of the rest of the mix rather than out of the ceiling, and
 * nothing has to be caught. The clamp at the end is for a first-order
 * filter's phase putting a peak a few per cent over, not for the boost.
 *
 * EDGES: ATTACK, DECAY and RELEASE, in milliseconds -- the fade up when
 * play is pressed or a pause ends, the fade up after a seek, and the
 * fade down when pause is pressed. A synth's names for a player's
 * gestures: what they share is that each is how long the sound takes to
 * arrive or leave. SUSTAIN, the fourth, is the crossfade length, and
 * lives where it always has (settings_crossfade_sec()).
 *
 * Neither half is applied on the decode side. Both are the writer's,
 * at the moment the samples are handed to the hardware, after
 * ReplayGain (which is measured on the plain track and folded in by the
 * decode loop) and before audio_out_write().
 *
 * 16-bit stereo interleaved, the only thing the rings hold. Pure C, no
 * FreeRTOS: texttest's fxtest links it as it is.
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

#define FX_STEPS        5               /* every row has five steps */
#define FX_TONE_FLAT    2               /* index of ×1 */
#define FX_BASS_HZ      (150.0f)
#define FX_TREBLE_HZ    (3500.0f)

/* "½" "⅔" "1" U+E000 (3/2, 6100) "2", and the gains they stand for. */
extern const char *const fx_tone_label[FX_STEPS];
extern const float       fx_tone_gain[FX_STEPS];

/* 0 10 50 250 1000 ms. */
extern const uint16_t    fx_edge_ms[FX_STEPS];

/*
 * 6101: VOX, Audacity's Vocal Reduction, as a gain on the centre --
 * what left and right have in common, where a lead vocal sits -- over
 * FX_VOX_LO_HZ..FX_VOX_HI_HZ only, so the bass, kick and the top of
 * the cymbals (centred too) are left alone:
 *
 *     c = band((L + R) / 2);   L' = L + (v - 1) c;   R' = R + (v - 1) c
 *
 * 0 is the centre gone (karaoke), ½ is the voice 6 dB back, 1 is the
 * track bit for bit, 3/2 and 2 bring it forward with the same headroom
 * rule as tone -- divided by v. On a mono track left and right are all
 * centre, so 0 thins the voice band out of everything rather than
 * finding a voice in it.
 */
#define FX_VOX_FLAT     2
#define FX_VOX_LO_HZ    (150.0f)
#define FX_VOX_HI_HZ    (8000.0f)
extern const char *const fx_vox_label[FX_STEPS];     /* "0" "½" "1" U+E000 "2" */
extern const float       fx_vox_gain[FX_STEPS];

typedef struct {
    float    lo, hi;            /* the centre's two one-poles */
    float    alo, ahi;
    float    gv;                /* the gain the last chunk ended on */
    uint32_t rate;
} fx_vox_t;

void fx_vox_reset(fx_vox_t *v);

/* As fx_tone_run(): filters always listening, samples written only off
 * 1, a change ramped across the chunk. */
void fx_vox_run(fx_vox_t *v, int16_t *pcm, size_t frames,
                unsigned idx, uint32_t rate);

/*
 * 6102: ECHO and TIME, Audacity's Echo -- repeats that fade:
 *
 *     y[n] = x[n] + g y[n - d]          out = (1 - g) y
 *
 * g is ECHO, the level of each repeat against the one before: 0 is off
 * and bit for bit, ⅛ a single faint repeat, ⅞ a long trail. Below 1
 * only -- at 1 the repeats never die, above it they grow. The (1 - g)
 * is the headroom: 1/(1 - g) is the most the repeats can sum to, so a
 * long trail costs level (⅞ is -18 dB) and never clips.
 *
 * d is TIME, fx_echo_ms[]. The line is float, stereo, sized for one
 * second at FX_ECHO_MAX_RATE by the caller -- 1.5 MB, PSRAM -- and
 * cleared when ECHO comes on from 0, so a repeat is never of audio
 * from before it was switched on.
 */
#define FX_ECHO_STEPS    8
#define FX_ECHO_MAX_RATE 192000u
#define FX_ECHO_FRAMES   (FX_ECHO_MAX_RATE)   /* one second at the top rate */
extern const char *const fx_echo_label[FX_ECHO_STEPS]; /* 0 ⅛ ¼ ⅓ ½ ⅔ ¾ ⅞ */
extern const float       fx_echo_gain[FX_ECHO_STEPS];
extern const uint16_t    fx_echo_ms[FX_STEPS];          /* 60 125 250 500 1000 */

typedef struct {
    float   *line;              /* FX_ECHO_FRAMES stereo frames, or NULL */
    uint32_t pos;
    float    g;                 /* the gain the last chunk ended on */
} fx_echo_t;

/* `line` is the caller's, FX_ECHO_FRAMES * 2 floats; NULL leaves echo
 * off for good (there was no memory for it). */
void fx_echo_init(fx_echo_t *e, float *line);
void fx_echo_run(fx_echo_t *e, int16_t *pcm, size_t frames,
                 unsigned echo, unsigned time, uint32_t rate);

/* ------------------------------------------------------------------ */

typedef struct {
    float    lb[2], lt[2];      /* the two one-poles' state, per channel */
    float    ab, at;            /* their coefficients at `rate` */
    float    gb, gt;            /* the gains the last chunk ended on */
    uint32_t rate;
} fx_tone_t;

void fx_tone_reset(fx_tone_t *t);

/*
 * Run a chunk at the gains for indexes `bass` and `treble`. The filters
 * always run, so turning tone on mid-track starts from a filter that
 * has been listening rather than from zero; the samples are only
 * written when either gain is, or is ramping away from, 1. So a track
 * at 1/1 is untouched, bit for bit.
 *
 * A change of gain is a ramp across the chunk, not a step.
 */
void fx_tone_run(fx_tone_t *t, int16_t *pcm, size_t frames,
                 unsigned bass, unsigned treble, uint32_t rate);

/* True once the gains have reached 1/1 again. */
bool fx_tone_flat(const fx_tone_t *t);

/* ------------------------------------------------------------------ */

/*
 * One gain, moving towards 0 or 1 at a set slope. The edges share it:
 * a resume in the middle of a release turns round from wherever it got
 * to, rather than starting the attack from silence.
 */
#define FX_EDGE_ONE     (1u << 30)      /* the gain's 1: Q30, so a 1 s ramp
                                         * at 192 kHz still moves every frame
                                         * and lands within a frame of its
                                         * length */
typedef struct {
    uint32_t g;                 /* 0 .. FX_EDGE_ONE */
    bool     up;                /* the target: 1 if true, 0 if not */
    uint32_t step;              /* per frame; 0 is a jump */
} fx_edge_t;

void fx_edge_reset(fx_edge_t *e);              /* at 1, still */

/* Head for 1 (up) or 0 over `ms` at `rate`; 0 ms jumps. */
void fx_edge_go(fx_edge_t *e, bool up, uint32_t ms, uint32_t rate);

/* Silence now, then up over `ms`: a start or a seek. */
void fx_edge_from_zero(fx_edge_t *e, uint32_t ms, uint32_t rate);

/* Apply to a chunk. At 1 and still, the chunk is untouched. */
void fx_edge_run(fx_edge_t *e, int16_t *pcm, size_t frames);

static inline bool fx_edge_silent(const fx_edge_t *e) { return !e->up && e->g == 0; }
static inline bool fx_edge_unity(const fx_edge_t *e)  { return e->up && e->g == FX_EDGE_ONE; }

#ifdef __cplusplus
}
#endif
