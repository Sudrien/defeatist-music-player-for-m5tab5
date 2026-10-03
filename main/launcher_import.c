/*
 * launcher_import.c -- see launcher_import.h.
 *
 * Shape, reimplemented from bmorcelli/Launcher src/wifi_crypto.cpp:
 *   key   = WIFI_ENC_KEY, cyclically padded/truncated to 16 bytes
 *   IV    = "LauncherWifiKey!"  (fixed; also our image marker)
 *   pwd   = base64( AES-128-CBC( passphrase + PKCS#7 ) )
 *   config.conf = [ { "wifi": [ { "ssid", "pwd", "secure" }, ... ] }, ... ]
 *
 * SPDX-License-Identifier: MIT
 */
#include "launcher_import.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "storage.h"     /* STORAGE_SD_MOUNT */
#include "wifistore.h"   /* wifistore_save() */
#include "i18n.h"        /* N_() -- status lines are looked up at draw time */

static const char *TAG = "limport";

/* Launcher's config.conf lives at the card root. */
#define LI_CONFIG_PATH   STORAGE_SD_MOUNT "/config.conf"

static const uint8_t LI_IV[16] = {
    0x4C,0x61,0x75,0x6E,0x63,0x68,0x65,0x72,  /* "Launcher" */
    0x57,0x69,0x66,0x69,0x4B,0x65,0x79,0x21   /* "WifiKey!"  */
};
#define LI_MARKER       "LauncherWifiKey!"
#define LI_MIN_CAND     8
#define LI_MAX_CAND     64
#define LI_READ_CHUNK   4096

/* ---- shared snapshot (copy-out, never borrowed; see portal.h) ---------- */
static SemaphoreHandle_t s_lock;
static li_status_t s_state = { .phase = LI_IDLE, .msg = "" };

static void set_status(li_phase_t ph, int imported, int skipped, const char *msg) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.phase = ph;
    s_state.imported = imported;
    s_state.skipped = skipped;
    snprintf(s_state.msg, sizeof(s_state.msg), "%s", msg ? msg : "");
    xSemaphoreGive(s_lock);
}

void launcher_import_status(li_status_t *out) {
    if (!out) return;
    if (!s_lock) { out->phase = LI_IDLE; out->imported = out->skipped = 0; out->msg[0] = 0; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

bool launcher_import_running(void) {
    li_status_t s; launcher_import_status(&s);
    return s.phase == LI_RUNNING;
}

/* ---- crypto core -------------------------------------------------------- */
static void li_build_key(const uint8_t *raw, size_t len, uint8_t key[16]) {
    if (len == 0) { memset(key, 0, 16); return; }
    for (int i = 0; i < 16; i++) key[i] = raw[i % len];
}

/* Decrypt `ct` with `key`; return unpadded printable length, else -1. */
static int li_decrypt(const uint8_t key[16], const uint8_t *ct, size_t ct_len,
                      uint8_t *out, size_t out_cap) {
    if (ct_len == 0 || ct_len % 16 || ct_len > out_cap) return -1;
    uint8_t iv[16];
    memcpy(iv, LI_IV, 16);
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    if (mbedtls_aes_setkey_dec(&c, key, 128) != 0) { mbedtls_aes_free(&c); return -1; }
    int rc = mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_DECRYPT, ct_len, iv, ct, out);
    mbedtls_aes_free(&c);
    if (rc != 0) return -1;
    int pad = out[ct_len - 1];
    if (pad < 1 || pad > 16 || (size_t)pad > ct_len) return -1;
    for (int i = 0; i < pad; i++) if (out[ct_len - 1 - i] != pad) return -1;
    int plen = (int)ct_len - pad;
    if (plen < 1) return -1;
    for (int i = 0; i < plen; i++) if (out[i] < 0x20 || out[i] > 0x7e) return -1;
    out[plen] = 0;
    return plen;
}

/* Up to two ciphertexts from config.conf, used to confirm a candidate key. */
typedef struct { uint8_t ct[2][64]; size_t len[2]; int n; } li_oracle_t;

static bool li_key_ok(const uint8_t key[16], const li_oracle_t *o) {
    uint8_t tmp[64];
    int l0 = li_decrypt(key, o->ct[0], o->len[0], tmp, sizeof(tmp));
    if (l0 < 0) return false;
    if (o->n >= 2) return li_decrypt(key, o->ct[1], o->len[1], tmp, sizeof(tmp)) >= 0;
    return l0 >= 8;   /* one-block single oracle: lean on WPA's 8-char floor */
}

/* ---- scan a partition's bytes for the key ------------------------------- */
static bool li_scan_part(const esp_partition_t *p, const li_oracle_t *o,
                         uint8_t key_out[16]) {
    uint8_t *buf = malloc(LI_READ_CHUNK);
    if (!buf) return false;
    uint8_t carry[LI_MAX_CAND]; size_t carry_len = 0;
    bool found = false;

    for (size_t off = 0; off < p->size && !found; off += LI_READ_CHUNK) {
        size_t want = (p->size - off < LI_READ_CHUNK) ? (p->size - off) : LI_READ_CHUNK;
        if (esp_partition_read(p, off, buf, want) != ESP_OK) break;

        uint8_t run[LI_MAX_CAND + 1]; size_t rl = carry_len;
        if (carry_len) memcpy(run, carry, carry_len);
        carry_len = 0;

        for (size_t i = 0; i <= want && !found; i++) {
            int b = (i < want) ? buf[i] : -1;
            bool pr = (b >= 0x20 && b <= 0x7e);
            if (pr && rl < LI_MAX_CAND) {
                run[rl++] = (uint8_t)b;
            } else {
                if (rl >= LI_MIN_CAND) {
                    for (size_t s = 0; s + LI_MIN_CAND <= rl && !found; s++) {
                        uint8_t key[16];
                        li_build_key(run + s, rl - s, key);
                        if (li_key_ok(key, o)) { memcpy(key_out, key, 16); found = true; }
                    }
                }
                if (pr) { run[0] = (uint8_t)b; rl = 1; } else rl = 0;
            }
        }
        if (rl && rl < LI_MAX_CAND) { memcpy(carry, run, rl); carry_len = rl; }
    }
    free(buf);
    return found;
}

/* Cheap pre-filter: does this partition carry Launcher's IV marker? */
static bool li_part_has_marker(const esp_partition_t *p) {
    uint8_t win[LI_READ_CHUNK];
    const size_t mlen = strlen(LI_MARKER), overlap = mlen - 1;
    for (size_t off = 0; off < p->size; off += (LI_READ_CHUNK - overlap)) {
        size_t want = (p->size - off < LI_READ_CHUNK) ? (p->size - off) : LI_READ_CHUNK;
        if (esp_partition_read(p, off, win, want) != ESP_OK) break;
        if (want >= mlen)
            for (size_t i = 0; i + mlen <= want; i++)
                if (memcmp(win + i, LI_MARKER, mlen) == 0) return true;
        if (want < LI_READ_CHUNK) break;
    }
    return false;
}

/* ---- config.conf -------------------------------------------------------- */
static cJSON *li_load_config(cJSON **wifi_out) {
    FILE *f = fopen(LI_CONFIG_PATH, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return NULL; }
    char *txt = malloc(sz + 1);
    if (!txt) { fclose(f); return NULL; }
    size_t rd = fread(txt, 1, sz, f); fclose(f);
    txt[rd] = 0;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return NULL;

    cJSON *wifi = NULL;
    if (cJSON_IsArray(root)) {
        cJSON *el;
        cJSON_ArrayForEach(el, root) {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(el, "wifi");
            if (cJSON_IsArray(w)) { wifi = w; break; }
        }
    } else {
        cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "wifi");
        if (cJSON_IsArray(w)) wifi = w;
    }
    if (!wifi) { cJSON_Delete(root); return NULL; }
    *wifi_out = wifi;
    return root;
}

static void li_fill_oracle(cJSON *wifi, li_oracle_t *o) {
    o->n = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, wifi) {
        if (o->n >= 2) break;
        cJSON *sec = cJSON_GetObjectItemCaseSensitive(e, "secure");
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(e, "pwd");
        if (sec && cJSON_IsFalse(sec)) continue;
        if (!cJSON_IsString(pwd) || !pwd->valuestring[0]) continue;
        size_t olen = 0;
        if (mbedtls_base64_decode(o->ct[o->n], sizeof(o->ct[0]), &olen,
                (const uint8_t *)pwd->valuestring, strlen(pwd->valuestring)) != 0) continue;
        if (olen == 0 || olen % 16) continue;
        o->len[o->n] = olen;
        o->n++;
    }
}

/* A hex 64-char value is a PSK; 8..63 is a passphrase; else unusable. */
static bool li_classify(const char *s, bool *is_psk) {
    size_t n = strlen(s);
    if (n == 64) {
        for (size_t i = 0; i < 64; i++) {
            char c = s[i];
            bool hex = (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
            if (!hex) { *is_psk = false; return (n >= 8 && n <= 63); }
        }
        *is_psk = true; return true;
    }
    *is_psk = false;
    return (n >= 8 && n <= 63);
}

/* ---- the job ------------------------------------------------------------ */
static void li_run(void) {
    set_status(LI_RUNNING, 0, 0, N_("Scanning..."));

    cJSON *wifi = NULL;
    cJSON *root = li_load_config(&wifi);
    if (!root) { set_status(LI_FAILED, 0, 0, N_("No M5Launcher config.conf on card")); return; }

    li_oracle_t oracle;
    li_fill_oracle(wifi, &oracle);
    if (oracle.n == 0) { cJSON_Delete(root); set_status(LI_FAILED, 0, 0, N_("No saved networks to import")); return; }

    /* Find the key in an app partition carrying the marker, skipping ours. */
    uint8_t key[16]; bool found = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it && !found) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p != running && li_part_has_marker(p) && li_scan_part(p, &oracle, key)) {
            found = true;
            ESP_LOGI(TAG, "key recovered from partition '%s'", p->label);
        }
        it = esp_partition_next(it);
    }
    if (it) esp_partition_iterator_release(it);

    if (!found) { cJSON_Delete(root); set_status(LI_FAILED, 0, 0, N_("M5Launcher not found on this board")); return; }

    /* Decrypt and save. Never log a passphrase, including on failure. */
    int imported = 0, skipped = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, wifi) {
        cJSON *jssid = cJSON_GetObjectItemCaseSensitive(e, "ssid");
        cJSON *jpwd  = cJSON_GetObjectItemCaseSensitive(e, "pwd");
        cJSON *jsec  = cJSON_GetObjectItemCaseSensitive(e, "secure");
        if (!cJSON_IsString(jssid) || !jssid->valuestring[0]) { skipped++; continue; }
        bool secure = !(jsec && cJSON_IsFalse(jsec));
        if (!secure || !cJSON_IsString(jpwd) || !jpwd->valuestring[0]) { skipped++; continue; }

        uint8_t ct[64]; size_t ctl = 0;
        if (mbedtls_base64_decode(ct, sizeof(ct), &ctl,
                (const uint8_t *)jpwd->valuestring, strlen(jpwd->valuestring)) != 0) { skipped++; continue; }
        uint8_t pt[65];
        int plen = li_decrypt(key, ct, ctl, pt, sizeof(pt) - 1);
        if (plen < 0) { ESP_LOGW(TAG, "entry '%s': decrypt failed", jssid->valuestring); skipped++; continue; }

        bool is_psk = false;
        if (!li_classify((const char *)pt, &is_psk)) { skipped++; continue; }
        if (wifistore_save(jssid->valuestring, (const char *)pt, is_psk) == ESP_OK) imported++;
        else skipped++;
        memset(pt, 0, sizeof(pt));
    }
    memset(key, 0, sizeof(key));
    cJSON_Delete(root);

    char msg[48];
    snprintf(msg, sizeof(msg), N_("Imported %d, skipped %d"), imported, skipped);
    set_status(LI_DONE, imported, skipped, msg);
    ESP_LOGI(TAG, "import done: %d imported, %d skipped", imported, skipped);
}

static void li_task(void *pv) {
    (void)pv;
    li_run();
    vTaskDelete(NULL);
}

esp_err_t launcher_import_request(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (launcher_import_running()) return ESP_ERR_INVALID_STATE;
    set_status(LI_RUNNING, 0, 0, N_("Scanning..."));
    /* 6 KB stack: the 4 KB read buffer and AES/cJSON state are on the heap. */
    if (xTaskCreate(li_task, "limport", 6144, NULL, 4, NULL) != pdPASS) {
        set_status(LI_IDLE, 0, 0, "");
        return ESP_FAIL;
    }
    return ESP_OK;
}
