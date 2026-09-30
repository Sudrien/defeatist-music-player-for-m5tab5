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

/* 5173: a decimal in `d` (dl bytes, all of them), 0..2147483647 with no
 * sign and no leading zero -- "0" itself is fine, "07" is not. */
static bool dec(const char *d, size_t dl, int *out)
{
    if (dl < 1 || dl > 10 || (dl > 1 && d[0] == '0')) return false;
    long long v = 0;
    for (size_t i = 0; i < dl; i++) {
        if (d[i] < '0' || d[i] > '9') return false;
        v = v * 10 + (d[i] - '0');
    }
    if (v > 2147483647LL) return false;
    *out = (int)v;
    return true;
}

bool remoteproto_path_ok(const char *p, size_t len)
{
    if (!p || len == 0 || len >= REMOTEPROTO_PATH_MAX || p[0] != '/') return false;
    if (len == 1) return true;                              /* "/" */
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7F || c == '\\') return false;
    }
    if (p[len - 1] == '/') return false;
    /* The volume. */
    static const char *const vols[] = { "/sd", "/usb" };
    bool vol = false;
    for (size_t i = 0; i < 2; i++) {
        const size_t n = strlen(vols[i]);
        if (len >= n && memcmp(p, vols[i], n) == 0 && (len == n || p[n] == '/')) vol = true;
    }
    if (!vol) return false;
    /* Every segment: not empty, not "." or "..". */
    size_t s = 1;
    while (s <= len) {
        size_t e = s;
        while (e < len && p[e] != '/') e++;
        const size_t sl = e - s;
        if (sl == 0) return false;
        if (sl == 1 && p[s] == '.') return false;
        if (sl == 2 && p[s] == '.' && p[s + 1] == '.') return false;
        s = e + 1;
    }
    return true;
}

bool remoteproto_parse(const char *msg, size_t len, remote_cmd_t *out)
{
    if (!out) return false;
    out->kind = REMOTE_CMD_NONE;
    out->value = 0;
    out->value2 = 0;
    out->path = NULL;
    out->path_len = 0;
    if (!msg || len == 0 || len > REMOTEPROTO_CMD_MAX) return false;

    static const struct { const char *w; remote_cmd_kind_t k; } plain[] = {
        { "hello", REMOTE_CMD_HELLO }, { "play",  REMOTE_CMD_PLAY  },
        { "pause", REMOTE_CMD_PAUSE }, { "next",  REMOTE_CMD_NEXT  },
        { "prev",  REMOTE_CMD_PREV  }, { "star",  REMOTE_CMD_STAR  },
        { "qclear", REMOTE_CMD_QCLEAR },                            /* 5173 */
    };
    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        if (word_is(msg, len, plain[i].w)) { out->kind = plain[i].k; return true; }
    }

    /* 5123: the path verbs. The verb, one space, then a path that
     * passes remoteproto_path_ok() -- "ls" alone is not "ls /". */
    static const struct { const char *w; remote_cmd_kind_t k; } paths[] = {
        { "ls", REMOTE_CMD_LS }, { "open", REMOTE_CMD_OPEN }, { "playdir", REMOTE_CMD_PLAYDIR },
        { "add", REMOTE_CMD_ADD }, { "addnext", REMOTE_CMD_ADDNEXT },   /* 5173 */
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        const size_t n = strlen(paths[i].w);
        if (len > n + 1 && memcmp(msg, paths[i].w, n) == 0 && msg[n] == ' ' &&
            remoteproto_path_ok(msg + n + 1, len - n - 1) &&
            /* Only a listing may name the root; there is nothing there
             * to play. */
            (paths[i].k == REMOTE_CMD_LS || len - n - 1 > 1)) {
            out->kind = paths[i].k;
            out->path = msg + n + 1;
            out->path_len = len - n - 1;
            return true;
        }
    }

    /* 5173: the queue's id verbs. One id, or an id and a position,
     * separated by exactly one space. */
    static const struct { const char *w; remote_cmd_kind_t k; } ids[] = {
        { "qdel", REMOTE_CMD_QDEL }, { "qplay", REMOTE_CMD_QPLAY }, { "qmove", REMOTE_CMD_QMOVE },
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const size_t n = strlen(ids[i].w);
        if (len < n + 2 || memcmp(msg, ids[i].w, n) != 0 || msg[n] != ' ') continue;
        const char *a = msg + n + 1;
        const size_t al = len - n - 1;
        int id, pos = 0;
        if (ids[i].k == REMOTE_CMD_QMOVE) {
            const char *sp = memchr(a, ' ', al);
            if (!sp || !dec(a, (size_t)(sp - a), &id) ||
                !dec(sp + 1, al - (size_t)(sp - a) - 1, &pos)) return false;
        } else if (!dec(a, al, &id)) {
            return false;
        }
        if (id == 0) return false;          /* ids start at 1 */
        out->kind = ids[i].k;
        out->value = id;
        out->value2 = pos;
        return true;
    }

    int v;
    if (verb_num(msg, len, "vol", &v))  { out->kind = REMOTE_CMD_VOLUME; out->value = v; return true; }
    if (verb_num(msg, len, "seek", &v)) { out->kind = REMOTE_CMD_SEEK;   out->value = v; return true; }
    /* 5269: the settings, each with its own ceiling under verb_num()'s. */
    static const struct { const char *w; remote_cmd_kind_t k; int max; } sets[] = {
        { "rg", REMOTE_CMD_RG, 1 }, { "xfade", REMOTE_CMD_XFADE, REMOTEPROTO_XFADE_MAX },
        { "xfalbum", REMOTE_CMD_XFALBUM, 1 }, { "sleep", REMOTE_CMD_SLEEP, REMOTEPROTO_SLEEP_STEPS },
    };
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        if (verb_num(msg, len, sets[i].w, &v)) {
            if (v > sets[i].max) return false;
            out->kind = sets[i].k;
            out->value = v;
            return true;
        }
    }
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
 *
 * 5155: exported, for the reason remoteproto.h gives. The local name
 * stays as a wrapper so the two call sites below read as they did.
 */
size_t remoteproto_utf8_len(const unsigned char *s, size_t left)
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

static size_t utf8_len(const unsigned char *s, size_t left)
{
    return remoteproto_utf8_len(s, left);
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
    key(&b, "path", false);     put_str(&b, s->path);
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
    key(&b, "recoff", false);   put_bool(&b, s->rec_off);           /* 5217 */
    key(&b, "batt", false);     putf(&b, "%lld", (long long)s->batt_pct);
    key(&b, "chg", false);      put_bool(&b, s->charging);
    key(&b, "wave", false);     putf(&b, "%lld", (long long)s->wave);
    key(&b, "rg", false);       put_bool(&b, s->rg);                /* 5269 */
    key(&b, "xf", false);       putf(&b, "%lld", (long long)s->xfade);
    key(&b, "xfa", false);      put_bool(&b, s->xfalbum);
    key(&b, "sleep", false);    putf(&b, "%lld", (long long)s->sleep_step);
    key(&b, "sleepleft", false); putf(&b, "%lld", (long long)s->sleep_left);
    put(&b, "}", 1);

    if (b.over) { out[0] = '\0'; return 0; }
    return b.n;
}

size_t remoteproto_queue_frame(const remote_qrow_t *rows, int n, int from,
                               uint32_t version, int cur,
                               char *out, size_t cap, int *next)
{
    /* The tail every frame ends with, reserved while rows are added. */
    static const char tail_no[] = "],\"done\":false}";
    if (!out || cap <= sizeof(tail_no) || n < 0 || from < 0 || from > n || (n && !rows))
        return 0;
    buf_t b = { out, cap - (sizeof(tail_no) - 1), 0, false };
    out[0] = '\0';

    puts_(&b, "{\"t\":\"q\",\"v\":");
    putf(&b, "%lld", (long long)version);
    puts_(&b, ",\"cur\":");
    putf(&b, "%lld", (long long)cur);
    puts_(&b, ",\"total\":");
    putf(&b, "%lld", (long long)n);
    puts_(&b, ",\"from\":");
    putf(&b, "%lld", (long long)from);
    puts_(&b, ",\"rows\":[");
    if (b.over) { out[0] = '\0'; return 0; }

    int i = from;
    for (; i < n; i++) {
        const size_t mark = b.n;
        if (i > from) put(&b, ",", 1);
        put(&b, "[", 1);
        putf(&b, "%lld", (long long)rows[i].id);
        put(&b, ",", 1);
        put_str(&b, rows[i].name ? rows[i].name : "");
        put(&b, "]", 1);
        if (b.over) {
            /* This row did not fit: take it back and end the frame. */
            b.over = false;
            b.n = mark;
            out[mark] = '\0';
            break;
        }
    }
    if (i == from && from < n) { out[0] = '\0'; return 0; }  /* not even one */

    b.cap = cap;
    puts_(&b, i == n ? "],\"done\":true}" : tail_no);
    if (b.over) { out[0] = '\0'; return 0; }
    if (next) *next = i;
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
