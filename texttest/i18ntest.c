/*
 * i18ntest -- main/i18n.c's lookups, on the host, under ASan. 6013.
 *
 * Two builds of one file. Against fixtures (I18NTEST_FIXTURE, below) it
 * checks the rules: fallback to the key, an English rewording winning
 * over the key, the plural forms, out-of-range languages, a key that
 * is a prefix of another. Against the real main/i18n_tab.c it checks
 * what the generator promised: the keys are in strcmp order with no
 * duplicates, so bsearch finds every one of them, in every language.
 *
 * What it cannot see: whether a translation fits its button. That is
 * the board's to show.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"

#ifdef I18NTEST_FIXTURE
/* Sorted by strcmp, as the generator writes them. */
const unsigned i18n_count = 4;
const char *const i18n_keys[] = { "OFF", "ON", "ONE", "Same album" };
const char *const i18n_vals[] = {
    /* en */    NULL,  NULL,  "One", NULL,
    /* zh-CN */ "关",  "开",  NULL,  "同一专辑",
    /* ja */    "オフ", NULL,  NULL,  NULL,
    /* es */    "NO",  "SÍ",  NULL,  "Mismo álbum",
};
const unsigned i18n_pcount = 1;
const char *const i18n_pkeys[] = { "%d tracks" };
const char *const i18n_pvals[] = {
    /* en */    "%d track", NULL,
    /* zh-CN */ NULL,       "%d 首",
    /* ja */    NULL,       NULL,
    /* es */    "%d pista", "%d pistas",
};

static void fixture(void)
{
    assert(i18n_lang() == I18N_EN);
    assert(strcmp(_("OFF"), "OFF") == 0);          /* en, NULL: the key */
    assert(strcmp(_("ONE"), "One") == 0);          /* en rewording */
    assert(strcmp(_("not marked"), "not marked") == 0);
    const char *p = "dynamic";
    assert(_(p) == p);                             /* same pointer back */
    assert(_(NULL) == NULL);

    i18n_set_lang(I18N_ZH_CN);
    assert(strcmp(_("OFF"), "关") == 0);
    assert(strcmp(_("ON"), "开") == 0);            /* not confused with ONE */
    assert(strcmp(_("ONE"), "One") == 0);          /* zh NULL: en rewording */
    assert(strcmp(_("O"), "O") == 0);              /* prefix of three keys */
    assert(strcmp(_("Same album"), "同一专辑") == 0);

    i18n_set_lang(I18N_JA);
    assert(strcmp(_("OFF"), "オフ") == 0);
    assert(strcmp(_("ON"), "ON") == 0);            /* ja NULL, en NULL: key */

    /* Plurals: en picks by n; zh has only other; ja falls back to en's
     * form for the same n. */
    i18n_set_lang(I18N_EN);
    assert(strcmp(_p("%d tracks", 1), "%d track") == 0);
    assert(strcmp(_p("%d tracks", 2), "%d tracks") == 0);   /* NULL: key */
    assert(strcmp(_p("%d tracks", 0), "%d tracks") == 0);
    i18n_set_lang(I18N_ZH_CN);
    assert(strcmp(_p("%d tracks", 1), "%d 首") == 0);
    assert(strcmp(_p("%d tracks", 5), "%d 首") == 0);
    i18n_set_lang(I18N_JA);
    assert(strcmp(_p("%d tracks", 1), "%d track") == 0);    /* en text, en rule */
    assert(strcmp(_p("%d tracks", 3), "%d tracks") == 0);
    assert(strcmp(_p("unknown %d", 3), "unknown %d") == 0);
    i18n_set_lang(I18N_ES);                        /* 6014: one/other */
    assert(strcmp(_p("%d tracks", 1), "%d pista") == 0);
    assert(strcmp(_p("%d tracks", 0), "%d pistas") == 0);
    assert(strcmp(_p("%d tracks", 2), "%d pistas") == 0);
    assert(strcmp(_("ON"), "SÍ") == 0);
    assert(strcmp(_("ONE"), "One") == 0);          /* es NULL: en rewording */

    i18n_set_lang((i18n_lang_t)7);                 /* from a bad NVS byte */
    assert(i18n_lang() == I18N_EN);
    i18n_set_lang((i18n_lang_t)-1);
    assert(i18n_lang() == I18N_EN);
    assert(strcmp(i18n_lang_name((i18n_lang_t)9), "English") == 0);
    assert(strcmp(i18n_lang_name(I18N_JA), "日本語") == 0);
    assert(strcmp(i18n_lang_name(I18N_ES), "Español") == 0);
    puts("i18ntest (fixture): ok");
}
#else
extern const char *const i18n_vals[];

static void real_table(void)
{
    for (unsigned i = 1; i < i18n_count; i++)
        assert(strcmp(i18n_keys[i - 1], i18n_keys[i]) < 0);
    for (unsigned i = 1; i < i18n_pcount; i++)
        assert(strcmp(i18n_pkeys[i - 1], i18n_pkeys[i]) < 0);
    for (int l = 0; l < I18N_LANG_COUNT; l++) {
        i18n_set_lang((i18n_lang_t)l);
        for (unsigned i = 0; i < i18n_count; i++) {
            const char *want = i18n_vals[l * i18n_count + i];
            if (!want) want = i18n_vals[i];
            if (!want) want = i18n_keys[i];
            assert(_(i18n_keys[i]) == want);
        }
    }
    printf("i18ntest (main/i18n_tab.c): ok, %u strings, %u plurals\n",
           i18n_count, i18n_pcount);
}
#endif

int main(void)
{
#ifdef I18NTEST_FIXTURE
    fixture();
#else
    real_table();
#endif
    return 0;
}
