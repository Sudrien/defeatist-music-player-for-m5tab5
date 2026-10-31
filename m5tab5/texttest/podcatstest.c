/*
 * podcatstest.c -- Podcast Index's categories/list as rows (8014).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podcats.h"

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

#define PARSE(s, max) podcats_parse((s), strlen(s), c, (max))

int main(void)
{
    static podcat_t c[PODCATS_MAX];
    printf("podcatstest\n");

    printf("  the reply as sent\n");
    {
        const char *r = "{\"status\":\"true\",\"feeds\":[{\"id\":1,\"name\":\"Arts\"},"
                        "{\"id\":2,\"name\":\"Books\"},{\"id\":77,\"name\":\"Religion & Spirituality\"}],"
                        "\"count\":3,\"description\":\"Categories\"}";
        CHECK(PARSE(r, PODCATS_MAX) == 3, "three");
        CHECK(c[0].id == 1 && !strcmp(c[0].name, "Arts"), "%d %s", c[0].id, c[0].name);
        CHECK(c[2].id == 77 && !strcmp(c[2].name, "Religion & Spirituality"), "%s", c[2].name);
    }

    printf("  spacing, key order, escapes\n");
    {
        const char *r = "{ \"feeds\" : [ { \"name\" : \"Kids \\\"&\\\" Family\" , \"id\" : 12 } ,\n"
                        "  {\"id\":13,\"name\":\"a } b { c\"} ] }";
        CHECK(PARSE(r, PODCATS_MAX) == 2, "two");
        CHECK(c[0].id == 12 && !strcmp(c[0].name, "Kids \"&\" Family"), "%d %s", c[0].id, c[0].name);
        CHECK(c[1].id == 13 && !strcmp(c[1].name, "a } b { c"), "braces in a name: %s", c[1].name);
    }

    printf("  what is skipped, and where it stops\n");
    {
        const char *r = "{\"feeds\":[{\"id\":0,\"name\":\"zero\"},{\"name\":\"no id\"},"
                        "{\"id\":5,\"name\":null},{\"id\":6,\"name\":\"\"},"
                        "{\"id\":7,\"name\":\"\\\"id\\\":9\"},{\"id\":8,\"name\":\"ok\"}]}";
        CHECK(PARSE(r, PODCATS_MAX) == 2, "only 7 and 8: %d", PARSE(r, PODCATS_MAX));
        CHECK(c[0].id == 7, "an \"id\" inside a name is not the id: %d", c[0].id);
        CHECK(c[1].id == 8, "%d", c[1].id);
        CHECK(PARSE(r, 1) == 1, "max");
    }
    {
        char big[200];
        snprintf(big, sizeof(big), "{\"feeds\":[{\"id\":1,\"name\":\"%060d\"},{\"id\":2,\"name\":\"fits\"}]}", 0);
        CHECK(PARSE(big, PODCATS_MAX) == 1 && c[0].id == 2, "a name too long is skipped");
    }
    CHECK(PARSE("{\"status\":\"false\",\"description\":\"no\"}", PODCATS_MAX) == 0, "no feeds");
    CHECK(PARSE("{\"description\":\"feeds\",\"x\":1}", PODCATS_MAX) == 0, "\"feeds\" as a value");
    CHECK(PARSE("{\"feeds\":{\"id\":1}}", PODCATS_MAX) == 0, "feeds not an array");
    CHECK(PARSE("{\"feeds\":[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"x\":{\"y\":1}},{\"id\":3,\"name\":\"c\"}]}",
                PODCATS_MAX) == 1, "nested ends the walk, keeps what came before");
    CHECK(PARSE("{\"feeds\":[{\"id\":1,\"name\":\"trunc", PODCATS_MAX) == 0, "truncated");
    CHECK(PARSE("<html>", PODCATS_MAX) == 0, "not JSON");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
