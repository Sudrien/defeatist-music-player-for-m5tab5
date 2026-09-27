/*
 * medialisttest.c -- medialist.h against a synthetic index.
 *
 * The expected listings here are written by hand from what the FOLDER
 * should contain, not by running the merge and recording what came out,
 * and the order is checked against an independent comparator below
 * rather than against midx_name_cmp() -- texttest/README.md's rule: a
 * reimplementation that shares the author's misunderstanding passes and
 * proves nothing, so the check has to come from somewhere else. The
 * ordering oracle here is "what `LC_ALL=C sort` would do to the paths",
 * which is where the '/'-sorts-lowest rule comes from in the first
 * place.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/medialist.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* ---- a synthetic volume -------------------------------------------- */

#define MAX_RECS 512

typedef struct {
    const char *path[MAX_RECS];
    bool        dead[MAX_RECS];
    uint32_t    n;
    uint32_t    reads;          /* how much of the index was touched */
} vol_t;

static bool v_read(void *ctx, uint32_t i, midx_rec_t *out)
{
    vol_t *v = ctx;
    if (i >= v->n) return false;
    v->reads++;

    uint8_t raw[MIDX_REC_SIZE];
    const midx_stamp_t st = { .mtime = 1, .size = 2 };
    if (!midx_rec_pack(raw, v->path[i], i, v->dead[i] ? MIDX_F_DEAD : 0, st)) {
        return false;
    }
    return midx_rec_unpack(raw, out);
}

/* cat_off is the record's own index here, which is all the test needs
 * it to be: a stable handle back to the whole path. */
static bool v_fullpath(void *ctx, uint32_t cat_off, char *buf, size_t n)
{
    vol_t *v = ctx;
    if (cat_off >= v->n) return false;
    const size_t len = strlen(v->path[cat_off]);
    if (len + 1 > n) return false;
    memcpy(buf, v->path[cat_off], len + 1);
    return true;
}

static char scratch_a[MIDX_PATH_MAX + 1];
static char scratch_b[MIDX_PATH_MAX + 1];

static midx_src_t mk(vol_t *v, char *scratch)
{
    midx_src_t s;
    memset(&s, 0, sizeof(s));
    s.read      = v_read;
    s.fullpath  = v_fullpath;
    s.ctx       = v;
    s.n         = v->n;
    s.scratch   = scratch;
    s.scratch_n = MIDX_PATH_MAX + 1;
    return s;
}

/* Paths must go in the index in the order the index is stored in. The
 * test builds them by hand, so this catches a fixture that is wrong
 * before it can be blamed on the code under test. */
static void assert_sorted(vol_t *v, const char *what)
{
    for (uint32_t i = 1; i < v->n; i++) {
        CHECK(midx_path_cmp(v->path[i - 1], v->path[i]) < 0,
              "%s fixture out of order at %u: \"%s\" then \"%s\"",
              what, i, v->path[i - 1], v->path[i]);
    }
}

/* ---- the listing ---------------------------------------------------- */

/* "name/" for a folder, "name" for a file, joined with '|'. */
static void listing(midx_src_t *a, midx_src_t *b, const char *dir,
                    char *out, size_t n, int *vols)
{
    medialist_t ml;
    out[0] = '\0';

    if (!medialist_open(&ml, a, b, dir)) {
        snprintf(out, n, "<open failed>");
        return;
    }

    medialist_ent_t e;
    int k = 0;
    while (medialist_next(&ml, &e)) {
        if (out[0]) strncat(out, "|", n - strlen(out) - 1);
        strncat(out, e.name, n - strlen(out) - 1);
        if (e.is_dir) strncat(out, "/", n - strlen(out) - 1);
        if (vols) vols[k] = e.vol;
        k++;
    }
    if (ml.err) snprintf(out, n, "<error>");
}

int main(void)
{
    char got[1024];

    /* ---- one volume, one folder ------------------------------------ */
    {
        vol_t v = { .n = 5 };
        v.path[0] = "Artist/Album/01 One.flac";
        v.path[1] = "Artist/Album/02 Two.flac";
        v.path[2] = "Artist/Other/01 Three.flac";
        v.path[3] = "Artist/cover.jpg";
        v.path[4] = "Solo.mp3";
        assert_sorted(&v, "single");

        midx_src_t s = mk(&v, scratch_a);

        listing(&s, NULL, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Artist/|Solo.mp3") == 0, "root: got \"%s\"", got);

        /* '/' sorts below every other byte, so the two subfolders come
         * before the file that shares their parent. */
        listing(&s, NULL, "Artist/", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Album/|Other/|cover.jpg") == 0,
              "Artist/: got \"%s\"", got);

        listing(&s, NULL, "Artist/Album/", got, sizeof(got), NULL);
        CHECK(strcmp(got, "01 One.flac|02 Two.flac") == 0,
              "Artist/Album/: got \"%s\"", got);

        /* A folder that is not in the index at all. */
        listing(&s, NULL, "Nothing/", got, sizeof(got), NULL);
        CHECK(strcmp(got, "") == 0, "missing folder: got \"%s\"", got);
    }

    /* ---- a listing does not read the subtree ----------------------- */
    {
        /*
         * Big enough for the two cost models to be distinguishable. At
         * 32 records a binary search is not cheaper than a walk, so a
         * bound set there proves nothing whichever way it falls; the
         * subtree has to be large enough that reading it would show.
         */
        vol_t v = { .n = 0 };
        static char paths[250][64];
        v.path[v.n++] = "A/a.flac";
        for (int i = 0; i < 250; i++) {
            snprintf(paths[i], sizeof(paths[i]), "B/Album/%03d t.flac", i);
            v.path[v.n++] = paths[i];
        }
        v.path[v.n++] = "C/c.flac";
        assert_sorted(&v, "wide");

        midx_src_t s = mk(&v, scratch_a);
        v.reads = 0;
        listing(&s, NULL, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "A/|B/|C/") == 0, "wide root: got \"%s\"", got);

        /*
         * Three children of the root, so the cost is a few searches of
         * log2(252) ~= 8 reads each, not the 252 records themselves.
         * The bound is loose -- it is testing which cost model is in
         * use, not counting reads -- but it sits well below a walk.
         */
        CHECK(v.reads < 60, "root listing read %u records of %u",
              v.reads, v.n);
    }

    /* ---- tombstones ------------------------------------------------ */
    {
        vol_t v = { .n = 4 };
        v.path[0] = "Gone/01.flac";  v.dead[0] = true;
        v.path[1] = "Gone/02.flac";  v.dead[1] = true;
        v.path[2] = "Here/01.flac";
        v.path[3] = "dead.flac";     v.dead[3] = true;
        assert_sorted(&v, "dead");

        midx_src_t s = mk(&v, scratch_a);

        /* A folder whose every track is buried is not on the card any
         * more, and a buried file is not a row. */
        listing(&s, NULL, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Here/") == 0, "all-dead: got \"%s\"", got);

        /* One live track is enough to keep the folder. */
        v.dead[1] = false;
        listing(&s, NULL, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Gone/|Here/") == 0, "one live: got \"%s\"", got);

        listing(&s, NULL, "Gone/", got, sizeof(got), NULL);
        CHECK(strcmp(got, "02.flac") == 0, "inside: got \"%s\"", got);
    }

    /* ---- two volumes ----------------------------------------------- */
    {
        vol_t sd = { .n = 3 };
        sd.path[0] = "Both/01 sd.flac";
        sd.path[1] = "Both/same.flac";
        sd.path[2] = "OnlySD/x.flac";
        assert_sorted(&sd, "sd");

        vol_t usb = { .n = 3 };
        usb.path[0] = "Both/02 usb.flac";
        usb.path[1] = "Both/same.flac";
        usb.path[2] = "OnlyUSB/y.flac";
        assert_sorted(&usb, "usb");

        midx_src_t a = mk(&sd, scratch_a), b = mk(&usb, scratch_b);

        /* Folders present on both list once. */
        listing(&a, &b, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Both/|OnlySD/|OnlyUSB/") == 0,
              "merged root: got \"%s\"", got);

        /* And the folder's contents are the union, in one order. */
        int vols[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        listing(&a, &b, "Both/", got, sizeof(got), vols);
        CHECK(strcmp(got, "01 sd.flac|02 usb.flac|same.flac") == 0,
              "merged folder: got \"%s\"", got);

        /* The same relative path on both is ONE entry, and it is the
         * preferred volume's -- MEDIA-INDEX.md point 3. */
        CHECK(vols[0] == 0, "01 sd.flac came from %d", vols[0]);
        CHECK(vols[1] == 1, "02 usb.flac came from %d", vols[1]);
        CHECK(vols[2] == 0, "the duplicate came from %d, wanted SD", vols[2]);

        /* Swapping the slots swaps which copy wins and nothing else. */
        midx_src_t a2 = mk(&usb, scratch_a), b2 = mk(&sd, scratch_b);
        listing(&a2, &b2, "Both/", got, sizeof(got), vols);
        CHECK(strcmp(got, "01 sd.flac|02 usb.flac|same.flac") == 0,
              "swapped: got \"%s\"", got);
        CHECK(vols[2] == 0, "swapped duplicate came from %d", vols[2]);

        /* One volume mounted, in slot 0, is the library. */
        listing(&a, NULL, "", got, sizeof(got), NULL);
        CHECK(strcmp(got, "Both/|OnlySD/") == 0, "sd alone: got \"%s\"", got);
    }

    /* ---- a folder on one volume and a file of the same name on the
     * other. The filesystem allows it across two cards; one row. ---- */
    {
        vol_t sd = { .n = 1 };
        sd.path[0] = "X/inside.flac";

        vol_t usb = { .n = 1 };
        usb.path[0] = "X";

        midx_src_t a = mk(&sd, scratch_a), b = mk(&usb, scratch_b);

        int vols[4] = { -1, -1, -1, -1 };
        listing(&a, &b, "", got, sizeof(got), vols);
        CHECK(strcmp(got, "X/") == 0, "dir vs file: got \"%s\"", got);
        CHECK(vols[0] == 0, "dir vs file resolved to %d", vols[0]);
    }

    /* ---- paths longer than the key ---------------------------------- */
    {
        /* Two siblings that agree on the first MIDX_KEY_LEN bytes, so
         * the key cannot separate them and the catalog is read. */
        static char p0[MIDX_PATH_MAX + 1], p1[MIDX_PATH_MAX + 1];
        memset(p0, 'a', MIDX_KEY_LEN);
        p0[MIDX_KEY_LEN] = '\0';
        strcpy(p1, p0);
        strcat(p0, "0.flac");
        strcat(p1, "1.flac");

        vol_t v = { .n = 2 };
        v.path[0] = p0;
        v.path[1] = p1;
        assert_sorted(&v, "long");

        midx_src_t s = mk(&v, scratch_a);
        listing(&s, NULL, "", got, sizeof(got), NULL);

        char want[MIDX_PATH_MAX * 2 + 4];
        snprintf(want, sizeof(want), "%s|%s", p0, p1);
        CHECK(strcmp(got, want) == 0, "long paths: got \"%s\"", got);
    }

    /* ---- a bad folder argument -------------------------------------- */
    {
        vol_t v = { .n = 1 };
        v.path[0] = "a.flac";
        midx_src_t s = mk(&v, scratch_a);

        medialist_t ml;
        /* Not "" and not ending in '/': the caller means a folder and
         * has handed over something that is not one. */
        CHECK(!medialist_open(&ml, &s, NULL, "Artist"),
              "a folder without its slash was accepted");
        CHECK(medialist_open(&ml, &s, NULL, ""), "the root was refused");
    }

    /* ---- a read that fails is not an empty folder ------------------- */
    {
        vol_t v = { .n = 3 };
        v.path[0] = "A/1.flac";
        v.path[1] = "A/2.flac";
        v.path[2] = "B/1.flac";

        midx_src_t s = mk(&v, scratch_a);
        s.n = 5;                    /* claims more than the array holds */

        medialist_t ml;
        medialist_ent_t e;
        int seen = 0;
        if (medialist_open(&ml, &s, NULL, "")) {
            while (medialist_next(&ml, &e)) seen++;
        }
        CHECK(ml.err, "a failing read reported a clean end after %d rows",
              seen);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
