/*
 * podauth.h -- what Podcast Index asks of a request, and what it means
 * by a good answer.
 *
 * 8006. Every call to api.podcastindex.org carries four headers:
 *
 *   User-Agent      the software, as for radio-browser
 *   X-Auth-Key      the listener's API key
 *   X-Auth-Date     the time now, unix seconds, as decimal text
 *   Authorization   SHA-1 of key + secret + that same text, lower hex
 *
 * The secret is never sent; the hash proves it. The date is why the
 * clock matters: the service refuses a date a few minutes from its own,
 * in the same words it refuses a wrong secret.
 *
 * The key is per listener, not per firmware. Podcast Index gives one to
 * anyone who signs up, the secret IS the authentication, and this
 * project is open source -- a key built into it would be everyone's,
 * share one rate limit, and be revoked for everyone at once.
 *
 * SHA-1 is here, sixty lines of it, rather than from Mbed TLS: IDF 5's
 * mbedtls_sha1() and IDF 6's PSA differ (approve.c carries that split
 * for SHA-256), PSA_WANT_ALG_SHA_1 is a config choice this repository
 * does not make, and a header this small is tested on the host whole.
 * It authenticates a request and protects nothing at rest, which is the
 * one use SHA-1's weaknesses do not reach.
 *
 * NO FREERTOS AND NO ALLOCATION, so podauthtest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jsonpick.h"

/* Podcast Index's are 20 and 40 characters; room to spare in case that
 * changes, and a bound on what the form will accept. */
#define PODAUTH_KEY_MAX     64
#define PODAUTH_SECRET_MAX  128
#define PODAUTH_HEX         41          /* 40 hex digits and the NUL */
#define PODAUTH_DATE        21          /* an int64 as text */

/* ---- SHA-1 (FIPS 180-4), small and slow, which is plenty ---- */

typedef struct {
    uint32_t h[5];
    uint64_t len;           /* bytes so far */
    uint8_t  buf[64];
    size_t   used;
} podauth_sha1_t;

static inline uint32_t podauth_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static inline void podauth_sha1_block(podauth_sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = podauth_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        const uint32_t t = podauth_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = podauth_rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static inline void podauth_sha1_init(podauth_sha1_t *s)
{
    static const uint32_t iv[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                                    0x10325476u, 0xC3D2E1F0u };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->used = 0;
}

static inline void podauth_sha1_add(podauth_sha1_t *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { podauth_sha1_block(s, s->buf); s->used = 0; }
    }
}

static inline void podauth_sha1_end(podauth_sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->len * 8;
    const uint8_t one = 0x80, zero = 0;
    podauth_sha1_add(s, &one, 1);
    while (s->used != 56) podauth_sha1_add(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    podauth_sha1_add(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
    memset(s, 0, sizeof(*s));       /* it held the secret */
}

/* ---- the request ---- */

/*
 * A key or a secret as the form will take it: 1 to max-1 characters of
 * printable ASCII with no space. Podcast Index's are letters, digits and
 * a little punctuation; anything else is a paste that picked up a
 * newline or a phone's smart punctuation, and is refused here rather
 * than sent and refused there, where it looks like a wrong secret.
 */
static inline bool podauth_text_ok(const char *s, size_t max)
{
    if (!s || !s[0]) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        const unsigned char c = (unsigned char)s[n];
        if (c <= ' ' || c > '~') return false;
    }
    return n < max;
}

/*
 * The date and Authorization headers for `epoch`, into `date` (as text)
 * and `hex`. False if the key or secret would not be accepted above.
 */
static inline bool podauth_sign(const char *key, const char *secret, int64_t epoch,
                                char date[PODAUTH_DATE], char hex[PODAUTH_HEX])
{
    if (!podauth_text_ok(key, PODAUTH_KEY_MAX) ||
        !podauth_text_ok(secret, PODAUTH_SECRET_MAX)) return false;
    snprintf(date, PODAUTH_DATE, "%lld", (long long)epoch);
    podauth_sha1_t s;
    podauth_sha1_init(&s);
    podauth_sha1_add(&s, key, strlen(key));
    podauth_sha1_add(&s, secret, strlen(secret));
    podauth_sha1_add(&s, date, strlen(date));
    uint8_t d[20];
    podauth_sha1_end(&s, d);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[2 * i]     = digits[d[i] >> 4];
        hex[2 * i + 1] = digits[d[i] & 15];
    }
    hex[40] = '\0';
    return true;
}

/*
 * Whether a 200 body is an answer: every Podcast Index reply carries
 * "status":"true" -- the STRING true -- and "false" with a description
 * when it is not. jsonpick's no-depth caveat does not bite: the
 * top-level status comes before the feeds array, and categories' items
 * are id and name only.
 */
static inline bool podauth_reply_ok(const char *body, size_t n)
{
    char v[8];
    return jsonpick_string(body, n, "status", v, sizeof(v)) && strcmp(v, "true") == 0;
}
