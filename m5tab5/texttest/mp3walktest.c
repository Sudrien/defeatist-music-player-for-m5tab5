/*
 * mp3walktest.c -- u-defeatist-0040's walker, lifted out of decoder.c,
 * against minimp3's own index.
 *
 * The Makefile copies everything between MP3WALK-BEGIN and MP3WALK-END in
 * ../main/decoder.c into mp3walk_lifted.inc, so this runs the code as it
 * is in the file. minimp3 is the pinned copy the build fetches.
 *
 * For each file: minimp3 opened as decoder.c opens it (a scan; for a file
 * with a Xing/Info frame, the index a first seek builds), then the walker
 * over the same bytes, in one step and in many. Every entry the walker
 * keeps must be a frame of minimp3's index with the same sample count;
 * the many-step walk must give the same table as the one-step walk and
 * never read past what each step was allowed; the length must agree.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
#define heap_caps_malloc(n, c)    malloc(n)
#define heap_caps_calloc(k, n, c) calloc((k), (n))
#define heap_caps_free(p)         free(p)

typedef struct mp3walk mp3walk_t;
typedef bool (*mp3walk_read_t)(void *ctx, uint64_t off, void *dst, size_t n);
mp3walk_t *mp3walk_new(int max, uint32_t spacing_sec);
void mp3walk_free(mp3walk_t *w);
bool mp3walk_step(mp3walk_t *w, mp3walk_read_t read, void *ctx, uint64_t avail, bool whole);
bool mp3walk_table(const mp3walk_t *w, const uint32_t **offset, const uint32_t **frame,
                   int *count, uint32_t *spacing_sec, uint64_t *frames,
                   uint32_t *hz, int *channels, int *kbps);

#include "mp3walk_lifted.inc"

static int s_fail, s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

/* ---- the bytes, in memory ------------------------------------------- */

typedef struct {
    const uint8_t *b;
    size_t len;
    uint64_t avail;             /* this step's allowance */
    bool over;                  /* a read past it */
} src_t;

static bool src_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    src_t *s = ctx;
    if (off + n > s->avail || off + n > s->len) { s->over = true; return false; }
    memcpy(dst, s->b + off, n);
    return true;
}

/* minimp3's io over the same bytes, as decoder.c's mp3_io_read/seek. */
typedef struct { const uint8_t *b; size_t len, pos; } mio_t;
static size_t mio_read(void *buf, size_t size, void *user)
{
    mio_t *m = user;
    const size_t n = m->pos + size > m->len ? m->len - m->pos : size;
    memcpy(buf, m->b + m->pos, n);
    m->pos += n;
    return n;
}
static int mio_seek(uint64_t pos, void *user)
{
    mio_t *m = user;
    if (pos > m->len) return -1;
    m->pos = (size_t)pos;
    return 0;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* ---- one file --------------------------------------------------------- */

typedef struct {
    uint32_t off[4096], frm[4096];
    int n;
    uint32_t spacing, hz;
    uint64_t frames;
    int ch, kbps;
    bool ok, over;
} walked_t;

/* `step` 0: one whole step. Otherwise steps of `step` bytes (an odd prime
 * size goes through every alignment), then the whole. */
static void walk(const uint8_t *b, size_t len, size_t step, int max, walked_t *out)
{
    memset(out, 0, sizeof(*out));
    mp3walk_t *w = mp3walk_new(max, 2);
    src_t s = { .b = b, .len = len };
    bool ok = true;
    if (step) {
        for (uint64_t a = step; a < len && ok; a += step) {
            s.avail = a;
            ok = mp3walk_step(w, src_read, &s, a, false);
        }
    }
    s.avail = len;
    ok = ok && mp3walk_step(w, src_read, &s, len, true);
    const uint32_t *o, *f;
    out->over = s.over;
    out->ok = ok && mp3walk_table(w, &o, &f, &out->n, &out->spacing, &out->frames,
                                  &out->hz, &out->ch, &out->kbps);
    if (out->ok) {
        memcpy(out->off, o, (size_t)out->n * 4);
        memcpy(out->frm, f, (size_t)out->n * 4);
    }
    mp3walk_free(w);
}

static void one(const char *name, const uint8_t *b, size_t len, int max)
{
    /* minimp3, as decoder.c opens it */
    mp3dec_ex_t ex;
    memset(&ex, 0, sizeof(ex));
    mio_t m = { .b = b, .len = len };
    mp3dec_io_t io = { .read = mio_read, .read_data = &m, .seek = mio_seek, .seek_data = &m };
    CHECK(mp3dec_ex_open_cb(&ex, &io, MP3D_SEEK_TO_SAMPLE) == 0);
    const int xing = ex.vbr_tag_found;
    if (xing) CHECK(mp3dec_ex_seek(&ex, 44100) == 0);      /* builds the index, as a first seek */
    const mp3dec_frame_t *fr = ex.index.frames;
    const size_t nf = ex.index.num_frames;
    const int ch = ex.info.channels;
    CHECK(fr && nf > 2 && ch > 0);

    static walked_t one_step, steps, small;
    walk(b, len, 0, max, &one_step);
    walk(b, len, 7919, max, &steps);
    walk(b, len, 65536 + 3, max, &small);
    CHECK(one_step.ok && steps.ok && small.ok);
    CHECK(!one_step.over && !steps.over && !small.over);    /* never past `avail` */

    /* the same table however the bytes arrived */
    CHECK(steps.n == one_step.n && small.n == one_step.n);
    CHECK(!memcmp(steps.off, one_step.off, (size_t)one_step.n * 4) &&
          !memcmp(steps.frm, one_step.frm, (size_t)one_step.n * 4));
    CHECK(!memcmp(small.off, one_step.off, (size_t)one_step.n * 4) &&
          !memcmp(small.frm, one_step.frm, (size_t)one_step.n * 4));
    CHECK(steps.frames == one_step.frames && steps.spacing == one_step.spacing);

    /* every entry a frame of minimp3's index, at its sample */
    int found = 0;
    size_t k = 0;
    for (int i = 0; i < one_step.n; i++) {
        while (k < nf && fr[k].offset < one_step.off[i]) k++;
        if (k < nf && fr[k].offset == one_step.off[i] &&
            fr[k].sample == (uint64_t)one_step.frm[i] * (uint64_t)ch) found++;
    }
    CHECK(found == one_step.n);
    CHECK(one_step.off[0] == fr[0].offset && one_step.frm[0] == 0);
    CHECK(one_step.n <= max);

    /* the length: all of minimp3's frames counted */
    const uint64_t ref_total = fr[nf - 1].sample / (uint64_t)ch +
                               hdr_frame_samples(b + fr[nf - 1].offset);
    CHECK(one_step.frames == ref_total);
    CHECK(one_step.hz == (uint32_t)ex.info.hz && one_step.ch == ch);
    /* and what decoder_install_index() would accept: increasing */
    bool inc = true;
    for (int i = 1; i < one_step.n; i++)
        if (one_step.off[i] <= one_step.off[i - 1] || one_step.frm[i] <= one_step.frm[i - 1]) inc = false;
    CHECK(inc);

    printf("%-34s %s %6zu frames; %3d entries every %2u s, %.1f s; %d of them in minimp3's index\n",
           name, xing ? "xing" : "scan", nf, one_step.n, one_step.spacing,
           (double)one_step.frames / one_step.hz, found);
    mp3dec_ex_close(&ex);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../test_audio_files";
    static const char *const files[] = {
        "03 mp3-cbr-noxing.mp3", "04 mp3-vbr-xing.mp3", "12 mp3-bigart-noxing.mp3",
    };
    char p[512];
    uint8_t *b03 = NULL;
    size_t l03 = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
        size_t len = 0;
        uint8_t *b = slurp(p, &len);
        if (!b) { fprintf(stderr, "FAIL: cannot read %s\n", p); s_fail++; continue; }
        one(files[i], b, len, 256);
        if (i == 0) { b03 = b; l03 = len; } else free(b);
    }
    if (b03) {
        /* an ID3v1 and an APEv2 footer: minimp3 drops both at the end */
        uint8_t *t = malloc(l03 + 32 + 40 + 128);
        memcpy(t, b03, l03);
        size_t n = l03;
        static const uint8_t ape_body[40] = { 0 };
        memcpy(t + n, ape_body, 40); n += 40;
        uint8_t ft[32] = "APETAGEX";
        ft[12] = 40 + 32;       /* tag size, little-endian, footer included */
        memcpy(t + n, ft, 32); n += 32;
        uint8_t v1[128] = "TAG";
        memcpy(t + n, v1, 128); n += 128;
        one("03 + APEv2 + ID3v1", t, n, 256);
        free(t);

        /* junk before the first frame (minimp3 does not count it in its
         * offsets once it has skipped it, and neither must this) */
        t = malloc(l03 + 1000);
        for (int j = 0; j < 1000; j++) t[j] = (uint8_t)(j * 37 + 11) & 0x7f;
        memcpy(t + 1000, b03, l03);
        one("1000 bytes of junk + 03", t, l03 + 1000, 256);
        free(t);

        /* full at 16 entries: thinned, the spacing doubling */
        one("03, at most 16 entries", b03, l03, 16);

        /* ten minutes of it, end to end: past 256 entries at 2 s, so
         * thinned at the real cap, as a podcast is */
        t = malloc(l03 * 10);
        for (int j = 0; j < 10; j++) memcpy(t + (size_t)j * l03, b03, l03);
        one("03 x 10, at most 256 entries", t, l03 * 10, 256);
        free(t);
        free(b03);
    }
    if (s_fail) { fprintf(stderr, "mp3walktest: %d of %d checks failed\n", s_fail, s_checks); return 1; }
    printf("mp3walktest: %d checks, all passed\n", s_checks);
    return 0;
}
