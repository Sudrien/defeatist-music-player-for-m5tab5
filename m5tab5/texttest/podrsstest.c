/*
 * podrsstest.c -- a podcast's own RSS feed as rows (8130).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "podrss.h"

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

static podep_t e[PODEPS_MAX], scratch;
static podrss_info_t info;
#define PARSE(s, max) podrss_parse((s), strlen(s), &info, e, (max), &scratch)

#define ITEM(title, date, url) \
    "<item><title>" title "</title><guid isPermaLink=\"false\">g-" title "</guid><pubDate>" date "</pubDate>" \
    "<enclosure url=\"" url "\" length=\"1234\" type=\"audio/mpeg\"/><itunes:duration>50:21</itunes:duration></item>"

int main(void)
{
    printf("podrsstest\n");

    printf("  a feed as sent\n");
    {
        const char *r =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- made by hand -->\n"
            "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\">\n<channel>\n"
            "<title>Caf&#233; &amp; Co</title>\n<link>https://x/</link>\n"
            "<image><url>https://x/not-this.png</url><title>not this title</title></image>\n"
            "<itunes:image href=\"https://x/cover.jpg\"/>\n"
            "<item>\n<title><![CDATA[Episode <2>]]></title>\n<guid>abc-2</guid>\n"
            "<pubDate>Wed, 04 Nov 2026 08:00:00 +0100</pubDate>\n"
            "<enclosure url=\"https://cdn.x/ep2.mp3?a=1&amp;b=2\" length=\"583650930\" type=\"audio/mpeg\"/>\n"
            "<itunes:duration>3021</itunes:duration><itunes:image href=\"https://x/ep2.jpg\"/>\n"
            "<description><![CDATA[<p>Hello &amp; <b>welcome</b>.</p><p>Second.</p>]]></description></item>\n"
            "<item><title>Episode 1</title><guid>abc-1</guid><pubDate>Tue, 03 Nov 2026 08:00:00 GMT</pubDate>\n"
            "<enclosure url='http://cdn.x/ep1.m4a' length='99'/>\n"
            "<description>&lt;p&gt;Escaped &amp;amp; fine&lt;/p&gt;</description></item>\n"
            "</channel></rss>";
        const int n = PARSE(r, PODEPS_MAX);
        CHECK(n == 2, "two items: %d", n);
        CHECK(!strcmp(info.title, "Caf\xc3\xa9 & Co"), "channel title, not the image's: %s", info.title);
        CHECK(!strcmp(info.image, "https://x/cover.jpg"), "cover: %s", info.image);
        CHECK(!strcmp(e[0].title, "Episode <2>"), "CDATA title: %s", e[0].title);
        CHECK(!strcmp(e[0].url, "https://cdn.x/ep2.mp3?a=1&b=2"), "attribute entity: %s", e[0].url);
        CHECK(e[0].size == 583650930LL && e[0].duration == 3021, "size and duration");
        CHECK(e[0].published == 1793775600LL, "date with an offset: %lld", (long long)e[0].published);
        CHECK(!strcmp(e[0].desc, "Hello & welcome .\nSecond."), "description: [%s]", e[0].desc);
        CHECK(!strcmp(e[0].image, "https://x/ep2.jpg") && !strcmp(e[0].feed_image, "https://x/cover.jpg"), "pictures");
        CHECK(e[0].id > 0 && e[0].id < ((int64_t)1 << 48) && e[0].id != e[1].id, "ids");
        CHECK(!strcmp(e[1].url, "http://cdn.x/ep1.m4a") && e[1].published == 1793692800LL, "single quotes, GMT: %lld", (long long)e[1].published);
        CHECK(!strcmp(e[1].desc, "Escaped & fine"), "escaped HTML: [%s]", e[1].desc);
        CHECK(podrss_ep_id("abc-2") == e[0].id, "the id is the guid's hash");
    }

    printf("  what is skipped, and the order\n");
    {
        char big[8192] = "<rss><channel><title>T</title>";
        strcat(big, "<item><title>no enclosure</title></item>");
        strcat(big, "<item><title>ftp</title><enclosure url=\"ftp://x/a.mp3\"/></item>");
        strcat(big, "<item><enclosure url=\"https://x/untitled.mp3\"/></item>");
        strcat(big, ITEM("ok", "Mon, 02 Nov 2026 00:00:00 GMT", "https://x/ok.mp3"));
        strcat(big, "</channel></rss>");
        CHECK(PARSE(big, PODEPS_MAX) == 1 && !strcmp(e[0].title, "ok"), "only the whole item");

        char old[8192] = "<rss><channel><title>T</title>";
        const char *days[] = { "01", "02", "03", "04", "05" };
        for (int i = 0; i < 5; i++) {
            char t[16], d[48], u[48];
            snprintf(t, sizeof(t), "ep%s", days[i]);
            snprintf(d, sizeof(d), "Mon, %s Oct 2026 00:00:00 GMT", days[i]);
            snprintf(u, sizeof(u), "https://x/%s.mp3", days[i]);
            strcat(old, "<item><title>"); strcat(old, t); strcat(old, "</title><pubDate>"); strcat(old, d);
            strcat(old, "</pubDate><enclosure url=\""); strcat(old, u); strcat(old, "\"/></item>");
        }
        strcat(old, "</channel></rss>");
        const int n = PARSE(old, 3);
        CHECK(n == 3 && !strcmp(e[0].title, "ep05") && !strcmp(e[1].title, "ep04") && !strcmp(e[2].title, "ep03"),
              "oldest-first feed: the newest three, newest first (%d %s)", n, n ? e[0].title : "");
        char nodate[1024] = "<rss><channel><title>T</title>";
        strcat(nodate, "<item><title>a</title><enclosure url=\"https://x/a.mp3\"/></item>");
        strcat(nodate, "<item><title>b</title><enclosure url=\"https://x/b.mp3\"/></item>");
        strcat(nodate, "<item><title>c</title><enclosure url=\"https://x/c.mp3\"/></item></channel></rss>");
        CHECK(PARSE(nodate, 2) == 2 && !strcmp(e[0].title, "a") && !strcmp(e[1].title, "b"), "no dates: feed order, first ones kept");
    }

    printf("  a feed that is not, or is cut\n");
    {
        CHECK(PARSE("<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>x</title><entry><title>y</title></entry></feed>", PODEPS_MAX) == 0, "Atom");
        CHECK(PARSE("<html><body>no</body></html>", PODEPS_MAX) == 0 && PARSE("", PODEPS_MAX) == 0, "not a feed");
        const char *ok = "<rss><channel><title>T</title>"
                         ITEM("one", "Mon, 02 Nov 2026 00:00:00 GMT", "https://x/1.mp3")
                         ITEM("two", "Tue, 03 Nov 2026 00:00:00 GMT", "https://x/2.mp3");
        char cut[1024];
        snprintf(cut, sizeof(cut), "%s<item><title>three</title><enclosure url=\"https://x/3.m", ok);
        CHECK(PARSE(cut, PODEPS_MAX) == 2, "cut in the middle of a tag: the whole items before it");
        snprintf(cut, sizeof(cut), "%s<item><title>thr", ok);
        CHECK(PARSE(cut, PODEPS_MAX) == 2, "cut in a title");
        CHECK(PARSE(ok, PODEPS_MAX) == 2 && !strcmp(e[0].title, "two"), "no closing tags at all");
        CHECK(podrss_parse(NULL, 0, &info, e, 5, &scratch) == 0 && PARSE("<rss/>", 0) == 0, "NULL and zero");
    }

    printf("  a long title is cut at a letter\n");
    {
        char big[2048] = "<rss><channel><title>";
        for (int i = 0; i < 70; i++) strcat(big, "\xe6\x97\xa5");              /* 210 bytes */
        strcat(big, "</title></channel></rss>");
        PARSE(big, 1);
        CHECK(strlen(info.title) <= PODFEEDS_TITLE_MAX - 1 && strlen(info.title) % 3 == 0, "%zu", strlen(info.title));
    }

    printf("  dates, durations and ids\n");
    {
        CHECK(podrss_date("Thu, 01 Jan 1970 00:00:01 GMT") == 1, "epoch + 1");
        CHECK(podrss_date("04 Nov 2026 08:00") == 1793779200LL, "no weekday, no seconds, no zone: %lld", (long long)podrss_date("04 Nov 2026 08:00"));
        CHECK(podrss_date("Wed, 04 Nov 2026 08:00:00 -0500") == 1793779200LL + 5 * 3600, "negative offset");
        CHECK(podrss_date("Wed, 4 Nov 26 08:00:00 Z") == 1793779200LL, "two-digit year, Z");
        CHECK(podrss_date("Sat, 29 Feb 2028 12:00:00 GMT") == 1835438400LL, "leap day: %lld", (long long)podrss_date("Sat, 29 Feb 2028 12:00:00 GMT"));
        CHECK(podrss_date("") == 0 && podrss_date("garbage") == 0 && podrss_date("32 Jan 2026") == 0 && podrss_date("04 Foo 2026") == 0, "not dates");
        CHECK(podrss_duration("3021") == 3021 && podrss_duration("50:21") == 3021 && podrss_duration("1:02:03") == 3723, "durations");
        CHECK(podrss_duration("50:21.500") == 3021 && podrss_duration("") == 0 && podrss_duration("x") == 0, "fraction and junk");
        CHECK(podrss_feed_for("https://a/b") == podrss_feed_for("https://a/b") && podrss_feed_for("https://a/b") != podrss_feed_for("https://a/c"), "feed ids differ by address");
        CHECK(podrss_feed_id(podrss_feed_for("https://a/b")) && !podrss_feed_id(6654186) && podrss_feed_for("x") < ((int64_t)1 << 42), "the range");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
