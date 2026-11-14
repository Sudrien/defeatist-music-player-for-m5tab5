/* SPDX-License-Identifier: MIT
 *
 * boxwin_arith.c -- see boxwin_arith.h (u-defeatist-0010).
 *
 * No ESP-IDF in this file: test/host.c builds it on a host, where its
 * results are the reference.
 *
 * The operands are made from integers, by their bits, and every result
 * goes into the hash by its bits, so nothing here depends on how a double
 * is printed or parsed. A NaN is hashed as one fixed pattern: which NaN an
 * operation returns is the one thing IEEE 754 leaves to the machine. The
 * conversions to an integer are done only where C defines them (the value
 * in range), and no divisor is zero.
 */
#include <math.h>
#include <string.h>

#include "boxwin.h"
#include "boxwin_arith.h"

typedef union { double d; uint64_t u; } dbits;
static inline uint64_t bits_of(double x) { dbits b = { .d = x }; return b.u; }
static inline double dbl_of(uint64_t u) { dbits b = { .u = u }; return b.d; }

static uint64_t s_rng;
static uint64_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static inline uint32_t fnv64(uint32_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h = (h ^ (uint32_t)(v & 0xff)) * 16777619u;
        v >>= 8;
    }
    return h;
}

static inline uint32_t fnv_dbl(uint32_t h, double x)
{
    uint64_t u = bits_of(x);
    if ((u & 0x7fffffffffffffffull) > 0x7ff0000000000000ull) u = 0x7ff8000000000000ull;
    return fnv64(h, u);
}

/* PSRAM in a box: 128 KB of doubles and as much of integers. */
static double   s_a[BW_ARITH_N], s_b[BW_ARITH_N];
static uint64_t s_n[BW_ARITH_N], s_d[BW_ARITH_N];

static void fill(void)
{
    s_rng = 0x6a09e667f3bcc908ull;
    for (int i = 0; i < BW_ARITH_N; i++) {
        uint64_t a = rnd(), b = rnd();
        switch (i & 7) {
        case 0: case 1:                 /* any two bit patterns: NaNs, infinities, subnormals too */
            break;
        case 2: case 3: {               /* exponents close: sums that align, cancel and round */
            const uint64_t e = ((a >> 52) & 0x7ff) + (rnd() % 9) - 4;
            b = (b & 0x800fffffffffffffull) | ((e & 0x7ff) << 52);
            break;
        }
        case 4:                         /* the middle of the range, where nothing overflows */
            a = (a & 0x800fffffffffffffull) | ((uint64_t)(0x3ff - 40 + rnd() % 80) << 52);
            b = (b & 0x800fffffffffffffull) | ((uint64_t)(0x3ff - 40 + rnd() % 80) << 52);
            break;
        case 5:                         /* few significant bits: ties and exact results */
            a &= 0xfff0000000000000ull | (~0ull << (rnd() % 52));
            b &= 0xfff0000000000000ull | (~0ull << (rnd() % 52));
            break;
        case 6:                         /* around the bottom of the normal range */
            a = (a & 0x800fffffffffffffull) | ((rnd() % 60) << 52);
            b = (b & 0x800fffffffffffffull) | ((uint64_t)(0x3ff - 30 + rnd() % 60) << 52);
            break;
        default:                        /* around the top */
            a = (a & 0x800fffffffffffffull) | ((uint64_t)(0x7fe - rnd() % 60) << 52);
            b = (b & 0x800fffffffffffffull) | ((uint64_t)(0x3ff - 30 + rnd() % 60) << 52);
            break;
        }
        s_a[i] = dbl_of(a);
        s_b[i] = dbl_of(b);

        /* Dividends and divisors of every length against each other; one in
         * four divisors small, as a count of microseconds is divided. */
        /* One rnd() a statement, here and above: two in one expression are
         * called in whichever order the compiler likes, and a build that
         * liked the other had other operands and another hash. */
        uint64_t n = rnd();
        n >>= rnd() % 64;
        uint64_t d = rnd();
        d >>= rnd() % 64;
        if ((i & 3) == 3) d &= 0xffff;
        if (!d) d = 1000;
        if ((n == 0x8000000000000000ull && d == ~0ull)) n = 1;      /* undefined, signed */
        s_n[i] = n;
        s_d[i] = d;
    }
}

/* The time of the loop a kind of operation is timed in, with nothing in it. */
static uint32_t ns_each(int64_t us, int64_t loop_us)
{
    const int64_t net = us - loop_us;
    if (net <= 0 || net > 4000000) return 0;
    return (uint32_t)net * 1000u / BW_ARITH_N;      /* 32 bits: not one of the divisions timed */
}

void bw_arith(bw_arith_t *out)
{
    memset(out, 0, sizeof *out);
    fill();
    uint32_t h = 2166136261u;

    int64_t t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv64(h, bits_of(s_a[i]) ^ bits_of(s_b[i]));
    const int64_t loop = bw_now() - t0;

    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv_dbl(h, s_a[i] + s_b[i]);
    out->add_ns = ns_each(bw_now() - t0, loop);
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv_dbl(h, s_a[i] - s_b[i]);

    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv_dbl(h, s_a[i] * s_b[i]);
    out->mul_ns = ns_each(bw_now() - t0, loop);

    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv_dbl(h, s_a[i] / s_b[i]);
    out->div_ns = ns_each(bw_now() - t0, loop);

    /* The compares, as the six relations; a NaN makes all but != false. */
    for (int i = 0; i < BW_ARITH_N; i++) {
        const double a = s_a[i], b = s_b[i];
        h = fnv64(h, (uint64_t)((a < b) | (a <= b) << 1 | (a == b) << 2 | (a != b) << 3 |
                                (a > b) << 4 | (a >= b) << 5));
    }

    /* From integers of each width and sign. */
    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv_dbl(h, (double)(int64_t)s_n[i]);
    out->conv_ns = ns_each(bw_now() - t0, loop);
    for (int i = 0; i < BW_ARITH_N; i++) {
        h = fnv_dbl(h, (double)s_n[i]);
        h = fnv_dbl(h, (double)(int32_t)s_d[i]);
        h = fnv_dbl(h, (double)(uint32_t)s_d[i]);
    }

    /* To float and back, and to integers where the value fits: the exponent
     * says whether it does, read from the bits. */
    for (int i = 0; i < BW_ARITH_N; i++) {
        const double a = s_a[i];
        const int e = (int)((bits_of(a) >> 52) & 0x7ff) - 0x3ff;
        h = fnv_dbl(h, (double)(float)a);
        if (e < 31) h = fnv64(h, (uint64_t)(int64_t)(int32_t)a);
        if (e < 63) h = fnv64(h, (uint64_t)(int64_t)a);
        if (e < 32 && !(bits_of(a) >> 63)) h = fnv64(h, (uint64_t)(uint32_t)a);
        if (e < 64 && !(bits_of(a) >> 63)) h = fnv64(h, (uint64_t)a);
    }
    out->dbl_hash = h;

    /* 64-bit division. */
    h = 2166136261u;
    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv64(h, s_n[i] / s_d[i]);
    out->udiv_ns = ns_each(bw_now() - t0, loop);
    for (int i = 0; i < BW_ARITH_N; i++) {
        h = fnv64(h, s_n[i] % s_d[i]);
        h = fnv64(h, (uint64_t)((int64_t)s_n[i] / (int64_t)s_d[i]));
        h = fnv64(h, (uint64_t)((int64_t)s_n[i] % (int64_t)s_d[i]));
    }
    /* By a divisor the compiler cannot see, as esp_timer's microseconds are
     * divided by a rate or a period; and the remainder with it. */
    volatile uint64_t thousand = 1000;
    t0 = bw_now();
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv64(h, s_n[i] / thousand);
    out->udiv1000_ns = ns_each(bw_now() - t0, loop);
    for (int i = 0; i < BW_ARITH_N; i++) h = fnv64(h, s_n[i] % thousand);
    out->div_hash = h;
}

/* ---- u-defeatist-0013: the double libm ----------------------------------- */

static double s_ma[BW_MATHS_N], s_mb[BW_MATHS_N];

/* Arguments where the decoders use these -- a few hundred either way of
 * zero, positive for the logs and roots -- and some anywhere. */
static void fill_maths(void)
{
    s_rng = 0xbb67ae8584caa73bull;
    for (int i = 0; i < BW_MATHS_N; i++) {
        uint64_t a = rnd();
        uint64_t b = rnd();
        if (i & 3) {
            const uint64_t ea = 0x3ff - 30 + rnd() % 38;
            const uint64_t eb = 0x3ff - 30 + rnd() % 36;
            a = (a & 0x800fffffffffffffull) | (ea << 52);
            b = (b & 0x800fffffffffffffull) | (eb << 52);
        }
        s_ma[i] = dbl_of(a);
        s_mb[i] = dbl_of(b);
    }
}

#define TIMED(field, expr) do {                                         \
        const int64_t t_ = bw_now();                                    \
        for (int i = 0; i < BW_MATHS_N; i++) h = fnv_dbl(h, (expr));    \
        out->field = maths_ns(bw_now() - t_, loop);                     \
    } while (0)

static uint32_t maths_ns(int64_t us, int64_t loop_us)
{
    const int64_t net = us - loop_us;
    if (net <= 0 || net > 4000000) return 0;
    return (uint32_t)net * 1000u / BW_MATHS_N;
}

void bw_maths(bw_maths_t *out)
{
    memset(out, 0, sizeof *out);
    fill_maths();
    uint32_t h = 2166136261u;

    int64_t t0 = bw_now();
    for (int i = 0; i < BW_MATHS_N; i++) h = fnv_dbl(h, s_ma[i]);
    const int64_t loop = bw_now() - t0;

    TIMED(sqrt_ns, sqrt(s_ma[i]));
    TIMED(sin_ns, sin(s_ma[i]));
    for (int i = 0; i < BW_MATHS_N; i++) h = fnv_dbl(h, cos(s_ma[i]));
    TIMED(exp_ns, exp(s_ma[i]));
    TIMED(log_ns, log(s_ma[i]));
    TIMED(pow_ns, pow(s_ma[i], s_mb[i]));
    for (int i = 0; i < BW_MATHS_N; i++) {
        h = fnv_dbl(h, atan2(s_ma[i], s_mb[i]));
        h = fnv_dbl(h, floor(s_ma[i]));
        h = fnv_dbl(h, fmod(s_ma[i], s_mb[i]));
    }
    out->hash = h;
}
