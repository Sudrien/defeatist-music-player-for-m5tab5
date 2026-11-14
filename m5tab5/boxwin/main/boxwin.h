/* SPDX-License-Identifier: MIT
 *
 * boxwin.h -- how long a boxed decoder computes between host calls
 * (u-defeatist-0002).
 *
 * In a feckless box (feckless-libs' umode branch) the app's own code runs in
 * RISC-V user mode in "windows": from the return of one host call to the
 * ecall of the next. A window holds interrupts off on its core
 * (u-feckless-0017), so its length is how long an interrupt on that core can
 * be kept waiting: the panel's DMA restart every frame, and the I2S and USB
 * interrupts of drivers the app brought up itself, behind DMA buffers of
 * about 20 ms (u-feckless-0031).
 *
 * This is the bookkeeping, with the clock passed in, so test/ runs it on a
 * host. boxwin_run.c closes a window before each host call it makes and
 * opens one after it.
 *
 * WHAT IS MEASURED IS AN UPPER BOUND. A host call this code does not make
 * itself ends a window without being seen here: libgcc's double arithmetic
 * and 64-bit division are host calls in a box (feckless_box_calls). The
 * decoders' inner loops use neither, but a window recorded here may have
 * been two on the board. It is never longer there than here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A window lands in the first bucket whose edge it is under; the last
 * bucket is everything from the last edge up. */
#define BW_BUCKETS 7
static const uint32_t bw_edge_us[BW_BUCKETS - 1] = { 1000, 2000, 5000, 10000, 20000, 50000 };

/* The budgets a window is judged against (see the top of this file). */
#define BW_PANEL_US  (5000)     /* u-feckless-0031: "a window of a few milliseconds starves it" */
#define BW_AUDIO_US  (20000)    /* the I2S DMA buffers */

typedef struct {
    uint32_t windows;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t hist[BW_BUCKETS];
} bw_stat_t;

static inline int bw_bucket(uint32_t us)
{
    int b = 0;
    while (b < BW_BUCKETS - 1 && us >= bw_edge_us[b]) b++;
    return b;
}

static inline void bw_stat_add(bw_stat_t *s, uint32_t us)
{
    s->windows++;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
    s->hist[bw_bucket(us)]++;
}

/* Windows of at least `us`, which must be one of the edges. */
static inline uint32_t bw_stat_over(const bw_stat_t *s, uint32_t us)
{
    uint32_t n = 0;
    for (int b = 0; b < BW_BUCKETS; b++) {
        if (b > 0 && bw_edge_us[b - 1] >= us) n += s->hist[b];
    }
    return n;
}

typedef struct {
    int64_t    open_us;     /* when the window now running began; <0: none */
    bw_stat_t *into;        /* NULL: not recording (set-up, between files) */
} bw_track_t;

/* A host call is about to be made: the window ends. */
static inline void bw_close(bw_track_t *t, int64_t now_us)
{
    if (t->into && t->open_us >= 0 && now_us >= t->open_us) {
        const int64_t d = now_us - t->open_us;
        bw_stat_add(t->into, d > UINT32_MAX ? UINT32_MAX : (uint32_t)d);
    }
    t->open_us = -1;
}

/* The host call returned: the next window begins. */
static inline void bw_open(bw_track_t *t, int64_t now_us)
{
    t->open_us = now_us;
}

/* ---- one file ---------------------------------------------------------- */

/* Blocks (samples a channel) one ape_next() is asked for: a 4 KB chunk of
 * 16-bit stereo, the player's PCM_CHUNK_BYTES. 23 ms of audio at 44.1 kHz,
 * so a level decoded at 1.2x real time is a 19 ms window. */
#define BW_APE_BLOCKS   1024

/* Audio decoded from one file before moving on. */
#define BW_MAX_AUDIO_MS 30000

typedef struct {
    bool        ok;
    bool        skipped;        /* not a format decoded by the app's own code */
    const char *why;            /* when !ok */
    char        kind[24];       /* "mp3", "ape 5000", "dsd128 DSF" */
    bool        pie;            /* decoded with the P4's PIE kernels */
    uint32_t    rate, channels; /* of the PCM that came out */
    uint32_t    audio_ms;       /* decoded */
    uint32_t    setup_ms;       /* probe and tables, before the first window */
    uint32_t    calls;          /* decode calls: frames, chunks or blocks */
    uint32_t    cuts;           /* windows ended where PCM would be handed on */
    uint32_t    reads;          /* windows ended by a read, while decoding */
    uint32_t    read_kb;        /* what those reads brought in */
    uint32_t    read_ms;        /* and took, lease and seek included */
    bw_stat_t   win;            /* windows == cuts + reads (u-defeatist-0006) */
    uint32_t    pcm_hash;       /* FNV-1a of every PCM sample that came out (0010) */
    uint32_t    pcm_want;       /* what a host gets for this file, 0: not a file it knows */
    bool        pcm_exact;      /* integer decoder: any machine must get pcm_want */
} bw_result_t;

/* What the PCM buffers are filled with before a decoder writes to them.
 * A hash that takes in a sample no decoder wrote is a hash of whatever the
 * allocator handed over: 0010 hashed two samples a block of a mono APE,
 * which writes one, and the board, the host and the host under ASan each
 * had a hash of their own for that file. test/host.c decodes every file
 * under two fills and requires one hash. */
extern uint8_t bw_fill;

/* The longest line bw_format() may make. A box's log line is 256 bytes
 * with its prefix, and one that does not fit loses its end and its
 * newline, so the next line starts on it. */
#define BW_LINE_MAX 224

/* What the platform supplies: main/boxwin_main.c in the box, test/host.c on
 * a host. bw_now() is a host call in a box (esp_timer_get_time()). The lease
 * is storage_io_acquire(STORAGE_IO_PLAYBACK) and its release, held across
 * each read as Spurious's sd_io_fatfs.c holds it. */
int64_t bw_now(void);
void    bw_lease(void);
void    bw_unlease(void);

/* Decode `path` with the app's own decoder for its extension (.mp3 .mp2
 * .ape .dsf .dff), from the start, BW_MAX_AUDIO_MS at most, recording the
 * windows. `pie`: with the PIE kernels, where the build has them and
 * bw_pie_selftest() said they work. Any other extension: skipped. */
void bw_run_file(const char *path, bool pie, bw_result_t *out);

/* Whether this build has PIE kernels at all (the P4, not a host). */
bool bw_pie_built(void);

/* Run ape.h's and dsd.h's PIE self-tests. THIS EXECUTES PIE INSTRUCTIONS:
 * in a box that does not turn the PIE unit on for user mode, the app ends
 * here on an illegal instruction, which is the answer. Returns whether both
 * passed. */
bool bw_pie_selftest(void);

/* The PCM hash a host gets for the first BW_MAX_AUDIO_MS of a file of
 * test_audio_files/, by the file's name without its folder; false for any
 * other. `exact` for the APE files, whose decoder is integer arithmetic
 * and must give the same samples anywhere, C kernel or PIE. MP3 and DSD go
 * through float, where a compiler may fuse a multiply and an add on one
 * machine and not another, so theirs need not agree with the board's. */
bool bw_pcm_expect(const char *name, uint32_t *want, bool *exact);

/* One line for the log and the screen. Returns out. */
char *bw_format(const char *name, const bw_result_t *r, char *out, size_t len);
