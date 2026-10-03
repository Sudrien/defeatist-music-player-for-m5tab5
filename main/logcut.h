/*
 * logcut.h -- how much of a string a log line can show without cutting
 * a character in half.
 *
 * `%.80s` is 80 BYTES. That is 80 letters of Latin and 40 of Arabic, and
 * when byte 80 falls inside a letter the terminal is handed half of it
 * and draws U+FFFD:
 *
 *     tab5_netstream: title: "عيسى الدغمري - رياح النصر - كلمات الشيخ خالد �"
 *
 * The title itself was whole -- tab5_mp3's line, and the screen, had all
 * of it -- so this is a log fault only, but a log is how a title is
 * checked, and one that ends in a replacement character reads as an
 * encoding bug in the stream.
 *
 *     const int n = logcut_len(s, 80);
 *     ESP_LOGI(TAG, "title: \"%.*s%s\"", n, s, logcut_more(s, n));
 *
 * Steps back from the limit over UTF-8 continuation bytes (10xxxxxx), so
 * the cut is before a lead byte or ASCII. A string that is not UTF-8 is
 * cut where the bytes say: at worst three bytes early, never in the
 * middle of a valid sequence. logcut_more() is "..." when anything was
 * left out, so a cut title says so.
 *
 * Host-tested: texttest/logcuttest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <string.h>

static inline int logcut_len(const char *s, int max)
{
    if (!s || max <= 0) return 0;
    const size_t len = strnlen(s, (size_t)max + 1);
    if (len <= (size_t)max) return (int)len;
    int n = max;
    /* At most three continuation bytes precede a boundary in UTF-8. */
    for (int k = 0; k < 3 && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80; k++) n--;
    return n;
}

static inline const char *logcut_more(const char *s, int n)
{
    return (s && s[n]) ? "..." : "";
}
