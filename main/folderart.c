/*
 * folderart.c -- the folder's cover, found once per folder and read
 * once per album. See folderart.h for what and why; this is how.
 *
 * WHAT A LOOKUP COSTS, AND WHY IT IS REMEMBERED
 *
 * Finding the file is a stat() per candidate name, and on FAT each one
 * is a walk of the folder's directory entries -- twenty walks of an
 * album folder for a folder with nothing in it. An album plays its
 * tracks in a row, so the answer is remembered per folder, found or
 * not: FOLDERART_SLOTS folders, by hash, the oldest replaced. The second
 * track of an album pays nothing to look.
 *
 * Reading it is the bigger cost: a folder.jpg is often a megabyte, and
 * every track of the album wants the same one. The cover cache already
 * shares identical images between paths (mediacache_put_art()), but only
 * after they have been read. So the last track that got a folder cover
 * is remembered as the donor, and the next track in the same folder
 * copies the picture out of the cache -- a memcpy in PSRAM instead of a
 * read off the card that playback shares. The donor is usually still
 * cached: the cache holds previous, current and next, and the previous
 * is pinned.
 *
 * All of this is media_task's, like do_art() that calls it, so no lock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "folderart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "albumart.h"           /* albumart_is_supported_image() */
#include "covertag.h"           /* COVERTAG_MAX_IMAGE */
#include "mediacache.h"
#include "storage_io.h"

static const char *TAG = "tab5_folderart";

#define FOLDERART_SLOTS     (4)
#define FOLDERART_PATH_MAX  (640)   /* a 512-byte track path's folder + a name */

typedef struct {
    uint32_t hash;          /* 0: empty */
    int8_t   found;         /* index into FOLDERART_NAMES, or -1 for none */
    bool     own;           /* 8027: a podcast folder: its tracks may have their own */
} slot_t;

static slot_t   s_slot[FOLDERART_SLOTS];
static int      s_next_slot;

/* PSRAM, allocated on first use and never freed: internal RAM is what
 * runs out, and these are only ever touched on media_task. */
static char    *s_dir;          /* FOLDERART_PATH_MAX */
static char    *s_path;         /* FOLDERART_PATH_MAX */
static char    *s_donor;        /* FOLDERART_PATH_MAX: a cache key with this folder's cover */
static uint32_t s_donor_hash;

static bool ensure(void)
{
    if (s_dir) return true;
    char *b = heap_caps_malloc(3 * FOLDERART_PATH_MAX, MALLOC_CAP_SPIRAM);
    if (!b) return false;
    s_dir = b;
    s_path = b + FOLDERART_PATH_MAX;
    s_donor = b + 2 * FOLDERART_PATH_MAX;
    s_dir[0] = s_path[0] = s_donor[0] = '\0';
    return true;
}

void folderart_forget(void)
{
    memset(s_slot, 0, sizeof(s_slot));
    s_next_slot = 0;
    s_donor_hash = 0;
    if (s_donor) s_donor[0] = '\0';
}

static slot_t *slot_find(uint32_t h)
{
    for (int i = 0; i < FOLDERART_SLOTS; i++)
        if (s_slot[i].hash == h) return &s_slot[i];
    return NULL;
}

static slot_t *slot_new(uint32_t h, int found)
{
    slot_t *s = &s_slot[s_next_slot];
    s_next_slot = (s_next_slot + 1) % FOLDERART_SLOTS;
    s->hash = h;
    s->found = (int8_t)found;
    s->own = false;
    return s;
}

/*
 * 8027: whether s_dir is a podcast's folder -- it holds the downloader's
 * ".podcast" link (podpath.h's PODPATH_LINK; the name is written out
 * here rather than included, because folderart.c has no business with
 * the rest of podpath.h). Only such a folder's tracks are looked for
 * their own pictures, so every other folder keeps costing one look per
 * folder rather than one per track.
 */
static bool is_podcast_dir(void)
{
    const int k = snprintf(s_path, FOLDERART_PATH_MAX, "%s/.podcast", s_dir);
    struct stat st;
    return k > 0 && k < FOLDERART_PATH_MAX && stat(s_path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The candidate's full path into s_path. False if it will not fit. */
static bool candidate(int i)
{
    const int k = snprintf(s_path, FOLDERART_PATH_MAX, "%s/%s", s_dir, FOLDERART_NAMES[i]);
    return k > 0 && k < FOLDERART_PATH_MAX;
}

/* The first name in the folder that exists as a regular file, or -1. */
static int look(long *size)
{
    struct stat st;
    for (int i = 0; i < FOLDERART_NNAMES; i++) {
        if (!candidate(i)) continue;
        if (stat(s_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            *size = (long)st.st_size;
            return i;
        }
    }
    return -1;
}

/* Copy the donor's cached cover, if the donor is this folder's and the
 * cache still has it. */
static bool from_donor(uint32_t h, uint8_t **img, size_t *len)
{
    if (!s_donor[0] || s_donor_hash != h) return false;
    size_t n = 0;
    const uint8_t *c = mediacache_art(s_donor, &n);
    if (!c || !n) return false;
    uint8_t *b = malloc(n);
    if (!b) return false;
    memcpy(b, c, n);
    *img = b;
    *len = n;
    return true;
}

static esp_err_t read_image(long size, uint8_t **img, size_t *len)
{
    if (size <= 0 || (unsigned long)size > COVERTAG_MAX_IMAGE) {
        ESP_LOGW(TAG, "%s is %ld bytes; over the %u byte cover limit",
                 s_path, size, (unsigned)COVERTAG_MAX_IMAGE);
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = storage_io_open(s_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    /* malloc, not PSRAM by caps: mediacache_put_art() free()s it, and
     * covertag's pictures come from malloc for the same reason. */
    uint8_t *b = malloc((size_t)size);
    if (!b) {
        storage_io_close(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t got = storage_io_fread(b, (size_t)size, f, STORAGE_IO_PLAYBACK);
    storage_io_close(f);
    if (got != (size_t)size) {
        free(b);
        return ESP_FAIL;
    }
    if (!albumart_is_supported_image(b, got)) {
        free(b);
        return ESP_ERR_NOT_SUPPORTED;
    }
    *img = b;
    *len = got;
    return ESP_OK;
}

bool folderart_in_hand(const char *file)
{
    if (!ensure() || !folderart_dir_of(file, s_dir, FOLDERART_PATH_MAX)) return true;
    const uint32_t h = folderart_dir_hash(s_dir);
    const slot_t *s = slot_find(h);
    if (!s) return false;                       /* never looked */
    if (s->own) return false;                   /* 8027: each track may have its own */
    if (s->found < 0) return true;              /* looked: none */
    return s_donor[0] && s_donor_hash == h && mediacache_art(s_donor, NULL) != NULL;
}

static void set_donor(const char *key, const char *file, uint32_t h)
{
    snprintf(s_donor, FOLDERART_PATH_MAX, "%s", key ? key : file);
    s_donor_hash = h;
}

esp_err_t folderart_load(const char *file, const char *key,
                         uint8_t **img, size_t *len, const char **name)
{
    *img = NULL;
    *len = 0;
    if (name) *name = NULL;
    if (!ensure()) return ESP_ERR_NO_MEM;
    if (!folderart_dir_of(file, s_dir, FOLDERART_PATH_MAX)) return ESP_ERR_NOT_FOUND;
    const uint32_t h = folderart_dir_hash(s_dir);

    slot_t *s = slot_find(h);

    /*
     * 8027: the folder is looked at once, as before, and now also asked
     * whether it is a podcast's. In one that is, the track's own picture
     * -- ".<file>.jpg" (or png, gif) beside it, which the downloader
     * writes when an episode's picture is not its podcast's -- comes
     * before the folder's. Not remembered as the donor: it is this
     * track's, not the folder's.
     */
    long size = 0;
    int found = -1;
    const bool fresh = (s == NULL);
    if (fresh) {
        found = look(&size);
        s = slot_new(h, found);
        s->own = is_podcast_dir();
    }
    if (s->own) {
        static const char *const own_ext[] = { "jpg", "png", "gif" };
        for (int i = 0; i < 3; i++) {
            struct stat ost;
            if (!folderart_own_path(file, own_ext[i], s_path, FOLDERART_PATH_MAX)) break;
            if (stat(s_path, &ost) != 0 || !S_ISREG(ost.st_mode) || ost.st_size <= 0) continue;
            if (read_image((long)ost.st_size, img, len) == ESP_OK) {
                /* 8055: the file's name, for the sidecar (was "its own
                 * picture", which nothing read). */
                if (name) { const char *b = strrchr(s_path, '/'); *name = b ? b + 1 : s_path; }
                ESP_LOGI(TAG, "the track's own picture (%u KB)", (unsigned)(*len / 1024));
                return ESP_OK;
            }
        }
    }
    if (!fresh && s->found < 0) return ESP_ERR_NOT_FOUND;   /* asked before: none */

    if (s && from_donor(h, img, len)) {
        if (name) *name = FOLDERART_NAMES[s->found];
        ESP_LOGI(TAG, "%s again, from the cover cache (%u KB)",
                 FOLDERART_NAMES[s->found], (unsigned)(*len / 1024));
        set_donor(key, file, h);
        return ESP_OK;
    }

    if (!fresh) {
        /* Known name; only the bytes are missing. */
        found = s->found;
        struct stat st;
        if (!candidate(found) || stat(s_path, &st) != 0) {
            s->found = -1;                  /* gone since: look no more */
            return ESP_ERR_NOT_FOUND;
        }
        size = (long)st.st_size;
    } else if (found < 0) {
        ESP_LOGI(TAG, "no cover image in %.120s", s_dir);
        return ESP_ERR_NOT_FOUND;
    } else if (!candidate(found)) {
        return ESP_ERR_NOT_FOUND;               /* 8027: s_path to the folder's, again */
    }

    const esp_err_t err = read_image(size, img, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", s_path, esp_err_to_name(err));
        /* A file that is not a picture, or will not fit, will not become
         * one: remembered as none. A failed allocation might pass next
         * time, so it is not. */
        if (err != ESP_ERR_NO_MEM) s->found = -1;
        return err == ESP_ERR_NO_MEM ? err : ESP_ERR_NOT_FOUND;
    }
    if (name) *name = FOLDERART_NAMES[found];
    ESP_LOGI(TAG, "%s from the folder (%u KB)", FOLDERART_NAMES[found],
             (unsigned)(*len / 1024));
    set_donor(key, file, h);
    return ESP_OK;
}

/*
 * 8055: a picture whose name the track's sidecar remembered (art.ext),
 * read straight from the track's folder, without the walk. The name is
 * the sidecar's, so it is held to one name in that folder: no '/', not
 * "." or "..". ESP_ERR_NOT_FOUND when it is not there any more, and the
 * caller looks again as before.
 */
esp_err_t folderart_load_named(const char *file, const char *name, uint8_t **img, size_t *len)
{
    *img = NULL;
    *len = 0;
    if (!name || !name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return ESP_ERR_INVALID_ARG;
    if (!ensure() || !folderart_dir_of(file, s_dir, FOLDERART_PATH_MAX)) return ESP_ERR_NOT_FOUND;
    if (snprintf(s_path, FOLDERART_PATH_MAX, "%s/%s", s_dir, name) >= FOLDERART_PATH_MAX)
        return ESP_ERR_NOT_FOUND;
    struct stat st;
    if (stat(s_path, &st) != 0 || !S_ISREG(st.st_mode)) return ESP_ERR_NOT_FOUND;
    const esp_err_t err = read_image((long)st.st_size, img, len);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s, as the sidecar said (%u KB)", name, (unsigned)(*len / 1024));
    return err;
}
