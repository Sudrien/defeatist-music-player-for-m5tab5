/*
 * nvsns.c -- move the old namespaces into DEFEATIST_NVS_NS, once.
 *
 * 8066: from a table of the keys those namespaces ever held, each with
 * its type, through nvsh_*; it walked NVS with an iterator before, which
 * an .elf app cannot. (clkfix, an i64 in "radiokeep", is not in the
 * table: nvsh has none, and NTP relearns it.)
 *
 * For each old namespace: every entry is copied across unless the new
 * namespace already has that key (the new one is the newer write), the
 * new namespace is committed, and only then the old one is erased. One
 * namespace at a time, so the 20 KB partition never holds more than one
 * namespace's worth twice. A device with nothing left in the old
 * namespaces does one empty search each and nothing else.
 *
 * If a copy fails -- the partition full -- the old namespace is left
 * where it is and the next boot tries again; nothing is erased that has
 * not been copied.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nvsns.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"                /* ESP_ERR_NVS_NOT_FOUND; nvs_handler.h does not bring it */
#include "nvs_handler.h"

static const char *TAG = "tab5_nvs";

typedef enum { K_BLOB, K_STR, K_U8 } ktype_t;

typedef struct {
    const char *ns;
    const char *key;
    ktype_t     type;
} oldkey_t;

/* Every key the three old namespaces held (6006), by namespace. */
static const oldkey_t k_old[] = {
    { "radiokeep", "last",    K_BLOB },
    { "radiokeep", "star",    K_BLOB },
    { "radiokeep", "wifi_on", K_U8   },
    { "radiokeep", "prefs",   K_BLOB },
    { "wifistore", "nets",    K_BLOB },
    { "devcert",   "key",     K_STR  },
    { "devcert",   "crt",     K_STR  },
};
#define N_OLD (sizeof(k_old) / sizeof(k_old[0]))

/* Is there a value of this key, of any type? NOT_FOUND is no. The size
 * query (a NULL buffer) is how nvsh says so without reading it. */
static esp_err_t probe(const char *ns, const char *key, ktype_t type, size_t *len)
{
    uint8_t u8;
    *len = 0;
    switch (type) {
    case K_BLOB: return nvsh_get_blob(ns, key, NULL, len);
    case K_STR:  return nvsh_get_str(ns, key, NULL, len);
    default:     *len = 1; return nvsh_get_u8(ns, key, &u8);
    }
}

static esp_err_t copy_one(const oldkey_t *k)
{
    size_t len = 0;
    esp_err_t err = probe(k->ns, k->key, k->type, &len);
    if (err != ESP_OK) return err;

    if (k->type == K_U8) {
        uint8_t v;
        err = nvsh_get_u8(k->ns, k->key, &v);
        return err == ESP_OK ? nvsh_set_u8(DEFEATIST_NVS_NS, k->key, v) : err;
    }
    void *buf = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    if (k->type == K_STR) {
        err = nvsh_get_str(k->ns, k->key, buf, &len);
        if (err == ESP_OK) err = nvsh_set_str(DEFEATIST_NVS_NS, k->key, buf);
    } else {
        err = nvsh_get_blob(k->ns, k->key, buf, &len);
        if (err == ESP_OK) err = nvsh_set_blob(DEFEATIST_NVS_NS, k->key, buf, len);
    }
    heap_caps_free(buf);
    return err;
}

static void migrate_one(const char *old)
{
    int moved = 0, kept = 0, found = 0;
    bool ok = true;
    for (size_t i = 0; i < N_OLD && ok; i++) {
        const oldkey_t *k = &k_old[i];
        if (strcmp(k->ns, old) != 0) continue;

        size_t len;
        if (probe(old, k->key, k->type, &len) == ESP_ERR_NVS_NOT_FOUND) continue;
        found++;
        if (probe(DEFEATIST_NVS_NS, k->key, k->type, &len) != ESP_ERR_NVS_NOT_FOUND) {
            kept++;                             /* the new one is the newer write */
        } else if (copy_one(k) == ESP_OK) {
            moved++;
        } else {
            ESP_LOGE(TAG, "%s/%s not copied; %s left in place for the next boot",
                     old, k->key, old);
            ok = false;
        }
    }
    if (!found) return;                         /* nothing there: the usual case */

    if (ok) {
        (void)nvsh_erase_ns(old);               /* also the old clkfix, if any */
        ESP_LOGW(TAG, "moved %d entr%s from \"%s\" into \"%s\"%s", moved,
                 moved == 1 ? "y" : "ies", old, DEFEATIST_NVS_NS,
                 kept ? " (some already there, kept)" : "");
    }
}

void nvsns_migrate(void)
{
    static const char *const k_ns[] = { "radiokeep", "wifistore", "devcert" };
    for (size_t i = 0; i < sizeof(k_ns) / sizeof(k_ns[0]); i++) migrate_one(k_ns[i]);
}
