/*
 * powerdown.h -- power the device off after a while with nothing going on.
 *
 * 6000. Separate from the sleep timer, and the difference is the point:
 * the sleep timer is a decision about tonight ("stop the music in 45
 * minutes") and ends playback; this is a standing preference ("if I
 * leave it lying here, turn it off") and only ever acts once nothing is
 * happening. Playing, recording, a reindex running, a touch, or a
 * command from the browser remote or an MPD client all keep it on. A
 * paused track left on the screen does not.
 *
 * Shaped after screendim.h and sleeptimer.h: fixed steps chosen on the
 * Sleep page, step 0 is never and is the default, the monotonic clock so
 * an NTP step cannot move the deadline, header-only and pure.
 *
 * THE OFF ITSELF is not here -- see power_off_now() in player.c: the
 * PMS150 power controller on the Tab5 cuts the supply when the P-4
 * pulses PWROFF_PULSE, P4 of the expander at 0x44.
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
 * 15 minutes is the shortest: shorter than that and choosing a track
 * from across the room, or reading the BUILD tab, is enough to lose the
 * device. Two hours is the longest step the sleep timer has too.
 */
#define POWERDOWN_STEPS         (4)
#define POWERDOWN_DEFAULT_STEP  (0)     /* never: not something to spring on anyone */

static inline int powerdown_seconds(int step)
{
    static const int secs[POWERDOWN_STEPS] = { 15 * 60, 30 * 60, 60 * 60, 120 * 60 };
    if (step <= 0 || step > POWERDOWN_STEPS) return 0;      /* never */
    return secs[step - 1];
}

/* "15 min", "1 h", "Never" -- what the row says. */
static inline const char *powerdown_label(int step)
{
    switch (step) {
    case 1:  return N_("15 min");
    case 2:  return N_("30 min");
    case 3:  return N_("1 h");
    case 4:  return N_("2 h");
    default: return N_("Never");
    }
}

/*
 * Is it time? `last_active_us` is when anything last kept the device on,
 * on the monotonic clock. 0 seconds is never; a `last_active_us` of 0 is
 * a clock that has not started, and also answers false.
 */
static inline bool powerdown_due(int64_t now_us, int64_t last_active_us, int seconds)
{
    if (seconds <= 0 || last_active_us <= 0) return false;
    return now_us - last_active_us >= (int64_t)seconds * 1000000;
}

#ifdef __cplusplus
}
#endif
