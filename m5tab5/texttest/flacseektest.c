/*
 * flacseektest.c -- flac_seek_find() (6160).
 *
 * 6160 replaced a plain bisection with interpolation plus a walk through
 * the window already read, after the board timed a seek at 3 s. What
 * must not change is the answer, and what must change is the number of
 * reads. So for every whole second of every FLAC in the corpus this
 * checks that the frame found holds the target (or is the last frame
 * before it), cross-checks it against a sequential walk of the whole
 * file, and totals the window reads. The bisection averaged about 20 a
 * seek on these files; the new one has to do better than half of that.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/flacseek.c"

bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    (void)cls;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
}

static int checks, failures;

#define CHECK(cond, ...) do {                           \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

#define DIR "../test_audio_files/"

/* Every confirmed frame in the file, the slow way: the reference. */
typedef struct { long off; uint64_t sample; uint32_t block; } ref_frame_t;

static int all_frames(FILE *f, const flac_seek_t *fs, ref_frame_t *out, int max)
{
    const long n = fs->file_end - fs->first_frame;
    uint8_t *b = malloc((size_t)n);
    fseek(f, fs->first_frame, SEEK_SET);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); return 0; }
    int k = 0;
    long at = 0;
    while (at < n && k < max) {
        flac_frame_t fr;
        const long rel = find_frame(b + at, (size_t)(n - at), fs, &fr);
        if (rel < 0) break;
        out[k].off = fs->first_frame + at + rel;
        out[k].sample = fr.sample;
        out[k].block = fr.block;
        k++;
        at += rel + fr.len;
    }
    free(b);
    return k;
}

static void one_file(const char *name, uint32_t *total_reads, uint32_t *seeks)
{
    char path[256];
    snprintf(path, sizeof(path), DIR "%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (skipped: %s)\n", name); return; }
    flac_seek_t fs;
    CHECK(flac_seek_probe(f, &fs), "%s did not probe", name);
    static ref_frame_t ref[20000];
    const int nref = all_frames(f, &fs, ref, 20000);
    CHECK(nref > 10, "%s: %d frames", name, nref);

    const uint32_t secs = (uint32_t)(fs.total_samples / fs.sample_rate);
    uint32_t reads = 0, worst = 0;
    for (uint32_t s = 0; s <= secs; s++) {
        uint64_t landed = 0;
        const long off = flac_seek_find(f, &fs, s, &landed);
        const uint32_t r = flac_seek_last_reads();
        reads += r;
        if (r > worst) worst = r;
        uint64_t target = (uint64_t)s * fs.sample_rate;
        if (target >= fs.total_samples) target = fs.total_samples - 1;
        /* The reference: the last frame starting at or before the target
         * -- except the file's last frame, which find_frame() can never
         * confirm (see its comment), so the one before it stands in. */
        int want = 0;
        for (int i = 0; i < nref; i++) if (ref[i].sample <= target) want = i;
        CHECK(off == ref[want].off && landed == ref[want].sample,
              "%s at %us: offset %ld sample %llu, the frame is at %ld (%llu)",
              name, s, off, (unsigned long long)landed, ref[want].off,
              (unsigned long long)ref[want].sample);
        /* 6179: the remembered answer checks out, and a wrong one does
         * not -- off by a byte, or the right frame with the wrong sample. */
        CHECK(flac_seek_check(f, &fs, off, landed),
              "%s at %us: check rejected its own answer", name, s);
        CHECK(!flac_seek_check(f, &fs, off + 1, landed),
              "%s at %us: check took a frame a byte late", name, s);
        CHECK(!flac_seek_check(f, &fs, off, landed + 1),
              "%s at %us: check took the wrong sample", name, s);
    }
    printf("    %-28s %3u s, %u frames: %.1f reads a seek, worst %u\n",
           name, secs, nref, (double)reads / (secs + 1), worst);
    *total_reads += reads;
    *seeks += secs + 1;
    fclose(f);
}

int main(void)
{
    printf("flacseektest\n");
    uint32_t reads = 0, seeks = 0;
    const char *files[] = {
        "05 flac.flac", "13 flac-short-3s.flac", "21 flac-24bit.flac",
        "22 cue-flac-image.flac", "23 cue-pregap-htoa.flac",
        "25a cue-multifile.flac", "27 cue-bumps.flac",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        one_file(files[i], &reads, &seeks);
    }
    if (seeks) {
        const double avg = (double)reads / seeks;
        printf("  %.1f reads a seek over %u seeks\n", avg, seeks);
        CHECK(avg < 8.0, "%.1f reads a seek: no better than the bisection", avg);
    }
    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
