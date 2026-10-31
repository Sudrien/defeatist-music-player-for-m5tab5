/*
 * screendim.h -- dim the screen after a while, with nothing to run.
 *
 * NOT ABOUT BURN-IN. This panel is an LCD -- brightness.h drives a PWM
 * backlight with a measured 1% floor, and a backlight is a thing OLEDs
 * do not have -- so a static picture ages every pixel through the same
 * lamp and leaves no pattern behind. What a static picture does cost is
 * backlight hours, which are uniform and finite, and battery. So this
 * dims and never moves anything: no pixel shifting, no screensaver, no
 * inverted UI. Those are emissive-display mitigations and would only
 * make the player worse here.
 *
 * Shaped after sleeptimer.h and for the same reasons: fixed steps chosen
 * on the Sleep page, arithmetic only, on the monotonic clock so an NTP
 * step cannot move the deadline, and header-only and pure so texttest
 * compiles this file.
 *
 * DIFFERENT FROM THE SLEEP TIMER IN TWO WAYS, both deliberate. It is
 * saved, because "dim after 30 s" is a preference where "sleep in 45
 * minutes" is a decision about tonight. And it is reversible: any touch
 * puts the brightness back, where the sleep timer's end is an end.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "i18n.h"   /* 6016: N_() on the labels */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The intervals, in seconds, and 0 for never.
 *
 * Steps rather than a slider's range because there are five of them and
 * a five-position slider is a row of choices pretending to be
 * continuous. Step 0 is never, so a saved 0 is off and the table is
 * indexed from 1 -- the sleep timer's convention, where step 0 is also
 * off.
 */
#define SCREENDIM_STEPS         (4)         /* 1..4; 0 is never */
#define SCREENDIM_DEFAULT_STEP  (2)         /* 30 s */

/*
 * Dim to half the brightness the listener chose.
 *
 * HALF THE SLIDER, NOT HALF THE LIGHT. brightness.h maps a slider
 * position through a gamma curve to a backlight duty and, below the
 * backlight's floor, to a filter over the picture -- so 50 on the slider
 * is a good deal less than half the photons of 100. That is the intended
 * reading of "dim to 50%": half as far up the control the listener
 * actually touched, which is what they will compare it against. A true
 * half-luminance dim would be a smaller visible change and a worse
 * saving.
 */
#define SCREENDIM_LEVEL_PCT     (50)

/*
 * And the screen off, which is a longer wait than the dim and its own
 * setting rather than a multiple of it -- a hidden multiplier is a rule
 * nobody can see or change.
 *
 * OFF HERE MEANS THE BACKLIGHT, NOT THE PANEL. The Sleep page's manual
 * "Screen off" already works this way and has to: ledc goes to zero
 * duty, the ST7121 keeps running and the ST7123 touch controller is
 * never told anything, which is why a tap can wake it (ui.c turns any
 * tap into UI_ACTION_SCREEN_ON while the screen is off, and deliberately
 * does not press what is under the finger). A panel or touch power-down
 * would strand the listener with no way back, and this reuses the path
 * that was already safe rather than inventing one that is not.
 */
#define SCREENOFF_STEPS         (4)         /* 1..4; 0 is never */
#define SCREENOFF_DEFAULT_STEP  (0)         /* never, until asked for */

static inline int screenoff_seconds(int step)
{
    static const int secs[SCREENOFF_STEPS] = { 30, 60, 120, 300 };
    if (step <= 0 || step > SCREENOFF_STEPS) return 0;      /* never */
    return secs[step - 1];
}

static inline int screenoff_step_for_seconds(int seconds)
{
    for (int s = 1; s <= SCREENOFF_STEPS; s++) {
        if (screenoff_seconds(s) == seconds) return s;
    }
    return 0;
}

static inline const char *screenoff_label(int step)
{
    switch (step) {
    case 1:  return N_("30 s");
    case 2:  return N_("1 min");
    case 3:  return N_("2 min");
    case 4:  return N_("5 min");
    default: return N_("Never");
    }
}

/*
 * The off wait, never shorter than the dim's.
 *
 * The two are set independently, so they can be set to contradict each
 * other -- off at 30 s with dim at 2 min means the screen goes black
 * without ever dimming, and the dim setting silently does nothing. The
 * clamp makes the later of the two win, so "off" is always something
 * that happens after a dim rather than instead of it, and neither row
 * has to police the other.
 *
 * Both are seconds, 0 for never. Off never applies when off is never,
 * whatever the dim says.
 */
static inline int screenoff_effective(int off_seconds, int dim_seconds)
{
    if (off_seconds <= 0) return 0;
    return (off_seconds < dim_seconds) ? dim_seconds : off_seconds;
}

static inline int screendim_seconds(int step)
{
    static const int secs[SCREENDIM_STEPS] = { 15, 30, 60, 120 };
    if (step <= 0 || step > SCREENDIM_STEPS) return 0;      /* never */
    return secs[step - 1];
}

/* The step a saved second-count means, or 0 (never) for anything that is
 * not one of the intervals -- a value from another build's table is not
 * silently rounded into this one's. */
static inline int screendim_step_for_seconds(int seconds)
{
    for (int s = 1; s <= SCREENDIM_STEPS; s++) {
        if (screendim_seconds(s) == seconds) return s;
    }
    return 0;
}

/* "15 s", "2 min", "Never" -- what the row says. */
static inline const char *screendim_label(int step)
{
    switch (step) {
    case 1:  return N_("15 s");
    case 2:  return N_("30 s");
    case 3:  return N_("1 min");
    case 4:  return N_("2 min");
    default: return N_("Never");
    }
}

/*
 * Is the screen due to dim?
 *
 * `last_us` is when the screen was last touched or woken, on the
 * monotonic clock. 0 seconds is never, and a `last_us` of 0 means
 * nothing has happened yet and the clock is not to be trusted -- both
 * answer false, because a screen that dims because a counter started at
 * zero dims a second after boot.
 */
static inline bool screendim_due(int64_t now_us, int64_t last_us, int seconds)
{
    if (seconds <= 0 || last_us <= 0) return false;
    if (now_us <= last_us) return false;            /* no time has passed */
    return (now_us - last_us) >= (int64_t)seconds * 1000000;
}

/*
 * The dimmed brightness for a chosen one, never below `min_pct`.
 *
 * The floor is the caller's (SETTINGS_BRIGHTNESS_MIN) rather than a
 * constant here, so this file keeps its only dependency on stdint and
 * the setting's range stays in one place. Dimming below the minimum the
 * slider itself allows would put the screen somewhere the listener
 * cannot choose and cannot see.
 */
static inline int screendim_level(int pct, int min_pct)
{
    int dim = pct * SCREENDIM_LEVEL_PCT / 100;
    if (dim < min_pct) dim = min_pct;
    if (dim > pct) dim = pct;           /* a min above the setting */
    return dim;
}

#ifdef __cplusplus
}
#endif
