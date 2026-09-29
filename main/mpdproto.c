/*
 * mpdproto.c -- see mpdproto.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mpdproto.h"

#include <stdio.h>
#include <string.h>

#include "remoteproto.h"     /* remoteproto_utf8_len(), for the value repair */

/* ---- the character classes ----------------------------------------- */

/*
 * MPD's, byte for byte, and the two of them are exact complements, which
 * is why an accented path needs no quoting:
 *
 *   IsWhitespaceOrNull(c)   -> (unsigned char)c <= 0x20
 *   valid_unquoted_char(c)  -> (unsigned char)c >  0x20 && c != '"' && c != '\''
 *
 * So "whitespace" here includes NUL, which is how the end of a line and a
 * separator are the same test in the walks below. Stripping is the other
 * one -- it must stop AT the NUL rather than walk through it -- so it
 * gets its own predicate, as MPD's StripLeft does.
 */
static bool is_ws(char c)   { return (unsigned char)c <= 0x20; }
static bool is_sep(char c)  { return c != '\0' && (unsigned char)c <= 0x20; }

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_word(char c)
{
    return is_alpha(c) || (c >= '0' && c <= '9') || c == '_';
}

static bool is_unquoted(char c)
{
    return (unsigned char)c > 0x20 && c != '"' && c != '\'';
}

static char *strip_left(char *p)
{
    while (is_sep(*p)) p++;
    return p;
}

/* ---- the tokeniser ------------------------------------------------- */

/*
 * Each of these returns the token and advances `*ip` past it, or returns
 * NULL. NULL with `*err` still NULL means end of line; NULL with `*err`
 * set is a malformed line, and the string is MPD's own wording so a
 * client's log matches what MPD would have said.
 */

static char *next_word(char **ip, const char **err)
{
    char *p = *ip;
    if (*p == '\0') return NULL;
    if (!is_alpha(*p)) { *err = "Letter expected"; return NULL; }

    char *const word = p;
    while (*++p != '\0') {
        if (is_ws(*p)) {
            *p = '\0';
            *ip = strip_left(p + 1);
            return word;
        }
        if (!is_word(*p)) { *err = "Invalid word character"; return NULL; }
    }
    *ip = p;                        /* at the NUL: the line ends here */
    return word;
}

static char *next_unquoted(char **ip, const char **err)
{
    char *p = *ip;
    if (*p == '\0') return NULL;
    if (!is_unquoted(*p)) { *err = "Invalid unquoted character"; return NULL; }

    char *const word = p;
    while (*++p != '\0') {
        if (is_ws(*p)) {
            *p = '\0';
            *ip = strip_left(p + 1);
            return word;
        }
        if (!is_unquoted(*p)) { *err = "Invalid unquoted character"; return NULL; }
    }
    *ip = p;
    return word;
}

/*
 * A quoted argument, unescaped OVER ITSELF. `dest` never passes `p`
 * because an escape removes a byte and nothing adds one, so this cannot
 * grow past its own token -- which is the property that makes in-place
 * tokenising safe rather than merely cheap.
 */
static char *next_string(char **ip, const char **err)
{
    char *p = *ip;
    if (*p == '\0') return NULL;
    if (*p != '"') { *err = "'\"' expected"; return NULL; }

    char *const word = p;
    char *dest = p;
    p++;

    while (*p != '"') {
        /* The backslash escapes WHATEVER FOLLOWS, not a named set: `\n`
         * in an MPD argument is the letter n. A backslash at the end of
         * the line lands the check below on the NUL, which is the same
         * fault as a missing quote and MPD reports it as one. */
        if (*p == '\\') p++;
        if (*p == '\0') { *err = "Missing closing '\"'"; return NULL; }
        *dest++ = *p++;
    }

    p++;                            /* past the closing quote */
    if (!is_ws(*p)) { *err = "Space expected after closing '\"'"; return NULL; }
    *dest = '\0';
    *ip = strip_left(p);
    return word;
}

static char *next_param(char **ip, const char **err)
{
    return (**ip == '"') ? next_string(ip, err) : next_unquoted(ip, err);
}

static void cmd_reset(mpd_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = MPD_CMD_UNKNOWN;
}

/*
 * `quote` is the piece MPD names in the message and `cur` what it puts in
 * {current_command}; either may be NULL. They are separate because they
 * differ for the two faults that matter: an unknown verb is named in the
 * message with an empty brace, an arity fault in both.
 */
static bool fail_at(mpd_cmd_t *out, mpd_ack_t ack, const char *err,
                    const char *quote, const char *cur)
{
    out->kind      = MPD_CMD_UNKNOWN;
    out->ack       = ack;
    out->err       = err;
    out->err_quote = quote;
    out->err_cmd   = cur;
    return false;
}

static bool fail(mpd_cmd_t *out, mpd_ack_t ack, const char *err)
{
    return fail_at(out, ack, err, NULL, NULL);
}

bool mpdproto_tokenise(char *line, mpd_cmd_t *out)
{
    if (!out) return false;
    cmd_reset(out);
    if (!line) return fail(out, MPD_ACK_UNKNOWN, "No command given");

    /* Longer than the buffer we agreed to read is not a command. The
     * caller has already refused to read past MPDPROTO_LINE_MAX, so this
     * is the belt to that braces -- and it runs before the walks, which
     * are the things that would otherwise chew through it. */
    if (strlen(line) >= MPDPROTO_LINE_MAX)
        return fail(out, MPD_ACK_UNKNOWN, "Line too long");

    /* NOT stripped first, deliberately: MPD hands the line to the
     * tokeniser as it stands, so a leading space is "Letter expected"
     * rather than a command. A client that indents is not one. */
    char *p = line;
    const char *err = NULL;

    char *const verb = next_word(&p, &err);
    if (!verb)
        return fail(out, MPD_ACK_UNKNOWN, err ? err : "No command given");
    out->verb = verb;

    for (;;) {
        err = NULL;
        char *const a = next_param(&p, &err);
        if (!a) {
            if (err) return fail(out, MPD_ACK_UNKNOWN, err);
            break;                  /* end of line */
        }
        if (out->argc >= MPDPROTO_MAX_ARGS) {
            /* MPD's wording and MPD's code, and note the code: this one
             * IS an argument fault, where the parse failures above are
             * not. MPD quotes nothing here -- the message is the whole
             * message -- and the brace is still empty, because the
             * lookup has not run. */
            out->verb = verb;
            return fail(out, MPD_ACK_ARG, "Too many arguments");
        }
        out->argv[out->argc++] = a;
    }

    /* A tokenised line with no kind yet: mpdproto_parse() fills it. */
    return true;
}

/* ---- the command table --------------------------------------------- */

/*
 * MPD's verbs, MPD's arities (src/command/AllCommands.cxx), in the order
 * the enum declares them so the two read together. `min` of -1 means MPD
 * does not check -- `close` and `kill` take whatever they are given and
 * ignore it -- and `max` of -1 means unbounded, up to MPDPROTO_MAX_ARGS.
 *
 * WHERE THIS IS NARROWER THAN MPD, it is because MPDPROTO_VERSION claims
 * 0.20 and the wider form is newer:
 *
 *   add   1..1 here, 1..2 there   the position argument is 0.24
 *   load  1..2 here, 1..3 there   the third argument is 0.24
 *
 * A verb in this table is not a verb that works; it is a verb whose
 * SHAPE is known, so that a wrong number of arguments is caught here
 * with MPD's message instead of inside a handler with a worse one.
 */
typedef struct {
    const char    *verb;
    mpd_cmd_kind_t kind;
    int8_t         min, max;
} cmd_def_t;

static const cmd_def_t s_cmds[] = {
    /* step 9 */
    { "ping",           MPD_CMD_PING,            0,  0 },
    { "close",          MPD_CMD_CLOSE,          -1, -1 },
    { "commands",       MPD_CMD_COMMANDS,        0,  0 },
    { "notcommands",    MPD_CMD_NOTCOMMANDS,     0,  0 },
    { "tagtypes",       MPD_CMD_TAGTYPES,        0, -1 },
    { "urlhandlers",    MPD_CMD_URLHANDLERS,     0,  0 },
    { "decoders",       MPD_CMD_DECODERS,        0,  0 },
    { "status",         MPD_CMD_STATUS,          0,  0 },
    { "stats",          MPD_CMD_STATS,           0,  0 },
    { "currentsong",    MPD_CMD_CURRENTSONG,     0,  0 },
    { "clearerror",     MPD_CMD_CLEARERROR,      0,  0 },
    { "play",           MPD_CMD_PLAY,            0,  1 },
    { "playid",         MPD_CMD_PLAYID,          0,  1 },
    { "pause",          MPD_CMD_PAUSE,           0,  1 },
    { "stop",           MPD_CMD_STOP,            0,  0 },
    { "next",           MPD_CMD_NEXT,            0,  0 },
    { "previous",       MPD_CMD_PREVIOUS,        0,  0 },
    { "seek",           MPD_CMD_SEEK,            2,  2 },
    { "seekid",         MPD_CMD_SEEKID,          2,  2 },
    { "seekcur",        MPD_CMD_SEEKCUR,         1,  1 },
    { "setvol",         MPD_CMD_SETVOL,          1,  1 },
    { "volume",         MPD_CMD_VOLUME,          1,  1 },
    { "outputs",        MPD_CMD_OUTPUTS,         0,  0 },
    /* 5161: arities from AllCommands.cxx */
    { "replay_gain_mode",   MPD_CMD_REPLAY_GAIN_MODE,   1, 1 },
    { "replay_gain_status", MPD_CMD_REPLAY_GAIN_STATUS, 0, 0 },
    { "channels",       MPD_CMD_CHANNELS,        0,  0 },      /* 5163 */

    /* step 10 */
    { "idle",           MPD_CMD_IDLE,            0, -1 },
    /* No "noidle" (5160): see the enum. `noidle x` is therefore
     * `unknown command "noidle"`, which is what MPD says to it. */

    /* step 7's modes */
    { "repeat",         MPD_CMD_REPEAT,          1,  1 },
    { "random",         MPD_CMD_RANDOM,          1,  1 },
    { "single",         MPD_CMD_SINGLE,          1,  1 },
    { "consume",        MPD_CMD_CONSUME,         1,  1 },

    /* step 11 */
    { "add",            MPD_CMD_ADD,             1,  1 },
    { "addid",          MPD_CMD_ADDID,           1,  2 },
    { "delete",         MPD_CMD_DELETE,          1,  1 },
    { "deleteid",       MPD_CMD_DELETEID,        1,  1 },
    { "move",           MPD_CMD_MOVE,            2,  2 },
    { "moveid",         MPD_CMD_MOVEID,          2,  2 },
    { "clear",          MPD_CMD_CLEAR,           0,  0 },
    { "shuffle",        MPD_CMD_SHUFFLE,         0,  1 },
    { "playlistinfo",   MPD_CMD_PLAYLISTINFO,    0,  1 },
    { "playlistid",     MPD_CMD_PLAYLISTID,      0,  1 },
    { "playlist",       MPD_CMD_PLAYLIST,        0,  0 },
    { "plchanges",      MPD_CMD_PLCHANGES,       1,  2 },
    { "plchangesposid", MPD_CMD_PLCHANGESPOSID,  1,  2 },

    /* step 12 */
    { "lsinfo",         MPD_CMD_LSINFO,          0,  1 },
    { "listall",        MPD_CMD_LISTALL,         0,  1 },
    { "listallinfo",    MPD_CMD_LISTALLINFO,     0,  1 },
    { "find",           MPD_CMD_FIND,            1, -1 },
    { "search",         MPD_CMD_SEARCH,          1, -1 },
    { "list",           MPD_CMD_LIST,            1, -1 },
    { "count",          MPD_CMD_COUNT,           1, -1 },
    { "update",         MPD_CMD_UPDATE,          0,  1 },
    { "rescan",         MPD_CMD_RESCAN,          0,  1 },

    /* step 13 */
    { "listplaylists",    MPD_CMD_LISTPLAYLISTS,    0, 0 },
    { "listplaylist",     MPD_CMD_LISTPLAYLIST,     1, 2 },
    { "listplaylistinfo", MPD_CMD_LISTPLAYLISTINFO, 1, 2 },
    { "load",             MPD_CMD_LOAD,             1, 2 },
    { "save",             MPD_CMD_SAVE,             1, 2 },
    { "rm",               MPD_CMD_RM,               1, 1 },
    { "playlistadd",      MPD_CMD_PLAYLISTADD,      2, 3 },     /* 5200 */
    { "playlistdelete",   MPD_CMD_PLAYLISTDELETE,   2, 2 },

    /* 5180: arities from AllCommands.cxx */
    { "listpartitions",   MPD_CMD_LISTPARTITIONS,   0, 0 },
    { "partition",        MPD_CMD_PARTITION,        1, 1 },
    { "newpartition",     MPD_CMD_NEWPARTITION,     1, 1 },
    { "delpartition",     MPD_CMD_DELPARTITION,     1, 1 },
    { "moveoutput",       MPD_CMD_MOVEOUTPUT,       1, 1 },
    { "listmounts",       MPD_CMD_LISTMOUNTS,       0, 0 },
    { "mount",            MPD_CMD_MOUNT,            2, 2 },
    { "unmount",          MPD_CMD_UNMOUNT,          1, 1 },
    { "listneighbors",    MPD_CMD_LISTNEIGHBORS,    0, 0 },

    /* 5218: arities from AllCommands.cxx */
    { "getvol",           MPD_CMD_GETVOL,           0, 0 },
    { "password",         MPD_CMD_PASSWORD,         1, 1 },
    { "crossfade",        MPD_CMD_CROSSFADE,        1, 1 },

    /* 5219: arities from AllCommands.cxx */
    { "enableoutput",     MPD_CMD_ENABLEOUTPUT,        1,  1 },
    { "disableoutput",    MPD_CMD_DISABLEOUTPUT,       1,  1 },
    { "toggleoutput",     MPD_CMD_TOGGLEOUTPUT,        1,  1 },

    /* 5220: arities from AllCommands.cxx */
    { "swap",             MPD_CMD_SWAP,                2,  2 },
    { "swapid",           MPD_CMD_SWAPID,              2,  2 },

    /* 5221: arities from AllCommands.cxx */
    { "findadd",          MPD_CMD_FINDADD,             1, -1 },
    { "searchadd",        MPD_CMD_SEARCHADD,           1, -1 },
    { "searchaddpl",      MPD_CMD_SEARCHADDPL,         2, -1 },

    /* 5222: arities from AllCommands.cxx */
    { "playlistfind",     MPD_CMD_PLAYLISTFIND,        1, -1 },
    { "playlistsearch",   MPD_CMD_PLAYLISTSEARCH,      1, -1 },

    /* 5223: arities from AllCommands.cxx */
    { "playlistclear",    MPD_CMD_PLAYLISTCLEAR,       1,  1 },
    { "playlistmove",     MPD_CMD_PLAYLISTMOVE,        3,  3 },
    { "rename",           MPD_CMD_RENAME,              2,  2 },

    /* 5228: arities from AllCommands.cxx */
    { "listfiles",        MPD_CMD_LISTFILES,           0,  1 },

    /* 5230: arities from AllCommands.cxx */
    { "subscribe",        MPD_CMD_SUBSCRIBE,           1,  1 },
    { "unsubscribe",      MPD_CMD_UNSUBSCRIBE,         1,  1 },
    { "readmessages",     MPD_CMD_READMESSAGES,        0,  0 },
    { "sendmessage",      MPD_CMD_SENDMESSAGE,         2,  2 },

    /* 5231: arities from AllCommands.cxx */
    { "prio",             MPD_CMD_PRIO,                2, -1 },
    { "prioid",           MPD_CMD_PRIOID,              2, -1 },
    { "rangeid",          MPD_CMD_RANGEID,             2,  2 },
    { "addtagid",         MPD_CMD_ADDTAGID,            3,  3 },
    { "cleartagid",       MPD_CMD_CLEARTAGID,          1,  2 },
    { "readcomments",     MPD_CMD_READCOMMENTS,        1,  1 },
    { "mixrampdb",        MPD_CMD_MIXRAMPDB,           1,  1 },
    { "mixrampdelay",     MPD_CMD_MIXRAMPDELAY,        1,  1 },
    { "kill",             MPD_CMD_KILL,               -1, -1 },
    { "config",           MPD_CMD_CONFIG,              0,  0 },
    { "sticker",          MPD_CMD_STICKER,             3, -1 },

    /* 5240: arities from AllCommands.cxx */
    { "albumart",         MPD_CMD_ALBUMART,            2,  2 },
};

#define N_CMDS  (sizeof(s_cmds) / sizeof(s_cmds[0]))

static const cmd_def_t *lookup(const char *verb)
{
    for (size_t i = 0; i < N_CMDS; i++)
        if (strcmp(s_cmds[i].verb, verb) == 0) return &s_cmds[i];
    return NULL;
}

const char *mpdproto_verb(mpd_cmd_kind_t kind)
{
    if (kind == MPD_CMD_UNKNOWN) return NULL;
    for (size_t i = 0; i < N_CMDS; i++)
        if (s_cmds[i].kind == kind) return s_cmds[i].verb;
    return NULL;
}

bool mpdproto_parse(char *line, mpd_cmd_t *out)
{
    if (!mpdproto_tokenise(line, out)) return false;

    const cmd_def_t *const d = lookup(out->verb);
    /* The verb is named in the message and the brace is left empty:
     * MPD only calls SetCommand() once the lookup has succeeded. */
    if (!d) return fail_at(out, MPD_ACK_UNKNOWN, "unknown command",
                           out->verb, NULL);

    /* MPD's three messages, and they are three rather than one because
     * "wrong number of arguments" tells a client nothing when the verb
     * takes a range. min < 0 is the pair that is never checked. By here
     * the lookup has succeeded, so the brace carries the verb too. */
    if (d->min >= 0) {
        if (d->min == d->max && out->argc != d->min)
            return fail_at(out, MPD_ACK_ARG, "wrong number of arguments for",
                           d->verb, d->verb);
        if (out->argc < d->min)
            return fail_at(out, MPD_ACK_ARG, "too few arguments for",
                           d->verb, d->verb);
        if (d->max >= 0 && out->argc > d->max)
            return fail_at(out, MPD_ACK_ARG, "too many arguments for",
                           d->verb, d->verb);
    }

    out->kind = d->kind;
    return true;
}

/* ---- command lists ------------------------------------------------- */

mpd_line_t mpdproto_line(char *line, mpd_list_t *st, mpd_cmd_t *out)
{
    if (!st || !out) return MPD_LINE_ERR;
    if (!line) { cmd_reset(out); fail(out, MPD_ACK_UNKNOWN, "No command given"); return MPD_LINE_ERR; }

    /* The three list verbs are handled here and never reach the table,
     * which is where MPD keeps them too. Compared as whole lines: they
     * take no arguments, and `command_list_begin x` is not one of them. */
    if (strcmp(line, "command_list_begin") == 0 ||
        strcmp(line, "command_list_ok_begin") == 0) {
        if (!st->active) {
            st->active  = true;
            st->verbose = (strcmp(line, "command_list_ok_begin") == 0);
            st->index   = 0;
            return MPD_LINE_LIST_BEGIN;
        }
        /* Inside a list this is NOT nesting. It falls through to the
         * table, which does not hold it, and comes back `unknown
         * command` -- which is what MPD answers, because MPD has queued
         * it as an ordinary line. */
    } else if (strcmp(line, "command_list_end") == 0) {
        if (!st->active) {
            cmd_reset(out);
            /* The code that exists for exactly this. */
            fail(out, MPD_ACK_NOT_LIST, "not in a command list");
            return MPD_LINE_ERR;
        }
        st->active  = false;
        st->verbose = false;
        st->index   = 0;
        return MPD_LINE_LIST_END;
    }

    if (!mpdproto_parse(line, out)) return MPD_LINE_ERR;
    return MPD_LINE_CMD;
}

/* ---- framing ------------------------------------------------------- */

static size_t put_line(const char *s, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    const size_t n = strlen(s);
    if (n + 1 > cap) return 0;      /* never half a terminator */
    memcpy(out, s, n + 1);
    return n;
}

size_t mpdproto_ok(char *out, size_t cap)      { return put_line("OK\n", out, cap); }
size_t mpdproto_list_ok(char *out, size_t cap) { return put_line("list_OK\n", out, cap); }

/*
 * One field of an ACK, with the bytes that would break framing removed.
 *
 * A newline in either field ends the response early and leaves the rest
 * to be read as the next one, which is a client desynchronised for the
 * life of the connection rather than a bad error string. `}` in the
 * command field would close the brace early, but the command has already
 * been through next_word() and cannot contain one; `text` can, because it
 * may name a path off a card, and MPD does not escape it either -- so the
 * brace is left alone and only the framing bytes are replaced.
 *
 * Nothing above 0x7F is touched: a UTF-8 path should arrive intact.
 */
static void put_clean(const char *s, char *out, size_t cap, size_t *n)
{
    for (; *s && *n + 1 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        out[(*n)++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    out[*n] = '\0';
}

size_t mpdproto_ack(mpd_ack_t code, int list_num, const char *cmd,
                    const char *text, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    char head[48];
    const int hn = snprintf(head, sizeof(head), "ACK [%d@%d] {",
                            (int)code, list_num < 0 ? 0 : list_num);
    if (hn <= 0 || (size_t)hn >= sizeof(head)) return 0;

    size_t n = 0;
    if ((size_t)hn + 1 > cap) return 0;
    memcpy(out, head, (size_t)hn);
    n = (size_t)hn;
    out[n] = '\0';

    put_clean(cmd ? cmd : "", out, cap, &n);
    put_clean("} ", out, cap, &n);
    put_clean(text ? text : "", out, cap, &n);

    /*
     * THE TERMINATOR IS WRITTEN RAW, and it has to be: put_clean() turns
     * every byte below 0x20 into a space, which is the whole point of it,
     * and a newline is one. Passing the terminator through the function
     * whose job is to remove newlines produced an ACK that ended in a
     * space, failed its own framing check below, and came back as no ACK
     * at all -- so every error on the connection was silent rather than
     * malformed, which is the worse of the two.
     *
     * Truncation is not an option for the same reason a partial OK is
     * not: the newline is what ends the response, so a message that did
     * not fit is no message.
     */
    if (n + 2 > cap) { out[0] = '\0'; return 0; }
    out[n++] = '\n';
    out[n]   = '\0';
    return n;
}

size_t mpdproto_ack_cmd(const mpd_cmd_t *c, int list_num, char *out, size_t cap)
{
    if (!c || !out || cap == 0) return 0;
    out[0] = '\0';

    const char *const err = c->err ? c->err : "unknown error";

    if (!c->err_quote)
        return mpdproto_ack(c->ack, list_num, c->err_cmd, err, out, cap);

    /*
     * MPD quotes the name it is complaining about -- `unknown command
     * "foo"` -- and the name came off the wire, so it is bounded by
     * next_word()'s alphabet and cannot carry a quote or a newline into
     * the message. The join needs a buffer, and this is the only one in
     * the file: a verb is at most a word, so it is small enough for a
     * stack frame by CLAUDE.md's rule and does not want to be static,
     * because two connections ACK independently.
     */
    char text[128];
    const int n = snprintf(text, sizeof(text), "%s \"%s\"", err, c->err_quote);
    if (n <= 0) return 0;
    /* A verb too long to name is still a verb to refuse, so the message
     * falls back to the wording alone rather than to no ACK at all. */
    if ((size_t)n >= sizeof(text))
        return mpdproto_ack(c->ack, list_num, c->err_cmd, err, out, cap);

    return mpdproto_ack(c->ack, list_num, c->err_cmd, text, out, cap);
}

/* ---- response bodies ----------------------------------------------- */

/*
 * An append buffer that refuses rather than truncates, like
 * remoteproto.c's. Once something did not fit, `over` is set and
 * w_done() returns 0 and empties the buffer, so a caller never sees a
 * response with a line missing from the middle -- which would be worse
 * than no response, because a client would act on it.
 *
 * The early-out on `over` in w_raw() is NOT OBSERVABLE through the
 * public surface, and is marked rather than removed for the same reason
 * as mpduri.c's leading-slash check: every w_raw() tests capacity for
 * itself, so a later short write cannot overflow anything, and w_done()
 * returns 0 either way once the flag is set. A mutation that deletes the
 * early-out is one the suite cannot catch. What it buys is that the
 * buffer stops being written after the first refusal, so a caller
 * reading it in a debugger sees where the response stopped rather than a
 * response with a hole in it.
 */
typedef struct {
    char  *p;
    size_t cap, n;
    bool   over;
} wbuf_t;

static void w_raw(wbuf_t *b, const char *s, size_t len)
{
    if (b->over) return;
    if (b->n + len + 1 > b->cap) { b->over = true; return; }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}

static void w_str(wbuf_t *b, const char *s) { w_raw(b, s, strlen(s)); }

/*
 * One value, repaired.
 *
 * Two separate faults, and a value off a card can carry both at once:
 * invalid UTF-8, which an MPD client shows as mojibake or drops the
 * response over, and a byte below 0x20, which would END THIS LINE and
 * make the remainder look like another key. So a bad byte becomes U+FFFD
 * and a control byte becomes a space, and neither is escaped, because
 * MPD's format has no escape -- a value runs to the newline.
 */
static void w_value(wbuf_t *b, const char *s)
{
    if (!s) return;
    const unsigned char *u = (const unsigned char *)s;
    size_t left = strlen(s);
    while (left && !b->over) {
        const unsigned char c = *u;
        if (c < 0x20 || c == 0x7F) {
            w_raw(b, " ", 1);
            u++; left--;
        } else {
            const size_t n = remoteproto_utf8_len(u, left);
            if (n) {
                w_raw(b, (const char *)u, n);
                u += n; left -= n;
            } else {
                w_raw(b, "\xEF\xBF\xBD", 3);     /* U+FFFD */
                u++; left--;
            }
        }
    }
}

static void w_kv(wbuf_t *b, const char *key, const char *value)
{
    w_str(b, key);
    w_raw(b, ": ", 2);
    w_value(b, value);
    w_raw(b, "\n", 1);
}

/* A tag that is absent is absent: see the header on why an empty line is
 * not the same thing to a client. */
static void w_tag(wbuf_t *b, const char *key, const char *value)
{
    if (value && value[0]) w_kv(b, key, value);
}

static void w_num(wbuf_t *b, const char *key, long long v)
{
    char t[24];
    const int n = snprintf(t, sizeof(t), "%lld", (long long)v);
    if (n <= 0 || (size_t)n >= sizeof(t)) { b->over = true; return; }
    w_str(b, key);
    w_raw(b, ": ", 2);
    w_raw(b, t, (size_t)n);
    w_raw(b, "\n", 1);
}

/*
 * Milliseconds as MPD's seconds-with-three-decimals. Written from the
 * integer rather than through a double and "%.3f": the value arrives as
 * an integer count of milliseconds, and passing it through a float to get
 * a fixed three decimals back is a rounding step that can only lose.
 */
static void w_ms(wbuf_t *b, const char *key, int32_t ms)
{
    if (ms < 0) return;
    char t[32];
    const int n = snprintf(t, sizeof(t), "%ld.%03ld",
                           (long)(ms / 1000), (long)(ms % 1000));
    if (n <= 0 || (size_t)n >= sizeof(t)) { b->over = true; return; }
    w_str(b, key);
    w_raw(b, ": ", 2);
    w_raw(b, t, (size_t)n);
    w_raw(b, "\n", 1);
}

/* Whole seconds, MPD's rounding and not truncation: at 12.6 s elapsed,
 * `time` says 13. Integer arithmetic, so a negative never reaches here. */
static long ms_round_s(int32_t ms)
{
    return (long)((ms + 500) / 1000);
}

static size_t w_done(wbuf_t *b)
{
    if (b->over) { if (b->cap) b->p[0] = '\0'; return 0; }
    return b->n;
}

size_t mpdproto_kv(const char *key, const char *value, char *out, size_t cap)
{
    if (!key || !out || cap == 0) return 0;
    wbuf_t b = { out, cap, 0, false };
    out[0] = '\0';
    w_kv(&b, key, value);
    return w_done(&b);
}

size_t mpdproto_directory(const char *uri, char *out, size_t cap)
{
    if (!uri || !out || cap == 0) return 0;
    wbuf_t b = { out, cap, 0, false };
    out[0] = '\0';
    w_kv(&b, "directory", uri);
    return w_done(&b);
}

size_t mpdproto_song(const mpd_song_t *s, char *out, size_t cap)
{
    if (!s || !s->uri || !out || cap == 0) return 0;
    wbuf_t b = { out, cap, 0, false };
    out[0] = '\0';

    /* file: first. MPD's readers key on it to start a new song, so a tag
     * emitted before it belongs to the previous one. */
    w_kv(&b, "file", s->uri);

    w_tag(&b, "Title",  s->title);
    w_tag(&b, "Artist", s->artist);
    w_tag(&b, "Album",  s->album);

    /* Both forms, as MPD sends both: `Time` is whole seconds for old
     * clients, `duration` is the fractional one modern clients read. Note
     * the capitalisation is MPD's and is not a typo -- Time, duration. */
    if (s->duration_ms >= 0) {
        w_num(&b, "Time", ms_round_s(s->duration_ms));
        w_ms(&b, "duration", s->duration_ms);
    }

    if (s->pos >= 0) {
        w_num(&b, "Pos", s->pos);
        w_num(&b, "Id", (long long)s->id);
    }

    return w_done(&b);
}

size_t mpdproto_status(const mpd_status_t *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) return 0;
    wbuf_t b = { out, cap, 0, false };
    out[0] = '\0';

    if (s->volume >= 0) w_num(&b, "volume", s->volume);

    w_num(&b, "repeat",  s->repeat  ? 1 : 0);
    w_num(&b, "random",  s->random  ? 1 : 0);
    if (s->single_oneshot) w_kv(&b, "single", "oneshot");          /* 5241, 0.21 */
    else w_num(&b, "single",  s->single  ? 1 : 0);
    w_num(&b, "consume", s->consume ? 1 : 0);
    if (s->partition && s->partition[0]) w_kv(&b, "partition", s->partition);   /* 5180 */
    w_num(&b, "playlist", (long long)s->playlist_version);
    w_num(&b, "playlistlength", s->playlist_length);

    const char *st = "stop";
    if (s->state == MPD_STATE_PLAY)  st = "play";
    if (s->state == MPD_STATE_PAUSE) st = "pause";
    w_kv(&b, "state", st);

    if (s->song >= 0) {
        w_num(&b, "song", s->song);
        w_num(&b, "songid", (long long)s->songid);
    }

    /*
     * Only when not stopped, as MPD does. A stopped player has no elapsed
     * time, and reporting 0 puts a client's progress bar at the start of
     * a track it is not playing -- which looks like a paused player.
     */
    if (s->state != MPD_STATE_STOP) {
        const int32_t el = s->elapsed_ms < 0 ? 0 : s->elapsed_ms;
        char t[48];
        const int n = snprintf(t, sizeof(t), "%ld:%ld", ms_round_s(el),
                               s->duration_ms < 0 ? 0L : ms_round_s(s->duration_ms));
        if (n > 0 && (size_t)n < sizeof(t)) {
            w_str(&b, "time");
            w_raw(&b, ": ", 2);
            w_raw(&b, t, (size_t)n);
            w_raw(&b, "\n", 1);
        } else {
            b.over = true;
        }
        w_ms(&b, "elapsed", el);
        if (s->duration_ms >= 0) w_ms(&b, "duration", s->duration_ms);
        if (s->bitrate >= 0) w_num(&b, "bitrate", s->bitrate);
        if (s->sample_rate > 0) {
            char a[48];
            const int m = snprintf(a, sizeof(a), "%d:%d:%d",
                                   s->sample_rate, s->bits, s->channels);
            if (m > 0 && (size_t)m < sizeof(a)) {
                w_str(&b, "audio");
                w_raw(&b, ": ", 2);
                w_raw(&b, a, (size_t)m);
                w_raw(&b, "\n", 1);
            } else {
                b.over = true;
            }
        }
    }

    if (s->updating_db) w_num(&b, "updating_db", (long long)s->updating_db);
    if (s->error && s->error[0]) w_kv(&b, "error", s->error);

    /* Last, which is where MPD puts it. */
    if (s->next_song >= 0) {
        w_num(&b, "nextsong", s->next_song);
        w_num(&b, "nextsongid", (long long)s->next_songid);
    }

    return w_done(&b);
}
