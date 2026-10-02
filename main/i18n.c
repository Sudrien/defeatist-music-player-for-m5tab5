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

/* Flat, because a key count known only in i18n_tab.c cannot be an
 * array dimension here: [lang * i18n_count + key] and
 * [(lang * i18n_pcount + key) * 2 + form]. */
extern const char *const i18n_vals[];
extern const char *const i18n_pvals[];

static volatile int s_lang = I18N_EN;

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
    const char *v = i18n_vals[lang * i18n_count + i];
    if (v) return v;
    v = i18n_vals[I18N_EN * i18n_count + i];          /* an English rewording, if any */
    return v ? v : msgid;
}

/* CLDR cardinal rules, cut to what the table has: English one/other;
 * Spanish one/other too (CLDR's "many" is for exact millions);
 * Chinese and Japanese only other. Index 0 is one, 1 is other. */
static int plural_form(int lang, long n)
{
    if (lang == I18N_EN || lang == I18N_ES) return n == 1 ? 0 : 1;
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
    const char *v = i18n_pvals[(lang * i18n_pcount + i) * 2 + plural_form(lang, n)];
    if (v) return v;
    v = i18n_pvals[(I18N_EN * i18n_pcount + i) * 2 + plural_form(I18N_EN, n)];
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
