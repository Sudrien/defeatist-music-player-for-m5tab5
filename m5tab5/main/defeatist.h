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

#ifdef __cplusplus
extern "C" {
#endif

/* The player. Does not return. Assumes what a launcher brings up before it
 * runs an app -- Avoidant, or standalone.c: NVS, the bus and expanders, the
 * USB host's owner, storage, and the network brought up to be asked for.
 * Every board init it repeats returns ESP_OK when run again. In the ELF
 * (8087) it is called from elf_main.c's app_main(). */
void defeatist_main(void);

#ifdef __cplusplus
}
#endif
