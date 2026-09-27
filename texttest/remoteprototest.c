/*
 * remoteprototest.c -- the browser remote's wire format, against the real
 * main/remoteproto.c. Commands come from anyone on the network and state
 * strings from any tag on a card, which is why it is pure.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "remoteproto.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static remote_cmd_t parse(const char *s)
{
    remote_cmd_t c;
    remoteproto_parse(s, strlen(s), &c);
    return c;
}

/* Minimal UTF-8 validity check, independent of the one under test. */
static int valid_utf8(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        int n = *u < 0x80 ? 1 : (*u & 0xE0) == 0xC0 ? 2 : (*u & 0xF0) == 0xE0 ? 3
              : (*u & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) return 0;
        for (int i = 1; i < n; i++) if ((u[i] & 0xC0) != 0x80) return 0;
        u += n;
    }
    return 1;
}

int main(void)
{
    printf("remoteprototest\n");

    printf("  commands\n");
    CHECK(parse("hello").kind == REMOTE_CMD_HELLO, "hello");
    CHECK(parse("play").kind == REMOTE_CMD_PLAY, "play");
    CHECK(parse("pause").kind == REMOTE_CMD_PAUSE, "pause");
    CHECK(parse("next").kind == REMOTE_CMD_NEXT, "next");
    CHECK(parse("prev").kind == REMOTE_CMD_PREV, "prev");
    CHECK(parse("star").kind == REMOTE_CMD_STAR, "star");
    {
        remote_cmd_t c = parse("vol 74");
        CHECK(c.kind == REMOTE_CMD_VOLUME && c.value == 74, "vol 74 -> %d/%d", c.kind, c.value);
        c = parse("vol 0");
        CHECK(c.kind == REMOTE_CMD_VOLUME && c.value == 0, "vol 0");
        c = parse("seek 100");
        CHECK(c.kind == REMOTE_CMD_SEEK && c.value == 100, "seek 100");
    }

    printf("  refusals\n");
    static const char *const bad[] = {
        "", "Play", "play ", " play", "playx", "record", "rec", "vol", "vol ",
        "vol 101", "vol -1", "vol +5", "vol 5x", "vol 0050", "vol  5", "seek",
        "play 5", "star 1", "hello!", "vol 1e2", "seek 999",
        "pause pause pause pause",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        remote_cmd_t c = { REMOTE_CMD_PLAY, 7 };
        const bool ok = remoteproto_parse(bad[i], strlen(bad[i]), &c);
        CHECK(!ok && c.kind == REMOTE_CMD_NONE, "\"%s\" accepted", bad[i]);
    }
    {
        /* Not terminated: the length is the truth. */
        remote_cmd_t c;
        CHECK(remoteproto_parse("playing", 4, &c) && c.kind == REMOTE_CMD_PLAY,
              "length-bounded play");
        CHECK(!remoteproto_parse("vol 5\0", 6, &c), "embedded NUL");
        CHECK(!remoteproto_parse(NULL, 4, &c), "NULL");
    }

    printf("  state JSON\n");
    {
        remote_state_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.title, sizeof(s.title), "Say \"hi\"\\ \n\x01");
        snprintf(s.artist, sizeof(s.artist), "Piotr Musia\xc5\x82");      /* valid */
        snprintf(s.album, sizeof(s.album), "Caf\xe9 \xff\xc0\xaf end");   /* Latin-1, overlong */
        snprintf(s.art, sizeof(s.art), "897bf3d9");
        s.pos_sec = 35; s.len_sec = 255; s.stats_valid = true; s.playing = true;
        s.volume = 74; s.fav = 2; s.batt_pct = -1; s.wave = 3;

        char out[1024];
        const size_t n = remoteproto_state_json(&s, out, sizeof(out));
        CHECK(n > 0 && n == strlen(out), "length %zu", n);
        CHECK(strstr(out, "\"title\":\"Say \\\"hi\\\"\\\\ \\u000a\\u0001\"") != NULL,
              "escapes: %s", out);
        CHECK(strstr(out, "Musia\xc5\x82") != NULL, "valid UTF-8 passes");
        CHECK(strstr(out, "Caf\xef\xbf\xbd \xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd end") != NULL,
              "invalid bytes become U+FFFD: %s", out);
        CHECK(valid_utf8(out), "output is valid UTF-8");
        CHECK(strstr(out, "\"batt\":-1") && strstr(out, "\"vol\":74") &&
              strstr(out, "\"playing\":true") && strstr(out, "\"rec\":false"),
              "fields: %s", out);
        CHECK(out[0] == '{' && out[n - 1] == '}', "one object");

        /* Every smaller buffer: 0 and empty, never a truncated object. */
        int partial = 0;
        for (size_t cap = 1; cap <= n; cap++) {
            char small[1024];
            const size_t m = remoteproto_state_json(&s, small, cap);
            if (m != 0 || small[0] != '\0') partial++;
        }
        CHECK(partial == 0, "%d undersized buffers produced output", partial);
        char exact[1024];
        CHECK(remoteproto_state_json(&s, exact, n + 1) == n, "fits exactly");
    }

    printf("  wave JSON\n");
    {
        const uint8_t lv[4] = { 0x00, 0x0f, 0xa5, 0xff };
        char out[64];
        const size_t n = remoteproto_wave_json(lv, 4, 9, out, sizeof(out));
        CHECK(n > 0 && strcmp(out, "{\"t\":\"wave\",\"gen\":9,\"lv\":\"000fa5ff\"}") == 0,
              "%s", out);
        CHECK(remoteproto_wave_json(lv, 4, 9, out, 10) == 0 && out[0] == '\0', "too small");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
