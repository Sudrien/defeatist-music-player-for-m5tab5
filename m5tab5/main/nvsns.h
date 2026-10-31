/*
 * nvsns.h -- the one NVS namespace this player keeps everything in.
 *
 * 6006. It used three, each named for the module that first needed one:
 * "radiokeep" (radiokeep.c's two stations, then settings.c's Wi-Fi
 * switch, prefs blob and clock correction joined it), "wifistore" (the
 * saved networks) and "devcert" (the remote's certificate and key).
 * Under a launcher every firmware on the board shares the one 20 KB nvs
 * partition, and only the namespace keeps their data apart -- so the
 * namespace should say whose it is. All eight keys (last, star,
 * wifi_on, prefs, clkfix, nets, key, crt) were already distinct.
 *
 * nvsns_migrate() moves the old three in once; see nvsns.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define DEFEATIST_NVS_NS "defeatist"    /* NVS allows 15 characters */

#ifdef __cplusplus
extern "C" {
#endif

/* After nvs_flash_init() and before anything opens DEFEATIST_NVS_NS. */
void nvsns_migrate(void);

#ifdef __cplusplus
}
#endif
