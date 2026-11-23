/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0041, 0042, 0047: what of the network this build has.
 *
 * portal_absent() is true while absent.c stands in for the setup portal's
 * access point and DHCP server. Since u-feckless-0071 a box answers the
 * routes' state and lwIP's sockets (a cable, MPD); since u-feckless-0073
 * the HTTP client (web radio, the benchmark, podcast search, station
 * pictures); since u-feckless-0075..0078 the HTTP server and the hash
 * (remote control); since u-feckless-0081 feckless_fetch (downloads); and
 * since u-feckless-0083 (u-defeatist-0047) the radio's control and the
 * network hooks, so Wi-Fi, its saved networks and NTP. Not yet the
 * portal's AP (wifi_ap_begin and the DHCP server): the "Add a network"
 * row is greyed and says why, and its press logs it. Was net_limited(),
 * which covered the radio and NTP too. Defined in absent.c beside the
 * stand-ins, and goes with them.
 */
#pragma once

#include <stdbool.h>

bool portal_absent(void);
