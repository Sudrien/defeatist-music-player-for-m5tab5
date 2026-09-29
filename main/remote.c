/*
 * remote.c -- see remote.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "heapmap.h"
#include "remote.h"

#include <dirent.h>
#include <inttypes.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"          /* ESP_ERR_WIFI_PASSWORD */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "cuedir.h"
#include "decoder.h"
#include "devcert.h"
#include "ethernet.h"
#include "mediacache.h"
#include "portal.h"
#include "portalweb.h"
#include "stationlist.h"
#include "stations.h"
#include "storage.h"
#include "storage_io.h"
#include "remoteproto.h"
#include "uireq.h"            /* MPD.md step 5 */
#include "mpdqueue.h"         /* 5174 */
#include "playlist.h"         /* 5174 */
#include "waveform.h"
#include "wifijoin.h"
#include "wifistore.h"
#include "wifi.h"

static const char *TAG = "tab5_remote";

/* The page, embedded by main/CMakeLists.txt (EMBED_TXTFILES, so it is
 * NUL-terminated and the NUL is not part of it). */
extern const char remote_html_start[] asm("_binary_remote_html_start");
extern const char remote_html_end[]   asm("_binary_remote_html_end");
/* 5119: a white disc with the switch's green play arrow, 16/32/48 px.
 * EMBED_FILES, so no NUL is added and the length is end - start. */
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

/* Four sockets: a phone and a laptop with a page each, and room for the
 * page's own /art fetch beside its WebSocket. lru_purge drops the oldest
 * when a fifth arrives rather than refusing it. */
#define REMOTE_SOCKETS      (4)

/* 5157: this number is one term of a global budget, and the ceiling it
 * counts against is checked there rather than here -- an httpd costs
 * REMOTE_SOCKETS + 3 and nothing in IDF validates the sum across
 * servers. Included so that raising it fails the build if it no longer
 * fits, for the reason NETDEC_MIN_STACK stopped living inside an
 * xTaskCreate call. */
#include "netbudget.h"
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

#define REMOTE_JSON_MAX     (4096)   /* 5123: a 512-byte path, escaped */
#define REMOTE_WAVE_MAX     (2 * FRAMEWALK_MAX_COLUMNS + 64)

static httpd_handle_t    s_srv;         /* 5121: HTTPS, port 443 */
static httpd_handle_t    s_plain;       /* 5121: port 80, a redirect only */
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
static char          *s_build;          /* the larger of REMOTE_WAVE_MAX and _JSON_MAX */
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

static esp_err_t h_icon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)favicon_ico_start,
                           (ssize_t)(favicon_ico_end - favicon_ico_start));
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

/* ---- Wi-Fi (5122) ------------------------------------------------------ */

/*
 * Scanning and joining from the page. Both block for seconds -- a scan
 * about four, a join up to thirty -- so they run on a worker task made
 * for the one job and gone after it, never on the httpd task, which
 * would stop answering the page (and the WebSocket) while it waited.
 * The page polls GET /wifi for the result.
 *
 * The join is wifijoin_try(), the portal's own: tried before it is
 * saved, PSK or passphrase by what the scan saw. It only runs over the
 * encrypted connection 5121 made -- there is no plain-HTTP path to it.
 *
 * Refused while the setup portal runs: that is the other page that can
 * join, and two joins on one radio at once is a race nobody can read.
 */
typedef enum { W_IDLE = 0, W_SCANNING, W_TRYING, W_JOINED, W_FAILED } wstate_t;

#define W_SEEN_MAX  (32)

static volatile wstate_t s_w_state;
static volatile bool     s_w_busy;          /* a worker exists */
static char              s_w_ssid[WIFISTORE_SSID_MAX + 1];
static char              s_w_msg[160];
static wifi_seen_t      *s_w_seen;          /* PSRAM, W_SEEN_MAX */
static int               s_w_seen_n;
static char              s_w_pass[WIFISTORE_SECRET_MAX + 1];

static void w_set(wstate_t st, const char *msg)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_w_state = st;
    snprintf(s_w_msg, sizeof(s_w_msg), "%s", msg ? msg : "");
    xSemaphoreGive(s_mu);
}

static void w_scan(void)
{
    static wifi_seen_t raw[64];         /* 2.4 KB: static, not the worker's stack */
    const int n = wifi_scan_list(raw, 64);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_w_seen_n = 0;
    for (int i = 0; i < n && s_w_seen_n < W_SEEN_MAX; i++) {
        if (raw[i].hidden) continue;
        bool dup = false;
        for (int k = 0; k < s_w_seen_n; k++) {
            if (strcmp(s_w_seen[k].ssid, raw[i].ssid) == 0) { dup = true; break; }
        }
        if (!dup) s_w_seen[s_w_seen_n++] = raw[i];
    }
    xSemaphoreGive(s_mu);
    if (n < 0) w_set(W_IDLE, "Could not scan. Is Wi-Fi on, on the player?");
    else       w_set(W_IDLE, "");
}

static void w_join(void)
{
    char ssid[sizeof(s_w_ssid)], pass[sizeof(s_w_pass)];
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(ssid, s_w_ssid, sizeof(ssid));
    memcpy(pass, s_w_pass, sizeof(pass));
    memset(s_w_pass, 0, sizeof(s_w_pass));
    portalweb_net_t net = PORTALWEB_NET_UNKNOWN;
    for (int i = 0; i < s_w_seen_n; i++) {
        if (strcmp(s_w_seen[i].ssid, ssid) == 0) { net = wifijoin_net(s_w_seen[i].auth); break; }
    }
    xSemaphoreGive(s_mu);

    const esp_err_t err = wifijoin_try(TAG, ssid, pass, net);
    memset(pass, 0, sizeof(pass));

    char msg[160];
    switch (err) {
    case ESP_OK:
        snprintf(msg, sizeof(msg), "Joined and saved %.32s.", ssid); break;
    case ESP_ERR_NOT_FOUND:
        snprintf(msg, sizeof(msg), "%.32s was not found. Is it in range?", ssid); break;
    case ESP_ERR_WIFI_PASSWORD:
        snprintf(msg, sizeof(msg), "%.32s refused the password.", ssid); break;
    case ESP_ERR_TIMEOUT:
        snprintf(msg, sizeof(msg), "%.32s did not answer.", ssid); break;
    case ESP_ERR_INVALID_STATE:
        snprintf(msg, sizeof(msg), "Wi-Fi is off on the player."); break;
    default:
        snprintf(msg, sizeof(msg), "Could not join %.32s (%s).", ssid, esp_err_to_name(err)); break;
    }
    w_set(err == ESP_OK ? W_JOINED : W_FAILED, msg);
}

static void w_task(void *arg)
{
    if ((intptr_t)arg == W_SCANNING) w_scan();
    else                             w_join();
    s_w_busy = false;
    vTaskDelete(NULL);
}

static bool w_start(wstate_t job, const char *msg)
{
    if (s_w_busy) return false;
    s_w_busy = true;
    w_set(job, msg);
    /* 6 KB: PBKDF2 and wifi_join()'s waits, nothing large -- the scan's
     * buffer is static above. Internal RAM for as long as the job runs. */
    if (xTaskCreate(w_task, "remote_wifi", 6144, (void *)(intptr_t)job, 3, NULL) != pdPASS) {
        s_w_busy = false;
        w_set(W_FAILED, "The player could not start that. Try again.");
        return false;
    }
    return true;
}

/* Whether this request arrived on the cable -- the one case where a join
 * does not cut the page off from the answer. */
static bool on_cable(httpd_req_t *req)
{
    char eth[20];
    if (!ethernet_ip(eth, sizeof(eth))) return false;
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &al) != 0) return false;
    char mine[48] = "";
    if (a.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, mine, sizeof(mine));
    } else if (a.ss_family == AF_INET6) {
        /* IPv4-mapped (::ffff:a.b.c.d) is how an IPv4 peer arrives on a
         * dual-stack socket. */
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&a;
        inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], mine, sizeof(mine));
    }
    return strcmp(mine, eth) == 0;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    static char esc[WIFISTORE_SSID_MAX * 6 + 4];
    static char line[WIFISTORE_SSID_MAX * 6 + 64];
    static const char *const names[] = { "idle", "scanning", "trying", "joined", "failed" };

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char cur[WIFISTORE_SSID_MAX + 1] = "";
    (void)wifi_sta_ssid(cur, sizeof(cur));
    remoteproto_json_str(cur, esc, sizeof(esc));
    snprintf(line, sizeof(line), "{\"current\":%s,\"cable\":%s,\"setup\":%s,\"state\":\"%s\",\"msg\":",
             esc, on_cable(req) ? "true" : "false", portal_running() ? "true" : "false",
             names[s_w_state]);
    httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    remoteproto_json_str(s_w_msg, line, sizeof(line));
    xSemaphoreGive(s_mu);
    httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, ",\"nets\":[", HTTPD_RESP_USE_STRLEN);

    for (int i = 0; ; i++) {
        wifi_seen_t w;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        const bool more = i < s_w_seen_n;
        if (more) w = s_w_seen[i];
        xSemaphoreGive(s_mu);
        if (!more) break;
        remoteproto_json_str(w.ssid, esc, sizeof(esc));
        snprintf(line, sizeof(line), "%s{\"s\":%s,\"a\":\"%s\",\"r\":%d}",
                 i ? "," : "", esc, wifi_auth_name(w.auth), w.rssi);
        httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    if (portal_running()) w_set(W_IDLE, "Network setup is running on the player; use that page.");
    else if (!w_start(W_SCANNING, "Scanning…")) w_set(s_w_state, "Busy; try again in a moment.");
    return h_wifi(req);
}

static esp_err_t h_wifi_join(httpd_req_t *req)
{
    static char body[512];
    static char chosen[129], typed[129], ssid[129];
    static char pass[WIFISTORE_SECRET_MAX + 1];

    if (portal_running()) {
        w_set(W_IDLE, "Network setup is running on the player; use that page.");
        return h_wifi(req);
    }
    if (s_w_busy) {
        w_set(s_w_state, "Busy; try again in a moment.");
        return h_wifi(req);
    }
    if (req->content_len == 0 || req->content_len > sizeof(body)) {
        w_set(W_FAILED, "That form was too large to be a network name and a password.");
        return h_wifi(req);
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { memset(body, 0, sizeof(body)); return ESP_FAIL; }
        got += (size_t)n;
    }
    /* The portal's h_join() fields and checks, so the two forms refuse
     * the same things in the same words. */
    if (!portalweb_field(body, got, "ssid", chosen, sizeof(chosen))) chosen[0] = '\0';
    if (!portalweb_field(body, got, "ssid_other", typed, sizeof(typed))) typed[0] = '\0';
    snprintf(ssid, sizeof(ssid), "%s", portalweb_pick_ssid(chosen, typed));
    const bool have_pass = portalweb_field(body, got, "pass", pass, sizeof(pass));
    memset(body, 0, sizeof(body));

    const portalweb_check_t c = (ssid[0] && have_pass) ? portalweb_check(ssid, pass)
                              : (ssid[0] ? PORTALWEB_BAD_SECRET : PORTALWEB_BAD_SSID);
    const char *problem =
        c == PORTALWEB_BAD_SSID   ? "A network name is 1 to 32 characters." :
        c == PORTALWEB_NO_SECRET  ? "Open networks are not supported yet." :
        c == PORTALWEB_BAD_SECRET ? "A Wi-Fi password is 8 to 63 characters, or 64 hex digits." :
        c == PORTALWEB_NON_ASCII  ? "The password has a character a Wi-Fi password cannot "
                                    "have -- often a phone's smart punctuation. Retype it "
                                    "with that turned off." : NULL;
    if (problem) {
        memset(pass, 0, sizeof(pass));
        w_set(W_FAILED, problem);
        return h_wifi(req);
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_w_ssid, sizeof(s_w_ssid), "%.32s", ssid);
    memcpy(s_w_pass, pass, sizeof(s_w_pass));
    xSemaphoreGive(s_mu);
    memset(pass, 0, sizeof(pass));

    char msg[96];
    snprintf(msg, sizeof(msg), "Trying %.32s…", ssid);
    if (!w_start(W_TRYING, msg)) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        memset(s_w_pass, 0, sizeof(s_w_pass));
        xSemaphoreGive(s_mu);
    }
    return h_wifi(req);
}

/* ---- stations (5120) --------------------------------------------------- */

/*
 * The list, as {"max":N,"list":["name",...]} -- names only, as the
 * portal's page shows them. Chunked, a name at a time, so nothing the
 * size of the list is held anywhere.
 */
static esp_err_t h_stations(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[48];
    snprintf(head, sizeof(head), "{\"max\":%d,\"list\":[", STATIONLIST_MAX);
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);

    /* Static: a station_t is 576 bytes and the escaped name up to 6x 64,
     * and this runs on the httpd task's 6 KB stack. One handler runs at
     * a time on that task, so one copy serves. */
    static station_t st;
    static char esc[STATION_NAME_MAX * 6 + 4];
    const int n = stations_count();
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!stations_get(i, &st)) continue;
        if (!remoteproto_json_str(st.name, esc, sizeof(esc))) continue;
        if (!first) httpd_resp_send_chunk(req, ",", 1);
        httpd_resp_send_chunk(req, esc, HTTPD_RESP_USE_STRLEN);
        first = false;
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t station_reply(httpd_req_t *req, bool ok, const char *msg)
{
    static char esc[256 * 6 + 4];
    if (!remoteproto_json_str(msg, esc, sizeof(esc))) snprintf(esc, sizeof(esc), "\"\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, ok ? "{\"ok\":true,\"msg\":" : "{\"ok\":false,\"msg\":",
                          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, esc, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "}", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/*
 * The portal's h_station(), answering in JSON for the page's fetch()
 * rather than with a redrawn form. Same fields (name, url; form-encoded),
 * same validation from stationlist.h, same stations_append(), same
 * sentences -- so a station added here and one added through the portal
 * cannot be held to different rules.
 */
#define STATION_BODY_MAX    (1024)
static esp_err_t h_station_add(httpd_req_t *req)
{
    static char body[STATION_BODY_MAX];
    static char name[STATION_NAME_MAX + 16], url[STATION_URL_MAX + 16];

    if (req->content_len == 0 || req->content_len > STATION_BODY_MAX) {
        return station_reply(req, false, "That form was too large to be a name "
                                         "and a stream address.");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += (size_t)n;
    }
    if (!portalweb_field(body, got, "name", name, sizeof(name))) name[0] = '\0';
    const bool have_url = portalweb_field(body, got, "url", url, sizeof(url));
    station_trim(name);
    if (have_url) station_trim(url);

    if (!have_url || !url[0]) return station_reply(req, false, "That needs a stream address.");
    if (!station_url_writable(url)) {
        return station_reply(req, false, "That is not a usable stream address. It has "
                             "to start with http:// or https:// and be one "
                             "unbroken address.");
    }
    if (!station_name_ok(name)) {
        return station_reply(req, false, "That name cannot be used. Names are up to "
                             "63 characters and cannot start with a #.");
    }
    ESP_LOGI(TAG, "station submitted: %.63s <%.200s>", name[0] ? name : "(unnamed)", url);
    if (!stations_append(name, url)) {
        return station_reply(req, false, "The station could not be saved. The card "
                             "may be full, absent, or write-protected, or the "
                             "list may already be full.");
    }
    return station_reply(req, true, "Station added.");
}

/* ---- the file chooser (5123) ------------------------------------------ */

/*
 * A folder, listed the way the device's chooser lists it -- same
 * filters, same cue view, same order -- and sent to the one page that
 * asked, over its WebSocket. The WebSocket rather than a GET: it is
 * already open, and on this device a new HTTPS connection is a TLS
 * handshake measured at 0.8 s. The shared-prefix elision the chooser
 * does is the page's job; the names go out whole, as they are opened.
 *
 * Read on the httpd task, which the page is waiting on anyway; a
 * listing is a readdir and, for a folder with cue sheets, the sheets.
 * Everything it holds is PSRAM and freed before it returns.
 *
 * Sent in frames of about 24 KB, each a complete JSON object with its
 * rows' offset, so a 512-entry folder of long names is several frames
 * rather than one allocation sized for the worst case.
 */
#define LS_MAX_ROWS     (512)       /* the chooser's MAX_ENTRIES */
#define LS_FRAME        (24 * 1024)

typedef struct {
    char *name;
    char *label;        /* a cue track's title, or NULL */
    bool  dir;
} ls_row_t;

/* MPD.md step 5: a chosen path goes through uireq_open(), whose slot must
 * hold any path this parser accepts. */
_Static_assert(REMOTEPROTO_PATH_MAX <= UIREQ_PATH_MAX, "uireq's slot is shorter than a remote path");

static int ls_cmp(const void *a, const void *b)
{
    const ls_row_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static char *ps_strdup(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memcpy(p, s, n);
    return p;
}

static void ws_send_text(httpd_req_t *req, const char *s, size_t len)
{
    httpd_ws_frame_t f = {
        .final = true, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s, .len = len,
    };
    (void)httpd_ws_send_frame(req, &f);
}

static void send_listing(httpd_req_t *req, const char *p, size_t plen)
{
    static char path[REMOTEPROTO_PATH_MAX];
    static char esc[REMOTEPROTO_PATH_MAX * 6 + 4];
    static char esc2[256 * 6 + 4];
    memcpy(path, p, plen);
    path[plen] = '\0';
    remoteproto_json_str(path, esc, sizeof(esc));

    ls_row_t *rows = heap_caps_calloc(LS_MAX_ROWS, sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *out = heap_caps_malloc(LS_FRAME + 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    const char *error = NULL;

    if (!rows || !out) {
        error = "The player is out of memory for that.";
    } else if (strcmp(path, "/") == 0) {
        /* The volumes, as folders. */
        if (storage_present(STORAGE_SD))  { rows[n].name = ps_strdup("sd");  rows[n++].dir = true; }
        if (storage_present(STORAGE_USB)) { rows[n].name = ps_strdup("usb"); rows[n++].dir = true; }
    } else {
        DIR *d = opendir(path);
        if (!d) {
            error = "That folder could not be opened.";
        } else {
            cuedir_t *cues = cuedir_load(path, STORAGE_IO_BACKGROUND);
            struct dirent *e;
            while ((e = readdir(d)) != NULL && n < LS_MAX_ROWS) {
                const bool dir = (e->d_type == DT_DIR);
                if (storage_is_hidden(e->d_name)) continue;
                if (!dir && !decoder_supports(e->d_name)) continue;
                if (!dir && cuedir_hides(cues, e->d_name)) continue;
                if (!(rows[n].name = ps_strdup(e->d_name))) break;
                rows[n++].dir = dir;
            }
            closedir(d);
            for (int i = 0; i < cuedir_count(cues) && n < LS_MAX_ROWS; i++) {
                if (!(rows[n].name = ps_strdup(cuedir_name(cues, i)))) break;
                rows[n].label = ps_strdup(cuedir_label(cues, i));
                rows[n++].dir = false;
            }
            cuedir_free(cues);
            qsort(rows, (size_t)n, sizeof(*rows), ls_cmp);
        }
    }

    if (error || !out) {
        char msg[REMOTEPROTO_PATH_MAX * 6 + 160];
        const int m = snprintf(msg, sizeof(msg), "{\"t\":\"ls\",\"path\":%s,\"error\":\"%s\"}",
                               esc, error ? error : "The player is out of memory for that.");
        ws_send_text(req, msg, (size_t)m);
    } else {
        int i = 0;
        do {
            const int from = i;
            size_t len = (size_t)snprintf(out, LS_FRAME, "{\"t\":\"ls\",\"path\":%s,\"from\":%d,"
                                          "\"total\":%d,\"rows\":[", esc, from, n);
            for (; i < n && len < LS_FRAME; i++) {
                if (!remoteproto_json_str(rows[i].name, esc2, sizeof(esc2))) continue;
                len += (size_t)snprintf(out + len, LS_FRAME + 2048 - len, "%s{\"n\":%s,\"d\":%d",
                                        i > from ? "," : "", esc2, rows[i].dir ? 1 : 0);
                if (rows[i].label && remoteproto_json_str(rows[i].label, esc2, sizeof(esc2))) {
                    len += (size_t)snprintf(out + len, LS_FRAME + 2048 - len, ",\"l\":%s", esc2);
                }
                out[len++] = '}';
            }
            len += (size_t)snprintf(out + len, LS_FRAME + 2048 - len, "],\"done\":%s}",
                                    i >= n ? "true" : "false");
            ws_send_text(req, out, len);
        } while (i < n);
    }

    for (int k = 0; rows && k < n; k++) { free(rows[k].name); free(rows[k].label); }
    free(rows);
    free(out);
}

/* ---- the queue (5174) -------------------------------------------------- */

/*
 * The page is sent the queue -- ids and file names -- whenever it or the
 * playing position changes, and once on hello. The httpd task cannot read
 * mpdqueue.c, so ui_task copies it (under playlist_lock(), 5172) into one
 * of two snapshots and publishes it under s_mu, as mpd.c does for its
 * clients. A hello copies the published one under s_mu before framing it,
 * so a second publish during a slow send cannot change it underneath.
 *
 * Names, not paths: the page shows a name and sends back an id, so the
 * path never needs to cross. It keeps a 1024-entry queue of ordinary
 * names to tens of KB rather than hundreds.
 */
#define Q_FRAME         (LS_FRAME)

typedef struct {
    uint32_t  ver;
    int       cur;
    int       n;
    uint32_t *id;           /* MPDQ_MAX, PSRAM */
    uint32_t *off;          /* MPDQ_MAX, PSRAM: into arena */
    char     *arena;        /* the names, NUL-separated, PSRAM, grown */
    size_t    len, cap;
} qsnap_t;

static qsnap_t        s_qs[2];
static int            s_qpub = -1;      /* under s_mu: published, -1 none */
static bool           s_qsent;          /* ui_task's: s_qsent_* are good */
static uint32_t       s_qsent_ver;      /* ui_task's */
static int            s_qsent_cur;      /* ui_task's */
static char          *s_qframe;         /* ui_task's, Q_FRAME */
static remote_qrow_t *s_qrows;          /* ui_task's, MPDQ_MAX */

static bool qsnap_add(qsnap_t *q, uint32_t id, const char *name)
{
    const size_t need = strlen(name) + 1;
    if (q->len + need > q->cap) {
        size_t cap = q->cap ? q->cap : 16 * 1024;
        while (q->len + need > cap) cap *= 2;
        char *a = heap_caps_realloc(q->arena, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a) return false;
        q->arena = a;
        q->cap = cap;
    }
    q->id[q->n] = id;
    q->off[q->n] = (uint32_t)q->len;
    memcpy(q->arena + q->len, name, need);
    q->len += need;
    q->n++;
    return true;
}

/* Every frame of `q`, each handed to `emit`. `rows` holds MPDQ_MAX and
 * `buf` Q_FRAME + 1. */
static void q_frames(const qsnap_t *q, remote_qrow_t *rows, char *buf,
                     void (*emit)(void *ctx, const char *s, size_t len), void *ctx)
{
    for (int i = 0; i < q->n; i++) {
        rows[i].id = q->id[i];
        rows[i].name = q->arena + q->off[i];
    }
    int from = 0;
    do {
        int next = from;
        const size_t len = remoteproto_queue_frame(rows, q->n, from, q->ver, q->cur,
                                                   buf, Q_FRAME + 1, &next);
        if (!len) {
            /* One name longer than a frame: it cannot be, a name is at
             * most 255 bytes, 1.5 KB escaped. Skip it rather than stall. */
            ESP_LOGW(TAG, "queue entry %d does not fit a frame; not sent", from);
            from++;
            continue;
        }
        emit(ctx, buf, len);
        from = next;
    } while (from < q->n);
}

static void emit_all(void *ctx, const char *s, size_t len) { (void)ctx; send_all(s, len); }
static void emit_one(void *ctx, const char *s, size_t len) { ws_send_text(ctx, s, len); }

/* ui_task: republish when the queue or the playing position moved. */
static void queue_publish(void)
{
    if (!s_srv) { s_qsent = false; return; }
    const int w = s_qpub == 0 ? 1 : 0;      /* the one the httpd task cannot see */
    qsnap_t *q = &s_qs[w];
    if (!q->id || !q->off || !s_qframe || !s_qrows) return;

    playlist_lock();
    const uint32_t ver = mpdq_version();
    const int cur = playlist_current();
    if (s_qsent && ver == s_qsent_ver && cur == s_qsent_cur) { playlist_unlock(); return; }
    q->ver = ver;
    q->cur = cur;
    q->n = 0;
    q->len = 0;
    const int count = mpdq_count();
    for (int i = 0; i < count; i++) {
        const char *p = mpdq_path(i);
        const char *slash = p ? strrchr(p, '/') : NULL;
        if (!qsnap_add(q, mpdq_id(i), slash ? slash + 1 : (p ? p : ""))) {
            ESP_LOGW(TAG, "out of PSRAM copying the queue at %d of %d", i, count);
            break;
        }
    }
    playlist_unlock();

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_qpub = w;
    xSemaphoreGive(s_mu);
    s_qsent = true;
    s_qsent_ver = ver;
    s_qsent_cur = cur;
    q_frames(q, s_qrows, s_qframe, emit_all, NULL);
}

/* httpd task: the published queue, to one page. */
static void send_queue(httpd_req_t *req)
{
    qsnap_t c = { 0 };
    const uint32_t ps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    remote_qrow_t *rows = heap_caps_malloc(MPDQ_MAX * sizeof(*rows), ps);
    char *buf = heap_caps_malloc(Q_FRAME + 1, ps);
    bool have = false;
    if (rows && buf) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_qpub >= 0) {
            const qsnap_t *q = &s_qs[s_qpub];
            c = (qsnap_t){ .ver = q->ver, .cur = q->cur, .n = q->n, .len = q->len };
            c.id = heap_caps_malloc(MPDQ_MAX * sizeof(uint32_t), ps);
            c.off = heap_caps_malloc(MPDQ_MAX * sizeof(uint32_t), ps);
            c.arena = heap_caps_malloc(q->len ? q->len : 1, ps);
            if (c.id && c.off && c.arena) {
                memcpy(c.id, q->id, (size_t)q->n * sizeof(uint32_t));
                memcpy(c.off, q->off, (size_t)q->n * sizeof(uint32_t));
                memcpy(c.arena, q->arena, q->len);
                have = true;
            }
        }
        xSemaphoreGive(s_mu);
    }
    /* Nothing published yet (ui_task has not run since the server came
     * up) sends nothing: the publish that follows reaches this page too. */
    if (have) q_frames(&c, rows, buf, emit_one, req);
    free(c.id);
    free(c.off);
    free(c.arena);
    free(rows);
    free(buf);
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

    /* Static: up to 520 bytes since 5123's path commands, and this is
     * the httpd task's stack. One handler runs at a time on that task. */
    static uint8_t buf[REMOTEPROTO_CMD_MAX + 1];
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
        send_queue(req);                /* 5174 */
        return ESP_OK;
    }
    if (c.kind == REMOTE_CMD_LS) {
        send_listing(req, c.path, c.path_len);
        return ESP_OK;
    }
    if (c.kind == REMOTE_CMD_OPEN || c.kind == REMOTE_CMD_PLAYDIR) {
        /* One slot: a second choice before ui_task took the first
         * replaces it, as a second tap on the chooser would. The slot is
         * uireq's since MPD.md step 5. */
        (void)uireq_open(c.path, c.path_len, c.kind == REMOTE_CMD_PLAYDIR);
        return ESP_OK;
    }
    /* 5173: the queue's edits. Dropped when the page's room is full, as a
     * press is; the page learns what landed from the queue it is sent. */
    {
        /* 5175: an add's pos is where it goes, and the page's adds go at
         * the end; only qmove carries a position. */
        uireq_edit_t e = { .kind = UIREQ_EDIT_CLEAR, .id = (uint32_t)c.value,
                           .pos = c.kind == REMOTE_CMD_QMOVE ? c.value2 : -1 };
        bool is_edit = true;
        switch (c.kind) {
        case REMOTE_CMD_ADD:     e.kind = UIREQ_EDIT_ADD;      break;
        case REMOTE_CMD_ADDNEXT: e.kind = UIREQ_EDIT_ADD_NEXT; break;
        case REMOTE_CMD_QDEL:    e.kind = UIREQ_EDIT_DELETE;   break;
        case REMOTE_CMD_QMOVE:   e.kind = UIREQ_EDIT_MOVE;     break;
        case REMOTE_CMD_QCLEAR:  e.kind = UIREQ_EDIT_CLEAR;    break;
        default:                 is_edit = false;              break;
        }
        if (is_edit) {
            if (!uireq_edit(UIREQ_REMOTE, &e, c.path, c.path_len))
                ESP_LOGW(TAG, "queue edit dropped: the player has not caught up");
            return ESP_OK;
        }
    }
    /* Mapped here, where remote_take() used to on ui_task, so what is
     * queued is the press the panel would have made. */
    ui_action_t a = { .kind = UI_ACTION_NONE, .value = 0 };
    switch (c.kind) {
    case REMOTE_CMD_PLAY:   a.kind = UI_ACTION_PLAY;     break;
    case REMOTE_CMD_PAUSE:  a.kind = UI_ACTION_PAUSE;    break;
    case REMOTE_CMD_NEXT:   a.kind = UI_ACTION_NEXT;     break;
    case REMOTE_CMD_PREV:   a.kind = UI_ACTION_PREV;     break;
    case REMOTE_CMD_STAR:   a.kind = UI_ACTION_FAVORITE; break;
    case REMOTE_CMD_VOLUME: a.kind = UI_ACTION_VOLUME; a.value = c.value; break;
    case REMOTE_CMD_SEEK:   a.kind = UI_ACTION_SEEK;   a.value = c.value; break;
    case REMOTE_CMD_QPLAY:  a.kind = UI_ACTION_PLAY_ID; a.value = c.value; break;   /* 5173 */
    default:                return ESP_OK;
    }
    /* Dropped rather than waited for when full: eight presses queued in
     * one ui_task pass is a script, not a hand. */
    (void)uireq_press(UIREQ_REMOTE, &a);
    return ESP_OK;
}

/* ---- the server -------------------------------------------------------- */

static bool have_ip(char *ip, size_t n)
{
    return wifi_sta_ip(ip, n) || ethernet_ip(ip, n);
}

/*
 * 5121: port 80 answers everything with a redirect to the same path on
 * https://. Nothing else is served in the clear -- a page that loaded
 * over HTTP could post a password over HTTP, which is the thing 5121
 * exists to stop. The Host header is used so a name that reached the
 * player (defeatist-xxxx.local, a router's DNS name) is kept; it is
 * checked to be a plain host before it goes into a header.
 */
static esp_err_t h_redirect(httpd_req_t *req)
{
    char host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) host[0] = '\0';
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    for (const char *p = host; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) { host[0] = '\0'; break; }
    }
    if (!host[0]) {
        char ip[20];
        if (!have_ip(ip, sizeof(ip))) return httpd_resp_send_404(req);
        snprintf(host, sizeof(host), "%s", ip);
    }
    char loc[64 + 16 + 64];
    const char *uri = req->uri[0] == '/' ? req->uri : "/";
    snprintf(loc, sizeof(loc), "https://%s%.64s", host, uri);
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static void plain_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = REMOTE_PORT_PLAIN;
    cfg.ctrl_port = REMOTE_CTRL_PORT + 1;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    cfg.max_uri_handlers = 1;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_plain, &cfg) != ESP_OK) { s_plain = NULL; return; }
    const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_plain, &any);
}

static void plain_stop(void)
{
    if (!s_plain) return;
    httpd_stop(s_plain);
    s_plain = NULL;
}

static void start(void)
{
    const char *crt, *key;
    size_t crt_len, key_len;
    if (!devcert_get(&crt, &crt_len, &key, &key_len)) {
        s_retry_us = esp_timer_get_time() + REMOTE_RETRY_US;
        ESP_LOGW(TAG, "no certificate; not starting");
        return;
    }
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.ctrl_port = REMOTE_CTRL_PORT;
    cfg.httpd.max_open_sockets = REMOTE_SOCKETS;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.max_uri_handlers = 10;
    cfg.port_secure = REMOTE_PORT;
    cfg.servercert = (const uint8_t *)crt;
    cfg.servercert_len = crt_len;
    cfg.prvtkey_pem = (const uint8_t *)key;
    cfg.prvtkey_len = key_len;
    if (httpd_ssl_start(&s_srv, &cfg) != ESP_OK) {
        s_srv = NULL;
        s_retry_us = esp_timer_get_time() + REMOTE_RETRY_US;
        ESP_LOGW(TAG, "could not start on port %d; trying again in 5 s", REMOTE_PORT);
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",    .method = HTTP_GET, .handler = h_page },
        { .uri = "/art", .method = HTTP_GET, .handler = h_art  },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = h_icon },
        { .uri = "/stations", .method = HTTP_GET,  .handler = h_stations },
        { .uri = "/station",  .method = HTTP_POST, .handler = h_station_add },
        { .uri = "/wifi",      .method = HTTP_GET,  .handler = h_wifi },
        { .uri = "/wifi/scan", .method = HTTP_POST, .handler = h_wifi_scan },
        { .uri = "/wifi/join", .method = HTTP_POST, .handler = h_wifi_join },
        { .uri = "/ws",  .method = HTTP_GET, .handler = h_ws, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_srv, &uris[i]);
    }
    char url[48];
    if (remote_url(url, sizeof(url))) ESP_LOGI(TAG, "up at %s", url);
    /* The third baseline. TLS is the biggest single consumer of internal
     * RAM this device starts, and it starts late, so a map here separates
     * what the server costs from what the radio did. */
    heapmap_log("remote up");
    /* Everything is new to whoever connects next. */
    memset(&s_last, 0xFF, sizeof(s_last));
    s_wave_gen = UINT32_MAX;
}

static void stop(const char *why)
{
    plain_stop();
    if (!s_srv) return;
    httpd_ssl_stop(s_srv);
    s_srv = NULL;
    ESP_LOGI(TAG, "down: %s", why);
}

void remote_init(void)
{
    if (s_mu) return;
    s_mu = xSemaphoreCreateMutex();
    s_json = heap_caps_calloc(1, REMOTE_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wave = heap_caps_calloc(1, REMOTE_WAVE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* The larger of the two things built in it. */
    s_build = heap_caps_calloc(1, REMOTE_WAVE_MAX > REMOTE_JSON_MAX ? REMOTE_WAVE_MAX : REMOTE_JSON_MAX,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_levels = heap_caps_calloc(1, FRAMEWALK_MAX_COLUMNS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_w_seen = heap_caps_calloc(W_SEEN_MAX, sizeof(wifi_seen_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 5174: the queue's snapshots and ui_task's framing. 8 KB of ids and
     * offsets each; the names are grown as the queue needs. */
    for (int i = 0; i < 2; i++) {
        s_qs[i].id  = heap_caps_calloc(MPDQ_MAX, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_qs[i].off = heap_caps_calloc(MPDQ_MAX, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_qframe = heap_caps_malloc(Q_FRAME + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_qrows = heap_caps_malloc(MPDQ_MAX * sizeof(remote_qrow_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void remote_poll(bool want)
{
    if (!s_mu || !s_json || !s_wave || !s_build || !s_levels || !s_w_seen) return;
    char ip[20];
    const bool net = have_ip(ip, sizeof(ip));
    /* 5120: the portal has port 80 while it runs; see REMOTE_PORT. */
    const bool portal = portal_running();
    /*
     * 5121: the controls are on 443 and stay up through network setup;
     * only the port-80 redirect steps aside for the portal, which needs
     * that port and nothing else.
     */
    if (s_srv && !want) stop("switched off");
    else if (s_srv && !net) stop("no network");
    else if (!s_srv && want && net && esp_timer_get_time() >= s_retry_us) start();

    if (s_srv && !portal && !s_plain) plain_start();
    else if (s_plain && (portal || !s_srv)) plain_stop();
}

bool remote_running(void) { return s_srv != NULL; }

bool remote_url(char *out, size_t out_size)
{
    char ip[20];
    if (!out || !out_size || !have_ip(ip, sizeof(ip))) return false;
    if (REMOTE_PORT == 443) snprintf(out, out_size, "https://%s/", ip);
    else                    snprintf(out, out_size, "https://%s:%d/", ip, REMOTE_PORT);
    return true;
}

/* ---- state -------------------------------------------------------------- */

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void remote_publish(const ui_state_t *st, const char *art_path, int rec_count)
{
    queue_publish();                    /* 5174: first, so it sees a stop too */
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
    copy_str(c->path, sizeof(c->path), art_path);
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
    c->rec_off = st->rec_off;                           /* 5217 */
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
