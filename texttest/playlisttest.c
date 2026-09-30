/*
 * playlisttest.c -- playlist.c, pinned before MPD.md step 4b changes
 * what it is built on.
 *
 * playlist.c had no test. Step 4b replaces its storage -- an array of
 * strdup'd paths -- with mpdqueue.c, and the promise that makes 4b
 * acceptable is that the glass behaves exactly as before. So this is
 * written first, against the file as it is, and the switch-over has to
 * pass it UNCHANGED. A test adjusted to fit the new code would prove
 * nothing about the old behaviour.
 *
 * The expectations come from playlist.h's contract. Three behaviours the
 * header does not spell out are pinned as well, because callers rely on
 * them and a rewrite could drop them silently -- each is marked
 * CHARACTERISATION where it is checked:
 *   - a load with a NULL or empty dir changes nothing (it returns before
 *     clearing);
 *   - playlist_next(ALL) with no current track starts at the top, where
 *     playlist_peek_next(ALL) says there is nothing;
 *   - a failed opendir leaves the list empty and the dir "", but an empty
 *     folder that opened keeps its name.
 *
 * The directory is REAL: a temporary one on the host, so readdir(),
 * d_type and the filtering run as they do on the card. The rest of
 * playlist.c's world is stood in for below -- the decoder's extension
 * check, the storage helpers, the cue sheets and esp_random().
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../main/playlist.h"
#include "../main/cuedir.h"
#include "../main/decoder.h"
#include "../main/storage.h"
#include "esp_random.h"

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

/* ---- the world, stood in for ---------------------------------------- */

/* What decoder.c's table accepts is not the point here; that SOME
 * extensions pass and others do not is. Case-insensitive, as the real
 * one's format_index() is. */
bool decoder_supports(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".ogg") == 0;
}

/* storage.h's rules: dotfiles, and so "." and "..", are hidden. */
bool storage_is_hidden(const char *name)
{
    return name[0] == '.';
}

bool storage_join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    const int n = snprintf(out, out_len, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_len;
}

/* esp_random(): a fixed sequence by default, so a shuffle failure
 * reproduces; a test can install its own. */
static uint32_t s_rng = 12345;
uint32_t esp_random(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

/* The cue sheets: one fake folder's worth, configured per test. */
struct cuedir { int n; };
static const char *s_cue_dir;           /* the folder that has a sheet */
static const char *s_cue_names[4];      /* its virtual tracks */
static int         s_cue_n;
static const char *s_cue_hidden[4];     /* the audio it covers */
static int         s_cue_hidden_n;
static int         s_cue_live;          /* loads minus frees */
static struct cuedir s_cue_obj;

cuedir_t *cuedir_load(const char *dir, storage_io_class_t cls)
{
    (void)cls;
    if (!s_cue_dir || strcmp(dir, s_cue_dir) != 0) return NULL;
    s_cue_live++;
    s_cue_obj.n = s_cue_n;
    return &s_cue_obj;
}
void cuedir_free(cuedir_t *cd) { if (cd) s_cue_live--; }
bool cuedir_hides(const cuedir_t *cd, const char *name)
{
    if (!cd) return false;
    for (int i = 0; i < s_cue_hidden_n; i++)
        if (strcmp(name, s_cue_hidden[i]) == 0) return true;
    return false;
}
int cuedir_count(const cuedir_t *cd) { return cd ? cd->n : 0; }
const char *cuedir_name(const cuedir_t *cd, int i)
{
    return (cd && i >= 0 && i < cd->n) ? s_cue_names[i] : NULL;
}

/* ---- a real directory ------------------------------------------------ */

static char s_root[256];

static void touch(const char *dir, const char *name)
{
    char p[768];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

static void mkdir_in(const char *dir, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", dir, name);
    mkdir(out, 0700);
}

static void rm_rf(const char *path)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) printf("warning: could not remove %s\n", path);
}

/* The name part of entry i, for readable checks. */
static const char *name_at(int i)
{
    const char *p = playlist_path(i);
    if (!p) return "(null)";
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(void)
{
    snprintf(s_root, sizeof(s_root), "/tmp/playlisttest.XXXXXX");
    if (!mkdtemp(s_root)) { printf("mkdtemp failed\n"); return 1; }

    /* ---- a folder: filtering and order ------------------------------ */
    char album[512];
    mkdir_in(s_root, "album", album, sizeof(album));
    /* a/B is the pair that tells case-insensitive from plain strcmp:
     * strcmp puts 'B' (0x42) before 'a' (0x61). */
    touch(album, "B.mp3");
    touch(album, "a.mp3");
    touch(album, "c.MP3");                 /* extension case */
    touch(album, "d.flac");
    touch(album, "cover.jpg");             /* not audio */
    touch(album, "notes.txt");
    touch(album, "._a.mp3");               /* AppleDouble sidecar */
    touch(album, ".hidden.mp3");
    char sub[512];
    mkdir_in(album, "disc2.mp3", sub, sizeof(sub));   /* a DIRECTORY named like audio */

    CHECK(playlist_load_dir(album) == ESP_OK, "load an album");
    CHECK(playlist_count() == 4, "four playable files, got %d", playlist_count());
    CHECK(strcmp(name_at(0), "a.mp3") == 0 && strcmp(name_at(1), "B.mp3") == 0 &&
          strcmp(name_at(2), "c.MP3") == 0 && strcmp(name_at(3), "d.flac") == 0,
          "case-insensitive order: %s %s %s %s", name_at(0), name_at(1), name_at(2), name_at(3));
    {
        char want[768];
        snprintf(want, sizeof(want), "%s/a.mp3", album);
        CHECK(playlist_path(0) && strcmp(playlist_path(0), want) == 0,
              "entries are full paths: %s", playlist_path(0));
        CHECK(playlist_index_of(want) == 0, "index_of a full path");
    }
    CHECK(playlist_path(-1) == NULL && playlist_path(4) == NULL, "path out of range is NULL");
    CHECK(playlist_index_of("a.mp3") == -1, "index_of compares whole paths, not names");
    CHECK(playlist_index_of(NULL) == -1, "index_of NULL");
    CHECK(strcmp(playlist_dir(), album) == 0, "dir is the folder loaded");
    CHECK(playlist_current() == -1, "a load leaves no current track");

    /* ---- the cursor, under each order ------------------------------- */
    playlist_set_current(7);
    CHECK(playlist_current() == -1, "set_current out of range is -1");

    /* CHARACTERISATION: next(ALL) with no current starts at the top;
     * peek(ALL) with no current says nothing. */
    CHECK(playlist_peek_next(PLAY_ORDER_ALL) == NULL, "peek ALL with no current: NULL");
    CHECK(playlist_has_next(PLAY_ORDER_ALL) == false, "has_next ALL with no current: false");
    {
        const char *p = playlist_next(PLAY_ORDER_ALL);
        CHECK(p && strcmp(p, playlist_path(0)) == 0, "next ALL with no current is the first");
        CHECK(playlist_current() == 0, "and makes it current");
    }

    playlist_set_current(1);
    CHECK(playlist_current() == 1, "set_current 1");
    CHECK(playlist_peek_next(PLAY_ORDER_ALL) && strcmp(playlist_peek_next(PLAY_ORDER_ALL), playlist_path(2)) == 0,
          "peek ALL is the next entry");
    CHECK(playlist_current() == 1, "peek does not move");
    CHECK(playlist_peek_next(PLAY_ORDER_ONE) == NULL, "peek ONE: nothing follows");
    CHECK(playlist_peek_next(PLAY_ORDER_SHUFFLE) == NULL, "peek SHUFFLE: unpredictable, so NULL");
    CHECK(playlist_peek_next(PLAY_ORDER_REPEAT_ONE) &&
          strcmp(playlist_peek_next(PLAY_ORDER_REPEAT_ONE), playlist_path(1)) == 0,
          "peek REPEAT_ONE is the current track");

    CHECK(playlist_has_next(PLAY_ORDER_ALL), "has_next ALL mid-list");
    CHECK(playlist_has_next(PLAY_ORDER_ONE), "has_next ONE mid-list (the skip button works)");
    CHECK(playlist_has_next(PLAY_ORDER_SHUFFLE), "has_next SHUFFLE");
    CHECK(playlist_has_next(PLAY_ORDER_REPEAT_ONE), "has_next REPEAT_ONE with a current");

    CHECK(playlist_next(PLAY_ORDER_ONE) == NULL, "next ONE: stop");
    CHECK(playlist_current() == 1, "and does not move");
    {
        const char *p = playlist_next(PLAY_ORDER_REPEAT_ONE);
        CHECK(p && strcmp(p, playlist_path(1)) == 0, "next REPEAT_ONE is the same track");
        CHECK(playlist_current() == 1, "without moving");
    }
    {
        const char *p = playlist_next(PLAY_ORDER_ALL);
        CHECK(p && strcmp(p, playlist_path(2)) == 0 && playlist_current() == 2, "next ALL advances");
        p = playlist_next(PLAY_ORDER_ALL);
        CHECK(p && playlist_current() == 3, "to the last");
        CHECK(!playlist_has_next(PLAY_ORDER_ALL), "has_next ALL at the end: false");
        CHECK(!playlist_has_next(PLAY_ORDER_ONE), "has_next ONE at the end: false");
        CHECK(playlist_has_next(PLAY_ORDER_REPEAT_ONE), "has_next REPEAT_ONE at the end: true");
        CHECK(playlist_has_next(PLAY_ORDER_SHUFFLE), "has_next SHUFFLE at the end: true");
        CHECK(playlist_peek_next(PLAY_ORDER_ALL) == NULL, "peek ALL at the end: NULL");
        CHECK(playlist_next(PLAY_ORDER_ALL) == NULL, "next ALL at the end: stop");
        CHECK(playlist_current() == 3, "and stays on the last");
        /* 5261: repeat-all at the end goes back to the top. */
        CHECK(playlist_has_next(PLAY_ORDER_REPEAT_ALL), "has_next REPEAT_ALL at the end: true");
        CHECK(playlist_peek_next(PLAY_ORDER_REPEAT_ALL) &&
              strcmp(playlist_peek_next(PLAY_ORDER_REPEAT_ALL), playlist_path(0)) == 0,
              "peek REPEAT_ALL at the end: the top");
        p = playlist_next(PLAY_ORDER_REPEAT_ALL);
        CHECK(p && strcmp(p, playlist_path(0)) == 0 && playlist_current() == 0,
              "next REPEAT_ALL at the end: the top");
        p = playlist_next(PLAY_ORDER_REPEAT_ALL);
        CHECK(p && playlist_current() == 1, "and on from there, as ALL");
        p = playlist_next(PLAY_ORDER_ALL);
        p = playlist_next(PLAY_ORDER_ALL);
        CHECK(playlist_current() == 3, "back at the last, for what follows");
    }

    /* prev */
    {
        const char *p = playlist_prev();
        CHECK(p && strcmp(p, playlist_path(2)) == 0 && playlist_current() == 2, "prev goes back one");
        playlist_set_current(0);
        CHECK(playlist_prev() == NULL && playlist_current() == 0, "prev at the top: NULL, stays");
        playlist_set_current(-1);
        CHECK(playlist_prev() == NULL, "prev with no current: NULL");
    }

    /* REPEAT_ONE with no current */
    playlist_set_current(-1);
    CHECK(playlist_next(PLAY_ORDER_REPEAT_ONE) == NULL, "next REPEAT_ONE with no current: NULL");
    CHECK(playlist_peek_next(PLAY_ORDER_REPEAT_ONE) == NULL, "peek REPEAT_ONE with no current: NULL");
    CHECK(!playlist_has_next(PLAY_ORDER_REPEAT_ONE), "has_next REPEAT_ONE with no current: false");

    /* ---- shuffle ---------------------------------------------------- */
    char many[512];
    mkdir_in(s_root, "many", many, sizeof(many));
    for (int i = 0; i < 9; i++) {
        char n[32];
        snprintf(n, sizeof(n), "t%02d.mp3", i);
        touch(many, n);
    }
    for (int seed = 1; seed <= 200; seed++) {
        s_rng = (uint32_t)seed * 2654435761u;
        CHECK(playlist_load_dir(many) == ESP_OK && playlist_count() == 9, "load 9");
        /* The current track counts as played: set_current marks it. */
        playlist_set_current(4);
        int seen[9] = { 0 };
        seen[4] = 1;
        bool ok = true;
        for (int k = 0; k < 8; k++) {
            const char *p = playlist_next(PLAY_ORDER_SHUFFLE);
            const int i = playlist_index_of(p);
            if (i < 0 || seen[i] || playlist_current() != i) { ok = false; break; }
            seen[i] = 1;
        }
        CHECK(ok, "seed %d: a pass plays every other track once, current not repeated", seed);
        /* Exhausted: the next pick starts a fresh pass that does not
         * repeat the track just played. */
        const int last = playlist_current();
        const char *p = playlist_next(PLAY_ORDER_SHUFFLE);
        CHECK(p && playlist_index_of(p) != last, "seed %d: the wrap does not repeat back to back", seed);
        /* And that fresh pass is itself a permutation of the other 8. */
        int seen2[9] = { 0 };
        seen2[playlist_index_of(p)] = 1;
        seen2[last] = 1;
        bool ok2 = true;
        for (int k = 0; k < 7; k++) {
            const int i = playlist_index_of(playlist_next(PLAY_ORDER_SHUFFLE));
            if (i < 0 || seen2[i]) { ok2 = false; break; }
            seen2[i] = 1;
        }
        CHECK(ok2, "seed %d: the second pass is a permutation too", seed);
    }
    /* A load resets the history. */
    s_rng = 99;
    playlist_load_dir(many);
    {
        int seen[9] = { 0 };
        bool ok = true;
        for (int k = 0; k < 9; k++) {
            const int i = playlist_index_of(playlist_next(PLAY_ORDER_SHUFFLE));
            if (i < 0 || seen[i]) { ok = false; break; }
            seen[i] = 1;
        }
        CHECK(ok, "after a load, a pass covers all nine");
    }
    /* One track: shuffle plays it again rather than stopping. */
    char one[512];
    mkdir_in(s_root, "one", one, sizeof(one));
    touch(one, "only.mp3");
    playlist_load_dir(one);
    CHECK(playlist_next(PLAY_ORDER_SHUFFLE) != NULL && playlist_current() == 0, "shuffle of one plays it");
    CHECK(playlist_next(PLAY_ORDER_SHUFFLE) != NULL && playlist_current() == 0, "and again");

    /* ---- cue sheets -------------------------------------------------- */
    char cue[512];
    mkdir_in(s_root, "cue", cue, sizeof(cue));
    touch(cue, "Disc.flac");               /* covered by the sheet */
    touch(cue, "Bonus.mp3");               /* not covered */
    touch(cue, "Disc.cue");                /* not audio to decoder_supports */
    s_cue_dir = cue;
    s_cue_names[0] = "Disc.cue#01";
    s_cue_names[1] = "Disc.cue#02";
    s_cue_n = 2;
    s_cue_hidden[0] = "Disc.flac";
    s_cue_hidden_n = 1;
    CHECK(playlist_load_dir(cue) == ESP_OK, "load a folder with a sheet");
    CHECK(playlist_count() == 3, "bonus and two sheet tracks, got %d", playlist_count());
    CHECK(strcmp(name_at(0), "Bonus.mp3") == 0 && strcmp(name_at(1), "Disc.cue#01") == 0 &&
          strcmp(name_at(2), "Disc.cue#02") == 0,
          "sheet tracks sorted in with the rest: %s %s %s", name_at(0), name_at(1), name_at(2));
    CHECK(playlist_index_of("x") == -1 && s_cue_live == 0, "the sheet set is freed after the load");
    s_cue_dir = NULL;

    /* Reading past the end of a list that REPLACED a longer one: the
     * slots after the last entry are the old list's, freed. A peek or a
     * next that looked there would hand out a dangling pointer, which a
     * fresh array hides by being all NULL. */
    playlist_load_dir(many);                /* 9 */
    playlist_load_dir(album);               /* 4 */
    playlist_set_current(3);
    CHECK(playlist_peek_next(PLAY_ORDER_ALL) == NULL, "peek ALL at the end of a shorter list: NULL");
    CHECK(playlist_next(PLAY_ORDER_ALL) == NULL, "next ALL at the end of a shorter list: NULL");
    CHECK(playlist_path(4) == NULL, "path past the end of a shorter list: NULL");
    CHECK(!playlist_has_next(PLAY_ORDER_ALL), "has_next at the end of a shorter list: false");

    /* ---- failures ---------------------------------------------------- */
    playlist_load_dir(album);
    const int before = playlist_count();
    playlist_set_current(2);
    /* CHARACTERISATION: a bad argument changes nothing. */
    CHECK(playlist_load_dir(NULL) == ESP_ERR_INVALID_ARG, "NULL dir");
    CHECK(playlist_load_dir("") == ESP_ERR_INVALID_ARG, "empty dir");
    CHECK(playlist_count() == before && playlist_current() == 2 && strcmp(playlist_dir(), album) == 0,
          "a bad argument leaves the list, the cursor and the dir alone");

    /* CHARACTERISATION: a folder that will not open empties the list and
     * forgets the dir. */
    char gone[600];
    snprintf(gone, sizeof(gone), "%s/no-such-folder", s_root);
    CHECK(playlist_load_dir(gone) == ESP_ERR_NOT_FOUND, "missing folder: NOT_FOUND");
    CHECK(playlist_count() == 0 && playlist_current() == -1 && playlist_dir()[0] == '\0',
          "and the list is empty, with no dir");
    CHECK(playlist_next(PLAY_ORDER_ALL) == NULL && playlist_prev() == NULL &&
          playlist_peek_next(PLAY_ORDER_REPEAT_ONE) == NULL && !playlist_has_next(PLAY_ORDER_SHUFFLE),
          "an empty list offers nothing under any order");

    /* CHARACTERISATION: an empty folder opens, keeps its name, and says
     * NOT_FOUND. */
    char empty[512];
    mkdir_in(s_root, "empty", empty, sizeof(empty));
    touch(empty, "readme.txt");
    CHECK(playlist_load_dir(empty) == ESP_ERR_NOT_FOUND, "no playable files: NOT_FOUND");
    CHECK(playlist_count() == 0 && strcmp(playlist_dir(), empty) == 0, "empty, but the dir is kept");

    /* clear */
    playlist_load_dir(album);
    playlist_set_current(1);
    playlist_clear();
    CHECK(playlist_count() == 0 && playlist_current() == -1 && playlist_dir()[0] == '\0',
          "clear empties and forgets");
    CHECK(playlist_path(0) == NULL, "nothing to read after clear");

    /* ---- the ceiling ------------------------------------------------- */
    char big[512];
    mkdir_in(s_root, "big", big, sizeof(big));
    for (int i = 0; i < PLAYLIST_MAX + 6; i++) {
        char n[32];
        snprintf(n, sizeof(n), "%05d.mp3", i);
        touch(big, n);
    }
    CHECK(playlist_load_dir(big) == ESP_OK, "an over-full folder still loads");
    CHECK(playlist_count() == PLAYLIST_MAX, "truncated at PLAYLIST_MAX, got %d", playlist_count());
    {
        bool sorted = true;
        for (int i = 1; i < playlist_count(); i++)
            if (strcasecmp(playlist_path(i - 1), playlist_path(i)) > 0) sorted = false;
        CHECK(sorted, "and what survives is sorted");
    }
    /* Loading something small afterwards leaves nothing of the big one. */
    playlist_load_dir(one);
    CHECK(playlist_count() == 1 && playlist_path(1) == NULL, "a smaller load replaces all of it");

    /* ---- 5171: edits keep the cursor and the shuffle history --------
     *
     * Added after the pinned sections above, which are unchanged. The
     * album is a.mp3 B.mp3 c.MP3 d.flac; "N(i)" below is its name. */
    char xp[768], yp[768];
    snprintf(xp, sizeof(xp), "%s/x.mp3", s_root);
    snprintf(yp, sizeof(yp), "%s/y.mp3", s_root);

    /* Insert before the current entry: the cursor follows its entry. */
    playlist_load_dir(album);
    playlist_set_current(1);                                /* B */
    CHECK(playlist_add(xp, 0) == 0, "add at the top");
    CHECK(playlist_count() == 5 && strcmp(playlist_path(0), xp) == 0, "x is first");
    CHECK(playlist_current() == 2 && strcmp(name_at(2), "B.mp3") == 0,
          "the cursor moved with B: %d", playlist_current());
    CHECK(strcmp(strrchr(playlist_peek_next(PLAY_ORDER_ALL), '/') + 1, "c.MP3") == 0,
          "and next is still c");
    /* After the cursor: it stays. At the end: negative `at`. */
    CHECK(playlist_add(yp, -1) == 5 && playlist_current() == 2, "add at the end");
    CHECK(playlist_add(xp, 7) == -1 && playlist_count() == 6, "past the end is refused");
    CHECK(playlist_add("", 0) == -1 && playlist_count() == 6, "an empty path is refused");

    /* Play next: straight after the cursor. */
    CHECK(playlist_add_next(yp) == 3, "add_next goes after the current entry");
    CHECK(playlist_next(PLAY_ORDER_ALL) && strcmp(playlist_path(playlist_current()), yp) == 0,
          "and is what next plays");

    /* Remove before the cursor, and after it. */
    playlist_load_dir(album);
    playlist_set_current(2);                                /* c */
    CHECK(playlist_remove(0) && playlist_current() == 1 &&
          strcmp(name_at(1), "c.MP3") == 0, "remove before: the cursor follows c");
    CHECK(playlist_remove(2) && playlist_current() == 1 && playlist_count() == 2,
          "remove after: the cursor stays");
    CHECK(!playlist_remove(-1) && !playlist_remove(2), "no such entry");

    /* Remove the entry playing: current -1, and the gap is the cursor. */
    playlist_load_dir(album);
    playlist_set_current(1);                                /* B */
    CHECK(playlist_remove(1), "remove the current entry");
    CHECK(playlist_current() == -1, "the current entry is no longer in the list");
    CHECK(playlist_has_next(PLAY_ORDER_ALL), "but there is a next");
    CHECK(strcmp(strrchr(playlist_peek_next(PLAY_ORDER_ALL), '/') + 1, "c.MP3") == 0,
          "peek: what slid into its place");
    CHECK(playlist_peek_next(PLAY_ORDER_REPEAT_ONE) == NULL,
          "repeat-one has nothing to repeat");
    {
        const char *n = playlist_next(PLAY_ORDER_ALL);
        CHECK(n && strcmp(strrchr(n, '/') + 1, "c.MP3") == 0 && playlist_current() == 1,
              "next plays c and the cursor is on it");
    }
    playlist_load_dir(album);
    playlist_set_current(2);                                /* c */
    playlist_remove(2);
    {
        const char *p = playlist_prev();
        CHECK(p && strcmp(strrchr(p, '/') + 1, "B.mp3") == 0 && playlist_current() == 1,
              "prev from the gap: what was before it");
    }
    /* The gap moves with the entries around it. */
    playlist_load_dir(album);
    playlist_set_current(1);
    playlist_remove(1);                                     /* gap before c (1) */
    playlist_remove(0);                                     /* a goes: gap 0 */
    CHECK(strcmp(strrchr(playlist_peek_next(PLAY_ORDER_ALL), '/') + 1, "c.MP3") == 0,
          "a removal before the gap moves it");
    playlist_remove(0);                                     /* c goes: d slides in */
    CHECK(strcmp(strrchr(playlist_peek_next(PLAY_ORDER_ALL), '/') + 1, "d.flac") == 0,
          "removing the successor makes the next one the successor");
    CHECK(playlist_add_next(xp) == 0 &&
          strcmp(playlist_peek_next(PLAY_ORDER_ALL), xp) == 0,
          "add_next fills the gap");
    playlist_set_current(1);
    CHECK(playlist_peek_next(PLAY_ORDER_ALL) == NULL, "setting current forgets the gap");
    /* The last entry, playing, removed: nothing follows. */
    playlist_load_dir(album);
    playlist_set_current(3);
    playlist_remove(3);
    CHECK(!playlist_has_next(PLAY_ORDER_ALL) && playlist_next(PLAY_ORDER_ALL) == NULL,
          "the last entry removed: nothing next");

    /* Move: the cursor follows the entry it is on, or makes room. */
    playlist_load_dir(album);
    playlist_set_current(1);                                /* B */
    CHECK(playlist_move(1, 3) && playlist_current() == 3 && strcmp(name_at(3), "B.mp3") == 0,
          "moving the current entry moves the cursor");
    CHECK(playlist_move(0, 3) && playlist_current() == 2, "a move across it from above");
    CHECK(playlist_move(3, 0) && playlist_current() == 3, "and from below");
    CHECK(strcmp(name_at(playlist_current()), "B.mp3") == 0, "still on B");
    CHECK(playlist_move(2, 2) && playlist_current() == 3, "a move to itself changes nothing");
    CHECK(!playlist_move(-1, 0) && !playlist_move(0, 4), "not a position");

    /* Shuffle's history follows the entries: a, B, c played, x inserted
     * at the top. The next two shuffle picks must be x and d, in either
     * order -- a bitmap left on the slots would call x played and B not. */
    playlist_load_dir(album);
    playlist_set_current(0);
    playlist_set_current(1);
    playlist_set_current(2);
    playlist_add(xp, 0);
    {
        const char *p1 = playlist_next(PLAY_ORDER_SHUFFLE);
        char n1[768];
        snprintf(n1, sizeof(n1), "%s", p1 ? p1 : "");
        const char *p2 = playlist_next(PLAY_ORDER_SHUFFLE);
        const char *b1 = strrchr(n1, '/'), *b2 = p2 ? strrchr(p2, '/') : NULL;
        const bool ok = b1 && b2 &&
            ((strcmp(b1, "/x.mp3") == 0 && strcmp(b2, "/d.flac") == 0) ||
             (strcmp(b1, "/d.flac") == 0 && strcmp(b2, "/x.mp3") == 0));
        CHECK(ok, "shuffle picks the two unplayed: %s then %s", n1, p2 ? p2 : "(null)");
    }
    /* And after a removal and a move. Played: a (0) and d (3). Remove B,
     * move d to the top: unplayed are c then... only c. */
    playlist_load_dir(album);
    playlist_set_current(0);
    playlist_set_current(3);
    playlist_remove(1);                     /* a c d, played a d */
    playlist_move(2, 0);                    /* d a c, played d a */
    {
        const char *p = playlist_next(PLAY_ORDER_SHUFFLE);
        CHECK(p && strcmp(strrchr(p, '/'), "/c.MP3") == 0, "the only unplayed is c: %s", p ? p : "(null)");
    }

    /* A move downwards: a and B played, a moved to the end. */
    playlist_load_dir(album);
    playlist_set_current(0);
    playlist_set_current(1);
    playlist_move(0, 3);                    /* B c d a, played B a */
    {
        const char *p1 = playlist_next(PLAY_ORDER_SHUFFLE);
        char n1[768];
        snprintf(n1, sizeof(n1), "%s", p1 ? p1 : "");
        const char *p2 = playlist_next(PLAY_ORDER_SHUFFLE);
        const char *b1 = strrchr(n1, '/'), *b2 = p2 ? strrchr(p2, '/') : NULL;
        const bool ok = b1 && b2 &&
            ((strcmp(b1, "/c.MP3") == 0 && strcmp(b2, "/d.flac") == 0) ||
             (strcmp(b1, "/d.flac") == 0 && strcmp(b2, "/c.MP3") == 0));
        CHECK(ok, "after a move down, shuffle picks c and d: %s then %s", n1, p2 ? p2 : "(null)");
    }

    /* Editing with nothing loaded: the list is still the queue. */
    playlist_clear();
    CHECK(playlist_add(xp, -1) == 0 && playlist_count() == 1, "add to an empty list");
    CHECK(playlist_next(PLAY_ORDER_ALL) && playlist_current() == 0, "and play it");

    /* 5175: shuffle in place. The cursor follows B by id; every entry is
     * still there once; the history is only B. */
    playlist_load_dir(album);
    playlist_set_current(0);
    playlist_set_current(1);                                /* B, a played */
    playlist_shuffle();
    CHECK(playlist_count() == 4, "shuffle keeps the count");
    CHECK(playlist_current() >= 0 && strcmp(name_at(playlist_current()), "B.mp3") == 0,
          "the cursor follows B: %d", playlist_current());
    {
        int seen = 0;
        for (int i = 0; i < 4; i++) {
            const char *nm = name_at(i);
            if (!strcmp(nm, "a.mp3")) seen |= 1;
            if (!strcmp(nm, "B.mp3")) seen |= 2;
            if (!strcmp(nm, "c.MP3")) seen |= 4;
            if (!strcmp(nm, "d.flac")) seen |= 8;
        }
        CHECK(seen == 15, "every entry once");
        /* Three shuffle picks before a wrap: a, c and d -- a is no longer
         * called played. */
        int picked = 0;
        for (int k = 0; k < 3; k++) {
            const char *p = playlist_next(PLAY_ORDER_SHUFFLE);
            const char *b = p ? strrchr(p, '/') + 1 : "";
            if (!strcmp(b, "a.mp3")) picked |= 1;
            if (!strcmp(b, "c.MP3")) picked |= 4;
            if (!strcmp(b, "d.flac")) picked |= 8;
        }
        CHECK(picked == 13, "the history restarts with only B played: %d", picked);
    }
    playlist_load_dir(album);
    playlist_set_current(1);
    playlist_remove(1);                                     /* a gap */
    playlist_shuffle();
    CHECK(playlist_current() == -1 && playlist_peek_next(PLAY_ORDER_ALL) == NULL,
          "a shuffle forgets the gap");
    playlist_clear();
    playlist_shuffle();
    CHECK(playlist_count() == 0 && playlist_current() == -1, "shuffling nothing");

    /* Full. */
    playlist_load_dir(big);
    CHECK(playlist_add(xp, -1) == -1 && playlist_count() == PLAYLIST_MAX, "full is refused");
    CHECK(playlist_remove(0) && playlist_add(xp, -1) == PLAYLIST_MAX - 1, "and room is room");

    /* 5252: EAT, MPD's consume. Leaving an entry forward removes it;
     * choosing one, prev and peeking do not; the last one eaten stops. */
    playlist_clear();
    CHECK(playlist_add(xp, -1) == 0 && playlist_add(yp, -1) == 1 && playlist_add(xp, -1) == 2,
          "eat: three entries");
    CHECK(playlist_next(PLAY_ORDER_EAT) && playlist_current() == 0 && playlist_count() == 3,
          "eat: starting from no current eats nothing");
    CHECK(playlist_peek_next(PLAY_ORDER_EAT) && strcmp(playlist_peek_next(PLAY_ORDER_EAT), yp) == 0 &&
          playlist_count() == 3, "eat: peek names the next and eats nothing");
    CHECK(playlist_has_next(PLAY_ORDER_EAT), "eat: has a next");
    {
        const char *p = playlist_next(PLAY_ORDER_EAT);
        CHECK(p && strcmp(p, yp) == 0 && playlist_count() == 2 && playlist_current() == 0,
              "eat: next eats the one left and plays what followed");
    }
    playlist_set_current(1);
    CHECK(playlist_count() == 2, "eat: choosing an entry eats nothing");
    CHECK(playlist_prev() && playlist_current() == 0 && playlist_count() == 2,
          "eat: prev eats nothing");
    CHECK(playlist_next(PLAY_ORDER_EAT) && playlist_count() == 1 && playlist_current() == 0,
          "eat: again");
    CHECK(!playlist_has_next(PLAY_ORDER_EAT), "eat: the last has no next");
    CHECK(playlist_next(PLAY_ORDER_EAT) == NULL && playlist_count() == 0 && playlist_current() == -1,
          "eat: the last one eaten empties the list and stops");

    playlist_clear();
    rm_rf(s_root);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
