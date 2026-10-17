/*
 * lanpeer.h -- who the browser remote answers: a peer on the same
 * network as the interface it reached, from a page the remote served.
 *
 * Two questions, both pure, so texttest/lanpeertest.c compiles this
 * header rather than transcribing it. remote.c asks the network for the
 * addresses and masks and hands them here.
 *
 * SAME NETWORK. An IPv4 peer is on the network when it agrees with the
 * address it connected to under that interface's netmask. An IPv6 peer
 * is on the network when it is link-local (fe80::/10, on the link by
 * definition) or shares the first 64 bits with one of the interface's
 * own addresses -- the /64 every SLAAC network uses. A peer behind a
 * router, a VPN or a port forward fails both, which is the point: the
 * remote has no password (remote.h), so the network is the password,
 * and "the network" has to mean the one the player is on, not any
 * network that can route to it.
 *
 * Refused rather than allowed when the answer cannot be known: an
 * interface with no address or a zero mask is not a network anything
 * can be on.
 *
 * SAME ORIGIN. A browser sends Origin on a WebSocket handshake and on a
 * POST. When it is there it must be exactly https:// plus the Host the
 * request was sent to, compared without regard to case: the remote's
 * own page, at whatever name or address the browser used to reach it.
 * Anything else is another site's page using the visitor's browser as
 * its way onto this network. "null" (a sandboxed frame, a file:// page)
 * is refused. An absent Origin is allowed: that is a client that is not
 * a browser, which can send whatever it likes anyway and is not what
 * this check is for. The subnet check above is what applies to it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Addresses in network byte order, as they sit in a sockaddr. */
static inline bool lanpeer_v4_on_link(const uint8_t peer[4],
                                      const uint8_t mine[4],
                                      const uint8_t mask[4])
{
    if (!peer || !mine || !mask) return false;
    bool mask_zero = true, mine_zero = true;
    for (int i = 0; i < 4; i++) {
        if (mask[i]) mask_zero = false;
        if (mine[i]) mine_zero = false;
    }
    if (mask_zero || mine_zero) return false;
    for (int i = 0; i < 4; i++) {
        if ((peer[i] & mask[i]) != (mine[i] & mask[i])) return false;
    }
    return true;
}

static inline bool lanpeer_v6_link_local(const uint8_t a[16])
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

/* `mine` is the interface's addresses, `n` of them. */
static inline bool lanpeer_v6_on_link(const uint8_t peer[16],
                                      const uint8_t (*mine)[16], int n)
{
    if (!peer) return false;
    if (lanpeer_v6_link_local(peer)) return true;
    for (int i = 0; mine && i < n; i++) {
        if (lanpeer_v6_link_local(mine[i])) continue;
        if (memcmp(peer, mine[i], 8) == 0) return true;
    }
    return false;
}

/*
 * The IPv4 address inside an IPv4-mapped IPv6 one (::ffff:a.b.c.d),
 * which is how an IPv4 peer arrives on a dual-stack socket. NULL if `a`
 * is not one.
 */
static inline const uint8_t *lanpeer_v4_mapped(const uint8_t a[16])
{
    static const uint8_t pre[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    return memcmp(a, pre, 12) == 0 ? a + 12 : NULL;
}

/* `origin` NULL or "" is a request with no Origin header. */
static inline bool lanpeer_origin_ok(const char *origin, const char *host)
{
    if (!origin || !origin[0]) return true;
    if (!host || !host[0]) return false;
    static const char scheme[] = "https://";
    const size_t sl = sizeof(scheme) - 1;
    if (strncasecmp(origin, scheme, sl) != 0) return false;
    return strcasecmp(origin + sl, host) == 0;
}
