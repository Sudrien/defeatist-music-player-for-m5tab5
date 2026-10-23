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
#include "podcats.h"          /* 8014 */
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

/*
 * 8014: the categories, from the same categories/list that proves a key.
 * A try that works keeps what it fetched; at boot, with a key already
 * saved, the PODCAST tab asks for them (podindex_cats_fetch()). PSRAM,
 * allocated on the first list kept. s_cats_gen moves whenever the list
 * or its state does, so the chooser knows to rebuild its rows.
 */
static podcat_t          *s_cats;
static int                s_cats_n;
static podindex_cats_t    s_cats_state;
static uint32_t           s_cats_gen;
static bool               s_cats_busy;           /* a fetch task is running */

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

/* 8014: parse `body` into the list, under the mutex. False if it held
 * no categories. */
static bool keep_cats(const char *body, size_t got)
{
    podcat_t *tmp = heap_caps_malloc(PODCATS_MAX * sizeof(podcat_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return false;
    const int n = podcats_parse(body, got, tmp, PODCATS_MAX);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (n > 0) {
        podcat_t *old = s_cats;
        s_cats = tmp;
        s_cats_n = n;
        tmp = old;
    }
    s_cats_state = n > 0 ? PODINDEX_CATS_READY : PODINDEX_CATS_FAILED;
    s_cats_gen++;
    xSemaphoreGive(s_mu);
    free(tmp);
    ESP_LOGI(TAG, "%d categories", n);
    return n > 0;
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
                (void)keep_cats(body, got);         /* 8014: the same reply */
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

/* 8014: categories/list with the saved pair. The secret is read from
 * NVS here, on this task's internal stack, and nowhere else. */
static void cats_task(void *arg)
{
    (void)arg;
    static char key[PODAUTH_KEY_MAX], secret[PODAUTH_SECRET_MAX];
    bool ok = false;
    nvs_handle_t h;
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t kn = sizeof(key), sn = sizeof(secret);
        ok = nvs_get_str(h, NVS_KEY_KEY, key, &kn) == ESP_OK &&
             nvs_get_str(h, NVS_KEY_SECRET, secret, &sn) == ESP_OK;
        nvs_close(h);
    }
    char *body = ok ? heap_caps_malloc(PI_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    bool kept = false;
    if (body) {
        size_t got = 0;
        const int status = ask(key, secret, body, &got);
        if (status == 200 && podauth_reply_ok(body, got)) kept = keep_cats(body, got);
        else if (status) ESP_LOGW(TAG, "categories: HTTP %d", status);
        else             ESP_LOGW(TAG, "categories: no connection");
        free(body);
    } else {
        /* Two calls, for 8007's reason. */
        if (ok) ESP_LOGE(TAG, "no PSRAM for the categories");
        else    ESP_LOGE(TAG, "categories: no saved key");
    }
    memset(secret, 0, sizeof(secret));
    ESP_LOGI(TAG, "stack: %u bytes never used of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PI_STACK);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!kept) { s_cats_state = PODINDEX_CATS_FAILED; s_cats_gen++; }
    s_cats_busy = false;
    xSemaphoreGive(s_mu);
    vTaskDelete(NULL);
}

bool podindex_cats_fetch(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_state != PODINDEX_WORKING || s_cats_busy) {
        xSemaphoreGive(s_mu);
        return false;
    }
    s_cats_busy = true;
    s_cats_state = PODINDEX_CATS_LOADING;
    s_cats_gen++;
    xSemaphoreGive(s_mu);
    if (xTaskCreate(cats_task, "podindex", PI_STACK, NULL, 3, NULL) != pdPASS) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_cats_busy = false;
        s_cats_state = PODINDEX_CATS_FAILED;
        s_cats_gen++;
        xSemaphoreGive(s_mu);
        return false;
    }
    return true;
}

podindex_cats_t podindex_cats(podcat_t *out, int max, int *n, uint32_t *gen)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const podindex_cats_t st = s_cats_state;
    int k = 0;
    if (out && s_cats) {
        k = s_cats_n < max ? s_cats_n : max;
        memcpy(out, s_cats, (size_t)k * sizeof(podcat_t));
    }
    if (n) *n = k;
    if (gen) *gen = s_cats_gen;
    xSemaphoreGive(s_mu);
    return st;
}

uint32_t podindex_cats_gen(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const uint32_t g = s_cats_gen;
    xSemaphoreGive(s_mu);
    return g;
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
