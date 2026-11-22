/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0041: whether this build has a network at all.
 *
 * True while absent.c stands in for the network section -- Wi-Fi,
 * esp_netif, sockets, the HTTPS server, feckless_fetch -- because a box
 * has no host calls for it yet. Then nothing connects, whatever the
 * settings say, and the screens that would wait for an address say so
 * instead. Defined in absent.c beside those stand-ins, and goes with
 * them when a box answers the network.
 */
#pragma once

#include <stdbool.h>

bool net_absent(void);
