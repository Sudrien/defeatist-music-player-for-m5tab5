/*
 * dst.h -- DST (Direct Stream Transfer) frames to plain DSD
 *
 * DST is SACD's lossless coding of 1-bit audio, ISO/IEC 14496-3 subpart
 * 10. It appears here inside DFF: a `DST ` chunk of `DSTF` chunks, one
 * frame each, 1/75 s of every channel. This turns one frame back into
 * the DFF byte layout the rest of dsd.h takes -- channels interleaved a
 * byte at a time, MSB first -- so DST is a different way into the same
 * converter, not a different converter.
 *
 * Where the format knowledge comes from: the ISO text is not public, and
 * the one open decoder is FFmpeg's libavcodec/dstdec.c (Peter Ross,
 * 2014, LGPL 2.1). This file is written for this player, MIT, from what
 * that decoder shows of the bitstream; none of its code is here. Its
 * output is the test's oracle (texttest/dsttest.c, u-defeatist-0016),
 * bit for bit.
 *
 * A FRAME
 *
 *   1 bit   coded. 0: the frame is stored plainly, the DSD bytes from
 *           the second byte on (one more bit, then 6 zero bits first).
 *   Segmentation: three bits, which must all be 1 (one segment, the
 *           same for every channel, to the end of the channel). The
 *           other cases are not in any file this has seen, and FFmpeg
 *           refuses them too; so does this.
 *   Mapping: whether the probability tables map as the filters do,
 *           then which filter each channel uses (and which table).
 *   Half probability: a bit a channel -- whether its first `length`
 *           bits are coded at p = 1/2 instead of from the table.
 *   Filters: per filter a length (7 bits, +1) and up to 128 9-bit
 *           signed coefficients, plain or predicted with a Rice code.
 *   Tables: per table a length (6 bits, +1) and 7-bit entries from 1,
 *           coded the same way.
 *   1 zero bit, then the arithmetic-coded data.
 *
 * Each bit: the channel's filter runs over its last 128 bits (+1 or -1
 * each), and the 16-bit sum both predicts the bit (its sign) and picks a
 * probability (|sum| / 8, into the table). The arithmetic decoder gives
 * whether the prediction was wrong. 128 taps a bit would be slow, so the
 * filter is kept as 16 tables of 256, one per 8 bits of history, built
 * when the frame's coefficients arrive: a bit costs 16 lookups.
 *
 * IN PIECES
 *
 * A frame at DSD64 is 37632 bits a channel; decoding it at once is a
 * long stretch with nothing to break it. Everything that carries from
 * one bit to the next is in dst_dec_t, so dst_run() takes a frame a
 * slice at a time, any whole number of bytes a channel, and a boxed
 * caller keeps its windows short (dsd.h asks for DST_STEP).
 *
 * For a 32-bit core (u-defeatist-0017): the history is four 32-bit
 * words, the coded data comes through a 32-bit cache, and the lookups
 * are built by doubling. 0016's first board run had the decoder at about
 * 500 ns a bit a channel.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DST_H
#define DST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DST_MAX_CH      2               /* what dsd.h takes */
#define DST_MAX_ELEM    (2 * DST_MAX_CH)
#define DST_MAX_TAPS    128
#define DST_MAX_PROBS   64

/* Bytes a channel one dst_run() is asked for by dsd.h: 2048 bits a
 * channel. 512 was 0016's, and with the frame's set-up in the same call
 * it made a 6.4 ms window on a Tab5 (u-defeatist-0017). */
#ifndef DST_STEP
#define DST_STEP        256u
#endif

typedef struct {
    /* the frame being decoded: its bytes, and where the reader is */
    const uint8_t *p;
    uint32_t bytes;
    uint32_t pos;               /* bit position */

    uint32_t ch;
    uint32_t frame_bits;        /* bits a channel in a frame: 588 * 64 * mult */
    uint32_t done;              /* bits a channel decoded of this frame */
    bool     plain;             /* stored, not coded */

    uint32_t a, c;              /* the arithmetic decoder */
    /* The coded data's reader (0017): up to 32 bits ahead, MSB first, in
     * `cache`, `have` of them; `next` the next byte to load. */
    uint32_t cache, have, next;

    uint8_t  felem[DST_MAX_CH], pelem[DST_MAX_CH];
    bool     half[DST_MAX_CH];
    uint32_t flen[DST_MAX_ELEM], plen[DST_MAX_ELEM];
    uint32_t nf, np;            /* filters, tables */
    int16_t  coef[DST_MAX_ELEM][DST_MAX_TAPS];
    uint8_t  prob[DST_MAX_ELEM][DST_MAX_PROBS];

    /* the last 128 bits, newest in bit 0 of [0]: 32-bit words, so the 16
     * lookups take their bytes at fixed shifts on a 32-bit core (0017) */
    uint32_t hist[DST_MAX_CH][4];
    int16_t  lut[DST_MAX_ELEM][16][256];
} dst_dec_t;

/* ---- reading the frame ---- */

/* Past the end of the frame the reader gives zeros: the arithmetic
 * decoder reads ahead of what it uses, and FFmpeg's padding is zeros. */
static inline uint32_t dst_bit(dst_dec_t *d)
{
    const uint32_t i = d->pos++;
    if (i >= d->bytes * 8u) return 0;
    return (d->p[i >> 3] >> (7 - (i & 7))) & 1u;
}

static inline uint32_t dst_bits(dst_dec_t *d, uint32_t n)
{
    uint32_t v = 0;
    while (n--) v = v << 1 | dst_bit(d);
    return v;
}

static inline int32_t dst_sbits(dst_dec_t *d, uint32_t n)
{
    const uint32_t v = dst_bits(d, n);
    return (v & (1u << (n - 1))) ? (int32_t)v - (int32_t)(1u << n) : (int32_t)v;
}

static inline bool dst_left(const dst_dec_t *d)
{
    return d->pos < d->bytes * 8u;
}

/* A Rice code: zeros, a one, k bits; then a sign bit if not zero. */
static inline bool dst_rice(dst_dec_t *d, uint32_t k, int32_t *out)
{
    uint32_t q = 0;
    for (;;) {
        if (!dst_left(d)) return false;
        if (dst_bit(d)) break;
        if (++q > 1u << 16) return false;
    }
    int32_t v = (int32_t)(q << k | dst_bits(d, k));
    if (v && dst_bit(d)) v = -v;
    *out = v;
    return true;
}

/* Which element each channel uses. The first is 0; each next is either
 * one already named or the next new one. */
static inline bool dst_map(dst_dec_t *d, uint8_t *map, uint32_t *n)
{
    *n = 1;
    map[0] = 0;
    if (dst_bit(d)) {
        for (uint32_t c = 1; c < d->ch; c++) map[c] = 0;
        return true;
    }
    for (uint32_t c = 1; c < d->ch; c++) {
        uint32_t w = 1;                         /* bits to name 0 .. n */
        while ((1u << w) <= *n) w++;
        const uint32_t m = dst_bits(d, w);
        if (m > *n) return false;
        if (m == *n && ++*n > DST_MAX_ELEM) return false;
        map[c] = (uint8_t)m;
    }
    return true;
}

/* Filter coefficients (`sign`, 9 bits, from 0) or table entries
 * (unsigned, 7 bits, from 1). A coded set gives its first 1 to 3 values
 * plainly and predicts the rest from the ones before, with fixed
 * predictor weights (eighths) and a Rice-coded correction. */
static inline bool dst_set(dst_dec_t *d, int32_t *v, uint32_t len,
                           uint32_t bits, bool sign, int32_t from,
                           const int8_t w[3][3])
{
    uint32_t plain = len;
    int m = -1;
    if (dst_bit(d)) {
        m = (int)dst_bits(d, 2);
        if (m == 3) return false;
        plain = (uint32_t)m + 1;
    }
    for (uint32_t i = 0; i < plain; i++)
        v[i] = (sign ? dst_sbits(d, bits) : (int32_t)dst_bits(d, bits)) + from;
    if (m < 0) return true;
    const uint32_t k = dst_bits(d, 3);
    for (uint32_t i = plain; i < len; i++) {
        int32_t x = 0;
        for (int j = 0; j <= m; j++) x += w[m][j] * v[i - (uint32_t)j - 1];
        int32_t e;
        if (!dst_rice(d, k, &e)) return false;
        e += x >= 0 ? -((x + 4) / 8) : (-x + 3) / 8;
        if (!sign && (e < from || e >= from + (1 << bits))) return false;
        v[i] = e;
    }
    return true;
}

static inline uint32_t dst_ac_k(uint32_t a) { return (a >> 8) | ((a >> 7) & 1u); }

/* The coded data's reader. Zeros past the end, as dst_bit(). */
static inline void dst_fill(dst_dec_t *d)
{
    while (d->have <= 24) {
        const uint32_t b = d->next < d->bytes ? d->p[d->next] : 0;
        d->next++;
        d->cache |= b << (24 - d->have);
        d->have += 8;
    }
}

static inline uint32_t dst_take(dst_dec_t *d, uint32_t n)       /* 1 .. 11 */
{
    if (d->have < n) dst_fill(d);
    const uint32_t v = d->cache >> (32 - n);
    d->cache <<= n;
    d->have -= n;
    return v;
}

/* From dst_bit()'s position to the fast reader: the bits of the byte
 * it is in that are not yet read go into the cache first. */
static inline void dst_fast(dst_dec_t *d)
{
    const uint32_t used = d->pos & 7u;
    d->next = d->pos >> 3;
    d->cache = 0;
    d->have = 0;
    if (used) {
        const uint32_t b = d->next < d->bytes ? d->p[d->next] : 0;
        d->next++;
        d->cache = (b << (24 + used));
        d->have = 8 - used;
    }
}

/* One arithmetic-coded event at probability p/256 of a zero. The
 * renormalisation takes all its bits from the cache at once. */
static inline uint32_t dst_ac(dst_dec_t *d, uint32_t p)
{
    const uint32_t q = dst_ac_k(d->a) * p, rest = d->a - q;
    uint32_t e, a, c = d->c;
    if (c < rest) { a = rest; e = 1; }
    else          { a = q; c -= rest; e = 0; }
    if (a < 2048) {
        uint32_t n = 0;
        while (a < 2048) { a <<= 1; n++; }
        c = c << n | dst_take(d, n);
    }
    d->a = a;
    d->c = c;
    return e;
}

static inline uint8_t dst_rev7(uint32_t c)
{
    uint32_t r = 0;
    for (int i = 0; i < 7; i++) r |= ((c >> i) & 1u) << (6 - i);
    return (uint8_t)r;
}

/*
 * Start a frame: `p`, `bytes` (kept, not copied, until the frame is
 * done), `ch` channels, `mult` 1 for DSD64. False on anything this does
 * not take; the frame's audio is then unknown and the caller drops it.
 */
static inline bool dst_begin(dst_dec_t *d, const uint8_t *p, uint32_t bytes,
                             uint32_t ch, uint32_t mult)
{
    static const int8_t wf[3][3] = { { -8 }, { -16, 8 }, { -9, -5, 6 } };
    static const int8_t wp[3][3] = { { -8 }, { -16, 8 }, { -24, 24, -8 } };

    if (!ch || ch > DST_MAX_CH || bytes < 2) return false;
    d->p = p;
    d->bytes = bytes;
    d->pos = 0;
    d->ch = ch;
    d->frame_bits = 588u * 64u * mult;
    d->done = 0;

    if (!dst_bit(d)) {
        d->plain = true;
        dst_bit(d);
        return dst_bits(d, 6) == 0;
    }
    d->plain = false;
    if (dst_bits(d, 3) != 7) return false;          /* one segment, everywhere */

    const bool same = dst_bit(d);
    if (!dst_map(d, d->felem, &d->nf)) return false;
    if (same) {
        d->np = d->nf;
        memcpy(d->pelem, d->felem, sizeof d->pelem);
    } else if (!dst_map(d, d->pelem, &d->np)) {
        return false;
    }
    for (uint32_t c = 0; c < ch; c++) d->half[c] = dst_bit(d);

    int32_t v[DST_MAX_TAPS];
    for (uint32_t e = 0; e < d->nf; e++) {
        d->flen[e] = dst_bits(d, 7) + 1;
        if (!dst_set(d, v, d->flen[e], 9, true, 0, wf)) return false;
        for (uint32_t i = 0; i < d->flen[e]; i++) {
            if (v[i] < INT16_MIN || v[i] > INT16_MAX) return false;
            d->coef[e][i] = (int16_t)v[i];
        }
    }
    for (uint32_t e = 0; e < d->np; e++) {
        d->plen[e] = dst_bits(d, 6) + 1;
        if (!dst_set(d, v, d->plen[e], 7, false, 1, wp)) return false;
        for (uint32_t i = 0; i < d->plen[e]; i++) d->prob[e][i] = (uint8_t)v[i];
    }
    if (dst_bit(d)) return false;

    /* the filters as lookups: entry k of group g is the sum over the 8
     * taps of g of +coef where bit l of k is 1, -coef where it is 0.
     * Built up a bit at a time -- entry k | 1<<l is entry k plus twice
     * tap l -- 256 adds a group, not 2048 (0017). */
    for (uint32_t e = 0; e < d->nf; e++) {
        for (uint32_t g = 0; g < 16; g++) {
            const int32_t n0 = (int32_t)d->flen[e] - (int32_t)g * 8;
            const uint32_t n = n0 < 0 ? 0 : n0 > 8 ? 8 : (uint32_t)n0;
            int32_t t[256];
            t[0] = 0;
            for (uint32_t l = 0; l < n; l++) t[0] -= d->coef[e][g * 8 + l];
            for (uint32_t l = 0; l < 8; l++) {
                const int32_t two = l < n ? 2 * d->coef[e][g * 8 + l] : 0;
                for (uint32_t k = 0; k < (1u << l); k++) t[k | 1u << l] = t[k] + two;
            }
            for (uint32_t k = 0; k < 256; k++) {
                if (t[k] < INT16_MIN || t[k] > INT16_MAX) return false;
                d->lut[e][g][k] = (int16_t)t[k];
            }
        }
    }

    d->a = 4095;
    d->c = dst_bits(d, 12);
    dst_fast(d);
    for (uint32_t c = 0; c < ch; c++) {
        d->hist[c][0] = d->hist[c][1] = d->hist[c][2] = d->hist[c][3] = 0xAAAAAAAAu;
    }
    (void)dst_ac(d, dst_rev7((uint32_t)d->coef[0][0] & 127u) + 1u);  /* a reserved bit */
    return true;
}

/* Bytes a channel still to come from this frame. */
static inline uint32_t dst_left_bytes(const dst_dec_t *d)
{
    return (d->frame_bits - d->done) / 8u;
}

/*
 * The next `nb` bytes a channel of the frame (at most what is left) into
 * `out`, channels interleaved a byte at a time, MSB first: DFF's layout.
 * Returns the bytes a channel written.
 */
static inline uint32_t dst_run(dst_dec_t *d, uint8_t *out, uint32_t nb)
{
    const uint32_t left = dst_left_bytes(d);
    if (nb > left) nb = left;
    const uint32_t ch = d->ch;

    if (d->plain) {
        /* stored bytes from byte 1, the short end filled with silence */
        const uint32_t from = 1u + d->done / 8u * ch, want = nb * ch;
        uint32_t have = from < d->bytes ? d->bytes - from : 0;
        if (have > want) have = want;
        memcpy(out, d->p + from, have);
        memset(out + have, 0x69, want - have);
        d->done += nb * 8u;
        return nb;
    }

    for (uint32_t b = 0; b < nb; b++) {
        uint8_t acc[DST_MAX_CH] = { 0 };
        for (uint32_t bit = 0; bit < 8; bit++) {
            const uint32_t i = d->done + bit;
            for (uint32_t c = 0; c < ch; c++) {
                const uint32_t fe = d->felem[c];
                const int16_t (*t)[256] = (const int16_t (*)[256])d->lut[fe];
                uint32_t *h = d->hist[c];
                const uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3];
#define DST_G(g, w) t[g][((w) >> (8 * ((g) & 3))) & 0xFFu]
                const int32_t s =
                    DST_G(0, h0) + DST_G(1, h0) + DST_G(2, h0) + DST_G(3, h0) +
                    DST_G(4, h1) + DST_G(5, h1) + DST_G(6, h1) + DST_G(7, h1) +
                    DST_G(8, h2) + DST_G(9, h2) + DST_G(10, h2) + DST_G(11, h2) +
                    DST_G(12, h3) + DST_G(13, h3) + DST_G(14, h3) + DST_G(15, h3);
#undef DST_G
                const int16_t pred = (int16_t)s;

                uint32_t p;
                if (d->half[c] && i < d->flen[fe]) {
                    p = 128;
                } else {
                    const uint32_t pe = d->pelem[c];
                    uint32_t ix = (uint32_t)(pred < 0 ? -(int32_t)pred : pred) >> 3;
                    if (ix > d->plen[pe] - 1) ix = d->plen[pe] - 1;
                    p = d->prob[pe][ix];
                }
                const uint32_t v = (pred < 0 ? 1u : 0u) ^ dst_ac(d, p);
                acc[c] |= (uint8_t)(v << (7 - bit));
                h[3] = h3 << 1 | h2 >> 31;
                h[2] = h2 << 1 | h1 >> 31;
                h[1] = h1 << 1 | h0 >> 31;
                h[0] = h0 << 1 | v;
            }
        }
        d->done += 8;
        for (uint32_t c = 0; c < ch; c++) out[b * ch + c] = acc[c];
    }
    return nb;
}

#ifdef __cplusplus
}
#endif

#endif /* DST_H */
