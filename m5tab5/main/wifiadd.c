/*
 * wifiadd.c -- see wifiadd.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wifiadd.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wifijoin.h"
#include "wifistore.h"

static const char *TAG = "tab5_wifiadd";

static SemaphoreHandle_t s_mu;
static volatile bool s_busy;
static uint32_t s_gen;
static wifiadd_result_t s_res;              /* 2.8 KB: static, not a stack */

/* the job's inputs, written before its task starts */
static wifiadd_job_t s_job;
static char s_ssid[WIFISTORE_SSID_MAX + 1];
static char s_pass[WIFISTORE_SECRET_MAX + 1];
static portalweb_net_t s_net;

static void lock(void)
{
    /* First use is ui_task's, before any job: no race to make it. Not
     * the static kind, which a box does not answer (xQueueCreateMutex
     * only). */
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mu, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mu); }

static void forget(void)
{
    volatile char *p = s_pass;
    for (size_t i = 0; i < sizeof s_pass; i++) p[i] = 0;
}

static void finish(wifiadd_job_t job, int n, const wifi_seen_t *seen, esp_err_t err)
{
    lock();
    s_res.job = job;
    s_res.n = n;
    s_res.err = err;
    if (seen && n > 0) memcpy(s_res.seen, seen, (size_t)n * sizeof *seen);
    /* Not busy before the result shows, and not before it is copied: a
     * Scan again tapped the moment it shows starts at once, and its scan
     * cannot write raw[] under this copy. wifiadd_take() reads s_gen
     * under this lock, so it sees both or neither. */
    s_busy = false;
    s_gen++;
    unlock();
}

static void task(void *arg)
{
    (void)arg;
    if (s_job == WIFIADD_SCAN) {
        static wifi_seen_t raw[WIFIADD_SEEN_MAX];
        const int n = wifi_scan_list(raw, WIFIADD_SEEN_MAX);
        finish(WIFIADD_SCAN, n, raw, ESP_OK);
    } else {
        const esp_err_t err = wifijoin_try(TAG, s_ssid, s_pass, s_net);
        forget();
        if (err != ESP_OK) ESP_LOGW(TAG, "join %.32s: %s", s_ssid, esp_err_to_name(err));
        finish(WIFIADD_JOIN, 0, NULL, err);
    }
    vTaskDelete(NULL);
}

static bool start(wifiadd_job_t job)
{
    s_job = job;
    /* 6 KB, as remote.c's w_start(): PBKDF2 and wifi_join()'s waits; the
     * scan's buffer is static above. */
    if (xTaskCreate(task, "wifiadd", 6144, NULL, 3, NULL) != pdPASS) {
        forget();
        finish(job, -1, NULL, ESP_ERR_NO_MEM);
        return false;
    }
    return true;
}

bool wifiadd_scan(void)
{
    if (s_busy) return false;
    s_busy = true;
    return start(WIFIADD_SCAN);
}

bool wifiadd_join(const char *ssid, const char *pass, portalweb_net_t net)
{
    if (s_busy) return false;
    s_busy = true;
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid ? ssid : "");
    snprintf(s_pass, sizeof s_pass, "%s", pass ? pass : "");
    s_net = net;
    ESP_LOGI(TAG, "joining %.32s", s_ssid);
    return start(WIFIADD_JOIN);
}

bool wifiadd_busy(void) { return s_busy; }

bool wifiadd_take(uint32_t *gen, wifiadd_result_t *out)
{
    if (!gen || !out) return false;
    lock();
    const bool fresh = s_gen != *gen;
    if (fresh) {
        *gen = s_gen;
        out->job = s_res.job;
        out->n = s_res.n;
        out->err = s_res.err;
        if (s_res.n > 0) memcpy(out->seen, s_res.seen, (size_t)s_res.n * sizeof s_res.seen[0]);
    }
    unlock();
    return fresh;
}
