/*
 * podhtml.h -- an episode's description, from HTML to lines of text.
 *
 * 8026. Podcast Index passes on what the feed said, and feeds say it in
 * HTML: <p>, <br>, <a href=...>, <strong>, entities. The screen wants
 * paragraphs gfx_draw_para() can wrap. So, in place:
 *
 *  - tags go; <br>, </p>, </div>, </li>, <li> and the headings end a
 *    line, everything else is nothing (an <a> keeps its text, not its
 *    address);
 *  - <script> and <style> go with what is inside them;
 *  - &amp; &lt; &gt; &quot; &apos; &#39; &nbsp; and &#NNN; / &#xHH; become
 *    their characters (UTF-8), unknown ones stay as written;
 *  - runs of spaces become one, and line ends never two in a row (no
 *    empty lines -- the screen has no room for them), none at either end.
 *
 * In place, because every rewrite is shorter than what it replaces: the
 * longest a numeric entity's character can be is four bytes, and the
 * shortest entity that makes one, "&#65536;", is eight.
 *
 * NOT AN HTML PARSER. A '>' inside an attribute's quotes ends the tag
 * early and what follows shows as text; a feed that does that loses some
 * tidiness, not the description.
 *
 * NO FREERTOS AND NO ALLOCATION, so podhtmltest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* Whether the tag name at `t` (after '<' and any '/') is `name`. */
static inline bool podhtml_is(const char *t, const char *name)
{
    const size_t n = strlen(name);
    if (strncasecmp(t, name, n) != 0) return false;
    const char c = t[n];
    return c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\n' || c == '\r' || c == '\0';
}

static inline size_t podhtml_utf8(uint32_t cp, char *o)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63));
                        o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* The entity at `p` ('&'): its text into `o`, and how many bytes of `p`
 * it was; 0 if it is not one this knows. */
static inline size_t podhtml_entity(const char *p, char *o, size_t *wrote)
{
    static const struct { const char *n; char c; } named[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
    };
    const char *semi = NULL;
    for (size_t i = 1; i < 12 && p[i]; i++) if (p[i] == ';') { semi = p + i; break; }
    if (!semi) return 0;
    const size_t len = (size_t)(semi - p) + 1;
    if (p[1] == '#') {
        uint32_t cp = 0;
        const bool hex = p[2] == 'x' || p[2] == 'X';
        const char *d = p + (hex ? 3 : 2);
        if (d == semi) return 0;
        for (; d < semi; d++) {
            const char c = *d;
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) return 0;
            cp = cp * (hex ? 16 : 10) + (uint32_t)v;
            if (cp > 0x10FFFF) cp = 0x110000;
        }
        if (cp == 0xA0) cp = ' ';
        *wrote = podhtml_utf8(cp, o);
        if (*wrote > len) return 0;             /* cannot happen; never grows */
        return len;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        const size_t nl = strlen(named[i].n);
        if (len == nl + 2 && !strncmp(p + 1, named[i].n, nl)) {
            o[0] = named[i].c;
            *wrote = 1;
            return len;
        }
    }
    return 0;
}

/* `s`, rewritten in place as described above. */
static inline void podhtml_text(char *s)
{
    if (!s) return;
    char *w = s;
    const char *r = s;
    int blank = 2;          /* 2: at the start, nothing yet; 1: after a line end; 0: after text */
    bool space = false;
    while (*r) {
        if (*r == '<') {
            const char *t = r + 1;
            const bool closing = *t == '/';
            if (closing) t++;
            const char *gt = strchr(r, '>');
            if (!gt) break;                     /* an unclosed tag: the rest is markup */
            const bool skip = !closing && (podhtml_is(t, "script") || podhtml_is(t, "style"));
            const bool brk = podhtml_is(t, "br") || podhtml_is(t, "li") ||
                             (closing && (podhtml_is(t, "p") || podhtml_is(t, "div") ||
                                          podhtml_is(t, "li") || podhtml_is(t, "ul") ||
                                          podhtml_is(t, "ol") || podhtml_is(t, "h1") ||
                                          podhtml_is(t, "h2") || podhtml_is(t, "h3") ||
                                          podhtml_is(t, "h4") || podhtml_is(t, "blockquote")));
            r = gt + 1;
            if (skip) {
                const char *end = r;
                const char *name = podhtml_is(t, "script") ? "</script" : "</style";
                while (*end && strncasecmp(end, name, strlen(name))) end++;
                if (!*end) break;
                const char *g = strchr(end, '>');
                r = g ? g + 1 : end + strlen(end);
            }
            if (brk) {
                /* One line end after text, never two: a screen this size
                 * has no room for empty lines, and <li></li> and <p></p>
                 * would otherwise make them. */
                if (blank == 0) { *w++ = '\n'; blank = 1; }
                space = false;
            } else if (!skip && !space && blank == 0) {
                space = true;                   /* a tag between words is a gap */
            }
            continue;
        }
        char ch[4];
        size_t n = 1;
        size_t used = 1;
        if (*r == '&') {
            size_t wn = 0;
            const size_t len = podhtml_entity(r, ch, &wn);
            if (len) { used = len; n = wn; }
            else ch[0] = '&';
        } else {
            ch[0] = *r;
        }
        r += used;
        if (n == 1 && (ch[0] == ' ' || ch[0] == '\t' || ch[0] == '\r' || ch[0] == '\n')) {
            if (blank == 0) space = true;
            continue;
        }
        if (space) { *w++ = ' '; space = false; }
        memcpy(w, ch, n);
        w += n;
        blank = 0;
    }
    while (w > s && (w[-1] == '\n' || w[-1] == ' ')) w--;
    *w = '\0';
}
