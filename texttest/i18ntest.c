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
 * 6016: and that every character of every translation is in Ark12. The
 * 12px cut holds 18299 of the CJK block, not all of it, and a missing
 * one draws as a notdef box -- nothing fails, the word is just wrong on
 * the board. Here it fails.
 *
 * 6018: and that the NET tab's notes whose words change with state --
 * remote, MPD, benchmark, clock -- fit the three rows panel.c gives
 * them, at portrait width, in every language, with real-length
 * arguments formatted in. The fixed notes need no such check: the
 * layout measures them.
 *
 * What it cannot see: whether a translation fits its button. That is
 * the board's to show.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "i18n.h"
#ifndef I18NTEST_FIXTURE
#include "ark12.h"
#include "gfx.h"
#endif

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
extern const char *const i18n_pvals[];

/* Every codepoint of s in the font; names the first one that is not. */
static int glyphs_ok(const char *s, const char *what)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80)           { cp = *p; n = 1; }
        else if (*p >> 5 == 6)   { cp = *p & 0x1F; n = 2; }
        else if (*p >> 4 == 14)  { cp = *p & 0x0F; n = 3; }
        else                     { cp = *p & 0x07; n = 4; }
        for (int i = 1; i < n; i++) {
            assert((p[i] & 0xC0) == 0x80);          /* well-formed UTF-8 */
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        int w;
        uint16_t rows[ARK12_H];
        /* 6017: '\n' is a line break in a notice body, split before
         * drawing -- never drawn as a glyph. */
        if (cp != '\n' && !ark12_glyph(cp, &w, rows)) {
            fprintf(stderr, "i18ntest: U+%04X in %s is not in Ark12: \"%s\"\n",
                   (unsigned)cp, what, s);
            return 0;
        }
        p += n;
    }
    return 1;
}

/* 6018: panel.c's note width in portrait, and its budget for a note
 * whose words depend on state. */
#define NOTE_W      (720 - 48)
#define NOTE_SCALE  (2)
#define NOTE_BUDGET (3)

static int rows_of(const char *const *paras, int n)
{
    int rows = 0;
    for (int i = 0; i < n; i++)
        if (paras[i]) rows += gfx_para_rows(_(paras[i]), NOTE_SCALE, NOTE_W);
    return rows;
}

static int budget_ok(const char *what, const char *const *paras, int n)
{
    const int rows = rows_of(paras, n);
    if (rows <= NOTE_BUDGET) return 1;
    fprintf(stderr, "i18ntest: %s, %s: %d rows of %d\n",
            i18n_lang_name(i18n_lang()), what, rows, NOTE_BUDGET);
    return 0;
}

static int check_budgets(void)
{
    int ok = 1;
    for (int l = 0; l < I18N_LANG_COUNT; l++) {
        i18n_set_lang((i18n_lang_t)l);
        char open[64], cert[48], conn[64], who[48], when[48];
        snprintf(open, sizeof(open), _("Open %s"), "https://192.168.100.100/");
        snprintf(cert, sizeof(cert), _("Cert %.29s..."),
                 "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01");
        snprintf(conn, sizeof(conn), _("Connect an MPD app to %s"), "192.168.100.100:6600");
        snprintf(who, sizeof(who), _p("%d apps connected.", 12), 12);
        snprintf(when, sizeof(when), "%s, %s", "2026-10-01 21:53Z", _("a guess"));
        const char *nopass = "No password: anyone on it can use it.";
        const char *v[][3] = {
            { open, cert, nopass },
            { open, "in a browser on the same network.", nopass },
            { "Off while network setup has the page.", "It comes back when setup closes.", nopass },
            { "Waiting for a network address.", "Then it shows the address to open.", nopass },
            { "Needs a network: Wi-Fi or a cable.", "Then it shows the address to open.", nopass },
            { "Play, pause, skip, seek and volume from a browser on the same network.", NULL, nopass },
            { conn, who, nopass },
            { "Waiting for a network address.", "Then it shows the address to use.", nopass },
            { "Play, pause, skip, seek and volume from an MPD app such as MALP or mpc.", NULL, nopass },
            { "Reads the selected station without decoding it, to tell a slow "
              "network from a slow decoder.", "Stop playback first.", NULL },
            { when, "RESET: back to the build time; this boot's recordings are "
              "renamed to match.", NULL },
        };
        /* 6022: the benchmark running, and its result, at a 20- and a
         * 16-character station name and three-digit rates. */
        char brun[64], bres[64], bneed[64], bread[64];
        snprintf(brun, sizeof(brun), _("Reading %s without decoding..."), "Radio Paradise Mellow");
        snprintf(bres, sizeof(bres), _("%s: %d mean, %d peak kbit/s"), "Radio Paradise M", 320, 448);
        snprintf(bneed, sizeof(bneed), _("Station needs %d -- that is %d%%"), 320, 100);
        snprintf(bread, sizeof(bread), "8K %d / 2K %d kbit/s", 448, 320);
        const char *bench[][3] = {
            { brun, "About twenty seconds.", NULL },
            { bres, bneed, bread },
        };
        for (size_t i = 0; i < 2; i++) {
            char what[32];
            snprintf(what, sizeof(what), "benchmark %s", i ? "result" : "running");
            ok &= budget_ok(what, bench[i], 3);
        }
        for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
            char what[32];
            snprintf(what, sizeof(what), "NET note variant %zu", i);
            ok &= budget_ok(what, v[i], 3);
        }
    }
    /* 6022: the benchmark's two name lines, at their widest -- a
     * 20- and a 16-byte name, both counts at INT_MIN -- into panel.c's
     * 64-byte lines, untruncated. */
    for (int l = 0; l < I18N_LANG_COUNT; l++) {
        i18n_set_lang((i18n_lang_t)l);
        char line[64];
        const int a = snprintf(line, sizeof(line), _("Reading %s without decoding..."),
                               "12345678901234567890");
        const int b = snprintf(line, sizeof(line), _("%s: %d mean, %d peak kbit/s"),
                               "1234567890123456", -2147483647 - 1, -2147483647 - 1);
        if (a >= (int)sizeof(line) || b >= (int)sizeof(line)) {
            fprintf(stderr, "i18ntest: %s: a benchmark line is %d/%d bytes of 63\n",
                    i18n_lang_name((i18n_lang_t)l), a, b);
            ok = 0;
        }
    }
    i18n_set_lang(I18N_EN);
    return ok;
}

static void real_table(void)
{
    int ok = check_budgets();
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_count; i++)
        if (i18n_vals[i]) ok &= glyphs_ok(i18n_vals[i], i18n_lang_name((i18n_lang_t)(i / i18n_count)));
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_pcount * 2; i++)
        if (i18n_pvals[i]) ok &= glyphs_ok(i18n_pvals[i], i18n_lang_name((i18n_lang_t)(i / (i18n_pcount * 2))));
    for (int l = 0; l < I18N_LANG_COUNT; l++)
        ok &= glyphs_ok(i18n_lang_name((i18n_lang_t)l), "the picker");
    assert(ok);

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
