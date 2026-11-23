/* Host stand-in for netformtest (u-defeatist-0048): the one name netform.c
 * takes from esp_wifi.h. ESP_ERR_WIFI_BASE + 10, as ESP-IDF has it. */
#pragma once
#include "esp_err.h"
#define ESP_ERR_WIFI_PASSWORD (0x3000 + 10)
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT 0x107
#endif
