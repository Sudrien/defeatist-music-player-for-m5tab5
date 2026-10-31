/*
 * medialist.h -- one folder of the library, both volumes merged.
 *
 * The query layer MEDIA-INDEX.md specified and 5010-5019 did not build.
 * mediaindex.h can find a path and jump past a subtree; this turns that
 * into "what is in this folder", which is what MPD's lsinfo asks, what
 * the remote page's chooser will ask over the socket, and what the web
 * UI wants for the same folder the glass is showing.
 *
 * HEADER-ONLY AND PURE, for the reason remoteproto.c is split out of
 * remote.c: it is driven entirely through midx_src_t's read/fullpath
 * callbacks, so the host test builds it against a synthetic index with
 * no card, no FatFs and no IDF. Nothing here opens a file.
 *
 * WHAT IT IS NOT: display order. Entries come out in midx_name_cmp()
 * order -- byte order with '/' lowest -- and NOT the chooser's
 * folders-first, case-insensitive order (browser.c). MEDIA-INDEX.md and
 * mediaindex.h both say this and it is still the mistake the obvious
 * implementation makes: this order is the one the index is stored in,
 * so it costs nothing, and a caller that wants the screen's order sorts
 * what comes out. MPD wants this order anyway.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mediaindex.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Two volumes, and index 0 wins.
 *
 * MEDIA-INDEX.md point 3: paths are relative to the volume root, the
 * same relative path on both volumes is ONE entry and the preferred
 * volume's copy is the one shown, and a folder present on both lists
 * the union of what is in each. The caller decides which volume is
 * preferred by which slot it puts it in -- SD in 0, USB in 1, per that
 * note -- and with one volume mounted it goes in 0 and slot 1 is NULL.
 *
 * Matching is byte-exact, so "ABBA/" and "Abba/" are two entries.
 *
 * AND THAT IS NOT THE FILESYSTEM'S ANSWER, which an earlier version of
 * this comment claimed. exFAT and FAT32-with-LFN are both
 * case-insensitive and case-preserving, so WITHIN one volume the two
 * cannot both exist and byte-exact matching costs nothing. It is ACROSS
 * two volumes that they can -- "ABBA/" on the SD and "Abba/" on the
 * USB stick are each legal, and this lists them as two folders where
 * the filesystem's own rule would call them one.
 *
 * Left as it is, for now, and written down rather than quietly fixed:
 * folding case to merge them means a case-folding comparison in the
 * merge and, to be consistent, an index built in a case-folded order --
 * which is a format change and midx_path_cmp()'s order is the one the
 * walk, the index and reconcile all agree on. The cost of the current
 * answer is two rows for one album on a two-card setup where the cases
 * disagree; the cost of changing it is the index format. That trade
 * should be made deliberately, with MEDIA-INDEX.md point 3 updated,
 * rather than as a patch to this function.
 */
#define MEDIALIST_VOLS  2

typedef struct {
    char        name[MIDX_PATH_MAX + 1];  /* the child, no path, no slash */
    bool        is_dir;
    int         vol;                      /* which slot supplied it */
    midx_rec_t  rec;                      /* files only; not set for a folder */
    bool        valid;
} medialist_ent_t;

typedef struct {
    midx_src_t     *src[MEDIALIST_VOLS];  /* NULL for a volume not mounted */
    char            dir[MIDX_PATH_MAX + 2];
    uint32_t        i[MEDIALIST_VOLS];    /* cursor */
    uint32_t        end[MEDIALIST_VOLS];  /* one past this folder's run */
    medialist_ent_t pend[MEDIALIST_VOLS]; /* one child read ahead per volume */
    bool            err;
} medialist_t;

/*
 * The next child of `dir` on one volume, into pend[v].
 *
 * Two things happen here that are the whole cost model of the listing.
 *
 * A FOLDER IS ONE SEEK, NOT A READ PER TRACK. Having seen that a record
 * lies under the child "Album/", the cursor jumps with MIDX_PAST_PREFIX
 * to the first record past everything beginning with "<dir>Album/" --
 * so listing a folder of 30 albums costs about 30 searches rather than
 * a read of all 400 tracks under them. mediaindex.h built PAST_PREFIX
 * for exactly this.
 *
 * A FOLDER WHOSE TRACKS ARE ALL BURIED DOES NOT APPEAR. The index keeps
 * tombstones (reconcile needs to see the dead to revive them), so a
 * deleted album is still a run of records, and emitting a folder on
 * sight would list albums that are not on the card. The run is scanned
 * for the first live record instead. That is one extra read in the
 * ordinary case -- the first record under a folder is nearly always
 * live -- and a full scan of the run only for a folder that is entirely
 * dead, which is the case that must not be got wrong and the one that
 * happens once after a deletion and never again.
 */
static inline void medialist_fill_(medialist_t *ml, int v)
{
    ml->pend[v].valid = false;

    midx_src_t *s = ml->src[v];
    if (!s || ml->err) return;

    while (ml->i[v] < ml->end[v]) {
        midx_rec_t r;
        if (!s->read(s->ctx, ml->i[v], &r)) {
            s->err = ml->err = true;
            return;
        }
        const char *full = midx_rec_fullpath(s, &r);
        if (!full) {
            ml->err = true;
            return;
        }

        char name[MIDX_PATH_MAX + 1];
        const bool is_dir = midx_child(ml->dir, full, name);

        if (!is_dir) {
            ml->i[v]++;
            if (r.flags & MIDX_F_DEAD) continue;   /* buried: not a file here */
            memcpy(ml->pend[v].name, name, strlen(name) + 1);
            ml->pend[v].is_dir = false;
            ml->pend[v].vol    = v;
            ml->pend[v].rec    = r;
            ml->pend[v].valid  = true;
            return;
        }

        /*
         * A subfolder. Find the run's end first, because the cursor
         * lands there whether or not anything in it is alive.
         */
        char pfx[MIDX_PATH_MAX + 2];
        const size_t dl = strlen(ml->dir), nl = strlen(name);
        if (dl + nl + 2 > sizeof(pfx)) {     /* cannot happen from a valid
                                             * index; refuse rather than
                                             * truncate into a prefix that
                                             * matches the wrong run */
            ml->err = true;
            return;
        }
        memcpy(pfx, ml->dir, dl);
        memcpy(pfx + dl, name, nl);
        pfx[dl + nl]     = '/';
        pfx[dl + nl + 1] = '\0';

        const uint32_t past = midx_seek(s, pfx, MIDX_PAST_PREFIX);
        if (s->err) {
            ml->err = true;
            return;
        }

        bool live = false;
        for (uint32_t k = ml->i[v]; k < past && k < ml->end[v]; k++) {
            midx_rec_t t;
            if (!s->read(s->ctx, k, &t)) {
                s->err = ml->err = true;
                return;
            }
            if (!(t.flags & MIDX_F_DEAD)) {
                live = true;
                break;
            }
        }

        ml->i[v] = (past > ml->i[v]) ? past : ml->i[v] + 1;   /* never stall */

        if (!live) continue;

        memcpy(ml->pend[v].name, name, nl + 1);
        ml->pend[v].is_dir = true;
        ml->pend[v].vol    = v;
        ml->pend[v].valid  = true;
        return;
    }
}

/*
 * Start a listing of `dir`, which is "" for the volume root or ends in
 * '/'. Both volumes are positioned at the folder's run.
 *
 * Returns false if the arguments cannot be used; a read failure shows
 * up later as ml->err, which the caller checks when the listing ends.
 */
static inline bool medialist_open(medialist_t *ml,
                                  midx_src_t *sd, midx_src_t *usb,
                                  const char *dir)
{
    if (!ml || !dir) return false;
    const size_t n = strlen(dir);
    if (n + 1 > sizeof(ml->dir)) return false;
    if (n && dir[n - 1] != '/') return false;     /* "" or ends in '/' */

    memset(ml, 0, sizeof(*ml));
    memcpy(ml->dir, dir, n + 1);
    ml->src[0] = sd;
    ml->src[1] = usb;

    for (int v = 0; v < MEDIALIST_VOLS; v++) {
        midx_src_t *s = ml->src[v];
        if (!s) continue;
        ml->i[v]   = midx_seek(s, ml->dir, MIDX_AT);
        ml->end[v] = midx_seek(s, ml->dir, MIDX_PAST_PREFIX);
        if (s->err) ml->err = true;
        medialist_fill_(ml, v);
    }
    return !ml->err;
}

/*
 * The next entry, or false when the folder is done. Check ml->err
 * afterwards: false with err set is a truncated listing and not an
 * empty folder, and a caller that shows one as the other tells the
 * listener their album is gone when the card merely stopped answering.
 *
 * THE MERGE. Both sides are in midx_name_cmp() order, so this is the
 * ordinary two-way merge, and equal names collapse to one entry from
 * the preferred volume -- including when one side's is a file and the
 * other's a folder, which the filesystem allows across two cards and
 * which resolves to the preferred volume's answer rather than to two
 * rows with the same name.
 */
static inline bool medialist_next(medialist_t *ml, medialist_ent_t *out)
{
    if (!ml || !out || ml->err) return false;

    const bool a = ml->pend[0].valid, b = ml->pend[1].valid;
    if (!a && !b) return false;

    int take = a ? 0 : 1;
    bool both = false;

    if (a && b) {
        const int c = midx_name_cmp(ml->pend[0].name, ml->pend[1].name);
        take = (c <= 0) ? 0 : 1;
        both = (c == 0);
    }

    *out = ml->pend[take];
    medialist_fill_(ml, take);
    if (both) medialist_fill_(ml, 1);      /* the copy that lost, dropped */

    return !ml->err;
}

#ifdef __cplusplus
}
#endif
