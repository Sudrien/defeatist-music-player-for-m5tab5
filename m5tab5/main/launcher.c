/*
 * launcher.c -- see launcher.h. Moved out of launcher_import.c by 8068.
 *
 * SPDX-License-Identifier: MIT
 */
#include "launcher.h"
#include "launcherpresence.h"   /* 8067 */

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"   /* esp_restart */

/* ---- launcher presence / return ----------------------------------------- */
/* 8067: the decision is launcherpresence.h's (host-tested). Both ways of
 * being run by a launcher: a firmware in another slot of its table, and an
 * ELF it loaded, which runs from its own (factory) slot. */
static const esp_partition_t *launcher_factory(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

static launcher_kind_t launcher_kind_of_table(void) {
    const esp_partition_t *fac = launcher_factory();
    const bool has_app2 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                   ESP_PARTITION_SUBTYPE_APP_OTA_0, "app2") != NULL;
    return launcher_kind(fac != NULL, fac && fac == esp_ota_get_running_partition(), has_app2);
}

launcher_kind_t launcher_kind_now(void) {
    return launcher_kind_of_table();
}

bool launcher_present(void) {
    return launcher_kind_of_table() != LAUNCHER_NONE;
}

void launcher_exit_to_launcher(void) {
    switch (launcher_kind_of_table()) {
    case LAUNCHER_SLOT:
        /* Choosing factory also erases otadata, which is how Avoidant
         * tells this from a reset out of its app2 (its 0015). The call
         * is named in the ELF build too (feckless-libs' 7002), though an
         * ELF never reaches it. */
        if (esp_ota_set_boot_partition(launcher_factory()) != ESP_OK) return;
        esp_restart();                                /* does not return */
        break;
    case LAUNCHER_HOSTED:
        /* The launcher's own slot is running, and nothing was chosen to
         * come back to: a restart lands in its menu. */
        esp_restart();
        break;
    case LAUNCHER_NONE:
    default:
        return;                                       /* nothing above us */
    }
}
