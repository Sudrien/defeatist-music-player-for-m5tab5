/*
 * hotspot.c -- see hotspot.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hotspot.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "approve.h"
#include "portal.h"
#include "portalweb.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "tab5_hotspot";

#define RETRY_US (10 * 1000 * 1000)         /* a refused start, tried again */

static volatile bool s_on;
static volatile bool s_restart;
static char s_name[PORTAL_SSID_MAX + 1];
static int64_t s_retry_us;

static bool wanted(void)
{
    return settings_wifi_enabled() && settings_hotspot() && !portal_running();
}

/* ---- the whitelist (u-defeatist-0050) ---------------------------------- */

#define GATE_US   (1000 * 1000)             /* look at who is on once a second */
#define GATE_MAX  4                         /* the AP's max_connection */

/* Phones asked about, so a request that lapsed unanswered is told from one
 * never made; and phones sent away, sent away again on sight. */
static struct { uint8_t mac[6]; bool used; } s_asked[GATE_MAX];
static struct { uint8_t mac[6]; int64_t until; } s_away[GATE_MAX];
static int64_t s_gate_us;

static int find_asked(const uint8_t mac[6])
{
    for (int i = 0; i < GATE_MAX; i++)
        if (s_asked[i].used && !memcmp(s_asked[i].mac, mac, 6)) return i;
    return -1;
}

static void set_asked(const uint8_t mac[6], bool on)
{
    int i = find_asked(mac);
    if (!on) {
        if (i >= 0) s_asked[i].used = false;
        return;
    }
    if (i < 0) for (i = 0; i < GATE_MAX && s_asked[i].used; i++) {}
    if (i >= GATE_MAX) i = 0;
    memcpy(s_asked[i].mac, mac, 6);
    s_asked[i].used = true;
}

static bool is_away(const uint8_t mac[6], int64_t now)
{
    for (int i = 0; i < GATE_MAX; i++)
        if (now < s_away[i].until && !memcmp(s_away[i].mac, mac, 6)) return true;
    return false;
}

static void send_away(const uint8_t mac[6], int64_t now, const char *why)
{
    int at = 0;
    for (int i = 0; i < GATE_MAX; i++) {
        if (!memcmp(s_away[i].mac, mac, 6)) { at = i; break; }
        if (s_away[i].until < s_away[at].until) at = i;
    }
    memcpy(s_away[at].mac, mac, 6);
    s_away[at].until = now + APPROVE_DENY_US;
    set_asked(mac, false);
    (void)why;                              /* a log that compiles out still has used it */
    ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x: %s; sent away", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5], why);
    (void)wifi_ap_kick(mac);
}

static void gate(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_gate_us) return;
    s_gate_us = now + GATE_US;
    uint8_t macs[GATE_MAX][6];
    const int n = wifi_ap_stations(macs, GATE_MAX);
    for (int i = 0; i < n; i++) {
        const uint8_t *m = macs[i];
        if (is_away(m, now)) {
            (void)wifi_ap_kick(m);          /* back already: again, quietly */
            continue;
        }
        /* Collected first, every time, even for a phone already on the
         * list: approve.c holds an answer a minute for its asker to
         * collect, and while it is held the one request slot is full --
         * the browser on that same phone was told "the player is asking
         * about another browser" for that minute (u-defeatist-0051). */
        const approve_state_t st = approve_hotspot_collect(m);
        if (st == APPROVE_YES || approve_hotspot_ok(m)) {
            set_asked(m, false);
            continue;
        }
        if (st == APPROVE_NO) {
            send_away(m, now, "refused");
            continue;
        }
        if (st == APPROVE_WAITING) continue;                    /* on the screen */
        if (find_asked(m) >= 0) {           /* asked, and the two minutes ran out */
            send_away(m, now, "not answered");
            continue;
        }
        switch (approve_hotspot_ask(m)) {
        case APPROVE_ASK_OK:
            set_asked(m, true);
            ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x joined; asking", m[0], m[1], m[2], m[3], m[4], m[5]);
            break;
        case APPROVE_ASK_REFUSED:
            send_away(m, now, "refused lately");
            break;
        default:                            /* another request is up: next second */
            break;
        }
    }
}

void hotspot_poll(void)
{
    if (s_on && (s_restart || !wanted() || !wifi_up())) {
        /* the radio going down took the AP with it (wifi_stop()); only a
         * radio still up has one to end */
        if (wifi_up()) (void)wifi_ap_end();
        s_on = false;
        ESP_LOGI(TAG, "hotspot down%s", s_restart ? ", to start again" : "");
        s_restart = false;
        s_retry_us = 0;
    }
    if (s_on) {
        gate();
        return;
    }
    if (!wanted() || !wifi_up() || esp_timer_get_time() < s_retry_us) return;

    uint8_t mac[6] = { 0 };
    (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    char name[sizeof s_name];
    portalweb_ap_name(mac, name, sizeof name);
    const esp_err_t err = wifi_ap_begin(name);      /* open: the whitelist is the gate */
    snprintf(s_name, sizeof s_name, "%s", name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hotspot %s: not started (%s); trying again in 10 s", name, esp_err_to_name(err));
        s_retry_us = esp_timer_get_time() + RETRY_US;
        return;
    }
    s_on = true;
    memset(s_asked, 0, sizeof s_asked);
    s_gate_us = 0;
    char ip[16] = "";
    (void)wifi_ap_ip(ip, sizeof ip);
    ESP_LOGI(TAG, "hotspot up: %s (open; phones are asked about here), at %s", name,
             ip[0] ? ip : "(address coming)");
}

bool hotspot_running(void) { return s_on; }

void hotspot_name(char *out, size_t out_size)
{
    if (out && out_size) snprintf(out, out_size, "%s", s_name);
}

bool hotspot_ip(char *out, size_t out_size)
{
    if (!s_on) {
        if (out && out_size) out[0] = '\0';
        return false;
    }
    return wifi_ap_ip(out, out_size);
}

int hotspot_clients(void) { return s_on ? wifi_ap_clients() : 0; }

void hotspot_restart(void) { s_restart = true; }
