/*
 * i18n.h -- the screen's text in English, Mandarin (simplified),
 * Japanese or Spanish, chosen at run time. lv_i18n's shape without LVGL:
 *
 *     gfx_draw_text(..., _("Same album"), ...);
 *     static const char *const k[] = { N_("Off"), N_("On") };
 *     ... _(k[i]) ...
 *     snprintf(buf, n, _p("%d tracks", n), n);
 *
 * The English is the key. ./tools/i18n.py extract finds every marked
 * literal in main/ and lists it in i18n/<locale>.yml; compile turns those
 * into i18n_tab.c. The tool's docstring has the YAML format and the
 * rules.
 *
 * _() of anything the table does not hold -- an unmarked string, a
 * filename, a translation still at ~ -- returns its argument unchanged.
 * So marking is incremental, and _() on a pointer is always safe; it
 * never returns NULL for a non-NULL argument.
 *
 * Only for text a person reads on the screen. Not for log lines, MPD or
 * HTTP protocol words, NVS keys, or anything parsed back -- those must
 * stay byte-identical whatever the language.
 *
 * A translated format string has had its printf conversions checked
 * against the English by the compile step, so passing the result to
 * snprintf is as safe as passing the key.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    I18N_EN = 0,        /* also the fallback: the keys are English */
    I18N_ZH_CN,
    I18N_JA,
    I18N_ES,            /* 6014 */
    I18N_PT,            /* 6068 */
    I18N_FR,            /* 6068 */
    I18N_IT,            /* 6068 */
    I18N_LANG_COUNT,
} i18n_lang_t;

/* Takes effect on the next draw; nothing caches a translated pointer
 * across frames. Out-of-range values select English. Safe from any
 * task: one aligned int, and a stale read draws one frame in the old
 * language. */
void i18n_set_lang(i18n_lang_t lang);
i18n_lang_t i18n_lang(void);

/* Endonym for a language picker: "English", "简体中文", "日本語",
 * "Español".
 * Deliberately not translated -- a reader has to find their own. */
const char *i18n_lang_name(i18n_lang_t lang);

const char *i18n_get(const char *msgid);
const char *i18n_get_plural(const char *msgid, long n);

/* 6024: the web pages' side. Each request picks its own language from
 * the browser's Accept-Language header -- whatever the screen is set
 * to -- and looks its strings up in that one. English when the header
 * names nothing we have. */
const char *i18n_get_in(i18n_lang_t lang, const char *msgid);
const char *i18n_get_plural_in(i18n_lang_t lang, const char *msgid, long n);
i18n_lang_t i18n_from_accept_language(const char *header);
const char *i18n_lang_code(i18n_lang_t lang);      /* "en", "zh-CN", ... */
#define _in(l, s)      i18n_get_in((l), (s))
#define _pin(l, s, n)  i18n_get_plural_in((l), (s), (long)(n))

#define _(s)      i18n_get(s)
#define N_(s)     (s)

/* 6016: deliberately NOT translated -- a tab name, the language row, a
 * unit, a product or protocol name. Expands to its argument and does
 * nothing else; what it adds is the decision, written where the string
 * is, so English that was chosen is not mistaken for English nobody has
 * converted yet. Kept English here even where the same words are
 * translated elsewhere -- a console name that is also a screen label --
 * which tools/i18n.py extract lists (6021). */
#define same(s)   (s)
#define _p(s, n)  i18n_get_plural((s), (long)(n))

/* 8030: the translations are not C strings. They are one block of text,
 * deflated into flash (i18n_blob) and inflated once into PSRAM with the
 * ROM's tinfl, where they stay: a pointer _() returned is good for good,
 * whatever language is later chosen, and the web pages may ask for any
 * language at any time. 12 KB of pointers and 70 KB of strings in flash
 * became 6 KB of offsets and 27 KB of deflate.
 *
 * Called from app_main() before anything draws or serves. A first lookup
 * that finds it not yet called does it itself, but a task that finds it
 * running in another gets the key (English) for that call -- no task
 * waits on another here. If the PSRAM or the inflate fails the lookups
 * return their keys for good, and the log says so. True if the text is
 * there. */
bool i18n_init(void);

/* The generated tables, for i18n.c and the host test. */
#define I18N_NONE 0xFFFFu           /* an offset for "not translated" */
extern const unsigned i18n_count;
extern const char *const i18n_keys[];
extern const unsigned i18n_pcount;
extern const char *const i18n_pkeys[];
extern const uint16_t i18n_voff[];      /* [lang * i18n_count + key] */
extern const uint16_t i18n_pvoff[];     /* [(lang * i18n_pcount + key) * 2 + form] */
extern const uint32_t i18n_lbase[I18N_LANG_COUNT];
extern const unsigned i18n_text_len;
extern const unsigned char i18n_blob[];
extern const unsigned i18n_blob_len;

/* A value as the table holds it: NULL for "not translated", and for
 * every lookup when the text is not there. No fallback -- that is what
 * the lookups above add. */
const char *i18n_val(i18n_lang_t lang, unsigned key);
const char *i18n_pval(i18n_lang_t lang, unsigned key, unsigned form);
