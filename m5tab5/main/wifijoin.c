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
#include "mbedtls/build_info.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/pkcs5.h"
#endif

#include "wifi.h"
#include "wifistore.h"

/* The WPA2 PSK: PBKDF2-HMAC-SHA1, the SSID as salt, 4096 rounds. 6031:
 * Mbed TLS 4 (IDF 6.x) makes pkcs5.h private and does this through PSA
 * key derivation, which is what IDF's own supplicant calls there; 3.6
 * (IDF 5.x) keeps the call this file always made. 0 on success. */
static int wpa_psk(const char *pass, const char *ssid, uint8_t key[32])
{
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_1));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, 4096);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                            (const uint8_t *)ssid, strlen(ssid));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)pass, strlen(pass));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_output_bytes(&op, key, 32);
    psa_key_derivation_abort(&op);
    return (int)st;
#else
    return mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA1, (const unsigned char *)pass, strlen(pass),
        (const unsigned char *)ssid, strlen(ssid), 4096, 32, key);
#endif
}

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
        const int rc = wpa_psk(pass, ssid, key);
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
