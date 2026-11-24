/*
 * hotspot.h -- the Wi-Fi switch's third position (u-defeatist-0049).
 *
 * Off, on, hotspot. In the hotspot the player is its own network: an open
 * access point named as the portal's was (portalweb_ap_name(),
 * "Defeatist-XXXX"), its DHCP server the AP's own, and a whitelist ON THE
 * PLAYER in place of a password (u-defeatist-0050): a phone that joins
 * and is not on it makes the player ask "Allow this phone on the
 * hotspot?" with its MAC (approve.h); allowed, it stays and is remembered;
 * refused, or not answered in approvelist.h's two minutes, it is sent
 * away (wifi_ap_kick()) and sent away again on sight for ten. A phone
 * that stays opens https://192.168.4.1/, and
 * has the remote and MPD with no router anywhere -- a car, a field, a
 * party whose Wi-Fi is not yours. Nothing on it reaches the internet;
 * whatever the player plays from its card works.
 *
 * OPEN, NOT ENCRYPTED. The air is readable by anyone near: the remote is
 * HTTPS regardless, MPD is plain text. A MAC can be copied by someone who
 * has seen an allowed phone's. The whitelist keeps the four places on the
 * AP and the player's attention for the phones meant to have them; the
 * remote's and MPD's own approvals still stand behind it.
 *
 * The station is not stopped: a network already joined stays joined (the
 * AP follows its channel), and only the hunting for one -- the background
 * join -- holds while the hotspot is up, through the portal_running hook
 * (player.c's net_hold()). So a player at home can be both, and a player
 * in a field does not hop channels under its own phones.
 *
 * hotspot_poll() raises and lowers the AP. It is called from the player's
 * loop, beside remote_poll(): wifi_ap_begin() and wifi_ap_kick() are not for ui_task
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

/* The allowed list was forgotten: the AP comes down and back up. */
void hotspot_restart(void);

/*
 * u-defeatist-0053: one prompt per phone. A browser asking to be allowed
 * from the address the hotspot's DHCP server gave a phone already allowed
 * onto it is that phone -- the listener said yes to it a moment ago -- so
 * it is allowed without a second prompt. ui_task only (it answers, and
 * answering writes NVS): called before each draw of the main screen, the
 * panel and the chooser. True if it answered one.
 */
bool hotspot_allow_own_browser(void);

#ifdef __cplusplus
}
#endif
