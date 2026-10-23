/*
 * podauthtest.c -- Podcast Index's request signing and reply check
 * (8006), against FIPS 180's SHA-1 vectors and one header worked by
 * Python's hashlib.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "podauth.h"

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

static void hexof(const void *in, size_t n, size_t reps, char out[41])
{
    podauth_sha1_t s;
    podauth_sha1_init(&s);
    for (size_t r = 0; r < reps; r++) podauth_sha1_add(&s, in, n);
    uint8_t d[20];
    podauth_sha1_end(&s, d);
    for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

int main(void)
{
    char h[41];
    printf("podauthtest\n");

    printf("  SHA-1\n");
    hexof("", 0, 1, h);
    CHECK(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty: %s", h);
    hexof("abc", 3, 1, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"), "abc: %s", h);
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        hexof(m, strlen(m), 1, h);      /* 56 bytes: the padding spills a block */
        CHECK(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "448 bits: %s", h);
    }
    hexof("aaaaaaaaaa", 10, 100000, h);
    CHECK(!strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "a million a: %s", h);

    printf("  the headers\n");
    {
        char date[PODAUTH_DATE], hex[PODAUTH_HEX];
        CHECK(podauth_sign("UXKCGDSYGUUEVQJSYDZH", "yHNWNE7Bm89G#mCcK2N2zzcj7Ak5sF8Q4dSGrpxd",
                           1700000000, date, hex), "a good pair signs");
        CHECK(!strcmp(date, "1700000000"), "date: %s", date);
        CHECK(!strcmp(hex, "8811cee97b75f383edcdcc9b5097445e31c1ba00"), "auth: %s", hex);
    }

    printf("  what the form refuses\n");
    CHECK(!podauth_text_ok("", PODAUTH_KEY_MAX), "empty");
    CHECK(!podauth_text_ok("ABC DEF", PODAUTH_KEY_MAX), "a space");
    CHECK(!podauth_text_ok("ABCDEF\n", PODAUTH_KEY_MAX), "a pasted newline");
    CHECK(!podauth_text_ok("ab\xe2\x80\x99" "cd", PODAUTH_KEY_MAX), "smart punctuation");
    {
        char big[PODAUTH_KEY_MAX + 1];
        memset(big, 'A', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
        CHECK(!podauth_text_ok(big, PODAUTH_KEY_MAX), "too long");
        big[PODAUTH_KEY_MAX - 1] = '\0';
        CHECK(podauth_text_ok(big, PODAUTH_KEY_MAX), "the longest that fits");
    }

    printf("  the reply\n");
    {
        const char *ok = "{\"status\":\"true\",\"feeds\":[{\"id\":1,\"name\":\"Arts\"}],"
                         "\"count\":1,\"description\":\"Categories\"}";
        const char *no = "{\"status\":\"false\",\"description\":\"Authorization header missing\"}";
        const char *nostatus = "{\"feeds\":[]}";
        const char *boolean = "{\"status\":true}";
        CHECK(podauth_reply_ok(ok, strlen(ok)), "an answer");
        CHECK(!podauth_reply_ok(no, strlen(no)), "status false");
        CHECK(!podauth_reply_ok(nostatus, strlen(nostatus)), "no status");
        CHECK(!podauth_reply_ok(boolean, strlen(boolean)), "a JSON true is not their \"true\"");
        CHECK(!podauth_reply_ok("<html>", 6), "a captive portal's page");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
