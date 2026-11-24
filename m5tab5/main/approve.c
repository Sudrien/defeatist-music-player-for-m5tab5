/*
 * approve.c -- see approve.h. The rules are approvelist.h's; this is
 * the mutex, NVS, the random source and the hash around them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "approve.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"        /* inet_ntop */
#include "nvs_handler.h"

/* Mbed TLS 4 (IDF 6) hashes through PSA and has no public sha256.h. */
#include "psa/crypto.h"

#include "nvsns.h"

static const char *TAG = "tab5_approve";

/* 8004: an MPD client's address is its request's ticket. */
_Static_assert(APPROVE_TICKET == APPROVE_ADDR, "an address must fit a ticket");

/* In DEFEATIST_NVS_NS, beside nvsns.h's list: the allowed browsers, as
 * the SHA-256 of each token. */
#define NVS_KEY_BROWSERS "rmt_ok"
/* 8004: the allowed MPD addresses, each 16 bytes (IPv4-mapped) and
 * zero to the list's key length. Not hashed: an address is not a
 * secret, and the prompt shows it anyway. */
#define NVS_KEY_MPD      "mpd_ok"
/* u-defeatist-0050: the phones allowed to stay on the open hotspot, each
 * its MAC in an address's 16 bytes (approve_hotspot_addr()). Not hashed,
 * for the MPD list's reason. */
#define NVS_KEY_HOTSPOT  "hs_ok"

static SemaphoreHandle_t  s_mu;
static approve_list_t     s_browsers;
static approve_list_t     s_mpd;            /* 8004 */
static approve_list_t     s_hotspot;        /* u-defeatist-0050 */
static approve_pending_t  s_pend;
static uint8_t            s_shown[APPROVE_TICKET];   /* ui_task's: last prompt woken for */

static bool hash(const uint8_t *in, size_t n, uint8_t out[APPROVE_KEY])
{
    size_t olen = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, in, n, out, APPROVE_KEY, &olen) == PSA_SUCCESS;
}

static bool unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else return false;
        }
        out[i] = (uint8_t)v;
    }
    return s[2 * n] == '\0';
}

static void tohex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static void load_one(const char *key, approve_list_t *l)
{
    size_t n = sizeof(*l);
    if (nvsh_get_blob(DEFEATIST_NVS_NS, key, l, &n) != ESP_OK || n != sizeof(*l)) approve_list_clear(l);
}

static void load(void)
{
    load_one(NVS_KEY_BROWSERS, &s_browsers);
    load_one(NVS_KEY_MPD, &s_mpd);                              /* 8004 */
    load_one(NVS_KEY_HOTSPOT, &s_hotspot);                      /* u-defeatist-0050 */
}

/* ui_task only (approve.h). From a copy: the mutex is not held here. */
static void store(const char *key, const approve_list_t *l)
{
    if (nvsh_set_blob(DEFEATIST_NVS_NS, key, l, sizeof(*l)) != ESP_OK) {
        ESP_LOGE(TAG, "could not save; the approval lasts until power off");
    }
}

/* 8004: an address as a list key. */
static void addr_key(const uint8_t addr[APPROVE_ADDR], uint8_t key[APPROVE_KEY])
{
    memset(key, 0, APPROVE_KEY);
    memcpy(key, addr, APPROVE_ADDR);
}

void approve_init(void)
{
    if (s_mu) return;
    s_mu = xSemaphoreCreateMutex();
    load();
    ESP_LOGI(TAG, "%d browser%s and %d MPD address%s allowed",
             approve_list_count(&s_browsers), approve_list_count(&s_browsers) == 1 ? "" : "s",
             approve_list_count(&s_mpd), approve_list_count(&s_mpd) == 1 ? "" : "es");
}

void approve_new_ticket(uint8_t ticket[APPROVE_TICKET])
{
    esp_fill_random(ticket, APPROVE_TICKET);
}

bool approve_browser_ok(const char *hex)
{
    uint8_t tok[APPROVE_TOKEN], key[APPROVE_KEY];
    if (!s_mu || !hex || !unhex(hex, tok, sizeof(tok)) || !hash(tok, sizeof(tok), key)) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = approve_list_find(&s_browsers, key) >= 0;
    xSemaphoreGive(s_mu);
    return ok;
}

approve_ask_t approve_browser_ask(const uint8_t addr[APPROVE_ADDR],
                                  const uint8_t ticket[APPROVE_TICKET], char code[5])
{
    if (!s_mu) return APPROVE_ASK_BUSY;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_ask_t r = approve_pend_ask(&s_pend, APPROVE_FOR_BROWSER, addr, ticket,
                                             esp_timer_get_time());
    approve_code(ticket, code);
    xSemaphoreGive(s_mu);
    return r;
}

approve_state_t approve_browser_collect(const uint8_t ticket[APPROVE_TICKET],
                                        char hex[APPROVE_HEX + 1], char code[5])
{
    uint8_t tok[APPROVE_TOKEN];
    hex[0] = '\0';
    approve_code(ticket, code);
    if (!s_mu) return APPROVE_IDLE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_state_t s = approve_pend_collect(&s_pend, APPROVE_FOR_BROWSER, ticket,
                                                   esp_timer_get_time(), tok);
    xSemaphoreGive(s_mu);
    if (s == APPROVE_YES) tohex(tok, sizeof(tok), hex);
    memset(tok, 0, sizeof(tok));
    return s;
}

bool approve_prompt(approve_kind_t *kind, char *who, size_t who_size,
                    char code[5], bool *fresh)
{
    if (fresh) *fresh = false;
    if (!s_mu) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    approve_pend_tick(&s_pend, esp_timer_get_time());
    const bool up = s_pend.state == APPROVE_WAITING;
    approve_pending_t p = s_pend;
    xSemaphoreGive(s_mu);
    if (!up) return false;

    if (kind) *kind = p.kind;
    if (code) memcpy(code, p.code, 5);
    if (who && who_size) {
        static const uint8_t mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
        if (p.kind == APPROVE_FOR_HOTSPOT) {                    /* u-defeatist-0050: a MAC */
            snprintf(who, who_size, "%02x:%02x:%02x:%02x:%02x:%02x", p.addr[0], p.addr[1], p.addr[2],
                     p.addr[3], p.addr[4], p.addr[5]);
        } else if (memcmp(p.addr, mapped, 12) == 0) {
            snprintf(who, who_size, "%u.%u.%u.%u", p.addr[12], p.addr[13], p.addr[14], p.addr[15]);
        } else if (!inet_ntop(AF_INET6, p.addr, who, who_size)) {
            who[0] = '\0';
        }
    }
    if (fresh && memcmp(s_shown, p.ticket, APPROVE_TICKET) != 0) {
        memcpy(s_shown, p.ticket, APPROVE_TICKET);
        *fresh = true;
    }
    memset(p.token, 0, sizeof(p.token));
    return true;
}

bool approve_prompt_raw(approve_kind_t *kind, uint8_t addr[APPROVE_ADDR])  /* u-defeatist-0053 */
{
    if (!s_mu) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    approve_pend_tick(&s_pend, esp_timer_get_time());
    const bool up = s_pend.state == APPROVE_WAITING;
    if (up) {
        if (kind) *kind = s_pend.kind;
        if (addr) memcpy(addr, s_pend.addr, APPROVE_ADDR);
    }
    xSemaphoreGive(s_mu);
    return up;
}

int approve_count(void)
{
    if (!s_mu) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int n = approve_list_count(&s_browsers) + approve_list_count(&s_mpd);   /* 8004 */
    xSemaphoreGive(s_mu);
    return n;
}

/* 8003: ui_task only, as approve_answer() is: it writes NVS. */
void approve_forget_all(void)
{
    if (!s_mu) return;
    approve_list_t copy;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    approve_list_clear(&s_browsers);
    approve_list_clear(&s_mpd);                                 /* 8004 */
    copy = s_browsers;
    xSemaphoreGive(s_mu);
    store(NVS_KEY_BROWSERS, &copy);
    store(NVS_KEY_MPD, &copy);          /* the same empty list */
    ESP_LOGI(TAG, "every browser and MPD address forgotten");
}

void approve_answer(bool yes)
{
    if (!s_mu) return;
    uint8_t tok[APPROVE_TOKEN], key[APPROVE_KEY];
    esp_fill_random(tok, sizeof(tok));
    const bool hashed = yes && hash(tok, sizeof(tok), key);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_kind_t kind = s_pend.kind;
    const bool waiting = s_pend.state == APPROVE_WAITING;
    /* A yes that could not be hashed is a no: a cookie the list cannot
     * recognise would be an approval that does nothing. */
    const bool answered = approve_pend_answer(&s_pend, hashed, hashed ? tok : NULL,
                                              esp_timer_get_time());
    approve_list_t copy;
    const bool save = answered && hashed;
    if (save && kind == APPROVE_FOR_BROWSER) {
        approve_list_add(&s_browsers, key);
        copy = s_browsers;
    } else if (save) {
        /* 8004: an MPD client is allowed by the address it asked from,
         * at the tap -- not when the client collects the answer, which
         * one that has already timed out and gone never will. So is a
         * hotspot phone, by its MAC (u-defeatist-0050). */
        uint8_t ak[APPROVE_KEY];
        addr_key(s_pend.addr, ak);
        approve_list_t *l = kind == APPROVE_FOR_HOTSPOT ? &s_hotspot : &s_mpd;
        approve_list_add(l, ak);
        copy = *l;
    }
    xSemaphoreGive(s_mu);
    memset(tok, 0, sizeof(tok));

    if (!waiting || !answered) return;
    ESP_LOGI(TAG, "request %s", hashed ? "allowed" : "refused");
    if (save) store(kind == APPROVE_FOR_BROWSER ? NVS_KEY_BROWSERS
                    : kind == APPROVE_FOR_HOTSPOT ? NVS_KEY_HOTSPOT : NVS_KEY_MPD, &copy);
}

/* ---- 8004: MPD ---------------------------------------------------------- */

bool approve_mpd_ok(const uint8_t addr[APPROVE_ADDR])
{
    if (!s_mu) return false;
    uint8_t k[APPROVE_KEY];
    addr_key(addr, k);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = approve_list_find(&s_mpd, k) >= 0;
    xSemaphoreGive(s_mu);
    return ok;
}

approve_ask_t approve_mpd_ask(const uint8_t addr[APPROVE_ADDR])
{
    if (!s_mu) return APPROVE_ASK_BUSY;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_ask_t r = approve_pend_ask(&s_pend, APPROVE_FOR_MPD, addr, addr,
                                             esp_timer_get_time());
    xSemaphoreGive(s_mu);
    return r;
}

approve_state_t approve_mpd_collect(const uint8_t addr[APPROVE_ADDR])
{
    if (!s_mu) return APPROVE_IDLE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_state_t s = approve_pend_collect(&s_pend, APPROVE_FOR_MPD, addr,
                                                   esp_timer_get_time(), NULL);
    xSemaphoreGive(s_mu);
    return s;
}

/* ---- u-defeatist-0050: the hotspot's phones ------------------------------ */

void approve_hotspot_addr(const uint8_t mac[6], uint8_t addr[APPROVE_ADDR])
{
    memset(addr, 0xee, APPROVE_ADDR);       /* no IPv6 address ends so: never an MPD key */
    memcpy(addr, mac, 6);
}

bool approve_hotspot_ok(const uint8_t mac[6])
{
    if (!s_mu) return false;
    uint8_t a[APPROVE_ADDR], k[APPROVE_KEY];
    approve_hotspot_addr(mac, a);
    addr_key(a, k);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = approve_list_find(&s_hotspot, k) >= 0;
    xSemaphoreGive(s_mu);
    return ok;
}

approve_ask_t approve_hotspot_ask(const uint8_t mac[6])
{
    if (!s_mu) return APPROVE_ASK_BUSY;
    uint8_t a[APPROVE_ADDR];
    approve_hotspot_addr(mac, a);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_ask_t r = approve_pend_ask(&s_pend, APPROVE_FOR_HOTSPOT, a, a, esp_timer_get_time());
    xSemaphoreGive(s_mu);
    return r;
}

approve_state_t approve_hotspot_collect(const uint8_t mac[6])
{
    if (!s_mu) return APPROVE_IDLE;
    uint8_t a[APPROVE_ADDR];
    approve_hotspot_addr(mac, a);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const approve_state_t s = approve_pend_collect(&s_pend, APPROVE_FOR_HOTSPOT, a, esp_timer_get_time(), NULL);
    xSemaphoreGive(s_mu);
    return s;
}

int approve_hotspot_count(void)
{
    if (!s_mu) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int n = approve_list_count(&s_hotspot);
    xSemaphoreGive(s_mu);
    return n;
}

/* ui_task only: it writes NVS. */
void approve_hotspot_forget(void)
{
    if (!s_mu) return;
    approve_list_t copy;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    approve_list_clear(&s_hotspot);
    copy = s_hotspot;
    xSemaphoreGive(s_mu);
    store(NVS_KEY_HOTSPOT, &copy);
    ESP_LOGI(TAG, "every hotspot phone forgotten");
}
