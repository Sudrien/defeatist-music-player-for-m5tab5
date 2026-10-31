/*
 * approvelisttest.c -- the browser whitelist's rules (8002), compiled
 * from main/approvelist.h rather than transcribed.
 *
 * What approve.c adds -- the mutex, NVS, the hash, the random tokens --
 * runs on the board. This is the decision: which keys are kept, and how
 * one request moves from asked to answered to collected.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "approvelist.h"

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

#define S (1000000LL)

static void key(uint8_t k[APPROVE_KEY], int n)
{
    memset(k, 0, APPROVE_KEY);
    k[0] = (uint8_t)n;
    k[31] = (uint8_t)(n * 7);
}

static void list(void)
{
    approve_list_t l;
    approve_list_clear(&l);
    uint8_t k[APPROVE_KEY];

    key(k, 1);
    CHECK(approve_list_find(&l, k) < 0, "empty list finds nothing");
    approve_list_add(&l, k);
    CHECK(approve_list_find(&l, k) >= 0, "added is found");
    CHECK(approve_list_count(&l) == 1, "one");
    approve_list_add(&l, k);
    CHECK(approve_list_count(&l) == 1, "adding again does not duplicate");

    /* An all-zero key is a key, not an empty slot. */
    uint8_t z[APPROVE_KEY] = { 0 };
    CHECK(approve_list_find(&l, z) < 0, "zero key not found before it is added");

    for (int i = 2; i <= APPROVE_MAX; i++) { key(k, i); approve_list_add(&l, k); }
    CHECK(approve_list_count(&l) == APPROVE_MAX, "full");

    /* Refresh 1, so 2 is now the oldest; the ninth replaces 2. */
    key(k, 1); approve_list_add(&l, k);
    key(k, 9); approve_list_add(&l, k);
    CHECK(approve_list_count(&l) == APPROVE_MAX, "still full");
    key(k, 9); CHECK(approve_list_find(&l, k) >= 0, "newest kept");
    key(k, 1); CHECK(approve_list_find(&l, k) >= 0, "refreshed kept");
    key(k, 2); CHECK(approve_list_find(&l, k) < 0, "oldest replaced");
    key(k, 3); CHECK(approve_list_find(&l, k) >= 0, "next oldest kept");

    /* The counter at its ceiling renumbers without losing the order. */
    for (int i = 0; i < APPROVE_MAX; i++) l.s[i].seq = UINT32_MAX - APPROVE_MAX + 1 + (uint32_t)i;
    const int oldest = 0;
    uint8_t was[APPROVE_KEY];
    memcpy(was, l.s[oldest].key, APPROVE_KEY);
    key(k, 50); approve_list_add(&l, k);
    CHECK(approve_list_find(&l, k) >= 0, "added at the ceiling");
    CHECK(approve_list_find(&l, was) < 0, "the oldest went, not the newest");
    int top = 0;
    for (int i = 0; i < APPROVE_MAX; i++) if (l.s[i].seq > l.s[top].seq) top = i;
    CHECK(memcmp(l.s[top].key, k, APPROVE_KEY) == 0, "and is the newest after renumbering");

    approve_list_clear(&l);
    CHECK(approve_list_count(&l) == 0, "cleared");
}

static void request(void)
{
    approve_pending_t p;
    memset(&p, 0, sizeof(p));
    uint8_t a1[APPROVE_ADDR] = { [10] = 0xff, [11] = 0xff, [12] = 192, [13] = 168, [14] = 1, [15] = 7 };
    uint8_t a2[APPROVE_ADDR] = { [10] = 0xff, [11] = 0xff, [12] = 192, [13] = 168, [14] = 1, [15] = 8 };
    uint8_t t1[APPROVE_TICKET], t2[APPROVE_TICKET], tok[APPROVE_TOKEN], got[APPROVE_TOKEN];
    memset(t1, 0x11, sizeof(t1));
    memset(t2, 0x22, sizeof(t2));
    memset(tok, 0x5a, sizeof(tok));
    int64_t now = 1000 * S;

    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now, got) == APPROVE_IDLE, "nothing asked");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now) == APPROVE_ASK_OK, "ask");
    CHECK(p.state == APPROVE_WAITING, "waiting");
    char c[5];
    approve_code(t1, c);
    CHECK(strcmp(c, p.code) == 0 && strlen(c) == 4, "code is four digits from the ticket");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + S) == APPROVE_ASK_OK, "same ticket again is the same request");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a2, t2, now + S) == APPROVE_ASK_BUSY, "another asker is busy, not queued");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t2, now + S) == APPROVE_ASK_BUSY, "same address, other ticket: busy");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t2, now + S, got) == APPROVE_IDLE, "another ticket collects nothing");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + S, got) == APPROVE_WAITING, "still waiting");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_MPD, t1, now + S, got) == APPROVE_IDLE, "other kind collects nothing");

    CHECK(approve_pend_answer(&p, true, tok, now + 2 * S), "answered");
    CHECK(!approve_pend_answer(&p, false, NULL, now + 2 * S), "answered once only");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t2, now + 3 * S, got) == APPROVE_IDLE, "the token is not another ticket's");
    memset(got, 0, sizeof(got));
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + 3 * S, got) == APPROVE_YES, "yes");
    CHECK(memcmp(got, tok, sizeof(tok)) == 0, "with the token");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + 3 * S, got) == APPROVE_IDLE, "and only once");
    uint8_t zero[APPROVE_TOKEN] = { 0 };
    CHECK(memcmp(p.token, zero, sizeof(zero)) == 0, "token not left behind");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a2, t2, now + 4 * S) == APPROVE_ASK_OK, "slot free for the next asker");

    /* Unanswered: gone after APPROVE_ASK_US, and the slot is free. */
    now += 100 * S;
    memset(&p, 0, sizeof(p));
    approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now);
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + APPROVE_ASK_US - 1, got) == APPROVE_WAITING, "just before expiry");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + APPROVE_ASK_US, got) == APPROVE_IDLE, "expired");
    CHECK(!approve_pend_answer(&p, true, tok, now + APPROVE_ASK_US), "a late tap answers nothing");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a2, t2, now + APPROVE_ASK_US) == APPROVE_ASK_OK, "free after expiry");

    /* Answered but never collected: the slot is held, then let go. */
    approve_pend_answer(&p, true, tok, now + APPROVE_ASK_US);
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + APPROVE_ASK_US + S) == APPROVE_ASK_BUSY, "held for collection");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + APPROVE_ASK_US + APPROVE_HOLD_US) == APPROVE_ASK_OK, "let go after the hold");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t2, now + APPROVE_ASK_US + APPROVE_HOLD_US, got) == APPROVE_IDLE, "uncollected yes is gone");

    /* Deny: told once, slot freed, that address refused for a while. */
    now += 1000 * S;
    memset(&p, 0, sizeof(p));
    approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now);
    CHECK(approve_pend_answer(&p, false, NULL, now + S), "denied");
    CHECK(approve_pend_collect(&p, APPROVE_FOR_BROWSER, t1, now + 2 * S, got) == APPROVE_NO, "told no");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + 3 * S) == APPROVE_ASK_REFUSED, "same address refused");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t2, now + 3 * S) == APPROVE_ASK_REFUSED, "new ticket, same address, refused");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a2, t2, now + 3 * S) == APPROVE_ASK_OK, "another address may ask");
    approve_pend_answer(&p, true, tok, now + 4 * S);
    approve_pend_collect(&p, APPROVE_FOR_BROWSER, t2, now + 4 * S, got);
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + S + APPROVE_DENY_US - 1) == APPROVE_ASK_REFUSED, "refused to the end");
    CHECK(approve_pend_ask(&p, APPROVE_FOR_BROWSER, a1, t1, now + S + APPROVE_DENY_US) == APPROVE_ASK_OK, "and then may ask");

    /* A yes with no token (an MPD client's) carries none. */
    memset(&p, 0, sizeof(p));
    approve_pend_ask(&p, APPROVE_FOR_MPD, a1, t1, now);
    approve_pend_answer(&p, true, NULL, now);
    memset(got, 0x77, sizeof(got));
    CHECK(approve_pend_collect(&p, APPROVE_FOR_MPD, t1, now, got) == APPROVE_YES, "mpd yes");
    CHECK(memcmp(got, zero, sizeof(zero)) == 0, "no token for mpd");
}

static void codes(void)
{
    uint8_t t[APPROVE_TICKET] = { 0 };
    char c[5];
    approve_code(t, c);
    CHECK(strcmp(c, "0000") == 0, "zero ticket: 0000, got %s", c);
    memset(t, 0xff, sizeof(t));
    approve_code(t, c);
    CHECK(strcmp(c, "7295") == 0, "0xffffffff %% 10000 = 7295, got %s", c);
}

int main(void)
{
    printf("approvelisttest\n");
    list();
    request();
    codes();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
