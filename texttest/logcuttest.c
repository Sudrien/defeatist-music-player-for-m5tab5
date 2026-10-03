/*
 * logcuttest.c -- logcut.h: a log line's cut on a character boundary.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/logcut.h"

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

/* Whether s[0..n) is whole UTF-8: every sequence complete. */
static int whole(const char *s, int n)
{
    for (int i = 0; i < n; ) {
        const unsigned char c = (unsigned char)s[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return 0;
        for (int k = 1; k < len; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        i += len;
    }
    return 1;
}

int main(void)
{
    CHECK(logcut_len("abc", 80) == 3 && !*logcut_more("abc", 3), "short: whole, no dots");
    CHECK(logcut_len("abcdef", 3) == 3 && !strcmp(logcut_more("abcdef", 3), "..."), "ASCII at the limit");
    CHECK(logcut_len(NULL, 5) == 0 && !*logcut_more(NULL, 0), "NULL");
    CHECK(logcut_len("abc", 0) == 0, "zero");

    /* The board's title: Arabic (2 bytes a letter) with ASCII spaces and
     * hyphens. At every limit the cut is whole, at most one byte short,
     * and says when something was left out. */
    const char *t = "\xd8\xb9\xd9\x8a\xd8\xb3\xd9\x89 \xd8\xa7\xd9\x84\xd8\xaf\xd8\xba\xd9\x85\xd8\xb1\xd9\x8a"
                    " - \xd8\xb1\xd9\x8a\xd8\xa7\xd8\xad \xd8\xa7\xd9\x84\xd9\x86\xd8\xb5\xd8\xb1";
    const int L = (int)strlen(t);
    int bad = 0;
    for (int m = 1; m <= L + 2; m++) {
        const int n = logcut_len(t, m);
        if (!whole(t, n) || n > m || (m <= L && n < m - 1) ||
            (n < L) != (strcmp(logcut_more(t, n), "...") == 0)) bad++;
    }
    CHECK(!bad, "Arabic: %d limits wrong", bad);

    /* 3- and 4-byte sequences step back up to three. */
    const char *cjk = "\xe4\xb8\x80\xe4\xb8\x80";        /* two CJK */
    CHECK(logcut_len(cjk, 4) == 3 && logcut_len(cjk, 5) == 3 && logcut_len(cjk, 6) == 6, "3-byte");
    const char *emo = "a\xf0\x9f\x8e\xb5";                /* a + U+1F3B5 */
    CHECK(logcut_len(emo, 2) == 1 && logcut_len(emo, 4) == 1 && logcut_len(emo, 5) == 5, "4-byte");

    /* Not UTF-8 (Latin-1): cut where the bytes say, never longer. */
    const char *lat = "caf\xe9 au lait";
    CHECK(logcut_len(lat, 4) == 4 && logcut_len(lat, 6) == 6, "Latin-1 is bytes");

    printf("logcuttest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
