/*
 * approvelist.h -- who the listener has allowed, and the one request
 * waiting to be allowed (8002).
 *
 * Pure, with the clock passed in, so texttest/approvelisttest.c
 * compiles this header rather than transcribing it. approve.c wraps it
 * in a mutex, NVS and a random source.
 *
 * THE LIST. Up to APPROVE_MAX keys of APPROVE_KEY bytes. For a browser
 * the key is SHA-256 of the token its cookie carries, so what NVS holds
 * cannot be replayed as a cookie. Full, a new key replaces the oldest:
 * the browser that loses its place asks again, which is visible, where
 * refusing the newest would be a phone that can never be added.
 *
 * NOT A MAC ADDRESS. Phones rotate theirs, and the player only sees a
 * MAC at all on its own link. Not an IP address for a browser either: a
 * new lease would be a new device, and the next device given the old
 * address would inherit the approval. The cookie is the browser itself,
 * across MACs and addresses, until its site data is cleared.
 *
 * THE REQUEST. One at a time. A second asker while one waits is told
 * the player is busy, not queued: a queue is a way to put a stranger's
 * request behind the listener's own, so that the tap meant for one
 * allows the other. The code (four digits, from the ticket) is shown on
 * the player and on the asking page for the same reason -- the listener
 * checks the two agree before allowing.
 *
 * A request waits APPROVE_ASK_US for an answer. An answer waits
 * APPROVE_HOLD_US for the asker to collect it. A refusal also refuses
 * that address's next requests for APPROVE_DENY_US, so a neighbour
 * cannot keep the prompt on the listener's screen by asking again.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APPROVE_MAX      (8)
#define APPROVE_KEY      (32)
#define APPROVE_TICKET   (16)
#define APPROVE_TOKEN    (16)
#define APPROVE_ADDR     (16)        /* IPv6, or IPv4-mapped */

#define APPROVE_ASK_US   (120 * 1000000LL)
#define APPROVE_HOLD_US  (60 * 1000000LL)
#define APPROVE_DENY_US  (600 * 1000000LL)

typedef struct {
    uint8_t  key[APPROVE_KEY];
    uint32_t seq;                    /* 0: empty; larger: newer */
} approve_slot_t;

typedef struct {
    approve_slot_t s[APPROVE_MAX];
} approve_list_t;

static inline int approve_list_find(const approve_list_t *l, const uint8_t key[APPROVE_KEY])
{
    for (int i = 0; i < APPROVE_MAX; i++) {
        if (l->s[i].seq && memcmp(l->s[i].key, key, APPROVE_KEY) == 0) return i;
    }
    return -1;
}

static inline int approve_list_count(const approve_list_t *l)
{
    int n = 0;
    for (int i = 0; i < APPROVE_MAX; i++) n += l->s[i].seq != 0;
    return n;
}

/* Already there: refreshed to newest. Otherwise into an empty slot, or
 * over the oldest. */
static inline void approve_list_add(approve_list_t *l, const uint8_t key[APPROVE_KEY])
{
    uint32_t top = 0;
    for (int i = 0; i < APPROVE_MAX; i++) if (l->s[i].seq > top) top = l->s[i].seq;
    if (top == UINT32_MAX) {
        /* Renumber in order rather than wrap, which would make the newest
         * the oldest. Not reached in any device's lifetime. */
        for (uint32_t r = 1; r <= APPROVE_MAX; r++) {
            int lo = -1;
            for (int i = 0; i < APPROVE_MAX; i++) {
                if (l->s[i].seq >= r && (lo < 0 || l->s[i].seq < l->s[lo].seq)) lo = i;
            }
            if (lo < 0) break;
            l->s[lo].seq = r;
        }
        top = 0;
        for (int i = 0; i < APPROVE_MAX; i++) if (l->s[i].seq > top) top = l->s[i].seq;
    }
    int at = approve_list_find(l, key);
    if (at < 0) {
        for (int i = 0; i < APPROVE_MAX; i++) {
            if (!l->s[i].seq) { at = i; break; }
        }
    }
    if (at < 0) {
        at = 0;
        for (int i = 1; i < APPROVE_MAX; i++) if (l->s[i].seq < l->s[at].seq) at = i;
    }
    memcpy(l->s[at].key, key, APPROVE_KEY);
    l->s[at].seq = top + 1;
}

static inline void approve_list_clear(approve_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

/* ---- the request ------------------------------------------------- */

typedef enum {
    APPROVE_IDLE = 0,
    APPROVE_WAITING,
    APPROVE_YES,
    APPROVE_NO,
} approve_state_t;

typedef enum {
    APPROVE_FOR_BROWSER = 0,
    APPROVE_FOR_MPD,
} approve_kind_t;

typedef enum {
    APPROVE_ASK_OK = 0,              /* waiting (now, or already) */
    APPROVE_ASK_BUSY,                /* someone else is waiting */
    APPROVE_ASK_REFUSED,             /* this address was refused lately */
} approve_ask_t;

typedef struct {
    approve_state_t state;
    approve_kind_t  kind;
    uint8_t  ticket[APPROVE_TICKET]; /* what the asker collects by */
    uint8_t  addr[APPROVE_ADDR];     /* where it asked from */
    char     code[5];
    int64_t  since_us;               /* asked, or answered */
    uint8_t  token[APPROVE_TOKEN];   /* a browser's, once allowed */
    uint8_t  deny_addr[APPROVE_ADDR];
    int64_t  deny_until_us;
} approve_pending_t;

/* Four digits from the ticket, so the asker can show them too. */
static inline void approve_code(const uint8_t ticket[APPROVE_TICKET], char out[5])
{
    const uint32_t v = ((uint32_t)ticket[0] << 24 | (uint32_t)ticket[1] << 16 |
                        (uint32_t)ticket[2] << 8 | ticket[3]) % 10000u;
    snprintf(out, 5, "%04u", (unsigned)v);
}

/* Lets a request that nobody answered, or an answer nobody collected,
 * go. Every entry point calls it first. */
static inline void approve_pend_tick(approve_pending_t *p, int64_t now)
{
    const int64_t age = now - p->since_us;
    if ((p->state == APPROVE_WAITING && age >= APPROVE_ASK_US) ||
        ((p->state == APPROVE_YES || p->state == APPROVE_NO) && age >= APPROVE_HOLD_US)) {
        p->state = APPROVE_IDLE;
        memset(p->token, 0, sizeof(p->token));
    }
}

static inline approve_ask_t approve_pend_ask(approve_pending_t *p, approve_kind_t kind,
                                             const uint8_t addr[APPROVE_ADDR],
                                             const uint8_t ticket[APPROVE_TICKET],
                                             int64_t now)
{
    approve_pend_tick(p, now);
    if (now < p->deny_until_us && memcmp(addr, p->deny_addr, APPROVE_ADDR) == 0) {
        return APPROVE_ASK_REFUSED;
    }
    if (p->state != APPROVE_IDLE) {
        /* The same asker again (a page reloaded, a client reconnecting)
         * is the same request; anyone else waits their turn. */
        if (p->kind == kind && memcmp(p->ticket, ticket, APPROVE_TICKET) == 0) return APPROVE_ASK_OK;
        return APPROVE_ASK_BUSY;
    }
    p->state = APPROVE_WAITING;
    p->kind = kind;
    memcpy(p->ticket, ticket, APPROVE_TICKET);
    memcpy(p->addr, addr, APPROVE_ADDR);
    approve_code(ticket, p->code);
    p->since_us = now;
    memset(p->token, 0, sizeof(p->token));
    return APPROVE_ASK_OK;
}

/* The listener's tap. `token` is the browser's new token on a yes (NULL
 * for an MPD client, or for a no). False when nothing was waiting. */
static inline bool approve_pend_answer(approve_pending_t *p, bool yes,
                                       const uint8_t token[APPROVE_TOKEN], int64_t now)
{
    approve_pend_tick(p, now);
    if (p->state != APPROVE_WAITING) return false;
    p->state = yes ? APPROVE_YES : APPROVE_NO;
    p->since_us = now;
    if (yes && token) memcpy(p->token, token, APPROVE_TOKEN);
    if (!yes) {
        memcpy(p->deny_addr, p->addr, APPROVE_ADDR);
        p->deny_until_us = now + APPROVE_DENY_US;
    }
    return true;
}

/*
 * The asker, asking how it went. APPROVE_IDLE: this ticket has no
 * request (never asked, expired, or someone else's). On APPROVE_YES the
 * token is copied out once and the request is over -- a second collect
 * gets IDLE, so a token is handed to one page and never again.
 */
static inline approve_state_t approve_pend_collect(approve_pending_t *p, approve_kind_t kind,
                                                   const uint8_t ticket[APPROVE_TICKET],
                                                   int64_t now, uint8_t token[APPROVE_TOKEN])
{
    approve_pend_tick(p, now);
    if (p->state == APPROVE_IDLE || p->kind != kind ||
        memcmp(p->ticket, ticket, APPROVE_TICKET) != 0) {
        return APPROVE_IDLE;
    }
    const approve_state_t s = p->state;
    if (s == APPROVE_YES && token) memcpy(token, p->token, APPROVE_TOKEN);
    if (s != APPROVE_WAITING) {
        /* Told once, then the slot is free for the next asker. A refusal
         * stays refused through deny_until_us, not through the slot. */
        memset(p->token, 0, sizeof(p->token));
        p->state = APPROVE_IDLE;
    }
    return s;
}
