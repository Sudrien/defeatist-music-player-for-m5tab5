/*
 * folderarttest.c -- folderart.c against real folders on the host.
 *
 * folderart.c is compiled in whole, with the four things it borrows
 * from the rest of the player replaced by small honest fakes: the
 * storage arbiter (plain stdio), the cover cache (one slot per key, as
 * many as the test needs), and the image check (JPEG or PNG magic).
 * Folders are made under a temporary directory, so stat(), fopen() and
 * the order of the name list are the real ones. What the host cannot
 * show is FAT's case folding -- "Folder.JPG" found as "folder.jpg" --
 * which is the filesystem's and not this file's.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../main/folderart.c"

/* ---- fakes ---- */
static int opens;

FILE *storage_io_open(const char *path, const char *mode)
{
    opens++;
    return fopen(path, mode);
}
int storage_io_close(FILE *f) { return fclose(f); }
size_t storage_io_fread(void *dst, size_t len, FILE *f, storage_io_class_t cls)
{
    (void)cls;
    return fread(dst, 1, len, f);
}

#define FAKE_KEYS 8
static struct { char key[256]; uint8_t *img; size_t len; } cache[FAKE_KEYS];

const uint8_t *mediacache_art(const char *path, size_t *len)
{
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (cache[i].img && strcmp(cache[i].key, path) == 0) {
            if (len) *len = cache[i].len;
            return cache[i].img;
        }
    }
    return NULL;
}
static void cache_put(const char *key, uint8_t *img, size_t len)
{
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (!cache[i].img) {
            snprintf(cache[i].key, sizeof(cache[i].key), "%s", key);
            cache[i].img = img;
            cache[i].len = len;
            return;
        }
    }
    free(img);
}
static void cache_clear(void)
{
    for (int i = 0; i < FAKE_KEYS; i++) { free(cache[i].img); cache[i].img = NULL; }
}

bool albumart_is_supported_image(const uint8_t *p, size_t len)
{
    if (len >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;
    return len >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0;
}

/* ---- the test ---- */
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

static char root[256];

static void put_file(const char *dir, const char *name, const char *kind, size_t n)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    if (!strcmp(kind, "jpg")) fwrite("\xFF\xD8\xFF\xE0", 1, 4, f);
    else if (!strcmp(kind, "png")) fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    for (size_t i = 0; i < n; i++) fputc((int)(i & 0xFF), f);
    fclose(f);
}

static void del_file(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    unlink(p);
}

static const char *mkdir_in(const char *name)
{
    static char d[8][512];
    static int k;
    char *o = d[k++ % 8];
    snprintf(o, 512, "%s/%s", root, name);
    mkdir(o, 0700);
    return o;
}

static const char *track(const char *dir, const char *name)
{
    static char t[4][600];
    static int k;
    char *o = t[k++ % 4];
    snprintf(o, 600, "%s/%s", dir, name);
    return o;
}

static esp_err_t load(const char *file, const char **name, uint8_t **img, size_t *len)
{
    return folderart_load(file, file, img, len, name);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/folderarttest.%d", (int)getpid());
    mkdir(root, 0700);

    /* The pure half. */
    char dir[64];
    CHECK(folderart_dir_of("/sd/a/b.mp3", dir, sizeof(dir)) && !strcmp(dir, "/sd/a"), "dir_of");
    CHECK(folderart_dir_of("/sd/b.mp3", dir, sizeof(dir)) && !strcmp(dir, "/sd"), "dir_of root");
    CHECK(!folderart_dir_of("b.mp3", dir, sizeof(dir)) && !dir[0], "no slash");
    CHECK(!folderart_dir_of("/b.mp3", dir, sizeof(dir)) && !dir[0], "slash at 0");
    CHECK(!folderart_dir_of("/sd/abcdefgh/x", dir, 8) && !dir[0], "too long refuses, does not cut");
    CHECK(folderart_dir_of("/sd/abc/x", dir, 8) && !strcmp(dir, "/sd/abc"), "exact fit");
    CHECK(folderart_dir_hash("/sd/Album") == folderart_dir_hash("/sd/album"), "hash folds case");
    CHECK(folderart_dir_hash("/sd/a") != folderart_dir_hash("/sd/b"), "hash differs");
    CHECK(folderart_dir_hash("") != 0, "0 is reserved");
    for (int i = 0; i < FOLDERART_NNAMES; i++) {
        for (const char *c = FOLDERART_NAMES[i]; *c; c++)
            CHECK(!(*c >= 'A' && *c <= 'Z'), "%s is not lower case", FOLDERART_NAMES[i]);
        for (int j = i + 1; j < FOLDERART_NNAMES; j++)
            CHECK(strcmp(FOLDERART_NAMES[i], FOLDERART_NAMES[j]), "%s twice", FOLDERART_NAMES[i]);
        CHECK(!strstr(FOLDERART_NAMES[i], "small"), "a thumbnail name is listed");
    }
    CHECK(FOLDERART_MPD_NNAMES == 4 && !strcmp(FOLDERART_MPD_NAMES[0], "cover.png") &&
          !strcmp(FOLDERART_MPD_NAMES[1], "cover.jpg") && !strcmp(FOLDERART_MPD_NAMES[2], "cover.tiff") &&
          !strcmp(FOLDERART_MPD_NAMES[3], "cover.bmp"), "MPD's list is MPD's, in MPD's order");

    uint8_t *img;
    size_t len;
    const char *name;

    /* Preference: cover.png beats folder.jpg, cover.jpg beats cover.png. */
    const char *a = mkdir_in("a");
    put_file(a, "folder.jpg", "jpg", 100);
    put_file(a, "cover.png", "png", 200);
    CHECK(load(track(a, "01.mp3"), &name, &img, &len) == ESP_OK && !strcmp(name, "cover.png") &&
          len == 208, "cover.png first (%s, %zu)", name ? name : "-", len);
    free(img);
    folderart_forget();
    put_file(a, "cover.jpg", "jpg", 50);
    CHECK(load(track(a, "01.mp3"), &name, &img, &len) == ESP_OK && !strcmp(name, "cover.jpg"),
          "cover.jpg before cover.png (%s)", name ? name : "-");
    free(img);

    /* An album: the first track reads, the rest copy from the cache. */
    folderart_forget();
    const char *b = mkdir_in("b");
    put_file(b, "folder.jpg", "jpg", 5000);
    opens = 0;
    const char *t1 = track(b, "01.flac");
    CHECK(!folderart_in_hand(t1), "never looked: not in hand");
    CHECK(load(t1, &name, &img, &len) == ESP_OK && len == 5004 && opens == 1, "first track reads");
    cache_put(t1, img, len);
    const char *t2 = track(b, "02.flac");
    CHECK(folderart_in_hand(t2), "donor cached: in hand");
    CHECK(load(t2, &name, &img, &len) == ESP_OK && len == 5004 && opens == 1,
          "second track copies, does not read (%d opens)", opens);
    CHECK(img != mediacache_art(t1, NULL) && !memcmp(img, mediacache_art(t1, NULL), len),
          "a copy, not the cache's own buffer");
    free(img);
    /* The donor evicted: the known name is read again, without a search. */
    cache_clear();
    CHECK(!folderart_in_hand(t2), "donor gone: not in hand");
    CHECK(load(t2, &name, &img, &len) == ESP_OK && opens == 2, "reread after eviction");
    free(img);

    /* A folder with nothing: remembered as nothing. */
    const char *c = mkdir_in("c");
    CHECK(load(track(c, "x.mp3"), &name, &img, &len) == ESP_ERR_NOT_FOUND && !img, "none");
    CHECK(folderart_in_hand(track(c, "y.mp3")), "known none: in hand");
    put_file(c, "cover.jpg", "jpg", 10);
    CHECK(load(track(c, "y.mp3"), &name, &img, &len) == ESP_ERR_NOT_FOUND,
          "remembered as none until forgotten");
    folderart_forget();
    CHECK(load(track(c, "y.mp3"), &name, &img, &len) == ESP_OK, "found after forget");
    free(img);

    /* A cover.jpg that is not a picture: refused, remembered, nothing leaks. */
    folderart_forget();
    const char *d = mkdir_in("d");
    put_file(d, "cover.jpg", "txt", 40);
    CHECK(load(track(d, "x.mp3"), &name, &img, &len) == ESP_ERR_NOT_FOUND && !img, "not an image");
    CHECK(folderart_in_hand(track(d, "x.mp3")), "and remembered");

    /* A file found by name, deleted before the next track: none, once. */
    folderart_forget();
    cache_clear();
    const char *e = mkdir_in("e");
    put_file(e, "front.png", "png", 30);
    CHECK(load(track(e, "1.mp3"), &name, &img, &len) == ESP_OK && !strcmp(name, "front.png"), "front.png");
    free(img);
    del_file(e, "front.png");
    CHECK(load(track(e, "2.mp3"), &name, &img, &len) == ESP_ERR_NOT_FOUND, "gone since");
    CHECK(folderart_in_hand(track(e, "3.mp3")), "and not looked for again");

    /* More folders than slots: the oldest is looked up again, correctly. */
    folderart_forget();
    const char *f[6];
    char nm[16];
    for (int i = 0; i < 6; i++) {
        snprintf(nm, sizeof(nm), "f%d", i);
        f[i] = mkdir_in(nm);
        if (i % 2 == 0) put_file(f[i], "album.jpg", "jpg", 5);
        const esp_err_t r = load(track(f[i], "t.mp3"), &name, &img, &len);
        CHECK((i % 2 == 0) == (r == ESP_OK), "folder %d", i);
        free(img);
    }
    CHECK(load(track(f[0], "t.mp3"), &name, &img, &len) == ESP_OK, "evicted slot looked up again");
    free(img);

    /* No folder in the path at all. */
    CHECK(load("x.mp3", &name, &img, &len) == ESP_ERR_NOT_FOUND, "bare name");

    cache_clear();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) printf("could not remove %s\n", root);

    printf("folderarttest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
