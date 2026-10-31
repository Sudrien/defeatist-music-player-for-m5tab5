/*
 * launcher.h -- is a launcher above this firmware, and the way back to it.
 *
 * Both ways of being run by a launcher count (8067): installed in a slot
 * of the launcher's partition table (a factory app exists and is not the
 * one running -- Avoidant's app2, M5Launcher's OTA slots), or an ELF the
 * launcher loaded and runs from its own slot (the table has Avoidant's
 * "app2" beside it). Only a firmware flashed on its own has none.
 * launcherpresence.h has the rule; this file gathers its three inputs
 * from the partition table. (8068: moved out of launcher_import.h, which
 * became shim_import.h.)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include "launcherpresence.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LAUNCHER_NONE / SLOT / HOSTED. Partition-table lookups: cheap and
 * instant, safe from a draw path. */
launcher_kind_t launcher_kind_now(void);

/* launcher_kind_now() != LAUNCHER_NONE. Used to hide the exit button on a
 * firmware flashed on its own, where there is nothing to return to. */
bool launcher_present(void);

/*
 * Go back to the launcher. From another slot: choose the factory slot to
 * boot and restart. As the launcher's ELF: restart, which lands in its
 * menu. Does not return on success; a no-op (returns) when there is no
 * launcher -- see launcher_present(). Caller should stop playback/
 * recording first; this does not unmount or flush.
 */
void launcher_exit_to_launcher(void);

#ifdef __cplusplus
}
#endif
