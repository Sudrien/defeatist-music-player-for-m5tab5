/*
 * mpdfilter.c -- MPD 0.21's filter expressions. See mpdfilter.h. 5238.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpdfilter.h"

#include <string.h>

#include "casefold.h"       /* 5244: the index's folding, so they agree */

/* ---- the parser ---------------------------------------------------------- */

typedef struct {
    const char  *p;
    mpdfilter_t *f;
    int          depth;
} parser_t;

static bool fail(parser_t *ps, const char *msg, bool unsupported)
{
    if (!ps->f->err) {
        ps->f->err = msg;
        ps->f->unsupported = unsupported;
    }
    return false;
}

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t') ps->p++;
}

static bool is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

/* A word into `w` (NUL-terminated, at most cap-1 bytes). False if none. */
static bool word(parser_t *ps, char *w, size_t cap)
{
    skip_ws(ps);
    size_t n = 0;
    while (is_word(*ps->p)) {
        if (n + 1 >= cap) return false;
        w[n++] = *ps->p++;
    }
    w[n] = '\0';
    return n > 0;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

/* A quoted string, unescaped into the filter's buffer. */
static bool string(parser_t *ps, const char **out)
{
    skip_ws(ps);
    const char q = *ps->p;
    if (q != '"' && q != '\'') return fail(ps, "Quoted string expected", false);
    ps->p++;
    mpdfilter_t *f = ps->f;
    char *const start = f->buf + f->used;
    for (;;) {
        char c = *ps->p;
        if (c == '\0') return fail(ps, "Closing quote not found", false);
        ps->p++;
        if (c == q) break;
        if (c == '\\') {
            c = *ps->p;
            if (c == '\0') return fail(ps, "Closing quote not found", false);
            ps->p++;
        }
        if (f->used + 1 >= sizeof(f->buf)) return fail(ps, "Filter expression too long", false);
        f->buf[f->used++] = c;
    }
    f->buf[f->used++] = '\0';
    *out = start;
    return true;
}

static int new_node(parser_t *ps, mpdf_kind_t kind)
{
    mpdfilter_t *f = ps->f;
    if (f->n >= MPDF_MAX_NODES) {
        fail(ps, "Filter expression too complex", false);
        return -1;
    }
    mpdf_node_t *n = &f->node[f->n];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->child = n->next = -1;
    return f->n++;
}

static bool expect(parser_t *ps, char c, const char *msg)
{
    skip_ws(ps);
    if (*ps->p != c) return fail(ps, msg, false);
    ps->p++;
    return true;
}

/* MPD's tag names this catalog can answer, and the rest. */
static int tag_field(const char *w)
{
    if (ieq(w, "title")) return MPDF_TITLE;
    if (ieq(w, "artist") || ieq(w, "albumartist")) return MPDF_ARTIST;
    if (ieq(w, "album")) return MPDF_ALBUM;
    if (ieq(w, "file")) return MPDF_FILE;
    if (ieq(w, "any")) return MPDF_ANY;
    return MPDF_NONE;
}

static int parse_expr(parser_t *ps);

/* After a '(' that does not open a nested expression: the inside of one. */
static int parse_term(parser_t *ps)
{
    char w[32];
    if (!word(ps, w, sizeof(w))) {
        fail(ps, "Word expected", false);
        return -1;
    }

    if (ieq(w, "base")) {
        const int i = new_node(ps, MPDF_N_BASE);
        if (i < 0 || !string(ps, &ps->f->node[i].value)) return -1;
        return i;
    }
    if (ieq(w, "modified-since")) {
        fail(ps, "modified-since is not supported: the library keeps no dates a client can compare",
             true);
        return -1;
    }
    if (ieq(w, "AudioFormat")) {
        fail(ps, "AudioFormat is not supported: the library keeps no formats", true);
        return -1;
    }

    const int field = tag_field(w);
    skip_ws(ps);
    mpdf_op_t op;
    if (strncmp(ps->p, "==", 2) == 0) { op = MPDF_EQ; ps->p += 2; }
    else if (strncmp(ps->p, "!=", 2) == 0) { op = MPDF_NE; ps->p += 2; }
    else if (strncmp(ps->p, "=~", 2) == 0 || strncmp(ps->p, "!~", 2) == 0) {
        fail(ps, "regular expressions are not supported by this player", true);
        return -1;
    } else {
        char o[16];
        if (!word(ps, o, sizeof(o)) || !(ieq(o, "contains") || ieq(o, "starts_with"))) {
            fail(ps, "Unknown filter operator", false);
            return -1;
        }
        op = ieq(o, "contains") ? MPDF_CONTAINS : MPDF_STARTS;
    }
    const int i = new_node(ps, MPDF_N_CMP);
    if (i < 0) return -1;
    ps->f->node[i].op = op;
    ps->f->node[i].field = field;
    if (!string(ps, &ps->f->node[i].value)) return -1;
    return i;
}

static int parse_expr(parser_t *ps)
{
    if (++ps->depth > MPDF_MAX_DEPTH) {
        fail(ps, "Filter expression too deeply nested", false);
        return -1;
    }
    if (!expect(ps, '(', "'(' expected")) return -1;
    skip_ws(ps);

    int i;
    if (*ps->p == '!') {
        ps->p++;
        i = new_node(ps, MPDF_N_NOT);
        if (i < 0) return -1;
        const int c = parse_expr(ps);
        if (c < 0) return -1;
        ps->f->node[i].child = c;
    } else if (*ps->p == '(') {
        /* One nested expression, or several joined by AND. */
        const int first = parse_expr(ps);
        if (first < 0) return -1;
        skip_ws(ps);
        if (*ps->p == ')') {
            i = first;
        } else {
            i = new_node(ps, MPDF_N_AND);
            if (i < 0) return -1;
            ps->f->node[i].child = first;
            int last = first;
            for (;;) {
                skip_ws(ps);
                if (*ps->p == ')') break;
                char w[8];
                if (!word(ps, w, sizeof(w)) || strcmp(w, "AND") != 0) {
                    fail(ps, "'AND' expected", false);
                    return -1;
                }
                const int nx = parse_expr(ps);
                if (nx < 0) return -1;
                ps->f->node[last].next = nx;
                last = nx;
            }
        }
    } else {
        i = parse_term(ps);
        if (i < 0) return -1;
    }
    if (!expect(ps, ')', "')' expected")) return -1;
    ps->depth--;
    return i;
}

bool mpdfilter_parse(const char *s, mpdfilter_t *f)
{
    memset(f, 0, sizeof(*f));
    f->root = -1;
    parser_t ps = { .p = s ? s : "", .f = f, .depth = 0 };
    const int r = parse_expr(&ps);
    if (r < 0) return false;
    skip_ws(&ps);
    if (*ps.p != '\0') return fail(&ps, "Unparsed garbage after expression", false);
    f->root = r;
    return true;
}

/* ---- evaluation ------------------------------------------------------------ */

/*
 * 5244: folded comparison a code point at a time, by casefold.h -- the
 * folding the search file is written with, so a filter and the pass that
 * pre-filters for it cannot disagree. No buffers: this runs on the MPD
 * task, and a folded copy of a tag is a stack allocation CLAUDE.md says
 * not to make. 5256: the iterators casefold.h normalises with are 60
 * bytes each, and str_has() holds three.
 */
static bool str_eq(const char *a, const char *b, bool fold)
{
    if (!fold) return strcmp(a, b) == 0;
    /* 5256: normalised as well as folded -- casefold_get(). */
    casefold_it_t x, y;
    casefold_begin(&x, a);
    casefold_begin(&y, b);
    for (;;) {
        const uint32_t p = casefold_get(&x), q = casefold_get(&y);
        if (p != q) return false;
        if (!p) return true;
    }
}

/* Whether `needle`, folded, starts where the iterator `h` is. `h` is a
 * copy, a bookmark, and is used up. */
static bool starts_it(casefold_it_t h, const char *needle)
{
    casefold_it_t n;
    casefold_begin(&n, needle);
    for (;;) {
        const uint32_t y = casefold_get(&n);
        if (!y) return true;
        const uint32_t x = casefold_get(&h);
        if (!x || x != y) return false;
    }
}

static bool starts(const char *hay, const char *needle)
{
    casefold_it_t h;
    casefold_begin(&h, hay);
    return starts_it(h, needle);
}

static bool str_has(const char *hay, const char *needle, bool fold)
{
    if (!*needle) return true;
    if (!fold) return strstr(hay, needle) != NULL;
    casefold_it_t h;
    casefold_begin(&h, hay);
    do {
        if (starts_it(h, needle)) return true;
    } while (casefold_get(&h));             /* on by a code point of the folded text */
    return false;
}

static bool cmp_one(mpdf_op_t op, const char *v, const char *want, bool fold)
{
    switch (op) {
    case MPDF_EQ:       return str_eq(v, want, fold);
    case MPDF_NE:       return !str_eq(v, want, fold);
    case MPDF_CONTAINS: return str_has(v, want, fold);
    case MPDF_STARTS:   return fold ? starts(v, want) : strncmp(v, want, strlen(want)) == 0;
    }
    return false;
}

static bool eval(const mpdfilter_t *f, int i, const char *const field[MPDF_NFIELDS], bool fold)
{
    const mpdf_node_t *n = &f->node[i];
    switch (n->kind) {
    case MPDF_N_CMP:
        if (n->field == MPDF_NONE)
            /* No song has this tag: only "not equal" holds. */
            return n->op == MPDF_NE;
        if (n->field == MPDF_ANY) {
            /* MPD's "any": some field matches. For != that is "some
             * field differs", which is MPD's reading too. */
            for (int k = 0; k < MPDF_NFIELDS; k++)
                if (cmp_one(n->op, field[k] ? field[k] : "", n->value, fold)) return true;
            return false;
        }
        return cmp_one(n->op, field[n->field] ? field[n->field] : "", n->value, fold);
    case MPDF_N_BASE: {
        /* A folder of the library, exactly -- never folded. */
        const char *uri = field[MPDF_FILE] ? field[MPDF_FILE] : "";
        const size_t len = strlen(n->value);
        return len == 0 || (strncmp(uri, n->value, len) == 0 &&
                            (uri[len] == '/' || uri[len] == '\0'));
    }
    case MPDF_N_NOT:
        return !eval(f, n->child, field, fold);
    case MPDF_N_AND:
        for (int c = n->child; c >= 0; c = f->node[c].next)
            if (!eval(f, c, field, fold)) return false;
        return true;
    }
    return false;
}

bool mpdfilter_eval(const mpdfilter_t *f, const char *const field[MPDF_NFIELDS], bool fold)
{
    if (!f || f->root < 0) return false;
    return eval(f, f->root, field, fold);
}

static int required(const mpdfilter_t *f, int i, int *out, int k, int max)
{
    const mpdf_node_t *n = &f->node[i];
    if (n->kind == MPDF_N_AND) {
        for (int c = n->child; c >= 0; c = f->node[c].next) k = required(f, c, out, k, max);
    } else if (n->kind == MPDF_N_CMP && n->field >= 0 &&
               (n->op == MPDF_EQ || n->op == MPDF_CONTAINS || n->op == MPDF_STARTS) && k < max) {
        out[k++] = i;
    }
    return k;
}

int mpdfilter_required(const mpdfilter_t *f, int *out, int max)
{
    if (!f || f->root < 0 || max <= 0) return 0;
    return required(f, f->root, out, 0, max);
}
