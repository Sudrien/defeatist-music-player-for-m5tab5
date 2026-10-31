/*
 * launcherpresence.h -- is a launcher above us, and how do we get back to
 * it? The decision, from three facts about the partition table; the
 * lookups and the restart are launcher_import.c's.
 *
 * 8067. Two ways to be run by a launcher, and they look different in the
 * table:
 *
 *   - a firmware installed in a slot of the launcher's table: the
 *     launcher is the factory app and we run from another slot (app2
 *     under Avoidant, an OTA slot under M5Launcher). To go back, choose
 *     the factory slot for the next boot and restart.
 *
 *   - an ELF the launcher loaded (feckless_abi): the board is the
 *     launcher's and so is the running partition -- the factory slot.
 *     Nothing was chosen, so to go back is only to restart. The table
 *     says Avoidant's by its second slot, "app2": a firmware that is
 *     merely in a factory-only layout has none, and restarting would
 *     just run it again.
 *
 * A firmware flashed on its own (the Arduino-style layout: app0, app1,
 * no factory) has no launcher, in either build.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAUNCHER_NONE = 0,   /* nothing to go back to */
    LAUNCHER_SLOT,       /* we are in another slot: choose factory, then restart */
    LAUNCHER_HOSTED,     /* we are the launcher's ELF: restart */
} launcher_kind_t;

static inline launcher_kind_t launcher_kind(bool has_factory, bool running_is_factory,
                                            bool has_app2)
{
    if (!has_factory) return LAUNCHER_NONE;
    if (!running_is_factory) return LAUNCHER_SLOT;
    return has_app2 ? LAUNCHER_HOSTED : LAUNCHER_NONE;
}

#ifdef __cplusplus
}
#endif
