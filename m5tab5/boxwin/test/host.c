/* SPDX-License-Identifier: MIT
 *
 * host.c -- boxwin_run.c on a host (u-defeatist-0002).
 *
 * The file that goes into the box, compiled here with a host clock and no
 * lease, and run over the repository's own test files. It checks that the
 * measuring code decodes every format it claims to, to the length the file
 * has, and that the window bookkeeping adds up. It says nothing about how
 * long a window is on the board: the times printed are this machine's.
 *
 *     make -C boxwin/test
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "boxwin.h"
#include "boxwin_arith.h"

static int checks, failures;

#define CHECK(cond, ...) do {                   \
    checks++;                                   \
    if (!(cond)) {                              \
        failures++;                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                    \
        printf("\n");                           \
    }                                           \
} while (0)

static int s_leases, s_lease_max;

int64_t bw_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void bw_lease(void)   { if (++s_leases > s_lease_max) s_lease_max = s_leases; }
void bw_unlease(void) { s_leases--; }

static void bookkeeping(void)
{
    CHECK(bw_bucket(0) == 0 && bw_bucket(999) == 0, "under 1 ms is the first bucket");
    CHECK(bw_bucket(1000) == 1 && bw_bucket(4999) == 2 && bw_bucket(5000) == 3, "the edges belong above");
    CHECK(bw_bucket(19999) == 4 && bw_bucket(20000) == 5, "20 ms is the audio budget's bucket");
    CHECK(bw_bucket(50000) == 6 && bw_bucket(UINT32_MAX) == 6, "the last bucket is open");

    bw_stat_t s;
    memset(&s, 0, sizeof s);
    bw_track_t t = { .open_us = -1, .into = &s };
    bw_close(&t, 100);                          /* nothing open: nothing recorded */
    CHECK(s.windows == 0, "a close with no window open records nothing");
    bw_open(&t, 1000);
    bw_close(&t, 1400);                         /* 400 us */
    bw_open(&t, 2000);
    bw_close(&t, 9000);                         /* 7 ms */
    bw_open(&t, 10000);
    bw_close(&t, 35000);                        /* 25 ms */
    bw_close(&t, 99000);                        /* closed twice: once */
    CHECK(s.windows == 3 && s.max_us == 25000 && s.sum_us == 32400, "three windows, their sum and longest");
    CHECK(bw_stat_over(&s, BW_PANEL_US) == 2, "two of 5 ms or more, got %u", (unsigned)bw_stat_over(&s, BW_PANEL_US));
    CHECK(bw_stat_over(&s, BW_AUDIO_US) == 1, "one of 20 ms or more, got %u", (unsigned)bw_stat_over(&s, BW_AUDIO_US));
    t.into = NULL;
    bw_open(&t, 40000);
    bw_close(&t, 90000);
    CHECK(s.windows == 3, "not recording: nothing added");
    bw_open(&t, 50);
    t.into = &s;
    bw_close(&t, 40);                           /* a clock that went back */
    CHECK(s.windows == 3, "a window that ends before it began is dropped");
}

/* --expect: main/boxwin_expect.h on stdout, from what this host decodes
 * (`make -C test expect`). */
static int expect(int argc, char **argv)
{
    printf("/* SPDX-License-Identifier: MIT\n *\n"
           " * The PCM hash of the first BW_MAX_AUDIO_MS of each file of\n"
           " * test_audio_files/ that boxwin decodes, as a host gets it\n"
           " * (u-defeatist-0010). Written by `make -C boxwin/test expect`; test/host.c\n"
           " * fails if a file decodes to anything else. `exact`: an integer decoder,\n"
           " * which must give this on any machine.\n */\n"
           "static const struct { const char *name; uint32_t pcm; bool exact; } bw_expect[] = {\n");
    for (int i = 2; i < argc; i++) {
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        bw_result_t r;
        bw_run_file(argv[i], false, &r);
        if (!r.ok) continue;
        printf("    { \"%s\", 0x%08xu, %s },\n", name, (unsigned)r.pcm_hash,
               !strncmp(r.kind, "ape", 3) ? "true" : "false");
    }
    printf("};\n");
    return 0;
}

static void arith(void)
{
    bw_arith_t a;
    bw_arith(&a);
    CHECK(a.dbl_hash == BW_ARITH_DBL_EXPECT, "the double hash is %08x here, boxwin_arith.h says %08x",
          (unsigned)a.dbl_hash, (unsigned)BW_ARITH_DBL_EXPECT);
    CHECK(a.div_hash == BW_ARITH_DIV_EXPECT, "the division hash is %08x here, boxwin_arith.h says %08x",
          (unsigned)a.div_hash, (unsigned)BW_ARITH_DIV_EXPECT);
    /* The same again: nothing in it carries over from one run to the next. */
    bw_arith_t b;
    bw_arith(&b);
    CHECK(a.dbl_hash == b.dbl_hash && a.div_hash == b.div_hash, "a second run gave other hashes");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--expect")) return expect(argc, argv);
    bookkeeping();
    arith();
    CHECK(!bw_pie_built() && !bw_pie_selftest(), "no PIE kernels in a host build");

    for (int i = 1; i < argc; i++) {
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        bw_result_t r;
        bw_run_file(argv[i], false, &r);
        char line[320];
        printf("%s\n", bw_format(name, &r, line, sizeof line));
        if (r.skipped) continue;
        /* The one file the player refuses by name. */
        if (strstr(name, "refuse")) {
            CHECK(!r.ok && r.why, "%s should be refused", name);
            continue;
        }
        CHECK(r.ok, "%s: %s", name, r.why ? r.why : "?");
        if (!r.ok) continue;
        CHECK(r.audio_ms >= 1000, "%s: only %u ms of audio", name, (unsigned)r.audio_ms);
        CHECK(r.rate >= 8000 && (r.channels == 1 || r.channels == 2), "%s: %u Hz x%u", name,
              (unsigned)r.rate, (unsigned)r.channels);
        /* u-defeatist-0006: a window for each place PCM is handed on and
         * one for each read, and no other -- not one for the set-up, which
         * is what the first cut used to record. */
        CHECK(r.win.windows == r.cuts + r.reads, "%s: %u windows for %u cuts and %u reads", name,
              (unsigned)r.win.windows, (unsigned)r.cuts, (unsigned)r.reads);
        CHECK(r.cuts >= r.calls && r.reads > 0 && r.read_kb > 0, "%s: %u cuts for %u calls, %u reads, %u KB",
              name, (unsigned)r.cuts, (unsigned)r.calls, (unsigned)r.reads, (unsigned)r.read_kb);
        uint32_t h = 0;
        for (int b = 0; b < BW_BUCKETS; b++) h += r.win.hist[b];
        CHECK(h == r.win.windows, "%s: the buckets hold %u of %u windows", name, (unsigned)h,
              (unsigned)r.win.windows);
        CHECK(r.pie == false, "%s: PIE on a host", name);
        /* u-defeatist-0010: the table the board judges itself by is what
         * this host decodes, and decoding twice gives the same. */
        CHECK(r.pcm_want != 0, "%s is not in main/boxwin_expect.h (make expect)", name);
        CHECK(r.pcm_hash == r.pcm_want, "%s: pcm %08x, main/boxwin_expect.h says %08x", name,
              (unsigned)r.pcm_hash, (unsigned)r.pcm_want);
        CHECK(r.pcm_exact == !strncmp(r.kind, "ape", 3), "%s: exact is for the integer decoder", name);
        /* 0011: again with the PCM buffers filled otherwise. A hash that
         * moves took in a sample the decoder did not write. */
        bw_result_t again;
        bw_fill = 0xa5;
        bw_run_file(argv[i], false, &again);
        bw_fill = 0;
        CHECK(again.pcm_hash == r.pcm_hash, "%s: pcm %08x with the buffers filled 0xa5, %08x filled 0", name,
              (unsigned)again.pcm_hash, (unsigned)r.pcm_hash);
        /* The line fits a box's log, at its worst: every count as wide as
         * it gets, and the longest name here. */
        bw_result_t wide = r;
        wide.pie = true;
        wide.audio_ms = wide.setup_ms = wide.read_kb = wide.read_ms = 99999;
        wide.win.windows = 99999;
        wide.win.sum_us = 99999000;
        wide.win.max_us = 99900;
        for (int b = 0; b < BW_BUCKETS; b++) wide.win.hist[b] = b >= 3 ? 9999 : 0;
        wide.pcm_exact = true;
        wide.pcm_want = ~wide.pcm_hash;
        char big[512];
        bw_format(argv[i], &wide, big, sizeof big);
        CHECK(strlen(big) < BW_LINE_MAX - 1, "%s: a line of %zu characters, of %d", name, strlen(big), BW_LINE_MAX);
        CHECK(strstr(big, "WRONG") != NULL && strstr(big, " !!") != NULL, "%s: the line lost its end: %s", name, big);
    }
    CHECK(s_leases == 0, "%d leases still held", s_leases);
    CHECK(s_lease_max == 1, "a lease taken while one was held (%d deep)", s_lease_max);
    printf("boxwin host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
