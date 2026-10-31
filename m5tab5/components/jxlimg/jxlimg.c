/*
 * jxlimg.c -- see jxlimg.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jxlimg.h"

#include <stdlib.h>
#include <string.h>

#include <stdbool.h>

#define JXLIMG_SIG "\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a"

/*
 * Every block J40 allocates is on one list, so whatever it has not freed
 * when the decode is over can be. J40 2270 leaks on several of its error
 * paths -- a codestream that ends inside an entropy code, a frame header
 * or a modular tree (jxltest's truncation sweep finds them, a few hundred
 * bytes over thousands of cut files). Upstream's code is fetched
 * unmodified (cmake/vendored.cmake), so the leak is closed here instead:
 * a damaged cover read every time a track comes round would otherwise
 * leak every time.
 *
 * A 16-byte header before each block keeps the allocator's alignment.
 * One decode at a time -- albumart.c runs J40 on one task, and texttest
 * on one thread -- so the list needs no lock.
 */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* PSRAM, all of it: J40's planes are megabytes, and internal RAM is the
 * radio's (CLAUDE.md). */
#define RAW_MALLOC(n)     heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RAW_FREE(p)       heap_caps_free(p)
#else
#define RAW_MALLOC(n)     malloc(n)
#define RAW_FREE(p)       free(p)
#endif
#define OUT_MALLOC(n)     RAW_MALLOC(n)

typedef struct blk {
    struct blk *prev, *next;
    size_t size;
    size_t pad;                 /* to 16 bytes on a 32-bit target */
} blk_t;

static blk_t s_live = { &s_live, &s_live, 0, 0 };
static size_t s_live_n;

static void *jxl_malloc(size_t n)
{
    if (n > SIZE_MAX - sizeof(blk_t)) return NULL;
    blk_t *b = RAW_MALLOC(sizeof(blk_t) + n);
    if (!b) return NULL;
    b->size = n;
    b->next = s_live.next;
    b->prev = &s_live;
    s_live.next->prev = b;
    s_live.next = b;
    s_live_n++;
    return b + 1;
}

static void jxl_free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    b->prev->next = b->next;
    b->next->prev = b->prev;
    s_live_n--;
    RAW_FREE(b);
}

static void *jxl_calloc(size_t n, size_t sz)
{
    if (sz && n > SIZE_MAX / sz) return NULL;
    void *p = jxl_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

static void *jxl_realloc(void *p, size_t n)
{
    if (!p) return jxl_malloc(n);
    if (!n) { jxl_free(p); return NULL; }
    const size_t old = ((blk_t *)p - 1)->size;
    void *q = jxl_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    jxl_free(p);
    return q;
}

/* Whatever J40 left behind. Returns how many blocks that was. */
static size_t jxl_sweep(void)
{
    size_t n = 0;
    while (s_live.next != &s_live) {
        jxl_free(s_live.next + 1);
        n++;
    }
    return n;
}

#define J40_MALLOC   jxl_malloc
#define J40_CALLOC   jxl_calloc
#define J40_REALLOC  jxl_realloc
#define J40_FREE     jxl_free

/*
 * Every allocation through J40_MALLOC and J40_FREE, and so into PSRAM.
 * Left alone, J40 takes its pixel planes from posix_memalign() wherever
 * _POSIX_C_SOURCE says it exists, which on the board is the default
 * heap -- and the default heap gives a block under 4 KB
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) internal RAM, the radio's.
 * Undefined here, after every header J40 includes has been included, J40
 * falls back to its own alignment over J40_MALLOC.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#undef _ISOC11_SOURCE
/* And its two direct free() calls (an error path in the permutation
 * reader, and lf_groups at the end), whose pointers came from J40_MALLOC. */
#define free(p) jxl_free(p)

#define J40_CONFIRM_THAT_THIS_IS_EXPERIMENTAL_AND_POTENTIALLY_UNSAFE
#define J40_IMPLEMENTATION
#include "j40.h"
#undef free

/* ------------------------------------------------------------------ */
/* 6139: a JPEG XL picture's size, without decoding it                 */
/* ------------------------------------------------------------------ */

/* Bits are read least significant first, as the codestream stores them. */
typedef struct { const uint8_t *p; size_t len, bit; bool over; } jxl_bits_t;

static uint32_t jxl_bits(jxl_bits_t *b, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, b->bit++) {
        if (b->bit / 8 >= b->len) { b->over = true; return 0; }
        v |= (uint32_t)((b->p[b->bit / 8] >> (b->bit % 8)) & 1u) << i;
    }
    return v;
}

/* U32(Bits(9), Bits(13), Bits(18), Bits(30)): a 2-bit selector, then
 * that many bits. The form SizeHeader uses for both dimensions. */
static uint32_t jxl_dim(jxl_bits_t *b)
{
    static const unsigned nbits[4] = { 9, 13, 18, 30 };
    return jxl_bits(b, nbits[jxl_bits(b, 2)]) + 1u;
}

/* Where the codestream starts: the bytes themselves for a bare one; in
 * a container, the payload of the first `jxlc` box, or of the first
 * `jxlp` box past its 4-byte index. NULL if there is none in `len`. */
static const uint8_t *jxlimg_codestream(const uint8_t *p, size_t len, size_t *cs_len)
{
    if (len >= 2 && p[0] == 0xFF && p[1] == 0x0A) { *cs_len = len; return p; }
    if (len < 12 || memcmp(p, JXLIMG_SIG, 12) != 0) return NULL;
    size_t i = 12;
    while (i + 8 <= len) {
        uint64_t size = ((uint64_t)p[i] << 24) | ((uint64_t)p[i + 1] << 16) |
                        ((uint64_t)p[i + 2] << 8) | p[i + 3];
        size_t head = 8;
        if (size == 1) {                        /* 64-bit size follows the type */
            if (i + 16 > len) return NULL;
            size = 0;
            for (int k = 0; k < 8; k++) size = (size << 8) | p[i + 8 + k];
            head = 16;
        } else if (size == 0) {                 /* to the end of the file */
            size = len - i;
        }
        if (size < head) return NULL;
        const uint8_t *type = p + i + 4;
        if (memcmp(type, "jxlc", 4) == 0 || memcmp(type, "jxlp", 4) == 0) {
            const size_t skip = head + (type[3] == 'p' ? 4 : 0);
            if (i + skip > len) return NULL;
            const uint64_t room = (uint64_t)(len - i - skip);
            const uint64_t want = size - skip;
            *cs_len = (size_t)(want < room ? want : room);
            return p + i + skip;
        }
        if (size > (uint64_t)(len - i)) return NULL;
        i += (size_t)size;
    }
    return NULL;
}

/*
 * The picture's width and height from the codestream's SizeHeader
 * (ISO/IEC 18181-1, 9.2.1), which is the first thing after its 0xFF0A:
 *
 *   div8 = Bool
 *   height = div8 ? 8 * (1 + u(5)) : U32(9, 13, 18, 30 bits) + 1
 *   ratio = u(3)
 *   width = ratio ? height * ratio : (div8 ? 8 * (1 + u(5)) : U32 + 1)
 *
 * with the seven fixed ratios 1, 12/10, 4/3, 3/2, 16/9, 5/4 and 2. False
 * for anything that is not a JPEG XL or runs out before the size does.
 */
static bool jxl_size(const uint8_t *p, size_t len, uint32_t *w, uint32_t *h)
{
    *w = *h = 0;
    size_t cs_len = 0;
    const uint8_t *cs = jxlimg_codestream(p, len, &cs_len);
    if (!cs || cs_len < 3 || cs[0] != 0xFF || cs[1] != 0x0A) return false;

    jxl_bits_t b = { cs + 2, cs_len - 2, 0, false };
    const bool div8 = jxl_bits(&b, 1) != 0;
    const uint32_t height = div8 ? 8u * (1u + jxl_bits(&b, 5)) : jxl_dim(&b);
    const uint32_t ratio = jxl_bits(&b, 3);
    uint32_t width;
    if (ratio == 0) {
        width = div8 ? 8u * (1u + jxl_bits(&b, 5)) : jxl_dim(&b);
    } else {
        static const uint32_t num[8] = { 0, 1, 12, 4, 3, 16, 5, 2 };
        static const uint32_t den[8] = { 0, 1, 10, 3, 2, 9, 4, 1 };
        width = (uint32_t)(((uint64_t)height * num[ratio]) / den[ratio]);
    }
    if (b.over || width == 0 || height == 0) return false;
    *w = width;
    *h = height;
    return true;
}

/*
 * J40 reads a codestream in one `jxlc` box and not one split over
 * `jxlp` boxes, which libjxl writes whenever metadata is interleaved
 * with the picture -- every JPEG it recompresses, for one. So a split
 * codestream is joined here, into a bare one J40 can read. NULL if
 * there is nothing to join (no `jxlp`), or no memory.
 */
static uint8_t *join_jxlp(const uint8_t *p, size_t len, size_t *out_len)
{
    if (len < 12 || memcmp(p, JXLIMG_SIG, 12) != 0) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        size_t total = 0, i = 12;
        uint8_t *buf = pass ? RAW_MALLOC(*out_len) : NULL;
        if (pass && !buf) return NULL;
        while (i + 8 <= len) {
            uint64_t size = ((uint64_t)p[i] << 24) | ((uint64_t)p[i + 1] << 16) |
                            ((uint64_t)p[i + 2] << 8) | p[i + 3];
            size_t head = 8;
            if (size == 1) {
                if (i + 16 > len) break;
                size = 0;
                for (int k = 0; k < 8; k++) size = (size << 8) | p[i + 8 + k];
                head = 16;
            } else if (size == 0) {
                size = len - i;
            }
            if (size < head || size > (uint64_t)(len - i)) break;
            if (memcmp(p + i + 4, "jxlp", 4) == 0 && size >= head + 4) {
                const size_t n = (size_t)size - head - 4;
                if (pass) memcpy(buf + total, p + i + head + 4, n);
                total += n;
            }
            i += (size_t)size;
        }
        if (!pass) {
            if (total == 0) return NULL;
            *out_len = total;
        } else {
            *out_len = total;
            return buf;
        }
    }
    return NULL;
}

jxlimg_err_t jxlimg_info(const uint8_t *in, size_t len, int *w, int *h)
{
    *w = *h = 0;
    uint32_t uw = 0, uh = 0;
    if (!in || !jxl_size(in, len, &uw, &uh)) return JXLIMG_NOT_JXL;
    if ((uint64_t)uw * uh > JXLIMG_MAX_PIXELS) {
        *w = uw > 0x7fffffffu ? 0x7fffffff : (int)uw;
        *h = uh > 0x7fffffffu ? 0x7fffffff : (int)uh;
        return JXLIMG_TOO_BIG;
    }
    *w = (int)uw;
    *h = (int)uh;
    return JXLIMG_OK;
}

size_t jxlimg_peak_bytes(int w, int h)
{
    /* Measured on the host with J40 2270 (6139): 4.3 MB at 500x500, 6.7 MB
     * at 700x700, 12.8 MB at 1000x1000, lossy -- the float planes, the
     * RGBA output and a fixed few MB of tables. Lossless is far less. */
    return (size_t)w * (size_t)h * 13u + 2u * 1024u * 1024u;
}

static jxlimg_err_t classify(const char *msg)
{
    if (!msg) return JXLIMG_FAILED;
    if (strncmp(msg, "Out of memory", 13) == 0) return JXLIMG_NO_MEM;
    if (strncmp(msg, "Unimplemented feature", 21) == 0) return JXLIMG_UNSUPPORTED;
    return JXLIMG_FAILED;
}

static inline uint16_t blend565(int r, int g, int b, int a, uint16_t bg)
{
    if (a < 255) {
        const int br = ((bg >> 11) & 31) << 3, bgg = ((bg >> 5) & 63) << 2, bb = (bg & 31) << 3;
        r = (r * a + br * (255 - a)) / 255;
        g = (g * a + bgg * (255 - a)) / 255;
        b = (b * a + bb * (255 - a)) / 255;
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

jxlimg_err_t jxlimg_decode_rgb565(const uint8_t *in, size_t len,
                                  int box_w, int box_h, uint16_t bg,
                                  uint16_t **out, size_t *out_size,
                                  int *w_out, int *h_out,
                                  const char **why)
{
    *out = NULL;
    *out_size = 0;
    *w_out = *h_out = 0;
    if (why) *why = NULL;

    int hw, hh;
    const jxlimg_err_t info = jxlimg_info(in, len, &hw, &hh);
    if (info != JXLIMG_OK) return info;

    size_t joined_len = 0;
    uint8_t *joined = join_jxlp(in, len, &joined_len);
    if (joined) {
        in = joined;
        len = joined_len;
    }

    j40_image image;
    /* J40 does not write to the buffer; with no free function it does
     * not free it either. */
    j40_from_memory(&image, (void *)(uintptr_t)in, len, NULL);
    j40_output_format(&image, J40_RGBA, J40_U8X4);

    jxlimg_err_t ret = JXLIMG_FAILED;
    if (j40_next_frame(&image)) {
        j40_frame frame = j40_current_frame(&image);
        j40_pixels_u8x4 px = j40_frame_pixels_u8x4(&frame, J40_RGBA);
        const int gw = px.width, gh = px.height;
        if (gw > 0 && gh > 0 && !j40_error(&image)) {
            int step = 1;
            if (box_w > 0 && box_h > 0) {
                while (gw / (step + 1) >= box_w && gh / (step + 1) >= box_h) step++;
            }
            const int ow = gw / step, oh = gh / step;
            const size_t need = (size_t)ow * (size_t)oh * 2u;
            uint16_t *dst = OUT_MALLOC(need);
            if (!dst) {
                ret = JXLIMG_NO_MEM;
            } else {
                for (int y = 0; y < oh; y++) {
                    const j40_u8x4 *row = j40_row_u8x4(px, y * step);
                    uint16_t *d = dst + (size_t)y * ow;
                    for (int x = 0; x < ow; x++) {
                        const uint8_t *s = row[x * step];
                        d[x] = blend565(s[0], s[1], s[2], s[3], bg);
                    }
                }
                *out = dst;
                *out_size = need;
                *w_out = ow;
                *h_out = oh;
                ret = JXLIMG_OK;
            }
        }
    }
    if (ret != JXLIMG_OK && ret != JXLIMG_NO_MEM && j40_error(&image)) {
        /* Copied: the string belongs to the image, freed just below. */
        static char msg[96];
        strncpy(msg, j40_error_string(&image), sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
        if (why) *why = msg;
        ret = classify(msg);
    }
    j40_free(&image);
    jxl_sweep();                /* what J40 lost on an error path */
    if (joined) RAW_FREE(joined);
    return ret;
}

const char *jxlimg_err_name(jxlimg_err_t e)
{
    switch (e) {
    case JXLIMG_OK:          return "ok";
    case JXLIMG_NOT_JXL:     return "not a JPEG XL";
    case JXLIMG_TOO_BIG:     return "too many pixels";
    case JXLIMG_NO_MEM:      return "out of memory";
    case JXLIMG_FAILED:      return "decode failed";
    case JXLIMG_UNSUPPORTED: return "uses a JPEG XL feature J40 does not implement";
    }
    return "?";
}
