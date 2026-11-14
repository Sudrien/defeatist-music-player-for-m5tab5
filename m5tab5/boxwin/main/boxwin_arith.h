/* SPDX-License-Identifier: MIT
 *
 * boxwin_arith.h -- the box's double arithmetic and 64-bit division,
 * checked and timed (u-defeatist-0010).
 *
 * In a box these are not instructions. A double operation and a 64-bit
 * divide are calls into libgcc, which for a boxed app were host calls
 * until feckless-libs carried its own in the app (u-feckless-0036, 0038).
 * boxwin's first runs on that arithmetic showed it was faster and could not
 * show it was right: boxwin timed decoders and never looked at a result.
 *
 * bw_arith() does a fixed, seeded run of every such operation the compiler
 * can emit a call for, and hashes the results. The same code on a host
 * does the same operations in its FPU and its own divider, and
 * test/host.c holds the hashes below to that. So on the board, equal
 * hashes mean every one of those results was the bit pattern IEEE 754 and
 * C give -- whichever way the box does them.
 *
 * It also times each kind, which nothing had: with feckless-libs before
 * 0036 the figure is a trap into the launcher and back, after it the
 * routine's own time.
 */
#pragma once

#include <stdint.h>

#define BW_ARITH_N 8192         /* operand pairs; each kind of operation is done on all of them */

/* What a host gets. test/host.c fails if they are not what it computes. */
#define BW_ARITH_DBL_EXPECT 0x1beec1c3u
#define BW_ARITH_DIV_EXPECT 0x2bebcd67u

typedef struct {
    uint32_t dbl_hash;          /* add, sub, mul, div, compares, every conversion */
    uint32_t div_hash;          /* / and % on 64 bits, unsigned and signed, and by 1000 */
    /* Nanoseconds an operation, the loop around it taken off; 0 where the
     * clock could not tell it from the loop. */
    uint32_t add_ns, mul_ns, div_ns, conv_ns;
    uint32_t udiv_ns, udiv1000_ns;
} bw_arith_t;

void bw_arith(bw_arith_t *out);

/* u-defeatist-0013: the double libm the decoders' set-up calls -- sqrt,
 * sin, cos, exp, log, pow, atan2, floor, fmod -- on BW_MATHS_N seeded
 * arguments each, hashed by their bits (a NaN as one pattern). The hash is
 * not a host's: libm is not IEEE-exact, and glibc's and newlib's differ.
 * It is what newlib's gives, the toolchain's libm.a under qemu-riscv32,
 * which is what a box answered these with as host calls and what
 * feckless-libs' newlib_math (u-feckless-0041) compiles into the app.
 * test/Makefile's maths target holds newlib_math to it on a host. */
#define BW_MATHS_N 2048
#define BW_MATHS_EXPECT 0xd01dbf09u

typedef struct {
    uint32_t hash;
    uint32_t sqrt_ns, sin_ns, exp_ns, log_ns, pow_ns;   /* a call, the loop taken off */
} bw_maths_t;

void bw_maths(bw_maths_t *out);
