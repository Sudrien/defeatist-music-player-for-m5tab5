/*
 * remoteproto.c -- see remoteproto.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "remoteproto.h"

#include <stdio.h>
#include <string.h>

/* ---- commands ---------------------------------------------------------- */

static bool word_is(const char *msg, size_t len, const char *w)
{
    const size_t n = strlen(w);
    return len == n && memcmp(msg, w, n) == 0;
}

/* "<verb> N", N a decimal 0..100 with no sign, no leading space, no
 * trailing bytes, and at most three digits. */
static bool verb_num(const char *msg, size_t len, const char *verb, int *out)
{
    const size_t n = strlen(verb);
    if (len < n + 2 || memcmp(msg, verb, n) != 0 || msg[n] != ' ') return false;
    const char *d = msg + n + 1;
    const size_t dl = len - n - 1;
    if (dl < 1 || dl > 3) return false;
    int v = 0;
    for (size_t i = 0; i < dl; i++) {
        if (d[i] < '0' || d[i] > '9') return false;
        v = v * 10 + (d[i] - '0');
    }
    if (v > 100) return false;
    *out = v;
    return true;
}

bool remoteproto_parse(const char *msg, size_t len, remote_cmd_t *out)
{
    if (!out) return false;
    out->kind = REMOTE_CMD_NONE;
    out->value = 0;
    if (!msg || len == 0 || len > REMOTEPROTO_CMD_MAX) return false;

    static const struct { const char *w; remote_cmd_kind_t k; } plain[] = {
        { "hello", REMOTE_CMD_HELLO }, { "play",  REMOTE_CMD_PLAY  },
        { "pause", REMOTE_CMD_PAUSE }, { "next",  REMOTE_CMD_NEXT  },
        { "prev",  REMOTE_CMD_PREV  }, { "star",  REMOTE_CMD_STAR  },
    };
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        if (word_is(msg, len, plain[i].w)) { out->kind = plain[i].k; return true; }
    }

    int v;
    if (verb_num(msg, len, "vol", &v))  { out->kind = REMOTE_CMD_VOLUME; out->value = v; return true; }
    if (verb_num(msg, len, "seek", &v)) { out->kind = REMOTE_CMD_SEEK;   out->value = v; return true; }
    return false;
}

/* ---- JSON -------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t cap, n;
    bool   over;
} buf_t;

static void put(buf_t *b, const char *s, size_t len)
{
    if (b->over) return;
    if (b->n + len + 1 > b->cap) { b->over = true; return; }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}

static void puts_(buf_t *b, const char *s) { put(b, s, strlen(s)); }

static void putf(buf_t *b, const char *fmt, long long v)
{
    char t[24];
    const int n = snprintf(t, sizeof(t), fmt, v);
    if (n > 0) put(b, t, (size_t)n);
}

/*
 * Length of the valid UTF-8 sequence at s (1..4), or 0 when it is not
 * one. Overlongs, surrogates and anything past U+10FFFF are invalid.
 */
static size_t utf8_len(const unsigned char *s, size_t left)
{
    const unsigned char c = s[0];
    if (c < 0x80) return 1;
    size_t n;
    uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (n > left) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

static void put_str(buf_t *b, const char *s)
{
    put(b, "\"", 1);
    const unsigned char *u = (const unsigned char *)s;
    size_t left = strlen(s);
    while (left) {
        const unsigned char c = *u;
        if (c == '"' || c == '\\') {
            const char e[2] = { '\\', (char)c };
            put(b, e, 2);
            u++; left--;
        } else if (c < 0x20 || c == 0x7F) {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            put(b, e, 6);
            u++; left--;
        } else {
            const size_t n = utf8_len(u, left);
            if (n) {
                put(b, (const char *)u, n);
                u += n; left -= n;
            } else {
                put(b, "\xEF\xBF\xBD", 3);     /* U+FFFD */
                u++; left--;
            }
        }
    }
    put(b, "\"", 1);
}

size_t remoteproto_json_str(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    buf_t b = { out, cap, 0, false };
    out[0] = '\0';
    put_str(&b, in ? in : "");
    if (b.over) { out[0] = '\0'; return 0; }
    return b.n;
}

static void key(buf_t *b, const char *k, bool first)
{
    if (!first) put(b, ",", 1);
    put(b, "\"", 1);
    puts_(b, k);
    put(b, "\":", 2);
}

static void put_bool(buf_t *b, bool v) { puts_(b, v ? "true" : "false"); }

size_t remoteproto_state_json(const remote_state_t *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) return 0;
    buf_t b = { out, cap, 0, false };
    out[0] = '\0';

    puts_(&b, "{\"t\":\"state\"");
    key(&b, "title", false);    put_str(&b, s->title);
    key(&b, "artist", false);   put_str(&b, s->artist);
    key(&b, "album", false);    put_str(&b, s->album);
    key(&b, "art", false);      put_str(&b, s->art);
    key(&b, "pos", false);      putf(&b, "%lld", (long long)s->pos_sec);
    key(&b, "len", false);      putf(&b, "%lld", (long long)s->len_sec);
    key(&b, "valid", false);    put_bool(&b, s->stats_valid);
    key(&b, "playing", false);  put_bool(&b, s->playing);
    key(&b, "seek", false);     put_bool(&b, s->can_seek);
    key(&b, "next", false);     put_bool(&b, s->has_next);
    key(&b, "vol", false);      putf(&b, "%lld", (long long)s->volume);
    key(&b, "muted", false);    put_bool(&b, s->muted);
    key(&b, "fav", false);      putf(&b, "%lld", (long long)s->fav);
    key(&b, "rec", false);      put_bool(&b, s->recording);
    key(&b, "count", false);    putf(&b, "%lld", (long long)s->rec_count);
    key(&b, "recok", false);    put_bool(&b, s->rec_ok);
    key(&b, "batt", false);     putf(&b, "%lld", (long long)s->batt_pct);
    key(&b, "chg", false);      put_bool(&b, s->charging);
    key(&b, "wave", false);     putf(&b, "%lld", (long long)s->wave);
    put(&b, "}", 1);

    if (b.over) { out[0] = '\0'; return 0; }
    return b.n;
}

size_t remoteproto_wave_json(const uint8_t *lv, int n, uint32_t gen,
                             char *out, size_t cap)
{
    if (!out || cap == 0 || n < 0) return 0;
    buf_t b = { out, cap, 0, false };
    out[0] = '\0';

    puts_(&b, "{\"t\":\"wave\",\"gen\":");
    putf(&b, "%lld", (long long)gen);
    puts_(&b, ",\"lv\":\"");
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n && lv; i++) {
        const char h[2] = { hex[lv[i] >> 4], hex[lv[i] & 15] };
        put(&b, h, 2);
    }
    puts_(&b, "\"}");

    if (b.over) { out[0] = '\0'; return 0; }
    return b.n;
}
