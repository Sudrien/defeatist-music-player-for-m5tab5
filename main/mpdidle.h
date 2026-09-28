/*
 * mpdidle.h -- MPD's `idle`: the subsystem names, the latch each
 * connection carries, and the rule for what counts as a change.
 *
 * MPD.md step 10. PURE: no IDF and no sockets, so every rule here is
 * tested on a host (texttest/mpdidletest.c) before a client relies on it.
 * mpd.c owns the connections and calls these; this file decides what is
 * said and when.
 *
 * WHY idle MATTERS. It is how every modern client avoids polling: the
 * client sends `idle`, the server says nothing until something changes,
 * then names what changed and the client re-reads it. A client that
 * relies on it -- ncmpcpp, MALP -- shows a stale screen forever when it
 * is missing or wrong, which looks like a network fault and is not.
 *
 * EVERY RULE BELOW WAS READ OUT OF MPD 0.20's SOURCE, not recalled, and
 * three of them are not what a reasonable guess gives:
 *
 *   - `changed:` lines come out in MPD's BIT ORDER (IdleFlags.cxx) --
 *     database, stored_playlist, playlist, player, mixer, ... -- and not
 *     in the order things happened. Client parsers do not care; a test
 *     that compares bytes does.
 *   - Answering an idle clears EVERY pending event, including those the
 *     client did not subscribe to (ClientIdle.cxx, `idle_flags = 0`). A
 *     client that idles on `player` alone and is woken by it has lost any
 *     `mixer` that was waiting. Copied, because a client that subscribes
 *     narrowly and later widens is relying on MPD's behaviour, whatever
 *     its merits.
 *   - `noidle` from a client that is NOT idling gets NO RESPONSE AT ALL
 *     -- not an OK (ClientProcess.cxx). The race it exists for is a
 *     client sending `noidle` just as the server answers its `idle`: the
 *     client then reads one response, the idle answer, and a stray OK
 *     would desynchronise it for the life of the connection.
 *
 * And the ones that are easier to guess but must still be exact:
 * `noidle` answers a bare OK and LEAVES pending events latched; any line
 * other than `noidle` while idling closes the connection; `noidle` is
 * matched as a raw line after trailing bytes <= 0x20 are stripped, so it
 * is not a verb at all (mpdproto.h); subsystem names are case-insensitive;
 * an unknown one is ACK_ERROR_ARG "Unrecognized idle event: NAME"; and a
 * bare `idle` subscribes to everything.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MPD's subsystems, with MPD's bit values (src/IdleFlags.hxx). All
 * thirteen are named so that a client subscribing to one this device
 * never raises -- `sticker`, `mount` -- is accepted rather than ACKed, as
 * MPD accepts it; the ones this device CAN raise are marked.
 */
enum {
    MPD_IDLE_DATABASE        = 1u << 0,   /* raised: a reindex changed the index */
    MPD_IDLE_STORED_PLAYLIST = 1u << 1,
    MPD_IDLE_PLAYLIST        = 1u << 2,   /* raised: the queue's version moved */
    MPD_IDLE_PLAYER          = 1u << 3,   /* raised: play/pause, song, seek, tag */
    MPD_IDLE_MIXER           = 1u << 4,   /* raised: volume */
    MPD_IDLE_OUTPUT          = 1u << 5,
    MPD_IDLE_OPTIONS         = 1u << 6,   /* raised: random/repeat/single/consume */
    MPD_IDLE_STICKER         = 1u << 7,
    MPD_IDLE_UPDATE          = 1u << 8,   /* raised: a reindex started or ended */
    MPD_IDLE_SUBSCRIPTION    = 1u << 9,
    MPD_IDLE_MESSAGE         = 1u << 10,
    MPD_IDLE_NEIGHBOR        = 1u << 11,
    MPD_IDLE_MOUNT           = 1u << 12,
};
#define MPD_IDLE_COUNT      (13)

/* A bare `idle`: MPD uses ~0, so a subsystem added later is included. */
#define MPD_IDLE_ALL        (~0u)

/*
 * The longest idle answer: every name on its own `changed: ` line, and
 * the OK. "stored_playlist" is the longest name at 15, so 13 * (9 + 15 +
 * 1) + 3 is 328; rounded up so a caller's buffer is not sized to the
 * byte.
 */
#define MPDIDLE_ANSWER_MAX  (384)

/* The name of one subsystem bit, or NULL for anything that is not
 * exactly one of the thirteen. */
const char *mpdidle_name(uint32_t bit);

/*
 * `idle`'s arguments. True with *mask set -- MPD_IDLE_ALL for none. False
 * with *bad set to the index of the first name that is not a subsystem,
 * for the ACK: MPD_ACK_ARG, "Unrecognized idle event: " + argv[*bad].
 * Case-insensitive, as MPD's StringEqualsCaseASCII.
 */
bool mpdidle_parse(int argc, char *const argv[], uint32_t *mask, int *bad);

/*
 * Whether a line, with its newline already removed, is `noidle`. Trailing
 * bytes <= 0x20 are ignored (MPD's StripRight), so a CRLF client's "\r"
 * does not stop it matching; leading ones are not, so " noidle" is not
 * `noidle` -- which, from an idling client, closes the connection, as in
 * MPD.
 */
bool mpdidle_is_noidle(const char *line);

/* ---- the latch: one per connection ------------------------------------ */

typedef struct {
    uint32_t pending;   /* raised since this connection last heard */
    uint32_t subs;      /* while waiting: what it asked to hear */
    bool     waiting;   /* inside an `idle` */
} mpd_idle_t;

/* A new connection hears nothing that happened before it arrived. */
void mpdidle_init(mpd_idle_t *st);

/*
 * Something changed. Latched whether or not the connection is idling, so
 * a change between a client's OK and its next `idle` is not lost -- the
 * failure MPD.md names, a stale queue forever. True when the connection
 * must be answered now: it is waiting and this touches what it asked for.
 */
bool mpdidle_add(mpd_idle_t *st, uint32_t flags);

/*
 * The client said `idle`. True when something it asked for is already
 * pending, in which case the caller answers at once; otherwise the
 * connection waits, silently, and the caller stops its inactivity timer.
 */
bool mpdidle_wait(mpd_idle_t *st, uint32_t subs);

/*
 * The idle answer: a `changed: NAME` line per pending subscribed
 * subsystem in bit order, then OK. Clears ALL pending events and leaves
 * the wait (see the file comment for why all). 0, with the latch
 * untouched, if `cap` is too small; MPDIDLE_ANSWER_MAX is always enough.
 * Only for a connection that is waiting.
 */
size_t mpdidle_answer(mpd_idle_t *st, char *out, size_t cap);

/*
 * The client said `noidle`. When it was waiting: "OK\n" in `out`, the
 * wait left and pending events kept. When it was not: 0 and nothing to
 * send -- which is correct and not an error (see the file comment).
 */
size_t mpdidle_noidle(mpd_idle_t *st, char *out, size_t cap);

/* ---- what counts as a change ----------------------------------------- */

/*
 * The part of the player's state that `idle` reports on, as mpd.c's
 * snapshot has it. `modes` is the four MPD flags as bits (repeat 1,
 * random 2, single 4, consume 8) so a change is one compare.
 */
typedef struct {
    int      state;         /* mpd_state_t */
    uint32_t id;            /* the song id; 0 with none */
    uint32_t version;       /* the queue's */
    int      volume;        /* <0 unknown */
    uint8_t  modes;
    bool     updating;      /* a reindex is running */
    int32_t  elapsed_ms;    /* <0 unknown */
} mpd_idle_view_t;

/*
 * The previous view and when it was taken. Zeroed means "no view yet",
 * and the first call reports nothing -- there is nobody to tell about the
 * difference between nothing and the first state, and a client that
 * connects later reads the state itself.
 */
typedef struct {
    mpd_idle_view_t last;
    int64_t         last_us;
    bool            have;
} mpd_idle_track_t;

/*
 * How far the position may be from where it should be, between two
 * views, before it counts as a seek. The position arrives in WHOLE
 * SECONDS (ui_state_t's pos_sec), so a normal tick can look up to a
 * second early or late against the clock; two is the remote's
 * REMOTE_POS_SLIP_S for the same reason. A seek of less than this is not
 * reported -- the cost of a position with no fractions.
 */
#define MPDIDLE_SEEK_SLIP_MS    (2000)

/*
 * The subsystems that changed between the last view and this one, taken
 * `now_us` into the boot. `db_changed` is the caller's: a reindex that
 * finished and altered the index (MPD raises `database` only when the
 * update modified something -- UpdateService::RunDeferred).
 *
 * PLAYER for a state change, a new song, or a seek -- and a seek is a
 * JUMP between consecutive views, not drift from a clock. Measured the
 * remote's way (against a reference that only resets when something is
 * sent) a stalled stream would count as a seek every two seconds for as
 * long as it stalled; pass to pass, a stall is a position standing still
 * and a seek is one that moves too far at once. While paused, any change
 * of position is a seek.
 *
 * PLAYLIST when the version moved, which mpd.c already does for a new
 * song and for a stream whose tags changed in place. A tag change is also
 * PLAYER: MPD raises both (Player::... OnPlayerTagModified, then the
 * queue's TagModified), and the caller passes `tags_changed` for it
 * because the tags are not in this view.
 *
 * MIXER for volume, OPTIONS for the modes, UPDATE when a reindex starts
 * AND when it ends (MPD raises it at both), DATABASE per `db_changed`.
 * OUTPUT never: there is one output and it cannot be switched.
 */
uint32_t mpdidle_changes(mpd_idle_track_t *t, const mpd_idle_view_t *now,
                         int64_t now_us, bool tags_changed, bool db_changed);

#ifdef __cplusplus
}
#endif
