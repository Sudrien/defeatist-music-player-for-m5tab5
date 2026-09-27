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
} remote_cmd_kind_t;

typedef struct {
    remote_cmd_kind_t kind;
    int value;
} remote_cmd_t;

/* The longest command worth reading. Anything longer is not one. */
#define REMOTEPROTO_CMD_MAX     (16)

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
 * The envelope as JSON: {"t":"wave","gen":G,"lv":"<hex>"} with two hex
 * digits per column. Returns the length, or 0 when it did not fit.
 */
size_t remoteproto_wave_json(const uint8_t *lv, int n, uint32_t gen,
                             char *out, size_t cap);

#ifdef __cplusplus
}
#endif
