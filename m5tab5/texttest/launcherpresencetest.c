/*
 * launcherpresencetest.c -- main/launcherpresence.h, compiled from the header.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "launcherpresence.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

int main(void)
{
    /* has_factory, running_is_factory, has_app2 */

    /* flashed on its own, Arduino-style table: app0, app1, no factory */
    CHECK(launcher_kind(false, false, false) == LAUNCHER_NONE, "standalone");

    /* installed in app2 under Avoidant, or an OTA slot under M5Launcher */
    CHECK(launcher_kind(true, false, true)  == LAUNCHER_SLOT, "in app2 under Avoidant");
    CHECK(launcher_kind(true, false, false) == LAUNCHER_SLOT, "in an OTA slot under M5Launcher");

    /* an ELF Avoidant loaded: the running partition is Avoidant's own */
    CHECK(launcher_kind(true, true, true)   == LAUNCHER_HOSTED, "an ELF under Avoidant");

    /* a firmware that is simply in a factory-only layout: nothing above it */
    CHECK(launcher_kind(true, true, false)  == LAUNCHER_NONE, "alone in factory");

    /* no factory slot: whatever else is true, no launcher */
    CHECK(launcher_kind(false, true, true)  == LAUNCHER_NONE, "no factory, running_is_factory set");
    CHECK(launcher_kind(false, false, true) == LAUNCHER_NONE, "no factory, app2 present");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
