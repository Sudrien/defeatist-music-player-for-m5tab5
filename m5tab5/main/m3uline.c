/*
 * m3uline.c -- see m3uline.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "m3uline.h"

#include <string.h>
#include <strings.h>

static bool ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s), t = strlen(tail);
    return n > t && strcasecmp(s + n - t, tail) == 0;
}

bool m3u_is_name(const char *name)
{
    return name && (ends_with(name, ".m3u") || ends_with(name, ".m3u8"));
}

m3u_enc_t m3u_enc_of_name(const char *name)
{
    return (name && ends_with(name, ".m3u8")) ? M3U_ENC_UTF8 : M3U_ENC_UNKNOWN;
}

bool m3u_directive(const char *line, m3u_enc_t *enc)
{
    if (!line || strncasecmp(line, "#EXTENC:", 8) != 0) return false;
    const char *v = line + 8;
    while (*v == ' ' || *v == '\t') v++;
    if (strncasecmp(v, "UTF-8", 5) == 0 || strncasecmp(v, "UTF8", 4) == 0) {
        if (enc) *enc = M3U_ENC_UTF8;
    } else if (strncasecmp(v, "ISO-8859-1", 10) == 0 || strncasecmp(v, "LATIN1", 6) == 0 ||
               strncasecmp(v, "LATIN-1", 7) == 0 || strncasecmp(v, "CP1252", 6) == 0 ||
               strncasecmp(v, "WINDOWS-1252", 12) == 0) {
        if (enc) *enc = M3U_ENC_LATIN1;
    }
    return true;                /* a directive either way, not a path */
}

/* Strict UTF-8: no overlongs, no surrogates, nothing past U+10FFFF. */
static bool valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t k;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF)      { k = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
        else return false;
        if (i + k >= n) return false;       /* the sequence runs off the end */
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += k + 1;
    }
    return true;
}

int m3u_line_clean(char *line, size_t cap, m3u_enc_t enc)
{
    if (!line || cap == 0) return -1;
    size_t n = 0;
    while (n < cap && line[n]) n++;
    if (n == cap) return -1;            /* not terminated inside cap */

    /* The line ending and trailing blanks. */
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
    line[n] = '\0';

    /* A byte-order mark: a file's first line, from Windows. */
    if (n >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        memmove(line, line + 3, n - 2);
        n -= 3;
    }

    /* Backslashes, one or escaped, to one '/'. */
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        if (line[r] == '\\') {
            while (r + 1 < n && line[r + 1] == '\\') r++;
            line[w++] = '/';
        } else {
            line[w++] = line[r];
        }
    }
    n = w;
    line[n] = '\0';

    /* The encoding. */
    const bool latin1 = enc == M3U_ENC_LATIN1 ||
                        (enc == M3U_ENC_UNKNOWN && !valid_utf8((const unsigned char *)line, n));
    if (!latin1) return (int)n;

    size_t high = 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)line[i] >= 0x80) high++;
    if (n + high + 1 > cap) return -1;
    /* From the end, so nothing is read after it is written over. */
    size_t o = n + high;
    line[o] = '\0';
    for (size_t i = n; i-- > 0;) {
        const unsigned char c = (unsigned char)line[i];
        if (c < 0x80) {
            line[--o] = (char)c;
        } else {
            line[--o] = (char)(0x80 | (c & 0x3F));
            line[--o] = (char)(0xC0 | (c >> 6));
        }
    }
    return (int)(n + high);
}
