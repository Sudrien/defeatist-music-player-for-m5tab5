/*
 * poddl.c -- see poddl.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "poddl.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>             /* 8042: LONG_MAX, fseek()'s reach */
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
#include "esp_timer.h"          /* 8034: download timing */
#include "lwip/opt.h"           /* 8034: TCP_WND in the timing line */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"     /* 8036: the writer's hand-off */
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "covertag.h"           /* 8027: COVERTAG_MAX_IMAGE */
#include "folderart.h"         /* 8027: folderart_own_path() */
#include "podpath.h"
#include "radiobrowser.h"       /* RADIOBROWSER_UA_FMT */
#include "storage_io.h"
#include "netbudget.h"           /* 8043: NETBUDGET_PODDL */
#include "esp_attr.h"            /* 8043: DRAM_ATTR on the helpers' TCBs */

static const char *TAG = "tab5_poddl";

#define DL_STACK      8192          /* TLS and FatFs; internal: it writes the card */
#define DL_CHUNK      (16 * 1024)   /* one lease's worth, storage_io.h's 16 KB */
#define DL_TIMEOUT_MS 15000
/* 8036: chunks in flight between the reader and the card writer, all in
 * PSRAM. Four is 64 KB: room for the card's slow writes (8034 saw ~22 ms
 * per 16 KB, longer while playback holds the lease) without the reader
 * stalling. The writer's stack is internal, as DL_STACK's is: it writes
 * the card. */
#define DL_NBUF       6             /* 8043: 4, and one more for each extra connection */
#define DL_WR_STACK   6144
/* 8042: the most segments a part file's map can hold, and how often the
 * writer saves the map while it writes (bytes). */
#define DL_SEGS_MAX   4
#define DL_MAP_EVERY  (4 * 1024 * 1024)
/* 8043: connections a download may use at once, and the smallest file
 * that is split between them. Each extra one is a helper task, created
 * once, its stack in PSRAM: it does sockets and TLS, and never writes the
 * card or flash. netbudget.h counts DL_CONNS sockets for this. */
#define DL_CONNS      3
#define DL_SPLIT_MIN  (8 * 1024 * 1024)
#define DL_HELP_STACK 6144
#define DL_HOPS       6             /* tracking prefixes stack */
#define DL_URL_MAX    1024
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
    /* 8041: what a resume is checked against. */
    char etag[96];
    char lastmod[40];
    char crange[80];
    char ranges[16];            /* 8043: Accept-Ranges */
    char if_range[96];          /* sent with Range, never cleared by a hop */
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
        else if (!strcasecmp(ev->header_key, "ETag"))                /* 8041 */
            snprintf(h->etag, sizeof(h->etag), "%.*s", (int)sizeof(h->etag) - 1, ev->header_value);
        else if (!strcasecmp(ev->header_key, "Last-Modified"))
            snprintf(h->lastmod, sizeof(h->lastmod), "%.*s", (int)sizeof(h->lastmod) - 1, ev->header_value);
        else if (!strcasecmp(ev->header_key, "Accept-Ranges"))      /* 8043 */
            snprintf(h->ranges, sizeof(h->ranges), "%.*s", (int)sizeof(h->ranges) - 1, ev->header_value);
        else if (!strcasecmp(ev->header_key, "Content-Range"))
            snprintf(h->crange, sizeof(h->crange), "%.*s", (int)sizeof(h->crange) - 1, ev->header_value);
    }
    return ESP_OK;
}

/* 8024: the generation moves every tenth of a MB -- what the status line
 * shows -- not every percent, which on a 557 MB file is 5.5 MB apart.
 * 8043: set_progress() became add_progress(), by the readers. */

static void finish(poddl_state_t st)
{
    lock();
    s_state = st;
    s_gen++;
    unlock();
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
 * which waits for a whole episode (finish_dir()).
 */
static bool make_part_dirs(const char *dir)
{
    static char p[DL_PART_MAX];
    if (!podpath_part(dir, NULL, p, sizeof(p))) return false;
    char *leaf = strrchr(p, '/');                   /* .../.partial/<podcast> */
    *leaf = '\0';
    char *partial = strrchr(p, '/');                /* .../Podcasts/.partial */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    *partial = '\0';
    bool ok = mk(p);                                /* <mount>/Podcasts */
    *partial = '/';
    ok = ok && mk(p);                               /* .partial */
    *leaf = '/';
    ok = ok && mk(p);                               /* .partial/<podcast> */
    storage_io_release();
    return ok;
}

/* 8029: the part file's folder goes when it is empty; rmdir() refuses
 * one that is not, which is what is wanted. */
/*
 * 8041: THE PART FILE'S IDENTITY. A resume asked for "the rest" and took
 * any 206 as the rest of the same file. art19 and its kind insert ads
 * per request, so the file a server has today need not be the one whose
 * first half is on the card, and a join of two of them is a jump in the
 * audio that no length check sees: s_total was from + the second reply's
 * length. Now <part>.id holds the first reply's total and its validator
 * -- a strong ETag, else Last-Modified -- and a resume sends If-Range
 * with it and checks Content-Range: the first byte must be `from`, and
 * the total the one saved. Anything else starts over.
 *
 * Two lines: the total in decimal (-1 if not given), then the validator
 * (empty if neither header came).
 */
static bool id_path(const char *part, char *out, size_t cap)
{
    return snprintf(out, cap, "%s.id", part) < (int)cap;
}

/*
 * 8042: THE MAP. The .id also says which bytes of the part file are
 * there, as segments: `s <start> <end> <done>` after 8041's two lines,
 * one a segment, end exclusive (-1 when the length is not known), done
 * the bytes from start that are on the card. 8041's two-line .id is one
 * segment, done from the part file's size, as 8024 resumed. The size
 * stops being the answer once a segment can be written past another's
 * end (8043), so with a map the map is the answer, and a part file with
 * more than one segment and no readable map starts over.
 *
 * Written to <part>.id.tmp and renamed over, so a reboot during the write
 * leaves the old map or the new one. rename() does not replace, so the
 * old one goes first; a read that finds no .id takes the .tmp.
 */
typedef struct { int64_t start, end, done; } seg_t;

static bool id_tmp_path(const char *part, char *out, size_t cap)
{
    return snprintf(out, cap, "%s.id.tmp", part) < (int)cap;
}

/* Returns the number of segments read (0: a .id with no map, as 8041
 * wrote), or -1 for no .id at all. */
static int id_read(const char *part, char *val, size_t vcap, int64_t *total,
                   seg_t *seg, int nmax)
{
    static char p[DL_PART_MAX + 8];
    val[0] = '\0';
    *total = -1;
    if (!id_path(part, p, sizeof(p))) return -1;
    FILE *f = fopen(p, "r");
    if (!f && id_tmp_path(part, p, sizeof(p))) f = fopen(p, "r");
    if (!f) return -1;
    char line[80];
    int n = -1;
    if (fgets(line, sizeof(line), f)) {
        *total = strtoll(line, NULL, 10);
        n = 0;
        if (fgets(val, (int)vcap, f)) val[strcspn(val, "\r\n")] = '\0';
        while (n < nmax && fgets(line, sizeof(line), f)) {
            long long a, b, d;
            if (sscanf(line, "s %lld %lld %lld", &a, &b, &d) != 3) break;
            seg[n].start = a; seg[n].end = b; seg[n].done = d;
            n++;
        }
    }
    fclose(f);
    return n;
}

static void id_write(const char *part, const char *val, int64_t total, const seg_t *seg, int nseg)
{
    static char p[DL_PART_MAX + 8], t[DL_PART_MAX + 8];
    if (!id_path(part, p, sizeof(p)) || !id_tmp_path(part, t, sizeof(t))) return;
    FILE *f = fopen(t, "w");
    if (!f) { ESP_LOGW(TAG, "could not write %s", t); return; }
    bool ok = fprintf(f, "%lld\n%s\n", (long long)total, val) > 0;
    for (int i = 0; i < nseg && ok; i++)
        ok = fprintf(f, "s %lld %lld %lld\n", (long long)seg[i].start,
                     (long long)seg[i].end, (long long)seg[i].done) > 0;
    if (fflush(f) != 0) ok = false;
    if (ok) fsync(fileno(f));
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(t); ESP_LOGW(TAG, "could not write %s", t); return; }
    remove(p);
    if (rename(t, p) != 0) ESP_LOGW(TAG, "could not rename %s", t);
}

static void remove_id(const char *part)
{
    static char p[DL_PART_MAX + 8];
    if (id_path(part, p, sizeof(p))) remove(p);
    if (id_tmp_path(part, p, sizeof(p))) remove(p);
}

static void remove_part(const char *part)
{
    remove(part);
    remove_id(part);                                /* 8042: and its .tmp */
}

/* 8041: the validator a later If-Range can use: a strong ETag, else the
 * date. A weak ETag (W/"...") is not allowed in If-Range. */
static const char *validator(const hdrs_t *h)
{
    if (h->etag[0] && strncmp(h->etag, "W/", 2) != 0) return h->etag;
    return h->lastmod;
}

static void drop_part_dir(const char *dir)
{
    static char p[DL_PART_MAX];
    if (podpath_part(dir, NULL, p, sizeof(p))) rmdir(p);
}

/* 8021, moved by 8029: the podcast's folder and its way back, now that
 * an episode is whole and about to go in. Rewritten each time, so a
 * renamed podcast's folder follows; a failure costs only the link. */
static bool finish_dir(void)
{
    static char link[DL_LINK_MAX], text[160];
    if (!mk(s_dir)) return false;
    snprintf(link, sizeof(link), "%s/" PODPATH_LINK, s_dir);
    const int n = podpath_link_format(s_feed, s_show, text, sizeof(text));
    FILE *lf = fopen(link, "w");
    const bool wrote = lf && n > 0 && fwrite(text, 1, (size_t)n, lf) == (size_t)n;
    if (lf) fclose(lf);
    if (!wrote) ESP_LOGW(TAG, "could not write %s", link);
    return true;
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

/* Open `url`, following redirects; the client, positioned at the body,
 * with its status in *status, or NULL. `url` is rewritten with the
 * address that answered. */
static esp_http_client_handle_t open_followed(char *url, hdrs_t *h, int *status, int64_t from, int64_t to)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT, desc && desc->version[0] ? desc->version : "0");
    *status = 0;
    for (int hop = 0; hop <= DL_HOPS; hop++) {
        h->location[0] = h->ctype[0] = '\0';
        h->etag[0] = h->lastmod[0] = h->crange[0] = h->ranges[0] = '\0';   /* 8041, 8043 */
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
        if (from > 0 || to >= 0) {                  /* 8024: the rest of a part file; 8042: or a segment */
            char range[48];
            if (to >= from)                         /* 8042: one segment's bytes */
                snprintf(range, sizeof(range), "bytes=%lld-%lld", (long long)from, (long long)to);
            else
                snprintf(range, sizeof(range), "bytes=%lld-", (long long)from);
            esp_http_client_set_header(c, "Range", range);
            /* 8041: and only if it is still the same file -- else the
             * server sends all of it, 200, and this starts over. */
            if (h->if_range[0]) esp_http_client_set_header(c, "If-Range", h->if_range);
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
    esp_http_client_handle_t c = open_followed(url, &h, &status, 0, -1);
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
    /* The guard on entry holds strlen(stem) to DL_STEM_MAX - 1; the
     * precision says so to the compiler, which -Os (8031) no longer
     * finds out for itself across the inlining. */
    snprintf(path, sizeof(path), "%.*s.%s", DL_STEM_MAX - 1, stem, ext);
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

/*
 * 8036: THE CARD WRITER. Before this the task read 16 KB and then wrote
 * it, and while it wrote nobody drained the socket: the 16 KB window
 * filled and the server waited, so 18-30% of every download was the card
 * (8034's `write` share). Now dl_task only reads, into one of DL_NBUF
 * buffers, and hands each to this task, which takes the lease and
 * writes. A chunk with len 0 is the end; the writer then gives s_wr.done
 * and exits. A write that fails sets s_wr.err, and the rest are dropped
 * unwritten -- the reader sees err and stops.
 */
/* 8042: and where: the segment the chunk is from, and its place in the
 * file. The writer seeks only when that is not where the last chunk
 * ended, so one segment written in order never seeks. */
typedef struct { uint8_t *p; int len; int seg; int64_t off; } wr_item_t;
static struct {
    QueueHandle_t     full, free_;
    SemaphoreHandle_t done;
    FILE             *f;
    volatile bool     err;
    volatile int      err_no;
    volatile int64_t  put;                  /* bytes written */
    volatile int64_t  t_lease, t_write;     /* 8034's shares, from this side */
    /* 8042: the map, which the writer keeps, since only it knows what is
     * on the card; saved every DL_MAP_EVERY after a flush and an fsync. */
    int64_t           pos;                  /* where the file is; -1 unknown */
    seg_t             seg[DL_SEGS_MAX];
    int               nseg;
    int64_t           total, since_map;
    const char       *part;
    char              val[96];
} s_wr;

static void wr_task(void *arg)
{
    (void)arg;
    wr_item_t it;
    for (;;) {
        xQueueReceive(s_wr.full, &it, portMAX_DELAY);
        if (it.len <= 0) break;
        if (!s_wr.err) {
            const int64_t t0 = esp_timer_get_time();
            storage_io_acquire(STORAGE_IO_BACKGROUND);
            const int64_t t1 = esp_timer_get_time();
            /* 8042: fseek() takes a long, so past 2 GB it cannot be asked;
             * dl_task keeps such a file to one segment, which never seeks. */
            bool at = it.off == s_wr.pos;
            if (!at && it.off <= LONG_MAX) at = fseek(s_wr.f, (long)it.off, SEEK_SET) == 0;
            const size_t put = at ? fwrite(it.p, 1, (size_t)it.len, s_wr.f) : 0;
            if (put == (size_t)it.len) {
                s_wr.pos = it.off + it.len;
                s_wr.seg[it.seg].done += it.len;
                s_wr.since_map += it.len;
                if (s_wr.since_map >= DL_MAP_EVERY) {
                    s_wr.since_map = 0;
                    if (fflush(s_wr.f) == 0) {
                        fsync(fileno(s_wr.f));
                        id_write(s_wr.part, s_wr.val, s_wr.total, s_wr.seg, s_wr.nseg);
                    }
                }
            } else {
                s_wr.pos = -1;
            }
            storage_io_release();
            const int64_t t2 = esp_timer_get_time();
            s_wr.t_lease += t1 - t0;
            s_wr.t_write += t2 - t1;
            if (put != (size_t)it.len) { s_wr.err_no = errno; s_wr.err = true; }
            else s_wr.put += it.len;
        }
        xQueueSend(s_wr.free_, &it, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "writer: stack %u bytes never used of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)DL_WR_STACK);
    xSemaphoreGive(s_wr.done);
    vTaskDelete(NULL);
}

/*
 * 8043: THE READERS. A download in segments (8042's map) has a reader a
 * segment: dl_task for the first not yet whole, on the connection that
 * asked for the file, and up to DL_CONNS - 1 helpers for the others,
 * each on its own connection. One stream is bounded by its TCP window a
 * round trip (8034-8036: 99% of the time waiting on the network), so
 * three streams move about three windows a round trip.
 *
 * Every reader takes a free buffer, reads at most to its segment's end,
 * and hands the chunk to the one writer (8036) with its segment and
 * offset; the writer keeps the map. s_rcv[k] is the next byte segment k's
 * reader wants, written only by that reader. A read or write failure on
 * any of them sets s_abort and the rest stop at their next chunk; the
 * part file and its map stay, and a tap goes on from them.
 */
enum { SRV_DONE = 0, SRV_RERR, SRV_WERR, SRV_STOP };

static seg_t           s_plan[DL_SEGS_MAX];
static int             s_nplan;
static int64_t         s_rcv[DL_SEGS_MAX];
static volatile bool   s_abort;

typedef struct {
    int64_t t_net, t_wait, t_mark, got_mark, w_net, w_wait, wl_mark, ww_mark;
} dl_timing_t;

/* 8043: set_progress() for several readers: bytes added, not a total. */
static int add_progress(int n)
{
    lock();
    s_got += n;
    const int64_t step = s_got * 10 >> 20;
    if (step != s_step) { s_step = step; s_gen++; }
    const int stop = s_stop;
    unlock();
    return stop;
}

/* Read segment k from c until its end, the stream's end, an error or a
 * stop. `tm` is dl_task's 8034 timing, NULL for a helper. */
static int serve(esp_http_client_handle_t c, int k, dl_timing_t *tm)
{
    for (;;) {
        if (s_abort) return SRV_STOP;
        wr_item_t it;
        const int64_t t0 = esp_timer_get_time();
        xQueueReceive(s_wr.free_, &it, portMAX_DELAY);
        const int64_t t1 = esp_timer_get_time();
        if (tm) { tm->t_wait += t1 - t0; tm->w_wait += t1 - t0; }
        if (s_wr.err) { xQueueSend(s_wr.free_, &it, 0); return SRV_WERR; }
        const int64_t off = s_rcv[k];
        int want = DL_CHUNK;
        if (s_plan[k].end >= 0) {                   /* 8042: no further than the segment */
            const int64_t left = s_plan[k].end - off;
            if (left <= 0) { xQueueSend(s_wr.free_, &it, 0); return SRV_DONE; }
            if (left < want) want = (int)left;
        }
        const int n = esp_http_client_read(c, (char *)it.p, want);
        const int64_t t2 = esp_timer_get_time();
        if (tm) { tm->t_net += t2 - t1; tm->w_net += t2 - t1; }
        if (n <= 0) {
            xQueueSend(s_wr.free_, &it, 0);
            if (n < 0) ESP_LOGW(TAG, "segment %d: read failed at %lld", k + 1, (long long)off);
            return n < 0 ? SRV_RERR : SRV_DONE;
        }
        it.len = n;
        it.seg = k;
        it.off = off;
        s_rcv[k] = off + n;
        xQueueSend(s_wr.full, &it, portMAX_DELAY);
        const int stop = add_progress(n);
        if (tm && t2 - tm->t_mark >= 5000000) {
            const int64_t span = t2 - tm->t_mark;
            const int64_t wl = s_wr.t_lease, ww = s_wr.t_write;
            lock();
            const int64_t got = s_got;
            unlock();
            ESP_LOGI(TAG, "timing 5s: %lld kbit/s, net %d%%, wait %d%%, lease %d%%, write %d%%, "
                     "internal free %u min %u, TCP_WND %d, %d connection(s)",
                     (long long)((got - tm->got_mark) * 8000 / span),
                     (int)(tm->w_net * 100 / span), (int)(tm->w_wait * 100 / span),
                     (int)((wl - tm->wl_mark) * 100 / span), (int)((ww - tm->ww_mark) * 100 / span),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (int)TCP_WND, s_nplan > 1 ? s_nplan : 1);
            tm->t_mark = t2; tm->got_mark = got;
            tm->wl_mark = wl; tm->ww_mark = ww;
            tm->w_net = tm->w_wait = 0;
        }
        if (stop != RUN) return SRV_STOP;
    }
}

/* Open segment k's rest on a connection of its own, checked as 8041 and
 * 8042 check a resume. NULL if the server will not give exactly that. */
static esp_http_client_handle_t open_segment(char *url, hdrs_t *h, int k)
{
    const int64_t from = s_rcv[k], to = s_plan[k].end - 1;
    int status = 0;
    snprintf(h->if_range, sizeof(h->if_range), "%s", s_wr.val);
    esp_http_client_handle_t c = open_followed(url, h, &status, from, to);
    if (!c) return NULL;
    long long a = -1, b = -1, t = -1;
    const int n = sscanf(h->crange, "bytes %lld-%lld/%lld", &a, &b, &t);
    if (status == 206 && n >= 2 && a == from && b == to && (n < 3 || s_wr.total <= 0 || t == s_wr.total))
        return c;
    ESP_LOGW(TAG, "segment %d: HTTP %d, Content-Range \"%s\" for %lld-%lld; not this way",
             k + 1, status, h->crange, (long long)from, (long long)to);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return NULL;
}

/*
 * The helpers: created once and never deleted, as recorder.c's tasks are
 * (6010) -- a task made per download would take its stack's worth from
 * whichever heap at the moment of the tap, and a WithCaps task that
 * deletes itself spawns a helper from internal RAM to free it (mpd.c).
 * Stacks in PSRAM; TCBs internal, DRAM_ATTR (CLAUDE.md).
 */
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "poddl.c puts its helper stacks in PSRAM and needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
#endif
#define DL_HELPERS (DL_CONNS - 1)
_Static_assert(DL_CONNS <= NETBUDGET_PODDL, "netbudget.h counts fewer sockets for poddl than DL_CONNS");
_Static_assert(DL_CONNS <= DL_SEGS_MAX, "a segment a connection");
static DRAM_ATTR StaticTask_t s_help_tcb[DL_HELPERS > 0 ? DL_HELPERS : 1];
static struct {
    TaskHandle_t  task;
    StackType_t  *stack;
    int           seg;
    volatile int  res;
    char          url[DL_URL_MAX];
    hdrs_t        h;
} s_help[DL_HELPERS > 0 ? DL_HELPERS : 1];
static SemaphoreHandle_t s_help_done;

static void help_task(void *arg)
{
    const int j = (int)(intptr_t)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int k = s_help[j].seg;
        esp_http_client_handle_t c = open_segment(s_help[j].url, &s_help[j].h, k);
        if (!c) {
            s_help[j].res = SRV_DONE;               /* not served: dl_task takes it after */
        } else {
            s_help[j].res = serve(c, k, NULL);
            if (s_help[j].res == SRV_RERR || s_help[j].res == SRV_WERR) s_abort = true;
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
        }
        xSemaphoreGive(s_help_done);
    }
}

static bool helpers_ready(void)
{
    if (DL_HELPERS <= 0) return false;
    if (!s_help_done) s_help_done = xSemaphoreCreateCounting(DL_HELPERS > 0 ? DL_HELPERS : 1, 0);
    if (!s_help_done) return false;
    for (int j = 0; j < DL_HELPERS; j++) {
        if (s_help[j].task) continue;
        if (!s_help[j].stack)
            s_help[j].stack = heap_caps_malloc(DL_HELP_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_help[j].stack) return false;
        s_help[j].task = xTaskCreateStatic(help_task, "poddl_h", DL_HELP_STACK, (void *)(intptr_t)j,
                                           3, s_help[j].stack, &s_help_tcb[j]);
        if (!s_help[j].task) return false;
    }
    return true;
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
    uint8_t *buf = heap_caps_malloc((size_t)DL_NBUF * DL_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  /* 8036 */
    esp_http_client_handle_t c = NULL;
    FILE *f = NULL;
    int status = 0;
    if (!buf || !make_part_dirs(s_dir)) goto out;
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
    if (!podpath_part(s_dir, s_base, part, sizeof(part))) goto out;     /* 8029 */
    {
        /* 8029: a part file where 8024-8028 kept it, in the podcast's
         * own folder, is moved here and taken up from, rather than left
         * behind while this starts again from nothing. */
        static char old[DL_FILE_MAX];
        struct stat ost;
        snprintf(old, sizeof(old), "%s/%s.part", s_dir, s_base);
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (stat(part, &ost) != 0 && stat(old, &ost) == 0 && S_ISREG(ost.st_mode)) {
            if (rename(old, part) == 0) ESP_LOGI(TAG, "moved an earlier part file to %s", part);
        }
        storage_io_release();
    }
    /*
     * 8042: what is on the card already, as segments (see id_read()). One
     * segment written in order -- every part file before 8043 -- is
     * 8024's resume: the rest from the file's size, which is never past
     * what was written. More than one is resumed from the map: the first
     * segment not yet whole is this connection's.
     */
    static seg_t seg[DL_SEGS_MAX];
    int nseg = 0;
    int64_t id_total = -1;                          /* 8041 */
    {
        struct stat pst;
        int64_t psize = 0;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (stat(part, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) psize = (int64_t)pst.st_size;
        const int n = psize > 0 ? id_read(part, h.if_range, sizeof(h.if_range), &id_total, seg, DL_SEGS_MAX) : -1;
        storage_io_release();
        if (psize > 0 && n < 0) ESP_LOGI(TAG, "part file has no .id (from before 8041); checking Content-Range only");
        if (psize > 0 && n <= 1) {
            seg[0].start = 0;
            seg[0].end = id_total > 0 ? id_total : -1;
            seg[0].done = psize;
            nseg = 1;
        } else if (n > 1) {
            nseg = n;
        }
    }
    int k0 = 0;                                     /* this connection's segment */
    while (k0 < nseg && seg[k0].end >= 0 && seg[k0].done >= seg[k0].end - seg[k0].start) k0++;
    if (nseg > 0 && k0 == nseg) {
        /* Whole, but not renamed: a reboot between the two. Rare enough
         * that the file is asked for again rather than finished blind. */
        ESP_LOGI(TAG, "part file's map says it is whole; starting over");
        nseg = 0;
        k0 = 0;
        h.if_range[0] = '\0';
    }
    int64_t from = nseg ? seg[k0].start + seg[k0].done : 0;
    int64_t to = (nseg > 1 && seg[k0].end > 0) ? seg[k0].end - 1 : -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        c = open_followed(url, &h, &status, from, to);
        if (!c) goto out;
        /* 8041: a 206 is the rest of the same file only if it says so. */
        const char *bad = NULL;
        if (status == 206 && (from > 0 || to >= 0)) {
            long long a = -1, b = -1, t = -1;
            const int n = sscanf(h.crange, "bytes %lld-%lld/%lld", &a, &b, &t);
            if (n < 2) bad = "no Content-Range";
            else if (a != from) bad = "Content-Range starts elsewhere";
            else if (to >= 0 && b != to) bad = "Content-Range ends elsewhere";              /* 8042 */
            else if (n == 3 && id_total > 0 && t != id_total) bad = "the file's size changed";
            else if (n == 3 && id_total <= 0) id_total = t;                                 /* 8042 */
            if (bad) ESP_LOGW(TAG, "resume refused, %s (%s, from %lld, saved total %lld)",
                              bad, h.crange, (long long)from, (long long)id_total);
        }
        if ((status == 416 && nseg > 0) || bad) {
            if (!bad) ESP_LOGI(TAG, "416 for the rest from %lld; starting over", (long long)from);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            c = NULL;
            lock(); snprintf(url, sizeof(url), "%s", s_url); unlock();
            nseg = 0; k0 = 0; from = 0; to = -1;
            id_total = -1;
            h.if_range[0] = '\0';
            continue;
        }
        break;
    }
    if (status == 200) {
        if (nseg > 0) ESP_LOGI(TAG, "200 for the rest from %lld: %s; starting over", (long long)from,
                               h.if_range[0] ? "the file changed" : "no ranges");
        nseg = 0; k0 = 0; from = 0; to = -1;
    } else if (status != 206 || nseg == 0) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto out;
    }

    char ext[8];
    podpath_url_ext(url, ext, sizeof(ext));
    if (!ext[0]) snprintf(ext, sizeof(ext), "%s", podpath_type_ext(h.ctype));
    if (!ext[0]) snprintf(ext, sizeof(ext), "mp3");
    const bool fresh = nseg == 0;                   /* 8042 */
    if (fresh) {
        const int64_t clen = esp_http_client_get_content_length(c);
        id_total = clen > 0 ? clen : -1;
        seg[0].start = 0;
        seg[0].end = id_total;
        seg[0].done = 0;
        nseg = 1;
        /* 8043: in pieces, when the server does ranges, the file is big
         * enough to be worth it, and small enough to seek in (8042). This
         * connection's 200 is the first piece, read only to its end. */
        if (DL_CONNS > 1 && id_total >= DL_SPLIT_MIN && id_total <= LONG_MAX &&
            !strcasecmp(h.ranges, "bytes") && helpers_ready()) {
            nseg = DL_CONNS;
            for (int i = 0; i < nseg; i++) {
                seg[i].start = id_total * i / nseg;
                seg[i].end = id_total * (i + 1) / nseg;
                seg[i].done = 0;
            }
        } else if (DL_CONNS > 1 && id_total >= DL_SPLIT_MIN) {
            ESP_LOGI(TAG, "one connection: Accept-Ranges \"%s\"%s", h.ranges,
                     id_total > LONG_MAX ? ", and past 2 GB" : "");
        }
    } else if (seg[0].end < 0 && id_total > 0 && nseg == 1) {
        seg[0].end = id_total;                      /* a part file from before 8041 */
    }
    int64_t have = 0;                               /* on the card already */
    for (int i = 0; i < nseg; i++) have += seg[i].done;
    lock();
    snprintf(s_path, sizeof(s_path), "%s/%s.%s", s_dir, s_base, ext);
    s_total = id_total;
    s_got = have;
    unlock();

    /* 8042: one segment appends, as before; a map writes in place. */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    f = storage_io_open(part, fresh ? "wb" : nseg == 1 ? "ab" : "r+b");
    if (f) {
        snprintf(s_wr.val, sizeof(s_wr.val), "%s", fresh || !h.if_range[0] ? validator(&h) : h.if_range);
        id_write(part, s_wr.val, id_total, seg, nseg);
    }
    storage_io_release();
    if (!f) {
        ESP_LOGW(TAG, "could not open %s", part);
        goto out;
    }
    ESP_LOGI(TAG, "to %s (%lld bytes%s, from %lld, segment %d of %d)", s_path, (long long)s_total,
             s_total < 0 ? ", length not given" : "", (long long)from, k0 + 1, nseg);

    int64_t got = have;                 /* received; s_wr.seg[].done is written */
    bool ok = true;
    int stop = RUN;

    /* 8036: the writer, its queues and buffers. */
    if (!s_wr.full) {
        s_wr.full  = xQueueCreate(DL_NBUF + 1, sizeof(wr_item_t));
        s_wr.free_ = xQueueCreate(DL_NBUF, sizeof(wr_item_t));
        s_wr.done  = xSemaphoreCreateBinary();
    }
    if (!s_wr.full || !s_wr.free_ || !s_wr.done) {
        ESP_LOGW(TAG, "no memory for the writer's queues");
        ok = false;
        goto closed;
    }
    xQueueReset(s_wr.full);
    xQueueReset(s_wr.free_);
    xSemaphoreTake(s_wr.done, 0);
    for (int i = 0; i < DL_NBUF; i++) {
        wr_item_t it = { buf + (size_t)i * DL_CHUNK, 0, 0, 0 };
        xQueueSend(s_wr.free_, &it, 0);
    }
    s_wr.f = f;
    s_wr.err = false;
    s_wr.err_no = 0;
    s_wr.put = 0;
    s_wr.t_lease = s_wr.t_write = 0;
    /* 8042: the map the writer keeps, and where the file is: at its end
     * for "ab" and "wb", unknown for "r+b" so the first chunk seeks. */
    memcpy(s_wr.seg, seg, sizeof(seg));
    s_wr.nseg = nseg;
    s_wr.total = id_total;
    s_wr.since_map = 0;
    s_wr.part = part;
    s_wr.pos = nseg == 1 ? from : -1;
    if (xTaskCreate(wr_task, "poddl_wr", DL_WR_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the writer");
        ok = false;
        goto closed;
    }

    /*
     * 8034, for 8036: net is time in esp_http_client_read; wait is the
     * reader waiting for a free buffer -- the card falling behind; lease
     * and write are the writer's, and overlap net now, so the four need
     * not add to 100%. 8043: net and wait are dl_task's connection's; the
     * rate is all of them.
     */
    memcpy(s_plan, seg, sizeof(seg));
    s_nplan = nseg;
    for (int i = 0; i < nseg; i++) s_rcv[i] = seg[i].start + seg[i].done;
    if (nseg == 1) s_rcv[0] = from;                 /* 8042: the size, not the map */
    s_abort = false;
    const int64_t t_start = esp_timer_get_time();
    dl_timing_t tm = { .t_mark = t_start, .got_mark = got };

    /* 8043: a helper for each other segment not yet whole. */
    int helping = 0;
    if (nseg > 1 && helpers_ready()) {
        for (int i = 0; i < nseg && helping < DL_HELPERS; i++) {
            if (i == k0 || s_rcv[i] >= seg[i].end) continue;
            snprintf(s_help[helping].url, sizeof(s_help[helping].url), "%s", url);
            s_help[helping].seg = i;
            s_help[helping].res = SRV_DONE;
            xTaskNotifyGive(s_help[helping].task);
            helping++;
        }
    }

    int res = serve(c, k0, &tm);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    c = NULL;
    if (res == SRV_RERR || res == SRV_WERR) s_abort = true;
    for (int j = 0; j < helping; j++) xSemaphoreTake(s_help_done, portMAX_DELAY);
    for (int j = 0; j < helping && res == SRV_DONE; j++)
        if (s_help[j].res != SRV_DONE) res = s_help[j].res;

    /* 8043: a segment nobody finished -- its helper could not get it, or
     * the map has more segments than there are helpers -- is dl_task's,
     * one after another, each on a connection of its own. */
    static hdrs_t h2;
    for (int i = 0; i < nseg && res == SRV_DONE && nseg > 1; i++) {
        if (s_rcv[i] >= seg[i].end) continue;
        esp_http_client_handle_t c2 = open_segment(url, &h2, i);
        if (!c2) { res = SRV_RERR; break; }
        res = serve(c2, i, &tm);
        esp_http_client_close(c2);
        esp_http_client_cleanup(c2);
    }
    if (res == SRV_RERR) ok = false;
    if (res == SRV_WERR) {
        ok = false;
        ESP_LOGW(TAG, "write failed after %lld bytes (errno %d)",
                 (long long)(have + s_wr.put), s_wr.err_no);
    }
    lock();
    stop = s_stop;
    unlock();
    if (res != SRV_STOP && stop != RUN) stop = RUN;     /* asked after the last chunk: done is done */
    const int64_t t_net = tm.t_net, t_wait = tm.t_wait;
    /* 8036: the end marker, then wait for every chunk before it to land. */
    {
        const wr_item_t end = { NULL, 0, 0, 0 };     /* 8044: every field, for -Wextra */
        xQueueSend(s_wr.full, &end, portMAX_DELAY);
        xSemaphoreTake(s_wr.done, portMAX_DELAY);
    }
    if (s_wr.err) {
        if (ok) ESP_LOGW(TAG, "write failed after %lld bytes (errno %d)",
                         (long long)(have + s_wr.put), s_wr.err_no);
        ok = false;
    }
    /* 8042: what the part file holds, by the writer's map, which is saved
     * once more now so a pause or a failure resumes from all of it. */
    got = 0;
    for (int i = 0; i < s_wr.nseg; i++) got += s_wr.seg[i].done;
    if (stop != STOP_CANCEL) {
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (fflush(f) == 0) fsync(fileno(f));
        id_write(part, s_wr.val, s_wr.total, s_wr.seg, s_wr.nseg);
        storage_io_release();
    }
    {
        const int64_t span = esp_timer_get_time() - t_start;
        if (span > 0)
            ESP_LOGI(TAG, "timing total: %lld bytes in %lld ms = %lld kbit/s; net %d%%, wait %d%%, "
                     "lease %d%%, write %d%%",
                     (long long)(got - have), (long long)(span / 1000),
                     (long long)((got - have) * 8000 / span),
                     (int)(t_net * 100 / span), (int)(t_wait * 100 / span),
                     (int)(s_wr.t_lease * 100 / span), (int)(s_wr.t_write * 100 / span));
    }
closed:
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
        remove_part(part);                          /* 8041: and its .id */
        drop_part_dir(s_dir);                       /* 8029 */
        drop_empty_podcast_dir(s_dir);
        storage_io_release();
        st = PODDL_IDLE;
        ESP_LOGI(TAG, "cancelled at %lld bytes", (long long)got);
        goto out;
    }
    if (ok && s_total > 0 && got != s_total) {
        ESP_LOGW(TAG, "ended at %lld of %lld bytes", (long long)got, (long long)s_total);
        ok = false;
    }
    if (ok && !finish_dir()) ok = false;           /* 8029: the folder, now */
    if (ok) {
        remove(s_path);                  /* a stale one, if any; rename will not replace */
        if (rename(part, s_path) != 0) {
            ESP_LOGW(TAG, "could not rename %s (errno %d)", part, errno);
            ok = false;
        }
    }
    /* 8024: a part that failed is kept too -- a dropped link is the usual
     * reason, and the next tap takes up from it. Only a cancel removes it. */
    if (ok) {
        remove_id(part);                            /* 8041, 8042: the part's .id goes with it */
        drop_part_dir(s_dir);                       /* 8029 */
    }
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
    static char dir[DL_PATH_MAX];
    if (st == PODDL_PAUSED || st == PODDL_FAILED) {
        if (!podpath_part(s_dir, s_base, part, sizeof(part))) part[0] = '\0';   /* 8029 */
        snprintf(dir, sizeof(dir), "%s", s_dir);
        s_state = PODDL_IDLE;
        s_gen++;
    }
    unlock();
    if (st == PODDL_PAUSED || st == PODDL_FAILED) {
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (part[0]) remove_part(part);             /* 8041 */
        drop_part_dir(dir);
        drop_empty_podcast_dir(dir);
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

void poddl_discard(const char *dir, const char *base)
{
    static char p[DL_PART_MAX];
    lock();
    const bool running = s_state == PODDL_RUNNING;
    unlock();
    if (running || !dir || !base) return;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (podpath_part(dir, base, p, sizeof(p))) remove_part(p);    /* 8041 */
    if (snprintf(p, sizeof(p), "%s/%s.part", dir, base) < (int)sizeof(p)) remove(p);   /* 8024-8028's */
    drop_part_dir(dir);
    drop_empty_podcast_dir(dir);
    storage_io_release();
    lock();
    if (s_state != PODDL_RUNNING) { s_state = PODDL_IDLE; s_gen++; }
    unlock();
    ESP_LOGI(TAG, "part file discarded");
}

bool poddl_part(const char *dir, const char *base, int64_t *size)
{
    static char p[DL_PART_MAX];
    struct stat st;
    if (!podpath_part(dir, base, p, sizeof(p))) return false;          /* 8029 */
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
        /* 8029: or where 8024-8028 kept it; dl_task() moves it over. */
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
