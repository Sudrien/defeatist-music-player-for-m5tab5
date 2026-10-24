/*
 * podpathtest.c -- where a downloaded episode goes (8020).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podpath.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                 \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
        failures++;                                                     \
    }                                                                   \
} while (0)

int main(void)
{
    char s[600];
    printf("podpathtest\n");

    printf("  names\n");
    podpath_name("Agenda Cultural Joinville | Sexta-feira, 23/10", s, sizeof(s));
    CHECK(!strcmp(s, "Agenda Cultural Joinville _ Sexta-feira, 23_10"), "%s", s);
    podpath_name("a\\b:c*d?e\"f<g>h", s, sizeof(s));
    CHECK(!strcmp(s, "a_b_c_d_e_f_g_h"), "%s", s);
    podpath_name("  leading, and trailing. . ", s, sizeof(s));
    CHECK(!strcmp(s, "leading, and trailing"), "[%s]", s);
    podpath_name("tab\there\nnewline", s, sizeof(s));
    CHECK(!strcmp(s, "tab_here_newline"), "%s", s);
    podpath_name("...", s, sizeof(s));
    CHECK(!strcmp(s, "_"), "nothing left: %s", s);
    podpath_name(NULL, s, sizeof(s));
    CHECK(!strcmp(s, "_"), "NULL");
    podpath_name("\xd8\xb4\xd8\xb9\xd8\xb1 \xda\xa9\xd8\xb3\xd8\xaa | SherCast", s, sizeof(s));
    CHECK(!strcmp(s, "\xd8\xb4\xd8\xb9\xd8\xb1 \xda\xa9\xd8\xb3\xd8\xaa _ SherCast"), "UTF-8 kept: %s", s);
    {
        char big[300];
        memset(big, 'a', 95); memcpy(big + 95, "\xc3\xa9zzz", 6);
        podpath_name(big, s, sizeof(s));
        CHECK(strlen(s) == 95, "cut before the letter that would split: %zu", strlen(s));
        memset(big, 'b', 200); big[200] = 0;
        podpath_name(big, s, sizeof(s));
        CHECK(strlen(s) == PODPATH_NAME_MAX, "cut at the limit: %zu", strlen(s));
        char tiny[6];
        podpath_name("abcdefgh", tiny, sizeof(tiny));
        CHECK(!strcmp(tiny, "abcde"), "and at the buffer: %s", tiny);
    }

    printf("  extensions\n");
    podpath_url_ext("https://op3.dev/e/traffic.megaphone.fm/ABC123.mp3?updated=1700", s, 8);
    CHECK(!strcmp(s, "mp3"), "%s", s);
    podpath_url_ext("https://x/y/Episode.M4A#t=10", s, 8);
    CHECK(!strcmp(s, "m4a"), "upper case: %s", s);
    podpath_url_ext("https://x/download?file=a.mp3", s, 8);
    CHECK(!strcmp(s, ""), "only the path, not the query: %s", s);
    podpath_url_ext("https://x.example.com/stream", s, 8);
    CHECK(!strcmp(s, ""), "no dot in the path: %s", s);
    podpath_url_ext("https://x.mp3.example/a/b", s, 8);
    CHECK(!strcmp(s, ""), "a dot in the host or a folder: %s", s);
    podpath_url_ext("https://x/a.html", s, 8);
    CHECK(!strcmp(s, ""), "not one this plays: %s", s);
    CHECK(!strcmp(podpath_type_ext("audio/mpeg"), "mp3"), "mpeg");
    CHECK(!strcmp(podpath_type_ext("audio/x-m4a; charset=binary"), "m4a"), "parameters");
    CHECK(!strcmp(podpath_type_ext("audio/mpegurl"), ""), "a prefix is not a match");
    CHECK(!strcmp(podpath_type_ext(NULL), ""), "NULL");

    printf("  paths\n");
    CHECK(podpath_dir("/sd", "Rádio Joinville: Cultural 105,1", s, sizeof(s)) &&
          !strcmp(s, "/sd/Podcasts/Rádio Joinville_ Cultural 105,1"), "%s", s);
    {
        char t[8];
        CHECK(!podpath_dir("/sd", "long name", t, sizeof(t)), "does not fit");
    }
    podpath_base(1761177600, "Agenda | 23/10", s, sizeof(s));
    CHECK(!strcmp(s, "2025-10-23 Agenda _ 23_10"), "%s", s);
    podpath_base(951782400, "leap", s, sizeof(s));
    CHECK(!strncmp(s, "2000-02-29 ", 11), "a leap day: %s", s);
    podpath_base(1792799761, "today", s, sizeof(s));
    CHECK(!strncmp(s, "2026-10-23 ", 11), "%s", s);
    podpath_base(0, "Undated", s, sizeof(s));
    CHECK(!strcmp(s, "Undated"), "%s", s);

    printf("  the folder's link back\n");
    {
        char t[64];
        int64_t feed = 0;
        const int n = podpath_link_format(6654186, "R\xc3\xa1" "dio Joinville", s, sizeof(s));
        CHECK(!strcmp(s, "feed 6654186\nR\xc3\xa1" "dio Joinville\n"), "%s", s);
        CHECK(podpath_link_parse(s, (size_t)n, &feed, t, sizeof(t)) && feed == 6654186 &&
              !strcmp(t, "R\xc3\xa1" "dio Joinville"), "%lld %s", (long long)feed, t);
        CHECK(podpath_link_parse("feed 5\r\ncaf\xc3\xa9", 13, &feed, t, sizeof(t)) &&
              feed == 5 && !strcmp(t, "caf\xc3\xa9"), "CRLF, a whole last letter kept: %s", t);
        char t5[5];
        CHECK(podpath_link_parse("feed 5\ncaf\xc3\xa9", 12, &feed, t5, sizeof(t5)) && !strcmp(t5, "caf"),
              "half a letter dropped: %s", t5);
        CHECK(podpath_link_parse("feed 7", 6, &feed, t, sizeof(t)) && feed == 7 && !t[0], "no title");
        CHECK(!podpath_link_parse("feed 0\nx", 8, &feed, t, sizeof(t)), "id 0");
        CHECK(!podpath_link_parse("feed x\n", 7, &feed, t, sizeof(t)), "no id");
        CHECK(!podpath_link_parse("feed 12a\n", 9, &feed, t, sizeof(t)), "junk after the id");
        CHECK(!podpath_link_parse("FEED 1\n", 7, &feed, t, sizeof(t)), "not the keyword");
        CHECK(!podpath_link_parse(NULL, 0, &feed, t, sizeof(t)), "NULL");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
