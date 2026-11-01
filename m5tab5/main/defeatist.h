/*
 * defeatist.h -- the two halves of what app_main() was (8074).
 *
 * The player is becoming an ELF that Avoidant, or this firmware's own
 * shim, runs through the feckless ABI. Whatever a launcher brings up
 * before it starts an app is the shim's: standalone.c, which owns
 * app_main() in the .bin. What the app does is defeatist_main(), in
 * player.c, and it does not return.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The player. Does not return. Assumes what standalone.c brings up: NVS,
 * the bus and expanders, the USB host's owner, storage, and the network
 * brought up to be asked for. Every board init it repeats returns ESP_OK
 * when run again. */
void defeatist_main(void);

/* How installing the console driver went (5089), for the banner's line
 * under it. A program whose console is a launcher's never calls this and
 * prints no such line. */
void defeatist_note_console(esp_err_t err);

#ifdef __cplusplus
}
#endif
