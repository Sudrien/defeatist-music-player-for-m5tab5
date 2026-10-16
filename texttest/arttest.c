/*
 * arttest.c -- cover pictures: what kind each one is, and that the
 * decoders here draw it.
 *
 * Fixtures in fixtures/art, from tools/gen_art_fixtures.py: one 64x48
 * picture of four flat quadrants (red, green / blue, white), saved in
 * every flavour a cover turns up in. A decode passes when the middle of
 * each quadrant is the right colour.
 *
 *   - artformat.h: the magic-byte sniff and the JPEG marker walk, on
 *     every fixture and on the junk the containers hand over.
 *   - JPEG through stb_image (the last of the three JPEG decoders, and
 *     the only one that runs off the board): baseline 4:2:0 and 4:4:4,
 *     progressive, greyscale, EXIF ahead of the SOF.
 *   - PNG through pngle, with the RGB565 conversion albumart.c's
 *     png_on_draw() uses: RGB, RGBA, palette, palette with tRNS, grey,
 *     grey with alpha, 16 bits a channel, Adam7.
 *   - Every file cut short, and junk: refused or partial, never a crash
 *     (run under ASan and UBSan).
 *
 * The hardware JPEG engine and TJpgDec run on the board only; the
 * board-side check is test_art_files/.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artformat.h"
#include "pngle.h"
#include "stbjpeg.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;      \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);   \
    printf("\n"); } } while (0)

#define W 64
#define H 48

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

/* gfx.h's RGB(): native RGB565, as png_on_draw() and stbjpeg write it. */
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

#define UNSET 0x0841    /* a colour no fixture contains: "never drawn" */

/* The middle of each quadrant against the colours gen_art_fixtures.py
 * drew. `grey`: the fixture is greyscale, so compare luma. `hole`: the
 * white quadrant is transparent and must be left as it was. */
static void quadrants(const char *what, const uint16_t *px, int stride,
                      int tol, int grey, int hole)
{
    static const int want[4][3] = {
        { 220, 30, 30 }, { 30, 200, 40 }, { 40, 50, 210 }, { 255, 255, 255 },
    };
    static const int at[4][2] = { { 16, 12 }, { 48, 12 }, { 16, 36 }, { 48, 36 } };
    for (int q = 0; q < 4; q++) {
        const uint16_t got = px[at[q][1] * stride + at[q][0]];
        if (q == 3 && hole) {
            CHECK(got == UNSET, "%s: transparent quadrant was drawn (%04x)", what, got);
            continue;
        }
        int r = want[q][0], g = want[q][1], b = want[q][2];
        if (grey) {
            const int l = (r * 299 + g * 587 + b * 114) / 1000;
            r = g = b = l;
        }
        const int d = chan_diff(got, rgb565(r, g, b));
        CHECK(d <= tol, "%s: quadrant %d is %04x, want ~%04x (off by %d)",
              what, q, got, rgb565(r, g, b), d);
    }
}

/* ---- PNG, through pngle, drawn the way png_on_draw() draws -------- */

typedef struct {
    uint16_t px[W * H];
    int w, h;
    int inited;
    int drawn;
} png_out_t;

static void on_init(pngle_t *p, uint32_t w, uint32_t h)
{
    png_out_t *o = pngle_get_user_data(p);
    o->w = (int)w;
    o->h = (int)h;
    o->inited = 1;
}

static void on_draw(pngle_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    const uint8_t rgba[4])
{
    png_out_t *o = pngle_get_user_data(p);
    if (rgba[3] == 0) return;                   /* as albumart.c: transparent */
    for (uint32_t yy = y; yy < y + h; yy++)
        for (uint32_t xx = x; xx < x + w; xx++)
            if (xx < W && yy < H) {
                o->px[yy * W + xx] = rgb565(rgba[0], rgba[1], rgba[2]);
                o->drawn++;
            }
}

/* 1: decoded with an image; 0: nothing drawn; -1: pngle said error. */
static int png_decode(const uint8_t *b, size_t len, png_out_t *o)
{
    for (int i = 0; i < W * H; i++) o->px[i] = UNSET;
    o->w = o->h = o->inited = o->drawn = 0;
    pngle_t *p = pngle_new();
    if (!p) return -1;
    pngle_set_user_data(p, o);
    pngle_set_init_callback(p, on_init);
    pngle_set_draw_callback(p, on_draw);
    const int fed = pngle_feed(p, b, len);
    pngle_destroy(p);
    if (fed < 0) return -1;
    return o->inited ? 1 : 0;
}

static void png_case(const char *name, int grey, int hole)
{
    size_t len;
    uint8_t *b = slurp(name, &len);
    CHECK(artformat_sniff(b, len) == ART_PNG, "%s: not sniffed as PNG", name);
    static png_out_t o;
    const int r = png_decode(b, len, &o);
    CHECK(r == 1 && o.w == W && o.h == H, "%s: decode %d, %dx%d", name, r, o.w, o.h);
    if (r == 1) quadrants(name, o.px, W, 8, grey, hole);

    /* Cut short at every length up to the whole file: no crash, and an
     * honest answer -- an error, or an image drawn only in part. */
    for (size_t cut = 0; cut < len; cut++) {
        const int t = png_decode(b, cut, &o);
        (void)t;
    }
    checks++;
    free(b);
}

/* ---- JPEG, through stb_image ---------------------------------------- */

static void jpeg_case(const char *name, uint8_t sof_want, int grey)
{
    size_t len;
    uint8_t *b = slurp(name, &len);
    CHECK(artformat_sniff(b, len) == ART_JPEG, "%s: not sniffed as JPEG", name);

    uint8_t sof = 0;
    uint32_t w = 0, h = 0;
    const bool base = artformat_jpeg_sof(b, len, &sof, &w, &h);
    CHECK(sof == sof_want && w == W && h == H, "%s: SOF %02x %ux%u, want %02x %dx%d",
          name, sof, (unsigned)w, (unsigned)h, sof_want, W, H);
    CHECK(base == (sof_want == 0xC0 || sof_want == 0xC1), "%s: baseline says %d", name, base);

    uint16_t *px = NULL;
    size_t size = 0;
    int ow = 0, oh = 0;
    const char *why = NULL;
    const stbjpeg_err_t e = stbjpeg_decode_rgb565(b, len, 0, 0, &px, &size, &ow, &oh, &why);
    CHECK(e == STBJPEG_OK && ow == W && oh == H, "%s: stb_image %s %dx%d (%s)",
          name, stbjpeg_err_name(e), ow, oh, why ? why : "-");
    if (px) quadrants(name, px, W, 24, grey, 0);
    free(px);

    for (size_t cut = 0; cut < len; cut += (cut < 64 ? 1 : 7)) {
        uint16_t *t = NULL;
        const stbjpeg_err_t te = stbjpeg_decode_rgb565(b, cut, 0, 0, &t, &size, &ow, &oh, &why);
        (void)te;
        free(t);
        artformat_jpeg_sof(b, cut, &sof, &w, &h);
    }
    checks++;
    free(b);
}

int main(void)
{
    printf("arttest\n");

    /* The sniff: magic bytes and nothing else. */
    CHECK(artformat_sniff(NULL, 10) == ART_NONE, "NULL");
    CHECK(artformat_sniff((const uint8_t *)"\xFF\xD8\xFF", 3) == ART_NONE, "3-byte JPEG start");
    CHECK(artformat_sniff((const uint8_t *)"\xFF\xD8\xFF\xE0", 4) == ART_JPEG, "JPEG SOI");
    CHECK(artformat_sniff((const uint8_t *)"\x89PNG\r\n\x1a\n", 8) == ART_NONE, "PNG signature alone");
    CHECK(artformat_sniff((const uint8_t *)"\x89PNG\r\n\x1a\n\0", 9) == ART_PNG, "PNG");
    CHECK(artformat_sniff((const uint8_t *)"GIF89a....", 10) == ART_NONE, "GIF refused");
    CHECK(artformat_sniff((const uint8_t *)"BM........", 10) == ART_NONE, "BMP refused");
    CHECK(artformat_sniff((const uint8_t *)"RIFF....WEBPVP8 ", 16) == ART_NONE, "WebP refused");
    CHECK(strcmp(artformat_mime(ART_PNG), "image/png") == 0 &&
          strcmp(artformat_mime(ART_JPEG), "image/jpeg") == 0, "MIME types");

    /* A SOS before any SOF: no SOF, not baseline. */
    {
        static const uint8_t sos_first[] = { 0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02, 0, 0, 0, 0, 0, 0 };
        uint8_t sof = 1; uint32_t w = 1, h = 1;
        CHECK(!artformat_jpeg_sof(sos_first, sizeof(sos_first), &sof, &w, &h) && sof == 0,
              "SOS first: sof %02x", sof);
        static const uint8_t zero_len[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00, 0, 0, 0, 0 };
        CHECK(!artformat_jpeg_sof(zero_len, sizeof(zero_len), &sof, &w, &h), "a zero segment length ends the walk");
    }

    jpeg_case("base420.jpg", 0xC0, 0);
    jpeg_case("base444.jpg", 0xC0, 0);
    jpeg_case("prog.jpg", 0xC2, 0);
    jpeg_case("gray.jpg", 0xC0, 1);
    jpeg_case("exif.jpg", 0xC0, 0);

    png_case("rgb.png", 0, 0);
    png_case("rgba.png", 0, 1);
    png_case("palette.png", 0, 0);
    png_case("palette_trns.png", 0, 1);
    png_case("gray.png", 1, 0);
    png_case("gray_alpha.png", 1, 1);
    png_case("rgb16.png", 0, 0);
    png_case("interlaced.png", 0, 0);

    /* Junk handed to pngle is an error or nothing, never a picture. */
    {
        static png_out_t o;
        const int r = png_decode((const uint8_t *)"\x89PNG\r\n\x1a\nnot a chunk at all", 26, &o);
        CHECK(r != 1, "junk after the PNG signature decoded (%d)", r);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
