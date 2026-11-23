/*
 * hotspot.c -- see hotspot.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hotspot.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "portal.h"
#include "portalweb.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "tab5_hotspot";

#define RETRY_US (10 * 1000 * 1000)         /* a refused start, tried again */

static volatile bool s_on;
static volatile bool s_restart;
static char s_name[PORTAL_SSID_MAX + 1];
static int64_t s_retry_us;

static bool wanted(void)
{
    return settings_wifi_enabled() && settings_hotspot() && !portal_running();
}

void hotspot_poll(void)
{
    if (s_on && (s_restart || !wanted() || !wifi_up())) {
        /* the radio going down took the AP with it (wifi_stop()); only a
         * radio still up has one to end */
        if (wifi_up()) (void)wifi_ap_end();
        s_on = false;
        ESP_LOGI(TAG, "hotspot down%s", s_restart ? " for a new password" : "");
        s_restart = false;
        s_retry_us = 0;
    }
    if (s_on || !wanted() || !wifi_up() || esp_timer_get_time() < s_retry_us) return;

    uint8_t mac[6] = { 0 };
    (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    char name[sizeof s_name];
    portalweb_ap_name(mac, name, sizeof name);
    char pass[16];
    settings_hotspot_pass(pass, sizeof pass);
    const esp_err_t err = wifi_ap_begin_secure(name, pass);
    memset(pass, 0, sizeof pass);
    snprintf(s_name, sizeof s_name, "%s", name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hotspot %s: not started (%s); trying again in 10 s", name, esp_err_to_name(err));
        s_retry_us = esp_timer_get_time() + RETRY_US;
        return;
    }
    s_on = true;
    char ip[16] = "";
    (void)wifi_ap_ip(ip, sizeof ip);
    ESP_LOGI(TAG, "hotspot up: %s, at %s", name, ip[0] ? ip : "(address coming)");
}

bool hotspot_running(void) { return s_on; }

void hotspot_name(char *out, size_t out_size)
{
    if (out && out_size) snprintf(out, out_size, "%s", s_name);
}

bool hotspot_ip(char *out, size_t out_size)
{
    if (!s_on) {
        if (out && out_size) out[0] = '\0';
        return false;
    }
    return wifi_ap_ip(out, out_size);
}

int hotspot_clients(void) { return s_on ? wifi_ap_clients() : 0; }

void hotspot_restart(void) { s_restart = true; }
