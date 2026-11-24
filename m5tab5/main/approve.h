/*
 * approve.h -- browsers the listener has allowed on the remote (8002).
 *
 * The remote has no password (remote.h). Since 8001 only the player's
 * own network reaches it; since this, only a browser the listener has
 * allowed ON THE PLAYER uses it. An unknown browser gets the page, which
 * asks; the player shows the address and a four-digit code with Allow
 * and Deny; on Allow the page is given a cookie, and that cookie is the
 * browser from then on. The rules -- one request at a time, the code,
 * how long each step waits -- are approvelist.h's.
 *
 * THREADS. The httpd task asks and collects. ui_task shows the prompt
 * and answers it, and is the only task that writes NVS here: the httpd
 * task's stack is in PSRAM (remote.c, 6005), and a task on a PSRAM
 * stack must never run with the cache off. Everything in between is
 * under one mutex, never held across a flash write.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "approvelist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The cookie, and its value's length: the token as hex. */
#define APPROVE_COOKIE      "dmp_ok"
#define APPROVE_TICKET_COOKIE "dmp_ask"
#define APPROVE_HEX         (2 * APPROVE_TOKEN)

/* Once, from app_main(), after NVS and before remote_init(). */
void approve_init(void);

/* ---- the httpd task ---- */

/* Whether a cookie's token is one the listener allowed. */
bool approve_browser_ok(const char *hex);

/* A request from `addr` (16 bytes, IPv4-mapped for IPv4), collected
 * later by `ticket`. The code to show the asker comes back in `code`. */
approve_ask_t approve_browser_ask(const uint8_t addr[APPROVE_ADDR],
                                  const uint8_t ticket[APPROVE_TICKET], char code[5]);

/* How a ticket's request went. On APPROVE_YES, `hex` is the token for
 * the cookie, once. On APPROVE_WAITING, `code` is the code again. */
approve_state_t approve_browser_collect(const uint8_t ticket[APPROVE_TICKET],
                                        char hex[APPROVE_HEX + 1], char code[5]);

/* A fresh ticket. */
void approve_new_ticket(uint8_t ticket[APPROVE_TICKET]);

/* ---- ui_task ---- */

/*
 * Whether a request is waiting for the listener, and what to show:
 * `who` is the asker's address, `code` its code. `fresh` is set on the
 * first call that sees this request, so the caller can wake the screen
 * once rather than every pass; a caller passing NULL (the drawing) does
 * not use that up.
 */
bool approve_prompt(approve_kind_t *kind, char *who, size_t who_size,
                    char code[5], bool *fresh);

/* The listener's tap. On yes, a browser's token is made and saved here. */
void approve_answer(bool yes);

/* 8003: how many are allowed, and forgetting them all. Each forgotten
 * browser asks again the next time it opens the page. 8004: MPD
 * addresses are counted and forgotten with them. */
int  approve_count(void);
void approve_forget_all(void);

/* ---- 8004: the MPD task ----
 *
 * An MPD client is allowed by its address: it has no cookie to carry and
 * no screen to show a code on, so the prompt shows only the address.
 * `addr` is 16 bytes, IPv4-mapped. The address is the request's ticket
 * too, so a client that reconnects while waiting is the same request. */
bool            approve_mpd_ok(const uint8_t addr[APPROVE_ADDR]);
approve_ask_t   approve_mpd_ask(const uint8_t addr[APPROVE_ADDR]);
approve_state_t approve_mpd_collect(const uint8_t addr[APPROVE_ADDR]);

/* ---- u-defeatist-0050: the open hotspot's phones, by MAC ----
 * The player's loop (hotspot.c) asks for a phone that joined, collects the
 * answer, and sends away one refused. The prompt shows the MAC. Only a MAC
 * is seen at that layer; a phone keeps one per network it has joined
 * (iOS's and Android's private address), so it is asked once. Counted and
 * forgotten apart from the browsers and MPD apps. The address form is the
 * MAC in the first 6 bytes and 0xee after, which no IPv4-mapped or real
 * IPv6 address in use here is. */
void            approve_hotspot_addr(const uint8_t mac[6], uint8_t addr[APPROVE_ADDR]);
bool            approve_hotspot_ok(const uint8_t mac[6]);
approve_ask_t   approve_hotspot_ask(const uint8_t mac[6]);
approve_state_t approve_hotspot_collect(const uint8_t mac[6]);
int             approve_hotspot_count(void);
void            approve_hotspot_forget(void);

#ifdef __cplusplus
}
#endif
