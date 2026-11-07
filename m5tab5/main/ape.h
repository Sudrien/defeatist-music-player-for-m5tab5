/*
 * ape.h -- Monkey's Audio (.ape), decoded to the 16-bit PCM everything
 * downstream is built from.
 *
 * WHAT IS HERE
 *
 * Files from Monkey's Audio 3.99 on (file version 3990 and later: the
 * 3.99 encoder writes 3990, and so do the 4.x to 11.x ones), at all five
 * levels (Fast, Normal, High, Extra high, Insane), mono or stereo, 16 or
 * 24 bit, with or without an APEv2 or ID3v1 tag after the audio. A frame
 * is independent of the one before it (the predictors and the entropy
 * coder are reset at its start), so a seek is to a frame and a skip.
 *
 * NOT HERE, and refused by ape_probe() with a reason, not played badly:
 * file versions before 3990 (3.97 and older, which use different
 * entropy and predictor code), 8 and 32 bit, more than two channels, an
 * ID3v2 tag before the header, and files over 4 GB (the seek table is
 * 32-bit).
 *
 * WHERE IT COMES FROM
 *
 * Written from Matthew T. Ashland's Monkey's Audio SDK source
 * (monkeysaudio.com, (c) 2000-2022 Matthew T. Ashland, all rights
 * reserved): the file layout, the range coder and its probability table,
 * the two-stage predictor and the neural-net filters are his design and
 * these are his constants, and the source licence asks that the ideas
 * used be plainly acknowledged, which this is. The code is not a copy
 * of it: it is C, it decodes only, and it is arranged to read a frame
 * from a stream a few bytes at a time rather than from a file loaded
 * whole. The test (texttest/apetest.c) checks every file in
 * test_audio_files/48-56 against its own per-frame CRC32 and, for the
 * 16-bit ones, against the audio they were made from, so the output is
 * the encoder's input to the bit.
 *
 * ONE ODDITY WORTH KNOWING: the entropy-coded stream is stored as
 * little-endian 32-bit words that are read from their most significant
 * byte, so each group of four bytes in the file is in reverse order in
 * the stream. The groups are aligned to the first frame's offset, not
 * to the start of the file. ape_byte() undoes it as it refills.
 *
 * COST
 *
 * The decoder's work is the neural-net filters, one multiply-accumulate
 * and one adaptation step per tap per sample per channel, and it is
 * fixed by the level, not by the music: 16 taps at Normal, 64 at High,
 * 288 at Extra high, 1552 at Insane, per channel, per sample.
 * ape_dec_t counts microseconds in the decode and in the card reads so a
 * log can say whether it is realtime (see decoder.c's `ape timing`).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APE_H
#define APE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APE_ALLOC
#define APE_ALLOC(n) malloc(n)
#endif
#ifndef APE_CLOCK
#define APE_CLOCK() ((uint64_t)0)
#endif

#define APE_MIN_VERSION   3990
#define APE_WINDOW        4096      /* samples a roll buffer holds before it rolls */
#define APE_HIST          8         /* the predictor's history */
#define APE_READ_CHUNK    16384     /* bytes a refill asks the card for */
#define APE_MAX_ORDER     1280

typedef enum {
    APE_OK = 0,
    APE_NOT_APE,
    APE_UNSUPPORTED,                /* a real .ape this does not play; see `why` */
    APE_BAD,                        /* damaged or inconsistent */
} ape_status_t;

typedef struct {
    uint32_t version;               /* 3990 */
    uint32_t level;                 /* 1000 .. 5000 */
    uint32_t flags;
    uint32_t blocks_per_frame;
    uint32_t final_frame_blocks;
    uint32_t total_frames;
    uint32_t rate;
    uint16_t bits;                  /* 16 or 24 */
    uint16_t channels;              /* 1 or 2 */
    uint64_t blocks;                /* samples per channel in the file */
    uint32_t *seek;                 /* file offset of each frame; total_frames long */
    int64_t  data_end;              /* first byte after the audio */
    const char *why;                /* set with APE_UNSUPPORTED / APE_BAD */
} ape_t;

static inline void ape_free(ape_t *a)
{
    free(a->seek);
    a->seek = NULL;
}

static inline uint32_t ape_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint32_t ape_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * Read the descriptor, the header and the seek table. `a->seek` is
 * allocated with plain malloc() (free it with ape_free()): it is a few
 * kilobytes, and the caller that wants it in PSRAM can ape_probe() with
 * APE_ALLOC defined as it likes -- not for this: a table that small is
 * not worth the argument.
 */
static inline ape_status_t ape_probe(FILE *f, int64_t size, ape_t *a,
                                     storage_io_class_t cls)
{
    memset(a, 0, sizeof *a);
    uint8_t h[52 + 24];
    if (size < (int64_t)sizeof h || !storage_io_read_at(f, 0, h, 6, cls))
        return APE_NOT_APE;
    if (memcmp(h, "MAC ", 4) != 0) {
        if (memcmp(h, "ID3", 3) == 0) {
            /* an APE with an ID3v2 tag in front of it is still an APE */
            a->why = "an ID3v2 tag before the header";
            return APE_UNSUPPORTED;
        }
        return APE_NOT_APE;
    }
    a->version = ape_le16(h + 4);
    if (a->version < APE_MIN_VERSION) {
        a->why = "an old file version (before 3.99)";
        return APE_UNSUPPORTED;
    }
    if (!storage_io_read_at(f, 0, h, 52, cls)) return APE_BAD;
    const uint32_t desc_bytes = ape_le32(h + 8);
    const uint32_t head_bytes = ape_le32(h + 12);
    const uint32_t seek_bytes = ape_le32(h + 16);
    const uint32_t wavh_bytes = ape_le32(h + 20);
    const uint32_t term_bytes = ape_le32(h + 32);
    if (desc_bytes < 52 || head_bytes < 24 || desc_bytes > 4096 || head_bytes > 4096) {
        a->why = "an unexpected header size";
        return APE_BAD;
    }
    if (!storage_io_read_at(f, desc_bytes, h + 52, 24, cls)) return APE_BAD;
    const uint8_t *g = h + 52;
    a->level              = ape_le16(g);
    a->flags              = ape_le16(g + 2);
    a->blocks_per_frame   = ape_le32(g + 4);
    a->final_frame_blocks = ape_le32(g + 8);
    a->total_frames       = ape_le32(g + 12);
    a->bits               = (uint16_t)ape_le16(g + 16);
    a->channels           = (uint16_t)ape_le16(g + 18);
    a->rate               = ape_le32(g + 20);

    if (a->level < 1000 || a->level > 5000 || a->level % 1000) {
        a->why = "an unknown compression level";
        return APE_UNSUPPORTED;
    }
    if (a->bits != 16 && a->bits != 24) {
        a->why = "a sample size other than 16 or 24 bits";
        return APE_UNSUPPORTED;
    }
    if (a->channels < 1 || a->channels > 2) {
        a->why = "more than two channels";
        return APE_UNSUPPORTED;
    }
    if (a->rate < 8000 || a->rate > 384000 || a->blocks_per_frame == 0 ||
        a->blocks_per_frame > 10000000u || a->total_frames == 0 ||
        a->final_frame_blocks == 0 || a->final_frame_blocks > a->blocks_per_frame ||
        seek_bytes / 4 < a->total_frames || (int64_t)seek_bytes > size) {
        a->why = "inconsistent header fields";
        return APE_BAD;
    }
    a->blocks = (uint64_t)(a->total_frames - 1) * a->blocks_per_frame +
                a->final_frame_blocks;

    a->seek = (uint32_t *)malloc((size_t)a->total_frames * 4);
    if (!a->seek) return APE_BAD;
    const int64_t at = (int64_t)desc_bytes + head_bytes;
    bool ok = true;
    for (uint32_t i = 0; i < a->total_frames && ok; ) {
        uint8_t t[1024];
        uint32_t n = a->total_frames - i;
        if (n > sizeof t / 4) n = sizeof t / 4;
        ok = storage_io_read_at(f, at + (int64_t)i * 4, t, (size_t)n * 4, cls);
        for (uint32_t j = 0; j < n && ok; j++) a->seek[i + j] = ape_le32(t + 4 * j);
        i += n;
    }
    if (!ok) { ape_free(a); return APE_BAD; }

    /* Where the audio ends: before an ID3v1 tag, an APEv2 tag, and the
     * terminating data the encoder may have kept from the WAV. */
    int64_t end = size;
    uint8_t t[32];
    if (end >= 128 && storage_io_read_at(f, end - 128, t, 3, cls) && !memcmp(t, "TAG", 3))
        end -= 128;
    if (end >= 32 && storage_io_read_at(f, end - 32, t, 32, cls) &&
        !memcmp(t, "APETAGEX", 8)) {
        end -= (int64_t)ape_le32(t + 12);
        if (ape_le32(t + 20) & 0x80000000u) end -= 32;
    }
    end -= term_bytes;
    a->data_end = end;

    /* The table has to climb, stay inside the audio, and start after the
     * headers; or a frame would be read from the middle of something. */
    const int64_t first = at + seek_bytes + wavh_bytes;
    for (uint32_t i = 0; i < a->total_frames; i++) {
        if ((int64_t)a->seek[i] < first || (int64_t)a->seek[i] >= end ||
            (i && a->seek[i] <= a->seek[i - 1])) {
            ape_free(a);
            a->why = "a seek table that does not fit the file";
            return APE_BAD;
        }
    }
    return APE_OK;
}

/* ------------------------------------------------------------------ */
/* The neural-net filter: a sign-sign LMS filter over the last `order`
 * outputs, in 16 bits. Dot products accumulate in four 32-bit lanes and
 * wrap there, as the encoder's SIMD code does, and that is what the
 * files were made with. */

typedef struct {
    int       order, shift, total;
    int32_t   round;
    int32_t   avg;
    int16_t  *in, *dl, *m;          /* in, delta: window + order; m: order */
    int16_t  *ip, *dp;              /* where the next sample goes */
} ape_nn_t;

static inline void ape_nn_flush(ape_nn_t *n)
{
    memset(n->in, 0, (size_t)(n->order + 1) * 2);
    memset(n->dl, 0, (size_t)(n->order + 1) * 2);
    memset(n->m, 0, (size_t)n->order * 2);
    n->ip = n->in + n->order;
    n->dp = n->dl + n->order;
    n->avg = 0;
}

static inline int16_t ape_sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static inline int32_t ape_nn_run(ape_nn_t *n, int32_t input)
{
    const int order = n->order;
    const int16_t *x = n->ip - order;
    int16_t *m = n->m;
    uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    for (int i = 0; i < order; i += 4) {
        t0 += (uint32_t)((int32_t)x[i]     * m[i]);
        t1 += (uint32_t)((int32_t)x[i + 1] * m[i + 1]);
        t2 += (uint32_t)((int32_t)x[i + 2] * m[i + 2]);
        t3 += (uint32_t)((int32_t)x[i + 3] * m[i + 3]);
    }
    const int64_t dot = (int64_t)(int32_t)t0 + (int32_t)t1 + (int32_t)t2 + (int32_t)t3;

    const int16_t *dd = n->dp - order;
    if (input < 0) {
        for (int i = 0; i < order; i++) m[i] = (int16_t)(m[i] + dd[i]);
    } else if (input > 0) {
        for (int i = 0; i < order; i++) m[i] = (int16_t)(m[i] - dd[i]);
    }

    const int32_t out = (int32_t)((int64_t)input + ((dot + n->round) >> n->shift));
    n->ip[0] = ape_sat16(out);

    const int32_t a = out < 0 ? -out : out;
    int32_t d;
    if (a > n->avg * 3)                d = ((out >> 25) & 64) - 32;
    else if (a > (n->avg * 4) / 3)     d = ((out >> 26) & 32) - 16;
    else if (a > 0)                    d = ((out >> 27) & 16) - 8;
    else                               d = 0;
    n->dp[0] = (int16_t)d;
    n->avg += (a - n->avg) / 16;
    n->dp[-1] = (int16_t)(n->dp[-1] >> 1);
    n->dp[-2] = (int16_t)(n->dp[-2] >> 1);
    n->dp[-8] = (int16_t)(n->dp[-8] >> 1);

    n->ip++;
    n->dp++;
    if (n->ip == n->in + n->total) {
        memmove(n->in, n->ip - order, (size_t)order * 2);
        memmove(n->dl, n->dp - order, (size_t)order * 2);
        n->ip = n->in + order;
        n->dp = n->dl + order;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* One channel's predictor: the filters, then a small adaptive stage
 * that mixes this channel's recent past with the other channel's. */

typedef struct {
    ape_nn_t  nn[3];
    int       nnn;
    int32_t  *base;                 /* four arrays of WINDOW + HIST */
    int32_t  *pa, *pb, *aa, *ab;    /* current positions */
    int32_t   ma[4], mb[5];
    int32_t   last_a, s1a, s1b;
    int       idx;
} ape_pred_t;

#define APE_ARR (APE_WINDOW + APE_HIST)

static inline void ape_pred_flush(ape_pred_t *p)
{
    for (int i = 0; i < p->nnn; i++) ape_nn_flush(&p->nn[i]);
    for (int i = 0; i < 4; i++) memset(p->base + i * APE_ARR, 0, (APE_HIST + 1) * 4);
    p->pa = p->base + APE_HIST;
    p->pb = p->base + APE_ARR + APE_HIST;
    p->aa = p->base + 2 * APE_ARR + APE_HIST;
    p->ab = p->base + 3 * APE_ARR + APE_HIST;
    memset(p->ma, 0, sizeof p->ma);
    memset(p->mb, 0, sizeof p->mb);
    p->ma[0] = 360; p->ma[1] = 317; p->ma[2] = -109; p->ma[3] = 98;
    p->last_a = p->s1a = p->s1b = 0;
    p->idx = 0;
}

static inline int32_t ape_sgn(int32_t v)
{
    return v ? ((v >> 30) & 2) - 1 : 0;
}

/* a: this channel's residual; b: the other channel's latest value (the
 * one before it for the second channel decoded). Everything is 32-bit and
 * wraps, 24-bit included, exactly as the reference does. */
static inline int32_t ape_pred_run(ape_pred_t *p, int32_t a, int32_t b)
{
    if (p->idx == APE_WINDOW) {
        int32_t **cur[4] = { &p->pa, &p->pb, &p->aa, &p->ab };
        for (int i = 0; i < 4; i++) {
            int32_t *base = p->base + i * APE_ARR;
            memmove(base, *cur[i] - APE_HIST, APE_HIST * 4);
            *cur[i] = base + APE_HIST;
        }
        p->idx = 0;
    }
    for (int i = 0; i < p->nnn; i++) a = ape_nn_run(&p->nn[i], a);

    int32_t *pa = p->pa, *pb = p->pb, *aa = p->aa, *ab = p->ab;
    pa[0] = p->last_a;
    pa[-1] = (int32_t)((uint32_t)pa[0] - (uint32_t)pa[-1]);
    const int32_t bf = (int32_t)((uint32_t)b - (uint32_t)((int32_t)((uint32_t)p->s1b * 31u) >> 5));
    p->s1b = b;
    pb[0] = bf;
    pb[-1] = (int32_t)((uint32_t)pb[0] - (uint32_t)pb[-1]);

    int32_t cur;
    {
        const uint32_t sa = (uint32_t)pa[0] * (uint32_t)p->ma[0] +
                            (uint32_t)pa[-1] * (uint32_t)p->ma[1] +
                            (uint32_t)pa[-2] * (uint32_t)p->ma[2] +
                            (uint32_t)pa[-3] * (uint32_t)p->ma[3];
        const uint32_t sb = (uint32_t)pb[0] * (uint32_t)p->mb[0] +
                            (uint32_t)pb[-1] * (uint32_t)p->mb[1] +
                            (uint32_t)pb[-2] * (uint32_t)p->mb[2] +
                            (uint32_t)pb[-3] * (uint32_t)p->mb[3] +
                            (uint32_t)pb[-4] * (uint32_t)p->mb[4];
        const int32_t s = (int32_t)(sa + (uint32_t)((int32_t)sb >> 1));
        cur = (int32_t)((uint32_t)a + (uint32_t)(s >> 10));
    }

    aa[0]  = ape_sgn(pa[0]);
    aa[-1] = ape_sgn(pa[-1]);
    ab[0]  = ape_sgn(pb[0]);
    ab[-1] = ape_sgn(pb[-1]);
    if (a > 0) {
        p->ma[0] -= aa[0]; p->ma[1] -= aa[-1]; p->ma[2] -= aa[-2]; p->ma[3] -= aa[-3];
        p->mb[0] -= ab[0]; p->mb[1] -= ab[-1]; p->mb[2] -= ab[-2]; p->mb[3] -= ab[-3];
        p->mb[4] -= ab[-4];
    } else if (a < 0) {
        p->ma[0] += aa[0]; p->ma[1] += aa[-1]; p->ma[2] += aa[-2]; p->ma[3] += aa[-3];
        p->mb[0] += ab[0]; p->mb[1] += ab[-1]; p->mb[2] += ab[-2]; p->mb[3] += ab[-3];
        p->mb[4] += ab[-4];
    }

    p->s1a = (int32_t)((uint32_t)cur + (uint32_t)((int32_t)((uint32_t)p->s1a * 31u) >> 5));
    p->last_a = cur;
    p->pa++; p->pb++; p->aa++; p->ab++;
    p->idx++;
    return p->s1a;
}

/* ------------------------------------------------------------------ */
/* The decoder. */

typedef struct { uint32_t k, ksum; } ape_rice_t;

typedef struct {
    ape_t        a;                 /* copied; `seek` is shared, not owned */
    ape_pred_t   pred[2];
    ape_rice_t   rice[2];
    uint8_t     *buf;               /* APE_READ_CHUNK */
    uint32_t     pos, len;          /* in buf */
    uint32_t     skip;              /* where in the next refill the reader starts */
    uint32_t     base;              /* seek[0] % 4: where the word groups start */
    int64_t      buf_off;           /* file offset of buf[0], on a group */
    int64_t      file_at;           /* where the stream is positioned, -1 unknown */
    uint32_t     range, low, buffer;/* the range coder */
    uint32_t     frame;             /* the frame being decoded */
    uint32_t     frame_left;        /* blocks of it still to come */
    uint32_t     special;           /* the frame's special codes */
    uint32_t     stored_crc;
    int32_t      last_x;
    bool         err;
    bool         open;              /* a frame is open */
    storage_io_class_t cls;
    /* what a log wants to know */
    uint64_t     us_read, us_decode, blocks_done;
    uint32_t     reads, crc_bad;
    uint32_t     crc;               /* running, when APE_CHECK_CRC */
} ape_dec_t;

static inline void ape_dec_free(ape_dec_t *d)
{
    if (!d) return;
    for (int c = 0; c < 2; c++) {
        free(d->pred[c].base);
        for (int i = 0; i < d->pred[c].nnn; i++) {
            free(d->pred[c].nn[i].in);      /* one block: in, dl, m */
        }
    }
    free(d->buf);
    free(d);
}

static inline bool ape_nn_init(ape_nn_t *n, int order, int shift)
{
    n->order = order;
    n->shift = shift;
    n->total = APE_WINDOW + order;
    n->round = 1 << (shift - 1);
    const size_t each = (size_t)n->total;
    int16_t *blk = (int16_t *)APE_ALLOC((each * 2 + (size_t)order) * 2);
    if (!blk) return false;
    n->in = blk;
    n->dl = blk + each;
    n->m = blk + 2 * each;
    ape_nn_flush(n);
    return true;
}

static inline ape_dec_t *ape_dec_new(const ape_t *a, storage_io_class_t cls)
{
    ape_dec_t *d = (ape_dec_t *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->a = *a;
    d->cls = cls;
    d->file_at = -1;
    d->buf = (uint8_t *)APE_ALLOC(APE_READ_CHUNK);
    static const struct { int n; int order[3]; int shift[3]; } lv[5] = {
        { 0, { 0, 0, 0 },        { 0, 0, 0 } },          /* Fast */
        { 1, { 16, 0, 0 },       { 11, 0, 0 } },         /* Normal */
        { 1, { 64, 0, 0 },       { 11, 0, 0 } },         /* High */
        { 2, { 32, 256, 0 },     { 10, 13, 0 } },        /* Extra high */
        { 3, { 16, 256, 1280 },  { 11, 13, 15 } },       /* Insane */
    };
    const int L = (int)(a->level / 1000) - 1;
    bool ok = d->buf != NULL;
    for (int c = 0; c < a->channels && ok; c++) {
        ape_pred_t *p = &d->pred[c];
        p->base = (int32_t *)APE_ALLOC((size_t)4 * APE_ARR * 4);
        ok = p->base != NULL;
        for (int i = 0; i < lv[L].n && ok; i++) {
            ok = ape_nn_init(&p->nn[i], lv[L].order[i], lv[L].shift[i]);
            if (ok) p->nnn = i + 1;
        }
    }
    if (!ok) { ape_dec_free(d); return NULL; }
    d->a.seek = a->seek;
    d->base = (uint32_t)(a->seek[0] & 3);
    return d;
}

/* ---- bytes ---- */

static inline uint32_t ape_byte(ape_dec_t *d, FILE *f)
{
    if (d->pos >= d->len) {
        const int64_t off = d->buf_off + d->len;
        if (off >= d->a.data_end) return 0;     /* zeros past the audio */
        int64_t want = d->a.data_end - off;
        if (want > APE_READ_CHUNK) want = APE_READ_CHUNK;
        const uint64_t t0 = APE_CLOCK();
        size_t got;
        if (d->file_at != off) {
            got = storage_io_read_at(f, off, d->buf, (size_t)want, d->cls) ? (size_t)want : 0;
        } else {
            got = storage_io_fread(d->buf, (size_t)want, f, d->cls);
        }
        d->us_read += APE_CLOCK() - t0;
        d->reads++;
        d->file_at = got ? off + (int64_t)got : -1;
        if (!got) { d->err = true; return 0; }
        while (got & 3) d->buf[got++] = 0;      /* the last group, short */
        for (size_t i = 0; i < got; i += 4) {   /* reverse each group */
            const uint8_t b0 = d->buf[i], b1 = d->buf[i + 1];
            d->buf[i] = d->buf[i + 3];
            d->buf[i + 1] = d->buf[i + 2];
            d->buf[i + 2] = b1;
            d->buf[i + 3] = b0;
        }
        d->buf_off = off;
        d->len = (uint32_t)got;
        d->pos = d->skip;
        d->skip = 0;
        if (d->pos >= d->len) { d->err = true; return 0; }
    }
    return d->buf[d->pos++];
}

/* ---- the range coder ---- */

#define APE_TOP     0x80000000u
#define APE_BOTTOM  (APE_TOP >> 8)

static inline void ape_norm(ape_dec_t *d, FILE *f)
{
    while (d->range <= APE_BOTTOM) {
        d->buffer = (d->buffer << 8) | ape_byte(d, f);
        d->low = (d->low << 8) | ((d->buffer >> 1) & 0xFF);
        d->range <<= 8;
        if (d->range == 0) { d->err = true; d->range = APE_TOP; return; }
    }
}

/* n bits, 1 <= n <= 16 */
static inline uint32_t ape_bits(ape_dec_t *d, FILE *f, int n)
{
    ape_norm(d, f);
    d->range >>= n;
    const uint32_t v = d->low / d->range;
    d->low %= d->range;
    return v;
}

/* a value in [0, n) with equal odds, n >= 1 */
static inline uint32_t ape_flat(ape_dec_t *d, FILE *f, uint32_t n)
{
    ape_norm(d, f);
    d->range /= n;
    if (!d->range) { d->err = true; d->range = 1; }
    const uint32_t v = d->low / d->range;
    d->low %= d->range;
    return v;
}

/* The cumulative odds of an overflow symbol 0..63 out of 65536: twenty
 * and a bit entries that matter, then one more each. */
static const uint16_t ape_tot[22] = {
    0, 19578, 36160, 48417, 56323, 60899, 63265, 64435, 64971, 65232, 65351,
    65416, 65447, 65466, 65476, 65482, 65485, 65488, 65490, 65491, 65492, 65493
};
static inline uint32_t ape_total(unsigned i) { return i < 22 ? ape_tot[i] : 65493u + (i - 21); }

static const uint32_t ape_kmin[32] = {
    0, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
    131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216,
    33554432, 67108864, 134217728, 268435456, 536870912, 1073741824,
    2147483648u, 0, 0, 0, 0
};

/* One residual, signed. */
static inline int32_t ape_value(ape_dec_t *d, FILE *f, ape_rice_t *r)
{
    uint32_t pivot = r->ksum / 32;
    if (pivot < 1) pivot = 1;

    ape_norm(d, f);
    d->range >>= 16;
    const uint32_t cum = d->low / d->range;
    if (cum >= 65536) { d->err = true; return 0; }
    uint32_t ov = 0;
    while (cum >= ape_total(ov + 1)) ov++;
    d->low -= d->range * ape_total(ov);
    d->range *= ape_total(ov + 1) - ape_total(ov);
    if (ov == 63) {
        ov = ape_bits(d, f, 16) << 16;
        ov |= ape_bits(d, f, 16);
    }

    uint32_t base;
    if (pivot >= 65536) {
        int nb = 0;
        while ((pivot >> nb) > 0) nb++;
        const uint32_t split = 1u << (nb - 16);
        const uint32_t pa = pivot / split + 1;
        base = ape_flat(d, f, pa) * split;
        base += ape_flat(d, f, split);
    } else {
        base = ape_flat(d, f, pivot);
    }

    const int64_t v = (int64_t)base + (int64_t)ov * pivot;
    r->ksum += (uint32_t)(((v + 1) / 2) - (int64_t)((r->ksum + 16) >> 5));
    if (r->ksum < ape_kmin[r->k]) r->k--;
    else if (ape_kmin[r->k + 1] && r->ksum >= ape_kmin[r->k + 1]) r->k++;
    return (int32_t)((v & 1) ? (v >> 1) + 1 : -(v >> 1));
}

/* ---- frames ---- */

static inline uint32_t ape_frame_blocks(const ape_dec_t *d, uint32_t fr)
{
    return fr + 1 == d->a.total_frames ? d->a.final_frame_blocks
                                       : d->a.blocks_per_frame;
}

/* Position at the start of frame `fr` and read what precedes its audio.
 * False if there is no such frame. */
static inline bool ape_frame_open(ape_dec_t *d, FILE *f, uint32_t fr)
{
    d->open = false;
    d->err = false;
    if (fr >= d->a.total_frames) return false;
    const int64_t off = d->a.seek[fr];
    if (off >= d->buf_off && off < d->buf_off + (int64_t)d->len) {
        d->pos = (uint32_t)(off - d->buf_off);
    } else {
        /* the group the frame starts in, and how far into it */
        const int64_t g = off - (int64_t)((off - (int64_t)d->base) & 3);
        d->buf_off = g;
        d->len = d->pos = 0;
        d->skip = (uint32_t)(off - g);
    }
    uint32_t crc = 0;
    for (int i = 0; i < 4; i++) crc = (crc << 8) | ape_byte(d, f);
    d->special = 0;
    if (crc & 0x80000000u) {
        for (int i = 0; i < 4; i++) d->special = (d->special << 8) | ape_byte(d, f);
    }
    d->stored_crc = crc & 0x7FFFFFFFu;
    for (int c = 0; c < d->a.channels; c++) {
        ape_pred_flush(&d->pred[c]);
        d->rice[c].k = 10;
        d->rice[c].ksum = (1u << 10) * 16;
    }
    d->last_x = 0;
    (void)ape_byte(d, f);                       /* a dummy byte */
    d->buffer = ape_byte(d, f);
    d->low = d->buffer >> 1;
    d->range = 1u << 7;
    d->frame = fr;
    d->frame_left = ape_frame_blocks(d, fr);
    d->crc = 0xFFFFFFFFu;
    d->open = !d->err;
    return d->open;
}

#ifdef APE_CHECK_CRC
static inline uint32_t ape_crc_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    return crc;
}
static inline void ape_crc_sample(ape_dec_t *d, int32_t v)
{
    d->crc = ape_crc_byte(d->crc, (uint8_t)v);
    d->crc = ape_crc_byte(d->crc, (uint8_t)(v >> 8));
    if (d->a.bits == 24) d->crc = ape_crc_byte(d->crc, (uint8_t)(v >> 16));
}
#define APE_CRC_SAMPLE(d, v) ape_crc_sample(d, v)
#else
#define APE_CRC_SAMPLE(d, v) ((void)0)
#endif

/* A 24-bit sample to 16, as pcmfold_24_to_16() does it. */
static inline int16_t ape_fold24(int32_t s)
{
    s = (s + 128) >> 8;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/*
 * Up to `max` blocks of the open frame into `out`, interleaved 16-bit.
 * Returns the number of blocks; 0 if no frame is open, the frame is
 * done, or the data was bad (d->err is set for that).
 */
static inline uint32_t ape_decode(ape_dec_t *d, FILE *f, int16_t *out, uint32_t max)
{
    if (!d->open || d->err || !d->frame_left) return 0;
    uint32_t n = d->frame_left < max ? d->frame_left : max;
    const uint64_t t0 = APE_CLOCK();
    const uint64_t r0 = d->us_read;
    const bool b24 = d->a.bits == 24;
    const uint32_t sp = d->special;

    if (d->a.channels == 2) {
        if ((sp & 3) == 3) {
            for (uint32_t i = 0; i < n; i++) {
                out[2 * i] = out[2 * i + 1] = 0;
                APE_CRC_SAMPLE(d, 0);
                APE_CRC_SAMPLE(d, 0);
            }
        } else if (sp & 4) {                     /* the same in both */
            for (uint32_t i = 0; i < n && !d->err; i++) {
                const int32_t x = ape_pred_run(&d->pred[0],
                                    ape_value(d, f, &d->rice[0]), 0);
                out[2 * i] = out[2 * i + 1] = b24 ? ape_fold24(x) : ape_sat16(x);
                APE_CRC_SAMPLE(d, x);
                APE_CRC_SAMPLE(d, x);
            }
        } else {
            for (uint32_t i = 0; i < n && !d->err; i++) {
                const int32_t ny = ape_value(d, f, &d->rice[1]);
                const int32_t nx = ape_value(d, f, &d->rice[0]);
                const int32_t y = ape_pred_run(&d->pred[1], ny, d->last_x);
                const int32_t x = ape_pred_run(&d->pred[0], nx, y);
                d->last_x = x;
                const int32_t l = (int32_t)((uint32_t)x - (uint32_t)(y / 2));
                const int32_t r = (int32_t)((uint32_t)l + (uint32_t)y);
                out[2 * i]     = b24 ? ape_fold24(l) : ape_sat16(l);
                out[2 * i + 1] = b24 ? ape_fold24(r) : ape_sat16(r);
                APE_CRC_SAMPLE(d, l);
                APE_CRC_SAMPLE(d, r);
            }
        }
    } else {
        if (sp & 1) {
            for (uint32_t i = 0; i < n; i++) {
                out[i] = 0;
                APE_CRC_SAMPLE(d, 0);
            }
        } else {
            for (uint32_t i = 0; i < n && !d->err; i++) {
                const int32_t x = ape_pred_run(&d->pred[0],
                                    ape_value(d, f, &d->rice[0]), 0);
                out[i] = b24 ? ape_fold24(x) : ape_sat16(x);
                APE_CRC_SAMPLE(d, x);
            }
        }
    }
    d->us_decode += APE_CLOCK() - t0 - (d->us_read - r0);
    if (d->err) return 0;
    d->frame_left -= n;
    d->blocks_done += n;
#ifdef APE_CHECK_CRC
    if (!d->frame_left && (((d->crc ^ 0xFFFFFFFFu) >> 1) != d->stored_crc)) d->crc_bad++;
#endif
    return n;
}

/* The next block run of the stream, opening the following frame when
 * this one is done. 0 at the end of the file, or on bad data. */
static inline uint32_t ape_next(ape_dec_t *d, FILE *f, int16_t *out, uint32_t max)
{
    if (d->open && !d->frame_left) {
        if (!ape_frame_open(d, f, d->frame + 1)) return 0;
    }
    return ape_decode(d, f, out, max);
}

#ifdef __cplusplus
}
#endif

#endif /* APE_H */
