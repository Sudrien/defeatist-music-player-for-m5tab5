/*
 * podfeedstest.c -- Podcast Index's podcasts/trending as rows (8018).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podfeeds.h"

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

#define PARSE(s, max) podfeeds_parse((s), strlen(s), f, (max))

/* One feed as the service sends it, nested categories and all. */
#define FEED(id, title) \
    "{\"id\":" #id ",\"url\":\"https://x/" #id ".xml\",\"title\":\"" title "\"," \
    "\"description\":\"{\\\"title\\\":\\\"not this\\\"}\",\"author\":\"A\"," \
    "\"categories\":{\"104\":\"Tv\",\"title\":\"nor this\"},\"trendScore\":9}"

int main(void)
{
    static podfeed_t f[PODFEEDS_MAX];
    char s[256];
    printf("podfeedstest\n");

    printf("  the reply as sent\n");
    {
        const char *r = "{\"status\":\"true\",\"feeds\":[" FEED(75075, "Design Matters") ","
                        FEED(920666, "99% Invisible") "],\"count\":2,\"max\":\"40\",\"description\":\"x\"}";
        CHECK(PARSE(r, PODFEEDS_MAX) == 2, "two: %d", PARSE(r, PODFEEDS_MAX));
        CHECK(f[0].id == 75075 && !strcmp(f[0].title, "Design Matters"), "%lld %s", (long long)f[0].id, f[0].title);
        CHECK(f[1].id == 920666 && !strcmp(f[1].title, "99% Invisible"), "%s", f[1].title);
    }

    printf("  a title is the feed's own, at its top level\n");
    {
        const char *r = "{\"feeds\":[{\"categories\":{\"title\":\"wrong\"},\"id\":5,\"title\":\"right\"}]}";
        CHECK(PARSE(r, PODFEEDS_MAX) == 1 && !strcmp(f[0].title, "right"), "%s", f[0].title);
        const char *r2 = "{\"description\":\"\\\"feeds\\\":[{\\\"id\\\":1}]\",\"feeds\":[{\"id\":6,\"title\":\"t\"}]}";
        CHECK(PARSE(r2, PODFEEDS_MAX) == 1 && f[0].id == 6, "\"feeds\" in a value is not the key");
    }

    printf("  escapes and unicode\n");
    {
        CHECK(podfeeds_str("\"a\\\"b\\\\c\\/d\"", "\"a\\\"b\\\\c\\/d\"" + 12, s, sizeof(s)) &&
              !strcmp(s, "a\"b\\c/d"), "%s", s);
        const char *u = "\"Caf\\u00e9 \\u65e5\\u672c \\ud83c\\udfa7\"";
        CHECK(podfeeds_str(u, u + strlen(u), s, sizeof(s)) &&
              !strcmp(s, "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\xa7"), "%s", s);
        const char *lone = "\"x\\ud800y\"";
        CHECK(podfeeds_str(lone, lone + strlen(lone), s, sizeof(s)) && !strcmp(s, "x\xef\xbf\xbdy"),
              "lone surrogate: %s", s);
        const char *nl = "\"one\\ntwo\"";
        CHECK(podfeeds_str(nl, nl + strlen(nl), s, sizeof(s)) && !strcmp(s, "one two"), "%s", s);
        const char *raw = "\"\xe6\x97\xa5\xe6\x9c\xac\"";
        CHECK(podfeeds_str(raw, raw + strlen(raw), s, sizeof(s)) && !strcmp(s, "\xe6\x97\xa5\xe6\x9c\xac"),
              "raw UTF-8 passes through");
    }

    printf("  a long title is cut, never split\n");
    {
        char t[8];
        const char *r = "\"ab\xe6\x97\xa5\xe6\x9c\xac\"";     /* 2 + 3 + 3 bytes */
        CHECK(podfeeds_str(r, r + strlen(r), t, sizeof(t)) && !strcmp(t, "ab\xe6\x97\xa5"),
              "cut before the letter that does not fit: %zu", strlen(t));
        const char *e = "\"abcde\\u00e9\"";
        CHECK(podfeeds_str(e, e + strlen(e), t, 7) && !strcmp(t, "abcde"), "an escape that does not fit");
        char big[400];
        snprintf(big, sizeof(big), "{\"feeds\":[{\"id\":1,\"title\":\"%0200d\"}]}", 0);
        CHECK(PARSE(big, PODFEEDS_MAX) == 1 && strlen(f[0].title) == PODFEEDS_TITLE_MAX - 1,
              "kept, cut: %zu", strlen(f[0].title));
    }

    printf("  what is skipped, and where it stops\n");
    {
        const char *r = "{\"feeds\":[{\"id\":0,\"title\":\"zero\"},{\"title\":\"no id\"},"
                        "{\"id\":3,\"title\":\"\"},{\"id\":4,\"title\":null},{\"id\":5,\"title\":\"ok\"}]}";
        CHECK(PARSE(r, PODFEEDS_MAX) == 1 && f[0].id == 5, "only 5: %d", PARSE(r, PODFEEDS_MAX));
        CHECK(PARSE("{\"feeds\":[" FEED(1, "a") "," FEED(2, "b") ",{\"id\":3,\"title\":\"cut sh", PODFEEDS_MAX) == 2,
              "cut short: the whole feeds before it");
        CHECK(PARSE("{\"feeds\":[" FEED(1, "a") "," FEED(2, "b") "]}", 1) == 1, "max");
        CHECK(PARSE("{\"status\":\"false\",\"description\":\"no\"}", PODFEEDS_MAX) == 0, "no feeds");
        CHECK(PARSE("{\"feeds\":{}}", PODFEEDS_MAX) == 0, "not an array");
        CHECK(PARSE("<html>", PODFEEDS_MAX) == 0, "not JSON");
        CHECK(PARSE("", PODFEEDS_MAX) == 0, "empty");
        CHECK(PARSE("{\"feeds\":[{\"id\":12345678901,\"title\":\"big id\"}]}", PODFEEDS_MAX) == 1 &&
              f[0].id == 12345678901LL, "an id past 32 bits");
    }

    printf("  one feed, by its address (8128)\n");
    {
        podfeed_t one;
#define ONE(s) podfeeds_parse_one((s), strlen(s), &one)
        CHECK(ONE("{\"status\":\"true\",\"query\":{\"url\":\"https://x/a.xml\"},\"feed\":{\"id\":920666,"
                  "\"title\":\"99% Invisible\",\"categories\":{\"104\":\"Tv\"}},\"description\":\"Found matching feeds.\"}")
              && one.id == 920666 && !strcmp(one.title, "99% Invisible"), "the reply as sent");
        CHECK(!ONE("{\"status\":\"false\",\"query\":{},\"feed\":[],\"description\":\"No feeds match\"}"), "feed:[] is not found");
        CHECK(!ONE("{\"status\":\"true\",\"feed\":{\"id\":0,\"title\":\"x\"}}"), "no id");
        CHECK(!ONE("{\"feed\":{\"id\":5,\"title\":\"\"}}"), "no title");
        CHECK(!ONE("{\"query\":{\"feed\":{\"id\":5,\"title\":\"nested\"}}}"), "only the top level's feed");
        CHECK(!ONE("{\"feed\":{\"id\":5,\"title\":\"cut sh"), "cut short");
        CHECK(!ONE("<html>") && !ONE(""), "not JSON");
        CHECK(podfeeds_url_ok("https://example.com/feed.xml") && podfeeds_url_ok("http://a.b/c?d=e&f=g"), "urls");
        CHECK(!podfeeds_url_ok("ftp://example.com/feed") && !podfeeds_url_ok("example.com/feed.xml") &&
              !podfeeds_url_ok("https://") && !podfeeds_url_ok("https://a b.com/x") &&
              !podfeeds_url_ok("https://a.com/x\n") && !podfeeds_url_ok("") && !podfeeds_url_ok(NULL), "not urls");
        char longu[PODFEEDS_URL_MAX + 20];
        memset(longu, 'a', sizeof(longu));
        memcpy(longu, "https://", 8);
        longu[PODFEEDS_URL_MAX - 1] = '\0';
        CHECK(podfeeds_url_ok(longu), "the longest");
        longu[PODFEEDS_URL_MAX - 1] = 'a'; longu[PODFEEDS_URL_MAX] = '\0';
        CHECK(!podfeeds_url_ok(longu), "one too long");
        char e[64];
        CHECK(podfeeds_urlenc("https://a.b/c?d=e&f=g h", e, sizeof(e)) &&
              !strcmp(e, "https%3A%2F%2Fa.b%2Fc%3Fd%3De%26f%3Dg%20h"), "encoded: %s", e);
        CHECK(podfeeds_urlenc("a-b_c.d~E9", e, sizeof(e)) && !strcmp(e, "a-b_c.d~E9"), "unreserved stay");
        CHECK(podfeeds_urlenc("\xe6\x97\xa5", e, sizeof(e)) && !strcmp(e, "%E6%97%A5"), "UTF-8 bytes");
        CHECK(!podfeeds_urlenc("abcd", e, 4) && podfeeds_urlenc("abc", e, 4), "the terminator fits");
        CHECK(!podfeeds_urlenc("a:", e, 4) && podfeeds_urlenc("a:", e, 5), "an escape fits whole or not at all");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
