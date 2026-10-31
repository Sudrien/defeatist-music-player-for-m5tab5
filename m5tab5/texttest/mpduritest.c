/*
 * mpduritest.c -- mpduri.c: a URI is an index path, and which volume.
 *
 * The cases are written from the RULE the format states -- mediaindex.h's
 * "paths are relative to the volume root ... SD preferred" and
 * MEDIA-INDEX.md point 3's merge -- rather than from what mpduri.c does
 * with it, per texttest/README.md. The shadowing case is the one that
 * matters: a relative path on both volumes must resolve to the SD copy
 * and the USB copy must be unreachable, and a test written by reading the
 * loop would bless whichever order the loop happened to use.
 *
 * The synthetic volume is medialisttest.c's, because a second
 * implementation of the same fixture is a second thing to get wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mpduri.h"

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

/* ---- a synthetic volume, as medialisttest.c builds one -------------- */

#define MAX_RECS 64

typedef struct {
    const char *path[MAX_RECS];
    bool        dead[MAX_RECS];
    uint32_t    n;
} vol_t;

static bool v_read(void *ctx, uint32_t i, midx_rec_t *out)
{
    vol_t *v = ctx;
    if (i >= v->n) return false;
    uint8_t raw[MIDX_REC_SIZE];
    const midx_stamp_t st = { .mtime = 1, .size = 2 };
    if (!midx_rec_pack(raw, v->path[i], i, v->dead[i] ? MIDX_F_DEAD : 0, st))
        return false;
    return midx_rec_unpack(raw, out);
}

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

/* The index is stored in one order and searched assuming it; a fixture
 * out of that order proves nothing, so it is checked first. */
static void assert_sorted(vol_t *v, const char *what)
{
    for (uint32_t i = 1; i < v->n; i++)
        CHECK(midx_path_cmp(v->path[i - 1], v->path[i]) < 0,
              "%s fixture out of order at %u: [%s] then [%s]",
              what, i, v->path[i - 1], v->path[i]);
}

static void ok_uri(const char *u)
{
    CHECK(mpduri_ok(u, false), "[%s] was refused as a file uri", u);
    CHECK(mpduri_ok(u, true), "[%s] was refused as a dir uri", u);
}

static void bad_uri(const char *u)
{
    CHECK(!mpduri_ok(u, false), "[%s] was accepted as a file uri", u);
    CHECK(!mpduri_ok(u, true), "[%s] was accepted as a dir uri", u);
}

int main(void)
{
    /* ---- the shape of a uri ----------------------------------------- */
    /*
     * An index path: relative, no leading slash. The absolute form is
     * what remoteproto_path_ok() wants and is NOT a uri -- getting that
     * round the wrong way is the bug this file exists to prevent, so it
     * is the first case.
     */
    ok_uri("Artist/Album/01 Song.flac");
    ok_uri("track.mp3");
    ok_uri("a/b/c/d/e.ogg");
    /* A cue track's uri keeps its '#NN': mediaindex.h:16 says that is the
     * name, so the '#' is path and not syntax. */
    ok_uri("Artist/Album/Album.cue#03");
    /* Spaces and UTF-8 are ordinary path bytes; the tokeniser's quoting
     * is what carries them, not an escape in the uri. */
    ok_uri("The Fall/Hex Enduction Hour/01 The Classical.flac");
    ok_uri("Caf\xC3\xA9/T\xC3\xB6rn.mp3");

    bad_uri("/sd/Artist/x.flac");        /* absolute: the OTHER form */
    bad_uri("/Artist/x.flac");
    bad_uri("Artist/");                  /* trailing slash */
    bad_uri("Artist//x.flac");           /* empty segment */
    bad_uri("Artist/./x.flac");
    bad_uri("Artist/../x.flac");
    bad_uri("../x.flac");
    bad_uri("..");
    bad_uri(".");
    bad_uri("Artist/x\\y.flac");         /* not a legal FAT byte, and the
                                          * protocol's escape character */
    bad_uri("Artist/x\ny.flac");
    bad_uri("Artist/x\x7Fy.flac");
    CHECK(!mpduri_ok(NULL, true), "NULL was accepted");

    /* The root is the empty string, and it is a directory and never a
     * file -- which is the whole of the allow_root argument. */
    CHECK(mpduri_is_root(""), "the empty uri is not the root");
    CHECK(!mpduri_is_root("a"), "[a] was called the root");
    CHECK(mpduri_ok("", true), "the root was refused where it is legal");
    CHECK(!mpduri_ok("", false), "the root was accepted as a file");

    /* The ceiling is the index's, not remoteproto's 512. */
    {
        char u[MPDURI_MAX + 8];
        memset(u, 'a', MPDURI_MAX);
        u[MPDURI_MAX] = '\0';
        CHECK(mpduri_ok(u, false), "a uri of exactly MPDURI_MAX was refused");
        u[MPDURI_MAX] = 'a';
        u[MPDURI_MAX + 1] = '\0';
        CHECK(!mpduri_ok(u, false), "a uri one past MPDURI_MAX was accepted");
    }

    /* ---- the directory form medialist wants ------------------------- */
    {
        char d[MPDURI_MAX + 4];
        CHECK(mpduri_dir("", d, sizeof(d)) && strcmp(d, "") == 0,
              "the root dir is [%s], want []", d);
        CHECK(mpduri_dir("Artist", d, sizeof(d)) && strcmp(d, "Artist/") == 0,
              "dir is [%s], want [Artist/]", d);
        CHECK(mpduri_dir("Artist/Album", d, sizeof(d)) &&
              strcmp(d, "Artist/Album/") == 0, "dir is [%s]", d);
        CHECK(!mpduri_dir("/sd/Artist", d, sizeof(d)), "an absolute dir passed");
        /* No room for the slash is a refusal, not a truncation into a
         * prefix that would match the wrong run. */
        CHECK(!mpduri_dir("Artist", d, 7), "dir truncated into 7 bytes");
        CHECK(mpduri_dir("Artist", d, 8) && strcmp(d, "Artist/") == 0,
              "dir did not fit its exact size");
    }

    /* ---- resolving a file, and which volume ------------------------- */
    {
        /*
         * The fixture, in index order. "Both/" is the case that matters:
         * the same relative path on each volume.
         */
        static vol_t sd = {
            .path = { "Both/same.flac", "Gone/dead.flac", "OnlySD/a.flac" },
            .dead = { false,            true,             false },
            .n    = 3,
        };
        static vol_t usb = {
            .path = { "Both/same.flac", "Gone/dead.flac", "OnlyUSB/b.flac" },
            .dead = { false,            false,            false },
            .n    = 3,
        };
        assert_sorted(&sd, "sd");
        assert_sorted(&usb, "usb");

        midx_src_t a = mk(&sd, scratch_a), b = mk(&usb, scratch_b);
        midx_src_t *const both[MPDURI_VOLS] = { &a, &b };

        char p[MPDURI_VFS_MAX];
        int vol;

        /*
         * 5191: THE URI NAMES ITS VOLUME. The same relative path on both
         * is two URIs, each resolving to its own copy -- the USB one was
         * unreachable before, which is why this changed.
         */
        CHECK(mpduri_to_vfs("sd/Both/same.flac", both, p, sizeof(p), &vol) &&
              strcmp(p, "/sd/Both/same.flac") == 0 && vol == MPDURI_VOL_SD,
              "sd/ on both volumes gave [%s] vol %d", p, vol);
        CHECK(mpduri_to_vfs("usb/Both/same.flac", both, p, sizeof(p), &vol) &&
              strcmp(p, "/usb/Both/same.flac") == 0 && vol == MPDURI_VOL_USB,
              "usb/ on both volumes gave [%s] vol %d", p, vol);

        CHECK(mpduri_to_vfs("sd/OnlySD/a.flac", both, p, sizeof(p), &vol) &&
              strcmp(p, "/sd/OnlySD/a.flac") == 0 && vol == MPDURI_VOL_SD,
              "sd-only gave [%s]", p);
        CHECK(mpduri_to_vfs("usb/OnlyUSB/b.flac", both, p, sizeof(p), &vol) &&
              strcmp(p, "/usb/OnlyUSB/b.flac") == 0 && vol == MPDURI_VOL_USB,
              "usb-only gave [%s]", p);
        /* A file on the other volume is not found under this one's name:
         * there is no falling back any more. */
        CHECK(!mpduri_to_vfs("sd/OnlyUSB/b.flac", both, p, sizeof(p), NULL),
              "sd/ found a usb-only file at [%s]", p);
        CHECK(!mpduri_to_vfs("usb/OnlySD/a.flac", both, p, sizeof(p), NULL),
              "usb/ found an sd-only file at [%s]", p);

        /* A tombstone is not a file, and the live copy on the other
         * volume is its own URI, not a fallback. */
        CHECK(!mpduri_to_vfs("sd/Gone/dead.flac", both, p, sizeof(p), &vol),
              "a tombstone on sd resolved to [%s]", p);
        CHECK(p[0] == '\0', "a failed resolve left [%s] behind", p);
        CHECK(vol == -1, "a failed resolve left vol %d behind", vol);
        CHECK(mpduri_to_vfs("usb/Gone/dead.flac", both, p, sizeof(p), &vol) &&
              strcmp(p, "/usb/Gone/dead.flac") == 0, "usb/Gone gave [%s]", p);

        /* No volume, an unknown one, a volume itself, the root. */
        CHECK(!mpduri_to_vfs("Both/same.flac", both, p, sizeof(p), NULL),
              "a uri with no volume resolved to [%s]", p);
        CHECK(!mpduri_to_vfs("sdx/Both/same.flac", both, p, sizeof(p), NULL),
              "[sdx/...] was read as sd");
        CHECK(!mpduri_to_vfs("sd", both, p, sizeof(p), NULL), "a volume resolved as a file");
        CHECK(!mpduri_to_vfs("Nope/x.flac", both, p, sizeof(p), NULL), "Nope resolved");
        CHECK(!mpduri_to_vfs("", both, p, sizeof(p), NULL), "the root resolved");
        CHECK(!mpduri_to_vfs("/sd/Both/same.flac", both, p, sizeof(p), NULL),
              "an absolute path resolved");
        CHECK(!mpduri_to_vfs("sd/Both/../Both/same.flac", both, p, sizeof(p), NULL),
              "a traversal resolved");

        /* A volume not mounted is a NULL slot, skipped, not dereferenced. */
        {
            midx_src_t *const sd_only[MPDURI_VOLS] = { &a, NULL };
            CHECK(mpduri_to_vfs("sd/OnlySD/a.flac", sd_only, p, sizeof(p), &vol) &&
                  strcmp(p, "/sd/OnlySD/a.flac") == 0, "sd alone gave [%s]", p);
            CHECK(!mpduri_to_vfs("usb/OnlyUSB/b.flac", sd_only, p, sizeof(p), NULL),
                  "a usb file resolved with no usb volume");
            midx_src_t *const none[MPDURI_VOLS] = { NULL, NULL };
            CHECK(!mpduri_to_vfs("sd/OnlySD/a.flac", none, p, sizeof(p), NULL),
                  "a file resolved with nothing mounted");
        }

        /* The split itself. */
        {
            const char *rel = NULL;
            CHECK(mpduri_split("sd/a/b.flac", &rel) == MPDURI_VOL_SD && strcmp(rel, "a/b.flac") == 0,
                  "split sd/a/b.flac gave [%s]", rel);
            CHECK(mpduri_split("usb", &rel) == MPDURI_VOL_USB && rel[0] == '\0',
                  "split usb gave [%s]", rel);
            CHECK(mpduri_split("", &rel) == -1, "split of the root named a volume");
            CHECK(mpduri_split("usbx/a", &rel) == -1, "split [usbx/a] named a volume");
            CHECK(mpduri_split("Music/a", &rel) == -1, "split [Music/a] named a volume");
        }

        /* A buffer that cannot hold the answer is a refusal. */
        CHECK(!mpduri_to_vfs("sd/OnlySD/a.flac", both, p, 10, NULL),
              "a resolve fitted 10 bytes");
    }

    /* ---- back the other way ---------------------------------------- */
    {
        /*
         * currentsong holds a VFS path and must publish a uri, and a
         * client sends that uri straight back in a playid -- so the two
         * directions have to agree exactly. Checked as a round trip
         * rather than as two independent expectations.
         */
        char u[MPDURI_MAX + 1];

        CHECK(mpduri_from_vfs("/sd/Artist/x.flac", u, sizeof(u)) &&
              strcmp(u, "sd/Artist/x.flac") == 0, "from_vfs gave [%s]", u);
        CHECK(mpduri_from_vfs("/usb/Artist/x.flac", u, sizeof(u)) &&
              strcmp(u, "usb/Artist/x.flac") == 0, "from_vfs gave [%s]", u);
        CHECK(mpduri_from_vfs("/sd/Album.cue#03", u, sizeof(u)) &&
              strcmp(u, "sd/Album.cue#03") == 0, "a cue uri gave [%s]", u);

        /* A volume root is not a file. */
        CHECK(!mpduri_from_vfs("/sd", u, sizeof(u)), "[/sd] became a uri");
        CHECK(!mpduri_from_vfs("/sd/", u, sizeof(u)), "[/sd/] became a uri");
        /* A prefix that merely looks like a mount is not one. */
        CHECK(!mpduri_from_vfs("/sdcard/x.flac", u, sizeof(u)),
              "[/sdcard/x.flac] was read as being under /sd");
        CHECK(!mpduri_from_vfs("/usbstick/x.flac", u, sizeof(u)),
              "[/usbstick/x.flac] was read as being under /usb");
        /* A stream is not a library file and has no uri. */
        CHECK(!mpduri_from_vfs("http://example.org/stream.mp3", u, sizeof(u)),
              "a stream url became a uri");
        CHECK(!mpduri_from_vfs("Artist/x.flac", u, sizeof(u)),
              "a uri was accepted as a vfs path");
        CHECK(!mpduri_from_vfs(NULL, u, sizeof(u)), "NULL became a uri");

        /* The round trip, over the shapes that have caught something. */
        static const char *const uris[] = {
            "a.flac", "Artist/x.flac", "A/B/C/d.mp3", "Album.cue#03",
            "The Fall/Hex Enduction Hour/01 The Classical.flac",
            "Caf\xC3\xA9/T\xC3\xB6rn.mp3",
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            for (int v = 0; v < MPDURI_VOLS; v++) {
                char vfs[MPDURI_VFS_MAX], back[MPDURI_MAX + 1];
                char want[MPDURI_MAX + 1];
                snprintf(vfs, sizeof(vfs), "%s/%s", mpduri_mount(v), uris[i]);
                snprintf(want, sizeof(want), "%s/%s", mpduri_name(v), uris[i]);
                CHECK(mpduri_from_vfs(vfs, back, sizeof(back)) &&
                      strcmp(back, want) == 0,
                      "[%s] round-tripped to [%s]", vfs, back);
                /* 5191: and the uri names the volume the path came from. */
                const char *rel = NULL;
                CHECK(mpduri_split(back, &rel) == v && strcmp(rel, uris[i]) == 0,
                      "[%s] split to vol %d [%s]", back, mpduri_split(back, NULL), rel);
            }
        }

        /* A buffer one short is a refusal, not a truncated uri that would
         * name a different file. */
        /* "sd/abcd.flac" is twelve bytes, so it needs thirteen. */
        CHECK(!mpduri_from_vfs("/sd/abcd.flac", u, 12), "from_vfs truncated");
        CHECK(mpduri_from_vfs("/sd/abcd.flac", u, 13) && strcmp(u, "sd/abcd.flac") == 0,
              "from_vfs did not fit its exact size");
    }

    /* ---- the mounts ------------------------------------------------- */
    CHECK(strcmp(mpduri_mount(MPDURI_VOL_SD), "/sd") == 0, "slot 0 is not /sd");
    CHECK(strcmp(mpduri_mount(MPDURI_VOL_USB), "/usb") == 0, "slot 1 is not /usb");
    CHECK(strcmp(mpduri_name(MPDURI_VOL_SD), "sd") == 0 && strcmp(mpduri_name(MPDURI_VOL_USB), "usb") == 0,
          "the volume names are not sd and usb");
    CHECK(mpduri_mount(-1) == NULL && mpduri_mount(MPDURI_VOLS) == NULL,
          "a slot that does not exist named a mount");
    /* The ceiling must hold the longest mount, a slash, a max uri and the
     * NUL -- otherwise a legal file is unresolvable at the far end of the
     * card and nothing says why. */
    CHECK(MPDURI_VFS_MAX >= strlen("/usb") + 1 + MPDURI_REL_MAX + 1,
          "MPDURI_VFS_MAX cannot hold a maximum path");

    /* ---- malformed bytes ------------------------------------------- */
    {
        /*
         * Not expected answers -- there are none -- but a check that no
         * input crashes the walks, and that mpduri_ok() is consistent
         * with what the two mappings will accept. The alphabet is the one
         * that matters: slashes, dots, the backslash, a high byte, a
         * control byte, and '#'.
         */
        static const char alpha[] = "ab/.\\#\x01\xC3\xA9 ";
        const size_t na = sizeof(alpha) - 1;
        unsigned seed = 20260927u;

        for (int iter = 0; iter < 40000; iter++) {
            char u[24];
            const size_t len = (size_t)(seed % (sizeof(u) - 1));
            for (size_t i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                u[i] = alpha[(seed >> 16) % na];
            }
            u[len] = '\0';
            seed = seed * 1103515245u + 12345u;

            const bool good = mpduri_ok(u, false);

            /* from_vfs must never emit something mpduri_ok() rejects: if
             * it did, the player would publish a uri no client could send
             * back. */
            char vfs[MPDURI_VFS_MAX], back[MPDURI_MAX + 1];
            snprintf(vfs, sizeof(vfs), "/sd/%s", u);
            if (mpduri_from_vfs(vfs, back, sizeof(back))) {
                checks++;
                if (!mpduri_ok(back, false)) {
                    failures++;
                    printf("FAIL %s:%d: from_vfs emitted [%s], not a uri\n",
                           __FILE__, __LINE__, back);
                }
            } else {
                /* The only reason to refuse a path built from a good uri
                 * is that it did not fit, and these all do. */
                checks++;
                if (good) {
                    failures++;
                    printf("FAIL %s:%d: [%s] is a uri but /sd/ + it is not\n",
                           __FILE__, __LINE__, u);
                }
            }

            /* And the directory form agrees with the shape test. */
            char d[MPDURI_MAX + 4];
            checks++;
            if (mpduri_dir(u, d, sizeof(d)) != mpduri_ok(u, true)) {
                failures++;
                printf("FAIL %s:%d: dir and ok disagree on [%s]\n",
                       __FILE__, __LINE__, u);
            }
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
