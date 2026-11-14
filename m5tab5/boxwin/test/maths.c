/* SPDX-License-Identifier: MIT
 *
 * bw_maths() on a host, against feckless-libs' newlib_math (u-defeatist-0013).
 *
 * test/host.c links the host's own libm, glibc's, whose results are not
 * newlib's, so it cannot judge BW_MATHS_EXPECT. This links bw_maths() with
 * the newlib sources feckless-libs compiles into a boxed app instead, and
 * requires the hash the toolchain's libm.a gave under qemu-riscv32: so the
 * constant boxwin judges the board by is the one those files give.
 *
 *     make -C test maths FECKLESS_BOX_LIBC=/path/to/feckless_box_libc
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>

#include "boxwin_arith.h"

int64_t bw_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void)
{
    bw_maths_t m, again;
    bw_maths(&m);
    bw_maths(&again);
    const int ok = m.hash == BW_MATHS_EXPECT && again.hash == m.hash;
    printf("boxwin maths: hash %08x, %s %08x%s\n", (unsigned)m.hash, ok ? "as boxwin_arith.h says," : "boxwin_arith.h says",
           (unsigned)BW_MATHS_EXPECT, again.hash == m.hash ? "" : ", and a second run differed");
    return !ok;
}
