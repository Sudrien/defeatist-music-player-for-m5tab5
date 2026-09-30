/*
 * mpdmodetest.c -- play_order_t against MPD's four flags.
 *
 * The expectations are written from what each side MEANS -- playlist.h's
 * four comments, and MPD's documented behaviour for repeat/random/single/
 * consume -- and not from mpdmode.c's tables, which is
 * texttest/README.md's rule and is the entire point of a test over a
 * lookup table. A test that reads the table back proves the table equals
 * itself.
 *
 * So the sixteen reverse cases are spelled out one at a time with the
 * behaviour named in a comment, rather than generated from the same
 * indexing the implementation uses. That is deliberately more typing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/mpdmode.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static const char *order_name(play_order_t o)
{
    switch (o) {
    case PLAY_ORDER_ONE:        return "ONE";
    case PLAY_ORDER_ALL:        return "ALL";
    case PLAY_ORDER_SHUFFLE:    return "SHUFFLE";
    case PLAY_ORDER_REPEAT_ONE: return "REPEAT_ONE";
    case PLAY_ORDER_EAT:        return "EAT";
    case PLAY_ORDER_REPEAT_ALL: return "REPEAT_ALL";
    default:                    return "?";
    }
}

static void flags_str(mpd_modes_t m, char *out, size_t n)
{
    snprintf(out, n, "repeat=%d random=%d single=%d consume=%d",
             m.repeat, m.random, m.single, m.consume);
}

/* One reverse row: these flags must give this order, and say whether the
 * device does it exactly. */
static void rev(bool random, bool repeat, bool single, bool consume,
                play_order_t want, bool want_exact, const char *what)
{
    const mpd_modes_t m = { .repeat = repeat, .random = random,
                            .single = single, .consume = consume };
    const play_order_t got = mpdmode_to_order(&m);
    CHECK(got == want, "%s (r=%d rep=%d s=%d c=%d) gave %s, want %s",
          what, random, repeat, single, consume,
          order_name(got), order_name(want));
    CHECK(mpdmode_exact(&m) == want_exact, "%s: exact is %d, want %d",
          what, mpdmode_exact(&m), want_exact);
}

/* One forward row: this order must be exactly these flags. */
static void fwd(play_order_t o, bool repeat, bool random, bool single, bool consume)
{
    const mpd_modes_t got = mpdmode_from_order(o);
    char g[96];
    flags_str(got, g, sizeof(g));
    CHECK(got.repeat == repeat && got.random == random &&
          got.single == single && got.consume == consume,
          "%s gave [%s], want repeat=%d random=%d single=%d consume=%d",
          order_name(o), g, repeat, random, single, consume);
}

int main(void)
{
    /* ---- forward: what each order IS in MPD's terms ------------------ */
    /*
     * playlist.h's own comments are the source for these.
     *
     * The pair to get right is ONE and REPEAT_ONE, which playlist.h:30-38
     * calls opposites whose names are as close as the concepts. MPD has no
     * "repeat one" flag: it is single AND repeat together, where single
     * alone means stop after this track. So ONE is single without repeat
     * and REPEAT_ONE is single with it, and swapping them is the mistake
     * this section exists to catch.
     */
    fwd(PLAY_ORDER_ONE,        /* repeat */ false, /* random */ false, /* single */ true, false);
    fwd(PLAY_ORDER_ALL,        false, false, false, false);
    fwd(PLAY_ORDER_SHUFFLE,    false, true,  false, false);
    fwd(PLAY_ORDER_REPEAT_ONE, true,  false, true,  false);
    /* 5252: EAT is MPD's consume and nothing else. */
    fwd(PLAY_ORDER_EAT,        false, false, false, true);
    /* 5261: repeat-all is repeat and nothing else. */
    fwd(PLAY_ORDER_REPEAT_ALL, true,  false, false, false);

    /* An order outside the enum is the device's default and not row zero:
     * a bad value should land on what the player does when nobody has
     * chosen, which browser.c:212 says is ALL, rather than on "stop after
     * this track". */
    {
        const mpd_modes_t bad = mpdmode_from_order((play_order_t)99);
        const mpd_modes_t all = mpdmode_from_order(PLAY_ORDER_ALL);
        CHECK(memcmp(&bad, &all, sizeof(bad)) == 0,
              "an out-of-range order did not give ALL's flags");
        CHECK(mpdmode_from_order((play_order_t)-1).single == false,
              "a negative order gave ONE");
    }

    /* ---- reverse: eight combinations onto four orders ---------------- */
    /*
     * Each row names the BEHAVIOUR the flags ask for, so the expectation
     * is readable as a claim about MPD rather than as a table index.
     */
    rev(false, false, false, false, PLAY_ORDER_ALL,        true,
        "play through and stop");
    rev(false, false, true,  false, PLAY_ORDER_ONE,        true,
        "stop after this track");
    rev(false, true,  true,  false, PLAY_ORDER_REPEAT_ONE, true,
        "repeat this track (single AND repeat, which is how MPD says it)");
    rev(true,  false, false, false, PLAY_ORDER_SHUFFLE,    true,
        "random order, stop when exhausted");

    /*
     * repeat without single is "start the folder again when it ends",
     * probably the most commonly set option in any MPD client. Until 5261
     * the device had no such mode and this row was ALL, not exact.
     */
    rev(false, true,  false, false, PLAY_ORDER_REPEAT_ALL, true,
        "repeat the folder (5261)");

    /*
     * Random plus single. Looks exact and is not: with single set the next
     * track is never chosen automatically, so a random order never gets
     * used -- but MPD's `next` would still pick randomly where ONE's goes
     * to the following track (playlist.h:125). The difference survives in
     * the one place a listener would notice it, which is the skip button.
     */
    rev(true,  false, true,  false, PLAY_ORDER_ONE,        false,
        "one random track then stop -- random dropped");
    rev(true,  true,  true,  false, PLAY_ORDER_REPEAT_ONE, false,
        "repeat this track, randomly -- random dropped");
    rev(true,  true,  false, false, PLAY_ORDER_SHUFFLE,    false,
        "reshuffle at the end -- NO ANALOGUE");

    /*
     * consume is a loss on top of any of the eight, including the four
     * that are otherwise exact -- it is not a traversal setting at all.
     * Checked against every combination, because "exact except when
     * consume is set" is the kind of condition an implementation gets
     * right for the inexact rows and forgets for the exact ones.
     */
    /* 5252: consume alone is EAT, exactly; with repeat, EAT too -- all
     * of it is eaten, so there is nothing to repeat -- but not exact. */
    rev(false, false, false, true, PLAY_ORDER_EAT,        true,  "consume: eat");
    rev(false, false, true,  true, PLAY_ORDER_ONE,        false, "consume + single");
    rev(false, true,  false, true, PLAY_ORDER_EAT,        false, "consume + repeat");
    rev(false, true,  true,  true, PLAY_ORDER_REPEAT_ONE, false, "consume + repeat one");
    rev(true,  false, false, true, PLAY_ORDER_SHUFFLE,    false, "consume + random");
    rev(true,  false, true,  true, PLAY_ORDER_ONE,        false, "consume + random single");
    rev(true,  true,  false, true, PLAY_ORDER_SHUFFLE,    false, "consume + reshuffle");
    rev(true,  true,  true,  true, PLAY_ORDER_REPEAT_ONE, false, "consume + all three");

    CHECK(mpdmode_to_order(NULL) == PLAY_ORDER_ALL, "NULL did not give ALL");
    CHECK(!mpdmode_exact(NULL), "NULL was called exact");

    /* ---- the two directions have to agree --------------------------- */
    {
        /*
         * Every order must survive the round trip, which is the claim
         * mpdmode.h makes at the top: all four orders have an EXACT MPD
         * equivalent, so nothing the glass can be set to is lost on the
         * way out. If this fails, a listener changing the mode on the
         * screen would see a client report something else.
         */
        for (int o = 0; o <= (int)PLAY_ORDER_REPEAT_ALL; o++) {
            const play_order_t want = (play_order_t)o;
            const mpd_modes_t f = mpdmode_from_order(want);
            CHECK(mpdmode_to_order(&f) == want,
                  "%s did not survive the round trip: came back %s",
                  order_name(want), order_name(mpdmode_to_order(&f)));
            CHECK(mpdmode_exact(&f),
                  "%s's own flags were not called exact", order_name(want));
        }

        /*
         * And a row's `exact` claim must agree with the round trip, over
         * all sixteen states. The table writes `exact` down per row rather
         * than computing it, so this is the check that the two halves were
         * not edited apart.
         */
        for (int i = 0; i < 16; i++) {
            const mpd_modes_t m = {
                .repeat  = (i & 1) != 0,
                .random  = (i & 2) != 0,
                .single  = (i & 4) != 0,
                .consume = (i & 8) != 0,
            };
            const mpd_modes_t n = mpdmode_normalise(&m);
            const bool same = memcmp(&m, &n, sizeof(m)) == 0;
            CHECK(mpdmode_exact(&m) == same,
                  "state %d: exact says %d but the round trip %s",
                  i, mpdmode_exact(&m), same ? "kept it" : "changed it");
        }

        /*
         * NORMALISING IS IDEMPOTENT, and this is the property with a real
         * failure behind it: a client reads status, sees flags, and may
         * send them straight back. If normalising twice differed from
         * once, a client echoing what it read would walk the setting
         * somewhere new on every round -- a mode that drifts while nobody
         * touches it, which would look like the device changing its mind.
         */
        for (int i = 0; i < 16; i++) {
            const mpd_modes_t m = {
                .repeat  = (i & 1) != 0,
                .random  = (i & 2) != 0,
                .single  = (i & 4) != 0,
                .consume = (i & 8) != 0,
            };
            const mpd_modes_t a = mpdmode_normalise(&m);
            const mpd_modes_t b = mpdmode_normalise(&a);
            char sa[96], sb[96];
            flags_str(a, sa, sizeof(sa));
            flags_str(b, sb, sizeof(sb));
            CHECK(memcmp(&a, &b, sizeof(a)) == 0,
                  "state %d is not a fixed point: [%s] then [%s]", i, sa, sb);
            /* A normalised state is always one the device does exactly --
             * otherwise "what status reports" would itself be a lie. */
            CHECK(mpdmode_exact(&a),
                  "state %d normalised to something inexact: [%s]", i, sa);
            /* And it is always some order's own flags. */
            bool found = false;
            for (int o = 0; o <= (int)PLAY_ORDER_REPEAT_ALL; o++) {
                const mpd_modes_t f = mpdmode_from_order((play_order_t)o);
                if (memcmp(&a, &f, sizeof(f)) == 0) found = true;
            }
            CHECK(found, "state %d normalised to [%s], which is no order", i, sa);
        }

        CHECK(mpdmode_normalise(NULL).single == false, "NULL normalised to ONE");
    }

    /* ---- the button cycle still covers every order ------------------- */
    {
        /*
         * browser.c:1410 cycles ONE -> ALL -> SHUFFLE -> REPEAT_ONE -> ONE.
         * Transcribed, so if a fifth order is added and the cycle grows,
         * this notices that the flag table was not grown with it -- the
         * forward table is indexed by the enum and a new value would read
         * off the end of it, which the _Static_asserts in mpdmode.c turn
         * into a build failure rather than a bad row.
         */
        static const play_order_t cycle[] = {
            PLAY_ORDER_ONE, PLAY_ORDER_ALL, PLAY_ORDER_REPEAT_ALL, PLAY_ORDER_EAT,
            PLAY_ORDER_SHUFFLE, PLAY_ORDER_REPEAT_ONE,
        };
        const int n = (int)(sizeof(cycle) / sizeof(cycle[0]));
        bool seen[6] = { false };
        for (int i = 0; i < n; i++) seen[(int)cycle[i]] = true;
        for (int o = 0; o <= (int)PLAY_ORDER_REPEAT_ALL; o++)
            CHECK(seen[o], "%s is not reachable from the button cycle",
                  order_name((play_order_t)o));

        /* Each step of the cycle must change what MPD is told, or two
         * presses would look identical to a client. */
        for (int i = 0; i < n; i++) {
            const mpd_modes_t a = mpdmode_from_order(cycle[i]);
            const mpd_modes_t b = mpdmode_from_order(cycle[(i + 1) % n]);
            CHECK(memcmp(&a, &b, sizeof(a)) != 0,
                  "%s and %s are the same to MPD",
                  order_name(cycle[i]), order_name(cycle[(i + 1) % n]));
        }
    }

    /* ---- ReplayGain (5161) --------------------------------------------
     * MPD's four names (src/ReplayGainMode.cxx), strcmp'd, against a
     * device with one switch whose "on" is per-track gain. */
    CHECK(mpdmode_rg_from_name("off") == 0, "off turns it off");
    CHECK(mpdmode_rg_from_name("track") == 1, "track turns it on");
    CHECK(mpdmode_rg_from_name("album") == 1, "album is honoured as on (no album gain here)");
    CHECK(mpdmode_rg_from_name("auto") == 1, "auto is honoured as on");
    CHECK(mpdmode_rg_from_name("Track") == -1, "case-sensitive, as MPD's strcmp");
    CHECK(mpdmode_rg_from_name("OFF") == -1, "OFF is not off");
    CHECK(mpdmode_rg_from_name("") == -1, "empty");
    CHECK(mpdmode_rg_from_name("tracks") == -1, "a longer word");
    CHECK(mpdmode_rg_from_name("on") == -1, "on is not an MPD mode");
    CHECK(mpdmode_rg_from_name(NULL) == -1, "NULL");
    CHECK(strcmp(mpdmode_rg_name(true), "track") == 0, "on reports track");
    CHECK(strcmp(mpdmode_rg_name(false), "off") == 0, "off reports off");
    /* What a client reads back must be something it can send back and
     * land on the same state -- Cantata saves the mode and sends it on
     * every connect, so this round trip runs every time it connects. */
    for (int on = 0; on <= 1; on++)
        CHECK(mpdmode_rg_from_name(mpdmode_rg_name(on)) == on,
              "status -> mode round trip for %d", on);
    /* And album/auto spring back to track, not to what was asked. */
    CHECK(strcmp(mpdmode_rg_name(mpdmode_rg_from_name("album") == 1), "track") == 0,
          "album reads back as track");

    /* ---- nextsong (5166) ---------------------------------------------
     * MPD's GetNextPosition() through the four orders. */
    CHECK(mpdmode_next_pos(PLAY_ORDER_ALL, 0, 3) == 1, "ALL: the next one");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ALL, 2, 3) == -1, "ALL at the end: none (no repeat-all here)");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ONE, 0, 3) == 1, "ONE: single without repeat still names the next");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ONE, 2, 3) == -1, "ONE at the end: none");
    CHECK(mpdmode_next_pos(PLAY_ORDER_REPEAT_ONE, 1, 3) == 1, "REPEAT_ONE: the same song");
    CHECK(mpdmode_next_pos(PLAY_ORDER_REPEAT_ONE, 2, 3) == 2, "REPEAT_ONE at the end: still the same");
    CHECK(mpdmode_next_pos(PLAY_ORDER_SHUFFLE, 0, 3) == -1, "SHUFFLE: unknowable, so none");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ALL, -1, 3) == -1, "no current: none");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ALL, 3, 3) == -1, "a current past the end: none");
    CHECK(mpdmode_next_pos(PLAY_ORDER_REPEAT_ONE, 0, 0) == -1, "an empty list: none");
    CHECK(mpdmode_next_pos(PLAY_ORDER_ALL, 0, 1) == -1, "one song: none after it");
    /* Agreement with the reverse table: what MPD would compute from the
     * flags this device reports for each order. */
    for (int o = 0; o <= (int)PLAY_ORDER_REPEAT_ALL; o++) {
        const mpd_modes_t m = mpdmode_from_order((play_order_t)o);
        for (int cur = 0; cur < 4; cur++) {
            int mpd;                                /* GetNextPosition, verbatim */
            if (m.single && m.repeat) mpd = cur;
            else if (cur + 1 < 4)     mpd = cur + 1;
            else if (m.repeat)        mpd = 0;
            else                      mpd = -1;
            const int ours = mpdmode_next_pos((play_order_t)o, cur, 4);
            if (m.random) CHECK(ours == -1, "%s: shuffle names nothing", order_name((play_order_t)o));
            else CHECK(ours == mpd, "%s at %d: ours %d, MPD's rule %d",
                       order_name((play_order_t)o), cur, ours, mpd);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
