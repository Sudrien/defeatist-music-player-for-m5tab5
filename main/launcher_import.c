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
#include "cJSON.h"

/*
 * AES-128 and base64 are VENDORED here rather than taken from mbedTLS.
 *
 * Mbed TLS 4.0 (IDF v6.1+) moved the low-level ciphers into TF-PSA-Crypto
 * and dropped the legacy mbedtls/aes.h and mbedtls/base64.h; reaching AES
 * now means the PSA API, which differs again from the 3.x path on IDF
 * 5.x. One small, self-contained implementation -- in the spirit of this
 * tree's vendored minimp3 and its own jsonpick -- builds on every IDF and
 * cannot break on an mbedTLS version bump. It is decrypt-only (all this
 * file ever does) and table-driven; ~5 KB of flash, reached once per tap.
 */

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

/* ---- vendored AES-128 (decrypt only) ------------------------------------ */
static const uint8_t AES_SB[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static const uint8_t AES_RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint8_t aes_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t aes_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) { if (b & 1) r ^= a; a = aes_xtime(a); b >>= 1; }
    return r;
}
static void aes_expand(const uint8_t k[16], uint8_t rk[176]) {
    memcpy(rk, k, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i-4], rk[i-3], rk[i-2], rk[i-1] };
        if (i % 16 == 0) {
            uint8_t tmp = t[0];
            t[0] = AES_SB[t[1]] ^ AES_RCON[i/16];
            t[1] = AES_SB[t[2]]; t[2] = AES_SB[t[3]]; t[3] = AES_SB[tmp];
        }
        for (int j = 0; j < 4; j++) rk[i+j] = rk[i-16+j] ^ t[j];
    }
}
static void aes_inv_shiftrows(uint8_t s[16]) {
    uint8_t t;
    t = s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t = s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t = s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
}
static void aes_dec_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16],
                          const uint8_t isb[256]) {
    uint8_t s[16]; memcpy(s, in, 16);
    for (int j = 0; j < 16; j++) s[j] ^= rk[160+j];
    for (int round = 9; round >= 1; round--) {
        aes_inv_shiftrows(s);
        for (int j = 0; j < 16; j++) s[j] = isb[s[j]];
        for (int j = 0; j < 16; j++) s[j] ^= rk[round*16+j];
        for (int c = 0; c < 4; c++) {                 /* InvMixColumns */
            uint8_t *q = s + c*4, a0=q[0], a1=q[1], a2=q[2], a3=q[3];
            q[0] = aes_mul(a0,14)^aes_mul(a1,11)^aes_mul(a2,13)^aes_mul(a3,9);
            q[1] = aes_mul(a0,9)^aes_mul(a1,14)^aes_mul(a2,11)^aes_mul(a3,13);
            q[2] = aes_mul(a0,13)^aes_mul(a1,9)^aes_mul(a2,14)^aes_mul(a3,11);
            q[3] = aes_mul(a0,11)^aes_mul(a1,13)^aes_mul(a2,9)^aes_mul(a3,14);
        }
    }
    aes_inv_shiftrows(s);
    for (int j = 0; j < 16; j++) s[j] = isb[s[j]];
    for (int j = 0; j < 16; j++) s[j] ^= rk[j];
    memcpy(out, s, 16);
}
/* AES-128-CBC decrypt in place of mbedtls_aes_crypt_cbc. */
static void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t isb[256];
    for (int i = 0; i < 256; i++) isb[AES_SB[i]] = (uint8_t)i;
    uint8_t rk[176]; aes_expand(key, rk);
    uint8_t prev[16]; memcpy(prev, iv, 16);
    for (size_t off = 0; off < n; off += 16) {
        uint8_t blk[16]; aes_dec_block(rk, in + off, blk, isb);
        for (int j = 0; j < 16; j++) out[off+j] = blk[j] ^ prev[j];
        memcpy(prev, in + off, 16);
    }
}
/* base64 decode in place of mbedtls_base64_decode. Returns bytes, or -1. */
static int li_b64_decode(const char *s, uint8_t *o, size_t cap) {
    int8_t t[256]; memset(t, -1, sizeof(t));
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) t[(uint8_t)A[i]] = (int8_t)i;
    size_t n = 0; int acc = 0, bits = 0;
    for (const char *p = s; *p && *p != '='; p++) {
        int v = t[(uint8_t)*p];
        if (v < 0) continue;                 /* skip whitespace/newlines */
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < cap) o[n++] = (uint8_t)((acc >> bits) & 0xff); else return -1; }
    }
    return (int)n;
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
    aes128_cbc_decrypt(key, iv, ct, ct_len, out);
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
        int olen = li_b64_decode(pwd->valuestring, o->ct[o->n], sizeof(o->ct[0]));
        if (olen <= 0 || olen % 16) continue;
        o->len[o->n] = (size_t)olen;
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

        uint8_t ct[64];
        int ctl = li_b64_decode(jpwd->valuestring, ct, sizeof(ct));
        if (ctl <= 0) { skipped++; continue; }
        uint8_t pt[65];
        int plen = li_decrypt(key, ct, (size_t)ctl, pt, sizeof(pt) - 1);
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
