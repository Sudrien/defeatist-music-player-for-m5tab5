/*
 * netbudget.h -- how many lwIP sockets this firmware can have open, and
 * who is holding them.
 *
 * `CONFIG_LWIP_MAX_SOCKETS` is a global ceiling on OPEN sockets, shared
 * by every server and every client in the image, and nothing at runtime
 * reports how close to it the device is. What happens at the ceiling is
 * an `accept()` or a `connect()` that fails somewhere unrelated to
 * whatever filled it -- so the budget has to be arithmetic written down
 * rather than a number someone raised until the symptom went away.
 *
 * The arithmetic is here, and the `_Static_assert` at the bottom makes
 * the compiler check it against the configured value. That assert is
 * also THE PROOF THE SDKCONFIG VALUE TOOK, which `CLAUDE.md` requires of
 * a patch touching `sdkconfig.defaults`: a stale `sdkconfig` -- the thing
 * that made 5033 "not working" -- fails the build here instead of
 * producing a board that runs with the old ceiling and misbehaves later.
 * A build-time check is used rather than a boot log line because
 * 5149-5152 is emphatic that on this device a boot-time diagnostic is not
 * free and "it only logs" is not a reason to leave one in.
 *
 * WHAT AN httpd INSTANCE COSTS IS max_open_sockets + 3, NOT
 * max_open_sockets. Three are reserved for the server's own working --
 * the listening socket, the UDP control socket, and one more -- and
 * ESP-IDF says so in `esp_http_server.h` and enforces it per instance in
 * `httpd_main.c`:
 *
 *     if (HTTPD_MAX_SOCKETS < config->max_open_sockets + 3) {
 *         ESP_LOGE(TAG, "Config option max_open_sockets is too large ...
 *
 * MPD.md's socket census -- "portal httpd 4, portal DNS 1, remote HTTPS
 * 4, remote plain 2 -- eleven if everything were up" -- counts only the
 * client slots and so undercounts by three per server. The real worst
 * case is below, and it is well past eleven.
 *
 * IDF CHECKS EACH INSTANCE AND NOT THE SUM. Every server here passes its
 * own check at 10 sockets, because each asks for at most 4 + 3. It is the
 * total across instances that oversubscribes, and nothing anywhere
 * validates that -- which is why this file exists and why the failure it
 * prevents would have looked like a network fault.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * THE CENSUS. Every number is from the code, not from a note:
 *
 *   portal httpd      4 + 3 = 7    portal.c:619, cfg.max_open_sockets = 4
 *   portal DNS            = 1      portal.c:128, one UDP socket
 *   remote HTTPS      4 + 3 = 7    remote.c:59, REMOTE_SOCKETS = 4
 *   remote plain      2 + 3 = 5    remote.c:805, cfg.max_open_sockets = 2
 *
 * THE PORTAL AND THE PLAIN SERVER NEVER RUN TOGETHER: the plain server
 * exists to hand port 80 to the portal and steps aside while it is up
 * (`remote.c:912-913`), so there are two worst cases rather than one sum,
 * and the larger of the two is the budget.
 *
 *   during setup   portal 7 + DNS 1 + remote HTTPS 7   = 15
 *   afterwards     remote HTTPS 7 + remote plain 5     = 12
 *
 * Plus the clients, which are not servers and are easy to forget because
 * none of them is configured anywhere -- each is one socket while it is
 * doing something, and all three can be doing something during setup:
 *
 *   SNTP                  = 1      settings.c, wifi.c
 *   netstream             = 1      netstream.c, a stream playing
 *   radiobrowser          = 1      radiobrowser.c, a search
 */
#define NETBUDGET_SERVERS_WORST     (15)    /* the setup-phase case */
#define NETBUDGET_CLIENTS_WORST     (3)

/*
 * MPD's share: the listener, plus the clients it will serve.
 *
 * AN IDLING MPD CLIENT HOLDS ITS SOCKET INDEFINITELY, which is not a
 * quirk but the protocol working correctly -- `idle` is how every modern
 * client avoids polling, so a connected client is a permanently occupied
 * socket rather than a transient one (MPD.md, "`idle` is not optional").
 * So these are not shared with anything and cannot be counted as
 * transient the way the three clients above are.
 *
 * Three rather than MPD.md's two, because the realistic case is a phone,
 * a desktop client, and one left open somewhere -- and the cost of one
 * more is a socket table entry rather than a buffer.
 */
#define NETBUDGET_MPD_CLIENTS       (3)
#define NETBUDGET_MPD               (1 + NETBUDGET_MPD_CLIENTS)

/* What the firmware needs in the worst case. */
#define NETBUDGET_NEEDED            (NETBUDGET_SERVERS_WORST + \
                                     NETBUDGET_CLIENTS_WORST + \
                                     NETBUDGET_MPD)

/*
 * 22, and `sdkconfig.defaults` sets 24 -- two spare, deliberately, so
 * that adding one client somewhere does not need this file edited before
 * it can be tested. It is not more than that because a ceiling with room
 * for anything is a ceiling that stops catching a leak.
 *
 * THE COST OF RAISING IT is the static socket table, which is a small
 * struct per slot: fourteen more slots is well under 2 KB of internal
 * RAM. It is NOT a buffer per socket -- lwIP's receive window
 * (`CONFIG_LWIP_TCP_WND_DEFAULT`, 16384 here since 5098) is filled from
 * the pbuf pool on demand by connections that actually exist, so a
 * raised ceiling costs nothing until something opens a socket. That
 * asymmetry is why raising this is cheap and why it was worth doing
 * rather than budgeting MPD down to two clients.
 *
 * lwIP's own range is 1..253 and its Kconfig warns that above 61 the
 * build needs `FD_SETSIZE` raised to match; 24 is well below that, so
 * nothing else has to change.
 */
#if CONFIG_LWIP_MAX_SOCKETS < NETBUDGET_NEEDED
#error "CONFIG_LWIP_MAX_SOCKETS is below netbudget.h's census. If sdkconfig.defaults was just changed, `rm sdkconfig` and build again (CLAUDE.md); if a new server or client was added, update the census here."
#endif
