/*
 * mpduri.c -- see mpduri.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpduri.h"

#include <string.h>

#include "remoteproto.h"

const char *mpduri_mount(int vol)
{
    switch (vol) {
    case MPDURI_VOL_SD:  return MPDURI_MOUNT_SD;
    case MPDURI_VOL_USB: return MPDURI_MOUNT_USB;
    default:             return NULL;
    }
}

const char *mpduri_name(int vol)
{
    const char *m = mpduri_mount(vol);
    return m ? m + 1 : NULL;                /* the mount without its '/' */
}

int mpduri_split(const char *uri, const char **rel)
{
    if (rel) *rel = "";
    if (!uri || !uri[0]) return -1;
    for (int v = 0; v < MPDURI_VOLS; v++) {
        const char *n = mpduri_name(v);
        const size_t nl = strlen(n);
        if (strncmp(uri, n, nl) != 0) continue;
        if (uri[nl] == '\0') return v;
        if (uri[nl] == '/') {
            if (rel) *rel = uri + nl + 1;
            return v;
        }
    }
    return -1;
}

bool mpduri_is_root(const char *uri)
{
    return uri && uri[0] == '\0';
}

bool mpduri_ok(const char *uri, bool allow_root)
{
    if (!uri) return false;
    if (uri[0] == '\0') return allow_root;

    const size_t len = strlen(uri);
    if (len > MPDURI_MAX) return false;
    /*
     * An index path is relative, and this line is REDUNDANT: a leading
     * '/' makes the first segment empty, which the walk below refuses
     * anyway, so removing it changes no answer -- confirmed by a mutation
     * that the suite could not catch. It stays because it is the rule
     * that matters most here, stated where a reader looks rather than
     * inferred from a segment loop twenty lines down, and because
     * remoteproto_path_ok() has the mirror-image line at the same place.
     * Marked so it is not mistaken for load-bearing.
     */
    if (uri[0] == '/') return false;
    if (uri[len - 1] == '/') return false;

    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)uri[i];
        if (c < 0x20 || c == 0x7F || c == '\\') return false;
    }

    /* Every segment: not empty, not "." or "..". The walk is
     * remoteproto_path_ok()'s, starting at 0 rather than 1 because there
     * is no leading slash to skip. */
    size_t s = 0;
    while (s <= len) {
        size_t e = s;
        while (e < len && uri[e] != '/') e++;
        const size_t sl = e - s;
        if (sl == 0) return false;
        if (sl == 1 && uri[s] == '.') return false;
        if (sl == 2 && uri[s] == '.' && uri[s + 1] == '.') return false;
        s = e + 1;
    }
    return true;
}

bool mpduri_dir(const char *uri, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!mpduri_ok(uri, true)) return false;

    const size_t len = strlen(uri);
    if (len == 0) return true;                  /* the root is "" */
    if (len + 2 > cap) return false;
    memcpy(out, uri, len);
    out[len]     = '/';
    out[len + 1] = '\0';
    return true;
}

bool mpduri_to_vfs(const char *uri, midx_src_t *const src[MPDURI_VOLS],
                   char *out, size_t cap, int *vol_out)
{
    if (vol_out) *vol_out = -1;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!src) return false;
    /* Files only, so the root is not a resolvable uri. */
    if (!mpduri_ok(uri, false)) return false;

    /* 5191: the volume the URI names, and only that one. */
    const char *rel = "";
    const int v = mpduri_split(uri, &rel);
    if (v < 0 || !rel[0] || !src[v]) return false;
    midx_src_t *s = src[v];

    midx_rec_t r;
    if (midx_find(s, rel, &r) < 0) return false;
    /* A tombstone is not a file on the card. */
    if (r.flags & MIDX_F_DEAD) return false;

    const char *const mount = mpduri_mount(v);
    const size_t ml = strlen(mount), ul = strlen(rel);
    if (ml + 1 + ul + 1 > cap) return false;
    memcpy(out, mount, ml);
    out[ml] = '/';
    memcpy(out + ml + 1, rel, ul + 1);

    /* MPD.md's reuse, arriving where it fits. */
    if (!remoteproto_path_ok(out, ml + 1 + ul)) {
        out[0] = '\0';
        return false;
    }
    if (vol_out) *vol_out = v;
    return true;
}

bool mpduri_from_vfs(const char *vfs, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!vfs) return false;

    for (int v = 0; v < MPDURI_VOLS; v++) {
        const char *const mount = mpduri_mount(v);
        const size_t ml = strlen(mount);
        /* The mount, then a '/', then something: "/sd" alone is the
         * volume root and not a file, and "/sdcard/x" is not under "/sd"
         * however much it looks like it -- which is what the explicit
         * slash test is for. */
        if (strncmp(vfs, mount, ml) != 0 || vfs[ml] != '/') continue;

        /* 5191: "/usb/x" is "usb/x" -- the mount without its '/'. */
        const char *const rel = vfs + 1;
        const size_t rl = strlen(rel);
        if (rl <= ml || rl + 1 > cap) return false;
        memcpy(out, rel, rl + 1);
        /* What comes out must be a uri, or the two directions disagree
         * and a client sends back something that will not resolve. */
        if (!mpduri_ok(out, false)) {
            out[0] = '\0';
            return false;
        }
        return true;
    }
    return false;
}
