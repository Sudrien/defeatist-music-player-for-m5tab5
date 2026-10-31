/*
 * oggseektest.c -- ogg_seek_probe() finds where the audio starts (6142).
 *
 * Fixtures in fixtures/ogg, from tools/gen_ogg_fixtures.py: one second of
 * Vorbis and of Opus, each without a picture and with one in
 * METADATA_BLOCK_PICTURE. The picture makes the comment header span
 * pages, which the probe used to stop at: "audio at 58" and the decoder
 * fed the rest of the comment, so an Ogg with a cover played nothing.
 * vorbis_pic70k's headers are also past the 64 KB the probe read in one
 * go.
 *
 * The check does not reuse the probe's rule. It walks the segment tables
 * up to the offset the probe gave and counts complete packets: three
 * headers for Vorbis, two for Opus, and the page at that offset must not
 * continue a packet.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/oggseek.c"

/* storage_io_read_at(), over stdio. */
bool storage_io_read_at(FILE *f, int64_t off, void *dst, size_t len, storage_io_class_t cls)
{
    (void)cls;
    if (fseek(f, (long)off, SEEK_SET) != 0) return false;
    return fread(dst, 1, len, f) == len;
}

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;      \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);   \
    printf("\n"); } } while (0)

/* Complete packets in [0, upto), by the lacing values alone. */
static int packets_before(const uint8_t *d, long upto, int *bad_page)
{
    long i = 0;
    int packets = 0;
    *bad_page = 0;
    while (i + 27 <= upto) {
        if (memcmp(d + i, "OggS", 4) != 0) { *bad_page = 1; return -1; }
        const int segs = d[i + 26];
        long payload = 0;
        for (int s = 0; s < segs; s++) {
            payload += d[i + 27 + s];
            if (d[i + 27 + s] < 255) packets++;
        }
        i += 27 + segs + payload;
    }
    if (i != upto) *bad_page = 1;
    return packets;
}

static void probe(const char *name, int want_packets)
{
    char path[256];
    snprintf(path, sizeof(path), "fixtures/ogg/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    uint8_t *d = malloc((size_t)size);
    fseek(f, 0, SEEK_SET);
    if (fread(d, 1, (size_t)size, f) != (size_t)size) exit(2);

    ogg_seek_t os;
    const bool ok = ogg_seek_probe(f, &os);
    CHECK(ok, "%s: probe failed", name);
    if (ok) {
        int bad = 0;
        const int n = packets_before(d, os.first_audio, &bad);
        CHECK(!bad && n == want_packets, "%s: audio at %ld leaves %d header packets (want %d)%s",
              name, os.first_audio, n, want_packets, bad ? ", not on a page boundary" : "");
        CHECK(os.first_audio + 27 <= size && memcmp(d + os.first_audio, "OggS", 4) == 0 &&
              !(d[os.first_audio + 5] & 1), "%s: the first audio page continues a packet", name);
        CHECK(os.header_len == (size_t)os.first_audio && memcmp(os.header, d, os.header_len) == 0,
              "%s: the kept header is not the file's first %ld bytes", name, os.first_audio);
        CHECK(os.last_granule > 0, "%s: no length", name);
        printf("  %-18s audio at %6ld of %6ld\n", name, os.first_audio, size);
        ogg_seek_free(&os);
    }
    fclose(f);
    free(d);
}

int main(void)
{
    printf("oggseektest\n");
    probe("vorbis.ogg", 3);
    probe("vorbis_pic20k.ogg", 3);
    probe("vorbis_pic70k.ogg", 3);
    probe("opus.opus", 2);
    probe("opus_pic20k.opus", 2);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
