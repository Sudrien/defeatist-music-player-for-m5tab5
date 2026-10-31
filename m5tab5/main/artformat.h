/*
 * artformat.h -- what kind of picture a cover is, from its bytes.
 *
 * The pure half of albumart.c, so it can be tested on the host
 * (texttest/arttest.c) and so every caller asks the same question the
 * same way. Before 6138 two of them answered "is it a PNG" with
 * `img[0] != 0xFF`, which was true only while JPEG and PNG were the
 * only two answers.
 *
 * Magic bytes decide, never a MIME type or a file extension: a tag's
 * MIME field and a file's name are both written by whoever wrote the
 * file, and are wrong often enough that the picture's own first bytes
 * are the only thing worth believing.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    ART_NONE = 0,       /* not a picture this player decodes */
    ART_JPEG,
    ART_PNG,
    ART_JXL,            /* 6139: JPEG XL, bare codestream or ISOBMFF container */
    ART_GIF,            /* 6145: GIF87a or GIF89a; the first frame is shown */
} art_format_t;

/* JPEG XL's two signatures: the bare codestream, and the container's
 * first box ("JXL " with its fixed 12-byte header). */
#define ARTFORMAT_JXL_BOX "\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a"

static inline art_format_t artformat_sniff(const uint8_t *p, size_t len)
{
    if (!p) return ART_NONE;
    if (len > 3 && p[0] == 0xFF && p[1] == 0xD8) return ART_JPEG;
    if (len > 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return ART_PNG;
    if (len > 3 && p[0] == 0xFF && p[1] == 0x0A) return ART_JXL;
    if (len > 12 && memcmp(p, ARTFORMAT_JXL_BOX, 12) == 0) return ART_JXL;
    if (len > 13 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) return ART_GIF;
    return ART_NONE;
}

/* The MIME type for a picture, for the web remote. */
static inline const char *artformat_mime(art_format_t f)
{
    switch (f) {
    case ART_JPEG: return "image/jpeg";
    case ART_PNG:  return "image/png";
    case ART_JXL:  return "image/jxl";
    case ART_GIF:  return "image/gif";
    default:       return "application/octet-stream";
    }
}

/*
 * A JPEG's SOF marker and size, by walking the markers to the first SOF
 * (see albumart_jpeg_is_baseline() in albumart.h for why it is asked).
 * True for SOF0 and SOF1, the two the hardware and TJpgDec take.
 * `*sof_out` is the marker found, 0 for none; the size comes from its
 * payload even when the answer is false.
 *
 * Markers are two bytes, 0xFF then a code, and every one except the
 * standalone few carries a big-endian length that includes itself. SOS
 * ends the header, so the SOF has to appear before it or there is not
 * one.
 */
static inline bool artformat_jpeg_sof(const uint8_t *p, size_t len, uint8_t *sof_out,
                                      uint32_t *w_out, uint32_t *h_out)
{
    *sof_out = 0;
    *w_out = 0;
    *h_out = 0;
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;   /* no SOI */

    size_t i = 2;
    while (i + 3 < len) {
        if (p[i] != 0xFF) { i++; continue; }             /* fill byte or junk */
        const uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }                /* fill */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xDA) return false;                     /* SOS, no SOF seen */

        /* SOFn: C0-CF except C4 (DHT), C8 (JPG), CC (DAC). C0 is
         * baseline, C1 is extended sequential and the hardware takes it
         * too; C2 is progressive, which it does not. */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *sof_out = m;
            /* Every SOF has the same first five payload bytes: precision,
             * then height and width, both big-endian 16-bit. */
            if (i + 9 < len) {
                *h_out = ((uint32_t)p[i + 5] << 8) | p[i + 6];
                *w_out = ((uint32_t)p[i + 7] << 8) | p[i + 8];
            }
            return (m == 0xC0 || m == 0xC1);
        }

        const size_t seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (seg < 2) return false;
        i += 2 + seg;
    }
    return false;
}
