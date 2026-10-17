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
#include "nvs.h"

#include "mbedtls/build_info.h"
/* certgen.c's split, for certgen.c's reason (6031): Mbed TLS 4 (IDF 6)
 * hashes through PSA and has no public sha256.h. */
#if MBEDTLS_VERSION_MAJOR >= 4
#define APPROVE_PSA 1
#include "psa/crypto.h"
#else
#define APPROVE_PSA 0
#include "mbedtls/sha256.h"
#endif

#include "nvsns.h"

static const char *TAG = "tab5_approve";

/* In DEFEATIST_NVS_NS, beside nvsns.h's list: the allowed browsers, as
 * the SHA-256 of each token. */
#define NVS_KEY_BROWSERS "rmt_ok"

static SemaphoreHandle_t  s_mu;
static approve_list_t     s_browsers;
static approve_pending_t  s_pend;
static uint8_t            s_shown[APPROVE_TICKET];   /* ui_task's: last prompt woken for */

static bool hash(const uint8_t *in, size_t n, uint8_t out[APPROVE_KEY])
{
#if APPROVE_PSA
    size_t olen = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, in, n, out, APPROVE_KEY, &olen) == PSA_SUCCESS;
#else
    return mbedtls_sha256(in, n, out, 0) == 0;
#endif
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

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_browsers);
    if (nvs_get_blob(h, NVS_KEY_BROWSERS, &s_browsers, &n) != ESP_OK || n != sizeof(s_browsers)) {
        approve_list_clear(&s_browsers);
    }
    nvs_close(h);
}

/* ui_task only (approve.h). From a copy: the mutex is not held here. */
static void store(const approve_list_t *l)
{
    nvs_handle_t h;
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "could not open NVS; the approval lasts until power off");
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_BROWSERS, l, sizeof(*l)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGE(TAG, "could not save; the approval lasts until power off");
    }
    nvs_close(h);
}

void approve_init(void)
{
    if (s_mu) return;
    s_mu = xSemaphoreCreateMutex();
    load();
    ESP_LOGI(TAG, "%d browser%s allowed", approve_list_count(&s_browsers),
             approve_list_count(&s_browsers) == 1 ? "" : "s");
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
        if (memcmp(p.addr, mapped, 12) == 0) {
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
    const bool save = answered && hashed && kind == APPROVE_FOR_BROWSER;
    if (save) {
        approve_list_add(&s_browsers, key);
        copy = s_browsers;
    }
    xSemaphoreGive(s_mu);
    memset(tok, 0, sizeof(tok));

    if (!waiting || !answered) return;
    ESP_LOGI(TAG, "request %s", hashed ? "allowed" : "refused");
    if (save) store(&copy);
}
