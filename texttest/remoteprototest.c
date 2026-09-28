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
        snprintf(s.path, sizeof(s.path), "/usb/a/b.mp3");
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

    printf("  paths\n");
    {
        static const char *const good[] = {
            "/", "/sd", "/usb", "/sd/Music", "/usb/B\xc3\xb4a - Twilight/01 Duvet.mp3",
            "/sd/a/b/c.flac", "/sd/.hidden/x", "/sd/..x", "/sd/x..",
        };
        for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            CHECK(remoteproto_path_ok(good[i], strlen(good[i])), "refused %s", good[i]);
        }
        static const char *const bad[] = {
            "", "sd", "sd/x", "/etc", "/sdx", "/usbx/a", "/sd/", "/sd//x", "/sd/./x",
            "/sd/../usb", "/sd/a/..", "/sd/a/.", "//sd", "/sd\\x", "/sd/a\nb",
            "/usb/a\x7f", "/data/x", "/spiffs",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            CHECK(!remoteproto_path_ok(bad[i], strlen(bad[i])), "accepted %s", bad[i]);
        }
        static char longp[REMOTEPROTO_PATH_MAX + 8];
        memset(longp, 'a', sizeof(longp));
        memcpy(longp, "/sd/", 4);
        CHECK(remoteproto_path_ok(longp, REMOTEPROTO_PATH_MAX - 1), "longest");
        CHECK(!remoteproto_path_ok(longp, REMOTEPROTO_PATH_MAX), "one past");

        remote_cmd_t c = parse("ls /");
        CHECK(c.kind == REMOTE_CMD_LS && c.path_len == 1 && c.path[0] == '/', "ls /");
        c = parse("open /usb/a b/c.mp3");
        CHECK(c.kind == REMOTE_CMD_OPEN && c.path_len == 14 && memcmp(c.path, "/usb/a b/c.mp3", 14) == 0,
              "open with a space");
        c = parse("playdir /sd/Album");
        CHECK(c.kind == REMOTE_CMD_PLAYDIR, "playdir");
        static const char *const badc[] = { "ls", "ls ", "ls  /sd", "open /", "xls /sd",
                                            "open /sd/../etc", "playdir sd", "ls /sd/" };
        for (size_t i = 0; i < sizeof(badc) / sizeof(badc[0]); i++) {
            remote_cmd_t k;
            CHECK(!remoteproto_parse(badc[i], strlen(badc[i]), &k) && k.kind == REMOTE_CMD_NONE,
                  "\"%s\" accepted", badc[i]);
        }
    }

    printf("  queue verbs (5173)\n");
    {
        remote_cmd_t c = parse("add /sd/Album/01.mp3");
        CHECK(c.kind == REMOTE_CMD_ADD && c.path_len == 16 && memcmp(c.path, "/sd/Album/01.mp3", 16) == 0,
              "add");
        c = parse("addnext /usb/x.flac");
        CHECK(c.kind == REMOTE_CMD_ADDNEXT && c.path_len == 11, "addnext");
        c = parse("qdel 7");
        CHECK(c.kind == REMOTE_CMD_QDEL && c.value == 7 && c.value2 == 0, "qdel 7");
        c = parse("qplay 2147483647");
        CHECK(c.kind == REMOTE_CMD_QPLAY && c.value == 2147483647, "qplay, the largest id");
        c = parse("qmove 12 0");
        CHECK(c.kind == REMOTE_CMD_QMOVE && c.value == 12 && c.value2 == 0, "qmove 12 0");
        c = parse("qmove 3 1023");
        CHECK(c.kind == REMOTE_CMD_QMOVE && c.value == 3 && c.value2 == 1023, "qmove 3 1023");
        c = parse("qclear");
        CHECK(c.kind == REMOTE_CMD_QCLEAR, "qclear");
        /* A second value never leaks from a verb that has none. */
        c = parse("qdel 5");
        CHECK(c.value2 == 0, "qdel leaves value2 0");

        static const char *const badq[] = {
            "add", "add ", "add /", "add sd/x.mp3", "add /sd/../x.mp3", "addnext", "addnext /",
            "qdel", "qdel ", "qdel 0", "qdel 07", "qdel -1", "qdel +1", "qdel 1x", "qdel 1 ",
            "qdel 2147483648", "qdel 99999999999", "qdel  1",
            "qmove", "qmove 1", "qmove 1 ", "qmove 0 1", "qmove 1  2", "qmove 1 02",
            "qmove 1 2 3", "qmove 1 -2", "qmove 1 2147483648", "qmove  1 2",
            "qplay", "qplay 0", "qclear ", "qclear 1", "QDEL 1", "qdelx 1",
        };
        for (size_t i = 0; i < sizeof(badq) / sizeof(badq[0]); i++) {
            remote_cmd_t k;
            CHECK(!remoteproto_parse(badq[i], strlen(badq[i]), &k) && k.kind == REMOTE_CMD_NONE,
                  "\"%s\" accepted", badq[i]);
        }
    }

    printf("  queue frames (5174)\n");
    {
        static char out[256];
        const remote_qrow_t rows[] = {
            { 3, "a.mp3" }, { 9, "B \"quoted\".flac" }, { 12, "c\xff.ogg" }, { 13, NULL },
        };
        int next = -1;
        size_t n = remoteproto_queue_frame(rows, 4, 0, 42, 1, out, sizeof(out), &next);
        CHECK(n > 0 && next == 4 && strcmp(out,
              "{\"t\":\"q\",\"v\":42,\"cur\":1,\"total\":4,\"from\":0,\"rows\":"
              "[[3,\"a.mp3\"],[9,\"B \\\"quoted\\\".flac\"],[12,\"c\xef\xbf\xbd.ogg\"],[13,\"\"]],"
              "\"done\":true}") == 0 && n == strlen(out), "whole: %s", out);

        n = remoteproto_queue_frame(NULL, 0, 0, 1, -1, out, sizeof(out), &next);
        CHECK(n > 0 && next == 0 && strcmp(out,
              "{\"t\":\"q\",\"v\":1,\"cur\":-1,\"total\":0,\"from\":0,\"rows\":[],\"done\":true}") == 0,
              "empty: %s", out);

        /* Split across frames: every row exactly once, in order, each frame
         * valid on its own, and none over its cap. */
        static remote_qrow_t many[40];
        static char names[40][24];
        for (int i = 0; i < 40; i++) {
            snprintf(names[i], sizeof(names[i]), "track %02d.mp3", i);
            many[i].id = (uint32_t)(100 + i);
            many[i].name = names[i];
        }
        int from = 0, frames = 0, seen = 0;
        bool ok = true;
        while (from < 40 && frames < 50) {
            memset(out, 'Z', sizeof(out));
            n = remoteproto_queue_frame(many, 40, from, 7, 5, out, 200, &next);
            if (!n || n >= 200 || out[n] != '\0' || next <= from) { ok = false; break; }
            char want[32];
            snprintf(want, sizeof(want), "\"from\":%d,", from);
            if (!strstr(out, want)) ok = false;
            if (strstr(out, next == 40 ? "\"done\":true}" : "\"done\":false}") != out + n - (next == 40 ? 12 : 13))
                ok = false;
            for (int i = from; i < next; i++) {
                char r[40];
                snprintf(r, sizeof(r), "[%d,\"track %02d.mp3\"]", 100 + i, i);
                if (!strstr(out, r)) ok = false;
                seen++;
            }
            from = next;
            frames++;
        }
        CHECK(ok && seen == 40 && frames > 1, "split: %d frames, %d rows", frames, seen);

        /* A row larger than a whole frame: refused, not truncated. */
        static char huge[300];
        memset(huge, 'x', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        const remote_qrow_t big[] = { { 1, huge } };
        CHECK(remoteproto_queue_frame(big, 1, 0, 1, 0, out, sizeof(out), &next) == 0 && out[0] == '\0',
              "a row that cannot fit is refused");
        CHECK(remoteproto_queue_frame(rows, 4, 5, 1, 0, out, sizeof(out), &next) == 0, "from past the end");
        CHECK(remoteproto_queue_frame(rows, 4, 0, 1, 0, out, 10, &next) == 0, "a cap too small for the header");
    }

    printf("  one string\n");
    {
        char out[64];
        size_t n = remoteproto_json_str("a\"b\xff", out, sizeof(out));
        CHECK(n > 0 && strcmp(out, "\"a\\\"b\xef\xbf\xbd\"") == 0, "%s", out);
        CHECK(remoteproto_json_str("", out, sizeof(out)) == 2 && strcmp(out, "\"\"") == 0, "empty");
        CHECK(remoteproto_json_str(NULL, out, sizeof(out)) == 2, "NULL is empty");
        CHECK(remoteproto_json_str("abcdef", out, 5) == 0 && out[0] == '\0', "too small");
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
