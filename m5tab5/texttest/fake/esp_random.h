/*
 * esp_random.h, for the host: declared here, defined by the test that
 * needs it, so a test can decide what "random" returns (playlisttest.c
 * replays a fixed sequence to pin shuffle's picks).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
uint32_t esp_random(void);
