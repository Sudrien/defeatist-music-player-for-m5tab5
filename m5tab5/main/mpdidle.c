/*
 * mpdidle.c -- see mpdidle.h. MPD.md step 10.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpdidle.h"

#include <stdio.h>
#include <string.h>

#include "mpdproto.h"   /* mpd_state_t */

/* MPD's order, which is also its bit order (src/IdleFlags.cxx). */
static const char *const s_names[MPD_IDLE_COUNT] = {
    "database", "stored_playlist", "playlist", "player", "mixer", "output",
    "options", "sticker", "update", "subscription", "message", "neighbor",
    "mount",
};

const char *mpdidle_name(uint32_t bit)
{
    for (int i = 0; i < MPD_IDLE_COUNT; i++)
        if (bit == (1u << i)) return s_names[i];
    return NULL;
}

/* MPD's StringEqualsCaseASCII: ASCII letters fold, nothing else does. */
static bool eq_ascii_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}

bool mpdidle_parse(int argc, char *const argv[], uint32_t *mask, int *bad)
{
    uint32_t m = 0;
    for (int i = 0; i < argc; i++) {
        uint32_t bit = 0;
        for (int k = 0; k < MPD_IDLE_COUNT; k++) {
            if (eq_ascii_nocase(argv[i], s_names[k])) { bit = 1u << k; break; }
        }
        if (!bit) {
            if (bad) *bad = i;
            return false;
        }
        m |= bit;
    }
    /* "No argument means that the client wants to receive everything"
     * (handle_idle) -- tested as `m == 0` there, which only a bare idle
     * can produce, since every name sets a bit. */
    *mask = m ? m : MPD_IDLE_ALL;
    return true;
}

bool mpdidle_is_noidle(const char *line)
{
    static const char word[] = "noidle";
    const size_t n = sizeof(word) - 1;
    if (strncmp(line, word, n) != 0) return false;
    for (const char *p = line + n; *p; p++)
        if ((unsigned char)*p > 0x20) return false;
    return true;
}

void mpdidle_init(mpd_idle_t *st)
{
    st->pending = 0;
    st->subs = 0;
    st->waiting = false;
}

bool mpdidle_add(mpd_idle_t *st, uint32_t flags)
{
    st->pending |= flags;
    return st->waiting && (st->pending & st->subs) != 0;
}

bool mpdidle_wait(mpd_idle_t *st, uint32_t subs)
{
    st->waiting = true;
    st->subs = subs;
    return (st->pending & st->subs) != 0;
}

size_t mpdidle_answer(mpd_idle_t *st, char *out, size_t cap)
{
    const uint32_t hear = st->pending & st->subs;
    size_t len = 0;
    for (int i = 0; i < MPD_IDLE_COUNT; i++) {
        if (!(hear & (1u << i))) continue;
        const int n = snprintf(out + len, cap - len, "changed: %s\n", s_names[i]);
        if (n < 0 || (size_t)n >= cap - len) return 0;
        len += (size_t)n;
    }
    if (cap - len < 4) return 0;
    memcpy(out + len, "OK\n", 4);
    len += 3;
    /* ALL of them, not just what was heard: ClientIdle.cxx. */
    st->pending = 0;
    st->waiting = false;
    return len;
}

size_t mpdidle_noidle(mpd_idle_t *st, char *out, size_t cap)
{
    if (!st->waiting) return 0;
    if (cap < 4) return 0;
    memcpy(out, "OK\n", 4);
    st->waiting = false;
    return 3;
}

uint32_t mpdidle_changes(mpd_idle_track_t *t, const mpd_idle_view_t *now,
                         int64_t now_us, bool tags_changed, bool db_changed)
{
    if (!t->have) {
        t->last = *now;
        t->last_us = now_us;
        t->have = true;
        return 0;
    }
    const mpd_idle_view_t *p = &t->last;
    uint32_t f = 0;

    if (now->state != p->state || now->id != p->id || tags_changed) {
        f |= MPD_IDLE_PLAYER;
    } else if (now->elapsed_ms >= 0 && p->elapsed_ms >= 0) {
        if (now->state == MPD_STATE_PAUSE) {
            if (now->elapsed_ms != p->elapsed_ms) f |= MPD_IDLE_PLAYER;
        } else if (now->state == MPD_STATE_PLAY) {
            const int64_t expect = (int64_t)p->elapsed_ms + (now_us - t->last_us) / 1000;
            const int64_t slip = (int64_t)now->elapsed_ms - expect;
            if (slip > MPDIDLE_SEEK_SLIP_MS || slip < -MPDIDLE_SEEK_SLIP_MS)
                f |= MPD_IDLE_PLAYER;
        }
    } else if ((now->elapsed_ms < 0) != (p->elapsed_ms < 0) &&
               now->state != MPD_STATE_STOP) {
        /* A position appearing or going away on the same song -- a
         * stream's first stats, say -- is new player state to a client
         * that drew none. */
        f |= MPD_IDLE_PLAYER;
    }

    if (now->version != p->version)   f |= MPD_IDLE_PLAYLIST;
    if (now->volume != p->volume)     f |= MPD_IDLE_MIXER;
    if (now->modes != p->modes)       f |= MPD_IDLE_OPTIONS;
    if (now->updating != p->updating) f |= MPD_IDLE_UPDATE;
    if (db_changed)                   f |= MPD_IDLE_DATABASE;

    t->last = *now;
    t->last_us = now_us;
    return f;
}
