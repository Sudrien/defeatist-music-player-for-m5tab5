/*
 * podwebtest.c -- the remote's podcast lists: when to ask, and the rows
 * as JSON (8033).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podweb.h"

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
    static char esc[PODWEB_ESC_MAX], row[PODWEB_ROW_MAX];
    printf("podwebtest\n");

    printf("  when a request fetches\n");
    {
        const podindex_cats_t all[] = { PODINDEX_CATS_NONE, PODINDEX_CATS_LOADING,
                                        PODINDEX_CATS_READY, PODINDEX_CATS_FAILED };
        for (int i = 0; i < 4; i++)
            for (int go = 0; go < 2; go++) {
                const podindex_cats_t st = all[i];
                CHECK(podweb_decide(go, st, false) ==
                      (st == PODINDEX_CATS_NONE || go ? PODWEB_FETCH : PODWEB_OTHER),
                      "not ours: state %d, go %d", (int)st, go);
            }
        CHECK(podweb_decide(false, PODINDEX_CATS_NONE, true) == PODWEB_FETCH, "nothing yet: fetch on a poll");
        CHECK(podweb_decide(false, PODINDEX_CATS_LOADING, true) == PODWEB_SHOW, "ours, loading: show");
        CHECK(podweb_decide(true, PODINDEX_CATS_LOADING, true) == PODWEB_SHOW, "ours, loading, go: no second fetch");
        CHECK(podweb_decide(true, PODINDEX_CATS_READY, true) == PODWEB_SHOW, "ours, ready, go: the list there is");
        CHECK(podweb_decide(false, PODINDEX_CATS_FAILED, true) == PODWEB_SHOW, "failed, a poll: say failed");
        CHECK(podweb_decide(true, PODINDEX_CATS_FAILED, true) == PODWEB_FETCH, "failed, asked again: fetch");
        CHECK(!strcmp(podweb_state_name(PODINDEX_CATS_READY), "ready") &&
              !strcmp(podweb_state_name(PODINDEX_CATS_FAILED), "failed") &&
              !strcmp(podweb_state_name(PODINDEX_CATS_LOADING), "loading"), "state names");
    }

    printf("  rows\n");
    {
        podcat_t c = { 2, "Arts" };
        size_t n = podweb_cat_row(&c, true, esc, row, sizeof(row));
        CHECK(n == strlen(row) && !strcmp(row, "{\"id\":2,\"t\":\"Arts\"}"), "a category: %s", row);
        n = podweb_cat_row(&c, false, esc, row, sizeof(row));
        CHECK(row[0] == ',' && n == strlen(row), "not first: a comma: %s", row);

        podfeed_t f = { 12345678901LL, "Say \"hi\"\\ \xe4\xb8\xad" };
        n = podweb_feed_row(&f, true, esc, row, sizeof(row));
        CHECK(n && !strcmp(row, "{\"id\":12345678901,\"t\":\"Say \\\"hi\\\"\\\\ \xe4\xb8\xad\"}"),
              "a podcast, escaped, an id past 32 bits: %s", row);

        static podep_t e;
        e.id = 4235; e.published = 1700000000; e.duration = 3021; e.size = 5000000000LL;
        snprintf(e.title, sizeof(e.title), "Ep 1");
        snprintf(e.url, sizeof(e.url), "https://x/ep.mp3");
        snprintf(e.desc, sizeof(e.desc), "not sent");
        n = podweb_ep_row(&e, true, esc, row, sizeof(row));
        CHECK(n && !strcmp(row, "{\"id\":4235,\"t\":\"Ep 1\",\"d\":1700000000,\"len\":3021,\"size\":5000000000}"),
              "an episode, a size past 4 GB, no URL or description: %s", row);

        /* The worst title: every byte a control character, six bytes each. */
        memset(e.title, 1, sizeof(e.title) - 1);
        e.title[sizeof(e.title) - 1] = '\0';
        e.id = INT64_MAX; e.published = INT64_MAX; e.duration = INT32_MAX; e.size = INT64_MAX;
        n = podweb_ep_row(&e, false, esc, row, sizeof(row));
        CHECK(n > 0 && n == strlen(row) && n < sizeof(row), "the longest row fits: %zu of %zu", n, sizeof(row));
        CHECK(podweb_ep_row(&e, false, esc, row, 40) == 0, "a row that does not fit: none, not half");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
