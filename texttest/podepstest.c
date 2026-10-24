/*
 * podepstest.c -- Podcast Index's episodes/byfeedid as rows (8019).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podeps.h"

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

#define PARSE(s, max) podeps_parse((s), strlen(s), e, (max))

#define EP(id, title, url) \
    "{\"id\":" #id ",\"title\":\"" title "\",\"link\":\"https://x/\"," \
    "\"description\":\"{\\\"enclosureUrl\\\":\\\"http://not.this/\\\"}\",\"datePublished\":1700000000," \
    "\"enclosureUrl\":\"" url "\",\"enclosureType\":\"audio/mpeg\",\"enclosureLength\":583650930,\"duration\":3021," \
    "\"persons\":[{\"id\":1,\"title\":\"not this\",\"enclosureUrl\":\"http://nor.this/\"}]," \
    "\"feedId\":6676981}"

int main(void)
{
    static podep_t e[PODEPS_MAX];
    printf("podepstest\n");

    printf("  the reply as sent\n");
    {
        const char *r = "{\"status\":\"true\",\"items\":[" EP(11, "Episode 1", "https://cdn.x/ep1.mp3") ","
                        EP(12, "Episode 2", "http:\\/\\/cdn.x\\/ep2.m4a?x=1&y=2") "],\"count\":2,"
                        "\"query\":\"6676981\",\"description\":\"Found matching items.\"}";
        CHECK(PARSE(r, PODEPS_MAX) == 2, "two: %d", PARSE(r, PODEPS_MAX));
        CHECK(e[0].id == 11 && !strcmp(e[0].title, "Episode 1"), "%s", e[0].title);
        CHECK(!strcmp(e[0].url, "https://cdn.x/ep1.mp3"), "the item's own URL: %s", e[0].url);
        CHECK(e[0].duration == 3021 && e[0].published == 1700000000, "%d %lld", (int)e[0].duration,
              (long long)e[0].published);
        CHECK(!strcmp(e[1].url, "http://cdn.x/ep2.m4a?x=1&y=2"), "escaped slashes: %s", e[1].url);
        CHECK(e[0].size == 583650930, "enclosureLength: %lld", (long long)e[0].size);
        CHECK(!strcmp(e[0].desc, "{\"enclosureUrl\":\"http://not.this/\"}"), "the item's own description, as text: %s", e[0].desc);
    }

    printf("  the description\n");
    {
        const char *r = "{\"items\":[{\"id\":1,\"title\":\"t\",\"enclosureUrl\":\"https://a/1\","
                        "\"description\":\"<p>On this episode &amp; more.<\\/p><p>Links: <a href=\\\"x\\\">here<\\/a><\\/p>\"},"
                        "{\"id\":2,\"title\":\"u\",\"enclosureUrl\":\"https://a/2\"}]}";
        CHECK(PARSE(r, PODEPS_MAX) == 2, "two");
        CHECK(!strcmp(e[0].desc, "On this episode & more.\nLinks: here"), "%s", e[0].desc);
        CHECK(e[1].desc[0] == '\0', "none: [%s]", e[1].desc);
    }

    printf("  what is dropped\n");
    {
        char big[1400];
        char longurl[600];
        memset(longurl, 'a', sizeof(longurl));
        memcpy(longurl, "https://", 8);
        longurl[sizeof(longurl) - 1] = '\0';
        snprintf(big, sizeof(big), "{\"items\":[{\"id\":1,\"title\":\"long\",\"enclosureUrl\":\"%s\"},"
                 "{\"id\":2,\"title\":\"ok\",\"enclosureUrl\":\"https://y/2.mp3\"}]}", longurl);
        CHECK(PARSE(big, PODEPS_MAX) == 1 && e[0].id == 2, "a URL too long to keep whole");
        const char *r = "{\"items\":[{\"id\":1,\"title\":\"no url\"},"
                        "{\"id\":2,\"title\":\"ftp\",\"enclosureUrl\":\"ftp://z/2.mp3\"},"
                        "{\"id\":3,\"title\":\"empty\",\"enclosureUrl\":\"\"},"
                        "{\"id\":4,\"title\":\"\",\"enclosureUrl\":\"https://z/4.mp3\"},"
                        "{\"id\":0,\"title\":\"zero\",\"enclosureUrl\":\"https://z/0.mp3\"},"
                        "{\"id\":6,\"title\":\"fine\",\"enclosureUrl\":\"https://z/6.mp3\"}]}";
        CHECK(PARSE(r, PODEPS_MAX) == 1 && e[0].id == 6, "only 6: %d", PARSE(r, PODEPS_MAX));
    }

    printf("  missing numbers are 0, and where it stops\n");
    {
        const char *r = "{\"items\":[{\"id\":7,\"title\":\"t\",\"enclosureUrl\":\"https://z/7.mp3\","
                        "\"duration\":null}]}";
        CHECK(PARSE(r, PODEPS_MAX) == 1 && e[0].duration == 0 && e[0].published == 0 && e[0].size == 0, "zeros");
        CHECK(PARSE("{\"items\":[" EP(1, "a", "https://a/1") "," EP(2, "b", "https://a/2") ",{\"id\":3,\"ti",
                    PODEPS_MAX) == 2, "cut short");
        CHECK(PARSE("{\"items\":[" EP(1, "a", "https://a/1") "," EP(2, "b", "https://a/2") "]}", 1) == 1, "max");
        CHECK(PARSE("{\"feeds\":[" EP(1, "a", "https://a/1") "]}", PODEPS_MAX) == 0, "feeds is not items");
        CHECK(PARSE("{\"status\":\"false\",\"description\":\"no\"}", PODEPS_MAX) == 0, "no items");
        CHECK(PARSE("<html>", PODEPS_MAX) == 0, "not JSON");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
