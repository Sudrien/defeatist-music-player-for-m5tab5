/*
 * screendimtest.c -- screendim.h: the steps, the deadline, the level.
 *
 * The expected values are written from what the feature should do --
 * "the default is 30 seconds", "a touch at the deadline does not dim" --
 * rather than read off the table, which is texttest/README.md's rule.
 * The deadline cases are the ones worth having: a counter that starts at
 * zero and a comparison that is off by one both produce a screen that
 * dims when it should not, and neither is visible in the arithmetic.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/screendim.h"

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

#define S (1000000LL)       /* one second, in microseconds */

int main(void)
{
    /* ---- the intervals the user asked for ---------------------------- */
    {
        CHECK(screendim_seconds(1) == 15,  "step 1 is %d", screendim_seconds(1));
        CHECK(screendim_seconds(2) == 30,  "step 2 is %d", screendim_seconds(2));
        CHECK(screendim_seconds(3) == 60,  "step 3 is %d", screendim_seconds(3));
        CHECK(screendim_seconds(4) == 120, "step 4 is %d", screendim_seconds(4));

        /* Never, from both ends and from nonsense. */
        CHECK(screendim_seconds(0) == 0, "step 0 should be never");
        CHECK(screendim_seconds(5) == 0, "a step past the table should be never");
        CHECK(screendim_seconds(-3) == 0, "a negative step should be never");

        /* The default is 30 s, which is the one number that was asked
         * for by value rather than by position. */
        CHECK(screendim_seconds(SCREENDIM_DEFAULT_STEP) == 30,
              "the default is %d s, wanted 30",
              screendim_seconds(SCREENDIM_DEFAULT_STEP));
    }

    /* ---- a saved value comes back as the same step ------------------- */
    {
        for (int s = 0; s <= SCREENDIM_STEPS; s++) {
            const int secs = screendim_seconds(s);
            CHECK(screendim_step_for_seconds(secs) == s,
                  "step %d -> %d s -> step %d", s, secs,
                  screendim_step_for_seconds(secs));
        }

        /* A value this build's table does not hold is never, not the
         * nearest step: rounding another build's 45 into 30 would change
         * a setting the listener chose without saying so. */
        CHECK(screendim_step_for_seconds(45) == 0, "45 s was rounded in");
        CHECK(screendim_step_for_seconds(300) == 0, "300 s was rounded in");
        CHECK(screendim_step_for_seconds(-1) == 0, "-1 s was accepted");
    }

    /* ---- the labels ------------------------------------------------- */
    {
        CHECK(strcmp(screendim_label(0), "Never") == 0, "label 0");
        CHECK(strcmp(screendim_label(1), "15 s") == 0, "label 1");
        CHECK(strcmp(screendim_label(2), "30 s") == 0, "label 2");
        CHECK(strcmp(screendim_label(3), "1 min") == 0, "label 3");
        CHECK(strcmp(screendim_label(4), "2 min") == 0, "label 4");
        /* Anything else reads as Never rather than as an empty row. */
        CHECK(strcmp(screendim_label(9), "Never") == 0, "label out of range");

        /* Every step has a label, and no two share one. */
        for (int a = 0; a <= SCREENDIM_STEPS; a++) {
            CHECK(screendim_label(a)[0] != '\0', "step %d has no label", a);
            for (int b = a + 1; b <= SCREENDIM_STEPS; b++) {
                CHECK(strcmp(screendim_label(a), screendim_label(b)) != 0,
                      "steps %d and %d share the label \"%s\"",
                      a, b, screendim_label(a));
            }
        }
    }

    /* ---- the deadline ----------------------------------------------- */
    {
        const int64_t t = 1000 * S;      /* some time well after boot */

        /* Just before, exactly at, just after 30 s. "At" dims: the
         * setting says dim after 30 s, and 30.000 s is after 30. */
        CHECK(!screendim_due(t + 29 * S, t, 30), "dimmed at 29 s");
        CHECK(screendim_due(t + 30 * S, t, 30), "did not dim at 30 s");
        CHECK(screendim_due(t + 31 * S, t, 30), "did not dim at 31 s");

        /* Never means never, however long it has been. */
        CHECK(!screendim_due(t + 3600 * S, t, 0),
              "dimmed after an hour with the setting off");

        /* NOTHING HAS HAPPENED YET. A last-touch of 0 is the state
         * before the first touch, and a screen that treats it as "touched
         * at boot" dims `seconds` after power-on whether or not anyone is
         * looking at it. */
        CHECK(!screendim_due(t, 0, 30), "dimmed with no touch recorded");
        CHECK(!screendim_due(t, -1, 30), "dimmed on a negative last-touch");

        /* A clock that has not moved, or appears to have gone backwards,
         * is not a reason to dim. */
        CHECK(!screendim_due(t, t, 30), "dimmed with no time passed");
        CHECK(!screendim_due(t - S, t, 30), "dimmed on a backwards clock");

        /* The shortest and longest steps, at their own deadlines. */
        CHECK(screendim_due(t + 15 * S, t, screendim_seconds(1)), "15 s step");
        CHECK(!screendim_due(t + 14 * S, t, screendim_seconds(1)), "14 s");
        CHECK(screendim_due(t + 120 * S, t, screendim_seconds(4)), "2 min step");
        CHECK(!screendim_due(t + 119 * S, t, screendim_seconds(4)), "119 s");
    }

    /* ---- the dim level ---------------------------------------------- */
    {
        /* Half the slider position, which is what "dim to 50%" means to
         * someone looking at the control. */
        CHECK(screendim_level(90, 5) == 45, "90 dims to %d",
              screendim_level(90, 5));
        CHECK(screendim_level(100, 5) == 50, "100 dims to %d",
              screendim_level(100, 5));
        CHECK(screendim_level(50, 5) == 25, "50 dims to %d",
              screendim_level(50, 5));

        /* Never below the minimum the slider itself allows: dimming into
         * a brightness the listener cannot choose is a screen they cannot
         * read and cannot explain. */
        CHECK(screendim_level(8, 5) == 5, "8 with a floor of 5 gave %d",
              screendim_level(8, 5));
        CHECK(screendim_level(5, 5) == 5, "at the floor already: %d",
              screendim_level(5, 5));

        /* And never brighter than the setting, even if the floor is above
         * it -- dimming must not turn the screen up. */
        CHECK(screendim_level(3, 5) == 3, "a floor above the setting gave %d",
              screendim_level(3, 5));

        /* A property over the whole range: dimming never brightens, and
         * never goes under the floor. */
        for (int pct = 1; pct <= 100; pct++) {
            const int d = screendim_level(pct, 5);
            CHECK(d <= pct, "%d%% dimmed UP to %d%%", pct, d);
            CHECK(d >= 5 || d == pct, "%d%% dimmed below the floor to %d%%",
                  pct, d);
        }
    }

    /* ---- the screen-off table --------------------------------------- */
    {
        CHECK(screenoff_seconds(1) == 30,  "off 1 is %d", screenoff_seconds(1));
        CHECK(screenoff_seconds(2) == 60,  "off 2 is %d", screenoff_seconds(2));
        CHECK(screenoff_seconds(3) == 120, "off 3 is %d", screenoff_seconds(3));
        CHECK(screenoff_seconds(4) == 300, "off 4 is %d", screenoff_seconds(4));
        CHECK(screenoff_seconds(0) == 0, "off 0 should be never");
        CHECK(screenoff_seconds(5) == 0, "off past the table should be never");

        /* Off is never by default: a screen that starts going black on
         * its own after an update nobody asked for is a fault report. */
        CHECK(screenoff_seconds(SCREENOFF_DEFAULT_STEP) == 0,
              "the default is %d s, wanted never",
              screenoff_seconds(SCREENOFF_DEFAULT_STEP));

        for (int s = 0; s <= SCREENOFF_STEPS; s++) {
            CHECK(screenoff_step_for_seconds(screenoff_seconds(s)) == s,
                  "off step %d did not survive a round trip", s);
        }
        CHECK(screenoff_step_for_seconds(45) == 0, "45 s was rounded in");

        for (int a = 0; a <= SCREENOFF_STEPS; a++) {
            CHECK(screenoff_label(a)[0] != '\0', "off step %d has no label", a);
            for (int b = a + 1; b <= SCREENOFF_STEPS; b++) {
                CHECK(strcmp(screenoff_label(a), screenoff_label(b)) != 0,
                      "off steps %d and %d share \"%s\"",
                      a, b, screenoff_label(a));
            }
        }
    }

    /* ---- off never comes before the dim ------------------------------ */
    {
        /* The ordinary case: off well after dim, unchanged. */
        CHECK(screenoff_effective(120, 30) == 120, "120 after 30 gave %d",
              screenoff_effective(120, 30));

        /*
         * The contradiction. Off at 30 s with dim at 2 min would take the
         * screen black without ever dimming, and the dim row would
         * silently do nothing -- a setting visibly set and having no
         * effect is worse than either behaviour.
         */
        CHECK(screenoff_effective(30, 120) == 120,
              "off 30 with dim 120 gave %d, wanted the dim's 120",
              screenoff_effective(30, 120));

        /* Equal is allowed: they happen in the same pass, and the off
         * branch runs after the dim one. */
        CHECK(screenoff_effective(60, 60) == 60, "equal gave %d",
              screenoff_effective(60, 60));

        /* Never means never, whatever the dim is -- including when the
         * dim is never too. */
        CHECK(screenoff_effective(0, 30) == 0, "off never was overridden");
        CHECK(screenoff_effective(0, 0) == 0, "both never");

        /* A dim of never does not hold off back: somebody who wants the
         * screen to go black without dimming first should get that. */
        CHECK(screenoff_effective(30, 0) == 30, "off 30 with dim never gave %d",
              screenoff_effective(30, 0));

        /* A property over both tables: the result is never earlier than
         * the dim, and never earlier than what was asked for. */
        for (int o = 0; o <= SCREENOFF_STEPS; o++) {
            for (int d = 0; d <= SCREENDIM_STEPS; d++) {
                const int os = screenoff_seconds(o), ds = screendim_seconds(d);
                const int e = screenoff_effective(os, ds);
                if (os == 0) {
                    CHECK(e == 0, "off never became %d", e);
                } else {
                    CHECK(e >= os, "off %d became the earlier %d", os, e);
                    CHECK(e >= ds, "off %d lands before the dim at %d", e, ds);
                }
            }
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
