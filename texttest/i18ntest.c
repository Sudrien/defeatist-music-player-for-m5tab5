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
/* Sorted by strcmp, as the generator writes them. 8030: the values are
 * offsets into one text, which is stored as a raw deflate stream; this
 * fixture's is a single stored block (header 01, LEN, NLEN), which every
 * inflater takes, so the offsets stay readable against 131 bytes of text:
 * en 0, zh-CN 13, ja 41, es 48, pt 87, fr 87, it 110. */
const unsigned i18n_count = 4;
const char *const i18n_keys[] = { "OFF", "ON", "ONE", "Same album" };
const uint16_t i18n_voff[] = {
    /* en    */ I18N_NONE, I18N_NONE, 0, I18N_NONE,
    /* zh-CN */ 0, 4, I18N_NONE, 8,
    /* ja    */ 0, I18N_NONE, I18N_NONE, I18N_NONE,
    /* es    */ 0, 3, I18N_NONE, 7,
    /* pt    */ I18N_NONE, I18N_NONE, I18N_NONE, I18N_NONE,
    /* fr    */ I18N_NONE, 0, I18N_NONE, I18N_NONE,
    /* it    */ I18N_NONE, I18N_NONE, I18N_NONE, I18N_NONE,
};
const unsigned i18n_pcount = 1;
const char *const i18n_pkeys[] = { "%d tracks" };
const uint16_t i18n_pvoff[] = {
    /* en    */ 4, I18N_NONE,
    /* zh-CN */ I18N_NONE, 21,
    /* ja    */ I18N_NONE, I18N_NONE,
    /* es    */ 20, 29,
    /* pt    */ I18N_NONE, I18N_NONE,
    /* fr    */ 4, 13,
    /* it    */ 0, 11,
};
const uint32_t i18n_lbase[I18N_LANG_COUNT] = { 0, 13, 41, 48, 87, 87, 110 };
const unsigned i18n_text_len = 131;
#ifndef I18NTEST_BAD
#define NLEN_HI 0xFF
#else
#define NLEN_HI 0x00                    /* NLEN no longer LEN's complement */
#endif
const unsigned char i18n_blob[] = {
    0x01, 0x83, 0x00, 0x7C, NLEN_HI, 0x4F, 0x6E, 0x65, 0x00, 0x25, 0x64, 0x20,
    0x74, 0x72, 0x61, 0x63, 0x6B, 0x00, 0xE5, 0x85, 0xB3, 0x00, 0xE5, 0xBC,
    0x80, 0x00, 0xE5, 0x90, 0x8C, 0xE4, 0xB8, 0x80, 0xE4, 0xB8, 0x93, 0xE8,
    0xBE, 0x91, 0x00, 0x25, 0x64, 0x20, 0xE9, 0xA6, 0x96, 0x00, 0xE3, 0x82,
    0xAA, 0xE3, 0x83, 0x95, 0x00, 0x4E, 0x4F, 0x00, 0x53, 0xC3, 0x8D, 0x00,
    0x4D, 0x69, 0x73, 0x6D, 0x6F, 0x20, 0xC3, 0xA1, 0x6C, 0x62, 0x75, 0x6D,
    0x00, 0x25, 0x64, 0x20, 0x70, 0x69, 0x73, 0x74, 0x61, 0x00, 0x25, 0x64,
    0x20, 0x70, 0x69, 0x73, 0x74, 0x61, 0x73, 0x00, 0x4F, 0x55, 0x49, 0x00,
    0x25, 0x64, 0x20, 0x70, 0x69, 0x73, 0x74, 0x65, 0x00, 0x25, 0x64, 0x20,
    0x70, 0x69, 0x73, 0x74, 0x65, 0x73, 0x00, 0x25, 0x64, 0x20, 0x74, 0x72,
    0x61, 0x63, 0x63, 0x69, 0x61, 0x00, 0x25, 0x64, 0x20, 0x74, 0x72, 0x61,
    0x63, 0x63, 0x65, 0x00,
};
const unsigned i18n_blob_len = 136;

#ifdef I18NTEST_BAD
/* A blob that does not inflate: init fails, and every lookup is the key. */
static void fixture(void)
{
    assert(!i18n_init());
    assert(!i18n_init());
    assert(i18n_val(I18N_ZH_CN, 0) == NULL);
    i18n_set_lang(I18N_ZH_CN);
    assert(strcmp(_("OFF"), "OFF") == 0);
    assert(strcmp(_p("%d tracks", 1), "%d tracks") == 0);
    puts("i18ntest (bad blob): ok");
}
#else
static void fixture(void)
{
    assert(i18n_init());
    assert(i18n_val(I18N_ES, 1) && strcmp(i18n_val(I18N_ES, 1), "SÍ") == 0);
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
    i18n_set_lang(I18N_FR);                        /* 6068: 0 is one too */
    assert(strcmp(_p("%d tracks", 0), "%d piste") == 0);
    assert(strcmp(_p("%d tracks", 1), "%d piste") == 0);
    assert(strcmp(_p("%d tracks", 2), "%d pistes") == 0);
    assert(strcmp(_("ON"), "OUI") == 0);
    assert(strcmp(_("OFF"), "OFF") == 0);          /* fr NULL, en NULL: key */
    i18n_set_lang(I18N_IT);                        /* English's rule */
    assert(strcmp(_p("%d tracks", 0), "%d tracce") == 0);
    assert(strcmp(_p("%d tracks", 1), "%d traccia") == 0);
    i18n_set_lang(I18N_PT);                        /* all NULL: en text, en rule */
    assert(strcmp(_p("%d tracks", 0), "%d tracks") == 0);
    assert(strcmp(_p("%d tracks", 1), "%d track") == 0);
    assert(strcmp(_p("%d tracks", 2), "%d tracks") == 0);
    assert(strcmp(_("ONE"), "One") == 0);

    i18n_set_lang((i18n_lang_t)7);                 /* from a bad NVS byte */
    assert(i18n_lang() == I18N_EN);
    i18n_set_lang((i18n_lang_t)-1);
    assert(i18n_lang() == I18N_EN);
    assert(strcmp(i18n_lang_name((i18n_lang_t)9), "English") == 0);
    assert(strcmp(i18n_lang_name(I18N_JA), "日本語") == 0);
    assert(strcmp(i18n_lang_name(I18N_ES), "Español") == 0);
    assert(strcmp(i18n_lang_name(I18N_FR), "Français") == 0);
    assert(strcmp(i18n_lang_code(I18N_PT), "pt") == 0);
    assert(strcmp(i18n_lang_code(I18N_IT), "it") == 0);
    /* 6024: Accept-Language */
    assert(i18n_from_accept_language(NULL) == I18N_EN);
    assert(i18n_from_accept_language("") == I18N_EN);
    assert(i18n_from_accept_language("ja") == I18N_JA);
    assert(i18n_from_accept_language("es-MX,es;q=0.9,en;q=0.8") == I18N_ES);
    assert(i18n_from_accept_language("zh-CN,zh;q=0.9,en;q=0.8") == I18N_ZH_CN);
    assert(i18n_from_accept_language("zh-Hans-CN") == I18N_ZH_CN);
    assert(i18n_from_accept_language("zh-TW,zh;q=0.9,en;q=0.8") == I18N_ZH_CN);  /* zh generic */
    assert(i18n_from_accept_language("zh-TW,en;q=0.8") == I18N_EN);             /* no Hant */
    assert(i18n_from_accept_language("zh-Hant-HK, ja;q=0.5") == I18N_JA);
    assert(i18n_from_accept_language("fr-FR,fr;q=0.9,ja;q=0.7,es;q=0.6") == I18N_FR);  /* 6068 */
    assert(i18n_from_accept_language("de-DE,de;q=0.9,ja;q=0.7,es;q=0.6") == I18N_JA);
    assert(i18n_from_accept_language("pt-BR,pt;q=0.9") == I18N_PT);
    assert(i18n_from_accept_language("pt-PT") == I18N_PT);
    assert(i18n_from_accept_language("it-IT,en;q=0.5") == I18N_IT);
    assert(i18n_from_accept_language("en;q=0.2, es;q=0.21") == I18N_ES);
    assert(i18n_from_accept_language("es;q=0.5, ja;q=0.5") == I18N_ES);      /* tie: first */
    assert(i18n_from_accept_language("ja;q=0, es") == I18N_ES);              /* q=0: not this */
    assert(i18n_from_accept_language("ja;q=0") == I18N_EN);
    assert(i18n_from_accept_language("de, *;q=0.1") == I18N_EN);
    assert(i18n_from_accept_language("JA-jp") == I18N_JA);
    assert(i18n_from_accept_language("es_ES") == I18N_ES);
    assert(i18n_from_accept_language("ja;q=1.0") == I18N_JA);
    assert(i18n_from_accept_language("de;q=x, ;;, ,es;q=.8") == I18N_ES);    /* junk */
    assert(i18n_from_accept_language("abcdefghijklmnopqrstuvwxyz0123456789, ja") == I18N_JA);
    assert(strcmp(i18n_lang_code(I18N_ZH_CN), "zh-CN") == 0);
    assert(strcmp(i18n_lang_code((i18n_lang_t)9), "en") == 0);
    i18n_set_lang(I18N_JA);                             /* the screen's choice */
    assert(strcmp(_in(I18N_ES, "ON"), "SÍ") == 0);      /* does not touch it */
    assert(strcmp(_pin(I18N_ZH_CN, "%d tracks", 1), "%d 首") == 0);
    i18n_set_lang(I18N_EN);
    puts("i18ntest (fixture): ok");
}
#endif
#else

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

extern const unsigned char i18n_on_screen[];
extern const unsigned char i18n_pon_screen[];

static int html_free(const char *s, const char *what)
{
    if (!strpbrk(s, "<>&")) return 1;
    fprintf(stderr, "i18ntest: a web string in %s has HTML in it: \"%s\"\n", what, s);
    return 0;
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
    /* 8030: the blob inflates to exactly the text the offsets index. */
    assert(i18n_init());
    assert(i18n_init());                           /* again: still true */
    const char *p0 = i18n_val(I18N_ZH_CN, 0);
    for (unsigned l = 0; l < I18N_LANG_COUNT; l++) {
        assert(i18n_lbase[l] <= i18n_text_len);
        if (l) assert(i18n_lbase[l] >= i18n_lbase[l - 1]);
    }
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_count; i++) {
        const unsigned o = i18n_voff[i];
        if (o == I18N_NONE) continue;
        const unsigned l = i / i18n_count;
        const unsigned end = (l + 1 < I18N_LANG_COUNT ? i18n_lbase[l + 1] : i18n_text_len);
        assert(i18n_lbase[l] + o < end);
        assert(o == 0 || *(i18n_val((i18n_lang_t)l, i % i18n_count) - 1) == '\0');
    }
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_pcount * 2; i++) {
        const unsigned o = i18n_pvoff[i];
        if (o == I18N_NONE) continue;
        const unsigned l = i / (i18n_pcount * 2);
        const unsigned end = (l + 1 < I18N_LANG_COUNT ? i18n_lbase[l + 1] : i18n_text_len);
        assert(i18n_lbase[l] + o < end);
    }
    assert(i18n_val(I18N_ZH_CN, 0) == p0);         /* permanent pointers */
    assert(i18n_val((i18n_lang_t)9, 0) == NULL);
    assert(i18n_val(I18N_EN, i18n_count) == NULL);
    assert(i18n_pval(I18N_EN, 0, 2) == NULL);
    int ok = check_budgets();
    /* 6024: Ark12 holds what the device draws; a browser draws the
     * web pages' strings, and those must instead carry no HTML of their
     * own -- portal.c puts them into a page unescaped. */
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_count; i++) {
        const char *v = i18n_val((i18n_lang_t)(i / i18n_count), i % i18n_count);
        if (!v) continue;
        const char *ln = i18n_lang_name((i18n_lang_t)(i / i18n_count));
        if (i18n_on_screen[i % i18n_count]) ok &= glyphs_ok(v, ln);
        else ok &= html_free(v, ln);
    }
    for (unsigned i = 0; i < I18N_LANG_COUNT * i18n_pcount * 2; i++) {
        const char *v = i18n_pval((i18n_lang_t)(i / (i18n_pcount * 2)),
                                  (i / 2) % i18n_pcount, i % 2);
        if (!v) continue;
        const char *ln = i18n_lang_name((i18n_lang_t)(i / (i18n_pcount * 2)));
        if (i18n_pon_screen[(i / 2) % i18n_pcount]) ok &= glyphs_ok(v, ln);
        else ok &= html_free(v, ln);
    }
    for (unsigned i = 0; i < i18n_count; i++)
        if (!i18n_on_screen[i]) ok &= html_free(i18n_keys[i], "English");
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
            const char *want = i18n_val((i18n_lang_t)l, i);
            if (!want) want = i18n_val(I18N_EN, i);
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
