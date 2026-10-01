/*
 * rtc8130.h -- the Tab5's battery-backed clock, an Epson RX8130CE at 0x32.
 *
 * 6003. The player's time is a floor (settings.c): the latest of the
 * build stamp, the card's newest record and the last NTP reply, plus
 * uptime. Every one of those is a time the player saw before it was
 * switched off, so after a real power-off (6000) the clock is behind by
 * however long the device sat off. The RX8130 keeps counting on BT1
 * while the board is unpowered, so it is one more floor -- the only one
 * that knows about the time spent off.
 *
 * Read at boot and offered to settings.c like any other floor: it wins
 * only when it is later. Written forward only from the player's own
 * time (rtc8130_write_forward()), because the RTC usually runs AHEAD of
 * the floor after a power-off and writing the floor would set it back;
 * and written unconditionally from NTP (rtc8130_write()), which is the
 * truth either way.
 *
 * Registers from Epson's RX8130CE application manual, checked against
 * M5Stack's driver (M5Tab5-UserDemo, rx8130.cpp) -- but NOT copying two
 * things it does: it stores the month 0-based and the weekday as a
 * number, where the chip counts months 1..12 and wants the weekday as a
 * one-hot bit. Its init of the backup bits in register 0x1F IS copied.
 *
 * Every call is a no-op returning false until rtc8130_init() has found
 * the chip, so a board without one, or a bus fault, costs nothing but a
 * log line.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Find the chip and enable its backup supply. False when absent. */
bool rtc8130_init(i2c_master_bus_handle_t bus);

/*
 * The time it holds, as a Unix epoch in UTC. False when there is no
 * chip, or it reports that its oscillator stopped or its backup voltage
 * fell (VLF) since it was last set -- a time it cannot vouch for.
 */
bool rtc8130_read(int64_t *epoch);

/* Set it. 2000..2099 only. Clears VLF, so the next read is trusted. */
bool rtc8130_write(int64_t epoch);

/*
 * Set it only if `epoch` is more than 2 s ahead of what it holds, or it
 * holds nothing it can vouch for. The player's floor goes in this way.
 */
bool rtc8130_write_forward(int64_t epoch);

#ifdef __cplusplus
}
#endif
