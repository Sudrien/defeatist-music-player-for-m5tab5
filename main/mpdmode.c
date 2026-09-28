/*
 * mpdmode.c -- see mpdmode.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpdmode.h"

#include <string.h>

/*
 * The forward table is indexed by the enum, so the enum's values matter.
 * Asserted rather than assumed: adding a fifth order, or reordering these
 * four, must fail the build here instead of silently reading the wrong
 * row.
 */
_Static_assert(PLAY_ORDER_ONE        == 0, "play_order_t values moved");
_Static_assert(PLAY_ORDER_ALL        == 1, "play_order_t values moved");
_Static_assert(PLAY_ORDER_SHUFFLE    == 2, "play_order_t values moved");
_Static_assert(PLAY_ORDER_REPEAT_ONE == 3, "play_order_t values moved");

#define N_ORDERS    (4)

/*
 * What each order IS, in MPD's terms. All four are exact.
 *
 *   ONE         stop at the end of the track   -> single, no repeat
 *   ALL         on to the next, stop at the
 *               end of the folder              -> nothing set
 *   SHUFFLE     random, no repeats until
 *               exhausted                      -> random
 *   REPEAT_ONE  this track again               -> single AND repeat
 *
 * The last is the one worth reading twice: MPD expresses "repeat this
 * track" as single AND repeat together, not as a mode of its own. single
 * alone means stop after this track, which is PLAY_ORDER_ONE -- and
 * playlist.h:30-38 says those two are opposites whose names are as close
 * as the concepts, so this is the place that pair is most likely to be
 * got backwards.
 */
static const mpd_modes_t s_fwd[N_ORDERS] = {
    /* ONE        */ { .repeat = false, .random = false, .single = true,  .consume = false },
    /* ALL        */ { .repeat = false, .random = false, .single = false, .consume = false },
    /* SHUFFLE    */ { .repeat = false, .random = true,  .single = false, .consume = false },
    /* REPEAT_ONE */ { .repeat = true,  .random = false, .single = true,  .consume = false },
};

/*
 * And back, as data. Indexed random<<2 | repeat<<1 | single, so the row
 * order here is the truth table in mpdmode.h read top to bottom.
 *
 * `exact` is not derived from a comparison at runtime, it is written down
 * per row, because the question "is this one of the four we actually do"
 * is a fact about the table and not about an implementation. The test
 * checks the two agree, which is the useful direction: a row whose
 * `exact` claim disagrees with the round trip is a table someone edited
 * halfway.
 */
static const struct {
    play_order_t order;
    bool         exact;
} s_rev[8] = {
    /* 0: random 0 repeat 0 single 0 */ { PLAY_ORDER_ALL,        true  },
    /* 1: random 0 repeat 0 single 1 */ { PLAY_ORDER_ONE,        true  },
    /* 2: random 0 repeat 1 single 0 */ { PLAY_ORDER_ALL,        false },  /* repeat-all */
    /* 3: random 0 repeat 1 single 1 */ { PLAY_ORDER_REPEAT_ONE, true  },
    /* 4: random 1 repeat 0 single 0 */ { PLAY_ORDER_SHUFFLE,    true  },
    /* 5: random 1 repeat 0 single 1 */ { PLAY_ORDER_ONE,        false },  /* random dropped */
    /* 6: random 1 repeat 1 single 0 */ { PLAY_ORDER_SHUFFLE,    false },  /* reshuffle */
    /* 7: random 1 repeat 1 single 1 */ { PLAY_ORDER_REPEAT_ONE, false },  /* random dropped */
};

static int rev_index(const mpd_modes_t *m)
{
    return (m->random ? 4 : 0) | (m->repeat ? 2 : 0) | (m->single ? 1 : 0);
}

mpd_modes_t mpdmode_from_order(play_order_t o)
{
    /* An order outside the enum is ALL, which is the device's own default
     * (`browser.c:212`) rather than the first row -- a bad value should
     * land on what the player does when nobody has chosen, not on
     * "stop after this track". */
    if ((int)o < 0 || (int)o >= N_ORDERS) return s_fwd[PLAY_ORDER_ALL];
    return s_fwd[o];
}

play_order_t mpdmode_to_order(const mpd_modes_t *m)
{
    if (!m) return PLAY_ORDER_ALL;
    return s_rev[rev_index(m)].order;
}

bool mpdmode_exact(const mpd_modes_t *m)
{
    if (!m) return false;
    /* consume is a loss on top of whatever the other three did: no order
     * expresses it, so a request carrying it is never exactly honoured. */
    if (m->consume) return false;
    return s_rev[rev_index(m)].exact;
}

mpd_modes_t mpdmode_normalise(const mpd_modes_t *m)
{
    if (!m) return s_fwd[PLAY_ORDER_ALL];
    return mpdmode_from_order(mpdmode_to_order(m));
}

int mpdmode_rg_from_name(const char *name)
{
    if (!name) return -1;
    /* strcmp, as MPD's FromString: case matters. */
    if (strcmp(name, "off") == 0) return 0;
    if (strcmp(name, "track") == 0 || strcmp(name, "album") == 0 ||
        strcmp(name, "auto") == 0) return 1;
    return -1;
}

const char *mpdmode_rg_name(bool on)
{
    return on ? "track" : "off";
}
