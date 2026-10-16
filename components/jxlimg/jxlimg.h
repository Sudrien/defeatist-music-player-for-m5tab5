/*
 * jxlimg.h -- JPEG XL cover art, through J40.
 *
 * 6139. J40 is the only JPEG XL decoder that fits in this firmware: one
 * C file, about 90 KB of code, no dependencies. libjxl is C++ and well
 * over a megabyte. J40 has limits, so the player has them too. As of
 * its version 2270 (2022-09), which is pinned here, it decodes:
 *
 *   - lossy over 256 px with some detail in it, lossless at effort 3
 *     and up, and many JPEGs recompressed into JPEG XL;
 *
 * and refuses -- found by testing against libjxl 0.11, and pinned by
 * texttest/jxltest.c:
 *
 *   - lossy at 256x256 or smaller; lossless at effort 1; lossy of one
 *     flat colour at 600 px and up; some recompressed JPEGs. These are
 *     "Decoding failed" (JXLIMG_FAILED), with J40's four-letter reason;
 *   - patches, splines, noise, upsampling, progressive passes,
 *     animation: "Unimplemented feature" (JXLIMG_UNSUPPORTED).
 *
 * Refused is a log line naming the reason. The player then carries on
 * without that picture, as it does for a progressive JPEG it cannot
 * afford. texttest/jxltest.c holds a fixture of each kind and checks
 * which side of the line it falls on.
 *
 * Like stb_image here, J40 decodes the whole picture at full size into
 * PSRAM and is thinned to the screen box afterwards. Its working set is
 * float planes: about 13 bytes a pixel at its peak, so a 1000x1000
 * cover is about 13 MB for a moment. Ask jxlimg_peak_bytes() first.
 * J40 is also deep: about 11.5 KB of stack on a 64-bit host, so it is
 * run on a task of its own with a PSRAM stack (albumart.c).
 *
 * J40 is Kang Seonghoon's, public domain or MIT No Attribution, pinned
 * in cmake/vendored.cmake. This wrapper is MIT.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Four megapixels, like stbjpeg: the arithmetic stays small and the
 * absurd is refused. The real bound is the free-PSRAM check against
 * jxlimg_peak_bytes(). */
#define JXLIMG_MAX_PIXELS  (4u * 1000u * 1000u)

typedef enum {
    JXLIMG_OK = 0,
    JXLIMG_NOT_JXL = -1,        /* no JPEG XL size header to read */
    JXLIMG_TOO_BIG = -2,        /* over JXLIMG_MAX_PIXELS */
    JXLIMG_NO_MEM = -3,
    JXLIMG_FAILED = -4,         /* J40 refused the data */
    JXLIMG_UNSUPPORTED = -5,    /* a JPEG XL feature J40 does not implement */
} jxlimg_err_t;

/* Width and height from the size header, without decoding. */
jxlimg_err_t jxlimg_info(const uint8_t *in, size_t len, int *w, int *h);

/* What a decode of a w x h picture may hold at once, in bytes. */
size_t jxlimg_peak_bytes(int w, int h);

/*
 * Decode to native little-endian RGB565 (gfx.h's RGB()), thinned by
 * whole steps to the smallest size that still covers box_w x box_h, as
 * stbjpeg_decode_rgb565() does. Alpha is blended over `bg`, an RGB565
 * colour, since the screen behind the cover is a flat one.
 * *out is malloc'd (PSRAM on the board) and belongs to the caller; its
 * stride is *w_out pixels.
 *
 * `why` is J40's own message on failure, for the log.
 */
jxlimg_err_t jxlimg_decode_rgb565(const uint8_t *in, size_t len,
                                  int box_w, int box_h, uint16_t bg,
                                  uint16_t **out, size_t *out_size,
                                  int *w_out, int *h_out,
                                  const char **why);

const char *jxlimg_err_name(jxlimg_err_t e);

#ifdef __cplusplus
}
#endif
