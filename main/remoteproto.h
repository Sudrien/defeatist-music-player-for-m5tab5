/*
 * remoteproto.h -- the browser remote's wire format, with no network
 * code in it.
 *
 * Everything the page sends is read here, and everything the player
 * sends it is written here. Both directions carry text that did not
 * originate in this program -- a command from anyone on the network, a
 * tag from any file on a card -- so this is split out of remote.c for
 * the same reason portalweb.c is split out of portal.c: it runs under
 * ASan and UBSan on a host (texttest/remoteprototest.c).
 *
 * THE COMMANDS are one short line of ASCII each, a verb and at most one
 * number:
 *
 *     hello            send me the whole state
 *     play | pause | next | prev | star
 *     vol N            N in 0..100
 *     seek N           N in 0..100, a percentage of the track
 *     ls PATH          list a folder ("/" lists the volumes)      5123
 *     open PATH        play a file; its folder becomes the list   5123
 *     playdir PATH     play a folder from the top                 5123
 *     add PATH         a file onto the end of the queue           5173
 *     addnext PATH     a file into the queue, to play next        5173
 *     qdel ID          the queue entry with that id, out          5173
 *     qmove ID POS     that entry to position POS                 5173
 *     qplay ID         play that entry                            5173
 *     qclear           empty the queue                            5173
 *
 * 5173: the queue verbs name an entry by its ID, not its position. The
 * page read the list a moment ago on another task, and a folder tap on
 * the glass may have replaced it since; a stale position would delete the
 * wrong track, a stale id finds nothing (ui.h's UI_ACTION_PLAY_ID has the
 * same reason). An id is 1..2147483647, a position 0..2147483647, both
 * plain decimal with no sign and no leading zero.
 *
 * PATH is a VFS path under a mounted volume -- "/sd/..." or "/usb/..." --
 * and nothing else: see remoteproto_path_ok(). It is the first thing the
 * page sends that names something on the card, so it is checked here,
 * where it is tested, before remote.c goes near a filesystem.
 *
 * Nothing else is accepted, and nothing here can start a recording: a
 * recording is someone's voice in the room the player is in, and the
 * person holding the phone may not be in that room. The page shows the
 * recording; it cannot make one.
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

typedef enum {
    REMOTE_CMD_NONE = 0,
    REMOTE_CMD_HELLO,
    REMOTE_CMD_PLAY,
    REMOTE_CMD_PAUSE,
    REMOTE_CMD_NEXT,
    REMOTE_CMD_PREV,
    REMOTE_CMD_STAR,
    REMOTE_CMD_VOLUME,      /* value 0..100 */
    REMOTE_CMD_SEEK,        /* value 0..100 */
    REMOTE_CMD_LS,          /* 5123: path */
    REMOTE_CMD_OPEN,        /* 5123: path */
    REMOTE_CMD_PLAYDIR,     /* 5123: path */
    REMOTE_CMD_ADD,         /* 5173: path */
    REMOTE_CMD_ADDNEXT,     /* 5173: path */
    REMOTE_CMD_QDEL,        /* 5173: value = id */
    REMOTE_CMD_QMOVE,       /* 5173: value = id, value2 = position */
    REMOTE_CMD_QPLAY,       /* 5173: value = id */
    REMOTE_CMD_QCLEAR,      /* 5173 */
} remote_cmd_kind_t;

/* A path command's path: the longest a VFS path here can be. */
#define REMOTEPROTO_PATH_MAX    (512)

typedef struct {
    remote_cmd_kind_t kind;
    int value;
    int value2;             /* 5173: QMOVE's position; 0 otherwise */
    /* For LS, OPEN and PLAYDIR: into the caller's message, NOT
     * terminated -- path_len is the truth. NULL otherwise. */
    const char *path;
    size_t      path_len;
} remote_cmd_t;

/* The longest command worth reading: a verb, a space and a path.
 * Anything longer is not one. */
#define REMOTEPROTO_CMD_MAX     (8 + REMOTEPROTO_PATH_MAX)

/*
 * Whether `p` (not terminated; `len` bytes) is a path the remote may
 * name: "/" (the volumes), or "/sd" or "/usb" and anything under them,
 * with no ".." or "." segment, no empty segment ("//"), no trailing
 * slash but the root's, no control bytes and nothing past
 * REMOTEPROTO_PATH_MAX - 1. Pure; says nothing about whether it exists.
 */
bool remoteproto_path_ok(const char *p, size_t len);

/*
 * Parse one command. `msg` need not be terminated. False, with `out`
 * set to REMOTE_CMD_NONE, for anything that is not exactly one of the
 * forms above: an unknown verb, a number out of range or with trailing
 * bytes, a missing number, a number where none belongs.
 */
bool remoteproto_parse(const char *msg, size_t len, remote_cmd_t *out);

/*
 * What the page is told. Strings are copied in by the player and may be
 * any bytes a tag contained; the JSON writer makes them valid UTF-8.
 */
typedef struct {
    char     title[128];
    char     artist[96];
    char     album[96];
    char     art[12];       /* the cover's key for /art, "" for none */
    char     path[512];     /* 5123: the shown file, for the page's list */
    uint32_t pos_sec;
    uint32_t len_sec;       /* 0 unknown */
    bool     stats_valid;   /* pos/len describe this track */
    bool     playing;
    bool     can_seek;
    bool     has_next;
    int      volume;        /* 0..100 */
    bool     muted;
    int      fav;           /* 0 hidden, 1 off, 2 on, 3 folder -- ui.h's order */
    bool     recording;
    int      rec_count;     /* 3..1 while counting down, 0 otherwise */
    bool     rec_ok;
    bool     rec_off;       /* 5217: Record from OFF -- no record mark */
    int      batt_pct;      /* -1 unknown */
    bool     charging;
    uint32_t wave;          /* the envelope's generation, see below */
} remote_state_t;

/*
 * The state as one JSON object, NUL-terminated. Returns its length, or 0
 * when it did not fit -- never a truncated object.
 *
 * Every string is escaped for JSON and made valid UTF-8 (an invalid byte
 * becomes U+FFFD): a browser closes a WebSocket that sends it a text
 * frame with invalid UTF-8 in it, and a Latin-1 tag is one.
 */
size_t remoteproto_state_json(const remote_state_t *s, char *out, size_t cap);

/*
 * 5174: the queue, sent to the page in frames the way a listing is.
 *
 *   {"t":"q","v":V,"cur":C,"total":N,"from":F,"rows":[[ID,"name"],...],
 *    "done":true|false}
 *
 * V is mpdq_version(), so the page can drop a frame from a list that has
 * since changed; C is the playing position or -1; F is the position of
 * this frame's first row. A row is the entry's id -- what qdel, qmove and
 * qplay take -- and a name to show, which the caller chooses (the file
 * name). A page rebuilds its list at from 0 and draws it at done.
 */
typedef struct {
    uint32_t    id;
    const char *name;
} remote_qrow_t;

/*
 * One frame, starting at row `from`, holding as many rows as fit `cap`.
 * Returns its length (NUL-terminated) and sets *next to the first row not
 * in it; `done` is true when *next == n. 0 when not even the frame's
 * frame fits, or when the first row alone does not -- never a frame with
 * no rows unless there are none to send (n == 0, or from == n).
 */
size_t remoteproto_queue_frame(const remote_qrow_t *rows, int n, int from,
                               uint32_t version, int cur,
                               char *out, size_t cap, int *next);

/*
 * 5120: one string as a JSON string literal, quotes included, with the
 * same escaping and UTF-8 repair as the state. Returns its length, or 0
 * when it did not fit (worst case is 6 bytes per input byte, plus 3).
 */
size_t remoteproto_json_str(const char *in, char *out, size_t cap);

/*
 * The length of the valid UTF-8 sequence at `s` (1..4), or 0 when it is
 * not one -- overlongs, surrogates and anything past U+10FFFF are not.
 *
 * 5155: exported rather than left static because mpdproto.c needs the
 * same judgement for a different repair. MPD.md predicted this ("the
 * UTF-8 repair in put_str() is needed here too ... Same function,
 * different escaping"): a browser CLOSES a WebSocket carrying invalid
 * UTF-8, and an MPD client instead shows mojibake or drops the response,
 * so both surfaces have to replace a bad byte and only the escaping
 * differs. A second copy of this function is the alternative, and the
 * parts that are easy to get wrong -- the overlong and surrogate tests --
 * are exactly the parts a copy would drift on.
 */
size_t remoteproto_utf8_len(const unsigned char *s, size_t left);

/*
 * The envelope as JSON: {"t":"wave","gen":G,"lv":"<hex>"} with two hex
 * digits per column. Returns the length, or 0 when it did not fit.
 */
size_t remoteproto_wave_json(const uint8_t *lv, int n, uint32_t gen,
                             char *out, size_t cap);

#ifdef __cplusplus
}
#endif
