/*
 * wifijoin.c -- see wifijoin.h. The body is portal.c's try_join() as it
 * was before 5122, less the portal's own state.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wifijoin.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "mbedtls/pkcs5.h"

#include "wifi.h"
#include "wifistore.h"

portalweb_net_t wifijoin_net(uint8_t auth)
{
    switch ((wifi_auth_mode_t)auth) {
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return PORTALWEB_NET_WPA3_CAPABLE;
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return PORTALWEB_NET_WPA2_ONLY;
    default:
        return PORTALWEB_NET_UNKNOWN;
    }
}

esp_err_t wifijoin_try(const char *TAG, const char *ssid, const char *pass,
                       portalweb_net_t net)
{
    const portalweb_check_t kind = portalweb_check(ssid, pass);
    const portalweb_plan_t plan = portalweb_join_plan(kind, net);
    if (plan == PORTALWEB_TRY_NONE) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "trying %.32s (%s)", ssid,
             plan == PORTALWEB_TRY_AS_TYPED        ? "PSK as typed" :
             plan == PORTALWEB_TRY_PASSPHRASE_ONLY ? "passphrase only: WPA3-capable network" :
             net  == PORTALWEB_NET_WPA2_ONLY       ? "PSK, then passphrase: WPA2 network" :
                                                     "PSK, then passphrase: not in the scan");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *stored = NULL;
    bool is_psk = false;
    char hex[WIFISTORE_SECRET_MAX + 1] = { 0 };

    if (plan == PORTALWEB_TRY_AS_TYPED) {
        err = wifi_join(ssid, pass, WIFI_JOIN_TIMEOUT_MS);
        stored = pass;
        is_psk = true;
    } else if (plan == PORTALWEB_TRY_PSK_THEN_PASSPHRASE) {
        uint8_t key[32];
        const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
            MBEDTLS_MD_SHA1, (const unsigned char *)pass, strlen(pass),
            (const unsigned char *)ssid, strlen(ssid), 4096, sizeof(key), key);
        if (rc == 0) {
            portalweb_hex(key, sizeof(key), hex);
            err = wifi_join(ssid, hex, WIFI_JOIN_TIMEOUT_MS);
            stored = hex;
            is_psk = true;
        } else {
            ESP_LOGW(TAG, "PBKDF2 failed (%d); trying the passphrase", rc);
            err = ESP_ERR_WIFI_PASSWORD;
        }
        memset(key, 0, sizeof(key));

        /* Refused, not absent: most likely an AP that insists on SAE,
         * which cannot use a precomputed key. */
        if (err == ESP_ERR_WIFI_PASSWORD) {
            err = wifi_join(ssid, pass, WIFI_JOIN_TIMEOUT_MS);
            stored = pass;
            is_psk = false;
        }
    } else if (plan == PORTALWEB_TRY_PASSPHRASE_ONLY) {
        err = wifi_join(ssid, pass, WIFI_JOIN_TIMEOUT_MS);
        stored = pass;
        is_psk = false;
    }

    if (err == ESP_OK) {
        const esp_err_t saved = wifistore_save(ssid, stored, is_psk);
        if (saved != ESP_OK) {
            ESP_LOGE(TAG, "joined but could not save: %s", esp_err_to_name(saved));
            err = saved;
        } else {
            ESP_LOGI(TAG, "saved %.32s as %s", ssid, is_psk ? "PSK" : "passphrase");
        }
    }
    memset(hex, 0, sizeof(hex));
    return err;
}
