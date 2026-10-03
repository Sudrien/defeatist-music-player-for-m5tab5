/*
 * hlsplaytest.c -- hlsplay.h: an HLS playlist, read.
 *
 * Written from RFC 8216's rules and from what real servers send, which
 * is not always the same thing: CRLF, BOMs, trailing blanks, CODECS with
 * commas inside quotes, tokens on every segment URI.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/hlsplay.h"

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

static hls_media_t m;       /* 2.3 KB: module scope, as on the board */

static void resolve(const char *base, const char *ref, const char *want)
{
    char out[160];
    const bool ok = hls_resolve(base, ref, strlen(ref), out, sizeof(out));
    if (want) CHECK(ok && strcmp(out, want) == 0,
                    "[%s] + [%s] gave [%s], want [%s]", base, ref, out, want);
    else CHECK(!ok && out[0] == '\0', "[%s] + [%s] should refuse, gave [%s]",
               base, ref, out);
}

static int seg_is(const char *t, int i, const char *want)
{
    return m.seg[i].len == strlen(want) &&
           memcmp(t + m.seg[i].off, want, m.seg[i].len) == 0;
}

static void test_kind(void)
{
    CHECK(hls_kind("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\na.m3u8\n", 40) == HLS_MASTER, "master");
    CHECK(hls_kind("#EXTM3U\r\n#EXT-X-TARGETDURATION:6\r\n", 33) == HLS_MEDIA, "media crlf");
    const char bom[] = "\xEF\xBB\xBF#EXTM3U\n#EXTINF:6,\na.aac\n";
    CHECK(hls_kind(bom, sizeof(bom) - 1) == HLS_MEDIA, "bom");
    CHECK(hls_kind("[playlist]\nFile1=x\n", 19) == HLS_NOT, "pls");
    CHECK(hls_kind("http://x/y\n", 11) == HLS_NOT, "bare url");
    CHECK(hls_kind(NULL, 0) == HLS_NOT, "null");
    CHECK(hls_kind("#EXTM3U\n", 8) == HLS_MEDIA, "empty live playlist is media");

    const char *radio = "#EXTM3U\n#EXTINF:-1,Station\nhttp://x/stream\n";
    CHECK(!hls_looks_like(radio, strlen(radio)), "station m3u is not HLS");
    const char *h = "#EXTM3U\n#EXT-X-VERSION:3\n";
    CHECK(hls_looks_like(h, strlen(h)), "EXT-X is HLS");
    CHECK(!hls_looks_like("#EXT-X", 6), "short");
}

static void test_attr(void)
{
    const char *a = "BANDWIDTH=64000,CODECS=\"mp4a.40.2,avc1.4d401e\",AUDIO=\"aac\",AUDIO-ONLY=1";
    size_t vo, vl;
    CHECK(hls_attr(a, 0, strlen(a), "CODECS", &vo, &vl) && vl == 21 &&
          memcmp(a + vo, "mp4a.40.2,avc1.4d401e", 21) == 0, "quoted with comma");
    CHECK(hls_attr(a, 0, strlen(a), "BANDWIDTH", &vo, &vl) && hls_u64(a, vo, vl) == 64000, "bw");
    CHECK(hls_attr(a, 0, strlen(a), "AUDIO", &vo, &vl) && hls_val_is(a, vo, vl, "aac"), "whole name");
    CHECK(hls_attr(a, 0, strlen(a), "AUDIO-ONLY", &vo, &vl) && hls_val_is(a, vo, vl, "1"), "dashed name");
    CHECK(!hls_attr(a, 0, strlen(a), "BAND", &vo, &vl), "prefix is not a match");
    const char *bad = "A=\"unterminated,B=2";
    CHECK(!hls_attr(bad, 0, strlen(bad), "B", &vo, &vl), "unterminated quote swallows the rest");

    CHECK(hls_secs_ms("9.984,", 0, 6) == 9984, "9.984");
    CHECK(hls_secs_ms("10", 0, 2) == 10000, "10");
    CHECK(hls_secs_ms("6.4", 0, 3) == 6400, "6.4");
    CHECK(hls_secs_ms("0.0001", 0, 6) == 0, "sub-ms");
    CHECK(hls_secs_ms("99999999999", 0, 11) == UINT32_MAX, "saturates");
    CHECK(hls_u64("99999999999999999999999", 0, 23) == UINT64_MAX, "u64 saturates");
}

static void test_resolve(void)
{
    const char *b = "https://cdn.example/live/radio/01.m3u8?tok=abc";
    resolve(b, "seg1.aac", "https://cdn.example/live/radio/seg1.aac");
    resolve(b, "seg1.aac?tok=xyz", "https://cdn.example/live/radio/seg1.aac?tok=xyz");
    resolve(b, "./seg1.aac", "https://cdn.example/live/radio/seg1.aac");
    resolve(b, "../hi/a.m3u8", "https://cdn.example/live/hi/a.m3u8");
    resolve(b, "../../../../x.ts", "https://cdn.example/x.ts");
    resolve(b, "/abs/x.ts", "https://cdn.example/abs/x.ts");
    resolve(b, "//other.example/x.ts", "https://other.example/x.ts");
    resolve(b, "http://plain.example/x.ts", "http://plain.example/x.ts");
    resolve("https://h.example", "x.ts", "https://h.example/x.ts");
    resolve("https://h.example?q=1", "x.ts", "https://h.example/x.ts");
    resolve("https://h.example:8443/a/b", "c", "https://h.example:8443/a/c");
    resolve("no-scheme/a", "b", NULL);
    resolve(b, "", NULL);

    /* Too long refuses, it does not cut. */
    char longref[200];
    memset(longref, 'a', sizeof(longref) - 1);
    longref[sizeof(longref) - 1] = '\0';
    resolve(b, longref, NULL);
    char fit[strlen("https://cdn.example/live/radio/seg1.aac") + 1];
    CHECK(hls_resolve(b, "seg1.aac", 8, fit, sizeof(fit)), "exact fit");
    CHECK(!hls_resolve(b, "seg1.aac", 8, fit, sizeof(fit) - 1) && fit[0] == '\0',
          "one short refuses and leaves it empty");
}

static void test_master(void)
{
    hls_choice_t c;
    const char *aud =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\n"
        "hi/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=48000,CODECS=\"mp4a.40.5\"\n"
        "lo/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=20000,CODECS=\"avc1.42e00a,mp4a.40.2\"\n"
        "vid/index.m3u8\n";
    CHECK(hls_pick_variant(aud, strlen(aud), &c) == HLS_OK && c.audio_only &&
          c.bandwidth == 48000 && c.variants == 3 &&
          memcmp(aud + c.uri_off, "lo/index.m3u8", c.uri_len) == 0,
          "lowest audio-only beats a cheaper video variant");

    const char *tv =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/en.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"ar\",DEFAULT=YES,URI=\"audio/ar.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",URI=\"subs.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,AUDIO=\"a\"\n"
        "v360.m3u8\n";
    CHECK(hls_pick_variant(tv, strlen(tv), &c) == HLS_OK && c.rendition && !c.audio_only &&
          memcmp(tv + c.uri_off, "audio/ar.m3u8", c.uri_len) == 0,
          "audio rendition, DEFAULT first");

    const char *bare =
        "#EXTM3U\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=1280x720\r\n"
        "v720.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=96000\r\n"
        "audio.m3u8  \r\n";
    CHECK(hls_pick_variant(bare, strlen(bare), &c) == HLS_OK && !c.audio_only &&
          c.uri_len == 10 && memcmp(bare + c.uri_off, "audio.m3u8", 10) == 0,
          "no RESOLUTION beats a picture; trailing blanks trimmed");

    const char *vidonly =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1920x1080\n"
        "a.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360\n"
        "b.m3u8\n";
    CHECK(hls_pick_variant(vidonly, strlen(vidonly), &c) == HLS_OK &&
          memcmp(vidonly + c.uri_off, "b.m3u8", 6) == 0 && c.bandwidth == 500000,
          "only video: the cheapest");

    const char *nobw =
        "#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"mp4a.40.2\"\nx.m3u8\n";
    CHECK(hls_pick_variant(nobw, strlen(nobw), &c) == HLS_OK && c.bandwidth == 0,
          "missing BANDWIDTH is 0, not UINT32_MAX");

    const char *dangling = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n";
    CHECK(hls_pick_variant(dangling, strlen(dangling), &c) == HLS_ERR_EMPTY, "no URI line");
    CHECK(hls_pick_variant("#EXTM3U\n#EXTINF:6,\na\n", 21, &c) == HLS_ERR_NOT_HLS, "media is not master");
}

static void test_media(void)
{
    const char *live =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:10\n"
        "#EXT-X-MEDIA-SEQUENCE:4711\n"
        "#EXTINF:9.984,News at nine\n"
        "s4711.aac\n"
        "#EXTINF:9.984,\n"
        "s4712.aac\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:10.007,\n"
        "s4713.aac\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-10-02T12:00:00Z\n"
        "#EXTINF:9.984,\n"
        "s4714.aac\n"
        "#EXTINF:9.984,\n"
        "s4715.aac\n";
    CHECK(hls_parse_media(live, strlen(live), &m) == HLS_OK, "parses");
    CHECK(m.n == 5 && m.first_seq == 4711 && m.target_ms == 10000 && !m.endlist, "shape");
    CHECK(seg_is(live, 0, "s4711.aac") && seg_is(live, 4, "s4715.aac"), "uris");
    CHECK(m.seg[0].dur_ms == 9984 && m.seg[2].dur_ms == 10007, "durations");
    CHECK(m.seg[0].title_len == 12 && memcmp(live + m.seg[0].title_off, "News at nine", 12) == 0,
          "title");
    CHECK(m.seg[1].title_len == 0, "empty title");
    CHECK(!m.seg[1].disc && m.seg[2].disc && !m.seg[3].disc, "discontinuity on one");

    /* The live edge, three back. */
    hls_cursor_t cur = {0};
    int idx;
    uint64_t lost;
    bool rs;
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_PLAY && idx == 2 &&
          cur.next_seq == 4713, "starts three back (%d)", idx);
    hls_advance(&cur);
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_PLAY && idx == 3, "next");
    hls_advance(&cur);
    hls_next(&cur, &m, &idx, &lost, &rs);
    hls_advance(&cur);
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_WAIT && idx == -1, "caught up");
    CHECK(hls_reload_ms(&m, true) == 10000 && hls_reload_ms(&m, false) == 5000, "reload");

    /* Window moves on by two: next is in it. */
    const char *moved =
        "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:4713\n"
        "#EXTINF:10,\ns4713.aac\n#EXTINF:10,\ns4714.aac\n#EXTINF:10,\ns4715.aac\n"
        "#EXTINF:10,\ns4716.aac\n#EXTINF:10,\ns4717.aac\n";
    hls_parse_media(moved, strlen(moved), &m);
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_PLAY && idx == 3 && lost == 0 && !rs,
          "resumes at 4716");

    /* We were slow: the window is past us. */
    const char *ahead =
        "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:4730\n"
        "#EXTINF:10,\ns4730.aac\n#EXTINF:10,\ns4731.aac\n#EXTINF:10,\ns4732.aac\n";
    hls_parse_media(ahead, strlen(ahead), &m);
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_PLAY && idx == 0 && lost == 14 && !rs,
          "fell behind: oldest kept, 14 lost (%llu)", (unsigned long long)lost);

    /* Encoder restart: sequence back to 1. */
    const char *reset =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1\n"
        "#EXTINF:6,\na\n#EXTINF:6,\nb\n#EXTINF:6,\nc\n#EXTINF:6,\nd\n#EXTINF:6,\ne\n";
    hls_parse_media(reset, strlen(reset), &m);
    hls_advance(&cur);
    CHECK(hls_next(&cur, &m, &idx, &lost, &rs) == HLS_NEXT_PLAY && rs && idx == 2 && lost == 0,
          "restart goes to the live edge");

    /* VOD: from the top, then ended. */
    const char *vod =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:6,\na\n#EXTINF:6,\nb\n#EXTINF:6,\nc\n#EXTINF:6,\nd\n#EXT-X-ENDLIST\n";
    CHECK(hls_parse_media(vod, strlen(vod), &m) == HLS_OK && m.endlist, "vod");
    hls_cursor_t v = {0};
    CHECK(hls_next(&v, &m, &idx, NULL, NULL) == HLS_NEXT_PLAY && idx == 0, "vod from the top");
    for (int i = 0; i < 4; i++) hls_advance(&v);
    CHECK(hls_next(&v, &m, &idx, NULL, NULL) == HLS_NEXT_ENDED, "vod ends");

    /* Short live list: starts at the first. */
    const char *two = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\na\n#EXTINF:6,\nb\n";
    hls_parse_media(two, strlen(two), &m);
    hls_cursor_t s2 = {0};
    CHECK(hls_next(&s2, &m, &idx, NULL, NULL) == HLS_NEXT_PLAY && idx == 0 && m.first_seq == 0,
          "short list, no MEDIA-SEQUENCE");

    /* Empty live playlist waits; empty VOD is empty. */
    CHECK(hls_parse_media("#EXTM3U\n#EXT-X-TARGETDURATION:6\n", 32, &m) == HLS_OK && m.n == 0, "empty live");
    hls_cursor_t e = {0};
    CHECK(hls_next(&e, &m, &idx, NULL, NULL) == HLS_NEXT_WAIT, "empty waits");
    CHECK(hls_parse_media("#EXTM3U\n#EXT-X-ENDLIST\n", 23, &m) == HLS_ERR_EMPTY, "empty vod");

    /* Refusals. */
    const char *key = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:6,\na\n";
    CHECK(hls_parse_media(key, strlen(key), &m) == HLS_ERR_KEY, "aes refused");
    const char *nokey = "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\na\n";
    CHECK(hls_parse_media(nokey, strlen(nokey), &m) == HLS_OK, "METHOD=NONE is fine");
    const char *mp4 = "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:6,\na.m4s\n";
    CHECK(hls_parse_media(mp4, strlen(mp4), &m) == HLS_ERR_FMP4, "fmp4 refused");
    CHECK(hls_parse_media("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nx\n", 40, &m) == HLS_ERR_NOT_HLS,
          "master is not media");

    /* Target duration clamps. */
    hls_parse_media("#EXTM3U\n#EXT-X-TARGETDURATION:0\n", 32, &m);
    CHECK(m.target_ms == HLS_TARGET_MIN_MS, "0 s clamps up");
    hls_parse_media("#EXTM3U\n#EXT-X-TARGETDURATION:999999999999\n", 43, &m);
    CHECK(m.target_ms == HLS_TARGET_MAX_MS, "huge clamps down");

    /* URI without EXTINF: kept, at the target. */
    hls_parse_media("#EXTM3U\n#EXT-X-TARGETDURATION:4\nbare.aac\n", 41, &m);
    CHECK(m.n == 1 && m.seg[0].dur_ms == 4000, "bare uri");

    /* DISCONTINUITY-SEQUENCE is not a discontinuity. */
    hls_parse_media("#EXTM3U\n#EXT-X-DISCONTINUITY-SEQUENCE:3\n#EXTINF:6,\na\n", 52, &m);
    CHECK(m.n == 1 && !m.seg[0].disc, "discontinuity-sequence");

    /* A NUL in the buffer ends the text. */
    const char nul[] = "#EXTM3U\n#EXTINF:6,\na\n\0#EXTINF:6,\nb\n";
    hls_parse_media(nul, sizeof(nul) - 1, &m);
    CHECK(m.n == 1, "stops at NUL (%d)", m.n);
}

static void test_long(void)
{
    /* More segments than kept: the last HLS_SEG_MAX, in order, with the
     * sequence numbers following them. */
    static char big[HLS_SEG_MAX * 64 + 256];
    size_t n = (size_t)snprintf(big, sizeof(big),
                                "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n");
    const int total = HLS_SEG_MAX + 37;
    for (int i = 0; i < total; i++)
        n += (size_t)snprintf(big + n, sizeof(big) - n, "#EXTINF:6,\ns%d.aac\n", 100 + i);
    CHECK(hls_parse_media(big, n, &m) == HLS_OK && m.n == HLS_SEG_MAX && m.dropped == 37,
          "kept %d dropped %u", m.n, (unsigned)m.dropped);
    CHECK(m.first_seq == 137, "first_seq follows the drop (%llu)", (unsigned long long)m.first_seq);
    char want[32];
    int in_order = 1;
    for (int i = 0; i < m.n; i++) {
        snprintf(want, sizeof(want), "s%d.aac", 137 + i);
        if (!seg_is(big, i, want)) { in_order = 0; break; }
    }
    CHECK(in_order, "ring straightened in order");

    /* Every prefix of a playlist parses without reading past it. */
    const char *live =
        "#EXTM3U\r\n#EXT-X-TARGETDURATION:10\r\n#EXT-X-MEDIA-SEQUENCE:9\r\n"
        "#EXT-X-KEY:METHOD=NONE\r\n#EXTINF:9.9,Title, with comma\r\nhttps://h/a.aac?t=1\r\n";
    const size_t L = strlen(live);
    for (size_t k = 0; k <= L; k++) {
        char *copy = (char *)malloc(k ? k : 1);
        memcpy(copy, live, k);
        (void)hls_kind(copy, k);
        (void)hls_parse_media(copy, k, &m);
        hls_choice_t c;
        (void)hls_pick_variant(copy, k, &c);
        (void)hls_looks_like(copy, k);
        free(copy);
    }
    CHECK(1, "prefixes");
    hls_parse_media(live, L, &m);
    CHECK(m.n == 1 && m.seg[0].title_len == 17, "title keeps its own comma");
}

int main(void)
{
    test_kind();
    test_attr();
    test_resolve();
    test_master();
    test_media();
    test_long();
    printf("hlsplaytest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
