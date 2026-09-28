/*
 * mpd.c -- see mpd.h. MPD.md step 9.
 *
 * THE SHAPE. One task owns the listener and every client socket and
 * select()s over all of them, the way portal.c's DNS task owns its one
 * socket. Not esp_http_server: MPD is a line protocol on a bare TCP
 * socket, and an httpd instance would cost its own task, a control socket
 * and a listener to carry none of what it is for.
 *
 * NOTHING HERE CALLS THE PLAYER. The task reads a copy of the state that
 * ui_task published (mpd_publish()) and asks for presses through a queue
 * that ui_task drains (mpd_take()), which is remote.c's arrangement and
 * MPD.md's rule: the server task "never calls playlist_* or
 * request_track() itself".
 *
 * A PRESS WAITS FOR ui_task, where the remote's does not. MPD is
 * synchronous in a way the remote page is not: a client that is told OK
 * for `pause` and then asks `status` expects to see `state: pause`, and
 * a client that sees `play` instead toggles again. So ask() queues the
 * press with a sequence number and waits until mpd_publish() has run in
 * the pass that took it -- the pass whose state reflects it -- before the
 * OK goes out. Bounded by MPD_ASK_WAIT_MS, because ui_task `continue`s
 * past the take and the publish while the panel, the chooser or the sleep
 * page is open (player.c, 5118's note), and a client must still get an
 * answer then: it gets its OK late and the press lands when the page
 * closes, as a remote press does.
 *
 * WHAT "SERVICED" DOES NOT MEAN. The press has been dispatched, not
 * completed: `next` calls request_track(), and the new track reaches
 * s_shown_path when its first frame is heard, some passes later. So a
 * `status` straight after `next` can still name the old song. MPD's own
 * answer to that is `idle player`, which a client gets when the new song
 * is published (5160).
 *
 * THE QUEUE IS ONE ENTRY LONG. The device's queue is its folder
 * (playlist.c), and MPD.md step 4's switch-over -- the folder filling
 * mpdqueue.c, which is what would give a client the whole list with
 * stable ids -- is not done. Until it is, what a client sees is a window
 * of one: the track on screen at position 0, with an id that changes
 * when the track does and a playlist version that moves with it. That is
 * true about what is playing and silent about what comes next, which is
 * the honest subset. The alternative, `playlistlength: 0` beside
 * `state: play`, is a state MPD itself can never be in and one clients
 * handle badly; the other alternative, exposing playlist.c's array as the
 * queue, would hand out positions and ids with nothing behind them to
 * keep them stable, which is exactly the problem mpdqueue.c exists to
 * solve.
 *
 * COMMAND LISTS ARE RUN AS THEY ARRIVE, the tradeoff mpdproto.h left to
 * this file. MPD accumulates the list and runs it at command_list_end;
 * running each line as it is read needs no buffer sized by the client and
 * gives the same result for any client that finishes its list -- MPD
 * halts at the first failure having run what came before, and so does
 * this. It differs when a connection breaks mid-list, where MPD would
 * have run nothing and this has run the part it read. After a failure the
 * rest of the list is read and discarded up to command_list_end, which is
 * what MPD's answer amounts to.
 *
 * WHERE THIS IS KINDER THAN MPD, deliberately: MPD closes the connection
 * on a line it cannot tokenise and on a blank line. This ACKs and keeps
 * the connection, because on this device a reconnect costs a socket out
 * of a small budget and the client learns the same thing from the ACK.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpd.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "browser.h"
#include "ethernet.h"
#include "medialib.h"
#include "mpdidle.h"         /* 5160 */
#include "mpdmode.h"
#include "mpdproto.h"
#include "mpduri.h"
#include "stationlist.h"
#include "settings.h"         /* 5161: settings_rg_enabled() */
#include "stations.h"
#include "wifi.h"

static const char *TAG = "tab5_mpd";

/*
 * 6 KB, the remote's worker size (remote.c's w_task) and the top of
 * MPD.md's 4-6 KB. What runs on it: select()'s fd_set, the handlers'
 * locals -- a few dozen bytes each -- and mpdproto.c, whose largest local
 * is a 48-byte format buffer. Every buffer the size of a line or a
 * response is PSRAM from mpd_init(), per CLAUDE.md. The high-water mark
 * is logged when the task stops, so the first board run says what it
 * used rather than this comment guessing.
 *
 * It must never call netdec_open() (NETDEC_MIN_STACK is 24576), and it
 * cannot: it calls nothing in the player.
 */
#define MPD_STACK           (6144)
#define MPD_PRIO            (3)

/*
 * 5159: THE STACK IS IN PSRAM, allocated once in mpd_init() and reused
 * for every start. The first board run of 5158 showed internal free fall
 * about 6.8 KB across the switch being turned on -- 31903 to 25131, with
 * nothing else starting in between -- which is this stack plus the TCB
 * that xTaskCreate() takes from internal RAM. Internal RAM is the heap
 * this device runs out of (5147: "the figure to watch now"), and the same
 * run bottomed out at 1532 bytes once the remote came up as well.
 *
 * Static creation, not xTaskCreateWithCaps(): a WithCaps task that
 * deletes itself makes IDF spawn a helper task FROM INTERNAL RAM to free
 * it, and abort()s if that allocation fails -- a panic on the switch
 * being turned off, in exactly the condition this change is for. A static
 * task frees nothing when it is deleted, so turning the switch off
 * allocates nothing. The TCB is .bss, a few hundred bytes paid at boot
 * whether the switch is on or not; the 6 KB is PSRAM.
 *
 * Safe in PSRAM because this task never runs with the cache disabled: it
 * does sockets and formatting and never writes flash (that is ui_task's,
 * through settings). IDF refuses an external stack unless this option is
 * set -- default y on the P4 -- so a build without it fails here rather
 * than asserting in xTaskCreateStatic() on the board.
 */
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "mpd.c puts its task stack in PSRAM and needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
#endif

/* How often the task looks up from select() to see whether it has been
 * asked to stop, and to time clients out. */
#define MPD_SELECT_MS       (250)

/* MPD's own connection_timeout default (src/client/ClientGlobal.cxx,
 * CLIENT_TIMEOUT_DEFAULT). A client that says nothing for a minute is
 * closed, which on this device is also a socket given back. A client
 * parked in `idle` is exempt, as in MPD (5160). */
#define MPD_TIMEOUT_US      (60 * 1000000LL)

/* A client that stops reading must not stop the task: a send that cannot
 * complete in this long closes that client instead. */
#define MPD_SEND_TIMEOUT_S  (2)

/* How long a press waits for ui_task; see the file comment. A second is
 * several passes at the slowest rate ui_task runs (10 Hz). */
#define MPD_ASK_WAIT_MS     (1000)
/* How long a press waits for room in the queue before it is refused. */
#define MPD_ASK_QUEUE_MS    (100)
#define MPD_QUEUE_DEPTH     (8)

/* After a listener that would not bind, how long before trying again --
 * the remote's REMOTE_RETRY_US. */
#define MPD_RETRY_US        (5 * 1000000LL)

/* What is written between flushes. A song is at most MPDPROTO_SONG_MAX,
 * so this holds a response and its OK with room over; anything longer is
 * flushed in pieces. */
#define MPD_OUT_MAX         (4096)
/* One body before it is copied out: the larger of a song and a status. */
#define MPD_BODY_MAX        (MPDPROTO_SONG_MAX > MPDPROTO_STATUS_MAX ? \
                             MPDPROTO_SONG_MAX : MPDPROTO_STATUS_MAX)
/* An ACK's message, which may quote an argument off the wire. */
#define MPD_TEXT_MAX        (MPDPROTO_LINE_MAX + 64)
/* 5160: an idle answer is built in s_body too. */
_Static_assert(MPD_BODY_MAX >= MPDIDLE_ANSWER_MAX, "an idle answer must fit s_body");

/* ---- the state a client is told --------------------------------------- */

/* A station URL or a URI: STATION_URL_MAX is the larger of the two, and
 * matches NETSTREAM_URL_MAX (stationlist.h). */
#define MPD_URI_MAX         (STATION_URL_MAX)

typedef struct {
    mpd_state_t state;
    int         volume;
    mpd_modes_t modes;
    int32_t     elapsed_ms;     /* <0 unknown */
    int32_t     duration_ms;    /* <0 unknown */
    bool        can_seek;
    bool        updating;       /* a reindex is running */
    bool        rg;             /* 5161: ReplayGain on */
    bool        have_song;      /* uri[0], kept as its own field for clarity */
    uint32_t    id;             /* the song id; 0 with no song */
    uint32_t    version;        /* the playlist version; starts at 1 */
    char        uri[MPD_URI_MAX];
    char        title[128];
    char        artist[96];
    char        album[96];
} snap_t;

static SemaphoreHandle_t s_mu;      /* s_pub */
static snap_t           *s_pub;     /* ui_task writes under s_mu; PSRAM */
static snap_t           *s_next;    /* ui_task's scratch; PSRAM */
static snap_t           *s_view;    /* the server task's copy; PSRAM */
static uint32_t          s_last_id; /* ui_task's: ids are never reused */

/*
 * 5160: what changed, for `idle`. ui_task works it out once per pass in
 * mpd_publish() and ORs it in here under s_mu; the server task takes and
 * clears it and latches it into every connection (mpdidle_add()), so a
 * change is delivered to a client whether or not it was idling when it
 * happened -- MPD.md's "latched per connection, not broadcast".
 */
static uint32_t          s_events;  /* under s_mu */
static mpd_idle_track_t  s_track;   /* ui_task's */
static medialib_state_t  s_db_was[STORAGE_COUNT];  /* ui_task's */

/* ---- presses ------------------------------------------------------------ */

typedef struct {
    ui_action_t act;
    uint32_t    seq;
} req_t;

static QueueHandle_t     s_q;
static uint32_t          s_seq;             /* the server task's */
static volatile uint32_t s_taken_seq;       /* ui_task: the last one taken */
static volatile uint32_t s_done_seq;        /* ui_task: taken AND published */

/* ---- the server --------------------------------------------------------- */

typedef struct {
    int         fd;         /* -1: free */
    char       *in;         /* MPDPROTO_LINE_MAX, PSRAM */
    size_t      fill;
    mpd_list_t  list;
    bool        list_dead;  /* a sub-command failed; discard to the end */
    /* 5160: `idle` inside a command list, run at command_list_end. */
    bool        list_idle;
    uint32_t    list_idle_mask;
    mpd_idle_t  idle;       /* 5160: this connection's latch */
    bool        broken;     /* a send failed; close after this read */
    int64_t     last_us;
} conn_t;

static conn_t             s_conn[MPD_CLIENTS];
static char              *s_out;            /* MPD_OUT_MAX, PSRAM */
static size_t             s_out_len;
static char              *s_body;           /* MPD_BODY_MAX, PSRAM */
static char              *s_text;           /* MPD_TEXT_MAX, PSRAM */

/* Non-NULL while a task exists or has not yet been seen to be deleted;
 * only mpd_poll() writes it (5159). */
static TaskHandle_t       s_task;
static StaticTask_t       s_tcb;            /* 5159: internal .bss */
static StackType_t       *s_stack;          /* 5159: MPD_STACK, PSRAM */
static volatile bool      s_stop;
static volatile bool      s_listening;
static volatile int       s_nclients;
static int64_t            s_retry_us;

static bool have_ip(char *ip, size_t n)
{
    return wifi_sta_ip(ip, n) || ethernet_ip(ip, n);
}

/* ---- output ------------------------------------------------------------- */

static void flush(conn_t *c)
{
    size_t off = 0;
    while (off < s_out_len && !c->broken) {
        const int n = send(c->fd, s_out + off, s_out_len - off, 0);
        if (n <= 0) {
            ESP_LOGI(TAG, "client %d: send failed (errno %d); closing", c->fd, errno);
            c->broken = true;
            break;
        }
        off += (size_t)n;
    }
    s_out_len = 0;
}

static void put(conn_t *c, const char *s, size_t n)
{
    while (n && !c->broken) {
        if (s_out_len == MPD_OUT_MAX) flush(c);
        const size_t room = MPD_OUT_MAX - s_out_len;
        const size_t k = n < room ? n : room;
        memcpy(s_out + s_out_len, s, k);
        s_out_len += k;
        s += k;
        n -= k;
    }
}

static void puts_(conn_t *c, const char *s) { put(c, s, strlen(s)); }

static void putf(conn_t *c, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) put(c, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* An ACK raised by a handler, with MPD's wording supplied by the caller.
 * mpdproto_ack() sanitises both fields; see mpdproto.h. */
static void ack(conn_t *c, mpd_ack_t code, int idx, const char *verb,
                const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_body, MPD_BODY_MAX, fmt, ap);
    va_end(ap);
    const size_t n = mpdproto_ack(code, idx, verb, s_body, s_text, MPD_TEXT_MAX);
    if (n) put(c, s_text, n);
}

/* ---- argument parsing, MPD's (src/protocol/ArgParser.cxx) ---------------- */

/*
 * Each of these ACKs on failure with MPD's message and returns false, so a
 * handler reads `if (!arg_x(...)) return RES_ERR;`. The messages name the
 * argument unquoted after a colon, as MPD's do.
 */
typedef struct {
    conn_t     *c;
    int         idx;
    const char *verb;
} ctx_t;

static bool arg_int(const ctx_t *x, const char *s, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Integer expected: %s", s);
        return false;
    }
    if (errno == ERANGE || v < lo || v > hi) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Number too large: %s", s);
        return false;
    }
    *out = v;
    return true;
}

static bool arg_unsigned(const ctx_t *x, const char *s, unsigned long hi,
                         unsigned long *out)
{
    char *end;
    errno = 0;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Integer expected: %s", s);
        return false;
    }
    if (errno == ERANGE || v > hi) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Number too large: %s", s);
        return false;
    }
    *out = v;
    return true;
}

static bool arg_bool(const ctx_t *x, const char *s, bool *out)
{
    char *end;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || (v != 0 && v != 1)) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Boolean (0/1) expected: %s", s);
        return false;
    }
    *out = v == 1;
    return true;
}

/*
 * A time in seconds, fractional allowed, as milliseconds. `signed_ok` is
 * seekcur's form; the others refuse a negative with MPD's message.
 * Parsed as a double and rounded once, which is what MPD's SongTime::FromS
 * does with the float it parses -- and a client sends at most three
 * decimals, which a double carries exactly enough.
 */
static bool arg_time(const ctx_t *x, const char *s, bool signed_ok, int64_t *out_ms)
{
    char *end;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0') {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Float expected: %s", s);
        return false;
    }
    if (!signed_ok && v < 0) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Negative value not allowed: %s", s);
        return false;
    }
    /* Beyond a day is not a position in anything this plays; bounded so
     * the conversion below cannot overflow. */
    if (v > 86400.0 || v < -86400.0) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Number too large: %s", s);
        return false;
    }
    *out_ms = (int64_t)(v * 1000.0 + (v < 0 ? -0.5 : 0.5));
    return true;
}

/*
 * A position or a START:END range, for playlistinfo and plchanges --
 * MPD's ParseCommandArgRange, including "-1" for everything. `end` is
 * exclusive; an open end ("3:") is INT32_MAX.
 */
static bool arg_range(const ctx_t *x, const char *s, long *start, long *end)
{
    char *t;
    long v = strtol(s, &t, 10);
    if (t == s || (*t != '\0' && *t != ':')) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Integer or range expected: %s", s);
        return false;
    }
    if (v == -1 && *t == '\0') { *start = 0; *end = INT32_MAX; return true; }
    if (v < 0) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Number is negative: %s", s);
        return false;
    }
    *start = v;
    if (*t == ':') {
        char *t2;
        const char *from = t + 1;
        long e = strtol(from, &t2, 10);
        if (*t2 != '\0') {
            ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Integer or range expected: %s", s);
            return false;
        }
        if (t2 == from) e = INT32_MAX;
        if (e < 0) {
            ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Number is negative: %s", s);
            return false;
        }
        *end = e;
    } else {
        *end = v + 1;
    }
    return true;
}

/* ---- the snapshot -------------------------------------------------------- */

static void take_view(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_view, s_pub, sizeof(*s_view));
    xSemaphoreGive(s_mu);
}

static void put_song(conn_t *c, const snap_t *v)
{
    const mpd_song_t s = {
        .uri = v->uri,
        .title = v->title,
        .artist = v->artist,
        .album = v->album,
        .duration_ms = v->duration_ms,
        .pos = 0,
        .id = v->id,
    };
    const size_t n = mpdproto_song(&s, s_body, MPD_BODY_MAX);
    if (n) put(c, s_body, n);
}

/* ---- asking ui_task ------------------------------------------------------ */

/*
 * Queue a press and wait for the pass that publishes its effect. False,
 * with an ACK sent, when the queue stayed full -- which is a client
 * sending presses faster than ui_task runs, and it is told so rather than
 * having them dropped the way the remote drops a burst.
 */
static bool ask(const ctx_t *x, ui_action_kind_t kind, int value)
{
    const req_t r = { .act = { .kind = kind, .value = value }, .seq = ++s_seq };
    if (xQueueSend(s_q, &r, pdMS_TO_TICKS(MPD_ASK_QUEUE_MS)) != pdTRUE) {
        ack(x->c, MPD_ACK_SYSTEM, x->idx, x->verb, "player busy; try again");
        return false;
    }
    const TickType_t t0 = xTaskGetTickCount();
    while ((int32_t)(s_done_seq - r.seq) < 0) {
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(MPD_ASK_WAIT_MS)) {
            ESP_LOGI(TAG, "%s: the player has not taken it yet (a page is "
                     "open?); answering now, it lands later", x->verb);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/*
 * A seek to `ms`, as the one kind of seek the player takes: a percentage
 * of the track (UI_ACTION_SEEK, which is what the slider and the remote
 * send). So the resolution is a hundredth of the track -- 2 s of a
 * 3-minute song, 36 s of an hour-long recording -- and a client that asks
 * for 1:23 lands on the nearest percent. A millisecond seek is a new
 * action and a change to request_seek(), which is not this patch; the
 * cost is written here so that a client landing a few seconds off is not
 * a mystery.
 */
static bool seek_ms(const ctx_t *x, int64_t ms)
{
    const snap_t *v = s_view;
    if (!v->can_seek || v->duration_ms <= 0) {
        /* MPD's decoder throws runtime_error("Not seekable"), which its
         * CommandError.cxx maps to ACK_ERROR_UNKNOWN. */
        ack(x->c, MPD_ACK_UNKNOWN, x->idx, x->verb, "Not seekable");
        return false;
    }
    if (ms < 0) ms = 0;
    if (ms > v->duration_ms) ms = v->duration_ms;
    const int pct = (int)((ms * 100 + v->duration_ms / 2) / v->duration_ms);
    return ask(x, UI_ACTION_SEEK, pct);
}

/* `play 0` and `playid N` restart the song, where a bare `play` resumes:
 * MPD's PlayPosition() with a position starts that song from the top. */
static bool restart(const ctx_t *x)
{
    if (s_view->can_seek && !ask(x, UI_ACTION_SEEK, 0)) return false;
    return ask(x, UI_ACTION_PLAY, 0);
}

/* ---- idle (5160) --------------------------------------------------------- */

/* Answer an idling connection: its `changed:` lines and the OK, into the
 * output being built for it. MPD restarts the inactivity timeout here. */
static void idle_answer(conn_t *c)
{
    const size_t n = mpdidle_answer(&c->idle, s_body, MPD_BODY_MAX);
    if (n) put(c, s_body, n);
    c->last_us = esp_timer_get_time();
}

/* Start idling; answered at once if something it asked for is already
 * latched, and otherwise silent until something is. */
static void idle_enter(conn_t *c, uint32_t mask)
{
    if (mpdidle_wait(&c->idle, mask)) idle_answer(c);
}

/* ---- the verbs ---------------------------------------------------------- */

/* Not R_OK: <unistd.h> has that name (access()'s read bit), and close()
 * brings <unistd.h> in. */
/* RES_IDLE (5160): the connection is now idling, or has been answered
 * already; either way no OK follows -- MPD's CommandResult::IDLE. */
typedef enum { RES_OK = 0, RES_ERR, RES_CLOSE, RES_IDLE } result_t;

/* Which verbs this step answers. `commands` and `notcommands` are both
 * read from here, so they cannot disagree with the dispatcher below --
 * a verb added to one and not the other is a verb this lists wrongly. */
static bool answered(mpd_cmd_kind_t k)
{
    switch (k) {
    case MPD_CMD_PING: case MPD_CMD_CLOSE: case MPD_CMD_COMMANDS:
    case MPD_CMD_NOTCOMMANDS: case MPD_CMD_TAGTYPES: case MPD_CMD_URLHANDLERS:
    case MPD_CMD_DECODERS: case MPD_CMD_STATUS: case MPD_CMD_STATS:
    case MPD_CMD_CURRENTSONG: case MPD_CMD_CLEARERROR:
    case MPD_CMD_PLAY: case MPD_CMD_PLAYID: case MPD_CMD_PAUSE: case MPD_CMD_STOP:
    case MPD_CMD_NEXT: case MPD_CMD_PREVIOUS:
    case MPD_CMD_SEEK: case MPD_CMD_SEEKID: case MPD_CMD_SEEKCUR:
    case MPD_CMD_SETVOL: case MPD_CMD_VOLUME: case MPD_CMD_OUTPUTS:
    case MPD_CMD_PLAYLISTINFO: case MPD_CMD_PLAYLISTID: case MPD_CMD_PLAYLIST:
    case MPD_CMD_PLCHANGES: case MPD_CMD_PLCHANGESPOSID:
    case MPD_CMD_IDLE:                                  /* 5160 */
    case MPD_CMD_REPLAY_GAIN_MODE: case MPD_CMD_REPLAY_GAIN_STATUS:  /* 5161 */
        return true;
    default:
        return false;
    }
}

/* The last kind in mpdproto.h's enum, for walking the table. */
#define MPD_CMD_LAST    MPD_CMD_RM

static result_t run_cmd(conn_t *c, const mpd_cmd_t *cmd, int idx)
{
    const ctx_t x = { c, idx, cmd->verb };
    const char *const a0 = cmd->argc > 0 ? cmd->argv[0] : NULL;
    const char *const a1 = cmd->argc > 1 ? cmd->argv[1] : NULL;

    switch (cmd->kind) {
    case MPD_CMD_PING:
    case MPD_CMD_CLEARERROR:
        return RES_OK;

    case MPD_CMD_CLOSE:
        return RES_CLOSE;

    case MPD_CMD_IDLE: {
        /* 5160, handle_idle(): names are checked before anything waits,
         * and an unknown one is ARG, naming it. */
        uint32_t mask;
        int bad = 0;
        if (!mpdidle_parse(cmd->argc, cmd->argv, &mask, &bad)) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Unrecognized idle event: %s", cmd->argv[bad]);
            return RES_ERR;
        }
        if (c->list.active) {
            /* Inside a command list MPD runs the list at its end, and
             * the idle there stops the rest of it running. This runs a
             * list as it arrives, so the idle is held until
             * command_list_end and what follows it is discarded unrun --
             * the same result, reached from the other side. */
            c->list_idle = true;
            c->list_idle_mask = mask;
            return RES_IDLE;
        }
        idle_enter(c, mask);
        return RES_IDLE;
    }

    case MPD_CMD_COMMANDS:
    case MPD_CMD_NOTCOMMANDS: {
        const bool want = cmd->kind == MPD_CMD_COMMANDS;
        for (int k = 1; k <= (int)MPD_CMD_LAST; k++) {
            const char *verb = mpdproto_verb((mpd_cmd_kind_t)k);
            if (verb && answered((mpd_cmd_kind_t)k) == want) putf(c, "command: %s\n", verb);
        }
        return RES_OK;
    }

    case MPD_CMD_TAGTYPES:
        /* `tagtypes clear|enable|disable|all` is 0.21; MPDPROTO_VERSION
         * claims 0.20, where tagtypes takes nothing. */
        if (cmd->argc > 0) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "too many arguments for \"tagtypes\"");
            return RES_ERR;
        }
        /* The catalog's three tags (mediacat.h), and no others: a tag
         * listed here is a tag a client may filter on in step 12. */
        puts_(c, "tagtype: Artist\ntagtype: Album\ntagtype: Title\n");
        return RES_OK;

    case MPD_CMD_URLHANDLERS:
    case MPD_CMD_DECODERS:
        /* Empty, and correct: no URL can be added over MPD yet, and a
         * decoder list is only read by clients deciding what to add. */
        return RES_OK;

    case MPD_CMD_STATUS: {
        take_view();
        const snap_t *v = s_view;
        const mpd_status_t st = {
            .state = v->state,
            .volume = v->volume,
            .repeat = v->modes.repeat,
            .random = v->modes.random,
            .single = v->modes.single,
            .consume = v->modes.consume,
            .playlist_version = v->version,
            .playlist_length = v->have_song ? 1 : 0,
            .song = v->have_song ? 0 : -1,
            .songid = v->id,
            .next_song = -1,
            .elapsed_ms = v->elapsed_ms,
            .duration_ms = v->duration_ms,
            .bitrate = -1,
            .sample_rate = 0,
            .updating_db = v->updating ? 1u : 0u,
            .error = NULL,
        };
        const size_t n = mpdproto_status(&st, s_body, MPD_BODY_MAX);
        if (!n) {
            ack(c, MPD_ACK_SYSTEM, idx, cmd->verb, "status did not fit");
            return RES_ERR;
        }
        put(c, s_body, n);
        return RES_OK;
    }

    case MPD_CMD_STATS:
        /* `playtime` and the database counts are left out, not zeroed:
         * nothing counts play time, and the counts are step 12's. */
        putf(c, "uptime: %" PRIu32 "\n", (uint32_t)(esp_timer_get_time() / 1000000));
        return RES_OK;

    case MPD_CMD_CURRENTSONG:
        take_view();
        if (s_view->have_song) put_song(c, s_view);
        return RES_OK;

    case MPD_CMD_REPLAY_GAIN_STATUS:
        /* 5161: handle_replay_gain_status. "track" or "off": see
         * mpdmode.h for why album and auto never come back. */
        take_view();
        putf(c, "replay_gain_mode: %s\n", mpdmode_rg_name(s_view->rg));
        return RES_OK;

    case MPD_CMD_REPLAY_GAIN_MODE: {
        /* 5161: handle_replay_gain_mode. MPD's FromString throws
         * invalid_argument for an unknown name, which is ARG. */
        const int on = mpdmode_rg_from_name(a0);
        if (on < 0) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Unrecognized replay gain mode");
            return RES_ERR;
        }
        return ask(&x, UI_ACTION_REPLAYGAIN, on) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_OUTPUTS:
        /* One output, always on: MPD 0.20's three fields
         * (src/output/OutputPrint.cxx). Headphones, speaker and a USB
         * DAC are one output here, switched by what is plugged in. */
        puts_(c, "outputid: 0\noutputname: Tab5\noutputenabled: 1\n");
        return RES_OK;

    /* ---- the window of one (see the file comment) --------------------- */

    case MPD_CMD_PLAYLISTINFO: {
        take_view();
        long lo = 0, hi = INT32_MAX;
        if (a0 && !arg_range(&x, a0, &lo, &hi)) return RES_ERR;
        const long len = s_view->have_song ? 1 : 0;
        if (hi > len) hi = len;
        /* MPD 0.20's queue print: a start past the end is BadRange. */
        if (a0 && lo > hi) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        if (lo == 0 && hi == 1) put_song(c, s_view);
        return RES_OK;
    }

    case MPD_CMD_PLAYLISTID: {
        take_view();
        if (!a0) {
            if (s_view->have_song) put_song(c, s_view);
            return RES_OK;
        }
        unsigned long id;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &id)) return RES_ERR;
        if (!s_view->have_song || id != s_view->id) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        put_song(c, s_view);
        return RES_OK;
    }

    case MPD_CMD_PLAYLIST:
        take_view();
        if (s_view->have_song) {
            /* MPD's queue_print_uris(): the position and a colon, then the
             * song's own `file: <uri>` line -- "0:file: Music/a.mp3", key
             * and all. Through mpdproto_kv() so a bad byte in the path is
             * repaired as it is everywhere else. */
            const size_t n = mpdproto_kv("file", s_view->uri, s_body, MPD_BODY_MAX);
            if (n) {
                puts_(c, "0:");
                put(c, s_body, n);
            }
        }
        return RES_OK;

    case MPD_CMD_PLCHANGES:
    case MPD_CMD_PLCHANGESPOSID: {
        take_view();
        unsigned long since;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &since)) return RES_ERR;
        long lo = 0, hi = INT32_MAX;
        if (a1 && !arg_range(&x, a1, &lo, &hi)) return RES_ERR;
        /* The one entry changed at the version it arrived at, which is
         * the current version: anything older than that has missed it. */
        if (s_view->have_song && since < s_view->version && lo == 0 && hi >= 1) {
            if (cmd->kind == MPD_CMD_PLCHANGES) put_song(c, s_view);
            else putf(c, "cpos: 0\nId: %" PRIu32 "\n", s_view->id);
        }
        return RES_OK;
    }

    /* ---- the transport ------------------------------------------------ */

    case MPD_CMD_PLAY: {
        take_view();
        long pos = -1;
        if (a0 && !arg_int(&x, a0, INT32_MIN, INT32_MAX, &pos)) return RES_ERR;
        if (pos == -1) return ask(&x, UI_ACTION_PLAY, 0) ? RES_OK : RES_ERR;
        if (!s_view->have_song || pos != 0) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        return restart(&x) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_PLAYID: {
        take_view();
        long id = -1;
        if (a0 && !arg_int(&x, a0, INT32_MIN, INT32_MAX, &id)) return RES_ERR;
        if (id == -1) return ask(&x, UI_ACTION_PLAY, 0) ? RES_OK : RES_ERR;
        if (!s_view->have_song || (uint32_t)id != s_view->id) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        return restart(&x) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_PAUSE: {
        if (!a0) return ask(&x, UI_ACTION_PLAY_PAUSE, 0) ? RES_OK : RES_ERR;
        bool p;
        if (!arg_bool(&x, a0, &p)) return RES_ERR;
        return ask(&x, p ? UI_ACTION_PAUSE : UI_ACTION_PLAY, 0) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_STOP:
        /*
         * There is no stop on this device -- the switch has play, pause
         * and record -- so `stop` is a pause, and `status` says `pause`
         * afterwards rather than claiming a state the player is not in.
         * A client's stop button therefore behaves as its pause button
         * does, which is what the glass would do.
         */
        return ask(&x, UI_ACTION_PAUSE, 0) ? RES_OK : RES_ERR;

    case MPD_CMD_NEXT:
        return ask(&x, UI_ACTION_NEXT, 0) ? RES_OK : RES_ERR;

    case MPD_CMD_PREVIOUS:
        /* The device's rule, not MPD's: the start of the track first, or
         * the track before it (ui.h, UI_ACTION_PREV). A client's back
         * button is the glass's back button. */
        return ask(&x, UI_ACTION_PREV, 0) ? RES_OK : RES_ERR;

    case MPD_CMD_SEEK: {
        take_view();
        unsigned long pos;
        int64_t ms;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &pos)) return RES_ERR;
        if (!arg_time(&x, a1, false, &ms)) return RES_ERR;
        if (!s_view->have_song || pos != 0) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        return seek_ms(&x, ms) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_SEEKID: {
        take_view();
        unsigned long id;
        int64_t ms;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &id)) return RES_ERR;
        if (!arg_time(&x, a1, false, &ms)) return RES_ERR;
        if (!s_view->have_song || id != s_view->id) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        return seek_ms(&x, ms) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_SEEKCUR: {
        take_view();
        int64_t ms;
        const bool rel = a0[0] == '+' || a0[0] == '-';
        if (!arg_time(&x, a0, true, &ms)) return RES_ERR;
        if (!s_view->have_song || s_view->state == MPD_STATE_STOP) {
            /* PlaylistError::NotPlaying, which MPD maps to PLAYER_SYNC. */
            ack(c, MPD_ACK_PLAYER_SYNC, idx, cmd->verb, "Not playing");
            return RES_ERR;
        }
        if (rel) ms += s_view->elapsed_ms < 0 ? 0 : s_view->elapsed_ms;
        return seek_ms(&x, ms) ? RES_OK : RES_ERR;
    }

    /* ---- volume ------------------------------------------------------- */

    case MPD_CMD_SETVOL: {
        unsigned long v;
        if (!arg_unsigned(&x, a0, 100, &v)) return RES_ERR;
        return ask(&x, UI_ACTION_VOLUME, (int)v) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_VOLUME: {
        /* Deprecated upstream, and relative: MPD clamps the sum rather
         * than refusing it (src/command/OtherCommands.cxx). */
        long rel;
        if (!arg_int(&x, a0, -100, 100, &rel)) return RES_ERR;
        take_view();
        long v = (long)s_view->volume + rel;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        return ask(&x, UI_ACTION_VOLUME, (int)v) ? RES_OK : RES_ERR;
    }

    default:
        /*
         * A verb mpdproto.c knows the shape of and this step does not
         * answer. UNKNOWN is the code `notcommands` implies -- a client
         * that read that list and asked anyway is told what it was told
         * there -- and the wording says why rather than pretending MPD
         * has no such command.
         */
        ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb, "not supported by this player yet");
        return RES_ERR;
    }
}

/* One line from a client. False to close the connection. */
static bool run_line(conn_t *c, char *line)
{
    mpd_cmd_t cmd;

    /*
     * 5160: `noidle` is a raw line, not a verb, and it is looked for
     * before anything else -- before the list state, as MPD does
     * (ClientProcess.cxx). While idling it is the ONLY thing a client may
     * say, and anything else closes the connection; outside idle it gets
     * no answer at all, because the client that sent it has already
     * received, or is about to receive, the idle answer it was racing.
     */
    if (c->idle.waiting) {
        if (!mpdidle_is_noidle(line)) {
            ESP_LOGW(TAG, "client %d: a command during idle; closing, as MPD does", c->fd);
            return false;
        }
        char ok[8];
        const size_t n = mpdidle_noidle(&c->idle, ok, sizeof(ok));
        if (n) put(c, ok, n);
        c->last_us = esp_timer_get_time();
        return true;
    }
    if (mpdidle_is_noidle(line)) return true;

    if (c->list_dead) {
        /* After a failed sub-command, or an idle inside the list: read
         * to the end, answer nothing -- and then, for the idle, idle. */
        if (mpdproto_line(line, &c->list, &cmd) == MPD_LINE_LIST_END) {
            c->list_dead = false;
            if (c->list_idle) {
                c->list_idle = false;
                idle_enter(c, c->list_idle_mask);
            }
        }
        return true;
    }

    switch (mpdproto_line(line, &c->list, &cmd)) {
    case MPD_LINE_LIST_BEGIN:
        return true;

    case MPD_LINE_LIST_END: {
        char ok[8];
        const size_t n = mpdproto_ok(ok, sizeof(ok));
        put(c, ok, n);
        return true;
    }

    case MPD_LINE_ERR: {
        const size_t n = mpdproto_ack_cmd(&cmd, c->list.active ? c->list.index : 0,
                                          s_text, MPD_TEXT_MAX);
        if (n) put(c, s_text, n);
        if (c->list.active) c->list_dead = true;
        return true;
    }

    case MPD_LINE_CMD:
    default: {
        const bool in_list = c->list.active;
        const int idx = in_list ? c->list.index : 0;
        const result_t r = run_cmd(c, &cmd, idx);
        if (r == RES_CLOSE) return false;
        if (r == RES_ERR || r == RES_IDLE) {
            /* An idle in a list ends it the way a failure does: nothing
             * after it runs, no list_OK and no OK -- MPD breaks out of
             * the list on any result that is not OK. */
            if (in_list) c->list_dead = true;
            return true;
        }
        char ok[16];
        if (in_list) {
            if (c->list.verbose) {
                const size_t n = mpdproto_list_ok(ok, sizeof(ok));
                put(c, ok, n);
            }
            c->list.index++;
        } else {
            const size_t n = mpdproto_ok(ok, sizeof(ok));
            put(c, ok, n);
        }
        return true;
    }
    }
}

/* ---- connections --------------------------------------------------------- */

static void conn_close(conn_t *c, const char *why)
{
    if (c->fd < 0) return;
    ESP_LOGI(TAG, "client %d: %s", c->fd, why);
    close(c->fd);
    c->fd = -1;
    c->fill = 0;
    if (s_nclients > 0) s_nclients--;
}

static void conn_accept(int ls)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int fd = accept(ls, (struct sockaddr *)&from, &fl);
    if (fd < 0) return;

    char ip[16] = "?";
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

    conn_t *c = NULL;
    for (int i = 0; i < MPD_CLIENTS; i++) {
        if (s_conn[i].fd < 0) { c = &s_conn[i]; break; }
    }
    if (!c) {
        /* Closed at once rather than left in the backlog, so the client
         * fails now instead of waiting for a greeting that is not coming. */
        ESP_LOGW(TAG, "refused %s: %d clients already", ip, MPD_CLIENTS);
        close(fd);
        return;
    }

    const struct timeval tv = { .tv_sec = MPD_SEND_TIMEOUT_S, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* Every response goes out in one send, so Nagle has nothing to
     * coalesce and would only hold the last segment back for a delayed
     * ACK -- 200 ms a command on some stacks. */
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    c->fd = fd;
    c->fill = 0;
    memset(&c->list, 0, sizeof(c->list));
    c->list_dead = false;
    c->list_idle = false;
    mpdidle_init(&c->idle);         /* 5160: nothing from before it came */
    c->broken = false;
    c->last_us = esp_timer_get_time();
    s_nclients++;
    ESP_LOGI(TAG, "client %d: connected from %s (%d of %d)", fd, ip, s_nclients, MPD_CLIENTS);

    s_out_len = 0;
    puts_(c, MPDPROTO_GREETING);
    flush(c);
    if (c->broken) conn_close(c, "could not send the greeting");
}

static void conn_read(conn_t *c)
{
    const int n = recv(c->fd, c->in + c->fill, MPDPROTO_LINE_MAX - c->fill, 0);
    if (n <= 0) {
        conn_close(c, n == 0 ? "closed by the client" : "read failed");
        return;
    }
    c->fill += (size_t)n;
    c->last_us = esp_timer_get_time();

    s_out_len = 0;
    size_t start = 0;
    bool keep = true;
    while (keep && !c->broken) {
        char *nl = memchr(c->in + start, '\n', c->fill - start);
        if (!nl) break;
        *nl = '\0';
        keep = run_line(c, c->in + start);
        start = (size_t)(nl - c->in) + 1;
    }
    flush(c);

    if (!keep)     { conn_close(c, "said close"); return; }
    if (c->broken) { conn_close(c, "stopped reading"); return; }

    memmove(c->in, c->in + start, c->fill - start);
    c->fill -= start;
    /*
     * A full buffer with no newline in it is not a line this protocol can
     * contain -- mpdproto.h sizes MPDPROTO_LINE_MAX above the longest
     * legal one -- so the connection is closed rather than the line
     * split, which would run the tail of it as a command. MPD also
     * closes on this.
     */
    if (c->fill == MPDPROTO_LINE_MAX) conn_close(c, "line too long");
}

/*
 * 5160: take what ui_task published as changed since the last look, and
 * latch it into every connection; answer the ones idling on it now. Runs
 * once per wake of the loop, so a change reaches an idling client within
 * MPD_SELECT_MS of the ui_task pass that saw it -- a quarter of a second
 * at worst, which a client showing a song change does not notice. Waking
 * the select() for it instead would need a socket to wake it with, and
 * the socket budget (netbudget.h) has none to spare for that.
 */
static void deliver_events(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const uint32_t ev = s_events;
    s_events = 0;
    xSemaphoreGive(s_mu);
    if (!ev) return;

    for (int i = 0; i < MPD_CLIENTS; i++) {
        conn_t *c = &s_conn[i];
        if (c->fd < 0) continue;
        if (!mpdidle_add(&c->idle, ev)) continue;
        s_out_len = 0;
        idle_answer(c);
        flush(c);
        if (c->broken) conn_close(c, "stopped reading");
    }
}

/* ---- the task ------------------------------------------------------------ */

static void mpd_task(void *arg)
{
    (void)arg;
    const int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) {
        ESP_LOGW(TAG, "no socket for the listener (errno %d); out of lwIP sockets?", errno);
        s_retry_us = esp_timer_get_time() + MPD_RETRY_US;
        goto out;
    }
    const int one = 1;
    (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(MPD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGW(TAG, "could not listen on port %d (errno %d); trying again in 5 s",
                 MPD_PORT, errno);
        close(ls);
        s_retry_us = esp_timer_get_time() + MPD_RETRY_US;
        goto out;
    }

    s_listening = true;
    /* Not the proof the socket ceiling took -- netbudget.h's #error is,
     * at build time (5157) -- but the number is here for whoever reads
     * this log wondering why a client was refused. */
    ESP_LOGI(TAG, "listening on port %d: %d clients, lwIP has %d sockets",
             MPD_PORT, MPD_CLIENTS, CONFIG_LWIP_MAX_SOCKETS);

    while (!s_stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        int maxfd = ls;
        for (int i = 0; i < MPD_CLIENTS; i++) {
            if (s_conn[i].fd < 0) continue;
            FD_SET(s_conn[i].fd, &rd);
            if (s_conn[i].fd > maxfd) maxfd = s_conn[i].fd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = MPD_SELECT_MS * 1000 };
        const int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "select failed (errno %d); stopping", errno);
            break;
        }
        /* 5160: before the accept, so a client arriving in this wake
         * is not told of changes from before it came. */
        deliver_events();
        if (n > 0 && FD_ISSET(ls, &rd)) conn_accept(ls);

        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < MPD_CLIENTS; i++) {
            conn_t *c = &s_conn[i];
            if (c->fd < 0) continue;
            if (n > 0 && FD_ISSET(c->fd, &rd)) conn_read(c);
            /* 5160: not while idling -- MPD cancels the timeout for
             * as long as a client waits, which may be all night. */
            else if (!c->idle.waiting && now - c->last_us >= MPD_TIMEOUT_US)
                conn_close(c, "silent for 60 s");
        }
    }

    for (int i = 0; i < MPD_CLIENTS; i++) conn_close(&s_conn[i], "server stopping");
    close(ls);

out:
    s_listening = false;
    ESP_LOGI(TAG, "down; stack high-water %u of %d bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), MPD_STACK);
    /* s_task is not cleared here (5159): mpd_poll() clears it once the
     * task is seen to be deleted, because until then its stack is still
     * in use and must not be handed to a new one. */
    vTaskDelete(NULL);
}

/* ---- ui_task's side ----------------------------------------------------- */

void mpd_init(void)
{
    if (s_q) return;
    const uint32_t ps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_q = xQueueCreate(MPD_QUEUE_DEPTH, sizeof(req_t));
    s_mu = xSemaphoreCreateMutex();
    s_pub  = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_next = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_view = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_out  = heap_caps_malloc(MPD_OUT_MAX, ps);
    s_body = heap_caps_malloc(MPD_BODY_MAX, ps);
    s_text = heap_caps_malloc(MPD_TEXT_MAX, ps);
    s_stack = heap_caps_malloc(MPD_STACK, ps);  /* 5159 */
    bool ok = s_q && s_mu && s_pub && s_next && s_view && s_out && s_body && s_text &&
              s_stack;
    for (int i = 0; i < MPD_CLIENTS; i++) {
        s_conn[i].fd = -1;
        s_conn[i].in = heap_caps_malloc(MPDPROTO_LINE_MAX, ps);
        if (!s_conn[i].in) ok = false;
    }
    if (!ok) {
        /* mpd_poll() checks the same pointers and never starts. */
        ESP_LOGE(TAG, "out of PSRAM; the MPD server will not start");
        return;
    }
    /* MPD's queue version starts at 1; a client's first plchanges is
     * usually `plchanges 0`, which must return the entry. */
    s_pub->version = 1;
    s_pub->state = MPD_STATE_STOP;
    s_pub->volume = -1;
    s_pub->elapsed_ms = s_pub->duration_ms = -1;
}

static bool ready(void)
{
    if (!s_q || !s_mu || !s_pub || !s_next || !s_view || !s_out || !s_body || !s_text ||
        !s_stack) return false;
    for (int i = 0; i < MPD_CLIENTS; i++) if (!s_conn[i].in) return false;
    return true;
}

void mpd_poll(bool want)
{
    if (!ready()) return;
    char ip[20];
    const bool net = have_ip(ip, sizeof(ip));

    /*
     * 5159: a task that has exited -- stopped, or failed to bind -- is let
     * go only once FreeRTOS says it is deleted, because its stack is the
     * one the next task will be given. eTaskGetState() is safe on the
     * handle afterwards: it is &s_tcb, which is ours and never freed, and
     * a TCB on the termination list or on none reads as eDeleted. The
     * handle is only ever written here, so the old race -- a task that
     * failed to bind clearing it before xTaskCreate() had returned it --
     * cannot happen any more.
     */
    if (s_task && eTaskGetState(s_task) == eDeleted) s_task = NULL;

    if (s_task) {
        if (!want || !net) {
            if (!s_stop) ESP_LOGI(TAG, "stopping: %s", !want ? "switched off" : "no network");
            s_stop = true;          /* the task sees it within MPD_SELECT_MS */
        }
        return;
    }
    if (!want || !net || esp_timer_get_time() < s_retry_us) return;

    s_stop = false;
    s_task = xTaskCreateStatic(mpd_task, "mpd", MPD_STACK, NULL, MPD_PRIO, s_stack, &s_tcb);
    if (!s_task) {
        ESP_LOGW(TAG, "could not create the task; trying again in 5 s");
        s_retry_us = esp_timer_get_time() + MPD_RETRY_US;
    }
}

bool mpd_running(void) { return s_listening; }

int mpd_clients(void) { return s_nclients; }

bool mpd_address(char *out, size_t out_size)
{
    char ip[20];
    if (!out || !out_size || !have_ip(ip, sizeof(ip))) return false;
    snprintf(out, out_size, "%s:%d", ip, MPD_PORT);
    return true;
}

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void mpd_publish(const ui_state_t *st, const char *path, bool streaming)
{
    if (!ready() || !st) return;
    /* Nothing listening: nothing to fill, but a press taken this pass
     * still counts as serviced. */
    if (!s_task) {
        s_done_seq = s_taken_seq;
        /* 5160: the next server starts from a fresh view rather than
         * comparing against one from before it was switched off. */
        s_track.have = false;
        return;
    }

    snap_t *n = s_next;
    n->uri[0] = '\0';
    if (streaming) {
        /* Static: a station_t is 600-odd bytes and this is ui_task. */
        static station_t sta;
        if (stations_get(stations_index(), &sta)) copy_str(n->uri, sizeof(n->uri), sta.url);
    } else if (path && path[0]) {
        /* A path that is not under a mount has no URI (mpduri.h) and so
         * is not a song a client can be shown. */
        if (!mpduri_from_vfs(path, n->uri, sizeof(n->uri))) n->uri[0] = '\0';
    }
    n->have_song = n->uri[0] != '\0';
    n->state = !n->have_song ? MPD_STATE_STOP : st->playing ? MPD_STATE_PLAY : MPD_STATE_PAUSE;
    n->volume = st->volume;
    n->modes = mpdmode_from_order(browser_order());
    n->elapsed_ms = st->stats_valid ? (int32_t)st->pos_sec * 1000 : -1;
    n->duration_ms = (st->stats_valid && st->len_sec) ? (int32_t)st->len_sec * 1000 : -1;
    n->can_seek = st->can_seek;
    n->updating = medialib_busy();
    n->rg = settings_rg_enabled();                  /* 5161 */
    /*
     * A station's Title is what is on the air, not the station's name:
     * MPD puts the ICY title in Title and the station in Name, and a
     * client shows Title. mpd_song_t has no Name, so with no ICY title
     * the station's name stands in -- st->title, which is what the glass
     * shows on that row. The ICY title moving on changes the tags and so
     * moves the version below, which is how a client learns of it.
     */
    const char *title = st->title;
    if (streaming && st->stream_title && st->stream_title[0]) title = st->stream_title;
    copy_str(n->title, sizeof(n->title), title);
    copy_str(n->artist, sizeof(n->artist), st->artist);
    copy_str(n->album, sizeof(n->album), st->album);

    /*
     * The id and the version. s_pub is only ever written here, on this
     * task, so reading it without the lock is reading our own last write.
     *
     * A different song is a new id and a new version. The same song with
     * different tags -- a station's ICY title moving on -- keeps its id
     * and moves the version, which is what MPD does when a stream's tag
     * changes: the entry was modified in place, and a client re-reads it
     * through plchanges.
     */
    const snap_t *p = s_pub;
    bool tags_changed = false;      /* 5160: for `idle player` */
    if (n->have_song != p->have_song || strcmp(n->uri, p->uri) != 0) {
        n->id = n->have_song ? ++s_last_id : 0;
        n->version = p->version + 1;
    } else if (strcmp(n->title, p->title) != 0 || strcmp(n->artist, p->artist) != 0 ||
               strcmp(n->album, p->album) != 0) {
        n->id = p->id;
        n->version = p->version + 1;
        tags_changed = true;
    } else {
        n->id = p->id;
        n->version = p->version;
    }

    /*
     * 5160: what changed, for `idle`. DATABASE is MPD's "an update
     * modified the database" (UpdateService::RunDeferred's `if
     * (modified)`), so it is a run seen to finish on a volume with
     * something added, updated, revived or buried -- not every reindex,
     * which on a card nobody touched keeps all of it and changes nothing.
     */
    bool db_changed = false;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        medialib_status_t ms;
        medialib_status((storage_id_t)v, &ms);
        if (s_db_was[v] == MEDIALIB_RUNNING && ms.state == MEDIALIB_DONE &&
            ms.stats.add + ms.stats.update + ms.stats.revive + ms.stats.bury > 0)
            db_changed = true;
        s_db_was[v] = ms.state;
    }
    const mpd_idle_view_t iv = {
        .state = n->state,
        .id = n->id,
        .version = n->version,
        .volume = n->volume,
        .modes = (uint8_t)((n->modes.repeat ? 1 : 0) | (n->modes.random ? 2 : 0) |
                           (n->modes.single ? 4 : 0) | (n->modes.consume ? 8 : 0) |
                           (n->rg ? 16 : 0)),     /* 5161: MPD raises options for it */
        .updating = n->updating,
        .elapsed_ms = n->elapsed_ms,
    };
    const uint32_t ev = mpdidle_changes(&s_track, &iv, esp_timer_get_time(),
                                        tags_changed, db_changed);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_pub, n, sizeof(*s_pub));
    s_events |= ev;
    xSemaphoreGive(s_mu);
    /* After the copy: a press is serviced once its effect is readable. */
    s_done_seq = s_taken_seq;
}

bool mpd_take(ui_action_t *out)
{
    if (!s_q || !out) return false;
    req_t r;
    if (xQueueReceive(s_q, &r, 0) != pdTRUE) return false;
    *out = r.act;
    s_taken_seq = r.seq;
    return true;
}
