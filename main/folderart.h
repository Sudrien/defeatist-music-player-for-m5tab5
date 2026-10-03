/*
 * folderart.h -- the cover that ships beside the album, not inside it.
 *
 * A great many albums carry their art as a file in the folder --
 * cover.jpg, folder.jpg (what Windows Media Player and EAC write),
 * front.jpg -- and none in the tracks. Until 6042 the screen only looked
 * inside the file (covertag_extract_art()), so those albums showed the
 * format card, while the MPD server already answered `albumart` from the
 * folder. Two parts of one program disagreeing about where a cover is.
 *
 * EMBEDDED FIRST. A track that carries its own picture keeps it; the
 * folder is the fallback when the file has none. A compilation with
 * per-track art in one folder and a folder.jpg of the box is the case
 * this order is for.
 *
 * TWO NAME LISTS, ON PURPOSE, AND BOTH HERE.
 *
 * - FOLDERART_MPD_NAMES is MPD 0.21's own list in MPD's own order
 *   (src/command/FileCommands.cxx), which mpd.c answers `albumart` from.
 *   A client that works against stock MPD should see the same file
 *   here, so it is not "improved".
 * - FOLDERART_NAMES is the screen's: the names rippers and stores
 *   actually write, JPEG before PNG within a name because the JPEG
 *   decoder here is the hardware one. TIFF and BMP are left out because
 *   albumart_show() decodes neither, and AlbumArtSmall.jpg (a 75 px
 *   thumbnail Windows writes beside folder.jpg) is left out because it
 *   would be the worst picture in the folder.
 *
 * FAT and exFAT match names without regard to case, so "Folder.JPG" is
 * found by asking for "folder.jpg". That is the filesystem's doing, not
 * a loop over spellings here.
 *
 * The pure half -- the lists, a file's folder, the folder's hash -- is
 * here and host-tested (texttest/folderarttest.c). The I/O is in
 * folderart.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static const char *const FOLDERART_NAMES[] = {
    "cover.jpg",  "cover.jpeg",  "cover.png",
    "folder.jpg", "folder.jpeg", "folder.png",
    "front.jpg",  "front.jpeg",  "front.png",
    "album.jpg",  "album.jpeg",  "album.png",
};
#define FOLDERART_NNAMES ((int)(sizeof(FOLDERART_NAMES) / sizeof(FOLDERART_NAMES[0])))

static const char *const FOLDERART_MPD_NAMES[] = {
    "cover.png", "cover.jpg", "cover.tiff", "cover.bmp",
};
#define FOLDERART_MPD_NNAMES \
    ((int)(sizeof(FOLDERART_MPD_NAMES) / sizeof(FOLDERART_MPD_NAMES[0])))

/*
 * The folder a file is in, without the trailing '/', into `out`. False
 * when the path has no '/' or the folder will not fit -- then `out` is
 * empty, never a cut path, because a cut path names some other folder.
 * "/sd/a.mp3" gives "/sd".
 */
static inline bool folderart_dir_of(const char *file, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!file) return false;
    const char *slash = strrchr(file, '/');
    if (!slash || slash == file) return false;
    const size_t n = (size_t)(slash - file);
    if (n + 1 > out_size) return false;
    memcpy(out, file, n);
    out[n] = '\0';
    return true;
}

/*
 * A folder's identity for the lookup cache: FNV-1a over the path with
 * ASCII folded to lower case, because the filesystem folds it too and
 * "/sd/Album" and "/sd/album" are one folder there. 0 is reserved for
 * "empty slot" and never returned.
 */
static inline uint32_t folderart_dir_hash(const char *dir)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)dir; p && *p; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/*
 * The folder's cover for `file`, read into a malloc()ed buffer the
 * caller owns (mediacache_put_art() takes it as it takes
 * covertag_extract_art()'s). `*name` is the file's name in the folder,
 * for the log. ESP_ERR_NOT_FOUND when the folder has none of the names
 * or none of them is a JPEG or PNG.
 *
 * `key` is the path the cover cache knows the track by (a cue track's
 * key differs from its file); it is remembered as the donor, so the next
 * track in the same folder copies the image out of the cache rather than
 * reading the card again.
 *
 * media_task only, the way do_art() is.
 */
esp_err_t folderart_load(const char *file, const char *key,
                         uint8_t **img, size_t *len, const char **name);

/*
 * Whether folderart_load() for this file would not touch the card: the
 * folder is known to have no cover, or its cover can be copied from the
 * cache. player.c's settle gate asks, because "this track has no
 * embedded picture" used to mean "nothing to read" and now does not.
 */
bool folderart_in_hand(const char *file);

/* Forget every folder's answer: the volume has changed. */
void folderart_forget(void);

#ifdef __cplusplus
}
#endif
