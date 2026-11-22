/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0041, 0042: what of the network this build has.
 *
 * net_limited() is true while absent.c stands in for part of the network
 * section. Since u-feckless-0071 a box answers the routes' state and
 * lwIP's sockets, so a cable the launcher brought up connects and MPD
 * serves on it; since u-feckless-0073 (u-defeatist-0044) it answers the
 * HTTP client too, so web radio, the benchmark, podcast search and station
 * pictures work over that cable. It does not answer the radio's control
 * (wifi_request_apply and the portal), NTP, feckless_fetch (podcast
 * downloads) or the HTTP server (remote control), because each calls back
 * into the player or needs the launcher's hooks. The screens grey those
 * and say why, and their switches log it. Defined in absent.c beside the
 * stand-ins, and goes with them.
 */
#pragma once

#include <stdbool.h>

bool net_limited(void);
