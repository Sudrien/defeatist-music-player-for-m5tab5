/*
 * hlssegtest.c -- hlsseg.h: a segment's ID3 tags off, its kind named.
 *
 * Every case is fed whole, then split at every byte into two reads, then
 * one byte at a time, and what comes out must be exactly the payload
 * every way. A read boundary is not something the server chooses to
 * respect.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/hlsseg.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static uint8_t got[8192];
static size_t  got_n;

static void take(const uint8_t *seg[2], const size_t segn[2])
{
    for (int k = 0; k < 2; k++) {
        if (!segn[k]) continue;
        if (got_n + segn[k] > sizeof(got)) { got_n = sizeof(got) + 1; return; }
        memcpy(got + got_n, seg[k], segn[k]);
        got_n += segn[k];
    }
}

/* Feed `in` as reads of the given lengths (0-terminated list, the last
 * read taking the rest), then end it. */
static hlsseg_kind_t run(const uint8_t *in, size_t n, const size_t *cuts, hlsseg_t *s)
{
    hlsseg_begin(s);
    got_n = 0;
    size_t off = 0;
    for (int k = 0; off < n; k++) {
        size_t len = cuts && cuts[k] ? cuts[k] : n - off;
        if (len > n - off) len = n - off;
        const uint8_t *seg[2];
        size_t segn[2];
        const size_t tot = hlsseg_feed(s, in + off, len, seg, segn);
        CHECK(tot == segn[0] + segn[1], "total is the two spans");
        take(seg, segn);
        off += len;
        if (s->kind == HLSSEG_TS || s->kind == HLSSEG_FMP4) break;
    }
    const uint8_t *tail;
    const size_t tn = hlsseg_end(s, &tail);
    if (tn) {
        const uint8_t *seg[2] = { tail, NULL };
        const size_t segn[2] = { tn, 0 };
        take(seg, segn);
    }
    return s->kind;
}

/* Whole, every two-way split, and byte at a time: all must agree. */
static void every_way(const char *what, const uint8_t *in, size_t n,
                      hlsseg_kind_t want_kind, const uint8_t *want, size_t want_n,
                      uint32_t want_tags)
{
    hlsseg_t s;
    int bad = 0;
    for (size_t cut = 0; cut <= n + 1; cut++) {
        size_t cuts[3] = { 0, 0, 0 };
        if (cut == n + 1) {
            /* byte at a time */
        } else if (cut > 0 && cut < n) {
            cuts[0] = cut;
        }
        hlsseg_kind_t k;
        if (cut == n + 1) {
            hlsseg_begin(&s);
            got_n = 0;
            for (size_t i = 0; i < n; i++) {
                const uint8_t *seg[2];
                size_t segn[2];
                hlsseg_feed(&s, in + i, 1, seg, segn);
                take(seg, segn);
                if (s.kind == HLSSEG_TS || s.kind == HLSSEG_FMP4) break;
            }
            const uint8_t *tail;
            const size_t tn = hlsseg_end(&s, &tail);
            if (tn) {
                const uint8_t *seg[2] = { tail, NULL };
                const size_t segn[2] = { tn, 0 };
                take(seg, segn);
            }
            k = s.kind;
        } else {
            k = run(in, n, cuts[0] ? cuts : NULL, &s);
        }
        const bool ok = k == want_kind && got_n == want_n &&
                        (want_n == 0 || memcmp(got, want, want_n) == 0) &&
                        s.tags == want_tags;
        if (!ok && !bad++) {
            printf("  %s: cut %zu gave kind %s, %zu bytes, %u tags; want %s, %zu, %u\n",
                   what, cut, hlsseg_kind_name(k), got_n, (unsigned)s.tags,
                   hlsseg_kind_name(want_kind), want_n, (unsigned)want_tags);
        }
    }
    CHECK(!bad, "%s: %d of %zu ways wrong", what, bad, n + 2);
}

/* An ID3v2.4 tag with a PRIV frame the size HLS uses for its timestamp. */
static size_t id3(uint8_t *b, size_t body, bool footer)
{
    b[0] = 'I'; b[1] = 'D'; b[2] = '3'; b[3] = 4; b[4] = 0;
    b[5] = footer ? 0x10 : 0;
    b[6] = (uint8_t)((body >> 21) & 0x7F);
    b[7] = (uint8_t)((body >> 14) & 0x7F);
    b[8] = (uint8_t)((body >> 7) & 0x7F);
    b[9] = (uint8_t)(body & 0x7F);
    for (size_t i = 0; i < body; i++) b[10 + i] = (uint8_t)(0x30 + i % 40);
    size_t n = 10 + body;
    if (footer) {
        memcpy(b + n, "3DI", 3);
        memset(b + n + 3, 0, 7);
        n += 10;
    }
    return n;
}

/* ADTS-looking payload: 0xFFF1 frames. */
static size_t adts(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(i % 64 == 0 ? 0xFF : i % 64 == 1 ? 0xF1 : i * 7);
    return n;
}

int main(void)
{
    static uint8_t in[4096], pay[4096];
    size_t n, pn;

    /* The ordinary case: one PRIV tag, then ADTS. */
    pn = adts(pay, 700);
    n = id3(in, 63, false);
    memcpy(in + n, pay, pn); n += pn;
    every_way("tag + adts", in, n, HLSSEG_AUDIO, pay, pn, 1);

    /* Two tags, the second with a footer. */
    n = id3(in, 30, false);
    n += id3(in + n, 5, true);
    memcpy(in + n, pay, pn); n += pn;
    every_way("two tags", in, n, HLSSEG_AUDIO, pay, pn, 2);

    /* No tag at all: MP3 straight away. */
    for (size_t i = 0; i < 500; i++) pay[i] = (uint8_t)(i % 100 == 0 ? 0xFF : i % 100 == 1 ? 0xFB : i);
    every_way("bare mp3", pay, 500, HLSSEG_AUDIO, pay, 500, 0);

    /* Payload that starts like a tag and is not one. */
    static const uint8_t idx[] = "IDXnot a tag, just bytes that begin with I and D";
    every_way("IDX", idx, sizeof(idx) - 1, HLSSEG_AUDIO, idx, sizeof(idx) - 1, 0);
    static const uint8_t i_only[] = "Ixyzzy plugh";
    every_way("I", i_only, sizeof(i_only) - 1, HLSSEG_AUDIO, i_only, sizeof(i_only) - 1, 0);

    /* A tag whose size is not syncsafe is not skipped: payload as is. */
    n = id3(in, 20, false);
    in[8] = 0x81;
    every_way("bad syncsafe", in, n, HLSSEG_AUDIO, in, n, 0);

    /* MPEG-TS after the tag: refused, nothing out. */
    n = id3(in, 20, false);
    for (int p = 0; p < 4; p++) {
        in[n] = 0x47;
        memset(in + n + 1, p, 187);
        n += 188;
    }
    every_way("tag + ts", in, n, HLSSEG_TS, NULL, 0, 1);
    /* TS with no tag, which is how TS segments usually come. */
    every_way("ts", in + 30, n - 30, HLSSEG_TS, NULL, 0, 0);
    /* One packet only: still TS. */
    every_way("one ts packet", in + 30, 188, HLSSEG_TS, NULL, 0, 0);
    /* 0x47 then not 0x47 at 188: audio after all. */
    memcpy(pay, in + 30, 300);
    pay[188] = 0x11;
    every_way("0x47 not ts", pay, 300, HLSSEG_AUDIO, pay, 300, 0);

    /* fMP4. */
    static const uint8_t mp4[] = { 0, 0, 0, 24, 's', 't', 'y', 'p', 'm', 's', 'd', 'h',
                                   0, 0, 0, 0, 'm', 's', 'd', 'h', 'm', 's', 'i', 'x' };
    every_way("fmp4", mp4, sizeof(mp4), HLSSEG_FMP4, NULL, 0, 0);

    /* Tiny segments: decided at the end. */
    static const uint8_t tiny[] = { 0xFF, 0xF1, 0x50 };
    every_way("tiny audio", tiny, sizeof(tiny), HLSSEG_AUDIO, tiny, sizeof(tiny), 0);
    static const uint8_t id[] = { 'I', 'D' };
    every_way("just ID", id, sizeof(id), HLSSEG_AUDIO, id, sizeof(id), 0);

    /* A tag and nothing else: an empty segment, which is legal. */
    n = id3(in, 40, false);
    every_way("tag only", in, n, HLSSEG_UNKNOWN, NULL, 0, 1);

    /* A tag claiming more than the segment holds: skip stays set. */
    n = id3(in, 4000, false);
    {
        hlsseg_t s;
        run(in, 200, NULL, &s);
        CHECK(s.skip > 0 && got_n == 0, "overlong tag leaves skip %llu",
              (unsigned long long)s.skip);
    }

    /* Empty input does nothing. */
    {
        hlsseg_t s;
        hlsseg_begin(&s);
        const uint8_t *seg[2];
        size_t segn[2];
        CHECK(hlsseg_feed(&s, in, 0, seg, segn) == 0 && s.kind == HLSSEG_UNKNOWN, "empty");
        const uint8_t *t;
        CHECK(hlsseg_end(&s, &t) == 0 && t == NULL, "empty end");
    }

    printf("hlssegtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
