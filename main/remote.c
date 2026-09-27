/*
 * remote.c -- see remote.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "remote.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "ethernet.h"
#include "mediacache.h"
#include "remoteproto.h"
#include "waveform.h"
#include "wifi.h"

static const char *TAG = "tab5_remote";

/* The page, embedded by main/CMakeLists.txt (EMBED_TXTFILES, so it is
 * NUL-terminated and the NUL is not part of it). */
extern const char remote_html_start[] asm("_binary_remote_html_start");
extern const char remote_html_end[]   asm("_binary_remote_html_end");

/* Four sockets: a phone and a laptop with a page each, and room for the
 * page's own /art fetch beside its WebSocket. lru_purge drops the oldest
 * when a fifth arrives rather than refusing it. */
#define REMOTE_SOCKETS      (4)
/* Not the portal's 32768: the two servers can run at once. */
#define REMOTE_CTRL_PORT    (32770)
/* The position is sent at most this often when nothing else moved --
 * the page counts it, and this only corrects its drift. */
#define REMOTE_POS_EVERY_US (10 * 1000000LL)
/* How far the page's count may be from the player's before a state is
 * sent out of turn: a seek, a stall, a track that did not start. */
#define REMOTE_POS_SLIP_S   (2)
/* After a failed start, how long before trying again. */
#define REMOTE_RETRY_US     (5 * 1000000LL)

#define REMOTE_JSON_MAX     (1024)
#define REMOTE_WAVE_MAX     (2 * FRAMEWALK_MAX_COLUMNS + 64)

static httpd_handle_t    s_srv;
static QueueHandle_t     s_q;
static SemaphoreHandle_t s_mu;          /* s_json, s_wave, s_art_* */

/* The last of each, for a page that has just said hello. PSRAM: nothing
 * reads these fast, and internal RAM is what the httpd task needs. */
static char   *s_json;
static size_t  s_json_len;
static char   *s_wave;
static size_t  s_wave_len;
static char    s_art_path[512];
static char    s_art_key[12];

/* ui_task's own, not shared. */
static remote_state_t s_last;
static remote_state_t s_cur;
static int64_t        s_sent_us;
static uint32_t       s_wave_gen = UINT32_MAX;
static int64_t        s_retry_us;
/* PSRAM, from remote_init(): 3 KB of scratch is 3 KB of internal RAM
 * the httpd task's stack would rather have. */
static char          *s_build;          /* REMOTE_WAVE_MAX, the larger */
static uint8_t       *s_levels;         /* FRAMEWALK_MAX_COLUMNS */

/* ---- sending ----------------------------------------------------------- */

typedef struct {
    size_t len;
    char   data[];
} msg_t;

/* On the httpd task, which is the only one that may write to a socket. */
static void send_all_work(void *arg)
{
    msg_t *m = arg;
    httpd_handle_t srv = s_srv;
    if (srv) {
        int fds[REMOTE_SOCKETS];
        size_t n = REMOTE_SOCKETS;
        if (httpd_get_client_list(srv, &n, fds) == ESP_OK) {
            for (size_t i = 0; i < n; i++) {
                if (httpd_ws_get_fd_info(srv, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
                httpd_ws_frame_t f = {
                    .final = true,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)m->data,
                    .len = m->len,
                };
                (void)httpd_ws_send_frame_async(srv, fds[i], &f);
            }
        }
    }
    free(m);
}

static void send_all(const char *json, size_t len)
{
    if (!s_srv || !len) return;
    msg_t *m = heap_caps_malloc(sizeof(*m) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return;
    m->len = len;
    memcpy(m->data, json, len);
    if (httpd_queue_work(s_srv, send_all_work, m) != ESP_OK) free(m);
}

/* Keep a copy for hello, then send. */
static void keep_and_send(char *keep, size_t *keep_len, size_t cap,
                          const char *json, size_t len)
{
    if (len >= cap) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(keep, json, len + 1);
    *keep_len = len;
    xSemaphoreGive(s_mu);
    send_all(json, len);
}

/* ---- handlers ---------------------------------------------------------- */

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, remote_html_start,
                           (ssize_t)(remote_html_end - remote_html_start - 1));
}

/*
 * The cover, as the file carried it. `k` must name the cover that is
 * current: the page caches /art?k=... forever, so serving a different
 * picture under an old key would stick.
 */
static esp_err_t h_art(httpd_req_t *req)
{
    char q[48] = "", k[16] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        (void)httpd_query_key_value(q, "k", k, sizeof(k));
    }

    char path[sizeof(s_art_path)];
    bool match;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    match = s_art_key[0] && strcmp(k, s_art_key) == 0;
    memcpy(path, s_art_path, sizeof(path));
    xSemaphoreGive(s_mu);

    size_t len = 0;
    uint8_t *img = match ? mediacache_art_dup(path, &len) : NULL;
    if (!img) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }
    const bool png = len > 8 && memcmp(img, "\x89PNG\r\n\x1a\n", 8) == 0;
    httpd_resp_set_type(req, png ? "image/png" : "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    const esp_err_t err = httpd_resp_send(req, (const char *)img, (ssize_t)len);
    free(img);
    return err;
}

/* One reply to one socket, from its own handler. */
static void send_one(httpd_req_t *req, const char *keep, const size_t *keep_len)
{
    char *copy = NULL;
    size_t len;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    len = *keep_len;
    if (len) {
        copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) memcpy(copy, keep, len);
    }
    xSemaphoreGive(s_mu);
    if (!copy) return;
    httpd_ws_frame_t f = {
        .final = true, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)copy, .len = len,
    };
    (void)httpd_ws_send_frame(req, &f);
    free(copy);
}

static esp_err_t h_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "page connected (socket %d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);     /* the length only */
    if (err != ESP_OK) return err;
    /*
     * Anything that is not a short text frame is not a command, and the
     * socket is dropped rather than drained: reading an arbitrary
     * payload to throw it away is the one thing a peer sending one wants.
     */
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len > REMOTEPROTO_CMD_MAX) return ESP_FAIL;

    uint8_t buf[REMOTEPROTO_CMD_MAX + 1];
    f.payload = buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err != ESP_OK) return err;

    remote_cmd_t c;
    if (!remoteproto_parse((const char *)buf, f.len, &c)) {
        ESP_LOGW(TAG, "not a command (%u bytes); ignored", (unsigned)f.len);
        return ESP_OK;
    }
    if (c.kind == REMOTE_CMD_HELLO) {
        send_one(req, s_json, &s_json_len);
        send_one(req, s_wave, &s_wave_len);
        return ESP_OK;
    }
    /* Dropped rather than waited for when full: eight presses queued in
     * one ui_task pass is a script, not a hand. */
    (void)xQueueSend(s_q, &c, 0);
    return ESP_OK;
}

/* ---- the server -------------------------------------------------------- */

static bool have_ip(char *ip, size_t n)
{
    return wifi_sta_ip(ip, n) || ethernet_ip(ip, n);
}

static void start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = REMOTE_PORT;
    cfg.ctrl_port = REMOTE_CTRL_PORT;
    cfg.max_open_sockets = REMOTE_SOCKETS;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        s_srv = NULL;
        s_retry_us = esp_timer_get_time() + REMOTE_RETRY_US;
        ESP_LOGW(TAG, "could not start on port %d; trying again in 5 s", REMOTE_PORT);
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",    .method = HTTP_GET, .handler = h_page },
        { .uri = "/art", .method = HTTP_GET, .handler = h_art  },
        { .uri = "/ws",  .method = HTTP_GET, .handler = h_ws, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_srv, &uris[i]);
    }
    char url[48];
    if (remote_url(url, sizeof(url))) ESP_LOGI(TAG, "up at %s", url);
    /* Everything is new to whoever connects next. */
    memset(&s_last, 0xFF, sizeof(s_last));
    s_wave_gen = UINT32_MAX;
}

static void stop(const char *why)
{
    if (!s_srv) return;
    httpd_stop(s_srv);
    s_srv = NULL;
    ESP_LOGI(TAG, "down: %s", why);
}

void remote_init(void)
{
    if (s_q) return;
    s_q = xQueueCreate(8, sizeof(remote_cmd_t));
    s_mu = xSemaphoreCreateMutex();
    s_json = heap_caps_calloc(1, REMOTE_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wave = heap_caps_calloc(1, REMOTE_WAVE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_build = heap_caps_calloc(1, REMOTE_WAVE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_levels = heap_caps_calloc(1, FRAMEWALK_MAX_COLUMNS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void remote_poll(bool want)
{
    if (!s_q || !s_mu || !s_json || !s_wave || !s_build || !s_levels) return;
    char ip[20];
    const bool net = have_ip(ip, sizeof(ip));
    if (s_srv && !want) stop("switched off");
    else if (s_srv && !net) stop("no network");
    else if (!s_srv && want && net && esp_timer_get_time() >= s_retry_us) start();
}

bool remote_running(void) { return s_srv != NULL; }

bool remote_url(char *out, size_t out_size)
{
    char ip[20];
    if (!out || !out_size || !have_ip(ip, sizeof(ip))) return false;
    snprintf(out, out_size, "http://%s:%d/", ip, REMOTE_PORT);
    return true;
}

/* ---- state -------------------------------------------------------------- */

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void remote_publish(const ui_state_t *st, const char *art_path, int rec_count)
{
    if (!s_srv || !st) return;

    /* The cover's key and path, for /art. */
    char key[sizeof(s_art_key)] = "";
    if (art_path && art_path[0]) {
        const uint32_t h = mediacache_art_hash(art_path);
        if (h) snprintf(key, sizeof(key), "%08" PRIx32, h);
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (strcmp(key, s_art_key) != 0 || strcmp(art_path ? art_path : "", s_art_path) != 0) {
        memcpy(s_art_key, key, sizeof(s_art_key));
        copy_str(s_art_path, sizeof(s_art_path), art_path);
    }
    xSemaphoreGive(s_mu);

    /* The envelope, when it is a new one. */
    const uint32_t gen = waveform_gen();
    if (gen != s_wave_gen) {
        s_wave_gen = gen;
        const int n = waveform_levels(s_levels, FRAMEWALK_MAX_COLUMNS);
        const size_t len = remoteproto_wave_json(s_levels, n, gen, s_build, REMOTE_WAVE_MAX);
        if (len) keep_and_send(s_wave, &s_wave_len, REMOTE_WAVE_MAX, s_build, len);
    }

    remote_state_t *c = &s_cur;
    memset(c, 0, sizeof(*c));
    copy_str(c->title, sizeof(c->title), st->title);
    copy_str(c->artist, sizeof(c->artist), st->artist);
    copy_str(c->album, sizeof(c->album), st->album);
    memcpy(c->art, key, sizeof(c->art));
    c->pos_sec = st->pos_sec;
    c->len_sec = st->len_sec;
    c->stats_valid = st->stats_valid;
    c->playing = st->playing;
    c->can_seek = st->can_seek;
    c->has_next = st->has_next;
    c->volume = st->volume;
    c->muted = st->muted;
    c->fav = (int)st->fav;
    c->recording = st->recording;
    c->rec_count = rec_count;
    c->rec_ok = st->rec_ok;
    c->batt_pct = st->battery_pct;
    c->charging = st->battery_charging;
    c->wave = gen;

    /* Did anything but the position move? */
    const uint32_t pos = c->pos_sec;
    c->pos_sec = s_last.pos_sec;
    bool send = memcmp(c, &s_last, sizeof(*c)) != 0;
    c->pos_sec = pos;

    const int64_t now = esp_timer_get_time();
    if (!send) {
        if (!c->playing) {
            send = pos != s_last.pos_sec;               /* a seek while paused */
        } else {
            const int64_t expect = (int64_t)s_last.pos_sec + (now - s_sent_us) / 1000000;
            const int64_t slip = (int64_t)pos - expect;
            send = slip > REMOTE_POS_SLIP_S || slip < -REMOTE_POS_SLIP_S ||
                   now - s_sent_us >= REMOTE_POS_EVERY_US;
        }
    }
    if (!send) return;

    const size_t len = remoteproto_state_json(c, s_build, REMOTE_JSON_MAX);
    if (!len) return;
    s_last = *c;
    s_sent_us = now;
    keep_and_send(s_json, &s_json_len, REMOTE_JSON_MAX, s_build, len);
}

/* ---- presses ----------------------------------------------------------- */

bool remote_take(ui_action_t *out)
{
    if (!s_q || !out) return false;
    remote_cmd_t c;
    if (xQueueReceive(s_q, &c, 0) != pdTRUE) return false;

    out->value = 0;
    switch (c.kind) {
    case REMOTE_CMD_PLAY:   out->kind = UI_ACTION_PLAY;     break;
    case REMOTE_CMD_PAUSE:  out->kind = UI_ACTION_PAUSE;    break;
    case REMOTE_CMD_NEXT:   out->kind = UI_ACTION_NEXT;     break;
    case REMOTE_CMD_PREV:   out->kind = UI_ACTION_PREV;     break;
    case REMOTE_CMD_STAR:   out->kind = UI_ACTION_FAVORITE; break;
    case REMOTE_CMD_VOLUME: out->kind = UI_ACTION_VOLUME; out->value = c.value; break;
    case REMOTE_CMD_SEEK:   out->kind = UI_ACTION_SEEK;   out->value = c.value; break;
    default:                return false;
    }
    return true;
}
