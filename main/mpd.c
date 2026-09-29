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
 * station, or a file played from outside an EMPTY queue, is still a
 * window of one: MPD never plays anything that is not in its queue, and
 * the one thing playing is the honest list for those. See s_ql. (5179:
 * a file outside a queue that has entries is not a window -- the queue
 * is shown, with no current song. See mpd_publish().)
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
#include "medialist.h"          /* 5177 */
#include "mediasearch.h"        /* 5180 */
#include "storage_io.h"         /* 5180: the search file's reads */
#include <strings.h>            /* 5180: strcasecmp() for tag names */
#include <dirent.h>             /* 5184: the Playlists folder */
#include <sys/stat.h>
#include <time.h>
#include "mpdidle.h"         /* 5160 */
#include "mpdmode.h"
#include "mpdqueue.h"         /* 5166 */
#include "playlist.h"         /* 5166 */
#include "mpdproto.h"
#include "mpduri.h"
#include "m3uline.h"            /* 5198 */
#include "urlclean.h"           /* 5204 */
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

/* 5180: the one partition, by MPD's name for the first, and the one
 * output, by the name `outputs` has always given it. */
#define MPD_PARTITION       "default"
#define MPD_OUTPUT_NAME     "Tab5"

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
 * Tags are known for the song that is playing; since 5185 the other
 * entries get theirs from the catalog as they are printed (lib_tags()).
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
    bool        after_stream;   /* 5202: the last command was add of a stream */
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

static bool lib_tags(const char *uri, mpd_song_t *s);   /* 5185 */

/* Entry i of the pinned list. The playing song carries its tags and its
 * length; since 5185 the others carry the catalog's tags when the
 * library is open for the command (lib_tags()), and no length. */
static void put_entry(conn_t *c, int i)
{
    const bool cur = i == s_view->song;
    mpd_song_t s = {
        .uri = list_uri(i),
        .title = cur ? s_view->title : NULL,
        .artist = cur ? s_view->artist : NULL,
        .album = cur ? s_view->album : NULL,
        .duration_ms = cur ? s_view->duration_ms : -1,
        .pos = i,
        .id = s_list->id[i],
    };
    if (!cur) (void)lib_tags(s.uri, &s);
    /* 5187: the playing song's own tags are the player's, and a field the
     * player has none for is filled from the catalog -- Cantata showed
     * unknown artist and album on the playing entry and nowhere else. */
    else if (!(s.title && s.title[0]) || !(s.artist && s.artist[0]) || !(s.album && s.album[0])) {
        mpd_song_t k = { 0 };
        if (lib_tags(s.uri, &k)) {
            if (!(s.title && s.title[0]))   s.title = k.title;
            if (!(s.artist && s.artist[0])) s.artist = k.artist;
            if (!(s.album && s.album[0]))   s.album = k.album;
        }
    }
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
    case MPD_CMD_LSINFO: case MPD_CMD_LISTALL: case MPD_CMD_LISTALLINFO:   /* 5177 */
    case MPD_CMD_LISTPARTITIONS: case MPD_CMD_PARTITION:                   /* 5180 */
    case MPD_CMD_FIND: case MPD_CMD_SEARCH: case MPD_CMD_COUNT:
    case MPD_CMD_LIST:                                                     /* 5182 */
    case MPD_CMD_LISTPLAYLISTS: case MPD_CMD_LISTPLAYLIST:                 /* 5184 */
    case MPD_CMD_LISTPLAYLISTINFO: case MPD_CMD_LOAD:
    case MPD_CMD_SAVE: case MPD_CMD_RM:
    case MPD_CMD_PLAYLISTADD: case MPD_CMD_PLAYLISTDELETE:            /* 5200 */
    case MPD_CMD_DELPARTITION: case MPD_CMD_MOVEOUTPUT:
    case MPD_CMD_LISTMOUNTS: case MPD_CMD_LISTNEIGHBORS:
    case MPD_CMD_GETVOL: case MPD_CMD_PASSWORD: case MPD_CMD_CROSSFADE:   /* 5218 */
    case MPD_CMD_ENABLEOUTPUT: case MPD_CMD_DISABLEOUTPUT: case MPD_CMD_TOGGLEOUTPUT:   /* 5219 */
    case MPD_CMD_SWAP: case MPD_CMD_SWAPID:   /* 5220 */
    case MPD_CMD_FINDADD: case MPD_CMD_SEARCHADD: case MPD_CMD_SEARCHADDPL:   /* 5221 */
    case MPD_CMD_PLAYLISTFIND: case MPD_CMD_PLAYLISTSEARCH:   /* 5222 */
    case MPD_CMD_PLAYLISTCLEAR: case MPD_CMD_PLAYLISTMOVE: case MPD_CMD_RENAME:   /* 5223 */
        return true;
    default:
        return false;
    }
}

/* The last kind in mpdproto.h's enum, for walking the table. */
#define MPD_CMD_LAST    MPD_CMD_RENAME

/*
 * 5166: the commands that read the list, over the pinned copy. Called
 * only from run_cmd(), which pins before and unpins after, so a return
 * from anywhere in here cannot leave the list pinned.
 */
static result_t queue_find(const ctx_t *x, const mpd_cmd_t *cmd);   /* 5222 */

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
        /* 5179: no current song, and a queue: MPD starts the queue from
         * the top (PlayerControl's play with no position). Since 5179 that
         * is what a client sees after `clear` and `add` while a file from
         * outside the queue plays on, and resuming that file would be the
         * wrong track. */
        if (pos == -1 && s_view->song < 0 && !s_list->window && n > 0)
            return ask(&x, UI_ACTION_PLAY_ID, (int)s_list->id[0]) ? RES_OK : RES_ERR;
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

    case MPD_CMD_PLAYLISTFIND:
    case MPD_CMD_PLAYLISTSEARCH:
        return queue_find(&x, cmd);                                 /* 5222 */

    default:
        return RES_ERR;     /* not reached: run_cmd routes only the above */
    }
}

/*
 * 5178: THE BUFFERS BELOW ARE PSRAM, allocated once in mpd_init(). 5175
 * and 5177 had them as statics, and a static is internal .bss: about
 * 9 KB of it, in a build whose main internal region is already full at
 * boot, which pushes other internal allocations into the one region the
 * radio's DMA comes from -- and esp_hosted's card init then could not
 * find 512 bytes (ARCHITECTURE.md, 5178). A task-owned buffer on this
 * task is only ever touched by it, so where it lives is only a cost.
 */
/* 5180: how much of a search file is read at a time -- the arbiter's
 * chunk, so each read is one lease. */
#define SEARCH_CHUNK    (16 * 1024)

typedef struct {
    midx_src_t *s;
    uint32_t    i, end;
    midx_rec_t  r;
    const char *path;       /* into r.key or s->scratch */
    bool        valid;
} lib_walk_t;

typedef struct {
    char            vfs[MPDURI_VFS_MAX];        /* add_uri() */
    uint32_t        edit_ids[MPDQ_MAX];         /* range_ids() */
    medialist_t     ml;                         /* lsinfo */
    medialist_ent_t ent;
    midx_rec_t      rec;
    char            dir[MPDURI_MAX + 2];
    char            uri[MPDURI_MAX + 2];
    lib_walk_t      w[MEDIALIST_VOLS];          /* listall */
    char            last[MPDURI_MAX + 2];
    /* 5180: search, find and count */
    char            sbuf[SEARCH_CHUNK + MEDIASEARCH_LINE_MAX];
    char            needle[MPDPROTO_MAX_ARGS / 2][MEDIASEARCH_LINE_MAX];
    station_t       st;                         /* 5200: [Radio Streams] */
    char            hay[MEDIASEARCH_LINE_MAX];  /* 5222: queue_find() */
} mpd_scratch_t;

static mpd_scratch_t *s_lib;        /* PSRAM, from mpd_init(); this task's */

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
 * `add` and `addid`: a URI onto the queue. 5191: the URI names its
 * volume ("sd/...", "usb/...", mpduri.h), so there is one path to try;
 * before, SD was tried and then USB. ui_task checks the file (it already
 * reads the card; this task's stack is in PSRAM and is kept off the
 * filesystem).
 */
static bool add_uri(const ctx_t *x, const char *uri, int pos, uint32_t *id_out)
{
    /* 5178: PSRAM, this task's -- see mpd_scratch_t. */
    char *const vfs = s_lib->vfs;
    if (!uri || !mpduri_ok(uri, false)) {
        ack(x->c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such directory");
        return false;
    }
    const uireq_edit_t e = { .kind = UIREQ_EDIT_ADD, .pos = pos };
    uireq_done_t how = UIREQ_DONE_NO_FILE;
    const char *rel = "";
    const int v = mpduri_split(uri, &rel);
    const int k = (v >= 0 && rel[0])
                ? snprintf(vfs, sizeof(s_lib->vfs), "%s/%s", mpduri_mount(v), rel) : -1;
    if (k > 0 && (size_t)k < sizeof(s_lib->vfs) && (size_t)k < UIREQ_PATH_MAX) {
        if (!ask_edit(x, &e, vfs, &how, id_out)) return false;
    }
    if (how != UIREQ_DONE_OK) { ack_done(x, how); return false; }
    return true;
}


/*
 * 5201: `add http(s)://...` -- a stream. The queue holds library files,
 * so a stream is not queued: it is played at once, as a station tapped
 * on the glass, and a client sees it as the window of one a station
 * always was (the list above). Handed to ui_task by uireq_open(), the
 * remote page's "open this", which tells a URL from a path.
 */
static bool is_stream_url(const char *u)
{
    return u && (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0);
}

static result_t add_stream(const ctx_t *x, const char *in)
{
    /* 5204: trackers out first (urlclean.h). */
    char *const url = s_lib->vfs;
    const size_t il = strlen(in);
    if (il >= sizeof(s_lib->vfs)) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Bad stream URL");
        return RES_ERR;
    }
    memcpy(url, in, il + 1);
    const int dropped = urlclean_strip(url);
    if (dropped) ESP_LOGI(TAG, "client %d: %d tracking parameter%s dropped", x->c->fd,
                          dropped, dropped == 1 ? "" : "s");
    const size_t n = strlen(url);
    if (strpbrk(url, "\r\n") || !uireq_open(url, n, false)) {
        ack(x->c, MPD_ACK_ARG, x->idx, x->verb, "Bad stream URL");
        return RES_ERR;
    }
    ESP_LOGI(TAG, "client %d: add stream %.160s", x->c->fd, url);
    x->c->after_stream = true;                                      /* 5202 */
    return RES_OK;
}

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
        for (long i = lo; i < hi; i++) s_lib->edit_ids[k++] = s_list->id[i];
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
        if (is_stream_url(a0)) return add_stream(&x, a0);          /* 5201 */
        return add_uri(&x, a0, -1, NULL) ? RES_OK : RES_ERR;

    case MPD_CMD_ADDID: {
        long pos = -1;
        /* MPD 0.23's "+N"/"-N", relative to the playing song, is not
         * taken: arg_int refuses the sign and says so. */
        if (a1 && !arg_int(&x, a1, 0, INT32_MAX, &pos)) return RES_ERR;
        uint32_t id = 0;
        if (is_stream_url(a0)) {                                    /* 5201 */
            ack(c, MPD_ACK_ARG, idx, cmd->verb,
                "a stream is played, not queued, on this player; use add");
            return RES_ERR;
        }
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
                const uireq_edit_t e = { .kind = UIREQ_EDIT_DELETE, .id = s_lib->edit_ids[i] };
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
            const uireq_edit_t e = { .kind = UIREQ_EDIT_MOVE, .id = s_lib->edit_ids[i],
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

    case MPD_CMD_SWAP:
    case MPD_CMD_SWAPID: {
        /*
         * 5220: two entries trade places. uireq has no swap, and one is
         * not needed: with the earlier one at p and the later at q,
         * moving the earlier to q shifts the later down to q-1, and
         * moving the later to p then leaves both where they belong. By
         * id, so the second move does not care that the first shifted
         * it. Both are found in one pin of the list, before either moves.
         */
        unsigned long u0, u1;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &u0)) return RES_ERR;
        if (!arg_unsigned(&x, a1, UINT32_MAX, &u1)) return RES_ERR;
        const bool by_id = cmd->kind == MPD_CMD_SWAPID;
        list_pin();
        const int n = s_list->n;
        const int p0 = by_id ? list_find_id((uint32_t)u0) : (u0 < (unsigned long)n ? (int)u0 : -1);
        const int p1 = by_id ? list_find_id((uint32_t)u1) : (u1 < (unsigned long)n ? (int)u1 : -1);
        const bool window = s_list->window;
        const uint32_t i0 = p0 >= 0 ? s_list->id[p0] : 0, i1 = p1 >= 0 ? s_list->id[p1] : 0;
        list_unpin();
        if (window || p0 < 0 || p1 < 0) {
            if (by_id && !window) ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such song");
            else ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad song index");
            return RES_ERR;
        }
        if (p0 == p1) return RES_OK;
        const int lo = p0 < p1 ? p0 : p1, hi = p0 < p1 ? p1 : p0;
        const uint32_t id_lo = p0 < p1 ? i0 : i1, id_hi = p0 < p1 ? i1 : i0;
        const uireq_edit_t e1 = { .kind = UIREQ_EDIT_MOVE, .id = id_lo, .pos = hi };
        const uireq_edit_t e2 = { .kind = UIREQ_EDIT_MOVE, .id = id_hi, .pos = lo };
        if (!ask_edit(&x, &e1, NULL, &how, NULL)) return RES_ERR;
        if (how != UIREQ_DONE_OK) { ack_done(&x, how); return RES_ERR; }
        if (!ask_edit(&x, &e2, NULL, &how, NULL)) return RES_ERR;
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

/* ---- the library (5177, MPD.md step 12) --------------------------------- */

/*
 * Both volumes' indexes, open for one command (medialib.h's readers, 5176)
 * and closed at its end, so a reindex waits at most one command. Slot 0 is
 * the SD, which wins a path both have (medialist.h). Static: this task's,
 * and a medialist_t is 2 KB.
 */
static medialib_rd_t s_rd[MEDIALIST_VOLS];
static bool          s_rd_open[MEDIALIST_VOLS];

static void lib_close(void)
{
    for (int v = 0; v < MEDIALIST_VOLS; v++) {
        if (s_rd_open[v]) medialib_rd_close(&s_rd[v]);
        s_rd_open[v] = false;
    }
}

/* Open what can be opened. False, ACKed, only while a reindex runs: a
 * volume with no index, or no card, is a volume with nothing in it. */
static bool lib_open(const ctx_t *x)
{
    static const storage_id_t vols[MEDIALIST_VOLS] = { STORAGE_SD, STORAGE_USB };
    if (medialib_busy()) {
        ack(x->c, MPD_ACK_SYSTEM, x->idx, x->verb,
            "the library is being indexed; try again when it is done");
        return false;
    }
    for (int v = 0; v < MEDIALIST_VOLS; v++)
        s_rd_open[v] = medialib_rd_open(vols[v], &s_rd[v]);
    return true;
}

static midx_src_t *lib_src(int v) { return s_rd_open[v] ? &s_rd[v].src : NULL; }

/*
 * One file's lines: its URI, and its tags from the catalog when they can
 * be read. No Time or duration: the catalog does not hold a length, and
 * reading one means opening the file. A client shows the song without it.
 */
static void put_lib_file(conn_t *c, int v, const midx_rec_t *r, const char *uri, bool tags)
{
    mpd_song_t s = { .uri = uri, .duration_ms = -1, .pos = -1 };
    if (tags && medialib_rd_cat(&s_rd[v], r->cat_off)) {
        const mediacat_rec_t *m = s_rd[v].rec;
        s.title = m->title[0] ? m->title : NULL;
        s.artist = m->artist[0] ? m->artist : NULL;
        s.album = m->album[0] ? m->album : NULL;
    }
    const size_t n = mpdproto_song(&s, s_body, MPD_BODY_MAX);
    if (n) put(c, s_body, n);
}

static void put_dir(conn_t *c, const char *uri)
{
    const size_t n = mpdproto_directory(uri, s_body, MPD_BODY_MAX);
    if (n) put(c, s_body, n);
}

/*
 * A URI that names a FILE, on the preferred volume that has it live.
 * lsinfo and listall of a file show that one song, as MPD's do.
 */
static void ack_no_dir(const ctx_t *x, const char *uri);

static int lib_file(const char *uri, midx_rec_t *r)
{
    /* 5191: on the volume the URI names, and no other. */
    const char *rel = "";
    const int v = mpduri_split(uri, &rel);
    midx_src_t *s = v >= 0 ? lib_src(v) : NULL;
    if (!s || !rel[0]) return -1;
    if (midx_find(s, rel, r) >= 0 && !(r->flags & MIDX_F_DEAD)) return v;
    s->err = false;                 /* a miss is not a failure */
    return -1;
}

/*
 * 5191: a folder URI taken apart for listing: the volume, and the index
 * directory below it into s_lib->dir ("" for the volume itself). -1 for
 * the root -- which lists the volumes -- and -2, ACKed, for a URI that
 * is not a folder of either.
 */
static int lib_dir(const ctx_t *x, const char *uri)
{
    if (!mpduri_ok(uri, true)) { ack_no_dir(x, uri); return -2; }
    if (!uri[0]) return -1;
    const char *rel = "";
    const int v = mpduri_split(uri, &rel);
    if (v < 0 || !mpduri_dir(rel, s_lib->dir, sizeof(s_lib->dir))) {
        ack_no_dir(x, uri);
        return -2;
    }
    return v;
}

/*
 * 5185: a queue entry's tags from the catalog, when the library is open
 * (run_cmd() opens it for the commands that print entries). Cantata
 * showed every entry but the playing one as unknown artist and album:
 * put_entry() had only the playing song's tags, and a client groups the
 * queue by them. The strings point into s_rd[], good until the next
 * medialib_rd_cat() on that volume -- put_entry() prints before that.
 */
static bool lib_tags(const char *uri, mpd_song_t *s)
{
    if (!uri || !uri[0] || (!s_rd_open[0] && !s_rd_open[1])) return false;
    midx_rec_t *const r = &s_lib->rec;
    const int v = lib_file(uri, r);
    if (v < 0 || !medialib_rd_cat(&s_rd[v], r->cat_off)) return false;
    const mediacat_rec_t *m = s_rd[v].rec;
    s->title = m->title[0] ? m->title : NULL;
    s->artist = m->artist[0] ? m->artist : NULL;
    s->album = m->album[0] ? m->album : NULL;
    return true;
}

/* 5180: the refusal says which path, in the log. The board run after 5179
 * had Cantata's `lsinfo` refused on connect with "/" handled, and MPD's
 * wording does not quote the argument -- so nothing said what it asked
 * for, or whether MPD would have refused it too. */
static void ack_no_dir(const ctx_t *x, const char *uri)
{
    ESP_LOGI(TAG, "client %d: %s \"%.120s\": not a folder or file in the library",
             x->c->fd, x->verb, uri ? uri : "");
    ack(x->c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such directory");
}

/* `lsinfo [URI]`: one folder. 5191: the root is the two volumes, as
 * folders, and below them each volume's own -- no longer merged. */
static result_t lib_lsinfo(const ctx_t *x, const char *uri)
{
    conn_t *const c = x->c;
    /* 5179: "/" is the root as well as "", as MPD takes it -- Cantata
     * asks for "/" on connect, and was told No such directory. */
    if (!uri || strcmp(uri, "/") == 0) uri = "";
    const int v = lib_dir(x, uri);
    if (v == -2) return RES_ERR;
    if (!lib_open(x)) return RES_ERR;

    if (v < 0) {
        /* The root: a folder per volume with an index open. */
        for (int k = 0; k < MEDIALIST_VOLS; k++) if (lib_src(k)) put_dir(c, mpduri_name(k));
        lib_close();
        return RES_OK;
    }

    medialist_ent_t *const ent = &s_lib->ent;
    const char *const name = mpduri_name(v);
    bool any = false;
    if (lib_src(v) && medialist_open(&s_lib->ml, v == 0 ? lib_src(0) : NULL,
                                     v == 1 ? lib_src(1) : NULL, s_lib->dir)) {
        while (!c->broken && medialist_next(&s_lib->ml, ent)) {
            any = true;
            const int k = snprintf(s_lib->uri, sizeof(s_lib->uri), "%s/%s%s", name, s_lib->dir, ent->name);
            if (k <= 0 || (size_t)k >= sizeof(s_lib->uri)) continue;
            if (ent->is_dir) put_dir(c, s_lib->uri);
            else            put_lib_file(c, v, &ent->rec, s_lib->uri, true);
        }
    }
    const bool err = lib_src(v) && s_lib->ml.err;

    /* Nothing listed under it: a file, an empty volume, or nothing. A
     * volume with nothing in it is an empty folder, which MPD answers
     * with OK; a volume not in is not a folder. */
    if (!any && !err && s_lib->dir[0]) {
        midx_rec_t *const r = &s_lib->rec;
        if (lib_file(uri, r) < 0) {
            lib_close();
            ack_no_dir(x, uri);
            return RES_ERR;
        }
        put_lib_file(c, v, r, uri, true);
    } else if (!lib_src(v)) {
        lib_close();
        ack_no_dir(x, uri);
        return RES_ERR;
    }
    lib_close();
    if (err) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "the library could not be read; reindex it");
        return RES_ERR;
    }
    return RES_OK;
}

/*
 * `listall [URI]` and `listallinfo [URI]`: everything below a folder, in
 * one pass over both indexes merged. Not medialist.h, which is one
 * folder deep and would need a 2 KB listing per level of recursion:
 * every file below a folder is one contiguous run of each index (the
 * PAST_PREFIX property), so the two runs are merged by path -- equal
 * paths collapse to the SD's -- and a `directory:` line is written for
 * each folder the first time a path enters it. midx_path_cmp() orders
 * '/' lowest, so a folder's contents come straight after its name and
 * before a sibling like "Album.flac", which is MPD's depth-first order.
 * Folders with no live file in them are not in the answer: the index
 * holds files, and a folder of only tombstones is not on the card.
 *
 * listallinfo is deprecated upstream (MPD.md) and costs a catalog read
 * per file here. Answered, not optimised.
 */
static void walk_fill(lib_walk_t *w)
{
    w->valid = false;
    while (w->s && !w->s->err && w->i < w->end) {
        if (!w->s->read(w->s->ctx, w->i++, &w->r)) { w->s->err = true; return; }
        if (w->r.flags & MIDX_F_DEAD) continue;
        w->path = midx_rec_fullpath(w->s, &w->r);
        if (!w->path) return;
        w->valid = true;
        return;
    }
}

/*
 * 5191: one volume's files below `dir` (an index directory, "" for all
 * of it), each written under the volume's name. What used to be a merge
 * of two walks is one walk per volume now that the volumes are separate
 * folders. False on a read failure.
 */
static bool listall_vol(conn_t *c, int v, const char *dir, bool info, bool *any)
{
    lib_walk_t *const w = &s_lib->w[0];
    char *const last = s_lib->last;         /* the folder of the last file, with '/' */
    const char *const name = mpduri_name(v);
    snprintf(last, sizeof(s_lib->last), "%s", dir);
    *w = (lib_walk_t){ .s = lib_src(v) };
    if (!w->s) return true;
    w->i = midx_seek(w->s, dir, MIDX_AT);
    w->end = dir[0] ? midx_seek(w->s, dir, MIDX_PAST_PREFIX) : w->s->n;
    walk_fill(w);
    while (!c->broken && w->valid) {
        const char *p = w->path;
        *any = true;
        /* Each folder between the last one written and this file's. */
        const char *slash = strrchr(p, '/');
        const size_t dl = slash ? (size_t)(slash - p) + 1 : 0;
        size_t k = 0;
        while (k < dl && last[k] && last[k] == p[k]) k++;
        while (k > 0 && p[k - 1] != '/') k--;            /* back to a boundary */
        for (size_t j = k; j < dl; j++) {
            if (p[j] != '/') continue;
            snprintf(s_lib->uri, sizeof(s_lib->uri), "%s/%.*s", name, (int)j, p);
            put_dir(c, s_lib->uri);
        }
        memcpy(last, p, dl);
        last[dl] = '\0';
        const int n = snprintf(s_lib->uri, sizeof(s_lib->uri), "%s/%s", name, p);
        if (n > 0 && (size_t)n < sizeof(s_lib->uri)) {
            if (info) put_lib_file(c, v, &w->r, s_lib->uri, true);
            else {
                const size_t m = mpdproto_kv("file", s_lib->uri, s_body, MPD_BODY_MAX);
                if (m) put(c, s_body, m);
            }
        }
        walk_fill(w);
    }
    return !w->s->err;
}

static result_t lib_listall(const ctx_t *x, const char *uri, bool info)
{
    conn_t *const c = x->c;
    /* 5179: "/" is the root as well as "", as MPD takes it -- Cantata
     * asks for "/" on connect, and was told No such directory. */
    if (!uri || strcmp(uri, "/") == 0) uri = "";
    const int v = lib_dir(x, uri);
    if (v == -2) return RES_ERR;
    if (!lib_open(x)) return RES_ERR;

    bool any = false, err = false;
    if (v < 0) {
        /* The root: each volume's folder, then everything in it. */
        for (int k = 0; k < MEDIALIST_VOLS && !c->broken && !err; k++) {
            if (!lib_src(k)) continue;
            put_dir(c, mpduri_name(k));
            if (!listall_vol(c, k, "", info, &any)) err = true;
        }
    } else if (lib_src(v)) {
        if (!listall_vol(c, v, s_lib->dir, info, &any)) err = true;
    }

    if (v >= 0 && !any && !err && (s_lib->dir[0] || !lib_src(v))) {
        midx_rec_t *const r = &s_lib->rec;
        if (lib_file(uri, r) < 0) {
            lib_close();
            ack_no_dir(x, uri);
            return RES_ERR;
        }
        if (info) put_lib_file(c, v, r, uri, true);
        else {
            const size_t n = mpdproto_kv("file", uri, s_body, MPD_BODY_MAX);
            if (n) put(c, s_body, n);
        }
    }
    lib_close();
    if (err) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "the library could not be read; reindex it");
        return RES_ERR;
    }
    return RES_OK;
}

/* ---- search, find and count (5180, MPD.md step 12) --------------------- */

/*
 * MPD's older form only: TAG VALUE pairs, `search any beatles`, `find
 * album "Abbey Road" artist Beatles`. Each pair must hold (AND).
 *
 * TWO STAGES, AS mediasearch.h DESIGNED. The search file (one folded
 * line per live track, per volume) is scanned start to finish and every
 * pair is tested as a folded substring -- which is all of `search`, and
 * for `find` a filter that cannot drop a true match. A line that passes
 * is read out of the catalog at its offset, for the real path and tags:
 * `find` then compares those exactly, and both write the song from them.
 *
 * WHAT THE LIBRARY KNOWS: title, artist, album and the path. So:
 *   - `any`, `title`, `artist`, `album` and `file` are searched;
 *   - `albumartist` is searched as `artist`, since the catalog keeps no
 *     album artist and the track artist is what a client using it
 *     usually gets on a single-artist album -- said, not pretended;
 *   - `base DIR` limits to a folder of the library, exactly;
 *   - every other tag (genre, date, track, composer, ...) is one this
 *     device never read, so nothing matches it and the answer is empty,
 *     rather than an ACK that would make a client stop asking;
 *   - `window START:END` pages the results; `sort` is ignored, and the
 *     answer is in path order, SD's volume first.
 * Filter expressions, "(artist == 'x')", are MPD 0.21's newer form and
 * are refused by name.
 *
 * 5191: a path on both volumes is answered twice, once under each
 * volume's name -- they are separate folders of the library now.
 */
typedef enum { Q_FIELD, Q_BASE, Q_NONE } qkind_t;
typedef struct {
    qkind_t             kind;
    mediasearch_field_t field;          /* Q_FIELD */
    const char         *value;          /* as sent; for find and base */
    const char         *folded;         /* into s_lib->needle */
} qpair_t;

typedef enum { Q_FIND, Q_SEARCH, Q_COUNT } qmode_t;

/* 5221: where a hit goes. Printed (find, search), added to the queue
 * (findadd, searchadd) or appended to a stored playlist (searchaddpl). */
typedef enum { QS_PRINT, QS_QUEUE, QS_PLAYLIST } qsink_t;

static result_t pl_append(const ctx_t *x, const char *name, const char *uri);

static bool q_tag(const char *t, qpair_t *p)
{
    static const struct { const char *name; mediasearch_field_t f; } tags[] = {
        { "any", MEDIASEARCH_ANY }, { "title", MEDIASEARCH_TITLE },
        { "artist", MEDIASEARCH_ARTIST }, { "albumartist", MEDIASEARCH_ARTIST },
        { "album", MEDIASEARCH_ALBUM }, { "file", MEDIASEARCH_FILE },
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (strcasecmp(t, tags[i].name) == 0) {
            p->kind = Q_FIELD;
            p->field = tags[i].f;
            return true;
        }
    }
    if (strcasecmp(t, "base") == 0) { p->kind = Q_BASE; return true; }
    p->kind = Q_NONE;           /* a tag the library does not hold */
    return false;
}

/* One catalog record against one pair, exactly (find) or as the folded
 * stage already found it (search). 5191: `uri` is the record's URI, with
 * its volume -- what `file` and `base` are compared against. */
static bool q_exact(const qpair_t *p, const mediacat_rec_t *r, const char *uri, qmode_t mode)
{
    if (p->kind == Q_BASE) {
        const size_t n = strlen(p->value);
        return n == 0 || (strncmp(uri, p->value, n) == 0 &&
                          (uri[n] == '/' || uri[n] == '\0'));
    }
    if (mode != Q_FIND) return true;
    const char *const f[4] = { r->title, r->artist, r->album, uri };
    if (p->field == MEDIASEARCH_ANY) {
        for (int i = 0; i < 4; i++) if (strcmp(f[i], p->value) == 0) return true;
        return false;
    }
    return strcmp(f[(int)p->field - 1], p->value) == 0;
}

/*
 * 5191: what the folded stage looks for. The search file holds each
 * path below its volume, so a `file` value that starts with a volume
 * ("usb/Album/...") is looked for without it; the exact stage then
 * compares the whole URI.
 */
static const char *q_fold_src(const qpair_t *p, const char *v)
{
    if (p->field != MEDIASEARCH_FILE) return v;
    const char *rel = "";
    return (mpduri_split(v, &rel) >= 0 && rel[0]) ? rel : v;
}

/* 5191: a record's URI, its volume's name and its path, into s_lib->uri.
 * False when it will not fit. */
static bool q_uri(int v, const mediacat_rec_t *r)
{
    const int k = snprintf(s_lib->uri, sizeof(s_lib->uri), "%s/%s", mpduri_name(v), r->path);
    return k > 0 && (size_t)k < sizeof(s_lib->uri);
}

/*
 * 5196: one line per library query, with what was asked, how much came
 * back and how long it took. A board run had Cantata's artist view
 * asking, and nothing on the console said what or whether it found
 * anything -- these succeed silently, unlike a refusal.
 */
static void log_query(const ctx_t *x, const mpd_cmd_t *cmd, long n, const char *what, int64_t t0)
{
    char a[160];
    size_t k = 0;
    a[0] = '\0';
    for (int i = 0; i < cmd->argc && k + 4 < sizeof(a); i++) {
        const int w = snprintf(a + k, sizeof(a) - k, "%s\"%s\"", i ? " " : "", cmd->argv[i]);
        if (w < 0) break;
        k += (size_t)w;
    }
    if (k >= sizeof(a)) k = sizeof(a) - 1;
    ESP_LOGI(TAG, "client %d: %s %s%s: %ld %s in %lld ms", x->c->fd, x->verb, a,
             k == sizeof(a) - 1 ? "..." : "", n, what,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static long s_find_hits;     /* 5227: the last lib_find()'s count */

static result_t lib_find(const ctx_t *x, const mpd_cmd_t *cmd, qmode_t mode,
                         qsink_t sink, const char *pl)
{
    conn_t *const c = x->c;
    const int64_t t0 = esp_timer_get_time();       /* 5196 */
    if (cmd->argc >= 1 && cmd->argv[0][0] == '(') {
        ack(c, MPD_ACK_UNKNOWN, x->idx, x->verb,
            "filter expressions are not supported by this player yet; use TAG VALUE pairs");
        return RES_ERR;
    }
    if (cmd->argc % 2) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "incorrect number of filter arguments");
        return RES_ERR;
    }

    static qpair_t pairs[MPDPROTO_MAX_ARGS / 2];
    int np = 0;
    long w_lo = 0, w_hi = INT32_MAX;
    bool never = false;                 /* a tag nothing can match */
    for (int i = 0; i + 1 < cmd->argc; i += 2) {
        const char *t = cmd->argv[i], *v = cmd->argv[i + 1];
        if (strcasecmp(t, "sort") == 0) continue;
        if (strcasecmp(t, "group") == 0) {
            ack(c, MPD_ACK_UNKNOWN, x->idx, x->verb, "group is not supported by this player yet");
            return RES_ERR;
        }
        if (strcasecmp(t, "window") == 0) {
            if (!arg_range(x, v, &w_lo, &w_hi)) return RES_ERR;
            continue;
        }
        qpair_t *p = &pairs[np];
        if (!q_tag(t, p)) {
            if (p->kind == Q_NONE) never = true;
            if (p->kind != Q_BASE) continue;
        }
        p->value = v;
        p->folded = NULL;
        if (p->kind == Q_FIELD) {
            if (mediasearch_fold(q_fold_src(p, v), s_lib->needle[np], sizeof(s_lib->needle[np])) < 0) never = true;
            p->folded = s_lib->needle[np];
        }
        np++;
    }

    long n_hit = 0;
    if (!never) {
        if (!lib_open(x)) return RES_ERR;
        bool err = false;
        bool refused = false;       /* 5221: an add was ACKed; stop there */
        for (int v = 0; v < MEDIALIST_VOLS && !c->broken && !err && !refused; v++) {
            if (!s_rd_open[v]) continue;
            FILE *f = medialib_rd_search(&s_rd[v]);
            if (!f) continue;
            storage_io_acquire(STORAGE_IO_BACKGROUND);
            const bool rew = fseek(f, 0, SEEK_SET) == 0;
            storage_io_release();
            if (!rew) { err = true; break; }

            char *const buf = s_lib->sbuf;
            size_t have = 0;
            bool eof = false, skipping = false;
            while (!eof && !c->broken && !err && !refused) {
                const size_t got = storage_io_fread(buf + have, SEARCH_CHUNK, f, STORAGE_IO_BACKGROUND);
                if (got == 0) eof = true;
                have += got;
                size_t at = 0;
                for (;;) {
                    char *nl = memchr(buf + at, '\n', have - at);
                    if (!nl) break;
                    const size_t len = (size_t)(nl - (buf + at));
                    const char *line = buf + at;
                    at += len + 1;
                    if (skipping) { skipping = false; continue; }   /* tail of a long line */

                    mediasearch_line_t l;
                    if (!mediasearch_parse(line, len, &l)) continue;
                    bool pass = true;
                    for (int k = 0; k < np && pass; k++)
                        if (pairs[k].kind == Q_FIELD) pass = mediasearch_match(&l, pairs[k].field, pairs[k].folded);
                    if (!pass) continue;

                    if (!medialib_rd_cat(&s_rd[v], l.cat_off)) continue;
                    const mediacat_rec_t *r = s_rd[v].rec;
                    if (!q_uri(v, r)) continue;
                    for (int k = 0; k < np && pass; k++) pass = q_exact(&pairs[k], r, s_lib->uri, mode);
                    if (!pass) continue;
                    /* 5191: a path on both volumes is two songs now,
                     * one under each; the SD's no longer hides the USB's. */
                    /*
                     * 5221: an add is one uireq edit, waited for as
                     * `add` waits (add_uri()), or one line on the end of
                     * the playlist file (pl_append()). The first refusal
                     * -- a full queue, a playlist that will not write --
                     * has been ACKed there and ends the command, with
                     * what was added before it left in place, as MPD's
                     * own add loop leaves it.
                     */
                    if (sink != QS_PRINT && n_hit >= w_lo && n_hit < w_hi) {
                        const bool ok = sink == QS_QUEUE ? add_uri(x, s_lib->uri, -1, NULL)
                                                         : pl_append(x, pl, s_lib->uri) == RES_OK;
                        if (!ok) { refused = true; break; }
                    } else if (mode != Q_COUNT && n_hit >= w_lo && n_hit < w_hi) {
                        const mpd_song_t sg = {
                            .uri = s_lib->uri, .duration_ms = -1, .pos = -1,
                            .title = r->title[0] ? r->title : NULL,
                            .artist = r->artist[0] ? r->artist : NULL,
                            .album = r->album[0] ? r->album : NULL,
                        };
                        const size_t n = mpdproto_song(&sg, s_body, MPD_BODY_MAX);
                        if (n) put(c, s_body, n);
                    }
                    n_hit++;
                }
                /* Keep the partial line; a line longer than the buffer
                 * is not one of ours, and is skipped to its newline. */
                memmove(buf, buf + at, have - at);
                have -= at;
                if (have >= MEDIASEARCH_LINE_MAX) { have = 0; skipping = true; }
            }
        }
        lib_close();
        if (refused) return RES_ERR;
        if (err) {
            ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "the library could not be read; reindex it");
            return RES_ERR;
        }
    }
    if (mode == Q_COUNT) putf(c, "songs: %ld\nplaytime: 0\n", n_hit);
    s_find_hits = n_hit;                                            /* 5227 */
    log_query(x, cmd, n_hit, n_hit == 1 ? "song" : "songs", t0);     /* 5196 */
    return RES_OK;
}

/*
 * 5222: `playlistfind` and `playlistsearch` -- find and search over the
 * queue rather than the library, answered as `playlistinfo` entries.
 * Called from run_list_cmd() with the list pinned and the library open
 * for tags, as `playlistinfo` is.
 *
 * The tags are the ones put_entry() prints: the catalog's, and the
 * player's for the playing entry where it has them. So a match is on
 * what the client is shown. find compares exactly; search folds case
 * (mediasearch_fold(), ASCII as the library search does) and looks for
 * the value anywhere in the field. The same tag names as find, and the
 * same refusals: a tag the catalog does not hold matches nothing, a
 * filter expression is refused by name. While a reindex runs the
 * catalog is closed, and only `file` and `base` can match.
 */
static result_t queue_find(const ctx_t *x, const mpd_cmd_t *cmd)
{
    conn_t *const c = x->c;
    const int64_t t0 = esp_timer_get_time();
    const bool fold = cmd->kind == MPD_CMD_PLAYLISTSEARCH;
    if (cmd->argc >= 1 && cmd->argv[0][0] == '(') {
        ack(c, MPD_ACK_UNKNOWN, x->idx, x->verb,
            "filter expressions are not supported by this player yet; use TAG VALUE pairs");
        return RES_ERR;
    }
    if (cmd->argc % 2) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "incorrect number of filter arguments");
        return RES_ERR;
    }
    static qpair_t pairs[MPDPROTO_MAX_ARGS / 2];
    int np = 0;
    for (int i = 0; i + 1 < cmd->argc; i += 2) {
        qpair_t *p = &pairs[np];
        if (!q_tag(cmd->argv[i], p) && p->kind != Q_BASE) return RES_OK;   /* matches nothing */
        p->value = cmd->argv[i + 1];
        p->folded = NULL;
        if (fold && p->kind == Q_FIELD) {
            if (mediasearch_fold(p->value, s_lib->needle[np], sizeof(s_lib->needle[np])) < 0)
                return RES_OK;
            p->folded = s_lib->needle[np];
        }
        np++;
    }

    long hits = 0;
    for (int i = 0; i < s_list->n && !c->broken; i++) {
        const char *const uri = list_uri(i);
        mpd_song_t t = { 0 };
        (void)lib_tags(uri, &t);
        if (i == s_view->song) {
            if (s_view->title[0])  t.title = s_view->title;
            if (s_view->artist[0]) t.artist = s_view->artist;
            if (s_view->album[0])  t.album = s_view->album;
        }
        const char *const f[4] = { t.title ? t.title : "", t.artist ? t.artist : "",
                                   t.album ? t.album : "", uri };
        bool pass = true;
        for (int k = 0; k < np && pass; k++) {
            const qpair_t *p = &pairs[k];
            if (p->kind == Q_BASE) {
                const size_t n = strlen(p->value);
                pass = n == 0 || (strncmp(uri, p->value, n) == 0 &&
                                  (uri[n] == '/' || uri[n] == '\0'));
                continue;
            }
            const int lo = p->field == MEDIASEARCH_ANY ? 0 : (int)p->field - 1;
            const int hi = p->field == MEDIASEARCH_ANY ? 3 : lo;
            pass = false;
            for (int j = lo; j <= hi && !pass; j++) {
                if (!fold) pass = strcmp(f[j], p->value) == 0;
                else pass = mediasearch_fold(f[j], s_lib->hay, sizeof(s_lib->hay)) >= 0 &&
                            strstr(s_lib->hay, p->folded) != NULL;
            }
        }
        if (!pass) continue;
        put_entry(c, i);
        hits++;
    }
    log_query(x, cmd, hits, hits == 1 ? "song" : "songs", t0);
    return RES_OK;
}

/*
 * 5227: a folder, added whole -- `add DIR` to the queue, `playlistadd
 * NAME DIR` to a stored playlist -- as MPD adds one: every song below it,
 * recursively. That is `findadd base DIR` (5221), so it is that: the same
 * scan, the same path order (SD's volume first), the same stop at the
 * first refusal. False, with nothing sent, when the index has no song
 * below it, so the caller can answer as it would for a file.
 */
static bool add_folder(const ctx_t *x, const char *uri, qsink_t sink, const char *pl,
                       result_t *r)
{
    static char base[] = "base";
    static mpd_cmd_t f;                 /* not on the task stack */
    memset(&f, 0, sizeof(f));
    f.kind = MPD_CMD_FINDADD;
    f.verb = (char *)x->verb;
    f.argc = 2;
    f.argv[0] = base;
    f.argv[1] = (char *)uri;
    s_find_hits = 0;
    *r = lib_find(x, &f, Q_FIND, sink, pl);
    return *r != RES_OK || s_find_hits > 0;
}

/* 5227: whether a library URI names a folder on its volume -- a volume's
 * own name ("sd", "usb") included. By stat, under a brief hold. */
static bool uri_is_dir(const char *uri)
{
    if (!uri || !uri[0] || strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0 ||
        !mpduri_ok(uri, true))
        return false;
    const char *rel = "";
    const int v = mpduri_split(uri, &rel);
    if (v < 0) return false;
    const storage_id_t id = v == MPDURI_VOL_SD ? STORAGE_SD : STORAGE_USB;
    const int k = rel[0] ? snprintf(s_lib->vfs, sizeof(s_lib->vfs), "%s/%s", mpduri_mount(v), rel)
                         : snprintf(s_lib->vfs, sizeof(s_lib->vfs), "%s", mpduri_mount(v));
    if (k <= 0 || (size_t)k >= sizeof(s_lib->vfs)) return false;
    storage_hold_brief(id);
    struct stat st;
    const bool is = storage_present(id) && stat(s_lib->vfs, &st) == 0 && S_ISDIR(st.st_mode);
    storage_release_brief(id);
    return is;
}

/* ---- list (5182, MPD.md step 12) ------------------------------------------ */

/*
 * `list TYPE [TAG VALUE ...] [group GTYPE]`: the distinct values of one
 * tag among the songs the filters match, sorted -- a client's artist and
 * album views. Cantata's album view is `list album group albumartist`.
 *
 * THE SAME TWO STAGES AS find (5180), with the catalog reads sorted. The
 * search file is scanned per volume and every line that passes the
 * folded filters has its catalog offset kept; the offsets are sorted and
 * the catalog read in that order, so an unfiltered `list album` over the
 * whole card is one forward pass through the catalog rather than a seek
 * per track -- the order search_build() already reads it in. Filters are
 * then compared exactly, as find's are, since MPD's list filters are.
 *
 * What can be listed is what the catalog holds: `artist`, `album`,
 * `title`, `file`, and `albumartist` from the artist (as 5180 searches
 * it). Any other type (genre, date, ...) is an empty OK. A song with no
 * value for the type is left out, as MPD leaves out a song without the
 * tag. One `group`, of the same five; a group on any other tag is
 * dropped, and the list comes out ungrouped rather than grouped under
 * empty headings. The old form `list album ARTIST` -- one argument after
 * the type -- is the artist filter it has always meant.
 *
 * 5191: both volumes' copies count, as separate songs.
 */
typedef struct {
    int         field;          /* 0 title, 1 artist, 2 album, 3 path; -1 none */
    const char *label;          /* MPD's key for it */
} ltype_t;

static bool l_type(const char *t, ltype_t *out)
{
    static const struct { const char *name; int field; const char *label; } types[] = {
        { "artist", 1, "Artist" }, { "albumartist", 1, "AlbumArtist" },
        { "album", 2, "Album" }, { "title", 0, "Title" }, { "file", 3, "file" },
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcasecmp(t, types[i].name) == 0) {
            out->field = types[i].field;
            out->label = types[i].label;
            return true;
        }
    }
    out->field = -1;
    out->label = NULL;
    return false;
}

static const char *l_value(const mediacat_rec_t *r, const char *uri, int field)
{
    switch (field) {
    case 0: return r->title;
    case 1: return r->artist;
    case 2: return r->album;
    case 3: return uri;                 /* 5191: with its volume */
    default: return "";
    }
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The collected (group, value) pairs: NUL-separated in an arena, an
 * offset each. Grown in PSRAM; one command's. */
typedef struct {
    char     *arena;
    size_t    len, cap;
    uint32_t *at;
    size_t    n, ncap;
} lset_t;

static const char *s_lset_arena;        /* for the comparator */

static int cmp_entry(const void *a, const void *b)
{
    const char *x = s_lset_arena + *(const uint32_t *)a;
    const char *y = s_lset_arena + *(const uint32_t *)b;
    const int g = strcmp(x, y);                             /* group */
    if (g) return g;
    return strcmp(x + strlen(x) + 1, y + strlen(y) + 1);    /* value */
}

static bool lset_add(lset_t *s, const char *group, const char *value)
{
    const size_t gl = strlen(group) + 1, vl = strlen(value) + 1;
    const uint32_t ps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (s->len + gl + vl > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16 * 1024;
        while (s->len + gl + vl > cap) cap *= 2;
        char *a = heap_caps_realloc(s->arena, cap, ps);
        if (!a) return false;
        s->arena = a;
        s->cap = cap;
    }
    if (s->n == s->ncap) {
        const size_t ncap = s->ncap ? s->ncap * 2 : 1024;
        uint32_t *at = heap_caps_realloc(s->at, ncap * sizeof(uint32_t), ps);
        if (!at) return false;
        s->at = at;
        s->ncap = ncap;
    }
    s->at[s->n++] = (uint32_t)s->len;
    memcpy(s->arena + s->len, group, gl);
    memcpy(s->arena + s->len + gl, value, vl);
    s->len += gl + vl;
    return true;
}

/*
 * One volume's search file, start to finish: the catalog offset of every
 * line that passes the folded filters, appended to *offs (grown in PSRAM).
 * False on a read failure.
 */
static bool lib_scan_offs(int v, const qpair_t *pairs, int np,
                          uint32_t **offs, size_t *n, size_t *cap)
{
    FILE *f = medialib_rd_search(&s_rd[v]);
    if (!f) return true;                    /* no search file: nothing here */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const bool rew = fseek(f, 0, SEEK_SET) == 0;
    storage_io_release();
    if (!rew) return false;

    char *const buf = s_lib->sbuf;
    size_t have = 0;
    bool eof = false, skipping = false;
    while (!eof) {
        const size_t got = storage_io_fread(buf + have, SEARCH_CHUNK, f, STORAGE_IO_BACKGROUND);
        if (got == 0) eof = true;
        have += got;
        size_t at = 0;
        for (;;) {
            char *nl = memchr(buf + at, '\n', have - at);
            if (!nl) break;
            const size_t len = (size_t)(nl - (buf + at));
            const char *line = buf + at;
            at += len + 1;
            if (skipping) { skipping = false; continue; }
            mediasearch_line_t l;
            if (!mediasearch_parse(line, len, &l)) continue;
            bool pass = true;
            for (int k = 0; k < np && pass; k++)
                if (pairs[k].kind == Q_FIELD) pass = mediasearch_match(&l, pairs[k].field, pairs[k].folded);
            if (!pass) continue;
            if (*n == *cap) {
                const size_t nc = *cap ? *cap * 2 : 1024;
                uint32_t *o = heap_caps_realloc(*offs, nc * sizeof(uint32_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!o) return false;
                *offs = o;
                *cap = nc;
            }
            (*offs)[(*n)++] = l.cat_off;
        }
        memmove(buf, buf + at, have - at);
        have -= at;
        if (have >= MEDIASEARCH_LINE_MAX) { have = 0; skipping = true; }
    }
    return true;
}

static result_t lib_list(const ctx_t *x, const mpd_cmd_t *cmd)
{
    conn_t *const c = x->c;
    const int64_t t0 = esp_timer_get_time();       /* 5196 */
    long n_out = 0;
    ltype_t type, group = { -1, NULL };
    const bool known = l_type(cmd->argv[0], &type);
    if (cmd->argc >= 2 && cmd->argv[1][0] == '(') {
        ack(c, MPD_ACK_UNKNOWN, x->idx, x->verb,
            "filter expressions are not supported by this player yet; use TAG VALUE pairs");
        return RES_ERR;
    }

    static qpair_t pairs[MPDPROTO_MAX_ARGS / 2];
    int np = 0;
    bool never = !known;
    if (cmd->argc == 2) {
        /* The old form: `list album ARTIST`. MPD took it for album only. */
        if (type.field != 2) {
            ack(c, MPD_ACK_ARG, x->idx, x->verb,
                "should be \"Album\" for 3 arguments");
            return RES_ERR;
        }
        pairs[0] = (qpair_t){ .kind = Q_FIELD, .field = MEDIASEARCH_ARTIST, .value = cmd->argv[1] };
        if (mediasearch_fold(cmd->argv[1], s_lib->needle[0], sizeof(s_lib->needle[0])) < 0) never = true;
        pairs[0].folded = s_lib->needle[0];
        np = 1;
    } else {
        if ((cmd->argc - 1) % 2) {
            ack(c, MPD_ACK_ARG, x->idx, x->verb, "incorrect number of filter arguments");
            return RES_ERR;
        }
        for (int i = 1; i + 1 < cmd->argc; i += 2) {
            const char *t = cmd->argv[i], *v = cmd->argv[i + 1];
            if (strcasecmp(t, "group") == 0) {
                ltype_t g;
                if (group.field < 0 && l_type(v, &g)) group = g;
                continue;
            }
            if (strcasecmp(t, "sort") == 0 || strcasecmp(t, "window") == 0) continue;
            qpair_t *p = &pairs[np];
            if (!q_tag(t, p)) {
                if (p->kind == Q_NONE) never = true;
                if (p->kind != Q_BASE) continue;
            }
            p->value = v;
            p->folded = NULL;
            if (p->kind == Q_FIELD) {
                if (mediasearch_fold(q_fold_src(p, v), s_lib->needle[np], sizeof(s_lib->needle[np])) < 0) never = true;
                p->folded = s_lib->needle[np];
            }
            np++;
        }
    }
    if (never) {
        /* 5196: said, since an empty answer to `list genre` looks like a
         * broken library from the client's side. */
        log_query(x, cmd, 0, "values (a tag the library does not hold)", t0);
        return RES_OK;
    }

    if (!lib_open(x)) return RES_ERR;
    lset_t set = { 0 };
    uint32_t *offs = NULL;
    size_t noffs = 0, capoffs = 0;
    bool err = false, full = false;
    for (int v = 0; v < MEDIALIST_VOLS && !err && !full; v++) {
        if (!s_rd_open[v]) continue;
        noffs = 0;
        if (!lib_scan_offs(v, pairs, np, &offs, &noffs, &capoffs)) { err = true; break; }
        qsort(offs, noffs, sizeof(uint32_t), cmp_u32);
        for (size_t i = 0; i < noffs; i++) {
            if (!medialib_rd_cat(&s_rd[v], offs[i])) continue;
            const mediacat_rec_t *r = s_rd[v].rec;
            if (!q_uri(v, r)) continue;
            bool pass = true;
            for (int k = 0; k < np && pass; k++) pass = q_exact(&pairs[k], r, s_lib->uri, Q_FIND);
            if (!pass) continue;
            /* 5191: both volumes' copies count; a value they share is
             * one line anyway, since the set is deduplicated. */
            const char *val = l_value(r, s_lib->uri, type.field);
            if (!val[0]) continue;
            if (!lset_add(&set, group.field >= 0 ? l_value(r, s_lib->uri, group.field) : "", val)) {
                full = true;
                break;
            }
        }
    }
    lib_close();
    free(offs);

    if (!err && !full) {
        s_lset_arena = set.arena;
        qsort(set.at, set.n, sizeof(uint32_t), cmp_entry);
        const char *last_g = NULL, *last_v = NULL;
        for (size_t i = 0; i < set.n && !c->broken; i++) {
            const char *g = set.arena + set.at[i];
            const char *v = g + strlen(g) + 1;
            const bool new_g = !last_g || strcmp(g, last_g) != 0;
            if (!new_g && strcmp(v, last_v) == 0) continue;         /* a repeat */
            if (group.field >= 0 && new_g) {
                const size_t n = mpdproto_kv(group.label, g, s_body, MPD_BODY_MAX);
                if (n) put(c, s_body, n);
            }
            const size_t n = mpdproto_kv(type.label, v, s_body, MPD_BODY_MAX);
            if (n) put(c, s_body, n);
            n_out++;
            last_g = g;
            last_v = v;
        }
    }
    free(set.arena);
    free(set.at);
    if (err) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "the library could not be read; reindex it");
        return RES_ERR;
    }
    if (full) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "out of memory for the list");
        return RES_ERR;
    }
    log_query(x, cmd, n_out, n_out == 1 ? "value" : "values", t0);   /* 5196 */
    return RES_OK;
}

/* ---- stored playlists (5184, MPD.md step 13) ------------------------------ */

/*
 * `<mount>/Playlists/<name>.m3u`, beside the recorder's `Recordings`
 * folder, made the first time something is saved. One URI a line, the
 * library URI -- since 5191 with its volume (`usb/Album/01 Track.mp3`);
 * a line from before, with none, is tried on the SD and then the USB
 * when it is loaded, as `add` did then. A line starting '#'
 * is a comment; a VFS path ("/usb/...") is read too, so a file written
 * by hand works.
 *
 * Both volumes' folders are read and merged by name; a name on both is
 * the SD's, and `rm` removes it from both. `save` writes to the SD when
 * there is one. Names are MPD's rule: not empty, no '/', no newline, and
 * here also no leading '.', which FAT would hide.
 *
 * FILES OPEN ONLY UNDER storage_hold_brief(), so a volume pulled during
 * a read or a write waits for the close to unmount.
 */
#define PL_DIR          "Playlists"
#define PL_EXT          ".m3u"
#define PL_NAME_MAX     (128)

static bool pl_name_ok(const char *n)
{
    const size_t len = n ? strlen(n) : 0;
    if (len == 0 || len > PL_NAME_MAX || n[0] == '.') return false;
    return !strpbrk(n, "/\\\n\r");
}

static const storage_id_t s_pl_vols[MEDIALIST_VOLS] = { STORAGE_SD, STORAGE_USB };

/* "<mount>/Playlists/<name><ext>", or false if it does not fit. 5198:
 * the extension is ".m3u" or ".m3u8"; what this player saves is .m3u. */
static bool pl_path_ext(storage_id_t v, const char *name, const char *ext, char *out, size_t cap)
{
    const int k = snprintf(out, cap, "%s/" PL_DIR "/%s%s", storage_mount_path(v), name, ext);
    return k > 0 && (size_t)k < cap;
}


static void pl_changed(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_events |= MPD_IDLE_STORED_PLAYLIST;
    xSemaphoreGive(s_mu);
}

static void put_mtime(conn_t *c, time_t t)
{
    struct tm tm;
    char ts[32];
    if (!gmtime_r(&t, &tm) || !strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm)) return;
    putf(c, "Last-Modified: %s\n", ts);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * 5191: a line saved before 5191 has no volume in it ("Album/01.mp3").
 * It is shown under the first volume that has the file, SD first -- what
 * `load` has always done with one -- or under `sd` when neither does,
 * so a client still sees a URI of today's shape.
 */
/* 5199: "<a>/<b>" (or "<b>" alone with `a` NULL) into s_lib->uri, false
 * when it does not fit. By length and memcpy, not snprintf: a playlist
 * line can be as long as the search buffer, and GCC's format-truncation
 * check (an error in this build) cannot see that it is refused first. */
static bool uri_join(char *uri, const char *a, const char *b)
{
    const size_t al = a ? strlen(a) : 0, bl = strlen(b);
    const size_t need = al + (a ? 1 : 0) + bl + 1;
    if (need > sizeof(s_lib->uri)) return false;
    if (a) { memcpy(uri, a, al); uri[al] = '/'; }
    memcpy(uri + al + (a ? 1 : 0), b, bl + 1);
    return true;
}

static void pl_legacy(const char *line, char *uri)
{
    for (int v = 0; v < MPDURI_VOLS; v++) {
        const storage_id_t id = v == MPDURI_VOL_SD ? STORAGE_SD : STORAGE_USB;
        storage_hold_brief(id);
        struct stat st;
        const int k = snprintf(s_lib->vfs, sizeof(s_lib->vfs), "%s/%s", mpduri_mount(v), line);
        const bool is = storage_present(id) && k > 0 && (size_t)k < sizeof(s_lib->vfs) &&
                        stat(s_lib->vfs, &st) == 0;
        storage_release_brief(id);
        if (is) {
            if (!uri_join(uri, mpduri_name(v), line)) uri[0] = '\0';
            return;
        }
    }
    if (!uri_join(uri, mpduri_name(MPDURI_VOL_SD), line)) uri[0] = '\0';
}

static result_t pl_list(const ctx_t *x)
{
    conn_t *const c = x->c;
    /* Up to 256 names from both folders; PSRAM, this command's. */
    enum { MAXN = 256 };
    char **names = heap_caps_calloc(MAXN, sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    time_t *mt = heap_caps_calloc(MAXN, sizeof(time_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *dir = s_lib->vfs;
    int n = 0;
    for (int v = 0; names && mt && v < MEDIALIST_VOLS; v++) {
        storage_hold_brief(s_pl_vols[v]);
        DIR *d = NULL;
        if (storage_present(s_pl_vols[v])) {
            snprintf(dir, sizeof(s_lib->vfs), "%s/" PL_DIR, storage_mount_path(s_pl_vols[v]));
            d = opendir(dir);
        }
        struct dirent *e;
        while (d && n < MAXN && (e = readdir(d)) != NULL) {
            const size_t l = strlen(e->d_name);
            /* 5198: .m3u8 as well, its name without the longer tail. */
            if (e->d_type == DT_DIR || e->d_name[0] == '.' || !m3u_is_name(e->d_name)) continue;
            const size_t xl = (e->d_name[l - 1] == '8') ? 5 : 4;
            char *nm = heap_caps_malloc(l - xl + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nm) break;
            memcpy(nm, e->d_name, l - xl);
            nm[l - xl] = '\0';
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = strcmp(names[k], nm) == 0;
            if (dup) { free(nm); continue; }       /* the SD's already */
            char *const p = s_lib->uri;
            struct stat st;
            const int pk = snprintf(p, sizeof(s_lib->uri), "%s/%s", dir, e->d_name);
            mt[n] = (pk > 0 && (size_t)pk < sizeof(s_lib->uri) && stat(p, &st) == 0) ? st.st_mtime : 0;
            names[n++] = nm;
        }
        if (d) closedir(d);
        storage_release_brief(s_pl_vols[v]);
    }
    /* Sorted by name, the modification times carried along by index. */
    char **sorted = heap_caps_malloc((size_t)(n ? n : 1) * sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sorted) {
        for (int i = 0; i < n; i++) sorted[i] = names[i];
        qsort(sorted, (size_t)n, sizeof(char *), cmp_str);
        for (int i = 0; i < n && !c->broken; i++) {
            int k = 0;
            while (k < n && names[k] != sorted[i]) k++;
            const size_t w = mpdproto_kv("playlist", sorted[i], s_body, MPD_BODY_MAX);
            if (w) put(c, s_body, w);
            if (k < n && mt[k]) put_mtime(c, mt[k]);
        }
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names); free(mt); free(sorted);
    return RES_OK;
}

/* The volume whose folder has `name` (SD first), its path in `out`; -1. */
static int pl_find(const char *name, char *out, size_t cap)
{
    for (int v = 0; v < MEDIALIST_VOLS; v++) {
        /* 5185: held, as storage.h asks; the stat was not. Whether that
         * is why `listplaylistinfo` said No such playlist straight after
         * a `save` wrote the file is not known -- the log line below
         * says what name was asked for. */
        storage_hold_brief(s_pl_vols[v]);
        struct stat st;
        bool is = false;
        /* 5198: <name>.m3u, then <name>.m3u8. */
        for (int x = 0; x < 2 && !is; x++)
            is = storage_present(s_pl_vols[v]) &&
                 pl_path_ext(s_pl_vols[v], name, x ? ".m3u8" : PL_EXT, out, cap) &&
                 stat(out, &st) == 0 && S_ISREG(st.st_mode);
        storage_release_brief(s_pl_vols[v]);
        if (is) return v;
    }
    ESP_LOGI(TAG, "no stored playlist \"%.64s\"", name);
    return -1;
}

static result_t pl_contents(const ctx_t *x, const char *name, bool info)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    const int v = pl_find(name, path, sizeof(s_lib->vfs));
    if (v < 0) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    if (info && !lib_open(x)) return RES_ERR;
    storage_hold_brief(s_pl_vols[v]);
    FILE *f = storage_present(s_pl_vols[v]) ? fopen(path, "r") : NULL;
    char *const line = s_lib->sbuf;
    char *const uri = s_lib->uri;
    /* 5198: as the player loads it (m3uline.h). */
    m3u_enc_t enc = m3u_enc_of_name(path);
    while (f && !c->broken && fgets(line, MEDIASEARCH_LINE_MAX, f)) {
        if (m3u_directive(line, &enc)) continue;
        const int n = m3u_line_clean(line, MEDIASEARCH_LINE_MAX, enc);
        if (n <= 0 || line[0] == '#') continue;
        /* A VFS path is shown as its URI; a station URL as itself. */
        if (line[0] == '/' && mpduri_from_vfs(line, uri, sizeof(s_lib->uri))) { /* uri set */ }
        else if (!strstr(line, "://") && mpduri_split(line, NULL) < 0) pl_legacy(line, uri);
        else if (!uri_join(uri, NULL, line)) uri[0] = '\0';
        if (!uri[0]) continue;              /* 5199: too long to be a URI */
        midx_rec_t *const r = &s_lib->rec;
        const int lv = info ? lib_file(uri, r) : -1;
        if (lv >= 0) {
            put_lib_file(c, lv, r, uri, true);
        } else {
            const size_t w = mpdproto_kv("file", uri, s_body, MPD_BODY_MAX);
            if (w) put(c, s_body, w);
        }
    }
    if (f) fclose(f);
    storage_release_brief(s_pl_vols[v]);
    if (info) lib_close();
    return RES_OK;
}

static result_t pl_save(const ctx_t *x, const char *name, const char *mode)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    /* MPD 0.24's mode: create (the default), append, replace. */
    const bool append = mode && strcmp(mode, "append") == 0;
    const bool replace = mode && strcmp(mode, "replace") == 0;
    if (mode && !append && !replace && strcmp(mode, "create") != 0) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Unrecognized save mode: %s", mode);
        return RES_ERR;
    }
    const int have = pl_find(name, path, sizeof(s_lib->vfs));
    /* 5198: an existing .m3u8 is written as itself, not beside itself. */
    const char *const ext = (have >= 0 && path[strlen(path) - 1] == '8') ? ".m3u8" : PL_EXT;
    if (have >= 0 && !append && !replace) {
        ack(c, MPD_ACK_EXIST, x->idx, x->verb, "Playlist already exists");
        return RES_ERR;
    }
    /* Where it already is, or the SD, or the drive. */
    int v = have;
    for (int k = 0; v < 0 && k < MEDIALIST_VOLS; k++) if (storage_present(s_pl_vols[k])) v = k;
    if (v < 0) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "no card or drive to save it on");
        return RES_ERR;
    }
    const storage_id_t vol = s_pl_vols[v];
    storage_hold_brief(vol);
    snprintf(path, sizeof(s_lib->vfs), "%s/" PL_DIR, storage_mount_path(vol));
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        storage_release_brief(vol);
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "could not make the " PL_DIR " folder");
        return RES_ERR;
    }
    FILE *f = (storage_present(vol) && pl_path_ext(vol, name, ext, path, sizeof(s_lib->vfs)))
            ? fopen(path, append ? "a" : "w") : NULL;
    int wrote = 0;
    bool ok = f != NULL;
    if (f) {
        list_pin();
        /* The queue as the client sees it -- a window of one is not a
         * queue, and a station has no library URI, so neither is saved. */
        for (int i = 0; ok && !s_list->window && i < s_list->n; i++) {
            ok = fprintf(f, "%s\n", list_uri(i)) > 0;
            wrote++;
        }
        list_unpin();
        ok = (fclose(f) == 0) && ok;
    }
    storage_release_brief(vol);
    if (!ok) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "could not write the playlist");
        return RES_ERR;
    }
    ESP_LOGI(TAG, "client %d: %s \"%s\": %d entries to %s", c->fd, x->verb, name, wrote, path);
    pl_changed();
    return RES_OK;
}

static result_t pl_rm(const ctx_t *x, const char *name)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    int gone = 0;
    for (int v = 0; v < MEDIALIST_VOLS; v++) {
        storage_hold_brief(s_pl_vols[v]);
        for (int k = 0; k < 2; k++)         /* 5198: either extension */
            if (storage_present(s_pl_vols[v]) &&
                pl_path_ext(s_pl_vols[v], name, k ? ".m3u8" : PL_EXT, path, sizeof(s_lib->vfs)) &&
                remove(path) == 0) gone++;
        storage_release_brief(s_pl_vols[v]);
    }
    if (!gone) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    pl_changed();
    return RES_OK;
}

/*
 * 5200: `playlistadd NAME URI` -- one line on the end of a stored
 * playlist, made if it is not there. A library URI or a stream URL, as
 * `save` writes them.
 */
static result_t pl_append(const ctx_t *x, const char *name, const char *uri)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    const bool url = strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0;
    if (!url && (!mpduri_ok(uri, false) || mpduri_split(uri, NULL) < 0)) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such song");
        return RES_ERR;
    }
    if (strpbrk(uri, "\r\n")) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad song URI");
        return RES_ERR;
    }
    const int have = pl_find(name, path, sizeof(s_lib->vfs));
    const char *const ext = (have >= 0 && path[strlen(path) - 1] == '8') ? ".m3u8" : PL_EXT;
    int v = have;
    for (int k = 0; v < 0 && k < MEDIALIST_VOLS; k++) if (storage_present(s_pl_vols[k])) v = k;
    if (v < 0) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "no card or drive to save it on");
        return RES_ERR;
    }
    const storage_id_t vol = s_pl_vols[v];
    storage_hold_brief(vol);
    snprintf(path, sizeof(s_lib->vfs), "%s/" PL_DIR, storage_mount_path(vol));
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        storage_release_brief(vol);
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "could not make the " PL_DIR " folder");
        return RES_ERR;
    }
    FILE *f = (storage_present(vol) && pl_path_ext(vol, name, ext, path, sizeof(s_lib->vfs)))
            ? fopen(path, "a") : NULL;
    /* 5204: a stream URL without its trackers. s_lib->uri is free here. */
    const char *line = uri;
    if (url && strlen(uri) < sizeof(s_lib->uri)) {
        memcpy(s_lib->uri, uri, strlen(uri) + 1);
        urlclean_strip(s_lib->uri);
        line = s_lib->uri;
    }
    bool ok = f && fprintf(f, "%s\n", line) > 0;
    if (f) ok = (fclose(f) == 0) && ok;
    storage_release_brief(vol);
    if (!ok) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "could not write the playlist");
        return RES_ERR;
    }
    ESP_LOGI(TAG, "playlistadd \"%.64s\": %s", name, path);
    pl_changed();
    return RES_OK;
}

/*
 * 5223: a stored playlist edited in place -- `playlistdelete NAME POS`,
 * `playlistmove NAME FROM TO` and `playlistclear NAME`.
 *
 * POSITIONS ARE WHAT listplaylist SHOWS: a line pl_contents() would list
 * (not a directive, not blank, not a comment once cleaned). Everything
 * else in the file is kept where it is, with one exception: `#EXTINF`
 * lines belong to the entry after them, and go where it goes -- deleted
 * with it, moved with it. A desktop player's export keeps its titles.
 *
 * By rewriting: the file is read line by line into "<file>.tmp" on the
 * same volume, the old file removed and the new one renamed over it
 * (FAT's rename will not replace). A power cut between the two leaves
 * the .tmp, which is not an .m3u and is not listed. The moving entry's
 * lines are found in a first pass. Buffers are s_lib's: a line in sbuf,
 * the moving block and the pending #EXTINF lines in the rest of it, the
 * cleaned copy for classifying in hay. A line longer than a search line,
 * or an entry's lines longer than half a chunk, refuse the edit rather
 * than being cut.
 */
typedef enum { PLE_DELETE, PLE_MOVE, PLE_CLEAR } pl_edit_t;

#define PLE_LINE    (MEDIASEARCH_LINE_MAX)
#define PLE_HALF    (SEARCH_CHUNK / 2)

/* Is this raw line (as read) an entry? `enc` follows #EXTENC. */
static bool ple_is_entry(const char *raw, size_t len, m3u_enc_t *enc)
{
    if (m3u_directive(raw, enc)) return false;
    memcpy(s_lib->hay, raw, len + 1);
    const int n = m3u_line_clean(s_lib->hay, sizeof(s_lib->hay), *enc);
    return n > 0 && s_lib->hay[0] != '#';
}

static bool ple_append(char *buf, size_t *have, const char *s, size_t n)
{
    if (*have + n > PLE_HALF) return false;
    memcpy(buf + *have, s, n);
    *have += n;
    return true;
}

/* One line, as fgets gave it; false at the end of the file. A line
 * longer than PLE_LINE comes back in pieces -- pass 1 refuses those. */
static bool ple_read(FILE *f, char *line, size_t *len)
{
    if (!fgets(line, PLE_LINE, f)) return false;
    *len = strlen(line);
    return true;
}

static result_t pl_edit(const ctx_t *x, const char *name, pl_edit_t op, long a, long b)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    char *const tmp = s_lib->last;
    char *const line = s_lib->sbuf;
    char *const block = s_lib->sbuf + PLE_LINE;
    char *const pend = block + PLE_HALF;
    size_t n_block = 0, n_pend = 0, len = 0;

    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    const int v = pl_find(name, path, sizeof(s_lib->vfs));
    if (v < 0) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    const storage_id_t vol = s_pl_vols[v];
    const int tk = snprintf(tmp, sizeof(s_lib->last), "%s.tmp", path);
    if (tk <= 0 || (size_t)tk >= sizeof(s_lib->last)) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "the playlist's name is too long to edit");
        return RES_ERR;
    }

    const char *why = NULL;             /* an ACK 52's text */
    bool bad_pos = false;
    storage_hold_brief(vol);
    if (op == PLE_CLEAR) {
        FILE *f = storage_present(vol) ? fopen(path, "w") : NULL;
        if (!f || fclose(f) != 0) why = "could not write the playlist";
        goto done;
    }

    /* Pass 1: count the entries, and keep the one that moves. */
    FILE *in = storage_present(vol) ? fopen(path, "r") : NULL;
    if (!in) { why = "could not read the playlist"; goto done; }
    m3u_enc_t enc = m3u_enc_of_name(path);
    long n = 0;
    while (!why && ple_read(in, line, &len)) {
        if (len == PLE_LINE - 1 && line[len - 1] != '\n') { why = "a line is too long to edit"; break; }
        const bool info = strncmp(line, "#EXTINF", 7) == 0;
        if (info) {
            if (!ple_append(pend, &n_pend, line, len)) why = "an entry is too long to edit";
            continue;
        }
        if (!ple_is_entry(line, len, &enc)) continue;
        if (op == PLE_MOVE && n == a) {
            if (!ple_append(block, &n_block, pend, n_pend) ||
                !ple_append(block, &n_block, line, len)) why = "an entry is too long to edit";
            /* A last line with no newline would run into the next. */
            if (!why && line[len - 1] != '\n' && !ple_append(block, &n_block, "\n", 1))
                why = "an entry is too long to edit";
        }
        n_pend = 0;
        n++;
    }
    fclose(in);
    if (why) goto done;
    if (a < 0 || a >= n || (op == PLE_MOVE && (b < 0 || b >= n))) { bad_pos = true; goto done; }
    if (op == PLE_MOVE && a == b) goto done;

    /* Pass 2: write the new file. */
    in = fopen(path, "r");
    FILE *out = in ? fopen(tmp, "w") : NULL;
    if (!in || !out) {
        if (in) fclose(in);
        why = "could not write the playlist";
        goto done;
    }
    enc = m3u_enc_of_name(path);
    long k = 0, j = 0;
    bool ok = true, placed = op != PLE_MOVE;
    n_pend = 0;
    while (ok && ple_read(in, line, &len)) {
        if (strncmp(line, "#EXTINF", 7) == 0) { ok = ple_append(pend, &n_pend, line, len); continue; }
        if (!ple_is_entry(line, len, &enc)) { ok = fwrite(line, 1, len, out) == len; continue; }
        const bool drop = k == a;
        if (!drop && !placed && j == b) {
            ok = fwrite(block, 1, n_block, out) == n_block;
            placed = true;
            j++;
        }
        if (!drop && ok) {
            ok = fwrite(pend, 1, n_pend, out) == n_pend && fwrite(line, 1, len, out) == len;
            if (ok && line[len - 1] != '\n') ok = fputc('\n', out) != EOF;
            j++;
        }
        n_pend = 0;
        k++;
    }
    if (ok && n_pend) ok = fwrite(pend, 1, n_pend, out) == n_pend;       /* trailing #EXTINF */
    if (ok && !placed) ok = fwrite(block, 1, n_block, out) == n_block;  /* moved to the end */
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    if (ok) ok = remove(path) == 0 && rename(tmp, path) == 0;
    else remove(tmp);
    if (!ok) why = "could not write the playlist";

done:
    storage_release_brief(vol);
    if (bad_pos) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad song index");
        return RES_ERR;
    }
    if (why) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "%s", why);
        return RES_ERR;
    }
    ESP_LOGI(TAG, "client %d: %s \"%.64s\"", c->fd, x->verb, name);
    pl_changed();
    return RES_OK;
}

/*
 * 5223: `rename FROM TO`, on the volume FROM is on and with its
 * extension. A TO on either volume is MPD's EXIST.
 */
static result_t pl_rename(const ctx_t *x, const char *from, const char *to)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    char *const dst = s_lib->last;
    if (!pl_name_ok(from) || !pl_name_ok(to)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    if (pl_find(to, dst, sizeof(s_lib->last)) >= 0) {
        ack(c, MPD_ACK_EXIST, x->idx, x->verb, "Playlist already exists");
        return RES_ERR;
    }
    const int v = pl_find(from, path, sizeof(s_lib->vfs));
    if (v < 0) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    const char *const ext = path[strlen(path) - 1] == '8' ? ".m3u8" : PL_EXT;
    const storage_id_t vol = s_pl_vols[v];
    storage_hold_brief(vol);
    const bool ok = storage_present(vol) && pl_path_ext(vol, to, ext, dst, sizeof(s_lib->last)) &&
                    rename(path, dst) == 0;
    storage_release_brief(vol);
    if (!ok) {
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb, "could not rename the playlist");
        return RES_ERR;
    }
    ESP_LOGI(TAG, "client %d: rename \"%.64s\" to \"%.64s\"", c->fd, from, to);
    pl_changed();
    return RES_OK;
}

/*
 * 5200: "[Radio Streams]" -- the stored playlist Cantata keeps its
 * streams in, asked for on every connect -- is the device's own
 * stations.m3u, so a stream added in Cantata is a station on the glass
 * and a station on the glass is a stream in Cantata. Cantata writes
 * each as "URL#Name" (the name after the last '#') and reads it back
 * the same way.
 */
#define STREAMS_PL      "[Radio Streams]"

static bool is_streams(const char *name)
{
    return name && strcmp(name, STREAMS_PL) == 0;
}

static result_t streams_list(const ctx_t *x, bool info)
{
    conn_t *const c = x->c;
    /* The card's list only: a directory search on screen is not the
     * person's stations. */
    if (stations_volume() >= STORAGE_COUNT) return RES_OK;
    station_t *const st = &s_lib->st;
    const int n = stations_count();
    for (int i = 0; i < n && !c->broken; i++) {
        if (!stations_get(i, st)) continue;
        /* 5202: Cantata's own form, "URL#StreamName=Name". */
        const int k = st->name[0]
            ? snprintf(s_lib->uri, sizeof(s_lib->uri), "%.*s#StreamName=%.*s", 400, st->url, 90, st->name)
            : snprintf(s_lib->uri, sizeof(s_lib->uri), "%.*s", 500, st->url);
        if (k <= 0 || (size_t)k >= sizeof(s_lib->uri)) continue;
        const size_t w = mpdproto_kv("file", s_lib->uri, s_body, MPD_BODY_MAX);
        if (w) put(c, s_body, w);
        if (info && st->name[0]) {
            const size_t t = mpdproto_kv("Name", st->name, s_body, MPD_BODY_MAX);
            if (t) put(c, s_body, t);
        }
    }
    return RES_OK;
}

static result_t streams_add(const ctx_t *x, const char *uri)
{
    conn_t *const c = x->c;
    /* "URL#Name": the name after the last '#', the URL before it. */
    const char *hash = strrchr(uri, '#');
    const size_t ul = hash ? (size_t)(hash - uri) : strlen(uri);
    char *const url = s_lib->vfs;
    if (ul == 0 || ul >= sizeof(s_lib->vfs)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad stream URL");
        return RES_ERR;
    }
    memcpy(url, uri, ul);
    url[ul] = '\0';
    const int dropped = urlclean_strip(url);                         /* 5204 */
    if (dropped) ESP_LOGI(TAG, "client %d: %d tracking parameter%s dropped", c->fd,
                          dropped, dropped == 1 ? "" : "s");
    const char *name = hash ? hash + 1 : "";
    /* 5202: Cantata writes "#StreamName=Name"; the name is after it. */
    if (strncmp(name, "StreamName=", 11) == 0) name += 11;
    if (!stations_append(name, url)) {
        ESP_LOGI(TAG, "client %d: station \"%.64s\" %.120s refused", c->fd, name, url);
        ack(c, MPD_ACK_SYSTEM, x->idx, x->verb,
            "the station could not be added (not http/https, too long, the list full, or no card)");
        return RES_ERR;
    }
    ESP_LOGI(TAG, "client %d: station \"%.64s\" added to %s", c->fd, name, stations_source());
    pl_changed();
    return RES_OK;
}

static result_t pl_load(const ctx_t *x, const char *name, const char *range)
{
    conn_t *const c = x->c;
    char *const path = s_lib->vfs;
    if (!pl_name_ok(name)) {
        ack(c, MPD_ACK_ARG, x->idx, x->verb, "Bad playlist name");
        return RES_ERR;
    }
    if (range) {
        ack(c, MPD_ACK_UNKNOWN, x->idx, x->verb,
            "loading part of a playlist is not supported by this player yet");
        return RES_ERR;
    }
    if (pl_find(name, path, sizeof(s_lib->vfs)) < 0) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    const uireq_edit_t e = { .kind = UIREQ_EDIT_LOAD, .pos = -1 };
    uireq_done_t how = UIREQ_DONE_OK;
    uint32_t added = 0;
    if (!ask_edit(x, &e, path, &how, &added)) return RES_ERR;
    if (how != UIREQ_DONE_OK) {
        ack(c, MPD_ACK_NO_EXIST, x->idx, x->verb, "No such playlist");
        return RES_ERR;
    }
    return RES_OK;
}

static result_t run_cmd(conn_t *c, const mpd_cmd_t *cmd, int idx)
{
    const ctx_t x = { c, idx, cmd->verb };
    const char *const a0 = cmd->argc > 0 ? cmd->argv[0] : NULL;

    /*
     * 5202: Cantata plays a stream as `add URL` then `move` of the new
     * entry to where it wants it, in one command list. A stream is never
     * queued here (5201), so there is no entry to move, and the `move`
     * failed the list with Bad song index. A move right after such an
     * add is answered OK and does nothing.
     */
    if (c->after_stream && (cmd->kind == MPD_CMD_MOVE || cmd->kind == MPD_CMD_MOVEID)) {
        ESP_LOGI(TAG, "client %d: %s after a stream: nothing queued to move", c->fd, cmd->verb);
        return RES_OK;
    }
    c->after_stream = false;

    switch (cmd->kind) {
    /* 5166: everything that reads the list, pinned for its length. */
    case MPD_CMD_CURRENTSONG:
    case MPD_CMD_PLAYLISTINFO: case MPD_CMD_PLAYLISTID: case MPD_CMD_PLAYLIST:
    case MPD_CMD_PLCHANGES: case MPD_CMD_PLCHANGESPOSID:
    case MPD_CMD_PLAY: case MPD_CMD_PLAYID:
    case MPD_CMD_SEEK: case MPD_CMD_SEEKID:
    case MPD_CMD_PLAYLISTFIND: case MPD_CMD_PLAYLISTSEARCH: {        /* 5222 */
        /* 5185: the commands that print entries read their tags from the
         * catalog. Not while a reindex runs: then they print as before,
         * without, rather than being refused. */
        const bool tags = cmd->kind == MPD_CMD_PLAYLISTINFO || cmd->kind == MPD_CMD_PLAYLISTID
                       || cmd->kind == MPD_CMD_PLCHANGES
                       || cmd->kind == MPD_CMD_CURRENTSONG      /* 5187 */
                       || cmd->kind == MPD_CMD_PLAYLISTFIND     /* 5222 */
                       || cmd->kind == MPD_CMD_PLAYLISTSEARCH;
        if (tags && !medialib_busy()) {
            static const storage_id_t vols[MEDIALIST_VOLS] = { STORAGE_SD, STORAGE_USB };
            for (int v = 0; v < MEDIALIST_VOLS; v++)
                s_rd_open[v] = medialib_rd_open(vols[v], &s_rd[v]);
        }
        list_pin();
        const result_t r = run_list_cmd(c, cmd, idx);
        list_unpin();
        if (tags) lib_close();
        return r;
    }

    /* 5184: stored playlists. 5200: "[Radio Streams]" is stations.m3u. */
    case MPD_CMD_LISTPLAYLISTS:    return pl_list(&x);
    case MPD_CMD_LISTPLAYLIST:
    case MPD_CMD_LISTPLAYLISTINFO:
        if (is_streams(a0)) return streams_list(&x, cmd->kind == MPD_CMD_LISTPLAYLISTINFO);
        return pl_contents(&x, a0, cmd->kind == MPD_CMD_LISTPLAYLISTINFO);
    case MPD_CMD_PLAYLISTADD:
        if (cmd->argc > 2) {
            ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
                "adding at a position is not supported by this player yet");
            return RES_ERR;
        }
        if (is_streams(a0)) return streams_add(&x, cmd->argv[1]);
        if (uri_is_dir(cmd->argv[1])) {                             /* 5227 */
            result_t r;
            if (!pl_name_ok(a0)) {
                ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad playlist name");
                return RES_ERR;
            }
            if (add_folder(&x, cmd->argv[1], QS_PLAYLIST, a0, &r)) return r;
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such directory");
            return RES_ERR;
        }
        return pl_append(&x, a0, cmd->argv[1]);
    case MPD_CMD_PLAYLISTDELETE:
    case MPD_CMD_PLAYLISTMOVE:                                      /* 5223 */
    case MPD_CMD_PLAYLISTCLEAR:
    case MPD_CMD_RENAME: {
        if (is_streams(a0) || (cmd->kind == MPD_CMD_RENAME && is_streams(cmd->argv[1]))) {
            ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
                "stations are edited on the player (the chooser's radio list) for now");
            return RES_ERR;
        }
        if (cmd->kind == MPD_CMD_RENAME) return pl_rename(&x, a0, cmd->argv[1]);
        if (cmd->kind == MPD_CMD_PLAYLISTCLEAR) return pl_edit(&x, a0, PLE_CLEAR, 0, 0);
        long from, to = 0;
        if (!arg_int(&x, cmd->argv[1], 0, INT32_MAX, &from)) return RES_ERR;
        if (cmd->kind == MPD_CMD_PLAYLISTDELETE) return pl_edit(&x, a0, PLE_DELETE, from, 0);
        if (!arg_int(&x, cmd->argv[2], 0, INT32_MAX, &to)) return RES_ERR;
        return pl_edit(&x, a0, PLE_MOVE, from, to);
    }
    case MPD_CMD_SAVE:             return pl_save(&x, a0, cmd->argc > 1 ? cmd->argv[1] : NULL);
    case MPD_CMD_RM:               return pl_rm(&x, a0);
    case MPD_CMD_LOAD:             return pl_load(&x, a0, cmd->argc > 1 ? cmd->argv[1] : NULL);

    /* 5180: search, find and count. */
    case MPD_CMD_FIND:   return lib_find(&x, cmd, Q_FIND, QS_PRINT, NULL);
    case MPD_CMD_SEARCH: return lib_find(&x, cmd, Q_SEARCH, QS_PRINT, NULL);
    case MPD_CMD_COUNT:  return lib_find(&x, cmd, Q_COUNT, QS_PRINT, NULL);
    /* 5221 */
    case MPD_CMD_FINDADD:   return lib_find(&x, cmd, Q_FIND, QS_QUEUE, NULL);
    case MPD_CMD_SEARCHADD: return lib_find(&x, cmd, Q_SEARCH, QS_QUEUE, NULL);
    case MPD_CMD_SEARCHADDPL: {
        /* The playlist's name first, then the filters find takes: the
         * same command without its first argument. [Radio Streams] is
         * the station list and holds no library songs. */
        if (is_streams(a0)) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "the station list holds streams, not songs");
            return RES_ERR;
        }
        if (!pl_name_ok(a0)) {
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Bad playlist name");
            return RES_ERR;
        }
        static mpd_cmd_t rest;          /* not on the task stack */
        rest = *cmd;
        rest.argc = cmd->argc - 1;
        memmove(rest.argv, cmd->argv + 1, (size_t)rest.argc * sizeof(rest.argv[0]));
        return lib_find(&x, &rest, Q_SEARCH, QS_PLAYLIST, a0);
    }
    case MPD_CMD_LIST:   return lib_list(&x, cmd);                 /* 5182 */

    /* 5177: the library. */
    case MPD_CMD_LSINFO:
        return lib_lsinfo(&x, a0);
    case MPD_CMD_LISTALL:
    case MPD_CMD_LISTALLINFO:
        return lib_listall(&x, a0, cmd->kind == MPD_CMD_LISTALLINFO);

    /* 5227: `add` of a folder adds the songs below it; a file, and
     * everything else, goes as it always has. */
    case MPD_CMD_ADD:
        if (uri_is_dir(a0)) {
            result_t r;
            if (add_folder(&x, a0, QS_QUEUE, NULL, &r)) return r;
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No indexed songs in that directory");
            return RES_ERR;
        }
        return run_queue_cmd(c, cmd, idx);

    /* 5175: the queue's edits. */
    case MPD_CMD_ADDID:
    case MPD_CMD_DELETE: case MPD_CMD_DELETEID:
    case MPD_CMD_MOVE: case MPD_CMD_MOVEID:
    case MPD_CMD_CLEAR: case MPD_CMD_SHUFFLE:
    case MPD_CMD_SWAP: case MPD_CMD_SWAPID:                          /* 5220 */
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
        /* 5201: http and https, which `add` plays as a station and
         * `playlistadd "[Radio Streams]"` keeps. Cantata's stream dialog
         * checks a URL's scheme against this list and said "invalid
         * protocol" for every one while it was empty. */
        puts_(c, "handler: http://\nhandler: https://\n");
        return RES_OK;
    case MPD_CMD_DECODERS:
        /* Empty: a decoder list is only read by clients deciding what to
         * add, and none asks. */
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
            .partition = MPD_PARTITION,                     /* 5180 */
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

    /* ---- partitions and mounts (5180) ---------------------------------- */

    /*
     * ONE PARTITION. An MPD partition is a second player in the same
     * server -- its own queue, its own state, its own outputs -- and this
     * device has one queue and one output. So there is `default`, which
     * every connection is in; switching to it is OK, to anything else is
     * MPD's "partition does not exist", and a new one cannot be made.
     * Deleting `default` is refused with MPD's own wording, since MPD
     * refuses it too.
     */
    case MPD_CMD_LISTPARTITIONS:
        putf(c, "partition: %s\n", MPD_PARTITION);
        return RES_OK;
    case MPD_CMD_PARTITION:
        if (strcmp(a0, MPD_PARTITION) == 0) return RES_OK;
        ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "partition does not exist");
        return RES_ERR;
    case MPD_CMD_NEWPARTITION:
        ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
            "this player has one queue and one output, so one partition");
        return RES_ERR;
    case MPD_CMD_DELPARTITION:
        if (strcmp(a0, MPD_PARTITION) == 0)
            ack(c, MPD_ACK_ARG, idx, cmd->verb, "Cannot delete the default partition");
        else
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "partition does not exist");
        return RES_ERR;
    case MPD_CMD_MOVEOUTPUT:
        /* Moving the one output into the partition it is already in. */
        if (strcmp(a0, MPD_OUTPUT_NAME) == 0) return RES_OK;
        ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such output");
        return RES_ERR;

    /*
     * ONE LIBRARY, TWO VOLUMES. MPD's mounts attach storage at a folder of
     * the library; since 5191 the SD card is the folder `sd` and the USB
     * drive `usb`, each listed while it is in. Nothing is mounted from a
     * client: the volumes mount themselves when they are put in, so
     * `mount` and `unmount` say that. No neighbours: nothing is browsed
     * for on the network.
     */
    case MPD_CMD_LISTMOUNTS:
        /* 5191: each volume is its own folder of the library now, so
         * each is a mount at that folder, as MPD lists one. The root
         * itself has no storage. */
        puts_(c, "mount: \n");
        for (int v = 0; v < MPDURI_VOLS; v++) {
            if (!storage_present(v == MPDURI_VOL_SD ? STORAGE_SD : STORAGE_USB)) continue;
            putf(c, "mount: %s\nstorage: %s\n", mpduri_name(v), mpduri_mount(v));
        }
        return RES_OK;
    case MPD_CMD_MOUNT:
    case MPD_CMD_UNMOUNT:
        ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
            "the SD card and USB drive mount themselves when they are put in");
        return RES_ERR;
    case MPD_CMD_LISTNEIGHBORS:
        return RES_OK;

    case MPD_CMD_OUTPUTS:
        /* One output, always on: MPD 0.20's three fields
         * (src/output/OutputPrint.cxx). Headphones, speaker and a USB
         * DAC are one output here, switched by what is plugged in. */
        putf(c, "outputid: 0\noutputname: %s\noutputenabled: 1\n", MPD_OUTPUT_NAME);
        return RES_OK;

    case MPD_CMD_ENABLEOUTPUT:
    case MPD_CMD_DISABLEOUTPUT:
    case MPD_CMD_TOGGLEOUTPUT: {
        /*
         * 5219: the one output, id 0, which is always on. Enabling it is
         * what already is. Turning it off is refused rather than done:
         * there is nothing between the decoder and the amplifier that
         * could hold audio back without it being pause, and a client
         * shown "off" while the speaker plays is worse than a refusal.
         * MPD's handle_enableoutput and siblings say "No such audio
         * output" (NO_EXIST) for an id that is not there.
         */
        unsigned long id;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &id)) return RES_ERR;
        if (id != 0) {
            ack(c, MPD_ACK_NO_EXIST, idx, cmd->verb, "No such audio output");
            return RES_ERR;
        }
        if (cmd->kind == MPD_CMD_ENABLEOUTPUT) return RES_OK;
        ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb,
            "the one output cannot be turned off; pause instead");
        return RES_ERR;
    }

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

    case MPD_CMD_GETVOL:
        /* 5218: MPD 0.23's handle_getvol, the one line `status` also
         * carries. There is always a mixer here, so never empty. */
        take_view();
        putf(c, "volume: %d\n", (int)s_view->volume);
        return RES_OK;

    /* ---- 5218: settings this player has none of ---------------------- */

    case MPD_CMD_PASSWORD:
        /* No password is configured, so every one is wrong: MPD's
         * handle_password with an empty password list says exactly this.
         * Every connection already has every permission. */
        ack(c, MPD_ACK_PASSWORD, idx, cmd->verb, "incorrect password");
        return RES_ERR;

    case MPD_CMD_CROSSFADE: {
        /* Tracks are not overlapped: one decoder feeds one output.
         * `crossfade 0` asks for what already is, and is OK; anything
         * else is refused rather than accepted and ignored, because
         * `status` carries no `xfade` line and a client's setting would
         * silently not stick. */
        unsigned long secs;
        if (!arg_unsigned(&x, a0, UINT32_MAX, &secs)) return RES_ERR;
        if (secs == 0) return RES_OK;
        ack(c, MPD_ACK_UNKNOWN, idx, cmd->verb, "crossfade is not supported by this player");
        return RES_ERR;
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
    s_lib  = heap_caps_calloc(1, sizeof(*s_lib), ps);     /* 5178 */
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
    bool ok = s_mu && s_lib && s_pub && s_next && s_view && s_out && s_body && s_text &&
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
    if (!s_mu || !s_lib || !s_pub || !s_next || !s_view || !s_out || !s_body || !s_text ||
        !s_stack) return false;
    for (int i = 0; i < 2; i++) if (!s_ql[i].id || !s_ql[i].ver || !s_ql[i].off) return false;
    for (int i = 0; i < MPD_CLIENTS; i++) if (!s_conn[i].in) return false;
    return true;
}

void mpd_media_changed(void)
{
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_events |= MPD_IDLE_DATABASE | MPD_IDLE_MOUNT | MPD_IDLE_STORED_PLAYLIST;
    xSemaphoreGive(s_mu);
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
    /*
     * 5179: A WINDOW ONLY WHEN THERE IS NO QUEUE TO SHOW, or for a
     * station. The board run after 5177: Cantata's "replace and play" is
     * `clear`, `add`, `play 0`. The clear took the playing track out of
     * the queue, so this showed that track as a window of one -- and
     * `play 0` restarted it instead of playing what had just been added,
     * because position 0 was the window. A client that has just filled
     * the queue is talking about the queue. So a file that is not the
     * queue's shows the queue with no current song, which is the state
     * MPD is in after a clear; the track plays on underneath, and
     * `status` says stop until something from the queue plays. A station
     * keeps its window: it is never in the queue, and its title in
     * `currentsong` is what a client shows while one plays.
     */
    const bool window = !queued && shown && n->uri[0] && (streaming || mpdq_count() == 0);
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
