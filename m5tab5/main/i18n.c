/*
 * i18n.c -- lookups into i18n_tab.c. See i18n.h.
 *
 * bsearch with strcmp over the sorted English keys: a few hundred
 * strings is nine comparisons, each usually decided in the first byte
 * or two, which is noise next to drawing the glyphs. No hashing, no
 * pointer-identity tricks -- the same literal in two files need not be
 * one pointer, so lookup has to be by content.
 *
 * SPDX-License-Identifier: MIT
 */
#include "i18n.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"      /* pngle's: tinfl, which the ROM holds (nm: A tinfl_decompress) */

static const char *TAG = "i18n";

/* The tables are flat, because a key count known only in i18n_tab.c
 * cannot be an array dimension here: [lang * i18n_count + key] and
 * [(lang * i18n_pcount + key) * 2 + form]. Their values are offsets into
 * s_text, which i18n_init() fills. */
enum { ST_NONE = 0, ST_BUSY, ST_READY, ST_FAILED };
static int s_state = ST_NONE;           /* atomic: ST_* */
static const char *s_text;              /* written once, before s_state is READY */

static volatile int s_lang = I18N_EN;

bool i18n_init(void)
{
    int expect = ST_NONE;
    if (!__atomic_compare_exchange_n(&s_state, &expect, ST_BUSY, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == ST_READY;      /* done, failed, or another task is in it */

    /* The text for good, and tinfl's 11 KB of state for a moment: PSRAM
     * for both -- internal RAM is the radio's -- and the state on the
     * heap, never on a stack (tinfl_decompress_mem_to_mem() would put it
     * on the caller's). */
    char *text = heap_caps_malloc(i18n_text_len ? i18n_text_len : 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tinfl_decompressor *d = heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (text && d) {
        size_t in_n = i18n_blob_len, out_n = i18n_text_len;
        tinfl_init(d);
        const tinfl_status st = tinfl_decompress(d, i18n_blob, &in_n,
                                                 (mz_uint8 *)text, (mz_uint8 *)text, &out_n,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        ok = st == TINFL_STATUS_DONE && out_n == i18n_text_len && in_n == i18n_blob_len;
        if (!ok)
            ESP_LOGE(TAG, "inflate failed: status %d, %u of %u bytes out",
                     (int)st, (unsigned)out_n, i18n_text_len);
    } else {
        ESP_LOGE(TAG, "no memory for %u bytes of text", i18n_text_len);
    }
    heap_caps_free(d);
    if (ok) {
        ESP_LOGI(TAG, "%u bytes of text inflated from %u in flash", i18n_text_len, i18n_blob_len);
        s_text = text;
    } else {
        heap_caps_free(text);
    }
    __atomic_store_n(&s_state, ok ? ST_READY : ST_FAILED, __ATOMIC_RELEASE);
    return ok;
}

/* The text, or NULL: never inflated, failed, or being inflated by another
 * task right now. */
static const char *text(void)
{
    if (__atomic_load_n(&s_state, __ATOMIC_ACQUIRE) == ST_NONE) i18n_init();
    return __atomic_load_n(&s_state, __ATOMIC_ACQUIRE) == ST_READY ? s_text : NULL;
}

const char *i18n_val(i18n_lang_t lang, unsigned key)
{
    const char *t = text();
    if (!t || (unsigned)lang >= I18N_LANG_COUNT || key >= i18n_count) return NULL;
    const unsigned o = i18n_voff[(unsigned)lang * i18n_count + key];
    return o == I18N_NONE ? NULL : t + i18n_lbase[lang] + o;
}

const char *i18n_pval(i18n_lang_t lang, unsigned key, unsigned form)
{
    const char *t = text();
    if (!t || (unsigned)lang >= I18N_LANG_COUNT || key >= i18n_pcount || form > 1) return NULL;
    const unsigned o = i18n_pvoff[((unsigned)lang * i18n_pcount + key) * 2 + form];
    return o == I18N_NONE ? NULL : t + i18n_lbase[lang] + o;
}

void i18n_set_lang(i18n_lang_t lang)
{
    s_lang = ((unsigned)lang < I18N_LANG_COUNT) ? (int)lang : I18N_EN;
}

i18n_lang_t i18n_lang(void) { return (i18n_lang_t)s_lang; }

const char *i18n_lang_name(i18n_lang_t lang)
{
    static const char *const names[I18N_LANG_COUNT] = {
        [I18N_EN]    = "English",
        [I18N_ZH_CN] = "简体中文",
        [I18N_JA]    = "日本語",
        [I18N_ES]    = "Español",
        [I18N_PT]    = "Português",
        [I18N_FR]    = "Français",
        [I18N_IT]    = "Italiano",
    };
    return names[(unsigned)lang < I18N_LANG_COUNT ? lang : I18N_EN];
}

static int find(const char *const *keys, unsigned n, const char *msgid)
{
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const int c = strcmp(msgid, keys[mid]);
        if (c == 0) return (int)mid;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

/* 6024: the lookups take a language; the screen's are the two below
 * with s_lang, the web pages' are these with the browser's choice. */
const char *i18n_get_in(i18n_lang_t lang_in, const char *msgid)
{
    if (!msgid) return msgid;
    const int lang = (unsigned)lang_in < I18N_LANG_COUNT ? (int)lang_in : I18N_EN;
    const int i = find(i18n_keys, i18n_count, msgid);
    if (i < 0) return msgid;
    const char *v = i18n_val((i18n_lang_t)lang, (unsigned)i);
    if (v) return v;
    v = i18n_val(I18N_EN, (unsigned)i);               /* an English rewording, if any */
    return v ? v : msgid;
}

/* CLDR cardinal rules, cut to what the table has: English one/other;
 * Spanish one/other too (CLDR's "many" is for exact millions);
 * Chinese and Japanese only other. Index 0 is one, 1 is other.
 * 6068: Italian is English's rule; French and Portuguese (CLDR's "pt",
 * which is Brazil's) say one for 0 as well as 1. Their "many", like
 * Spanish's, is for exact millions. */
static int plural_form(int lang, long n)
{
    if (lang == I18N_EN || lang == I18N_ES || lang == I18N_IT) return n == 1 ? 0 : 1;
    if (lang == I18N_FR || lang == I18N_PT) return (n == 0 || n == 1) ? 0 : 1;
    return 1;
}

const char *i18n_get(const char *msgid)
{
    return i18n_get_in((i18n_lang_t)s_lang, msgid);
}

const char *i18n_get_plural_in(i18n_lang_t lang_in, const char *msgid, long n)
{
    if (!msgid) return msgid;
    const int lang = (unsigned)lang_in < I18N_LANG_COUNT ? (int)lang_in : I18N_EN;
    const int i = find(i18n_pkeys, i18n_pcount, msgid);
    if (i < 0) return msgid;
    const char *v = i18n_pval((i18n_lang_t)lang, (unsigned)i, (unsigned)plural_form(lang, n));
    if (v) return v;
    v = i18n_pval(I18N_EN, (unsigned)i, (unsigned)plural_form(I18N_EN, n));
    return v ? v : msgid;
}

const char *i18n_get_plural(const char *msgid, long n)
{
    return i18n_get_plural_in((i18n_lang_t)s_lang, msgid, n);
}

const char *i18n_lang_code(i18n_lang_t lang)
{
    static const char *const codes[I18N_LANG_COUNT] = {
        [I18N_EN] = "en", [I18N_ZH_CN] = "zh-CN", [I18N_JA] = "ja", [I18N_ES] = "es",
        [I18N_PT] = "pt", [I18N_FR] = "fr", [I18N_IT] = "it",
    };
    return codes[(unsigned)lang < I18N_LANG_COUNT ? lang : I18N_EN];
}

/*
 * 6024: one language tag from Accept-Language to ours, or -1. The
 * primary subtag decides, except that Chinese has to be Simplified:
 * zh-TW, zh-HK, zh-MO and anything marked Hant are Traditional, which
 * a reader of it can mostly follow in Simplified but would not choose,
 * so they match nothing and the next preference gets its turn.
 */
static int tag_lang(const char *t, size_t n)
{
    char tag[24];
    if (n == 0 || n >= sizeof(tag)) return -1;
    for (size_t i = 0; i < n; i++) {
        const char c = t[i];
        tag[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (c == '_' ? '-' : c);
    }
    tag[n] = '\0';
    const size_t p = strcspn(tag, "-");
    if (p == 2 && strncmp(tag, "en", 2) == 0) return I18N_EN;
    if (p == 2 && strncmp(tag, "ja", 2) == 0) return I18N_JA;
    if (p == 2 && strncmp(tag, "es", 2) == 0) return I18N_ES;
    if (p == 2 && strncmp(tag, "pt", 2) == 0) return I18N_PT;     /* pt-PT too */
    if (p == 2 && strncmp(tag, "fr", 2) == 0) return I18N_FR;
    if (p == 2 && strncmp(tag, "it", 2) == 0) return I18N_IT;
    if (p == 2 && strncmp(tag, "zh", 2) == 0) {
        if (strstr(tag, "-hant") || strstr(tag, "-tw") || strstr(tag, "-hk") ||
            strstr(tag, "-mo")) return -1;
        return I18N_ZH_CN;
    }
    if (strcmp(tag, "*") == 0) return I18N_EN;
    return -1;
}

/*
 * The highest-q language we have, first listed winning a tie; English
 * when nothing matches or there is no header. q is read to three
 * decimals, as RFC 9110 allows, and q=0 means "not this one".
 */
i18n_lang_t i18n_from_accept_language(const char *h)
{
    int best = I18N_EN, best_q = -1;
    if (!h) return I18N_EN;
    while (*h) {
        while (*h == ' ' || *h == ',' || *h == '\t') h++;
        if (!*h) break;
        const char *t = h;
        while (*h && *h != ';' && *h != ',' && *h != ' ' && *h != '\t') h++;
        const size_t tn = (size_t)(h - t);
        int q = 1000;
        while (*h && *h != ',') {
            if ((h[0] == 'q' || h[0] == 'Q') && h[1] == '=') {
                h += 2;
                q = 0;
                int digits = 0, frac = 0;
                if (*h == '1') { q = 1000; h++; }
                else if (*h == '0') h++;
                if (*h == '.') {
                    h++;
                    while (*h >= '0' && *h <= '9') {
                        if (digits < 3 && q < 1000) { frac = frac * 10 + (*h - '0'); digits++; }
                        h++;
                    }
                    while (digits < 3) { frac *= 10; digits++; }
                    if (q < 1000) q = frac;
                }
                continue;
            }
            h++;
        }
        const int l = tag_lang(t, tn);
        if (l >= 0 && q > 0 && q > best_q) {
            best = l;
            best_q = q;
        }
    }
    return (i18n_lang_t)best;
}
