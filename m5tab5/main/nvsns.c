/*
 * nvsns.c -- move the old namespaces into DEFEATIST_NVS_NS, once.
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

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "tab5_nvs";

static const char *const k_old[] = { "radiokeep", "wifistore", "devcert" };

static esp_err_t copy_one(nvs_handle_t src, nvs_handle_t dst, const char *key, nvs_type_t type)
{
    esp_err_t err;
    switch (type) {
#define INT_CASE(T, ctype, get, set) \
    case T: { ctype v; err = get(src, key, &v); if (err == ESP_OK) err = set(dst, key, v); break; }
    INT_CASE(NVS_TYPE_U8,  uint8_t,  nvs_get_u8,  nvs_set_u8)
    INT_CASE(NVS_TYPE_I8,  int8_t,   nvs_get_i8,  nvs_set_i8)
    INT_CASE(NVS_TYPE_U16, uint16_t, nvs_get_u16, nvs_set_u16)
    INT_CASE(NVS_TYPE_I16, int16_t,  nvs_get_i16, nvs_set_i16)
    INT_CASE(NVS_TYPE_U32, uint32_t, nvs_get_u32, nvs_set_u32)
    INT_CASE(NVS_TYPE_I32, int32_t,  nvs_get_i32, nvs_set_i32)
    INT_CASE(NVS_TYPE_U64, uint64_t, nvs_get_u64, nvs_set_u64)
    INT_CASE(NVS_TYPE_I64, int64_t,  nvs_get_i64, nvs_set_i64)
#undef INT_CASE
    case NVS_TYPE_STR: {
        size_t len = 0;
        err = nvs_get_str(src, key, NULL, &len);
        if (err != ESP_OK) break;
        char *buf = malloc(len);
        if (!buf) return ESP_ERR_NO_MEM;
        err = nvs_get_str(src, key, buf, &len);
        if (err == ESP_OK) err = nvs_set_str(dst, key, buf);
        free(buf);
        break;
    }
    case NVS_TYPE_BLOB: {
        size_t len = 0;
        err = nvs_get_blob(src, key, NULL, &len);
        if (err != ESP_OK) break;
        void *buf = malloc(len ? len : 1);
        if (!buf) return ESP_ERR_NO_MEM;
        err = nvs_get_blob(src, key, buf, &len);
        if (err == ESP_OK) err = nvs_set_blob(dst, key, buf, len);
        free(buf);
        break;
    }
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    return err;
}

/* Does dst already hold `key`, of any type? */
static bool has_key(nvs_handle_t dst, const char *key)
{
    nvs_type_t t;
    return nvs_find_key(dst, key, &t) == ESP_OK;
}

static void migrate_one(const char *old)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, old, NVS_TYPE_ANY, &it);
    if (err != ESP_OK) return;                  /* nothing there: the usual case */

    nvs_handle_t src, dst;
    if (nvs_open(old, NVS_READWRITE, &src) != ESP_OK) { nvs_release_iterator(it); return; }
    if (nvs_open(DEFEATIST_NVS_NS, NVS_READWRITE, &dst) != ESP_OK) {
        nvs_close(src);
        nvs_release_iterator(it);
        return;
    }

    int moved = 0, kept = 0;
    bool ok = true;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (has_key(dst, info.key)) {
            kept++;
        } else if (copy_one(src, dst, info.key, info.type) == ESP_OK) {
            moved++;
        } else {
            ESP_LOGE(TAG, "%s/%s not copied; %s left in place for the next boot",
                     old, info.key, old);
            ok = false;
            break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    if (ok && nvs_commit(dst) == ESP_OK) {
        if (nvs_erase_all(src) == ESP_OK) (void)nvs_commit(src);
        ESP_LOGW(TAG, "moved %d entr%s from \"%s\" into \"%s\"%s", moved,
                 moved == 1 ? "y" : "ies", old, DEFEATIST_NVS_NS,
                 kept ? " (some already there, kept)" : "");
    }
    nvs_close(dst);
    nvs_close(src);
}

void nvsns_migrate(void)
{
    for (size_t i = 0; i < sizeof(k_old) / sizeof(k_old[0]); i++) migrate_one(k_old[i]);
}
