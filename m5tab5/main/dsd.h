/*
 * dsd.h -- DSF and DFF (Direct Stream Digital), found, described and
 * converted to 16-bit PCM.
 *
 * WHY THIS IS CHEAP ENOUGH
 *
 * The README says "DSD and APE require too much processing". For APE
 * that is true. For DSD it is true only of a naive converter, which
 * treats the stream as 2.8 million samples a second and runs a long
 * filter over every one. A bit has two values, so a filter over bits is
 * a sum of +h or -h, and eight bits at a time is one table lookup:
 * tbl[k][byte] already holds the contribution of byte k of the filter's
 * window. A 128-tap filter is 16 lookups per output, and the output of
 * that stage is not 44.1 kHz but 176.4 kHz, so the lookups are spread
 * over 16 input bits per output, not 64.
 *
 *   stage 1  bits -> float at 176.4 kHz. Windowed sinc, Kaiser beta ~10,
 *            128 taps (DSD64) or 256 (DSD128), by table lookup. It only
 *            has to keep 0-22 kHz and reject what would fold into it,
 *            which starts at 176.4 - 22 = 154 kHz, so it can be short.
 *   stage 2  halfband, 176.4 -> 88.2 kHz, 31 taps, 16 of them nonzero.
 *   stage 3  halfband, 88.2 -> 44.1 kHz, 135 taps, 68 of them nonzero.
 *            Flat to 20 kHz, down >90 dB from 24 kHz.
 *
 * About 55 float multiply-adds per output frame per channel for stages
 * 2 and 3, plus 32 (DSD64) or 64 (DSD128) lookups for stage 1.
 *
 * The output is always 44.1 kHz. DSD64 divides by 64, DSD128 by 128, DSD256 by 256.
 * Unity gain: full-scale modulation is full-scale PCM, saturated. Real
 * DSD stays under about half modulation, which is why this does not
 * attenuate.
 *
 * WHAT IS REFUSED, AND SAYS SO
 *
 *   - DST-compressed DFF (a different codec; there is none here).
 *   - Rates that are not 44.1 kHz * 64 * 2^n, or are above DSD_MAX_MULT (DSD256).
 *   - More than two channels.
 *   - DSF with a format id other than 0 (raw DSD).
 *
 * DSF: little-endian chunks. `DSD ` (28 bytes), `fmt ` (52), `data`.
 * Audio is in blocks of block_size bytes per channel, channels taking
 * turns, bits LSB-first or MSB-first as `fmt` says. The last block is
 * padded; the sample count says where the music stops.
 *
 * DFF: big-endian, 64-bit chunk sizes, even-padded. FRM8 holding FVER,
 * PROP (FS, CHNL, CMPR, ...), and the audio in a `DSD ` chunk (or `DST `
 * when compressed). Bytes interleave per channel, MSB first.
 *
 * Reads go through storage_io_read_at(), as aiff.h's do.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DSD_H
#define DSD_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_io.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation: the decoder defines DSD_ALLOC to PSRAM. The table is read
 * at random, 16 to 48 times an output frame, which the cache handles;
 * internal RAM belongs to the radio (CLAUDE.md). Everything goes through
 * it, and everything is released with free(). */

/* Highest DSD multiple taken: 1 = DSD64, 2 = DSD128, 4 = DSD256.
 * DSD256 has the lookups per output DSD128 had before the window was
 * halved (48 here, see DSD_K_BYTES) and a 48 KB table. 1 or 2 takes
 * the faster rates away again. */
#ifndef DSD_MAX_MULT
#define DSD_MAX_MULT 4
#endif

#define DSD_BASE_RATE   2822400u        /* 44100 * 64 */
#define DSD_OUT_RATE    44100u
#define DSD_BLOCK_MAX   4096u           /* DSF's block size, bytes per channel */
#define DSD_IDLE        0x69            /* the standard silence pattern */

typedef enum {
    DSD_OK = 0,
    DSD_NOT_DSD,        /* not a DSF or DFF at all */
    DSD_DST,            /* DFF, DST-compressed */
    DSD_UNSUPPORTED,    /* a DSD, but a shape this does not take */
} dsd_status_t;

typedef struct {
    int64_t  data_start;    /* first audio byte */
    uint64_t bits;          /* per channel: where the music stops */
    uint32_t rate;          /* the 1-bit rate, Hz */
    uint16_t channels;      /* 1 or 2 */
    bool     lsb_first;     /* DSF may be either; DFF is MSB first */
    bool     dff;           /* byte-interleaved, not block-interleaved */
    uint32_t block;         /* bytes per channel per block (DSF); 0 for DFF */
    uint64_t data_bytes;    /* all channels, as the chunk states, cut to the file */
    char     kind[4];       /* "DSF" or "DFF" */
} dsd_t;

static inline uint32_t dsd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static inline uint64_t dsd_le64(const uint8_t *p)
{
    return (uint64_t)dsd_le32(p) | (uint64_t)dsd_le32(p + 4) << 32;
}
static inline uint32_t dsd_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint64_t dsd_be64(const uint8_t *p)
{
    return (uint64_t)dsd_be32(p) << 32 | dsd_be32(p + 4);
}

/* 44100 * 64 * 2^n up to DSD_MAX_MULT, as a ratio to the base rate;
 * 0 for anything else. */
static inline uint32_t dsd_mult(uint32_t rate)
{
    for (uint32_t m = 1; m <= DSD_MAX_MULT; m *= 2) {
        if (rate == DSD_BASE_RATE * m) return m;
    }
    return 0;
}

static inline dsd_status_t dsd_probe_dsf(FILE *f, int64_t size, dsd_t *d,
                                         storage_io_class_t cls)
{
    uint8_t h[28 + 52 + 12];
    if (size < (int64_t)sizeof(h) || !storage_io_read_at(f, 0, h, sizeof(h), cls)) {
        return DSD_NOT_DSD;
    }
    if (memcmp(h, "DSD ", 4) || dsd_le64(h + 4) != 28) return DSD_NOT_DSD;
    const uint8_t *m = h + 28;
    if (memcmp(m, "fmt ", 4) || dsd_le64(m + 4) != 52) return DSD_NOT_DSD;
    const uint32_t fmtid = dsd_le32(m + 16), chans = dsd_le32(m + 24),
                   rate = dsd_le32(m + 28), bps = dsd_le32(m + 32),
                   block = dsd_le32(m + 44);
    const uint64_t samples = dsd_le64(m + 36);
    if (fmtid != 0 || (bps != 1 && bps != 8) || chans < 1 || chans > 2 ||
        !dsd_mult(rate) || block == 0 || block > DSD_BLOCK_MAX ||
        (block & (block - 1)) || block % (8 * dsd_mult(rate))) {
        return DSD_UNSUPPORTED;
    }
    const uint8_t *c = h + 80;
    if (memcmp(c, "data", 4)) return DSD_NOT_DSD;
    d->kind[0] = 'D'; d->kind[1] = 'S'; d->kind[2] = 'F'; d->kind[3] = 0;
    d->dff = false;
    d->lsb_first = bps == 1;
    d->channels = (uint16_t)chans;
    d->rate = rate;
    d->block = block;
    d->data_start = 92;
    uint64_t len = dsd_le64(c + 4);
    len = len >= 12 ? len - 12 : 0;
    if (d->data_start + (int64_t)len > size) len = (uint64_t)(size - d->data_start);
    d->data_bytes = len;
    /* The count in the header, but never more than the file holds. */
    const uint64_t have = len / chans * 8;
    d->bits = (samples && samples < have) ? samples : have;
    return d->bits ? DSD_OK : DSD_UNSUPPORTED;
}

static inline dsd_status_t dsd_probe_dff(FILE *f, int64_t size, dsd_t *d,
                                         storage_io_class_t cls)
{
    uint8_t h[16];
    if (size < 16 || !storage_io_read_at(f, 0, h, 16, cls)) return DSD_NOT_DSD;
    if (memcmp(h, "FRM8", 4) || memcmp(h + 12, "DSD ", 4)) return DSD_NOT_DSD;

    uint32_t rate = 0, chans = 0;
    bool dst = false;
    int64_t pos = 16;
    while (pos + 12 <= size) {
        uint8_t c[12];
        if (!storage_io_read_at(f, pos, c, 12, cls)) break;
        const uint64_t len = dsd_be64(c + 4);
        if (len > (uint64_t)(size - pos - 12) && memcmp(c, "DSD ", 4) &&
            memcmp(c, "DST ", 4)) {
            break;
        }
        if (!memcmp(c, "PROP", 4)) {
            int64_t p = pos + 12 + 4;                   /* past 'SND ' */
            const int64_t end = pos + 12 + (int64_t)len;
            while (p + 12 <= end && p + 12 <= size) {
                uint8_t s[12 + 16];
                if (!storage_io_read_at(f, p, s, 12, cls)) break;
                const uint64_t sl = dsd_be64(s + 4);
                if (!memcmp(s, "FS  ", 4) && sl >= 4) {
                    uint8_t v[4];
                    if (storage_io_read_at(f, p + 12, v, 4, cls)) rate = dsd_be32(v);
                } else if (!memcmp(s, "CHNL", 4) && sl >= 2) {
                    uint8_t v[2];
                    if (storage_io_read_at(f, p + 12, v, 2, cls)) chans = (uint32_t)(v[0] << 8 | v[1]);
                } else if (!memcmp(s, "CMPR", 4) && sl >= 4) {
                    uint8_t v[4];
                    if (storage_io_read_at(f, p + 12, v, 4, cls) && memcmp(v, "DSD ", 4)) dst = true;
                }
                p += 12 + (int64_t)sl + (int64_t)(sl & 1);
            }
        } else if (!memcmp(c, "DST ", 4)) {
            return DSD_DST;
        } else if (!memcmp(c, "DSD ", 4)) {
            if (dst) return DSD_DST;
            if (chans < 1 || chans > 2 || !dsd_mult(rate)) return DSD_UNSUPPORTED;
            d->kind[0] = 'D'; d->kind[1] = 'F'; d->kind[2] = 'F'; d->kind[3] = 0;
            d->dff = true;
            d->lsb_first = false;
            d->channels = (uint16_t)chans;
            d->rate = rate;
            d->block = 0;
            d->data_start = pos + 12;
            uint64_t dl = len;
            if (d->data_start + (int64_t)dl > size || !dl || dl == ~0ull) {
                dl = (uint64_t)(size - d->data_start);
            }
            d->data_bytes = dl;
            d->bits = dl / chans * 8;
            return d->bits ? DSD_OK : DSD_UNSUPPORTED;
        }
        pos += 12 + (int64_t)len + (int64_t)(len & 1);
    }
    if (dst) return DSD_DST;
    return DSD_NOT_DSD;
}

static inline dsd_status_t dsd_probe(FILE *f, int64_t size, dsd_t *d,
                                     storage_io_class_t cls)
{
    memset(d, 0, sizeof(*d));
    uint8_t m[4];
    if (size < 16 || !storage_io_read_at(f, 0, m, 4, cls)) return DSD_NOT_DSD;
    if (!memcmp(m, "DSD ", 4)) return dsd_probe_dsf(f, size, d, cls);
    if (!memcmp(m, "FRM8", 4)) return dsd_probe_dff(f, size, d, cls);
    return DSD_NOT_DSD;
}

/* ------------------------------------------------------------------ */
/* Conversion                                                          */
/* ------------------------------------------------------------------ */

/* A microsecond clock, for finding out where the time goes on the board.
 * decoder.c defines it; left alone, nothing is measured. */
#ifndef DSD_CLOCK
#define DSD_CLOCK() ((uint64_t)0)
#endif

#ifndef DSD_ALLOC
#define DSD_ALLOC(n) malloc(n)
#endif

/* Stage-1 window, in bytes per DSD64-equivalent unit: K = DSD_KB * mult.
 * 16 (128 taps at DSD64) is the shortest that still leaves the chain's
 * error against a 64-byte window at -126 dB re full scale (0.015 LSB of
 * 16 bits) on the DSD64 test file and -143 dB at DSD128; 12 is at -88 dB.
 * It used to be 32. Stage 1 is nearly all of the conversion's time, so
 * this halves it. The lookups' count is the cost; K must be a multiple
 * of 4. */
#ifndef DSD_KB
#define DSD_KB      16
#endif
/* The window in bytes for a rate multiple. DSD256 gets 12 per unit, 48
 * bytes: against a 128-byte window the chain's error on file 43 is
 * -154 dB at 16 (64 bytes), -116 dB (0.05 LSB of 16 bits) at 12, and
 * -97 dB (0.5 LSB) at 8. A quarter fewer lookups than 16 is worth the
 * margin that is left. Measured offline on one file, not on a board. */
#ifndef DSD_K_BYTES
#define DSD_K_BYTES(mult) ((mult) >= 4u ? 12u * (mult) : (uint32_t)DSD_KB * (mult))
#endif

#define DSD_HB1_H   15      /* 31 taps: 176.4 -> 88.2 kHz */
#define DSD_HB2_H   67      /* 135 taps: 88.2 -> 44.1 kHz */

typedef struct {
    float *x;               /* carry, then the new samples */
    int    n;               /* samples held */
} dsd_hb_state_t;

typedef struct {
    uint8_t        *bh;     /* byte history + new bytes, MSB-of-time order */
    int             bn;
    dsd_hb_state_t  s1, s2;
} dsd_ch_t;

/* ------------------------------------------------------------------ */
/* Stage 1, in integers                                                */
/* ------------------------------------------------------------------ */
/*
 * The window is K bytes, S1 bytes to an output, so a byte takes part in
 * Mr = K / S1 <= 8 outputs, each time at a different row of the window.
 * Per byte value v and position s within an S1-byte group the table holds
 * those Mr contributions side by side, eight int32 lanes (the spare lanes
 * zero): tbl[(v * S1 + s) * 8 + m] is what byte (q * S1 + s) = v adds to
 * output q - m. A group's vector V_q is the sum of S1 such entries, and
 * output j is lane m of V_{j+m} summed over m. Scale 2^28: the window's
 * absolute sum stays under 2, so the int32 sums do not overflow, and an
 * entry's rounding is 2^-29, below a float's.
 */
#define DSD_TBL_SCALE   268435456.0f        /* 2^28 */

/* V[0..8) = sum over s of T[(b[s] * S1 + s) * 8 + lane], wrapping. */
static inline void dsd_grp_c(const uint8_t *b, const int32_t *T, uint32_t S1,
                             int32_t *V)
{
    uint32_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (uint32_t s = 0; s < S1; s++) {
        const int32_t *e = T + ((size_t)b[s] * S1 + s) * 8;
        for (int m = 0; m < 8; m++) a[m] += (uint32_t)e[m];
    }
    for (int m = 0; m < 8; m++) V[m] = (int32_t)a[m];
}

#if defined(DSD_PIE_MODEL)
/* The host's stand-in for the vector kernel: the same sums. */
static inline void dsd_grp_pie(const uint8_t *b, const int32_t *T, uint32_t S1,
                               int32_t *V)
{
    dsd_grp_c(b, T, S1, V);
}
#define DSD_HAVE_PIE 1
#elif defined(DSD_PIE) && defined(__riscv)
/*
 * The ESP32-P4's PIE does the group sum: per byte, one scalar load, the
 * address (table + ((v * S1 + s) << 5)), two 16-byte vector loads and two
 * four-lane int32 adds, against Mr * 4 instructions of lookups. q0/q1 are
 * the sum, q2/q3 the entry. The first byte loads straight into the sum
 * (S1 >= 2, so the loop runs at least once). PIE reaches a0-a5 among the
 * registers used here: a0 bytes, a1 the entry's s-th slot, a2 count, a3
 * the address, a4 the result, a5 the shift 5 + log2(S1). esp.vadd.s32
 * saturates at the int32 limits where the C code wraps; the sums stay far
 * inside them.
 */
static inline void dsd_grp_pie(const uint8_t *b, const int32_t *T, uint32_t S1,
                               int32_t *V)
{
    /* 5 + log2(S1), without __builtin_ctz(): on this core that is a call
     * into libgcc, and a call between the register variables' setup and the
     * asm clobbers a0-a5 (8117 crashed on it, a load from address 1). Worked
     * out first, so nothing is left to call once the registers are set. */
    const uint32_t sh = S1 == 2 ? 6u : (S1 == 4 ? 7u : 8u);
    register const uint8_t *r0 __asm__("a0") = b;
    register const int32_t *r1 __asm__("a1") = T;
    register uint32_t r2 __asm__("a2") = S1 - 1;
    register uint32_t r3 __asm__("a3");
    register int32_t *r4 __asm__("a4") = V;
    register uint32_t r5 __asm__("a5") = sh;
    __asm__ volatile(
        ".option push\n .option arch, +xesppie\n .option norvc\n"
        "lbu a3, 0(a0)\n"
        "addi a0, a0, 1\n"
        "sll a3, a3, a5\n"
        "add a3, a3, a1\n"
        "addi a1, a1, 32\n"
        "esp.vld.128.ip q0, a3, 16\n"
        "esp.vld.128.ip q1, a3, 0\n"
        "1:\n"
        "lbu a3, 0(a0)\n"
        "addi a0, a0, 1\n"
        "sll a3, a3, a5\n"
        "add a3, a3, a1\n"
        "addi a1, a1, 32\n"
        "esp.vld.128.ip q2, a3, 16\n"
        "esp.vld.128.ip q3, a3, 0\n"
        "addi a2, a2, -1\n"
        "esp.vadd.s32 q0, q0, q2\n"
        "esp.vadd.s32 q1, q1, q3\n"
        "bnez a2, 1b\n"
        "esp.vst.128.ip q0, a4, 16\n"
        "esp.vst.128.ip q1, a4, 0\n"
        ".option pop\n"
        : "+r"(r0), "+r"(r1), "+r"(r2), "=r"(r3), "+r"(r4)
        : "r"(r5)
        : "memory");
}
#define DSD_HAVE_PIE 1
#endif

#ifdef DSD_HAVE_PIE
static bool dsd_pie_on = true;      /* false after a failed self-test */
static inline void dsd_grp(const uint8_t *b, const int32_t *T, uint32_t S1,
                           int32_t *V)
{
    if (dsd_pie_on) dsd_grp_pie(b, T, S1, V);
    else dsd_grp_c(b, T, S1, V);
}

/* The vector kernel against the C one on random tables and bytes, at every
 * hop there is; once, from the first dsd_conv_new(). A mismatch turns the
 * vector kernel off (dsd_pie_on stays false) and the C one carries on. */
static inline bool dsd_pie_selftest(void)
{
    static bool done = false;
    if (done) return dsd_pie_on;
    done = true;
    static int32_t traw[256 * 8 * 8 + 4];
    int32_t *T = (int32_t *)(((uintptr_t)traw + 15) & ~(uintptr_t)15);
    int32_t vraw[2 * 8 + 4];
    int32_t *va = (int32_t *)(((uintptr_t)vraw + 15) & ~(uintptr_t)15), *vb = va + 8;
    uint32_t r = 2463534242u;
    for (int i = 0; i < 256 * 8 * 8; i++) {
        r = r * 1664525u + 1013904223u;
        T[i] = (int32_t)(r >> 3) - (1 << 28);       /* about +-2^28 */
    }
    bool ok = true;
    for (uint32_t S1 = 2; S1 <= 8 && ok; S1 *= 2) {
        for (int t = 0; t < 200 && ok; t++) {
            uint8_t b[8];
            for (uint32_t i = 0; i < S1; i++) { r = r * 1664525u + 1013904223u; b[i] = (uint8_t)(r >> 24); }
            dsd_grp_c(b, T, S1, va);
            dsd_grp_pie(b, T, S1, vb);
            if (memcmp(va, vb, 8 * sizeof(int32_t)) != 0) ok = false;
        }
    }
    dsd_pie_on = ok;
    return ok;
}
#else
#define dsd_grp dsd_grp_c
static inline bool dsd_pie_selftest(void) { return false; }
#endif

typedef struct {
    dsd_t     d;
    uint32_t  S1;           /* stage-1 hop in bytes: rate / 176400 / 8 */
    uint32_t  K;            /* stage-1 window in bytes */
    int32_t  *tbl;          /* [byte value][S1][8 lanes], 16-byte aligned */
    void     *tbl_raw;      /* what DSD_ALLOC returned */
    int32_t   vring_raw[8 * 8 + 4];     /* see dsd_vring() */
    float     hb1[(DSD_HB1_H + 1) / 2];     /* taps at offsets 1,3,5.. */
    float     hb2[(DSD_HB2_H + 1) / 2];
    dsd_ch_t  ch[2];
    uint32_t  frame_bytes;  /* input bytes per channel per output frame */
    uint32_t  latency;      /* output frames the filters delay the signal */
    /* Scratch for dsd_conv_feed(), here and not on the decode task's stack. */
    float     mid[1088], fin[576];
    /* Where dsd_next() spent its time (DSD_CLOCK units, cumulative over
     * seeks), and the frames the filters produced, preroll included. */
    uint64_t  us_read, us_feed, frames_fed, reads;
    uint32_t  us_read_max, slow_reads;      /* the longest read; reads over 5 ms */
    uint32_t  first_us[16];                 /* the first reads after a seek plan */
    uint32_t  first_n;
} dsd_conv_t;

static inline double dsd_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

/* Kaiser window value at position n of N taps (n from 0). */
static inline double dsd_kaiser(int n, int N, double beta)
{
    const double r = 2.0 * n / (N - 1) - 1.0;
    return dsd_i0(beta * sqrt(1.0 - r * r)) / dsd_i0(beta);
}

/* Halfband: h[n] = sin(pi n / 2) / (pi n), centre 0.5, Kaiser-windowed.
 * out[i] is the tap at offset 2i+1 from the centre. */
static inline void dsd_halfband(float *out, int H, double beta)
{
    const int N = 2 * H + 1;
    for (int i = 0; i < (H + 1) / 2; i++) {
        const int off = 2 * i + 1;
        const double ideal = ((i & 1) ? -1.0 : 1.0) / (M_PI * off);
        out[i] = (float)(ideal * dsd_kaiser(H + off, N, beta));
    }
}

static inline void dsd_conv_free(dsd_conv_t *c)
{
    if (!c) return;
    for (int i = 0; i < 2; i++) {
        free(c->ch[i].bh);
        free(c->ch[i].s1.x);
        free(c->ch[i].s2.x);
    }
    free(c->tbl_raw);
    free(c);
}

/* Back to silence, for the start of a file and after a seek. */
static inline void dsd_conv_reset(dsd_conv_t *c)
{
    for (int i = 0; i < c->d.channels; i++) {
        memset(c->ch[i].bh, DSD_IDLE, c->K / 2 + 3 * c->S1);
        c->ch[i].bn = (int)(c->K / 2 + 3 * c->S1);
        memset(c->ch[i].s1.x, 0, sizeof(float) * (2 * DSD_HB1_H));
        c->ch[i].s1.n = 2 * DSD_HB1_H;
        memset(c->ch[i].s2.x, 0, sizeof(float) * (2 * DSD_HB2_H));
        c->ch[i].s2.n = 2 * DSD_HB2_H;
    }
}

/* `in_chunk` is the largest nbytes (per channel) dsd_conv_feed will see. */
static inline dsd_conv_t *dsd_conv_new(const dsd_t *d, uint32_t in_chunk)
{
    const uint32_t mult = dsd_mult(d->rate);
    if (!mult || d->channels < 1 || d->channels > 2) return NULL;
    dsd_conv_t *c = (dsd_conv_t *)DSD_ALLOC(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->d = *d;
    c->S1 = 2u * mult;                          /* bytes per 176.4 kHz sample */
    c->K = DSD_K_BYTES(mult);
    c->frame_bytes = 8u * mult;
    if (c->K < 6u * c->S1 || c->K > 8u * c->S1 || c->K % c->S1) {
        /* dsd_conv_reset() wants 6 hops, the table's lanes allow 8 */
        free(c);
        return NULL;
    }
    const uint32_t taps1 = c->K * 8u;

    /* Stage 1: windowed sinc at the DSD rate, cut at 88.2 kHz. */
    float *h = (float *)DSD_ALLOC(sizeof(float) * taps1);
    const size_t tbl_n = (size_t)256 * c->S1 * 8;
    c->tbl_raw = DSD_ALLOC(sizeof(int32_t) * tbl_n + 16);
    if (!h || !c->tbl_raw) {
        free(h);
        dsd_conv_free(c);
        return NULL;
    }
    c->tbl = (int32_t *)(((uintptr_t)c->tbl_raw + 15) & ~(uintptr_t)15);
    const double fc = 88200.0 / (double)d->rate;
    double sum = 0;
    for (uint32_t n = 0; n < taps1; n++) {
        const double t = (double)n - (taps1 - 1) / 2.0;
        const double sinc = fabs(t) < 1e-9 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        const double v = sinc * dsd_kaiser((int)n, (int)taps1, 10.0);
        h[n] = (float)v;
        sum += v;
    }
    for (uint32_t n = 0; n < taps1; n++) h[n] = (float)(h[n] / sum);
    /* The window's byte `row` is (v, s, m) with row = s + m * S1 (see
     * the block comment at dsd_grp_c). A set bit is +1. MSB-first files
     * hold the oldest bit in bit 7, LSB-first in bit 0; the window is
     * oldest-first either way. */
    const uint32_t Mr = c->K / c->S1;
    for (int v = 0; v < 256; v++) {
        for (uint32_t s = 0; s < c->S1; s++) {
            for (uint32_t m = 0; m < 8; m++) {
                int32_t e = 0;
                if (m < Mr) {
                    const uint32_t row = s + m * c->S1;
                    double a = 0;
                    for (int i = 0; i < 8; i++) {
                        const int t = d->lsb_first ? i : 7 - i;
                        const double w = h[row * 8 + (uint32_t)t];
                        a += ((v >> i) & 1) ? w : -w;
                    }
                    /* Round half away from zero with a cast: lrint() is
                     * not in the feckless ABI. */
                    const double x = a * (double)DSD_TBL_SCALE;
                    e = (int32_t)(x < 0 ? x - 0.5 : x + 0.5);
                }
                c->tbl[((size_t)v * c->S1 + s) * 8 + m] = e;
            }
        }
    }
    free(h);

    (void)dsd_pie_selftest();
    dsd_halfband(c->hb1, DSD_HB1_H, 9.65);
    dsd_halfband(c->hb2, DSD_HB2_H, 9.65);

    for (int i = 0; i < d->channels; i++) {
        c->ch[i].bh = (uint8_t *)DSD_ALLOC(c->K + in_chunk + 8);
        c->ch[i].s1.x = (float *)DSD_ALLOC(sizeof(float) * (2 * DSD_HB1_H + in_chunk / c->S1 + 8));
        c->ch[i].s2.x = (float *)DSD_ALLOC(sizeof(float) * (2 * DSD_HB2_H + in_chunk / c->S1 / 2 + 8));
        if (!c->ch[i].bh || !c->ch[i].s1.x || !c->ch[i].s2.x) {
            dsd_conv_free(c);
            return NULL;
        }
    }
    /* Group delay in 176.4 kHz samples: stage 1, then the centres of the
     * two halfbands. Output m's window is centred on stream byte
     * m * S1 - 3 * S1 whatever K is (dsd_conv_reset() sets the history
     * so), which is three samples: K / 16 samples, as it was when the
     * history was K - S1 bytes, is a whole number of output frames only
     * when K is a multiple of 16 * mult, and anything else was a
     * fraction of a frame of time shift (found with K = 48 at DSD256:
     * 31 LSB against a reference that agreed to 0.3 with K = 64). */
    const double d176 = 3.0 + DSD_HB1_H + 2.0 * DSD_HB2_H;
    c->latency = (uint32_t)(d176 / 4.0 + 0.5);
    dsd_conv_reset(c);
    return c;
}

static inline int16_t dsd_sat(float v)
{
    v *= 32768.0f;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

/* One halfband decimate-by-2 stage over st->x[0..n), 2H+1 taps. */
static inline int dsd_hb_run(dsd_hb_state_t *st, const float *taps, int H,
                             float *out)
{
    const int N = 2 * H + 1;
    int pos = 0, no = 0;
    const int nt = (H + 1) / 2;
    while (pos + N <= st->n) {
        const float *x = st->x + pos;
        /* Four sums: with one the 34 multiply-adds of the second halfband
         * are a chain, each waiting for the one before. */
        float a = 0.5f * x[H], b = 0, c2 = 0, d = 0;
        int i = 0;
        for (; i + 3 < nt; i += 4) {
            const int o = 2 * i + 1;
            a  += taps[i]     * (x[H - o]     + x[H + o]);
            b  += taps[i + 1] * (x[H - o - 2] + x[H + o + 2]);
            c2 += taps[i + 2] * (x[H - o - 4] + x[H + o + 4]);
            d  += taps[i + 3] * (x[H - o - 6] + x[H + o + 6]);
        }
        for (; i < nt; i++) {
            const int o = 2 * i + 1;
            a += taps[i] * (x[H - o] + x[H + o]);
        }
        a = (a + b) + (c2 + d);
        out[no++] = a;
        pos += 2;
    }
    const int left = st->n - pos;
    memmove(st->x, st->x + pos, sizeof(float) * (size_t)left);
    st->n = left;
    return no;
}

/* The ring of group vectors, 16-byte aligned for the vector loads. */
static inline int32_t *dsd_vring(dsd_conv_t *c)
{
    return (int32_t *)(((uintptr_t)c->vring_raw + 15) & ~(uintptr_t)15);
}

/*
 * Convert `nbytes` per channel to 44.1 kHz interleaved int16. Channel c's
 * byte i is in[c * chan_off + i * stride]: DSF is (4096, 1), DFF is
 * (1, channels). `out` must hold nbytes / frame_bytes + 2 frames.
 * Returns frames written. `nbytes` should be a multiple of frame_bytes
 * except at the very end, or the phase of the output drifts by the
 * remainder -- the history carries it across calls, so it does not.
 */
static inline size_t dsd_conv_feed(dsd_conv_t *c, const uint8_t *in,
                                   size_t nbytes, size_t chan_off,
                                   size_t stride, int16_t *out)
{
    const int nch = c->d.channels;
    size_t frames = 0;
    for (int ch = 0; ch < nch; ch++) {
        dsd_ch_t *s = &c->ch[ch];
        const uint8_t *src = in + (size_t)ch * chan_off;
        for (size_t i = 0; i < nbytes; i++) s->bh[s->bn + (int)i] = src[i * stride];
        const int avail = s->bn + (int)nbytes;
        int pos = 0;
        /* Stage 1: one group vector per S1 bytes, one output per group
         * once Mr of them are in (see dsd_grp_c). The vectors are kept in
         * a ring of eight; the history bytes that carry over from the last
         * call are summed again, which is K - S1 bytes a call. */
        const uint32_t S1 = c->S1, Mr = c->K / c->S1;
        const int nq = avail / (int)S1;
        int32_t *ring = dsd_vring(c);
        float *x1 = s->s1.x + s->s1.n;
        for (int q = 0; q < nq; q++) {
            dsd_grp(s->bh + (size_t)q * S1, c->tbl, S1, ring + (q & 7) * 8);
            if ((uint32_t)q + 1 >= Mr) {
                const int j = q - (int)Mr + 1;
                uint32_t t = 0;
                for (uint32_t m = 0; m < Mr; m++)
                    t += (uint32_t)ring[((j + (int)m) & 7) * 8 + (int)m];
                *x1++ = (float)(int32_t)t * (1.0f / DSD_TBL_SCALE);
                pos += (int)S1;
            }
        }
        s->s1.n = (int)(x1 - s->s1.x);
        memmove(s->bh, s->bh + pos, (size_t)(avail - pos));
        s->bn = avail - pos;

        /* in_chunk <= 4096: at most 1024 stage-2 outputs, 512 stage-3, and a
         * little over for the carry's odd sample. */
        float *mid = c->mid, *fin = c->fin;
        const int n1 = dsd_hb_run(&s->s1, c->hb1, DSD_HB1_H, mid);
        for (int i = 0; i < n1; i++) s->s2.x[s->s2.n++] = mid[i];
        const int n2 = dsd_hb_run(&s->s2, c->hb2, DSD_HB2_H, fin);
        for (int i = 0; i < n2; i++) out[(size_t)i * (size_t)nch + (size_t)ch] = dsd_sat(fin[i]);
        frames = (size_t)n2;
    }
    return frames;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Output frames in the file: whole 44.1 kHz frames of music. */
static inline uint64_t dsd_frames(const dsd_conv_t *c)
{
    return c->d.bits / ((uint64_t)c->frame_bytes * 8u);
}

/* Where the reader is. Per-channel byte counts, not file offsets. */
typedef struct {
    uint64_t pos;       /* bytes per channel fed so far */
    uint64_t skip;      /* output frames still to discard */
    bool     flushed;   /* the idle tail that drains the filters is in */
    int64_t  at;        /* file offset the stream is positioned at, or -1 */
} dsd_rd_t;

/* Start at output frame `frame` (a seek, or 0 for the start). Resets the
 * filters. The read begins a little before the target so the filters
 * have settled by the time the first kept frame comes out, and the
 * frames before it -- and the filters' own delay -- are discarded by
 * dsd_next(), so the first frame returned is exactly `frame`. */
static inline void dsd_seek_plan(dsd_conv_t *c, dsd_rd_t *r, uint64_t frame)
{
    const uint64_t fb = c->frame_bytes;
    const uint64_t at = frame * fb;
    uint64_t start = at > 128 * fb ? at - 128 * fb : 0;
    if (!c->d.dff) start -= start % c->d.block;
    r->pos = start;
    r->skip = frame - start / fb + c->latency;
    r->flushed = false;
    r->at = -1;
    c->first_n = 0;
    dsd_conv_reset(c);
}

/* The file offset dsd_next() will read first after a seek plan, or -1 when
 * the plan is at the end. The same arithmetic as in dsd_next(). */
static inline int64_t dsd_rd_offset(const dsd_conv_t *c, const dsd_rd_t *r)
{
    const dsd_t *d = &c->d;
    if (r->pos >= (d->bits + 7) / 8) return -1;
    if (d->dff) return d->data_start + (int64_t)(r->pos * d->channels);
    return d->data_start + (int64_t)((r->pos / d->block) * d->block * d->channels);
}

/*
 * The next run of frames into `out` (room for DSD_BLOCK_MAX / frame_bytes
 * + 2 frames), 0 at the end. `buf` is channels * DSD_BLOCK_MAX bytes of
 * scratch. The caller stops at dsd_frames(): what follows the last whole
 * frame is the padding of the final block, and this does not hide it.
 */
static inline size_t dsd_next(dsd_conv_t *c, dsd_rd_t *r, FILE *f,
                              uint8_t *buf, int16_t *out,
                              storage_io_class_t cls)
{
    const dsd_t *d = &c->d;
    const uint64_t total = (d->bits + 7) / 8;
    for (;;) {
        size_t n;
        if (r->pos < total) {
            size_t nb = DSD_BLOCK_MAX;
            if (!d->dff && nb > d->block) nb = d->block;
            if (nb > total - r->pos) nb = (size_t)(total - r->pos);
            int64_t off;
            size_t want = nb * d->channels;
            if (d->dff) {
                off = d->data_start + (int64_t)(r->pos * d->channels);
            } else {
                const uint64_t blk = r->pos / d->block;
                off = d->data_start + (int64_t)(blk * d->block * d->channels);
                want = (size_t)d->block * d->channels;      /* a whole group */
            }
            const uint64_t t0 = DSD_CLOCK();
            /* Only seek when the stream is not already there. The next
             * block of a run is where the last one ended, and a seek
             * before every read -- which is what storage_io_read_at() is --
             * is a stdio fseek() per 4-8 KB on a handle whose buffer is 16
             * KB, for no reason: the other decoders read a run with
             * storage_io_fread() after one seek, and this does too. */
            if (r->at >= 0 && r->at < off) {
                /*
                 * The stream is behind the target (dsd_seek() reopened the
                 * file and left it at 0): read up to it and throw the bytes
                 * away instead of seeking, so that no lseek() is ever made
                 * on this stream. 8100/8101/8102's logs: a stream that is
                 * sought is slow to read from there on, whatever the
                 * offset's alignment and whether the FILE is new.
                 */
                const size_t cap = (size_t)d->channels * DSD_BLOCK_MAX;
                while (r->at < off) {
                    size_t n2 = cap;
                    if ((int64_t)n2 > off - r->at) n2 = (size_t)(off - r->at);
                    if (storage_io_fread(buf, n2, f, cls) != n2) return 0;
                    r->at += (int64_t)n2;
                }
            }
            if (r->at != off) {
                if (!storage_io_read_at(f, off, buf, want, cls)) return 0;
            } else if (storage_io_fread(buf, want, f, cls) != want) {
                return 0;
            }
            r->at = off + (int64_t)want;
            const uint64_t t1 = DSD_CLOCK();
            n = d->dff ? dsd_conv_feed(c, buf, nb, 1, d->channels, out)
                       : dsd_conv_feed(c, buf, nb, d->block, 1, out);
            c->us_read += t1 - t0;
            if (t1 - t0 > c->us_read_max) c->us_read_max = (uint32_t)(t1 - t0);
            if (t1 - t0 > 5000) c->slow_reads++;
            if (c->first_n < 16) c->first_us[c->first_n++] = (uint32_t)(t1 - t0);
            c->us_feed += DSD_CLOCK() - t1;
            c->frames_fed += n;
            c->reads++;
            r->pos += nb;
        } else if (!r->flushed) {
            const size_t nb = (size_t)(c->latency + 4) * c->frame_bytes;
            memset(buf, DSD_IDLE, nb * d->channels);
            n = dsd_conv_feed(c, buf, nb, nb, 1, out);
            r->flushed = true;
        } else {
            return 0;
        }
        if (r->skip) {
            const size_t s = r->skip < n ? (size_t)r->skip : n;
            memmove(out, out + s * d->channels,
                    (n - s) * d->channels * sizeof(int16_t));
            r->skip -= s;
            n -= s;
        }
        if (n) return n;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_H */
