/*
 * lanpeertest.c -- who the browser remote answers (8001), compiled from
 * main/lanpeer.h rather than transcribed.
 *
 * What remote.c does around it -- getsockname(), getpeername(), finding
 * the interface -- runs on the board. This is the decision.
 *
 * The cases lean on what must be REFUSED: a refusal too many is a
 * remote that will not load, which is visible and gets reported; an
 * acceptance too many is the remote answering the internet, which
 * nobody notices.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "lanpeer.h"

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

#define V4(a, b, c, d) ((const uint8_t[4]){ (a), (b), (c), (d) })

static void v4(void)
{
    const uint8_t *me = V4(192, 168, 1, 50), *m24 = V4(255, 255, 255, 0);

    CHECK(lanpeer_v4_on_link(V4(192, 168, 1, 7), me, m24), "same /24");
    CHECK(lanpeer_v4_on_link(V4(192, 168, 1, 50), me, m24), "itself");
    CHECK(lanpeer_v4_on_link(V4(192, 168, 1, 255), me, m24), "broadcast byte still on the subnet");
    CHECK(!lanpeer_v4_on_link(V4(192, 168, 2, 7), me, m24), "neighbouring /24");
    CHECK(!lanpeer_v4_on_link(V4(8, 8, 8, 8), me, m24), "the internet");
    CHECK(!lanpeer_v4_on_link(V4(10, 0, 0, 5), me, m24), "another private range");

    /* The mask is the interface's, not assumed to be /24. */
    const uint8_t *m16 = V4(255, 255, 0, 0);
    CHECK(lanpeer_v4_on_link(V4(192, 168, 200, 1), me, m16), "same /16");
    CHECK(!lanpeer_v4_on_link(V4(192, 169, 1, 50), me, m16), "outside the /16");
    const uint8_t *m26 = V4(255, 255, 255, 192);
    CHECK(lanpeer_v4_on_link(V4(192, 168, 1, 63), me, m26), "top of a /26");
    CHECK(!lanpeer_v4_on_link(V4(192, 168, 1, 64), me, m26), "next /26");

    /* Nothing known is nothing allowed. */
    CHECK(!lanpeer_v4_on_link(V4(1, 2, 3, 4), me, V4(0, 0, 0, 0)), "zero mask");
    CHECK(!lanpeer_v4_on_link(V4(0, 0, 0, 1), V4(0, 0, 0, 0), m24), "no address");
    CHECK(!lanpeer_v4_on_link(NULL, me, m24), "NULL peer");
}

static void v6(void)
{
    const uint8_t mine[2][16] = {
        { 0xfe,0x80,0,0,0,0,0,0, 0x02,0x11,0x22,0xff,0xfe,0x33,0x44,0x55 },
        { 0x20,0x01,0x0d,0xb8,0x00,0x01,0x00,0x02, 0x02,0x11,0x22,0xff,0xfe,0x33,0x44,0x55 },
    };
    const uint8_t ll[16]    = { 0xfe,0x80,0,0,0,0,0,0, 1,2,3,4,5,6,7,8 };
    const uint8_t febf[16]  = { 0xfe,0xbf,0,0,0,0,0,0, 1,2,3,4,5,6,7,8 };
    const uint8_t fec0[16]  = { 0xfe,0xc0,0,0,0,0,0,0, 1,2,3,4,5,6,7,8 };
    const uint8_t same64[16]= { 0x20,0x01,0x0d,0xb8,0x00,0x01,0x00,0x02, 9,9,9,9,9,9,9,9 };
    const uint8_t other[16] = { 0x20,0x01,0x0d,0xb8,0x00,0x01,0x00,0x03, 9,9,9,9,9,9,9,9 };

    CHECK(lanpeer_v6_on_link(ll, mine, 2), "link-local");
    CHECK(lanpeer_v6_on_link(febf, mine, 2), "top of fe80::/10");
    CHECK(!lanpeer_v6_on_link(fec0, mine, 2), "fec0:: is not link-local");
    CHECK(lanpeer_v6_on_link(same64, mine, 2), "same /64 as a global address");
    CHECK(!lanpeer_v6_on_link(other, mine, 2), "next /64");
    /* A link-local of ours is not a prefix to share. */
    CHECK(!lanpeer_v6_on_link(other, mine, 1), "only a link-local of ours");
    CHECK(!lanpeer_v6_on_link(same64, NULL, 0), "no addresses of ours");

    const uint8_t mapped[16] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff, 192,168,1,7 };
    const uint8_t *m = lanpeer_v4_mapped(mapped);
    CHECK(m && m[0] == 192 && m[3] == 7, "IPv4-mapped unwraps");
    CHECK(!lanpeer_v4_mapped(same64), "a real IPv6 address is not mapped");
    const uint8_t compat[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 192,168,1,7 };
    CHECK(!lanpeer_v4_mapped(compat), "::a.b.c.d is not ::ffff:a.b.c.d");
}

static void origin(void)
{
    CHECK(lanpeer_origin_ok(NULL, "192.168.1.50"), "no Origin: not a browser");
    CHECK(lanpeer_origin_ok("", "192.168.1.50"), "empty Origin");
    CHECK(lanpeer_origin_ok("https://192.168.1.50", "192.168.1.50"), "own page by address");
    CHECK(lanpeer_origin_ok("https://defeatist-1a2b.local", "Defeatist-1A2B.local"), "own page by name, any case");
    CHECK(lanpeer_origin_ok("HTTPS://192.168.1.50", "192.168.1.50"), "scheme case");
    CHECK(lanpeer_origin_ok("https://192.168.1.50:8443", "192.168.1.50:8443"), "a port on both");
    CHECK(lanpeer_origin_ok("https://[fe80::1]", "[fe80::1]"), "IPv6 literal");

    CHECK(!lanpeer_origin_ok("https://evil.example", "192.168.1.50"), "another site");
    CHECK(!lanpeer_origin_ok("http://192.168.1.50", "192.168.1.50"), "the plain-HTTP page");
    CHECK(!lanpeer_origin_ok("null", "192.168.1.50"), "null origin");
    CHECK(!lanpeer_origin_ok("https://192.168.1.50.evil.example", "192.168.1.50"), "suffix trick");
    CHECK(!lanpeer_origin_ok("https://192.168.1.5", "192.168.1.50"), "prefix trick");
    CHECK(!lanpeer_origin_ok("https://192.168.1.50", "192.168.1.5"), "longer than Host");
    CHECK(!lanpeer_origin_ok("https://192.168.1.50:444", "192.168.1.50"), "a different port");
    CHECK(!lanpeer_origin_ok("https://192.168.1.50", ""), "no Host");
    CHECK(!lanpeer_origin_ok("https://192.168.1.50", NULL), "NULL Host");
    CHECK(!lanpeer_origin_ok("https://", ""), "bare scheme");
}

int main(void)
{
    printf("lanpeertest\n");
    v4();
    v6();
    origin();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
