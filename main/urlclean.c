/*
 * urlclean.c -- see urlclean.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "urlclean.h"

#include <string.h>
#include <strings.h>

/* Whole names. */
static const char *const s_exact[] = {
    "fbclid", "gclid", "dclid", "msclkid", "gbraid", "wbraid", "igshid",
    "mc_cid", "mc_eid", "yclid", "twclid", "ttclid",
    "awparams", "amsparams", "listenerid", "listener_id", "lsid", "tdtok",
    "aw_0_1st.playerid", "aw_0_1st.skey",
};
/* Prefixes: a family of names. */
static const char *const s_prefix[] = {
    "utm_", "aw_0_", "aw_", "_hs",
};

bool urlclean_is_tracker(const char *name, int len)
{
    if (!name || len <= 0) return false;
    for (size_t i = 0; i < sizeof(s_exact) / sizeof(s_exact[0]); i++) {
        const int n = (int)strlen(s_exact[i]);
        if (n == len && strncasecmp(name, s_exact[i], (size_t)n) == 0) return true;
    }
    for (size_t i = 0; i < sizeof(s_prefix) / sizeof(s_prefix[0]); i++) {
        const int n = (int)strlen(s_prefix[i]);
        if (len > n && strncasecmp(name, s_prefix[i], (size_t)n) == 0) return true;
    }
    return false;
}

int urlclean_strip(char *url)
{
    if (!url) return 0;
    /* The query is between the first '?' and the fragment; a '?' after
     * the '#' is the fragment's. */
    char *hash = strchr(url, '#');
    char *q = strchr(url, '?');
    if (!q || (hash && q > hash)) return 0;
    {
        /* Nothing to take out: the URL is left exactly as it came. */
        const char *e = hash ? hash : q + strlen(q);
        bool found = false;
        for (const char *r = q + 1; r < e && !found;) {
            const char *amp = memchr(r, '&', (size_t)(e - r));
            const char *pe = amp ? amp : e;
            const char *eq = memchr(r, '=', (size_t)(pe - r));
            found = pe > r && urlclean_is_tracker(r, (int)((eq ? eq : pe) - r));
            r = amp ? amp + 1 : e;
        }
        if (!found) return 0;
    }
    const size_t tail = hash ? strlen(hash) : 0;
    char *end = hash ? hash : q + strlen(q);

    /* Walk the parameters, copying the kept ones down over the query. */
    char *w = q + 1;
    const char *r = q + 1;
    int removed = 0;
    bool any = false;
    while (r < end) {
        const char *amp = memchr(r, '&', (size_t)(end - r));
        const char *pe = amp ? amp : end;
        const char *eq = memchr(r, '=', (size_t)(pe - r));
        const int nl = (int)((eq ? eq : pe) - r);
        if (pe > r && urlclean_is_tracker(r, nl)) {
            removed++;
        } else if (pe > r) {
            if (any) *w++ = '&';
            memmove(w, r, (size_t)(pe - r));
            w += pe - r;
            any = true;
        }
        r = amp ? amp + 1 : end;
    }
    if (!any) w = q;                        /* nothing left: no '?' either */
    if (hash) memmove(w, hash, tail + 1);
    else *w = '\0';
    return removed;
}
