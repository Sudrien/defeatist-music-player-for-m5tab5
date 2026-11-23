/*
 * wifiadd.h -- the scan and the join behind netform.c (u-defeatist-0048).
 *
 * Both block for seconds -- a scan about four, a join up to two of
 * wifi_join()'s timeouts -- and netform.c is driven from ui_task, which
 * must not wait (portal.h, "IT CANNOT BLOCK"). So each is a short task of
 * its own, as remote.c's Wi-Fi section does it (its w_start()), and the
 * result is a copy fetched by generation: panel.c asks wifiadd_take()
 * each draw and hands anything new to netform.
 *
 * The join is wifijoin_try(): the same plan, attempts and save as the
 * browser's Wi-Fi section and the portal. In a box, wifi_scan_list,
 * wifi_join, wifi_psk and wifistore_save are the box's (u-feckless-0083).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "portalweb.h"
#include "wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFIADD_SEEN_MAX (64)

typedef enum { WIFIADD_NONE = 0, WIFIADD_SCAN, WIFIADD_JOIN } wifiadd_job_t;

/* A job finished: what it was and what came of it. */
typedef struct {
    wifiadd_job_t job;
    int n;                  /* SCAN: networks in seen, or -1 */
    wifi_seen_t seen[WIFIADD_SEEN_MAX];
    esp_err_t err;          /* JOIN: wifijoin_try()'s answer */
} wifiadd_result_t;

/* Start one. false while another runs, or if the task would not start
 * (then a JOIN's result is ESP_ERR_NO_MEM, delivered as any other). The
 * password is copied and cleared once tried. */
bool wifiadd_scan(void);
bool wifiadd_join(const char *ssid, const char *pass, portalweb_net_t net);

bool wifiadd_busy(void);

/* The latest finished job if it is newer than *gen: copied into out,
 * *gen moved on, true. */
bool wifiadd_take(uint32_t *gen, wifiadd_result_t *out);

#ifdef __cplusplus
}
#endif
