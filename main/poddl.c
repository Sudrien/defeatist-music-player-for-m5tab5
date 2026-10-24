/*
 * poddl.c -- see poddl.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "poddl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "podpath.h"
#include "radiobrowser.h"       /* RADIOBROWSER_UA_FMT */
#include "storage_io.h"

static const char *TAG = "tab5_poddl";

#define DL_STACK      8192          /* TLS and FatFs; internal: it writes the card */
#define DL_CHUNK      (16 * 1024)   /* one lease's worth, storage_io.h's 16 KB */
#define DL_TIMEOUT_MS 15000
#define DL_HOPS       6             /* tracking prefixes stack */
#define DL_URL_MAX    1024
#define DL_PATH_MAX   400

static SemaphoreHandle_t s_mu;
static poddl_state_t     s_state;
static int64_t           s_id, s_got, s_total;
static uint32_t          s_gen;
static int               s_pct = -1;            /* last percent the generation moved for */
static char              s_url[DL_URL_MAX];
static char              s_dir[DL_PATH_MAX];
static char              s_base[PODPATH_NAME_MAX + 16];
static char              s_path[DL_PATH_MAX];   /* the finished file */

/* What the response headers said; filled by the event handler. */
typedef struct {
    char location[DL_URL_MAX];
    char ctype[64];
} hdrs_t;

static void lock(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mu, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mu); }

static esp_err_t on_event(esp_http_client_event_t *ev)
{
    hdrs_t *h = ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_HEADER && h && ev->header_key && ev->header_value) {
        if (!strcasecmp(ev->header_key, "Location"))
            snprintf(h->location, sizeof(h->location), "%s", ev->header_value);
        else if (!strcasecmp(ev->header_key, "Content-Type"))
            snprintf(h->ctype, sizeof(h->ctype), "%s", ev->header_value);
    }
    return ESP_OK;
}

static void set_progress(int64_t got)
{
    lock();
    s_got = got;
    const int pct = s_total > 0 ? (int)(got * 100 / s_total) : (int)(got >> 20);   /* MB when unknown */
    if (pct != s_pct) { s_pct = pct; s_gen++; }
    unlock();
}

static void finish(poddl_state_t st)
{
    lock();
    s_state = st;
    s_gen++;
    unlock();
}

static bool make_dirs(const char *dir)
{
    /* <mount>/Podcasts, then <mount>/Podcasts/<podcast>. */
    char p[DL_PATH_MAX];
    snprintf(p, sizeof(p), "%s", dir);
    char *slash = strrchr(p, '/');
    bool ok = true;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (slash) {
        *slash = '\0';
        if (mkdir(p, 0777) != 0 && errno != EEXIST) ok = false;
        *slash = '/';
    }
    if (ok && mkdir(p, 0777) != 0 && errno != EEXIST) ok = false;
    storage_io_release();
    if (!ok) ESP_LOGW(TAG, "could not make %s (errno %d)", p, errno);
    return ok;
}

/* Open `url`, following redirects; the client, positioned at the body,
 * with its status in *status, or NULL. `url` is rewritten with the
 * address that answered. */
static esp_http_client_handle_t open_followed(char *url, hdrs_t *h, int *status)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT, desc && desc->version[0] ? desc->version : "0");
    *status = 0;
    for (int hop = 0; hop <= DL_HOPS; hop++) {
        h->location[0] = h->ctype[0] = '\0';
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = DL_TIMEOUT_MS,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
            .user_agent = ua,
            .disable_auto_redirect = true,
            .event_handler = on_event,
            .user_data = h,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) return NULL;
        if (esp_http_client_open(c, 0) != ESP_OK) {
            ESP_LOGW(TAG, "no connection to %.80s", url);
            esp_http_client_cleanup(c);
            return NULL;
        }
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        const bool redirect = *status == 301 || *status == 302 || *status == 303 ||
                              *status == 307 || *status == 308;
        if (!redirect) return c;
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (!h->location[0]) return NULL;
        if (!strncmp(h->location, "http://", 7) || !strncmp(h->location, "https://", 8)) {
            snprintf(url, DL_URL_MAX, "%s", h->location);
        } else if (h->location[0] == '/') {
            /* Absolute path on the same host. */
            char *p = strstr(url, "://");
            p = p ? strchr(p + 3, '/') : NULL;
            const size_t keep = p ? (size_t)(p - url) : strlen(url);
            if (keep + strlen(h->location) + 1 > DL_URL_MAX) return NULL;
            memmove(url + keep, h->location, strlen(h->location) + 1);
        } else {
            ESP_LOGW(TAG, "a redirect to \"%.60s\" not followed", h->location);
            return NULL;
        }
        ESP_LOGI(TAG, "%d to %.100s", *status, url);
    }
    ESP_LOGW(TAG, "more than %d redirects", DL_HOPS);
    return NULL;
}

static void dl_task(void *arg)
{
    (void)arg;
    static char url[DL_URL_MAX], part[DL_PATH_MAX];
    static hdrs_t h;
    lock();
    snprintf(url, sizeof(url), "%s", s_url);
    unlock();

    poddl_state_t st = PODDL_FAILED;
    uint8_t *buf = heap_caps_malloc(DL_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_handle_t c = NULL;
    FILE *f = NULL;
    int status = 0;
    if (!buf || !make_dirs(s_dir)) goto out;
    c = open_followed(url, &h, &status);
    if (!c) goto out;
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto out;
    }

    char ext[8];
    podpath_url_ext(url, ext, sizeof(ext));
    if (!ext[0]) snprintf(ext, sizeof(ext), "%s", podpath_type_ext(h.ctype));
    if (!ext[0]) snprintf(ext, sizeof(ext), "mp3");
    lock();
    snprintf(s_path, sizeof(s_path), "%s/%s.%s", s_dir, s_base, ext);
    s_total = esp_http_client_get_content_length(c);
    if (s_total <= 0) s_total = -1;
    unlock();
    snprintf(part, sizeof(part), "%s.part", s_path);

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    f = storage_io_open(part, "wb");
    storage_io_release();
    if (!f) {
        ESP_LOGW(TAG, "could not create %s", part);
        goto out;
    }
    ESP_LOGI(TAG, "to %s (%lld bytes%s)", s_path, (long long)s_total,
             s_total < 0 ? ", length not given" : "");

    int64_t got = 0;
    bool ok = true;
    for (;;) {
        int n = esp_http_client_read(c, (char *)buf, DL_CHUNK);
        if (n < 0) { ok = false; ESP_LOGW(TAG, "read failed after %lld bytes", (long long)got); break; }
        if (n == 0) break;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        const size_t put = fwrite(buf, 1, (size_t)n, f);
        storage_io_release();
        if (put != (size_t)n) {
            ok = false;
            ESP_LOGW(TAG, "write failed after %lld bytes (errno %d)", (long long)got, errno);
            break;
        }
        got += n;
        set_progress(got);
    }
    if (ok && s_total > 0 && got != s_total) {
        ESP_LOGW(TAG, "ended at %lld of %lld bytes", (long long)got, (long long)s_total);
        ok = false;
    }
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (storage_io_close(f) != 0) ok = false;
    f = NULL;
    if (ok) {
        remove(s_path);                  /* a stale one, if any; rename will not replace */
        if (rename(part, s_path) != 0) {
            ESP_LOGW(TAG, "could not rename %s (errno %d)", part, errno);
            ok = false;
        }
    }
    if (!ok) remove(part);
    storage_io_release();
    if (ok) {
        st = PODDL_DONE;
        ESP_LOGI(TAG, "done: %lld bytes", (long long)got);
    }

out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    ESP_LOGI(TAG, "stack: %u bytes never used of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)DL_STACK);
    finish(st);
    vTaskDelete(NULL);
}

bool poddl_start(int64_t id, const char *url, const char *dir, const char *base)
{
    if (!url || !dir || !base) return false;
    lock();
    if (s_state == PODDL_RUNNING) { unlock(); return false; }
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    snprintf(s_base, sizeof(s_base), "%s", base);
    s_path[0] = '\0';
    s_id = id;
    s_got = 0;
    s_total = -1;
    s_pct = -1;
    s_state = PODDL_RUNNING;
    s_gen++;
    unlock();
    if (xTaskCreate(dl_task, "poddl", DL_STACK, NULL, 3, NULL) != pdPASS) {
        finish(PODDL_FAILED);
        return false;
    }
    ESP_LOGI(TAG, "episode %lld: downloading", (long long)id);
    return true;
}

poddl_state_t poddl_state(int64_t *id, int64_t *got, int64_t *total,
                          char *path, size_t path_size, uint32_t *gen)
{
    lock();
    const poddl_state_t st = s_state;
    if (id) *id = s_id;
    if (got) *got = s_got;
    if (total) *total = s_total;
    if (path && path_size) snprintf(path, path_size, "%s", s_path);
    if (gen) *gen = s_gen;
    unlock();
    return st;
}

bool poddl_find(const char *dir, const char *base, char *out, size_t out_size)
{
    static const char *const exts[] = { "mp3", "m4a", "mp4", "aac", "ogg", "oga", "opus", "flac", "wav" };
    struct stat st;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        const int n = snprintf(out, out_size, "%s/%s.%s", dir, base, exts[i]);
        if (n <= 0 || (size_t)n >= out_size) return false;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    out[0] = '\0';
    return false;
}
