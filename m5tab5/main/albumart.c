/*
 * ID3v2 APIC extraction and display on the Tab5's ST7121 panel.
 *
 * Decoding is feckless_image_handler's (8069): img_decode() picks the
 * P4's hardware JPEG engine, TJpgDec, stb_image, pngle or J40 by format
 * and by what memory there is, and img_fit_draw() fits the result to the
 * box. What stays here is what is the player's: the ID3 walk, the one
 * retained frame, where the cover sits (artplace.h), the keystone lean,
 * and the blit to the panel through ui.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "feckless_image.h"
#include "feckless_image_decode.h"
#include "feckless_image_esp.h"
#include "feckless_image_fit.h"

#include "albumart.h"
#include "artplace.h"
#include "heapmap.h"
#include "storage_io.h"
#include "gfx.h"
#include "ui.h"      /* ui_blit_art(): the artwork is a band in portrait
                     * and a column in landscape, and only ui.c knows */

static const char *TAG = "tab5_art";

/*
 * MurmurHash2, 32-bit, public domain. Identity for a blob of bytes: two
 * covers that hash the same and are the same length are the same image
 * for every purpose this program has.
 *
 * No longer static: mediacache.c hashes on the store path so that two
 * entries holding the same picture can share one buffer, and it is
 * load-bearing that it is this function and not a second one. Declared
 * in albumart.h, which mediacache.h already includes.
 *
 * The 4-byte load goes through memcpy() rather than a cast. The input is
 * a DMA-reachable buffer whose alignment nothing here promises, and an
 * unaligned 32-bit load is a fault on some targets and a silent
 * slow path on others; the compiler turns this back into one load where
 * it is allowed to.
 */
uint32_t albumart_cover_hash(const void *key, size_t len)
{
    const uint32_t m = 0x5bd1e995u;
    const int r = 24;
    uint32_t h = 0x9747b28cu ^ (uint32_t)len;
    const unsigned char *d = (const unsigned char *)key;

    while (len >= 4) {
        uint32_t k;
        memcpy(&k, d, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        d += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= (uint32_t)d[2] << 16; /* fall through */
        case 2: h ^= (uint32_t)d[1] << 8;  /* fall through */
        case 1: h ^= (uint32_t)d[0]; h *= m; break;
        default: break;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* v2.4 frame sizes are syncsafe; v2.3 are plain big-endian. Getting
 * this backwards walks straight off the end of the first frame. */
/* 5266: an ID3v2.2 frame size -- 24 bits, plain. */
static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}

/*
 * Find the first APIC frame holding a JPEG and return a malloc'd copy of
 * the image bytes. Caller frees.
 *
 * APIC payload: encoding byte, MIME string (NUL-terminated latin1),
 * picture type byte, description (NUL-terminated, two NULs for the
 * UTF-16 encodings), then the image.
 */
/*
 * Is this something albumart_show() can decode?
 *
 * Exported because every container's picture block ends in the same
 * question, and each of them answering it separately is how one of them
 * ends up accepting a BMP and failing further downstream, where the
 * error message is about the decoder rather than about the file.
 */
bool albumart_is_supported_image(const uint8_t *p, size_t len)
{
    return img_sniff(p, len) != IMG_NONE;     /* 6138; 8069: feckless_image.h */
}

esp_err_t albumart_extract_at(FILE *f, storage_io_class_t cls, long base,
                              uint8_t **out, size_t *out_len)
{
    uint8_t hdr[10];

    *out = NULL;
    *out_len = 0;

    fseek(f, base, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const int ver = hdr[3];
    /* 5266: v2.2 is read -- its PIC frame is APIC with a 3-byte id, a
     * 3-byte size and a 3-letter image format where APIC has a MIME
     * type. Below that there is nothing: v2.0 and v2.1 were never
     * published. Its 0x40 is compression, not an extended header, and no
     * tagger wrote it; refused, and said. */
    const bool v22 = (ver == 2);
    if (ver < 2 || (v22 && (hdr[5] & 0x40))) {
        /*
         * Raised from LOGD because the refusal is invisible otherwise:
         * the player prints "no cover art in this file
         * (ESP_ERR_NOT_SUPPORTED)" and that code covers two unrelated
         * causes -- a picture in a format this device cannot draw
         * (warned about below, with its MIME type) and this, a tag this
         * reader does not know. Reading the first as the second sends
         * someone looking for a converter for a file that has nothing to
         * convert.
         */
        ESP_LOGI(TAG, "ID3v2.%d tag%s: no cover read", ver, v22 ? ", compressed" : "");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGD(TAG, "ID3v2.%d tag", ver);
    if (!v22 && (hdr[5] & 0x40)) {
        /* Extended header: skip it. Its own size field is syncsafe in
         * v2.4 and plain in v2.3, same trap as frame sizes. */
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return ESP_ERR_INVALID_SIZE;
        const uint32_t esz = (ver >= 4) ? syncsafe32(ext) : be32(ext);
        fseek(f, (long)esz - ((ver >= 4) ? 4 : 0), SEEK_CUR);
    }

    const long tag_end = base + 10 + (long)syncsafe32(&hdr[6]);
    const size_t fhl = v22 ? 6 : 10;                /* 5266 */

    while (ftell(f) + (long)fhl <= tag_end) {
        uint8_t fh[10];
        if (fread(fh, 1, fhl, f) != fhl) break;
        if (fh[0] == 0) break;                      /* padding */

        const uint32_t fsz = v22 ? be24(&fh[3])
                           : (ver >= 4) ? syncsafe32(&fh[4]) : be32(&fh[4]);
        if (fsz == 0 || ftell(f) + (long)fsz > tag_end) break;

        if (memcmp(fh, v22 ? "PIC" : "APIC", v22 ? 3 : 4) != 0) {
            fseek(f, (long)fsz, SEEK_CUR);
            continue;
        }

        uint8_t *frame = malloc(fsz);
        if (!frame) return ESP_ERR_NO_MEM;
        /* The APIC frame is the cover itself -- the one read in this file
         * big enough to matter, and the half-megabyte that used to sit in
         * front of the decoder's next refill. The ten-byte header reads
         * around it are left alone: a lease costs two semaphore
         * operations and those reads are shorter than that. */
        if (storage_io_fread(frame, fsz, f, cls) != fsz) {
            free(frame);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t i = 0;
        const uint8_t enc = frame[i++];
        const char *mime = (const char *)&frame[i];
        char fmt[4] = "";
        if (v22) {
            /* 5266: "JPG" or "PNG", three bytes and no NUL. For the log;
             * the image's own magic decides, as for APIC. */
            if (fsz < 5) { free(frame); return ESP_ERR_INVALID_SIZE; }
            memcpy(fmt, &frame[i], 3);
            mime = fmt;
            i += 3;
        } else {
            while (i < fsz && frame[i]) i++;
            i++;                                    /* MIME NUL */
        }
        if (i >= fsz) { free(frame); return ESP_ERR_INVALID_SIZE; }
        i++;                                        /* picture type */

        /* Description. Encodings 1 and 2 are UTF-16 and terminate with
         * two NUL bytes on an even boundary. */
        if (enc == 1 || enc == 2) {
            while (i + 1 < fsz && !(frame[i] == 0 && frame[i + 1] == 0)) i += 2;
            i += 2;
        } else {
            while (i < fsz && frame[i]) i++;
            i++;
        }
        if (i >= fsz) { free(frame); return ESP_ERR_INVALID_SIZE; }

        const size_t img_len = fsz - i;
        if (!albumart_is_supported_image(&frame[i], img_len)) {
            ESP_LOGW(TAG, "APIC is %s, which is not JPEG, PNG, JPEG XL or GIF", mime);
            free(frame);
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint8_t *img = malloc(img_len);
        if (!img) { free(frame); return ESP_ERR_NO_MEM; }
        memcpy(img, &frame[i], img_len);

        /* Logged before the free, not after. `mime` points into frame[],
         * so the old order read freed memory to print it -- which usually
         * still showed the right string, because nothing had reused the
         * block yet, and would have started printing something else the
         * moment anything did. */
        ESP_LOGI(TAG, "cover art: %s, %u bytes", mime, (unsigned)img_len);
        free(frame);

        *out = img;
        *out_len = img_len;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

/* An ID3v2 tag at the front of the file, which is where an MP3 keeps it. */
esp_err_t albumart_extract(FILE *f, storage_io_class_t cls,
                           uint8_t **out, size_t *out_len)
{
    return albumart_extract_at(f, cls, 0, out, out_len);
}

/*
 * Append one codepoint to a UTF-8 buffer, if it fits whole.
 *
 * "If it fits whole" is the point: a 3 byte character with 2 bytes of
 * room left has to be dropped rather than half-written, or the tag ends
 * in a truncated sequence and gfx.c draws a replacement box for it. The
 * old code could not have this bug because every character was one byte.
 *
 * Returns the number of bytes written, 0 if it did not fit.
 */
static size_t utf8_put(char *out, size_t o, size_t out_len, uint32_t cp)
{
    /* Anything past the BMP is outside the font's subset by a wide
     * margin -- emoji in a TIT2 frame, usually -- so it becomes U+FFFD
     * here rather than costing four bytes to render as a box anyway. */
    if (cp > 0xFFFF) cp = 0xFFFD;

    size_t n = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : 3;
    if (o + n + 1 > out_len) return 0;

    if (n == 1) {
        out[o] = (char)cp;
    } else if (n == 2) {
        out[o]     = (char)(0xC0 | (cp >> 6));
        out[o + 1] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[o]     = (char)(0xE0 | (cp >> 12));
        out[o + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o + 2] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
}

/*
 * Copy an ID3 text frame body into a UTF-8 buffer.
 *
 * The first byte is the encoding: 0 latin1, 1 UTF-16 with BOM, 2 UTF-16BE,
 * 3 UTF-8. This used to flatten all four to ASCII and replace everything
 * above 0x7F with '?', because the font was ASCII. The font is now
 * ark12, which has Latin-1 Supplement and Latin Extended-A, so the
 * flattening is gone and each encoding is converted properly instead.
 *
 * Note that encoding 0 is Latin-1, not ASCII and not UTF-8: byte 0xE9 in
 * a v2.3 frame means 'é' and has to be widened to two bytes, not copied.
 * Getting this backwards is the classic ID3 mojibake, and it is silent --
 * the tag looks fine to anything that also gets it backwards.
 *
 * Surrogate pairs in UTF-16 are decoded rather than dropped so that a
 * tag containing an emoji yields one replacement box rather than two.
 */
static void id3_text_to_utf8(const uint8_t *body, size_t len, char *out, size_t out_len)
{
    if (len < 1 || out_len < 1) { if (out_len) out[0] = 0; return; }

    const uint8_t enc = body[0];
    size_t i = 1, o = 0;

    if (enc == 1 || enc == 2) {
        bool le = (enc == 1);
        if (enc == 1 && i + 1 < len) {
            if (body[i] == 0xFF && body[i + 1] == 0xFE) { le = true;  i += 2; }
            else if (body[i] == 0xFE && body[i + 1] == 0xFF) { le = false; i += 2; }
        }
        for (; i + 1 < len; i += 2) {
            uint32_t u = le ? ((uint32_t)body[i + 1] << 8 | body[i])
                            : ((uint32_t)body[i] << 8 | body[i + 1]);
            if (u == 0) break;

            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
                const uint32_t lo = le ? ((uint32_t)body[i + 3] << 8 | body[i + 2])
                                       : ((uint32_t)body[i + 2] << 8 | body[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;  /* unpaired */
            if (u < 0x20) continue;                      /* control codes */

            const size_t n = utf8_put(out, o, out_len, u);
            if (!n) break;
            o += n;
        }
    } else if (enc == 3) {
        /* Already UTF-8. Copied through rather than decoded and
         * re-encoded, but stopped at a character boundary: a byte-wise
         * memcpy into a 64 byte field is how the last character of a
         * long title becomes a box. */
        for (; i < len; ) {
            const uint8_t b = body[i];
            if (b == 0) break;
            size_t n = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2
                     : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xF8) == 0xF0) ? 4 : 1;
            if (i + n > len) break;
            if (o + n + 1 > out_len) break;
            memcpy(&out[o], &body[i], n);
            o += n;
            i += n;
        }
    } else {
        /* Latin-1. Every byte is its own codepoint, by definition. */
        for (; i < len; i++) {
            const uint8_t c = body[i];
            if (c == 0) break;
            if (c < 0x20) continue;
            const size_t n = utf8_put(out, o, out_len, c);
            if (!n) break;
            o += n;
        }
    }
    out[o] = 0;
}

esp_err_t id3_read_tags_at(FILE *f, long base, id3_tags_t *out)
{
    return id3_read_tags_ext(f, base, out, NULL);
}

esp_err_t id3_read_tags_ext(FILE *f, long base, id3_tags_t *out,
                            tag_extra_t *extra)
{
    uint8_t hdr[10];

    memset(out, 0, sizeof(*out));
    if (extra) memset(extra, 0, sizeof(*extra));

    fseek(f, base, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const int ver = hdr[3];
    /*
     * 5266: v2.2 is read. It names its frames in three characters (TT2,
     * TP1, TAL) where v2.3 uses four (TIT2, TPE1, TALB), with a 6-byte
     * frame header and a plain 24-bit size; each id is mapped to its v2.3
     * name below and the walk is otherwise the same. It was refused, with
     * a line per file, until a USB drive's first index printed 155 of
     * them and left those files untitled in the library.
     *
     * v2.2's 0x40 flag is compression, which no tagger wrote: refused,
     * as is anything before v2.2, which was never published.
     */
    const bool v22 = (ver == 2);
    if (ver < 2 || (v22 && (hdr[5] & 0x40))) {
        ESP_LOGI(TAG, "ID3v2.%d tag%s: not read", ver, v22 ? ", compressed" : "");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!v22 && (hdr[5] & 0x40)) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return ESP_ERR_INVALID_SIZE;
        const uint32_t esz = (ver >= 4) ? syncsafe32(ext) : be32(ext);
        fseek(f, (long)esz - ((ver >= 4) ? 4 : 0), SEEK_CUR);
    }

    const long tag_end = base + 10 + (long)syncsafe32(&hdr[6]);
    int found = 0;

    /* 5266: v2.2's ids, as the v2.3 ids the comparisons below know. TYE
     * is v2.3's TYER and not TDRC -- a year. */
    static const char v22_ids[][2][5] = {
        { "TT2", "TIT2" }, { "TP1", "TPE1" }, { "TAL", "TALB" }, { "TCO", "TCON" },
        { "TYE", "TYER" }, { "TP2", "TPE2" }, { "TRK", "TRCK" }, { "TPA", "TPOS" },
    };
    const size_t fhl = v22 ? 6 : 10;

    /* 5262: with extra, eight frames are wanted and the walk is the tag. */
    while (ftell(f) + (long)fhl <= tag_end && (extra || found < 3)) {
        uint8_t fh[10];
        if (fread(fh, 1, fhl, f) != fhl) break;
        if (fh[0] == 0) break;                      /* padding */

        const uint32_t fsz = v22 ? be24(&fh[3])
                           : (ver >= 4) ? syncsafe32(&fh[4]) : be32(&fh[4]);
        if (fsz == 0 || ftell(f) + (long)fsz > tag_end) break;
        if (v22) {
            /* The id rewritten in place; one v2.3 never uses stands for
             * a frame this reader does not want. */
            const char *as = "XXXX";
            for (size_t k = 0; k < sizeof(v22_ids) / sizeof(v22_ids[0]); k++)
                if (!memcmp(fh, v22_ids[k][0], 3)) { as = v22_ids[k][1]; break; }
            memcpy(fh, as, 4);
        }

        char *dst = NULL;
        size_t dst_len = 0;
        if      (!memcmp(fh, "TIT2", 4)) { dst = out->title;  dst_len = sizeof(out->title);  }
        else if (!memcmp(fh, "TPE1", 4)) { dst = out->artist; dst_len = sizeof(out->artist); }
        else if (!memcmp(fh, "TALB", 4)) { dst = out->album;  dst_len = sizeof(out->album);  }
        /* 5262. TDRC is v2.4's date and TYER v2.3's year; either fills
         * date, and a TDRC does not lose to a TYER after it. TPE2 is the
         * band -- what every tagger writes album artist into. */
        else if (extra && !memcmp(fh, "TCON", 4)) { dst = extra->genre;       dst_len = sizeof(extra->genre); }
        else if (extra && !memcmp(fh, "TDRC", 4)) { dst = extra->date;        dst_len = sizeof(extra->date); }
        else if (extra && !memcmp(fh, "TYER", 4) && !extra->date[0])
                                                  { dst = extra->date;        dst_len = sizeof(extra->date); }
        else if (extra && !memcmp(fh, "TPE2", 4)) { dst = extra->albumartist; dst_len = sizeof(extra->albumartist); }
        else if (extra && !memcmp(fh, "TRCK", 4)) { dst = extra->track;       dst_len = sizeof(extra->track); }
        else if (extra && !memcmp(fh, "TPOS", 4)) { dst = extra->disc;        dst_len = sizeof(extra->disc); }

        if (!dst) {
            fseek(f, (long)fsz, SEEK_CUR);
            continue;
        }

        /* Frames are small; anything absurd is a corrupt size field, and
         * malloc'ing it would be the corruption's idea rather than ours. */
        if (fsz > 512) { fseek(f, (long)fsz, SEEK_CUR); continue; }

        uint8_t body[512];
        if (fread(body, 1, fsz, f) != fsz) break;
        id3_text_to_utf8(body, fsz, dst, dst_len);
        found++;
    }
    if (extra) tag_genre_fix(extra->genre, sizeof(extra->genre));

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t id3_read_tags(FILE *f, id3_tags_t *out)
{
    return id3_read_tags_at(f, 0, out);
}

/* ------------------------------------------------------------------ */
/* Decode and draw                                                     */
/* ------------------------------------------------------------------ */

/*
 * Is this a baseline JPEG?
 *
 * The P4's decoder handles SOF0 and nothing else. Handed a progressive
 * file it walks the markers, never finds SOF0, reaches the scan and
 * reports:
 *
 *   E jpeg.decoder: SOS encountered before SOF0
 *   E tab5_art: albumart_draw(261): jpeg info
 *   W tab5_mp3: cover art failed to decode (ESP_ERR_NOT_FOUND)
 *
 * Which is accurate, in the way a stack trace is accurate. It reads as a
 * corrupt tag or a bug in this file, and it is neither: the file is a
 * perfectly good JPEG that this silicon cannot decode. Saying so is a
 * fifteen line marker walk, and it is worth it because the answer tells
 * you what to do about it -- re-encode that cover as baseline -- while
 * the driver's version does not.
 *
 * Markers are two bytes, 0xFF then a code, and every one except the
 * standalone few carries a big-endian length that includes itself. SOS
 * ends the header, so the SOF has to appear before it or there is not
 * one.
 */
bool albumart_jpeg_is_baseline(const uint8_t *p, size_t len, uint8_t *sof_out,
                               uint32_t *w_out, uint32_t *h_out)
{
    return img_jpeg_sof(p, len, sof_out, w_out, h_out);   /* 6138; 8069: feckless_image.h */
}

static const char *sof_name(uint8_t m)
{
    switch (m) {
    case 0xC2: return "progressive";
    case 0xC3: return "lossless";
    case 0xC5: case 0xC6: case 0xC7: return "differential";
    case 0xC9: case 0xCA: case 0xCB: return "arithmetic-coded";
    case 0xCD: case 0xCE: case 0xCF: return "differential arithmetic-coded";
    case 0x00: return "no SOF marker";
    default:   return "unsupported";
    }
}

/*
 * ONE DECODED COVER, KEPT.
 *
 * mediacache.h has said since it was written that caching the decode
 * would cost forty times the memory to save a delay nobody can perceive.
 * That was true while every cover went through the hardware codec in
 * single-digit milliseconds. 1104 made the 3000x3000 cover work by
 * routing it through TJpgDec, and the board then measured the same
 * picture -- same 1871582 bytes, same hash 04d36f37 -- decoding for 4.6
 * seconds at every track change AND at every settings-panel close. Nine
 * decodes in one session, nine identical hashes, one of them necessary.
 *
 * 4.6 s is not a delay nobody can perceive, and it runs on media_task,
 * so the prefetch queues behind it. The arithmetic that made caching
 * absurd has been overturned by its own subject.
 *
 * WHAT IS KEPT: one frame, the last one drawn, with the hash of the
 * bitstream it came from. 1011 wanted exactly this and could not have
 * it, because the hash existed only on the draw path; 1102 put it on the
 * store path as a side effect of sharing buffers, and 1108's
 * albumart_cover_hash() made it one function rather than two that had to
 * agree.
 *
 * THE CAP IS THE LOAD-BEARING PART. A full-size decode of a large cover
 * is megabytes -- 3000x3000 is 17 MB -- and the largest free PSRAM block
 * is a structural 16128 KB with the framebuffer sitting across the
 * middle of the heap. Retaining a frame of that size would guarantee the
 * next large cover cannot decode at all, turning a slow success into a
 * permanent failure. Two megabytes covers the sizes actually reached:
 * a 700x700 native decode is ~991 KB and TJpgDec's 1/4 of a 3000 px
 * cover is 1098 KB. Anything larger is drawn and dropped.
 *
 * AND THE OLD FRAME GOES BEFORE THE NEW DECODE STARTS, not after. Held
 * across the decode it would be a megabyte of the contiguous block the
 * decode is about to ask for, on the one path where that block is
 * already short.
 */
#define COVER_KEEP_MAX_BYTES  (2u * 1024 * 1024)

static uint8_t *s_kept;
static size_t   s_kept_size;
static int      s_kept_w, s_kept_h, s_kept_stride;
static uint32_t s_kept_hash;

static void cover_drop(void)
{
    if (!s_kept) return;
    free(s_kept);
    s_kept = NULL;
    s_kept_size = 0;
    s_kept_hash = 0;
}

/* Takes ownership when it keeps, and says whether it did -- the caller
 * frees only what was refused. */
static bool cover_retain(uint8_t *rgb, size_t size, int iw, int ih,
                         int stride, uint32_t hash)
{
    if (!rgb || !size || size > COVER_KEEP_MAX_BYTES) return false;
    cover_drop();
    s_kept = rgb;
    s_kept_size = size;
    s_kept_w = iw;
    s_kept_h = ih;
    s_kept_stride = stride;
    s_kept_hash = hash;
    return true;
}

void albumart_forget_cover(void)
{
    cover_drop();
}

/* ------------------------------------------------------------------ */
/* Keystone                                                            */
/* ------------------------------------------------------------------ */

/*
 * A slight lean on every cover, as if the sleeve were a print propped on
 * a shelf rather than a flat bitmap.
 *
 * Drawn once, when the cover is placed, and never animated: the cost is
 * one resample of the placed rectangle on a load that already takes
 * 100-250 ms, and nothing at all while music plays. There is no sensor
 * behind it -- a live tilt would repaint the art band at frame rate,
 * which is the PSRAM load the cyan flash was about.
 *
 * Seeded by the TRACK, not the picture: the caller names what is being
 * shown with albumart_set_key() -- a file's path, a station's URL -- so
 * every track on an album leans its own way, and a repaint of the same
 * track (closing the chooser, a card going away) does not move it.
 * Without a key it falls back to the cover's own hash.
 *
 * The wide edge keeps the full fitted size and only the far edge
 * shrinks, so the trapezoid always fits where the square did and no
 * cover gets smaller to make room.
 *
 * One axis per cover: either the top or bottom edge recedes (each ROW
 * is narrower), or the left or right edge does (each COLUMN is shorter).
 * No depth foreshortening along the other axis -- at these angles it is
 * under a pixel per hundred and nobody can see it.
 *
 * ART_KEYSTONE 0 turns it off.
 */
#ifndef ART_KEYSTONE
#define ART_KEYSTONE        1
#endif
/* The far edge's shrink, as a fraction of the near one: 3..8 percent,
 * which reads as roughly 2..5 degrees of lean. More starts to look like
 * a rendering fault instead of a photograph. */
#define KEYSTONE_MIN_PM     (30)    /* per mille */
#define KEYSTONE_MAX_PM     (80)

/*
 * The hardware JPEG engine is made once, at boot, and never destroyed:
 * the radio takes the DMA-capable internal RAM its descriptors need when
 * the station joins (5145), so an engine made later is a coin toss. The
 * measurements and the reasoning moved with the code to
 * feckless_image_handler's image_esp.c (8069); img_esp_init() is what
 * albumart_init() calls.
 */
static uint32_t s_keystone_seed;
static uint32_t s_keystone_key;     /* 0 = none named; see albumart_set_key() */

void albumart_init(void)
{
    img_esp_init();
}

void albumart_set_key(const char *key)
{
    s_keystone_key = (key && key[0]) ? albumart_cover_hash(key, strlen(key)) | 1u : 0;
}

/*
 * And where the cover sits in its box: artplace.h, which has the whole
 * argument. Kept there rather than here so it is pure and testable --
 * albumart.c cannot be host-compiled, and the placement is arithmetic
 * with two callers and an off-by-one's worth of room to be wrong in.
 *
 * Seeded from s_keystone_seed, so the offset is per track exactly as the
 * lean is, and a repaint of the same track does not move it.
 */
static void art_place(int box_w, int box_h, int cw, int ch, int *dx, int *dy)
{
    const artplace_t p = artplace(box_w, box_h, cw, ch, s_keystone_seed);
    *dx = p.dx;
    *dy = p.dy;
    if (p.shift) {
        ESP_LOGI(TAG, "cover offset %d px %s of centre (%d spare)",
                 p.shift < 0 ? -p.shift : p.shift,
                 p.horizontal ? (p.shift < 0 ? "left" : "right")
                              : (p.shift < 0 ? "up" : "down"),
                 p.slack);
    }
}

/* a over b, alpha 0..256, RGB565 */
static inline uint16_t mix565(uint16_t a, uint16_t b, int alpha)
{
    const int ia = 256 - alpha;
    const int r = (((a >> 11) & 0x1F) * alpha + ((b >> 11) & 0x1F) * ia) >> 8;
    const int g = (((a >> 5) & 0x3F) * alpha + ((b >> 5) & 0x3F) * ia) >> 8;
    const int bl = ((a & 0x1F) * alpha + (b & 0x1F) * ia) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/*
 * Resample one line of n pixels into a narrower centred span of the same
 * line: `in` is a copy, `out` is the framebuffer. `shrink_q16` is the
 * span's width as a 16.16 fraction of n. The two cut edges are blended
 * into the background by their coverage, so the slant is not a stair.
 * `step` is the distance between pixels in both buffers (1 for a row,
 * the stride for a column).
 */
static void keystone_line(const uint16_t *in, uint16_t *out, int n,
                          size_t in_step, size_t out_step, uint32_t shrink_q16)
{
    const uint16_t bg = UI_BG_RGB565;
    /* span in 16.16 output pixels, and where it starts */
    const int64_t span = ((int64_t)n * shrink_q16);
    const int64_t lo = (((int64_t)n << 16) - span) / 2;
    const int64_t hi = lo + span;
    /* source step per output pixel: n / span */
    const uint32_t step = (uint32_t)((((int64_t)n) << 32) / (span ? span : 1));

    for (int i = 0; i < n; i++) {
        const int64_t p0 = (int64_t)i << 16, p1 = p0 + 65536;
        uint16_t *o = &out[(size_t)i * out_step];
        if (p1 <= lo || p0 >= hi) { *o = bg; continue; }
        /* the source pixel under this output pixel's centre */
        const int64_t c = p0 + 32768 - lo;
        int sx = (int)((c * step) >> 32);
        if (sx < 0) sx = 0;
        if (sx >= n) sx = n - 1;
        const uint16_t v = in[(size_t)sx * in_step];
        /* coverage of this pixel by the span, 0..65536 */
        const int64_t a0 = p0 > lo ? p0 : lo, a1 = p1 < hi ? p1 : hi;
        const int cov = (int)(a1 - a0);
        *o = cov >= 65536 ? v : mix565(v, bg, cov >> 8);
    }
}

static void keystone_apply(uint16_t *fb, int fbw, int dx, int dy, int cw, int ch)
{
#if ART_KEYSTONE
    if (cw < 16 || ch < 16) return;

    const uint32_t h = s_keystone_seed * 2654435761u;   /* spread the bits */
    const bool rows = (h >> 31) & 1;     /* top/bottom recedes vs left/right */
    const bool flip = (h >> 30) & 1;     /* which of the two */
    const int  pm = KEYSTONE_MIN_PM +
                    (int)((h >> 8) % (KEYSTONE_MAX_PM - KEYSTONE_MIN_PM + 1));
    const uint32_t k_q16 = (uint32_t)(((uint64_t)pm << 16) / 1000);

    uint16_t *tmp = heap_caps_malloc((size_t)cw * ch * 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        ESP_LOGW(TAG, "keystone: no room for %dx%d; drawn flat", cw, ch);
        return;
    }
    for (int y = 0; y < ch; y++) {
        memcpy(&tmp[(size_t)y * cw], &fb[(size_t)(dy + y) * fbw + dx], (size_t)cw * 2);
    }

    /* Lines run along the receding edge's direction; `t` goes 0 at the
     * near edge to 1 at the far one. */
    const int lines = rows ? ch : cw;
    for (int l = 0; l < lines; l++) {
        const int from_far = flip ? l : (lines - 1 - l);
        const uint32_t t_q16 = (uint32_t)(((uint64_t)(lines - 1 - from_far) << 16) / (lines - 1));
        const uint32_t shrink = 65536u - (uint32_t)(((uint64_t)k_q16 * t_q16) >> 16);
        if (rows) {
            keystone_line(&tmp[(size_t)l * cw], &fb[(size_t)(dy + l) * fbw + dx],
                          cw, 1, 1, shrink);
        } else {
            keystone_line(&tmp[l], &fb[(size_t)dy * fbw + dx + l],
                          ch, (size_t)cw, (size_t)fbw, shrink);
        }
    }
    heap_caps_free(tmp);

    ESP_LOGI(TAG, "keystone: %s edge %d.%d%% narrower",
             rows ? (flip ? "top" : "bottom") : (flip ? "left" : "right"),
             pm / 10, pm % 10);
#else
    (void)fb; (void)fbw; (void)dx; (void)dy; (void)cw; (void)ch;
#endif
}

/*
 * Centre, fit and blit a decoded RGB565 cover.
 *
 * Split out so it can be reached twice: once behind a decode, and once
 * from the retained frame when the same picture is asked for again.
 * Everything it needs is the pixels and their shape -- it never sees the
 * file -- which is what makes the second path possible at all.
 *
 * The fit (both directions, whole pixels for a small picture, 5088) and
 * the draw are feckless_image_fit.h's; where the result sits and the
 * keystone are ours.
 */
static esp_err_t blit_cover(esp_lcd_panel_handle_t panel,
                            int screen_w, int screen_h,
                            const uint8_t *rgb, int iw, int ih, int stride)
{
    (void)panel;

    int cw, ch, kint;
    img_fit_size(iw, ih, screen_w, screen_h, &cw, &ch, &kint);

    int dx, dy;
    art_place(screen_w, screen_h, cw, ch, &dx, &dy);

    if (kint) {
        ESP_LOGI(TAG, "cover enlarged %dx to %dx%d", kint, cw, ch);
    } else if (cw != iw || ch != ih) {
        ESP_LOGI(TAG, "cover %s to %dx%d",
                 (cw > iw) ? "enlarged" : "fitted", cw, ch);
    }

    /* The shadow, not the panel's buffer. Drawing straight into the
     * scanned-out buffer is what made the cover appear as a flash of
     * black followed by a slow fill. */
    uint16_t *fb = gfx_fb();
    ESP_RETURN_ON_ERROR(fb ? ESP_OK : ESP_ERR_INVALID_STATE,
                        TAG, "no shadow buffer");

    /* Clear to the UI's background, then place the picture, then copy
     * the band up once.
     *
     * screen_h is the height of the artwork area, not of the panel: the
     * caller passes the space above the transport bar. Clearing the full
     * panel height here would blank the bar in the shadow and blit that
     * over it.
     *
     * AND screen_w is not the framebuffer's stride. In portrait the
     * artwork band is the full width and the two are equal; in landscape
     * the band is 560 wide inside a 1280-wide buffer. fbw is the stride,
     * screen_w is the region -- they are different numbers and every
     * write uses the right one. */
    const int fbw = gfx_w();
    gfx_fill_rect(0, 0, screen_w, screen_h, UI_BG_RGB565);
    const img_pic_t pic = { .px = (uint16_t *)(uintptr_t)rgb, .w = iw, .h = ih, .stride = stride };
    img_fit_draw(&pic, &fb[(size_t)dy * fbw + dx], (size_t)fbw, cw, ch, kint);
    keystone_apply(fb, fbw, dx, dy, cw, ch);

    /* ui_blit_art_err() knows whether the artwork is a band or a column;
     * this does not, and the difference is the whole screen. The _err
     * form because a cover that did not reach the glass must not be
     * reported as shown. */
    return ui_blit_art_err();
}

/*
 * Any picture this player shows: JPEG, PNG, JPEG XL or GIF, by its
 * magic bytes (img_sniff), decoded by img_decode() in the order
 * feckless_image_handler's feckless_image_decode.h describes, and drawn.
 *
 * The retained frame answers a repeat without a decoder: the
 * settings-panel close and the track change within an album, which the
 * board measured at 4.6 s each (and which PNG, JPEG XL and GIF now share;
 * only JPEG had it before).
 */
static esp_err_t err_of(img_err_t e)
{
    switch (e) {
    case IMG_OK:              return ESP_OK;
    case IMG_ERR_NO_MEM:      return ESP_ERR_NO_MEM;
    case IMG_ERR_BAD:
    case IMG_ERR_UNSUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    default:                  return ESP_FAIL;
    }
}

esp_err_t albumart_show(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *img, size_t img_len)
{
    /*
     * The hash first, before an engine, a buffer or a byte of work: it
     * is what lets the whole decode be skipped.
     */
    const uint32_t in_hash = albumart_cover_hash(img, img_len);
    s_keystone_seed = s_keystone_key ? s_keystone_key : in_hash;

    if (s_kept && s_kept_hash == in_hash) {
        ESP_LOGI(TAG, "cover from the kept frame (hash %08x, %dx%d)",
                 (unsigned)in_hash, s_kept_w, s_kept_h);
        return blit_cover(panel, screen_w, screen_h, s_kept,
                          s_kept_w, s_kept_h, s_kept_stride);
    }

    /*
     * A miss, so the old frame goes NOW rather than after the decode.
     * Held across it, it is a megabyte of the contiguous block the
     * decode is about to ask for.
     */
    cover_drop();

    const bool jpeg = img_sniff(img, img_len) == IMG_JPEG;
    if (jpeg) {
        /* The last baseline, and the one that matters: a map taken
         * before the FIRST hardware decode says what held the
         * DMA-capable internal heap when the engine needed it. Once only. */
        static bool mapped;
        if (!mapped) {
            mapped = true;
            heapmap_log("before the first hardware jpeg decode");
        }
        /* Idempotent: with the boot engine in place it returns at once;
         * without one (boot-time creation failed, or an old caller) it
         * tries again, as this path always has, and software has it if
         * that fails too. */
        img_esp_init();
    }

    img_pic_t pic;
    const int64_t t0 = esp_timer_get_time();
    const img_err_t de = img_decode(img_esp_ops(), img, img_len, screen_w, screen_h,
                                    UI_BG_RGB565, &pic);

    if (pic.sof_w) {
        ESP_LOGI(TAG, "cover is %"PRIu32"x%"PRIu32" (SOF 0x%02X)", pic.sof_w, pic.sof_h, pic.sof);
    }
    if (pic.skipped) {
        ESP_LOGI(TAG, "cover: %s", pic.skipped);
    }
    if (de != IMG_OK) {
        if (pic.sof && de == IMG_ERR_UNSUPPORTED) {
            ESP_LOGW(TAG, "cover is a %s JPEG (SOF marker 0x%02X), %"PRIu32"x%"PRIu32"; "
                          "no decoder here reads it", sof_name(pic.sof), pic.sof,
                     pic.sof_w, pic.sof_h);
        } else {
            ESP_LOGW(TAG, "cover not decoded (%d): %s", (int)de,
                     pic.why ? pic.why : "no reason given");
        }
        return err_of(de);
    }
    ESP_LOGI(TAG, "cover decoded by %s: %dx%d (stride %d), %u KB, %lld ms",
             pic.decoder ? pic.decoder : "?", pic.w, pic.h, pic.stride,
             (unsigned)(pic.size / 1024),
             (long long)((esp_timer_get_time() - t0) / 1000));

    esp_err_t ret = blit_cover(panel, screen_w, screen_h, (const uint8_t *)pic.px,
                               pic.w, pic.h, pic.stride);

    /*
     * Kept only if it was drawn. A frame that failed to blit is not
     * what is on the panel, and answering a later request with it would
     * put up a picture nobody has seen instead of retrying.
     */
    if (ret == ESP_OK && cover_retain((uint8_t *)pic.px, pic.size, pic.w, pic.h,
                                      pic.stride, in_hash)) {
        pic.px = NULL;          /* ownership moved */
    }
    free(pic.px);
    return ret;
}

/* Kept for its callers' sake: a JPEG was once the only thing it took. */
esp_err_t albumart_draw(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *jpeg, size_t jpeg_len)
{
    return albumart_show(panel, screen_w, screen_h, jpeg, jpeg_len);
}
