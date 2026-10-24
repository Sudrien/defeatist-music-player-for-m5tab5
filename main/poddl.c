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

#include "covertag.h"           /* 8027: COVERTAG_MAX_IMAGE */
#include "folderart.h"         /* 8027: folderart_own_path() */
#include "podpath.h"
#include "radiobrowser.h"       /* RADIOBROWSER_UA_FMT */
#include "storage_io.h"

static const char *TAG = "tab5_poddl";

#define DL_STACK      8192          /* TLS and FatFs; internal: it writes the card */
#define DL_CHUNK      (16 * 1024)   /* one lease's worth, storage_io.h's 16 KB */
#define DL_TIMEOUT_MS 15000
#define DL_HOPS       6             /* tracking prefixes stack */
#define DL_URL_MAX    1024
#define DL_PATH_MAX   400           /* a podcast's folder */
/* 8022: the file and the part file, sized for the longest folder, name
 * and extension rather than the folder's bound again -- GCC's
 * format-truncation check at -O2 proves these cannot be cut. */
#define DL_FILE_MAX   (DL_PATH_MAX + 1 + PODPATH_NAME_MAX + 16 + 1 + 8)
#define DL_PART_MAX   (DL_PATH_MAX + 1 + PODPATH_NAME_MAX + 16 + 5)   /* 8024: <base>.part */
#define DL_LINK_MAX   (DL_PATH_MAX + sizeof("/" PODPATH_LINK))
/* 8027: a picture's path before its extension -- "<dir>/cover", or the
 * episode's file with a dot in front -- then with ".gif" and ".tmp". */
#define DL_STEM_MAX   (DL_FILE_MAX + 2)
#define DL_PIC_MAX    (DL_STEM_MAX + 5)
#define DL_PICTMP_MAX (DL_PIC_MAX + 4)

static SemaphoreHandle_t s_mu;
static poddl_state_t     s_state;
static int64_t           s_id, s_got, s_total;
static uint32_t          s_gen;
static int64_t           s_step = -1;           /* 8024: last tenth of a MB the generation moved for */
/* 8024: what the task should do at its next chunk. */
enum { RUN = 0, STOP_PAUSE, STOP_CANCEL };
static int               s_stop;
static char              s_url[DL_URL_MAX];
static char              s_dir[DL_PATH_MAX];
static char              s_base[PODPATH_NAME_MAX + 16];
static char              s_path[DL_FILE_MAX];   /* the finished file */
static int64_t           s_feed;                /* 8021: for the folder's link */
static char              s_show[128];
static char              s_cover_url[DL_URL_MAX];   /* 8027: the podcast's picture, or "" */
static char              s_own_url[DL_URL_MAX];     /* 8027: the episode's, when not the podcast's */

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

/* 8024: the generation moves every tenth of a MB -- what the status line
 * shows -- not every percent, which on a 557 MB file is 5.5 MB apart.
 * Returns what the task has been asked to do. */
static int set_progress(int64_t got)
{
    lock();
    s_got = got;
    const int64_t step = got * 10 >> 20;
    if (step != s_step) { s_step = step; s_gen++; }
    const int stop = s_stop;
    unlock();
    return stop;
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
static esp_http_client_handle_t open_followed(char *url, hdrs_t *h, int *status, int64_t from)
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
        if (from > 0) {                             /* 8024: the rest of a part file */
            char range[40];
            snprintf(range, sizeof(range), "bytes=%lld-", (long long)from);
            esp_http_client_set_header(c, "Range", range);
        }
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

/*
 * 8027: one picture, `url`, to `stem` plus the extension its first bytes
 * say (jpg, png or gif; anything else is not kept), through a .tmp file
 * renamed when whole. Bounded by the screen's own cover limit, since a
 * picture over it would not be shown. Failure is a log line: a missing
 * picture is not a failed download.
 */
static void fetch_picture(const char *what, const char *src, const char *stem)
{
    if (strlen(stem) + 1 > DL_STEM_MAX) return;
    static char url[DL_URL_MAX], path[DL_PIC_MAX], tmp[DL_PICTMP_MAX];
    static hdrs_t h;
    snprintf(url, sizeof(url), "%s", src);
    int status = 0;
    esp_http_client_handle_t c = open_followed(url, &h, &status, 0);
    if (!c) return;
    uint8_t *img = NULL;
    size_t got = 0;
    if (status != 200) {
        ESP_LOGI(TAG, "%s: HTTP %d; none kept", what, status);
        goto done;
    }
    const int64_t clen = esp_http_client_get_content_length(c);
    if (clen > (int64_t)COVERTAG_MAX_IMAGE) {
        ESP_LOGI(TAG, "%s is %lld KB, over the %u KB cover limit; none kept", what,
                 (long long)(clen / 1024), (unsigned)(COVERTAG_MAX_IMAGE / 1024));
        goto done;
    }
    /* The size it says, when it says one, plus a byte to see it end;
     * else the limit. PSRAM, for the length of the fetch. */
    const size_t cap = clen > 0 ? (size_t)clen + 1 : COVERTAG_MAX_IMAGE;
    img = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) goto done;
    for (;;) {
        if (got >= cap) {
            ESP_LOGI(TAG, "%s is over the %u KB cover limit; none kept", what,
                     (unsigned)(COVERTAG_MAX_IMAGE / 1024));
            goto done;
        }
        const int n = esp_http_client_read(c, (char *)img + got, (int)(cap - got));
        if (n < 0) goto done;
        if (n == 0) break;
        got += (size_t)n;
    }
    const char *ext = got > 3 && img[0] == 0xFF && img[1] == 0xD8 && img[2] == 0xFF ? "jpg"
                    : got > 8 && !memcmp(img, "\x89PNG\r\n\x1a\n", 8)           ? "png"
                    : got > 6 && (!memcmp(img, "GIF87a", 6) || !memcmp(img, "GIF89a", 6)) ? "gif" : NULL;
    if (!ext) {
        ESP_LOGI(TAG, "%s is not a JPEG, PNG or GIF; none kept", what);
        goto done;
    }
    snprintf(path, sizeof(path), "%s.%s", stem, ext);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(img, 1, got, f) == got;
    if (f && fclose(f) != 0) ok = false;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    if (!ok) remove(tmp);
    storage_io_release();
    if (ok) ESP_LOGI(TAG, "%s: %u KB as %s", what, (unsigned)(got / 1024), path);
    else    ESP_LOGW(TAG, "%s: could not write %s", what, path);
done:
    free(img);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

/* 8027: whether the folder has a cover of its own already -- one that
 * came with an earlier episode, or one the listener put there. */
static bool has_cover(const char *dir)
{
    static char p[DL_LINK_MAX + 16];
    struct stat st;
    for (int i = 0; i < FOLDERART_NNAMES; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, FOLDERART_NAMES[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    return false;
}

static void dl_task(void *arg)
{
    (void)arg;
    static char url[DL_URL_MAX], part[DL_PART_MAX];
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
    {
        /* 8021: the folder's way back to the podcast. Rewritten each
         * time, so a renamed podcast's folder follows; a failure here
         * costs only the link, not the download. */
        static char link[DL_LINK_MAX], text[160];
        snprintf(link, sizeof(link), "%s/" PODPATH_LINK, s_dir);
        const int n = podpath_link_format(s_feed, s_show, text, sizeof(text));
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        FILE *lf = fopen(link, "w");
        const bool wrote = lf && n > 0 && fwrite(text, 1, (size_t)n, lf) == (size_t)n;
        if (lf) fclose(lf);
        storage_io_release();
        if (!wrote) ESP_LOGW(TAG, "could not write %s", link);
    }
    /*
     * 8024: a part file already there is the start of this download --
     * paused, cut off by a reboot or a dropped link -- and the rest is
     * asked for with Range. 206 appends; 200 means the server sends the
     * whole thing, so it starts again; 416 means the part is no good to
     * it, so it goes and the whole is asked for once.
     *
     * The size comes from stat(), whose st_size is off_t: a part past
     * 2 GB here starts over rather than resuming at a wrong place.
     */
    snprintf(part, sizeof(part), "%s/%s.part", s_dir, s_base);
    int64_t from = 0;
    {
        struct stat pst;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (stat(part, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) from = (int64_t)pst.st_size;
        storage_io_release();
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        c = open_followed(url, &h, &status, from);
        if (!c) goto out;
        if (status == 416 && from > 0) {
            ESP_LOGI(TAG, "416 for the rest from %lld; starting over", (long long)from);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            c = NULL;
            lock(); snprintf(url, sizeof(url), "%s", s_url); unlock();
            from = 0;
            continue;
        }
        break;
    }
    if (status == 200) {
        from = 0;
    } else if (status != 206 || from == 0) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto out;
    }

    char ext[8];
    podpath_url_ext(url, ext, sizeof(ext));
    if (!ext[0]) snprintf(ext, sizeof(ext), "%s", podpath_type_ext(h.ctype));
    if (!ext[0]) snprintf(ext, sizeof(ext), "mp3");
    {
        const int64_t clen = esp_http_client_get_content_length(c);
        lock();
        snprintf(s_path, sizeof(s_path), "%s/%s.%s", s_dir, s_base, ext);
        s_total = clen > 0 ? from + clen : -1;
        s_got = from;
        unlock();
    }

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    f = storage_io_open(part, from > 0 ? "ab" : "wb");
    storage_io_release();
    if (!f) {
        ESP_LOGW(TAG, "could not open %s", part);
        goto out;
    }
    ESP_LOGI(TAG, "to %s (%lld bytes%s, from %lld)", s_path, (long long)s_total,
             s_total < 0 ? ", length not given" : "", (long long)from);

    int64_t got = from;
    bool ok = true;
    int stop = RUN;
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
        stop = set_progress(got);
        if (stop != RUN) break;
    }
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (storage_io_close(f) != 0) ok = false;
    f = NULL;
    if (stop == STOP_PAUSE) {
        /* Kept: the next start of this episode resumes from it. */
        storage_io_release();
        st = PODDL_PAUSED;
        ESP_LOGI(TAG, "paused at %lld bytes", (long long)got);
        goto out;
    }
    if (stop == STOP_CANCEL) {
        remove(part);
        storage_io_release();
        st = PODDL_IDLE;
        ESP_LOGI(TAG, "cancelled at %lld bytes", (long long)got);
        goto out;
    }
    if (ok && s_total > 0 && got != s_total) {
        ESP_LOGW(TAG, "ended at %lld of %lld bytes", (long long)got, (long long)s_total);
        ok = false;
    }
    if (ok) {
        remove(s_path);                  /* a stale one, if any; rename will not replace */
        if (rename(part, s_path) != 0) {
            ESP_LOGW(TAG, "could not rename %s (errno %d)", part, errno);
            ok = false;
        }
    }
    /* 8024: a part that failed is kept too -- a dropped link is the usual
     * reason, and the next tap takes up from it. Only a cancel removes it. */
    storage_io_release();
    if (ok) {
        st = PODDL_DONE;
        ESP_LOGI(TAG, "done: %lld bytes", (long long)got);
        /*
         * 8027: the pictures, after the episode and never instead of it.
         * The podcast's as cover.<ext> when the folder has no cover yet,
         * so every episode in it has one; the episode's own, when it
         * differs, as the dotfile folderart.c reads first in a podcast's
         * folder.
         */
        static char stem[DL_STEM_MAX + 2];          /* + ".x", taken off again */
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        const bool covered = has_cover(s_dir);
        storage_io_release();
        if (s_cover_url[0] && !covered) {
            snprintf(stem, sizeof(stem), "%s/cover", s_dir);
            fetch_picture("the podcast's picture", s_cover_url, stem);
        }
        if (s_own_url[0] && folderart_own_path(s_path, "x", stem, sizeof(stem))) {
            stem[strlen(stem) - 2] = '\0';          /* without the ".x": fetch_picture() adds it */
            fetch_picture("the episode's picture", s_own_url, stem);
        }
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

bool poddl_start(int64_t id, const char *url, const char *dir, const char *base,
                 int64_t feed, const char *show, const char *cover, const char *own)
{
    if (!url || !dir || !base) return false;
    lock();
    if (s_state == PODDL_RUNNING) { unlock(); return false; }
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    snprintf(s_base, sizeof(s_base), "%s", base);
    s_feed = feed;                                  /* 8021 */
    snprintf(s_show, sizeof(s_show), "%s", show ? show : "");
    snprintf(s_cover_url, sizeof(s_cover_url), "%s", cover ? cover : "");            /* 8027 */
    snprintf(s_own_url, sizeof(s_own_url), "%s",
             own && own[0] && (!cover || strcmp(own, cover)) ? own : "");
    s_path[0] = '\0';
    s_id = id;
    s_got = 0;
    s_total = -1;
    s_step = -1;
    s_stop = RUN;                                   /* 8024 */
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

void poddl_pause(void)
{
    lock();
    if (s_state == PODDL_RUNNING) s_stop = STOP_PAUSE;
    unlock();
}

void poddl_cancel(void)
{
    static char part[DL_PART_MAX];
    lock();
    const poddl_state_t st = s_state;
    if (st == PODDL_RUNNING) s_stop = STOP_CANCEL;
    if (st == PODDL_PAUSED || st == PODDL_FAILED) {
        snprintf(part, sizeof(part), "%s/%s.part", s_dir, s_base);
        s_state = PODDL_IDLE;
        s_gen++;
    }
    unlock();
    if (st == PODDL_PAUSED || st == PODDL_FAILED) {
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        remove(part);
        storage_io_release();
        ESP_LOGI(TAG, "cancelled; part file removed");
    }
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

bool poddl_part(const char *dir, const char *base, int64_t *size)
{
    static char p[DL_PART_MAX];
    struct stat st;
    if (snprintf(p, sizeof(p), "%s/%s.part", dir, base) >= (int)sizeof(p)) return false;
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (size) *size = (int64_t)st.st_size;
    return true;
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
