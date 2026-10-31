/*
 * devcert.c -- see devcert.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nvsns.h"              /* 6006 */
#include "devcert.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "certgen.h"
#include "portal.h"         /* PORTAL_SSID_MAX */
#include "portalweb.h"      /* portalweb_ap_name() */

static const char *TAG = "tab5_cert";

#define DEVCERT_NS      DEFEATIST_NVS_NS     /* 6006: was "devcert" */
#define KEY_CAP         (512)
#define CRT_CAP         (1400)

static char  *s_key, *s_crt;        /* PSRAM, once loaded */
static size_t s_key_len, s_crt_len;
static char   s_fp[96];

static int hw_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static bool load(void)
{
    nvs_handle_t h;
    if (nvs_open(DEVCERT_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t kl = KEY_CAP, cl = CRT_CAP;
    const bool ok = nvs_get_str(h, "key", s_key, &kl) == ESP_OK &&
                    nvs_get_str(h, "crt", s_crt, &cl) == ESP_OK;
    nvs_close(h);
    if (!ok) return false;
    s_key_len = kl;         /* nvs_get_str counts the NUL */
    s_crt_len = cl;
    return true;
}

static bool store(void)
{
    nvs_handle_t h;
    if (nvs_open(DEVCERT_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, "key", s_key) == ESP_OK &&
                    nvs_set_str(h, "crt", s_crt) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool devcert_get(const char **crt, size_t *crt_len,
                 const char **key, size_t *key_len)
{
    if (!s_crt_len) {
        if (!s_key) s_key = heap_caps_calloc(1, KEY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_crt) s_crt = heap_caps_calloc(1, CRT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_key || !s_crt) return false;

        if (!load()) {
            /* The same name the setup network carries, so the two read
             * as one device. The radio's MAC when there is a radio; the
             * chip's own when there is only a cable. */
            uint8_t mac[6] = { 0 };
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) esp_efuse_mac_get_default(mac);
            char name[PORTAL_SSID_MAX + 1];
            portalweb_ap_name(mac, name, sizeof(name));

            const int64_t t0 = esp_timer_get_time();
            const int ret = certgen_make(name, hw_rng, NULL, s_key, KEY_CAP, s_crt, CRT_CAP);
            if (ret != 0) {
                ESP_LOGE(TAG, "could not make a certificate: -0x%04x", (unsigned)-ret);
                return false;
            }
            s_key_len = strlen(s_key) + 1;
            s_crt_len = strlen(s_crt) + 1;
            ESP_LOGI(TAG, "made a certificate for %s in %lld ms%s", name,
                     (long long)((esp_timer_get_time() - t0) / 1000),
                     store() ? "" : " (NOT kept: NVS refused it; a new one next boot)");
        }

        unsigned char fp[32];
        if (certgen_fingerprint(s_crt, fp) == 0) {
            for (int i = 0; i < 32; i++) {
                snprintf(s_fp + i * 3, sizeof(s_fp) - (size_t)i * 3, i < 31 ? "%02X:" : "%02X", fp[i]);
            }
            ESP_LOGI(TAG, "SHA-256 %s", s_fp);
        }
    }
    if (crt) *crt = s_crt;
    if (crt_len) *crt_len = s_crt_len;
    if (key) *key = s_key;
    if (key_len) *key_len = s_key_len;
    return true;
}

bool devcert_fingerprint(char *out, size_t out_size)
{
    if (!s_fp[0] || !out || out_size < sizeof(s_fp)) return false;
    memcpy(out, s_fp, sizeof(s_fp));
    return true;
}
