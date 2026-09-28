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
 * ui_task published (mpd_publish()) and asks for presses through uireq.h,
 * which ui_task drains and remote.c feeds too (MPD.md step 5) -- MPD.md's
 * rule: the server task "never calls playlist_* or
 * request_track() itself".
 *
 * A PRESS WAITS FOR ui_task, where the remote's does not. MPD is
 * synchronous in a way the remote page is not: a client that is told OK
 * for `pause` and then asks `status` expects to see `state: pause`, and
 * a client that sees `play` instead toggles again. So ask() queues the
 * press as uireq_press() numbers it and waits until mpd_publish() has run
 * in the pass that took it -- the pass whose state reflects it -- before the
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
 * THE QUEUE IS THE QUEUE (5166). A client is shown mpdqueue.c -- the
 * folder, since 5165 -- through a copy ui_task hands over whenever it
 * changes, because this task cannot read a queue with no lock and two
 * writers. Positions, ids, `song` and `nextsong` are real, so a client's
 * queue lists the folder and its Next button knows there is a next. A
 * station, or a file played from outside the queue's folder, is still a
 * window of one: MPD never plays anything that is not in its queue, and
 * the one thing playing is the honest list for those. See s_ql.
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
#include "mpdqueue.h"         /* 5166 */
#include "playlist.h"         /* 5166 */
#include "mpdproto.h"
#include "mpduri.h"
#include "uireq.h"            /* MPD.md step 5 */
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
/* How long a press waits for room in the queue before it is refused.
 * The room is uireq.h's UIREQ_PER_SOURCE, which was MPD_QUEUE_DEPTH. */
#define MPD_ASK_QUEUE_MS    (100)

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
    bool        have_song;      /* song >= 0 */
    uint32_t    id;             /* the song id; 0 with no song */
    uint32_t    version;        /* the playlist version; starts at 1 */
    /* 5166: where the song is in the list a client is shown, how long
     * that list is, and what follows it (mpdmode_next_pos()). */
    int         song;           /* -1: none */
    int         length;
    int         next_song;      /* -1: none */
    uint32_t    next_id;
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
 * 5166: THE LIST A CLIENT IS SHOWN.
 *
 * Since 5165 the queue is real -- a folder fills mpdqueue.c -- but this
 * task cannot read it: the queue has no lock, and ui_task and media_task
 * both change it. So ui_task copies it, in mpd_publish(), whenever it
 * changes, the way it copies the status: every entry's URI, its id (the
 * queue's own, which survives a move and is never reused) and the
 * version at which its position last changed, in THIS file's version
 * space so plchanges answers in the same counter `status` reports.
 *
 * TWO COPIES, so neither side waits on the other. ui_task builds into
 * the one clients are not reading and publishes it by flipping s_ql_pub
 * under s_mu; this task pins the published one for the length of a
 * command (list_pin()), and ui_task skips a rebuild whose target is
 * pinned and tries again next pass. A command that sends a thousand
 * entries to a slow client therefore never holds s_mu across a send and
 * never stalls the pass that draws the screen.
 *
 * WHAT IS A WINDOW OF ONE, STILL: a station, and a single file played
 * from somewhere other than the queue's folder. Neither is in the queue,
 * and MPD never plays anything that is not, so for those the list is the
 * one thing playing -- 5158's arrangement, kept for exactly the case it
 * is honest about.
 *
 * WINDOW IDS FIT A SIGNED 32-BIT INT (5167). MPD's ids are small numbers
 * and clients store them as int: Cantata parses them with toInt() into a
 * qint32, which returns 0 for anything above INT32_MAX. 5166 marked
 * window ids with the TOP bit -- 0x80000001 -- and Cantata read every one
 * as 0. They start at 2^30 now: still apart from the queue's, which count
 * up from 1 and would need a billion appends in one boot to meet them,
 * and still a positive int.
 *
 * Tags are known for the song that is playing and for nothing else: the
 * catalog has them (mediacat.h) but looking every entry up is step 12's
 * query layer. Until then the other entries are `file:` with a position
 * and an id, and a client shows the file name.
 */
#define MPD_WINDOW_ID       (0x40000000u)
_Static_assert(MPD_WINDOW_ID + 0x3FFFFFFFu <= 0x7FFFFFFFu, "window ids must stay positive ints");

typedef struct {
    int       n;
    bool      window;       /* a window of one, not the queue */
    uint32_t  src;          /* mpdq_version() it copies; unused for a window */
    uint32_t *id;           /* MPDQ_MAX each, PSRAM */
    uint32_t *ver;
    uint32_t *off;          /* into arena */
    char     *arena;        /* the URIs, NUL-separated, PSRAM, grown */
    size_t    len, cap;
} qlist_t;

static qlist_t           s_ql[2];
static int               s_ql_pub;          /* under s_mu */
static int               s_ql_pin = -1;     /* under s_mu; this task's */
static const qlist_t    *s_list;            /* this task's, while pinned */

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

/*
 * 5162: every ACK is logged as it is sent -- the line a client is told,
 * which is the one worth reading when a new client complains. Refusals
 * are rare and each is a thing to build or a bug, so INFO; the log is
 * capped at 160 bytes because an ACK can quote a whole argument off the
 * wire, and mpdproto_ack() has already replaced any control byte in it.
 */
static void put_ack(conn_t *c, size_t n)
{
    if (!n) return;
    ESP_LOGI(TAG, "client %d: %.*s", c->fd, (int)(n - 1 < 160 ? n - 1 : 160), s_text);
    put(c, s_text, n);
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
    put_ack(c, mpdproto_ack(code, idx, verb, s_body, s_text, MPD_TEXT_MAX));
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

/* 5166: the status and the list it describes, taken together so a song
 * position always refers to the list it is read against. */
static void list_pin(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_view, s_pub, sizeof(*s_view));
    s_ql_pin = s_ql_pub;
    s_list = &s_ql[s_ql_pin];
    xSemaphoreGive(s_mu);
}

static void list_unpin(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_ql_pin = -1;
    xSemaphoreGive(s_mu);
    s_list = NULL;
}

static const char *list_uri(int i) { return s_list->arena + s_list->off[i]; }

static int list_find_id(uint32_t id)
{
    for (int i = 0; i < s_list->n; i++) if (s_list->id[i] == id) return i;
    return -1;
}

/* Entry i of the pinned list. The playing song carries its tags and its
 * length; the others are known by path alone (see above). */
static void put_entry(conn_t *c, int i)
{
    const bool cur = i == s_view->song;
    const mpd_song_t s = {
        .uri = list_uri(i),
        .title = cur ? s_view->title : NULL,
        .artist = cur ? s_view->artist : NULL,
        .album = cur ? s_view->album : NULL,
        .duration_ms = cur ? s_view->duration_ms : -1,
        .pos = i,
        .id = s_list->id[i],
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
    const ui_action_t a = { .kind = kind, .value = value };
    /*
     * 5162: a press from a client says which client and which command,
     * because the player's own "button:" line that follows it reads the
     * same as a tap on the glass. Only presses: `status` and friends are
     * polled, some clients every second, and would drown the console.
     */
    if (kind == UI_ACTION_VOLUME || kind == UI_ACTION_SEEK)
        ESP_LOGI(TAG, "client %d: %s -> %s %d%%", x->c->fd, x->verb, ui_action_name(kind), value);
    else if (kind == UI_ACTION_REPLAYGAIN)
        ESP_LOGI(TAG, "client %d: %s -> replaygain %s", x->c->fd, x->verb, value ? "on" : "off");
    else
        ESP_LOGI(TAG, "client %d: %s -> %s", x->c->fd, x->verb, ui_action_name(kind));
    /* uireq_press() does not block, so the wait for room that
     * xQueueSend() did is here: polled at the same 10 ms as the wait
     * below. */
    uint32_t seq;
    const TickType_t q0 = xTaskGetTickCount();
    while ((seq = uireq_press(UIREQ_MPD, &a)) == 0) {
        if (xTaskGetTickCount() - q0 >= pdMS_TO_TICKS(MPD_ASK_QUEUE_MS)) {
            ack(x->c, MPD_ACK_SYSTEM, x->idx, x->verb, "player busy; try again");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const TickType_t t0 = xTaskGetTickCount();
    while (!uireq_serviced(seq)) {
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
    case MPD_CMD_REPEAT: case MPD_CMD_RANDOM:           /* 5168 */
    case MPD_CMD_SINGLE: case MPD_CMD_CONSUME:
    case MPD_CMD_REPLAY_GAIN_MODE: case MPD_CMD_REPLAY_GAIN_STATUS:  /* 5161 */
    case MPD_CMD_CHANNELS:                                           /* 5163 */
    case MPD_CMD_ADD: case MPD_CMD_ADDID:                            /* 5175 */
    case MPD_CMD_DELETE: case MPD_CMD_DELETEID:
    case MPD_CMD_MOVE: case MPD_CMD_MOVEID:
    case MPD_CMD_CLEAR: case MPD_CMD_SHUFFLE:
        return true;
    default:
        return false;
    }
}

/* The last kind in mpdproto.h's enum, for walking the table. */
#define MPD_CMD_LAST    MPD_CMD_RM

/*
 * 5166: the commands that read the list, over the pinned copy. Called
 * only from run_cmd(), which pins before and unpins after, so a return
 * from anywhere in here cannot leave the list pinned.
 */
static result_t run_list_cmd(conn_t *c, const mpd_cmd_t *cmd, int idx)
{
    const ctx_t x = { c, idx, cmd->verb };
    const char *const a0 = cmd->argc > 0 ? cmd->argv[0] : NULL;
    const char *const a1 = cmd->argc > 1 ? cmd->argv[1] : NULL;
    const int n = s_list->n;

    switch (cmd->kind) {
    case MPD_CMD_CURRENTSONG:
        if (s_view->song >= 0 && s_view->song < n) put_entry(c, s_view->song);
        return RES_OK;

    case MPD_CMD_PLAYLISTINFO: {
        long lo = 0, hi = INT32_MAX;
        if (a0 && !arg_range(&x, a0, &lo, &hi)) return RES_ERR;
        if (hi > n) hi = n;
        /* MPD 0.20's queue print: a start past the end is BadRange. */
        if (a0 && lo > hi) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        for (long i = lo; i < hi && !c->broken; i++) put_entry(c, (int)i);
        return RES_OK;
    }

    case MPD_CMD_PLAYLISTID: {
        if (!a0) {
            for (int i = 0; i < n && !c->broken; i++) put_entry(c, i);
            return RES_OK;
        }
        unsigned long id;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &id)) return RES_ERR;
        const int pos = list_find_id((uint32_t)id);
        if (pos < 0) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        put_entry(c, pos);
        return RES_OK;
    }

    case MPD_CMD_PLAYLIST:
        /* MPD's queue_print_uris(): the position and a colon, then the
         * song's own `file: <uri>` line -- "0:file: Music/a.mp3", key and
         * all. Through mpdproto_kv() so a bad byte in the path is
         * repaired as it is everywhere else. */
        for (int i = 0; i < n && !c->broken; i++) {
            const size_t k = mpdproto_kv("file", list_uri(i), s_body, MPD_BODY_MAX);
            if (!k) continue;
            putf(c, "%d:", i);
            put(c, s_body, k);
        }
        return RES_OK;

    case MPD_CMD_PLCHANGES:
    case MPD_CMD_PLCHANGESPOSID: {
        unsigned long since;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &since)) return RES_ERR;
        long lo = 0, hi = INT32_MAX;
        if (a1 && !arg_range(&x, a1, &lo, &hi)) return RES_ERR;
        if (hi > n) hi = n;
        /* Every entry whose position last changed after `since` -- which
         * is what the per-entry version is kept for (mpdqueue.h's
         * subtle part, carried into this file's counter). */
        for (long i = lo; i < hi && !c->broken; i++) {
            if (s_list->ver[i] <= since) continue;
            if (cmd->kind == MPD_CMD_PLCHANGES) put_entry(c, (int)i);
            else putf(c, "cpos: %ld\nId: %" PRIu32 "\n", i, s_list->id[i]);
        }
        return RES_OK;
    }

    case MPD_CMD_PLAY: {
        long pos = -1;
        if (a0 && !arg_int(&x, a0, INT32_MIN, INT32_MAX, &pos)) return RES_ERR;
        if (pos == -1) return ask(&x, UI_ACTION_PLAY, 0) ? RES_OK : RES_ERR;
        if (pos < 0 || pos >= n) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        /* The song already playing starts again; another entry of the
         * queue is played by its id (ui.h, UI_ACTION_PLAY_ID). */
        if (pos == s_view->song) return restart(&x) ? RES_OK : RES_ERR;
        if (s_list->window) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        return ask(&x, UI_ACTION_PLAY_ID, (int)s_list->id[pos]) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_PLAYID: {
        long id = -1;
        if (a0 && !arg_int(&x, a0, INT32_MIN, INT32_MAX, &id)) return RES_ERR;
        if (id == -1) return ask(&x, UI_ACTION_PLAY, 0) ? RES_OK : RES_ERR;
        const int pos = list_find_id((uint32_t)id);
        if (pos < 0) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        if (pos == s_view->song) return restart(&x) ? RES_OK : RES_ERR;
        return ask(&x, UI_ACTION_PLAY_ID, (int)s_list->id[pos]) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_SEEK:
    case MPD_CMD_SEEKID: {
        unsigned long which;
        int64_t ms;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &which)) return RES_ERR;
        if (!arg_time(&x, a1, false, &ms)) return RES_ERR;
        const int pos = cmd->kind == MPD_CMD_SEEK
                      ? ((long)which < n ? (int)which : -1)
                      : list_find_id((uint32_t)which);
        if (pos < 0) {
            if (cmd->kind == MPD_CMD_SEEK) ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            else ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            return RES_ERR;
        }
        /* MPD would start that song at that point. This player seeks
         * only what it is playing -- UI_ACTION_SEEK is a percentage of
         * the current track -- and starting another song somewhere into
         * it is a new action, not this patch. Said, not approximated. */
        if (pos != s_view->song) {
            ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
                "seeking a song that is not playing is not supported by this player yet");
            return RES_ERR;
        }
        return seek_ms(&x, ms) ? RES_OK : RES_ERR;
    }

    default:
        return RES_ERR;     /* not reached: run_cmd routes only the above */
    }
}

/* ---- the queue's edits (5175, MPD.md step 11) --------------------------- */

/*
 * An edit, the way ask() is a press: through uireq (5173's path, which the
 * remote page's verbs already use), waited for, and answered for. Two
 * waits, not one. First for the OUTCOME, which ui_task records at the top
 * of its pass -- including while a page on the glass keeps it from
 * publishing -- because an edit can be refused (a full queue, a stale id)
 * and the client must be told which. Then, bounded and without complaint,
 * for the publish, so a `playlistinfo` straight after an OK sees the
 * change when the player can show it, as a press's `status` does.
 *
 * If the outcome itself does not come in MPD_ASK_WAIT_MS -- the edit is
 * queued behind a press, and presses are not taken while a page is open
 * -- the client is told the player is busy. The edit is still queued and
 * will land when the page closes; a client that retries adds twice. That
 * is written here rather than hidden, and the page is the only way to
 * cause it.
 */
static bool ask_edit(const ctx_t *x, const uireq_edit_t *e, const char *path,
                     uireq_done_t *how_out, uint32_t *id_out)
{
    ESP_LOGI(TAG, "client %d: %s -> queue edit", x->c->fd, x->verb);
    uint32_t seq;
    const TickType_t q0 = xTaskGetTickCount();
    while ((seq = uireq_edit(UIREQ_MPD, e, path, path ? strlen(path) : 0)) == 0) {
        if (xTaskGetTickCount() - q0 >= pdMS_TO_TICKS(MPD_ASK_QUEUE_MS)) {
            ack(x->c, MPD_ACK_SYSTEM, x->idx, x->verb, "player busy; try again");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uireq_done_t how = UIREQ_DONE_OK;
    uint32_t id = 0;
    const TickType_t t0 = xTaskGetTickCount();
    while (!uireq_edit_outcome(seq, &how, &id)) {
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(MPD_ASK_WAIT_MS)) {
            ESP_LOGI(TAG, "%s: the player has not taken it yet (a page is open?)", x->verb);
            ack(x->c, MPD_ACK_SYSTEM, x->idx, x->verb,
                "player busy; the change is queued and may still happen");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const TickType_t p0 = xTaskGetTickCount();
    while (!uireq_serviced(seq) && xTaskGetTickCount() - p0 < pdMS_TO_TICKS(MPD_ASK_WAIT_MS))
        vTaskDelay(pdMS_TO_TICKS(10));
    if (how_out) *how_out = how;
    if (id_out) *id_out = id;
    return true;
}

/* The ACK for a refused edit: MPD's code and wording for each reason. */
static void ack_done(const ctx_t *x, uireq_done_t how)
{
    switch (how) {
    case UIREQ_DONE_OK:
        break;
    case UIREQ_DONE_NO_FILE:
        /* MPD's DatabaseError NOT_FOUND on an add, which is this. */
        ack(x->c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such directory");
        break;
    case UIREQ_DONE_NOT_TRACK:
        ack(x->c, MPD_ACK_UNKNOWN, x->idx, x->verb,
            "adding a folder or a file this player cannot play is not supported");
        break;
    case UIREQ_DONE_FULL:
        ack(x->c, MPD_ACK_PLAYLIST_MAX, x->idx, x->verb, "Playlist is too large");
        break;
    case UIREQ_DONE_BAD_POS:
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Bad song index");
        break;
    case UIREQ_DONE_GONE:
        ack(x->c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such song");
        break;
    }
}

/*
 * `add` and `addid`: a URI onto the queue. The URI is a path below a
 * volume, and which volume is not in it (mpduri.h), so SD is tried first
 * and USB when SD has no such file -- the shadowing mpduri.h describes,
 * decided by the file rather than by the index, since the index can lag
 * the card. ui_task checks the file (it already reads the card; this
 * task's stack is in PSRAM and is kept off the filesystem).
 */
static bool add_uri(const ctx_t *x, const char *uri, int pos, uint32_t *id_out)
{
    /* Static: this task's, and 512 bytes is not for its stack. */
    static char vfs[MPDURI_VFS_MAX];
    if (!uri || !mpduri_ok(uri, false)) {
        ack(x->c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such directory");
        return false;
    }
    const uireq_edit_t e = { .kind = UIREQ_EDIT_ADD, .pos = pos };
    uireq_done_t how = UIREQ_DONE_NO_FILE;
    for (int v = 0; v < MPDURI_VOLS && how == UIREQ_DONE_NO_FILE; v++) {
        const char *m = mpduri_mount(v);
        const int k = m ? snprintf(vfs, sizeof(vfs), "%s/%s", m, uri) : -1;
        if (k <= 0 || (size_t)k >= sizeof(vfs) || (size_t)k >= UIREQ_PATH_MAX) continue;
        if (!ask_edit(x, &e, vfs, &how, id_out)) return false;
    }
    if (how != UIREQ_DONE_OK) { ack_done(x, how); return false; }
    return true;
}

/* Static: up to MPDQ_MAX ids, this task's. */
static uint32_t s_edit_ids[MPDQ_MAX];

/*
 * Positions to ids, over the pinned list: a range [lo, hi) the client
 * named against the list it was shown. False, ACKed, when the range is
 * not inside it -- and on a window of one (a station, a file from outside
 * the queue), whose positions are not the queue's.
 */
static int range_ids(const ctx_t *x, long lo, long hi, bool open_end, int *n_out)
{
    list_pin();
    const int n = s_list->n;
    int k = -1;
    if (open_end && hi > n) hi = n;
    if (n_out) *n_out = n;
    if (!s_list->window && lo >= 0 && lo < hi && hi <= n) {
        k = 0;
        for (long i = lo; i < hi; i++) s_edit_ids[k++] = s_list->id[i];
    }
    list_unpin();
    if (k < 0) ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Bad song index");
    return k;
}

static result_t run_queue_cmd(conn_t *c, const mpd_cmd_t *cmd, int idx)
{
    const ctx_t x = { c, idx, cmd->verb };
    const char *const a0 = cmd->argc > 0 ? cmd->argv[0] : NULL;
    const char *const a1 = cmd->argc > 1 ? cmd->argv[1] : NULL;
    uireq_done_t how = UIREQ_DONE_OK;

    switch (cmd->kind) {
    case MPD_CMD_ADD:
        return add_uri(&x, a0, -1, NULL) ? RES_OK : RES_ERR;

    case MPD_CMD_ADDID: {
        long pos = -1;
        /* MPD 0.23's "+N"/"-N", relative to the playing song, is not
         * taken: arg_int refuses the sign and says so. */
        if (a1 && !arg_int(&x, a1, 0, INT32_MAX, &pos)) return RES_ERR;
        uint32_t id = 0;
        if (!add_uri(&x, a0, (int)pos, &id)) return RES_ERR;
        putf(c, "Id: %" PRIu32 "\n", id);
        return RES_OK;
    }

    case MPD_CMD_DELETE:
    case MPD_CMD_MOVE: {
        long lo, hi;
        if (!arg_range(&x, a0, &lo, &hi)) return RES_ERR;
        long to = 0;
        if (cmd->kind == MPD_CMD_MOVE && !arg_int(&x, a1, 0, INT32_MAX, &to)) return RES_ERR;
        int n = 0;
        const int k = range_ids(&x, lo, hi, hi == INT32_MAX, &n);
        if (k < 0) return RES_ERR;
        /* Checked whole before anything moves, so a block that will not
         * fit is refused rather than half moved. */
        if (cmd->kind == MPD_CMD_MOVE && to + k > n) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        if (cmd->kind == MPD_CMD_DELETE) {
            for (int i = 0; i < k; i++) {
                const uireq_edit_t e = { .kind = UIREQ_EDIT_DELETE, .id = s_edit_ids[i] };
                if (!ask_edit(&x, &e, NULL, &how, NULL)) return RES_ERR;
                if (how != UIREQ_DONE_OK) { ack_done(&x, how); return RES_ERR; }
            }
            return RES_OK;
        }
        /*
         * A block moves so its first entry lands at `to`, in order.
         * Upwards, each goes to to+i in turn. Downwards, each goes to
         * to+k-1: every one taken from above that position shifts the
         * ones already placed up by one, and they end at to..to+k-1.
         */
        const bool down = to > lo;
        for (int i = 0; i < k; i++) {
            const uireq_edit_t e = { .kind = UIREQ_EDIT_MOVE, .id = s_edit_ids[i],
                                     .pos = (int)(down ? to + k - 1 : to + i) };
            if (!ask_edit(&x, &e, NULL, &how, NULL)) return RES_ERR;
            if (how != UIREQ_DONE_OK) { ack_done(&x, how); return RES_ERR; }
        }
        return RES_OK;
    }

    case MPD_CMD_DELETEID:
    case MPD_CMD_MOVEID: {
        unsigned long id;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &id)) return RES_ERR;
        long to = 0;
        if (cmd->kind == MPD_CMD_MOVEID && !arg_int(&x, a1, 0, INT32_MAX, &to)) return RES_ERR;
        const uireq_edit_t e = { .kind = cmd->kind == MPD_CMD_DELETEID ? UIREQ_EDIT_DELETE
                                                                      : UIREQ_EDIT_MOVE,
                                 .id = (uint32_t)id, .pos = (int)to };
        if (!ask_edit(&x, &e, NULL, &how, NULL)) return RES_ERR;
        if (how != UIREQ_DONE_OK) { ack_done(&x, how); return RES_ERR; }
        return RES_OK;
    }

    case MPD_CMD_CLEAR:
    case MPD_CMD_SHUFFLE: {
        if (a0) {
            ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
                "shuffling part of the queue is not supported by this player yet");
            return RES_ERR;
        }
        const uireq_edit_t e = { .kind = cmd->kind == MPD_CMD_CLEAR ? UIREQ_EDIT_CLEAR
                                                                   : UIREQ_EDIT_SHUFFLE };
        return ask_edit(&x, &e, NULL, &how, NULL) ? RES_OK : RES_ERR;
    }

    default:
        return RES_ERR;     /* not reached: run_cmd routes only the above */
    }
}

static result_t run_cmd(conn_t *c, const mpd_cmd_t *cmd, int idx)
{
    const ctx_t x = { c, idx, cmd->verb };
    const char *const a0 = cmd->argc > 0 ? cmd->argv[0] : NULL;

    switch (cmd->kind) {
    /* 5166: everything that reads the list, pinned for its length. */
    case MPD_CMD_CURRENTSONG:
    case MPD_CMD_PLAYLISTINFO: case MPD_CMD_PLAYLISTID: case MPD_CMD_PLAYLIST:
    case MPD_CMD_PLCHANGES: case MPD_CMD_PLCHANGESPOSID:
    case MPD_CMD_PLAY: case MPD_CMD_PLAYID:
    case MPD_CMD_SEEK: case MPD_CMD_SEEKID: {
        list_pin();
        const result_t r = run_list_cmd(c, cmd, idx);
        list_unpin();
        return r;
    }

    /* 5175: the queue's edits. */
    case MPD_CMD_ADD: case MPD_CMD_ADDID:
    case MPD_CMD_DELETE: case MPD_CMD_DELETEID:
    case MPD_CMD_MOVE: case MPD_CMD_MOVEID:
    case MPD_CMD_CLEAR: case MPD_CMD_SHUFFLE:
        return run_queue_cmd(c, cmd, idx);

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
            .playlist_length = v->length,                   /* 5166 */
            .song = v->song,
            .songid = v->id,
            .next_song = v->next_song,
            .next_songid = v->next_id,
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


    case MPD_CMD_CHANNELS:
        /*
         * 5163: MPD's handle_channels lists every channel any client has
         * subscribed to. `subscribe` is not a verb here, so no client can
         * have, and the empty list is exact rather than a stand-in -- it
         * is what MPD says with nobody subscribed. Cantata reads it to
         * look for its dynamic-playlist helper and, finding none, turns
         * that feature off, which is the right answer too.
         */
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

    case MPD_CMD_REPEAT:
    case MPD_CMD_RANDOM:
    case MPD_CMD_SINGLE:
    case MPD_CMD_CONSUME: {
        /*
         * 5168: MPD's handle_repeat and its three siblings, through
         * mpdmode.h's table. The other three flags are as the client last
         * saw them; the one asked for changes; the four together go to
         * the nearest of the device's four orders.
         *
         * WHAT DOES NOT FIT SPRINGS BACK (5156's rule): `repeat 1` alone
         * is repeat-all, which this device has not got, and `consume` is
         * not a thing it can do at all. The command still says OK, as
         * MPD's always does, and `status` then reports what the device
         * will actually do.
         *
         * AND A SPRING-BACK RAISES `options`, which MPD never needs to:
         * there a flag always changes as asked, so a client's toggle
         * always matches. Here a client that sent `repeat 1` has drawn
         * the toggle on, and without an event it would stay on while the
         * device does not repeat. The event makes it re-read.
         */
        bool on;
        if (!arg_bool(&x, a0, &on)) return RES_ERR;
        take_view();
        mpd_modes_t m = s_view->modes;
        bool *const flag = cmd->kind == MPD_CMD_REPEAT ? &m.repeat
                         : cmd->kind == MPD_CMD_RANDOM ? &m.random
                         : cmd->kind == MPD_CMD_SINGLE ? &m.single : &m.consume;
        *flag = on;
        const play_order_t now = mpdmode_to_order(&s_view->modes);
        const play_order_t want = mpdmode_to_order(&m);
        const mpd_modes_t got = mpdmode_from_order(want);
        const bool honoured = memcmp(&got, &m, sizeof(m)) == 0;
        if (!honoured) {
            ESP_LOGI(TAG, "client %d: %s %d has no exact play order here; "
                     "status says what the player will do", c->fd, cmd->verb, on);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            s_events |= MPD_IDLE_OPTIONS;
            xSemaphoreGive(s_mu);
        }
        if (want == now) return RES_OK;
        return ask(&x, UI_ACTION_ORDER, (int)want) ? RES_OK : RES_ERR;
    }

    case MPD_CMD_OUTPUTS:
        /* One output, always on: MPD 0.20's three fields
         * (src/output/OutputPrint.cxx). Headphones, speaker and a USB
         * DAC are one output here, switched by what is plugged in. */
        puts_(c, "outputid: 0\noutputname: Tab5\noutputenabled: 1\n");
        return RES_OK;

    /* ---- the transport ------------------------------------------------ */

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
        put_ack(c, mpdproto_ack_cmd(&cmd, c->list.active ? c->list.index : 0,
                                    s_text, MPD_TEXT_MAX));
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
    if (s_mu) return;
    const uint32_t ps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_mu = xSemaphoreCreateMutex();
    s_pub  = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_next = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_view = heap_caps_calloc(1, sizeof(snap_t), ps);
    s_out  = heap_caps_malloc(MPD_OUT_MAX, ps);
    s_body = heap_caps_malloc(MPD_BODY_MAX, ps);
    s_text = heap_caps_malloc(MPD_TEXT_MAX, ps);
    s_stack = heap_caps_malloc(MPD_STACK, ps);  /* 5159 */
    for (int i = 0; i < 2; i++) {               /* 5166: the list, twice */
        s_ql[i].id  = heap_caps_calloc(MPDQ_MAX, sizeof(uint32_t), ps);
        s_ql[i].ver = heap_caps_calloc(MPDQ_MAX, sizeof(uint32_t), ps);
        s_ql[i].off = heap_caps_calloc(MPDQ_MAX, sizeof(uint32_t), ps);
    }
    bool ok = s_mu && s_pub && s_next && s_view && s_out && s_body && s_text &&
              s_stack;
    for (int i = 0; i < 2; i++) ok = ok && s_ql[i].id && s_ql[i].ver && s_ql[i].off;
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
    s_pub->song = s_pub->next_song = -1;        /* 5166 */
}

static bool ready(void)
{
    if (!s_mu || !s_pub || !s_next || !s_view || !s_out || !s_body || !s_text ||
        !s_stack) return false;
    for (int i = 0; i < 2; i++) if (!s_ql[i].id || !s_ql[i].ver || !s_ql[i].off) return false;
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

/* ---- the list, built on ui_task (5166) ------------------------------------ */

static bool list_add(qlist_t *q, const char *uri, uint32_t id)
{
    const size_t need = strlen(uri) + 1;
    if (q->len + need > q->cap) {
        size_t cap = q->cap ? q->cap : 4096;
        while (cap < q->len + need) cap *= 2;
        char *a = heap_caps_realloc(q->arena, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a) return false;
        q->arena = a;
        q->cap = cap;
    }
    memcpy(q->arena + q->len, uri, need);
    q->off[q->n] = (uint32_t)q->len;
    q->id[q->n] = id;
    q->len += need;
    q->n++;
    return true;
}

/*
 * Fill `w` with the queue, or with the one thing playing, and give each
 * entry its version: the old one if the same id sits at the same
 * position as before, `vnew` if not -- the range rule (mpdqueue.c's
 * bump_range()) reached by comparison, since this copy is rebuilt rather
 * than edited. `cur_changed` stamps the playing entry too, for tags that
 * changed in place. True when anything differs from `old`.
 */
static bool list_build(qlist_t *w, const qlist_t *old, bool window, const char *uri,
                       uint32_t window_id, int song, bool cur_changed, uint32_t vnew)
{
    w->n = 0;
    w->len = 0;
    w->window = window;
    if (window) {
        w->src = 0;
        (void)list_add(w, uri, window_id);
    } else {
        /* Static: ui_task's, and 507 bytes is not a thing for its stack. */
        static char u[MPDURI_MAX + 1];
        w->src = mpdq_version();
        const int count = mpdq_count();
        for (int i = 0; i < count; i++) {
            const char *vfs = mpdq_path(i);
            /* Every queue path is under a mount, so this does not fail;
             * if it ever did, the entry keeps its place under its raw
             * path rather than shifting every position after it. */
            if (!vfs || !mpduri_from_vfs(vfs, u, sizeof(u)))
                snprintf(u, sizeof(u), "%s", vfs ? vfs : "");
            if (!list_add(w, u, mpdq_id(i))) {
                ESP_LOGW(TAG, "out of PSRAM copying the queue at entry %d of %d", i, count);
                break;
            }
        }
    }
    bool changed = w->n != old->n || w->window != old->window;
    for (int i = 0; i < w->n; i++) {
        const bool same = i < old->n && old->id[i] == w->id[i] && !(cur_changed && i == song);
        w->ver[i] = same ? old->ver[i] : vnew;
        if (!same) changed = true;
    }
    return changed;
}

void mpd_publish(const ui_state_t *st, const char *path, bool streaming)
{
    if (!ready() || !st) return;
    /* Nothing listening: nothing to fill, but a press taken this pass
     * still counts as serviced. */
    if (!s_task) {
        uireq_published();
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
     * The id, the version and the list (5166). s_pub and the published
     * list are only ever written here, on this task, so reading them
     * without the lock is reading our own last write.
     *
     * WHAT IS PLAYING, AND IS IT THE QUEUE'S? The shown path is the
     * queue's when it is the entry the playlist's cursor is on; then the
     * list is the queue and the song is that position. A station, or a
     * file played from outside the queue's folder, is a window of one
     * (see s_ql). Nothing shown at all is the queue with no song.
     */
    const snap_t *p = s_pub;
    const qlist_t *L = &s_ql[s_ql_pub];
    /* 5172: the queue is read directly below, and main_task can load or
     * clear it meanwhile; held until the copy clients read is built. */
    playlist_lock();
    const int cur = playlist_current();
    const char *qp = cur >= 0 ? mpdq_path(cur) : NULL;
    const bool shown = streaming || (path && path[0]);
    /*
     * 5167: THE CURSOR, NOT THE SCREEN, SAYS WHICH ENTRY IS PLAYING. A
     * folder chosen on the glass loads the queue and moves the cursor at
     * once; the screen commits to the new track only when its first
     * frame is ready (track_commit()). In that gap 5166 saw a shown path
     * that was not the cursor's entry and showed the OLD track as a
     * window of one -- with an id Cantata could not read -- which is what
     * the board reported as "still playing" the last folder. The queue's
     * cursor is where the player is going; the screen catches up.
     *
     * BUT ONLY WHILE THE SCREEN IS CATCHING UP. The screen and the cursor
     * can also disagree for good: a double-tap of Prev walks back through
     * history into another folder's track and leaves the cursor where it
     * was. The two differ in which side moved. A lag is the CURSOR moving
     * (a load, a Next) while the screen still shows what it showed then;
     * a mismatch is the SCREEN moving to something the queue does not
     * hold. So the path on screen is remembered each time the queue or
     * the cursor changes, and the cursor is trusted while the screen still
     * shows that same path -- and once the screen shows something else
     * that is not the cursor's entry, it is a window of one.
     *
     * The entry's tags are the screen's only once the screen is showing
     * that entry (`inq`); until then it is its path alone, and the tags
     * arriving a moment later are a change in place, as a stream's are.
     */
    static uint32_t s_seen_qv;
    static int      s_seen_cur = -2;
    static char     s_seen_shown[512];      /* ui_task's */
    if (mpdq_version() != s_seen_qv || cur != s_seen_cur) {
        s_seen_qv = mpdq_version();
        s_seen_cur = cur;
        snprintf(s_seen_shown, sizeof(s_seen_shown), "%s", (path && !streaming) ? path : "");
    }
    const bool inq = !streaming && shown && qp && strcmp(qp, path) == 0;
    const bool lagging = !streaming && qp && !inq &&
                         strcmp((path && shown) ? path : "", s_seen_shown) == 0;
    const bool queued = !streaming && cur >= 0 && qp && (inq || lagging || !shown);
    const bool window = !queued && shown && n->uri[0];
    if (queued && !inq) {
        n->title[0] = n->artist[0] = n->album[0] = '\0';
        n->duration_ms = -1;
    }

    uint32_t sid = 0;
    if (queued) {
        sid = mpdq_id(cur);
    } else if (window) {
        /* The same thing still playing keeps its id; anything else is new. */
        sid = (L->window && L->n == 1 && strcmp(L->arena, n->uri) == 0)
            ? L->id[0] : (MPD_WINDOW_ID | ++s_last_id);
    }
    /* The same song with different tags -- a station's ICY title moving
     * on -- keeps its id and moves the version, which is what MPD does
     * when a stream's tag changes: modified in place, re-read through
     * plchanges. */
    const bool tags_changed = sid && sid == p->id &&
        (strcmp(n->title, p->title) != 0 || strcmp(n->artist, p->artist) != 0 ||
         strcmp(n->album, p->album) != 0);

    const bool stale = window != L->window ||
        (window ? (L->n != 1 || L->id[0] != sid) : L->src != mpdq_version());

    /* Rebuild into the copy clients are not reading -- unless this task
     * still has it pinned from a flip ago, in which case the list stays
     * as published for another pass and so does everything that points
     * into it. */
    int w = -1;
    if (stale || tags_changed) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        const int target = 1 - s_ql_pub;
        const bool pinned = target == s_ql_pin;
        xSemaphoreGive(s_mu);
        if (!pinned) w = target;
    }

    const qlist_t *ql = L;
    if (w >= 0) {
        const uint32_t vnew = p->version + 1;
        const int song_guess = queued ? cur : 0;
        const bool changed = list_build(&s_ql[w], L, window, n->uri, sid, song_guess,
                                        tags_changed, vnew);
        n->version = changed ? vnew : p->version;
        ql = &s_ql[w];
    } else {
        n->version = p->version;
    }
    playlist_unlock();      /* 5172: nothing below reads the queue */

    if (w < 0 && (stale || tags_changed)) {
        /* Deferred: what points into the list stays as it was, so a
         * position never refers to a list a client cannot see. */
        n->song = p->song;
        n->id = p->id;
        n->length = p->length;
        n->next_song = p->next_song;
        n->next_id = p->next_id;
    } else {
        n->length = ql->n;
        n->song = queued ? (cur < ql->n ? cur : -1) : (window && ql->n == 1 ? 0 : -1);
        n->id = n->song >= 0 ? ql->id[n->song] : 0;
        n->next_song = queued ? mpdmode_next_pos(browser_order(), n->song, ql->n) : -1;
        n->next_id = n->next_song >= 0 ? ql->id[n->next_song] : 0;
    }
    n->have_song = n->song >= 0;
    n->state = !n->have_song ? MPD_STATE_STOP : st->playing ? MPD_STATE_PLAY : MPD_STATE_PAUSE;

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
    if (w >= 0) s_ql_pub = w;       /* 5166: the list with the snapshot */
    s_events |= ev;
    xSemaphoreGive(s_mu);
    /* After the copy: a press is serviced once its effect is readable. */
    uireq_published();
}
