/*
 * podindex.c -- see podindex.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "podindex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "nvsns.h"
#include "podauth.h"
#include "radiobrowser.h"       /* RADIOBROWSER_UA_FMT: the same name */
#include "settings.h"           /* settings_now(), settings_time_verified() */

static const char *TAG = "tab5_podindex";

/* In DEFEATIST_NVS_NS, beside nvsns.h's list. */
#define NVS_KEY_KEY     "pi_key"
#define NVS_KEY_SECRET  "pi_sec"

#define PI_URL          "https://api.podcastindex.org/api/1.0/categories/list"
#define PI_TIMEOUT_MS   10000
/* categories/list is about 5 KB. In PSRAM, for the length of one try. */
#define PI_BODY_MAX     (16 * 1024)
/*
 * esp_http_client and the TLS handshake run on this stack, and the task
 * writes NVS, so it is internal RAM. 8192 is ui_task's, which already
 * runs radiobrowser's HTTPS fetches; NOT measured for this task. The
 * high-water mark is logged at the end of every try so a board run
 * gives the number to size against.
 */
#define PI_STACK        8192

static SemaphoreHandle_t s_mu;
static podindex_state_t  s_state;
static int               s_http;
static char              s_key[PODAUTH_KEY_MAX];       /* the saved key, or the one being tried */
static char              s_secret[PODAUTH_SECRET_MAX]; /* only while CHECKING */

static void set(podindex_state_t st, int http)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_state = st;
    s_http = http;
    if (st != PODINDEX_WORKING && st != PODINDEX_CHECKING) s_key[0] = '\0';
    xSemaphoreGive(s_mu);
}

void podindex_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t kn = sizeof(s_key), sn = sizeof(s_secret);
    const bool have = nvs_get_str(h, NVS_KEY_KEY, s_key, &kn) == ESP_OK &&
                      nvs_get_str(h, NVS_KEY_SECRET, s_secret, &sn) == ESP_OK;
    nvs_close(h);
    memset(s_secret, 0, sizeof(s_secret));
    if (have) {
        s_state = PODINDEX_WORKING;
        ESP_LOGI(TAG, "an API key is saved");
    } else {
        s_key[0] = '\0';
    }
}

podindex_state_t podindex_state(char *key, size_t key_size, int *http)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const podindex_state_t st = s_state;
    if (key && key_size) snprintf(key, key_size, "%s", st == PODINDEX_WORKING ? s_key : "");
    if (http) *http = s_http;
    xSemaphoreGive(s_mu);
    return st;
}

/* One GET of categories/list. The status, or 0 for no connection; the
 * body (when 200) in `body`, `*got` bytes. */
static int ask(const char *key, const char *secret, char *body, size_t *got)
{
    char date[PODAUTH_DATE], hex[PODAUTH_HEX];
    if (!podauth_sign(key, secret, settings_now(), date, hex)) return 0;

    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT,
             desc && desc->version[0] ? desc->version : "0");
    esp_http_client_config_t cfg = {
        .url = PI_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = PI_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = ua,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;
    esp_http_client_set_header(c, "X-Auth-Key", key);
    esp_http_client_set_header(c, "X-Auth-Date", date);
    esp_http_client_set_header(c, "Authorization", hex);

    int status = 0;
    *got = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        while (status == 200 && *got + 1 < PI_BODY_MAX) {
            const int n = esp_http_client_read(c, body + *got, (int)(PI_BODY_MAX - *got - 1));
            if (n <= 0) break;
            *got += (size_t)n;
        }
        body[*got] = '\0';
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    memset(hex, 0, sizeof(hex));
    return status;
}

static bool save(const char *key, const char *secret)
{
    nvs_handle_t h;
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, NVS_KEY_KEY, key) == ESP_OK &&
                    nvs_set_str(h, NVS_KEY_SECRET, secret) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void try_task(void *arg)
{
    (void)arg;
    static char key[PODAUTH_KEY_MAX], secret[PODAUTH_SECRET_MAX];
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(key, s_key, sizeof(key));
    memcpy(secret, s_secret, sizeof(secret));
    memset(s_secret, 0, sizeof(s_secret));
    xSemaphoreGive(s_mu);

    char *body = heap_caps_malloc(PI_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    podindex_state_t st = PODINDEX_FAILED;
    int status = 0;
    if (!body) {
        ESP_LOGE(TAG, "no PSRAM for the reply");
    } else {
        size_t got = 0;
        status = ask(key, secret, body, &got);
        if (status == 200 && podauth_reply_ok(body, got)) {
            if (save(key, secret)) {
                st = PODINDEX_WORKING;
                ESP_LOGI(TAG, "the API key works; saved (%u bytes of categories)", (unsigned)got);
            } else {
                ESP_LOGE(TAG, "the API key works, but NVS would not take it");
            }
        } else if (status == 200) {
            st = PODINDEX_BAD_REPLY;
            ESP_LOGW(TAG, "a 200 without \"status\":\"true\" (%u bytes)", (unsigned)got);
        } else if (status == 401 || status == 403) {
            st = settings_time_verified() ? PODINDEX_REFUSED : PODINDEX_CLOCK;
            ESP_LOGW(TAG, "HTTP %d: refused%s", status,
                     st == PODINDEX_CLOCK ? "; the clock has not been set by NTP" : "");
        } else {
            st = PODINDEX_UNREACHABLE;
            /* 8007: two calls -- ESP_LOGW pastes its format into a
             * string literal, so it cannot be a ?: expression. */
            if (status) ESP_LOGW(TAG, "HTTP %d", status);
            else        ESP_LOGW(TAG, "no connection");
        }
        free(body);
    }
    memset(secret, 0, sizeof(secret));
    ESP_LOGI(TAG, "stack: %u bytes never used of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PI_STACK);
    set(st, st == PODINDEX_WORKING ? 0 : status);
    vTaskDelete(NULL);
}

podindex_try_t podindex_try(const char *key, const char *secret)
{
    if (!podauth_text_ok(key, PODAUTH_KEY_MAX) ||
        !podauth_text_ok(secret, PODAUTH_SECRET_MAX)) return PODINDEX_TRY_INVALID;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_state == PODINDEX_WORKING || s_state == PODINDEX_CHECKING) {
        const podindex_try_t r = s_state == PODINDEX_WORKING ? PODINDEX_TRY_LOCKED
                                                             : PODINDEX_TRY_BUSY;
        xSemaphoreGive(s_mu);
        return r;
    }
    snprintf(s_key, sizeof(s_key), "%s", key);
    snprintf(s_secret, sizeof(s_secret), "%s", secret);
    s_state = PODINDEX_CHECKING;
    s_http = 0;
    xSemaphoreGive(s_mu);

    if (xTaskCreate(try_task, "podindex", PI_STACK, NULL, 3, NULL) != pdPASS) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        memset(s_secret, 0, sizeof(s_secret));
        xSemaphoreGive(s_mu);
        set(PODINDEX_FAILED, 0);
        return PODINDEX_TRY_FAILED;
    }
    return PODINDEX_TRY_STARTED;
}
