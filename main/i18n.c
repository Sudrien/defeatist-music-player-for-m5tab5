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

const char *i18n_get(const char *msgid)
{
    if (!msgid) return msgid;
    const int lang = s_lang;
    const int i = find(i18n_keys, i18n_count, msgid);
    if (i < 0) return msgid;
    const char *v = i18n_vals[lang * i18n_count + i];
    if (v) return v;
    v = i18n_vals[I18N_EN * i18n_count + i];          /* an English rewording, if any */
    return v ? v : msgid;
}

/* CLDR cardinal rules, cut to what the table has: English one/other;
 * Chinese and Japanese only other. Index 0 is one, 1 is other. */
static int plural_form(int lang, long n)
{
    if (lang == I18N_EN) return n == 1 ? 0 : 1;
    return 1;
}

const char *i18n_get_plural(const char *msgid, long n)
{
    if (!msgid) return msgid;
    const int lang = s_lang;
    const int i = find(i18n_pkeys, i18n_pcount, msgid);
    if (i < 0) return msgid;
    const char *v = i18n_pvals[(lang * i18n_pcount + i) * 2 + plural_form(lang, n)];
    if (v) return v;
    v = i18n_pvals[(I18N_EN * i18n_pcount + i) * 2 + plural_form(I18N_EN, n)];
    return v ? v : msgid;
}
