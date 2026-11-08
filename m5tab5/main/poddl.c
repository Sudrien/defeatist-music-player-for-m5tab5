/*
 * poddl.c -- see poddl.h.
 *
 * 8124: the download itself -- the redirects, the part file and its .id,
 * the Range resume, the stripes over three connections, the card writer,
 * the retries -- is feckless-libs' feckless_fetch (its 9003, lifted from
 * this file's 8020-8057). What stays here is what is a podcast's: where
 * the part file lives (podpath.h), the extension, the folder's link back
 * to Podcast Index, the pictures, and the clean-up of the folders a
 * cancel leaves empty. They are the callback's.
 *
 * SPDX-License-Identifier: MIT
 */
#include "poddl.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>           /* 8056: PRIx32 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>             /* 8029: rmdir() */

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "covertag.h"           /* 8027: COVERTAG_MAX_IMAGE */
#include "feckless_fetch.h"     /* 8124 */
#include "folderart.h"          /* 8027: folderart_own_path() */
#include "podpath.h"
#include "radiobrowser.h"       /* RADIOBROWSER_UA_FMT */
#include "storage_io.h"
#include "replaygain.h"         /* 8056: the episode's picture, by name */
#include "netbudget.h"          /* 8043: NETBUDGET_PODDL */

static const char *TAG = "tab5_poddl";

/* 8043, 8124: sockets a download may hold at once, and the smallest file
 * that is split between them (feckless_fetch's default). netbudget.h counts
 * NETBUDGET_PODDL for this. The helper connections' stacks are in PSRAM,
 * which needs the option below; without it every download is one stream. */
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "poddl.c asks feckless_fetch for three connections, whose helper stacks are in PSRAM: it needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
#endif
#define DL_CONNS      FECKLESS_FETCH_CONNS_MAX
_Static_assert(DL_CONNS <= NETBUDGET_PODDL, "netbudget.h counts fewer sockets for poddl than DL_CONNS");

#define DL_TIMEOUT_MS 15000
#define DL_HOPS       FECKLESS_FETCH_HOPS
#define DL_URL_MAX    FECKLESS_FETCH_URL_MAX
#define DL_PATH_MAX   400           /* a podcast's folder */
/* 8022: the file and the part file, sized for the longest folder, name
 * and extension rather than the folder's bound again -- GCC's
 * format-truncation check at -O2 proves these cannot be cut. */
#define DL_FILE_MAX   (DL_PATH_MAX + 1 + PODPATH_NAME_MAX + 16 + 1 + 8)
/* 8024: <base>.part; 8029: under <mount>/Podcasts/.partial/<podcast>/ */
#define DL_PART_MAX   (DL_PATH_MAX + sizeof("/" PODPATH_PARTIAL) + 1 + PODPATH_NAME_MAX + 16 + 5)
#define DL_LINK_MAX   (DL_PATH_MAX + sizeof("/" PODPATH_LINK))
/* 8027: a picture's path before its extension -- "<dir>/cover", or the
 * episode's file with a dot in front -- then with ".gif" and ".tmp". */
#define DL_STEM_MAX   (DL_FILE_MAX + 2)
#define DL_PIC_MAX    (DL_STEM_MAX + 5)
#define DL_PICTMP_MAX (DL_PIC_MAX + 4)

/*
 * What the callback reads. Written by poddl_start() only while nothing is
 * running, under s_mu, and read by the download's task (and by poddl_cancel()'s
 * caller for a paused one) -- which feckless_fetch never starts without a
 * start that has returned.
 */
static SemaphoreHandle_t s_mu;
static char              s_dir[DL_PATH_MAX];
static char              s_base[PODPATH_NAME_MAX + 16];
static int64_t           s_feed;                /* 8021: for the folder's link */
static char              s_show[128];
static char              s_cover_url[DL_URL_MAX];   /* 8027: the podcast's picture, or "" */
static char              s_own_url[DL_URL_MAX];     /* 8027: the episode's, when not the podcast's */

static void lock(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mu, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mu); }

static poddl_state_t map_state(feckless_fetch_state_t st)
{
    switch (st) {
    case FECKLESS_FETCH_RUNNING: return PODDL_RUNNING;
    case FECKLESS_FETCH_DONE:    return PODDL_DONE;
    case FECKLESS_FETCH_FAILED:  return PODDL_FAILED;
    case FECKLESS_FETCH_PAUSED:  return PODDL_PAUSED;
    case FECKLESS_FETCH_IDLE:
    default:                     return PODDL_IDLE;
    }
}

/* mkdir that does not mind the folder being there. */
static bool mk(const char *p)
{
    if (mkdir(p, 0777) == 0 || errno == EEXIST) return true;
    ESP_LOGW(TAG, "could not make %s (errno %d)", p, errno);
    return false;
}

/*
 * 8029: the folders the part file needs -- <mount>/Podcasts, its
 * .partial, and .partial/<podcast> -- and not the podcast's own folder,
 * which waits for a whole episode. feckless_fetch makes the part file's
 * folders itself; this is for the move of an old part file into them,
 * which it asks for before it does (the START event).
 */
static bool make_part_dirs(const char *dir)
{
    static char p[DL_PART_MAX];
    if (!podpath_part(dir, NULL, p, sizeof(p))) return false;
    char *leaf = strrchr(p, '/');                   /* .../.partial/<podcast> */
    *leaf = '\0';
    char *partial = strrchr(p, '/');                /* .../Podcasts/.partial */
    *partial = '\0';
    bool ok = mk(p);                                /* <mount>/Podcasts */
    *partial = '/';
    ok = ok && mk(p);                               /* .partial */
    *leaf = '/';
    ok = ok && mk(p);                               /* .partial/<podcast> */
    return ok;
}

/* 8029: the part file's folder goes when it is empty; rmdir() refuses
 * one that is not, which is what is wanted. */
static void drop_part_dir(const char *dir)
{
    static char p[DL_PART_MAX];
    if (podpath_part(dir, NULL, p, sizeof(p))) rmdir(p);
}

/* 8021: the podcast's way back, in its folder. 8127: written when the
 * server has agreed to send the episode, and again when it is whole.
 * Rewritten each time, so a renamed podcast's folder follows; a failure
 * costs only the link. */
static void write_link(void)
{
    static char link[DL_LINK_MAX], text[160];
    snprintf(link, sizeof(link), "%s/" PODPATH_LINK, s_dir);
    const int n = podpath_link_format(s_feed, s_show, text, sizeof(text));
    FILE *lf = fopen(link, "w");
    const bool wrote = lf && n > 0 && fwrite(text, 1, (size_t)n, lf) == (size_t)n;
    if (lf) fclose(lf);
    if (!wrote) ESP_LOGW(TAG, "could not write %s", link);
}

/*
 * 8029: a podcast folder that holds nothing a person would see -- only
 * the link, and part files from before 8029 put them elsewhere -- goes,
 * with those. Anything else in it, a cover or an episode or a file the
 * listener added, keeps it. The cancel that left 8021-8028's folders
 * with only a link is what runs this.
 */
static void drop_empty_podcast_dir(const char *dir)
{
    static char p[DL_LINK_MAX + DL_FILE_MAX];
    DIR *d = opendir(dir);
    if (!d) return;
    bool other = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && !other) {
        const char *n = e->d_name;
        const size_t l = strlen(n);
        if (!strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, PODPATH_LINK)) continue;
        if (l > 5 && !strcmp(n + l - 5, ".part")) continue;
        other = true;
    }
    if (other) { closedir(d); return; }
    rewinddir(d);
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) < (int)sizeof(p)) remove(p);
    }
    closedir(d);
    if (rmdir(dir) == 0) ESP_LOGI(TAG, "removed %s: it held no episode", dir);
}

/* ---- the pictures (8027, 8056) ----------------------------------------- */

/* What a picture's reply says of a redirect; filled by the event handler. */
static char s_location[DL_URL_MAX];

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value &&
        !strcasecmp(ev->header_key, "Location"))
        snprintf(s_location, sizeof(s_location), "%s", ev->header_value);
    return ESP_OK;
}

/* Open `url`, following redirects by hand; the client, positioned at the
 * body, with its status in *status, or NULL. `url` is rewritten with the
 * address that answered. The picture's own, since feckless_fetch is for
 * files and one runs at a time -- this runs inside one's callback. */
static esp_http_client_handle_t open_followed(char *url, int *status)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT, desc && desc->version[0] ? desc->version : "0");
    *status = 0;
    for (int hop = 0; hop <= DL_HOPS; hop++) {
        s_location[0] = '\0';
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = DL_TIMEOUT_MS,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
            .user_agent = ua,
            .disable_auto_redirect = true,
            .event_handler = on_http_event,
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
        if (!s_location[0]) return NULL;
        if (!strncmp(s_location, "http://", 7) || !strncmp(s_location, "https://", 8)) {
            snprintf(url, DL_URL_MAX, "%s", s_location);
        } else if (s_location[0] == '/') {
            /* Absolute path on the same host. */
            char *p = strstr(url, "://");
            p = p ? strchr(p + 3, '/') : NULL;
            const size_t keep = p ? (size_t)(p - url) : strlen(url);
            if (keep + strlen(s_location) + 1 > DL_URL_MAX) return NULL;
            memmove(url + keep, s_location, strlen(s_location) + 1);
        } else {
            ESP_LOGW(TAG, "a redirect to \"%.60s\" not followed", s_location);
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
static bool fetch_picture(const char *what, const char *src, const char *stem)
{
    if (strlen(stem) + 1 > DL_STEM_MAX) return false;
    static char url[DL_URL_MAX], path[DL_PIC_MAX], tmp[DL_PICTMP_MAX];
    snprintf(url, sizeof(url), "%s", src);
    int status = 0;
    esp_http_client_handle_t c = open_followed(url, &status);
    if (!c) return false;
    uint8_t *img = NULL;
    size_t got = 0;
    bool ok = false;
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
    /* The guard on entry holds strlen(stem) to DL_STEM_MAX - 1; the
     * precision says so to the compiler, which -Os (8031) no longer
     * finds out for itself across the inlining. */
    snprintf(path, sizeof(path), "%.*s.%s", DL_STEM_MAX - 1, stem, ext);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    FILE *f = fopen(tmp, "wb");
    ok = f && fwrite(img, 1, got, f) == got;
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
    return ok;
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

/*
 * 8027: the pictures, after the episode and never instead of it. The
 * podcast's as cover.<ext> when the folder has no cover yet, so every
 * episode in it has one; the episode's own, when it differs.
 *
 * 8056: the episode's own picture, named for its address --
 * art-<MurmurHash2 of the URL>.<ext> in the podcast's folder -- so
 * episodes that share one (a miniseries) share one file, and the second
 * episode does not fetch it again. The episode's sidecar says which is
 * its (art.ext, 8055); there is no link on FAT to say it otherwise. Not
 * a dotfile: it is a picture the listener may want to see in the folder.
 * Pictures 8027 saved as .<file>.jpg are still found by folderart.c.
 */
static void fetch_pictures(const char *episode_path)
{
    static char stem[DL_STEM_MAX + 2];              /* + ".x", taken off again */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const bool covered = has_cover(s_dir);
    storage_io_release();
    if (s_cover_url[0] && !covered) {
        snprintf(stem, sizeof(stem), "%s/cover", s_dir);
        fetch_picture("the podcast's picture", s_cover_url, stem);
    }
    if (!s_own_url[0]) return;

    static char name[24], p[DL_PATH_MAX + 32];
    const uint32_t uh = albumart_cover_hash(s_own_url, strlen(s_own_url));
    static const char *const exts[] = { "jpg", "png", "gif" };
    const char *have = NULL;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    for (int i = 0; i < 3 && !have; i++) {
        struct stat pst;
        snprintf(name, sizeof(name), "art-%08" PRIx32 ".%s", uh, exts[i]);
        snprintf(p, sizeof(p), "%s/%s", s_dir, name);
        if (stat(p, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) have = name;
    }
    storage_io_release();
    if (have) {
        ESP_LOGI(TAG, "the episode's picture: %s, already here", have);
    } else {
        snprintf(stem, sizeof(stem), "%s/art-%08" PRIx32, s_dir, uh);
        if (fetch_picture("the episode's picture", s_own_url, stem)) {
            /* fetch_picture() chose the extension; find it again. */
            storage_io_acquire(STORAGE_IO_BACKGROUND);
            for (int i = 0; i < 3 && !have; i++) {
                struct stat pst;
                snprintf(name, sizeof(name), "art-%08" PRIx32 ".%s", uh, exts[i]);
                snprintf(p, sizeof(p), "%s/%s", s_dir, name);
                if (stat(p, &pst) == 0 && S_ISREG(pst.st_mode)) have = name;
            }
            storage_io_release();
        }
    }
    if (have) {
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        const esp_err_t rerr = replaygain_save_art_ext(episode_path, have);
        storage_io_release();
        if (rerr != ESP_OK) ESP_LOGW(TAG, "could not record %s in the sidecar", have);
    }
}

/* ---- feckless_fetch's callback ----------------------------------------- */

/* The extension: the URL's, else the reply's Content-Type, else mp3. */
static void choose_ext(const char *url, const char *ctype, char *ext, size_t cap)
{
    ext[0] = '\0';
    char tmp[8];
    podpath_url_ext(url, tmp, sizeof(tmp));
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "%s", podpath_type_ext(ctype));
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "mp3");
    snprintf(ext, cap, "%s", tmp);
}

static bool on_event(feckless_fetch_ev_t *ev)
{
    switch (ev->kind) {
    case FECKLESS_FETCH_EV_START: {
        /* 8029: a part file where 8024-8028 kept it, in the podcast's own
         * folder, is moved here and taken up from, rather than left
         * behind while this starts again from nothing. */
        static char old[DL_FILE_MAX];
        struct stat ost;
        snprintf(old, sizeof(old), "%s/%s.part", s_dir, s_base);
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        const bool dirs = make_part_dirs(s_dir);
        if (dirs && stat(ev->part, &ost) != 0 && stat(old, &ost) == 0 && S_ISREG(ost.st_mode)) {
            if (rename(old, ev->part) == 0) ESP_LOGI(TAG, "moved an earlier part file to %s", ev->part);
        }
        storage_io_release();
        return true;
    }
    case FECKLESS_FETCH_EV_HEADERS: {
        char ext[8];
        choose_ext(ev->url, ev->ctype, ext, sizeof(ext));
        snprintf(ev->path, ev->path_size, "%s/%s.%s", s_dir, s_base, ext);
        /* 8127: the server has said yes (a 200, or the 206 of a resume), so
         * the podcast's folder and its way back are made now: the chooser
         * shows the link while the episode comes. Not at START (8126),
         * which a 401, a 403 or a dead host would have left a folder
         * behind for. A download that is cancelled takes the folder with
         * it if nothing else is in it (CANCELLED). */
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (mk(s_dir)) write_link();
        storage_io_release();
        return true;
    }
    case FECKLESS_FETCH_EV_VERIFY:
        return true;
    case FECKLESS_FETCH_EV_DONE:
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        write_link();                               /* 8021: again, for a renamed podcast */
        drop_part_dir(s_dir);                       /* 8029 */
        storage_io_release();
        fetch_pictures(ev->path);
        return true;
    case FECKLESS_FETCH_EV_CANCELLED:
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        drop_part_dir(s_dir);                       /* 8029 */
        drop_empty_podcast_dir(s_dir);
        storage_io_release();
        return true;
    }
    return true;
}

/* ---- the interface ----------------------------------------------------- */

bool poddl_start(int64_t id, const char *url, const char *dir, const char *base,
                 int64_t feed, const char *show, const char *cover, const char *own)
{
    if (!url || !dir || !base) return false;
    static char part[DL_PART_MAX], path[DL_FILE_MAX], ua[64];
    char ext[8];
    lock();
    if (feckless_fetch_state(NULL, NULL, NULL, NULL, 0, NULL) == FECKLESS_FETCH_RUNNING) {
        unlock();
        return false;
    }
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    snprintf(s_base, sizeof(s_base), "%s", base);
    s_feed = feed;                                  /* 8021 */
    snprintf(s_show, sizeof(s_show), "%s", show ? show : "");
    snprintf(s_cover_url, sizeof(s_cover_url), "%s", cover ? cover : "");            /* 8027 */
    snprintf(s_own_url, sizeof(s_own_url), "%s",
             own && own[0] && (!cover || strcmp(own, cover)) ? own : "");
    if (!podpath_part(s_dir, s_base, part, sizeof(part))) { unlock(); return false; }   /* 8029 */
    /* Until the reply says what it is, the URL's extension is the guess;
     * the HEADERS event sets the real one. */
    choose_ext(url, "", ext, sizeof(ext));
    snprintf(path, sizeof(path), "%s/%s.%s", s_dir, s_base, ext);
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT, desc && desc->version[0] ? desc->version : "0");
    const feckless_fetch_opts_t o = {
        .url = url,
        .path = path,
        .part = part,
        .conns = DL_CONNS,
        .split_min = 0,                             /* its 8 MB */
        .user_agent = ua,
        .https_only = false,                        /* http at the far end, as netstream allows it (0418) */
        .cb = on_event,
        .arg = NULL,
        .id = id,
    };
    const bool ok = feckless_fetch_start(&o);
    unlock();
    if (!ok) {
        char why[64];
        feckless_fetch_why(why, sizeof(why));
        ESP_LOGW(TAG, "episode %lld: not started: %s", (long long)id, why);
        return false;
    }
    ESP_LOGI(TAG, "episode %lld: downloading", (long long)id);
    return true;
}

void poddl_pause(void)
{
    feckless_fetch_pause();
}

void poddl_cancel(void)
{
    feckless_fetch_cancel();                        /* its CANCELLED event cleans up */
}

poddl_state_t poddl_state(int64_t *id, int64_t *got, int64_t *total,
                          char *path, size_t path_size, uint32_t *gen)
{
    return map_state(feckless_fetch_state(id, got, total, path, path_size, gen));
}

void poddl_discard(const char *dir, const char *base)
{
    static char p[DL_PART_MAX];
    if (!dir || !base) return;
    if (feckless_fetch_state(NULL, NULL, NULL, NULL, 0, NULL) == FECKLESS_FETCH_RUNNING) return;
    if (podpath_part(dir, base, p, sizeof(p))) feckless_fetch_discard(p);     /* 8041: and its .id */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (snprintf(p, sizeof(p), "%s/%s.part", dir, base) < (int)sizeof(p)) remove(p);   /* 8024-8028's */
    drop_part_dir(dir);
    drop_empty_podcast_dir(dir);
    storage_io_release();
}

bool poddl_part(const char *dir, const char *base, int64_t *size)
{
    static char p[DL_PART_MAX];
    struct stat st;
    if (!podpath_part(dir, base, p, sizeof(p))) return false;          /* 8029 */
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
        /* 8029: or where 8024-8028 kept it; the START event moves it over. */
        if (snprintf(p, sizeof(p), "%s/%s.part", dir, base) >= (int)sizeof(p)) return false;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    }
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

bool poddl_live(const char *path, char *part, size_t part_size,
                int64_t *ready, int64_t *total, poddl_state_t *st)
{
    feckless_fetch_state_t fs = FECKLESS_FETCH_IDLE;
    if (!feckless_fetch_live(path, part, part_size, ready, total, &fs)) return false;
    if (st) *st = map_state(fs);
    return true;
}
