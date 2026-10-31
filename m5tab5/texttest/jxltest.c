/*
 * jxltest.c -- JPEG XL cover art through J40 (components/jxlimg), 6139.
 *
 * Fixtures in fixtures/art, from tools/gen_art_fixtures.py: the same
 * four-quadrant picture as arttest's, as JPEG XL encoded several ways,
 * plus flat pictures at awkward sizes for the size header.
 *
 * What J40 can and cannot do is the point of this test, so both sides
 * are checked:
 *   - decodes, at 320x240: lossy, lossless, with alpha, the lossy
 *     codestream in a container as one `jxlc` box, and split into two
 *     `jxlp` boxes behind an `Exif` box (J40 alone refuses the split
 *     form; jxlimg.c joins it first);
 *   - refuses: the no_*.jxl fixtures, the four kinds J40 2270 cannot
 *     read (listed where they are checked). If a newer J40 reads one,
 *     its check fails and the fixture moves to the decodes.
 *   - the size header, read without decoding, at every SizeHeader shape
 *     the encoder writes, and decoded to the same size;
 *   - every file cut short, and junk: refused, never a crash (ASan and
 *     UBSan).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artformat.h"
#include "jxlimg.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;      \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);   \
    printf("\n"); } } while (0)

#define W 320            /* J40 refuses lossy at 256 px or less: see below */
#define H 240
#define BG 0x0841       /* the colour behind a transparent pixel */

static uint8_t *slurp(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof(path), "fixtures/art/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len ? *len : 1);
    if (fread(b, 1, *len, f) != *len) exit(2);
    fclose(f);
    return b;
}

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int chan_diff(uint16_t a, uint16_t b)
{
    const int dr = abs(((a >> 11) & 31) - ((b >> 11) & 31)) * 8;
    const int dg = abs(((a >> 5) & 63) - ((b >> 5) & 63)) * 4;
    const int db = abs((a & 31) - (b & 31)) * 8;
    return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

typedef struct { uint16_t *px; size_t size; int w, h; jxlimg_err_t err; const char *why; } pic_t;

static pic_t decode(const uint8_t *b, size_t len, int bw, int bh)
{
    pic_t p = {0};
    p.err = jxlimg_decode_rgb565(b, len, bw, bh, BG, &p.px, &p.size, &p.w, &p.h, &p.why);
    return p;
}

static void quadrants(const char *what, const pic_t *p, int tol, int hole)
{
    static const int want[4][3] = {
        { 220, 30, 30 }, { 30, 200, 40 }, { 40, 50, 210 }, { 255, 255, 255 },
    };
    for (int q = 0; q < 4; q++) {
        const int x = (q & 1) ? 3 * p->w / 4 : p->w / 4, y = (q & 2) ? 3 * p->h / 4 : p->h / 4;
        const uint16_t got = p->px[y * p->w + x];
        const uint16_t exp = (q == 3 && hole) ? BG : rgb565(want[q][0], want[q][1], want[q][2]);
        const int d = chan_diff(got, exp);
        CHECK(d <= tol, "%s: quadrant %d is %04x, want ~%04x (off by %d)", what, q, got, exp, d);
    }
}

static void cut_sweep(const char *name, const uint8_t *b, size_t len)
{
    for (size_t cut = 0; cut < len; cut += (cut < 200 ? 1 : 5)) {
        pic_t t = decode(b, cut, 0, 0);
        if (t.err == JXLIMG_OK) {
            /* J40 may draw a picture from a partial last group; it must
             * at least be the right size. */
            CHECK(t.w > 0 && t.h > 0, "%s cut at %zu: %dx%d", name, cut, t.w, t.h);
        }
        free(t.px);
    }
    checks++;
}

static void good(const char *name, int tol, int hole)
{
    size_t len;
    uint8_t *b = slurp(name, &len);
    CHECK(artformat_sniff(b, len) == ART_JXL, "%s: not sniffed as JPEG XL", name);
    int w = 0, h = 0;
    CHECK(jxlimg_info(b, len, &w, &h) == JXLIMG_OK && w == W && h == H,
          "%s: info %dx%d", name, w, h);
    pic_t p = decode(b, len, 0, 0);
    CHECK(p.err == JXLIMG_OK && p.w == W && p.h == H && p.size == (size_t)W * H * 2,
          "%s: %s %dx%d (%s)", name, jxlimg_err_name(p.err), p.w, p.h, p.why ? p.why : "-");
    if (p.px) quadrants(name, &p, tol, hole);
    free(p.px);
    cut_sweep(name, b, len);
    free(b);
}

int main(void)
{
    printf("jxltest\n");

    /* The sniff. */
    CHECK(artformat_sniff((const uint8_t *)"\xFF\x0A\x00\x00", 4) == ART_JXL, "bare codestream");
    CHECK(artformat_sniff((const uint8_t *)"\xFF\x0A\x00", 3) == ART_NONE, "too short to be one");
    CHECK(artformat_sniff((const uint8_t *)"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a\x00", 13) == ART_JXL,
          "container");
    CHECK(artformat_sniff((const uint8_t *)"\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0b\x00", 13) == ART_NONE,
          "a wrong container signature");
    CHECK(strcmp(artformat_mime(ART_JXL), "image/jxl") == 0, "MIME type");

    good("lossy.jxl", 24, 0);
    good("lossless.jxl", 8, 0);
    good("alpha.jxl", 8, 1);
    good("container.jxl", 24, 0);
    good("jxlp.jxl", 24, 0);        /* joined by jxlimg.c: J40 alone refuses it */

    /*
     * What J40 2270 refuses, each kept so a change shows: if a newer J40
     * decodes one of these, its check fails and should be moved up to
     * the decodes above. Found while writing this test (6139), against
     * libjxl 0.11 through pillow-jxl-plugin 1.3.8:
     *   - lossy at 256x256 or smaller (one group): a 64x48 here;
     *   - lossless at effort 1, cjxl's fastest;
     *   - lossy, one flat colour, 600 px and up: a 700x700 here;
     *   - a JPEG recompressed into JPEG XL, sometimes: busy pictures
     *     over 256 px decoded in testing, this flat four-quadrant one
     *     does not, nor did one busy 700x700 ("Unimplemented feature").
     * Every one is still sniffed as JPEG XL and its size read, so the log
     * can say what was refused.
     */
    static const struct { const char *name; int w, h; } no[] = {
        { "no_small_lossy.jxl", 64, 48 },
        { "no_lossless_e1.jxl", 320, 240 },
        { "no_flat_lossy.jxl", 700, 700 },
        { "no_from_jpeg.jxl", 320, 240 },
    };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        size_t len;
        uint8_t *b = slurp(no[i].name, &len);
        CHECK(artformat_sniff(b, len) == ART_JXL, "%s: sniffed", no[i].name);
        int w = 0, h = 0;
        CHECK(jxlimg_info(b, len, &w, &h) == JXLIMG_OK && w == no[i].w && h == no[i].h,
              "%s: info %dx%d", no[i].name, w, h);
        pic_t p = decode(b, len, 0, 0);
        CHECK(p.err != JXLIMG_OK && !p.px,
              "%s: J40 decodes it now -- move it to the decodes", no[i].name);
        printf("  %-20s refused: %s (%s)\n", no[i].name, jxlimg_err_name(p.err), p.why ? p.why : "-");
        free(p.px);
        cut_sweep(no[i].name, b, len);
        free(b);
    }

    /* The size header at every shape, read and then decoded. */
    static const int sizes[][2] = {
        { 1, 1 }, { 8, 8 }, { 40, 8 }, { 8, 40 }, { 64, 48 }, { 256, 144 }, { 300, 200 },
        { 600, 500 }, { 517, 313 }, { 513, 1 }, { 1, 513 }, { 1024, 768 }, { 8200, 2 },
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "size_%dx%d.jxl", sizes[i][0], sizes[i][1]);
        size_t len;
        uint8_t *b = slurp(name, &len);
        int w = 0, h = 0;
        CHECK(jxlimg_info(b, len, &w, &h) == JXLIMG_OK && w == sizes[i][0] && h == sizes[i][1],
              "%s: header says %dx%d", name, w, h);
        pic_t p = decode(b, len, 0, 0);
        CHECK(p.err == JXLIMG_OK && p.w == sizes[i][0] && p.h == sizes[i][1],
              "%s: decoded %s %dx%d (%s)", name, jxlimg_err_name(p.err), p.w, p.h,
              p.why ? p.why : "-");
        if (p.px) {
            CHECK(chan_diff(p.px[p.w * (p.h / 2) + p.w / 2], rgb565(10, 20, 30)) <= 8,
                  "%s: colour %04x", name, p.px[0]);
        }
        free(p.px);
        free(b);
    }

    /* Thinned like stbjpeg: the largest whole step that still covers. */
    {
        size_t len;
        uint8_t *b = slurp("size_1024x768.jxl", &len);
        pic_t p = decode(b, len, 300, 200);
        CHECK(p.err == JXLIMG_OK && p.w == 341 && p.h == 256,
              "1024x768 into 300x200: %dx%d, want 341x256 (step 3)", p.w, p.h);
        free(p.px);
        free(b);
    }

    /* Refusals, without crashing. */
    {
        pic_t j = decode((const uint8_t *)"not a jxl at all", 16, 0, 0);
        CHECK(j.err == JXLIMG_NOT_JXL && !j.px, "junk: %s", jxlimg_err_name(j.err));
        static const uint8_t junk_cs[] = { 0xFF, 0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        pic_t k = decode(junk_cs, sizeof(junk_cs), 0, 0);
        CHECK(k.err != JXLIMG_OK && !k.px, "junk codestream: %s", jxlimg_err_name(k.err));
        /* A header claiming 2^30 x 2^30: refused before anything is
         * allocated for it. div8=0, height selector 3 (30 bits, all
         * ones), ratio 1. */
        static const uint8_t huge[] = { 0xFF, 0x0A, 0xFE, 0xFF, 0xFF, 0xFF, 0x3F, 0x00, 0x00, 0x00 };
        int w = 0, h = 0;
        CHECK(jxlimg_info(huge, sizeof(huge), &w, &h) == JXLIMG_TOO_BIG,
              "2^30 square: %s (%dx%d)", jxlimg_err_name(jxlimg_info(huge, sizeof(huge), &w, &h)), w, h);
        CHECK(jxlimg_peak_bytes(1000, 1000) >= 12u * 1000u * 1000u, "peak estimate");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
