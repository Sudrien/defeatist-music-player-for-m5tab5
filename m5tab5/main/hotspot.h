/*
 * hotspot.h -- the Wi-Fi switch's third position (u-defeatist-0049).
 *
 * Off, on, hotspot. In the hotspot the player is its own network: a WPA2
 * access point named as the portal's was (portalweb_ap_name(),
 * "Defeatist-XXXX"), its password settings_hotspot_pass(), its DHCP
 * server the AP's own. A phone joins it, opens https://192.168.4.1/, and
 * has the remote and MPD with no router anywhere -- a car, a field, a
 * party whose Wi-Fi is not yours. Nothing on it reaches the internet;
 * whatever the player plays from its card works.
 *
 * The station is not stopped: a network already joined stays joined (the
 * AP follows its channel), and only the hunting for one -- the background
 * join -- holds while the hotspot is up, through the portal_running hook
 * (player.c's net_hold()). So a player at home can be both, and a player
 * in a field does not hop channels under its own phones.
 *
 * hotspot_poll() raises and lowers the AP. It is called from the player's
 * loop, beside remote_poll(): wifi_ap_begin_secure() is not for ui_task
 * (wifi.h). In a box, the AP calls are the box's (u-feckless-0084).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void hotspot_poll(void);

/* The AP is up, by this file's doing. */
bool hotspot_running(void);

/* Its name ("" before the first start), its address while up, and the
 * phones on it. */
void hotspot_name(char *out, size_t out_size);
bool hotspot_ip(char *out, size_t out_size);
int  hotspot_clients(void);

/* The password changed: the AP comes down and back up with it. */
void hotspot_restart(void);

#ifdef __cplusplus
}
#endif
