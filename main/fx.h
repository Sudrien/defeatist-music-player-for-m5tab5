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

/*
 * 6103: LCUT and HCUT, Audacity's High-Pass and Low-Pass Filter -- cuts,
 * where BASS and TREB are shelves: past the corner the sound falls away
 * at 12 dB an octave (second-order Butterworth, RBJ's cookbook) instead
 * of being tilted. Index 0 is off, bit for bit, on both.
 *
 *   LCUT (a high-pass): off, 40, 80, 150, 300 Hz -- rumble, boom.
 *   HCUT (a low-pass):  off, 12k, 8k, 4k, 3k     -- hiss, harshness.
 *
 * Both at once is a band: 150 Hz - 8 kHz an AM radio, 300 Hz - 3 kHz a
 * telephone. A cut takes nothing away from the ceiling, so no headroom
 * is spent. A change of corner crossfades old filter into new across
 * one chunk, so neither turning one on nor moving it clicks.
 */
extern const uint16_t fx_lcut_hz[FX_STEPS];     /* 0 = off */
extern const uint16_t fx_hcut_hz[FX_STEPS];     /* 0 = off */

typedef struct {
    float b0, b1, b2, a1, a2;
} fx_bq_t;

typedef struct {
    fx_bq_t  k;                 /* the coefficients in use */
    float    z[2][2];           /* direct form II transposed, per channel */
    unsigned idx;               /* the index k was made for */
    uint32_t rate;
} fx_cut_t;

void fx_cut_reset(fx_cut_t *c);

/* `hp` true for LCUT, false for HCUT; `idx` into its table. */
void fx_cut_run(fx_cut_t *c, bool hp, int16_t *pcm, size_t frames,
                unsigned idx, uint32_t rate);

/*
 * 6104: DUST, a worn record's surface -- crackle on top of the track:
 * short ticks at random moments and a little hiss under them. Nothing
 * of the track is changed but its level.
 *
 * Index 0 is off, bit for bit. 1..4 are ticks a second on average,
 * fx_dust_cps[] -- 1 a clean pressing, 40 a 78 found in a loft -- and
 * the hiss rises with them. Each tick is noise under a decay of a
 * fraction of a millisecond, its own length, level and place between
 * the speakers; most are faint, a few are not (the level is a uniform
 * number squared). Lengths are in time, not samples, so a tick sounds
 * the same at 8 kHz as at 192.
 *
 * Headroom as everywhere on the sheet: the track is turned down by the
 * most the dust can add, FX_DUST_ROOM[] of full scale, so a tick on a
 * full-scale peak does not clip. A change of step ramps hiss and
 * headroom across the chunk; a tick already sounding finishes.
 *
 * The noise is a xorshift32 seeded by the caller -- per boot is
 * enough, the sequence never repeats within a listening.
 */
extern const uint16_t fx_dust_cps[FX_STEPS];        /* 0 1 4 12 40 */

typedef struct {
    uint32_t rng;
    float    env, k;            /* the tick sounding: level, and its decay per frame */
    float    pl, pr;            /* where it sits */
    float    hs[2];             /* the hiss's one-pole, per channel */
    float    ah;                /* its coefficient at `rate` */
    float    hl, room;          /* hiss level and headroom the last chunk ended on */
    uint32_t rate;
} fx_dust_t;

void fx_dust_reset(fx_dust_t *d, uint32_t seed);
void fx_dust_run(fx_dust_t *d, int16_t *pcm, size_t frames,
                 unsigned idx, uint32_t rate);

/*
 * 6105: WIDE, the stereo width -- a gain on what left and right do NOT
 * share:
 *
 *     M = (L + R) / 2    S = (L - R) / 2    L' = M + w S    R' = M - w S
 *
 * 0 is mono, 1 the track bit for bit, 2 doubles the difference. The
 * centre is never touched, so the mono sum L' + R' is the track's at
 * every step. Above 1 a hard-panned sound gets an upside-down copy on
 * the other side -- which is what puts it outside the speakers -- and
 * the louder side reaches (1 + w) / 2 of what it was, so the headroom
 * rule divides by that.
 */
#define FX_WIDE_FLAT    2
extern const float fx_wide_gain[FX_STEPS];            /* 0 ½ 1 3/2 2; labels fx_vox_label */

typedef struct {
    float g;                    /* the width the last chunk ended on */
} fx_wide_t;

void fx_wide_reset(fx_wide_t *w);
void fx_wide_run(fx_wide_t *w, int16_t *pcm, size_t frames,
                 unsigned idx, uint32_t rate);

/*
 * 6105: WOW and FLTR, a turntable that does not turn quite evenly.
 * Both are a delay that moves, read with linear interpolation; a delay
 * shrinking is the pitch going up, growing is it going down.
 *
 *   WOW   once a revolution -- an off-centre hole, a warped disc. Its
 *         rate is the speed selector's PLAYED: 0.28 Hz at 16⅔, 0.56 at
 *         33⅓, 0.75 at 45, 1.3 at 78.
 *   FLTR  FX_FLTR_PER_REV times a revolution -- a motor's cogging, an
 *         idler wheel -- so 10 Hz at 33⅓, 23 at 78.
 *
 * The steps are the peak pitch deviation, in per cent:
 *   WOW   off, 0.1, 0.25, 0.5, 1      FLTR  off, 0.05, 0.1, 0.2, 0.5
 * A deviation d at rate f is a sine of amplitude d / (2 pi f) seconds,
 * so the same step is a deeper delay at a slower speed -- 5.7 ms for
 * WOW 1 at 16⅔. The delay is A (1 - cos), never below zero: at A = 0
 * the output is the input, bit for bit, and off is off.
 *
 * A moves at most FX_WOW_SLEW seconds a second, so a change of step or
 * of speed glides rather than jumping the pitch: the glide itself
 * bends pitch by no more than 2 * FX_WOW_SLEW (0.2 %), and WOW 1 at
 * 16⅔ takes about 6 s to reach full depth. The line is int16 stereo,
 * FX_WOW_FRAMES -- 21 ms at 192 kHz, the deepest setting needs 12.
 */
#define FX_WOW_FRAMES   4096
#define FX_FLTR_PER_REV 18
#define FX_WOW_SLEW     (0.001f)
extern const char *const fx_wow_label[FX_STEPS];     /* "off" is the UI's */
extern const float       fx_wow_dev[FX_STEPS];       /* fractions: 0 .001 ... */
extern const char *const fx_fltr_label[FX_STEPS];
extern const float       fx_fltr_dev[FX_STEPS];

typedef struct {
    int16_t  line[FX_WOW_FRAMES][2];
    uint32_t pos;
    float    aw, af;            /* the two amplitudes now, seconds */
    float    cw, sw, cf, sf;    /* the two phases, as unit phasors */
    bool     live;              /* the line holds this run's audio */
} fx_wow_t;

void fx_wow_reset(fx_wow_t *w);

/* `rpm_thirds` is varispeed_rpm_thirds[PLAYED]: 100 for 33⅓. */
void fx_wow_run(fx_wow_t *w, int16_t *pcm, size_t frames,
                unsigned wow, unsigned fltr, unsigned rpm_thirds,
                uint32_t rate);

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
