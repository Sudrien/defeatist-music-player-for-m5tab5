/*
 * wifijoin.h -- try a network and save it only if it worked.
 *
 * Moved out of portal.c's try_join() (5122) so the setup portal and the
 * browser remote's Wi-Fi section use one path: portalweb_join_plan()'s
 * order (derived PSK first where it can work, the passphrase where the
 * network is WPA3-capable), wifi_join() for each attempt, and
 * wifistore_save() of exactly what joined. See portal.h for why each of
 * those is the way it is; none of it changed in the move.
 *
 * Blocks for up to two join timeouts (about thirty seconds each in the
 * worst case -- see wifi_join()). Not from ui_task. Never logs the
 * secret, and clears its own copies of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "portalweb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a scanned network's security says about which key it can use,
 * from a wifi_seen_t's auth (a wifi_auth_mode_t). */
portalweb_net_t wifijoin_net(uint8_t auth);

/*
 * Check, plan, join, and on success save. `tag` names the caller in the
 * log lines ("tab5_portal", "tab5_remote"), so a board log says which
 * page the attempt came from.
 *
 *   ESP_OK                 joined and saved
 *   ESP_ERR_INVALID_ARG    the pair did not pass portalweb_check()
 *   anything else          wifi_join()'s or wifistore_save()'s error
 */
esp_err_t wifijoin_try(const char *tag, const char *ssid, const char *pass,
                       portalweb_net_t net);

#ifdef __cplusplus
}
#endif
