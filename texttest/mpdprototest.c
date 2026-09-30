/*
 * mpdprototest.c -- mpdproto.c: the grammar, the table, and the framing.
 *
 * The cases here are written from MPD's OWN rules -- src/util/Tokenizer.cxx
 * for the quoting, src/command/AllCommands.cxx for the arities and the
 * three arity messages, src/protocol/Ack.hxx for the codes -- and not
 * from what mpdproto.c happens to do with them, which is
 * texttest/README.md's rule. It matters more here than usual: a grammar
 * test written by reading the implementation blesses whatever the
 * implementation got wrong about a protocol nobody here can interview,
 * and the failure mode is a real client sending a legal line and being
 * told it is illegal.
 *
 * So the expected strings below are MPD's, transcribed. Where this device
 * is deliberately narrower than MPD -- `add` is 1..1 because
 * MPDPROTO_VERSION claims 0.20 and the position argument is 0.24 -- the
 * case says so.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mpdproto.h"
/* For MPDURI_MAX: the worst-case song line has to hold a maximum uri,
 * and tying the case to that constant keeps it testing the real worst
 * case if the index's path limit ever moves. */
#include "../main/mpduri.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/*
 * Tokenising is in place, so every case needs its own writable copy --
 * and a fresh heap one rather than a reused buffer, so ASan sees a walk
 * that runs off either end of the actual line rather than into a
 * neighbour's slack.
 */
static char *dup_line(const char *s)
{
    const size_t n = strlen(s);
    char *p = malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* The tokens, joined, so a whole parse is one comparison: "verb|a|b". */
static void join(const mpd_cmd_t *c, char *out, size_t n)
{
    out[0] = '\0';
    if (!c->verb) return;
    snprintf(out, n, "%s", c->verb);
    for (int i = 0; i < c->argc; i++) {
        strncat(out, "|", n - strlen(out) - 1);
        strncat(out, c->argv[i], n - strlen(out) - 1);
    }
}

/* Tokenise `in` and expect the joined result `want`. */
static void ok_tok(const char *in, const char *want)
{
    char *line = dup_line(in);
    mpd_cmd_t c;
    char got[512];
    const bool r = mpdproto_tokenise(line, &c);
    CHECK(r, "tokenise refused [%s]: %s", in, c.err ? c.err : "(no message)");
    if (r) {
        join(&c, got, sizeof(got));
        CHECK(strcmp(got, want) == 0, "[%s] gave [%s], want [%s]", in, got, want);
    }
    free(line);
}

/* Tokenise `in` and expect refusal with MPD's message `want`. */
static void bad_tok(const char *in, mpd_ack_t ack, const char *want)
{
    char *line = dup_line(in);
    mpd_cmd_t c;
    const bool r = mpdproto_tokenise(line, &c);
    CHECK(!r, "tokenise accepted [%s]", in);
    if (!r) {
        CHECK(c.ack == ack, "[%s] gave ack %d, want %d", in, (int)c.ack, (int)ack);
        CHECK(c.err && strcmp(c.err, want) == 0,
              "[%s] said [%s], want [%s]", in, c.err ? c.err : "(null)", want);
        CHECK(c.kind == MPD_CMD_UNKNOWN, "[%s] left a kind behind", in);
    }
    free(line);
}

/* Parse `in` and expect refusal, with the assembled ACK equal to `want`. */
static void bad_parse(const char *in, mpd_ack_t ack, const char *want)
{
    char *line = dup_line(in);
    mpd_cmd_t c;
    char ack_buf[256];
    const bool r = mpdproto_parse(line, &c);
    CHECK(!r, "parse accepted [%s]", in);
    if (!r) {
        CHECK(c.ack == ack, "[%s] gave ack %d, want %d", in, (int)c.ack, (int)ack);
        const size_t n = mpdproto_ack_cmd(&c, 0, ack_buf, sizeof(ack_buf));
        CHECK(n == strlen(want) && strcmp(ack_buf, want) == 0,
              "[%s] acked [%s], want [%s]", in, ack_buf, want);
    }
    free(line);
}

static void ok_parse(const char *in, mpd_cmd_kind_t kind, int argc)
{
    char *line = dup_line(in);
    mpd_cmd_t c;
    const bool r = mpdproto_parse(line, &c);
    CHECK(r, "parse refused [%s]: %s", in, c.err ? c.err : "(no message)");
    if (r) {
        CHECK(c.kind == kind, "[%s] gave kind %d, want %d", in, (int)c.kind, (int)kind);
        CHECK(c.argc == argc, "[%s] gave %d args, want %d", in, c.argc, argc);
    }
    free(line);
}

int main(void)
{
    /* ---- the verb ---------------------------------------------------- */
    /*
     * MPD's NextWord: ASCII alpha first, then alnum or '_'. Nothing else,
     * and note what that excludes -- a leading digit and an interior dash
     * are both errors, with two DIFFERENT messages, because the first
     * character is checked before the loop.
     */
    ok_tok("play", "play");
    ok_tok("command_list_begin", "command_list_begin");
    ok_tok("replay_gain_mode", "replay_gain_mode");
    ok_tok("mpd2", "mpd2");

    bad_tok("", MPD_ACK_UNKNOWN, "No command given");
    bad_tok("2play", MPD_ACK_UNKNOWN, "Letter expected");
    bad_tok("_play", MPD_ACK_UNKNOWN, "Letter expected");
    bad_tok("play-now", MPD_ACK_UNKNOWN, "Invalid word character");
    bad_tok("play.now", MPD_ACK_UNKNOWN, "Invalid word character");
    /* A leading space is not stripped: MPD hands the line to the
     * tokeniser as it stands, so the first character is a space and the
     * first character must be a letter. */
    bad_tok(" play", MPD_ACK_UNKNOWN, "Letter expected");

    /* ---- unquoted arguments ----------------------------------------- */
    /*
     * Any run of bytes above 0x20 except '"' and '\''. Above 0x20 takes
     * in every UTF-8 continuation byte, so an accented path needs no
     * quotes -- and an apostrophe is an ERROR rather than a quote,
     * because MPD's NextParam only ever opens a string on '"'.
     */
    ok_tok("add sd/a.mp3", "add|sd/a.mp3");
    ok_tok("add caf\xC3\xA9/a.mp3", "add|caf\xC3\xA9/a.mp3");
    ok_tok("seek 0 12.5", "seek|0|12.5");
    ok_tok("add a\\b", "add|a\\b");     /* a backslash is ordinary unquoted */
    bad_tok("add don't", MPD_ACK_UNKNOWN, "Invalid unquoted character");
    bad_tok("add 'x'", MPD_ACK_UNKNOWN, "Invalid unquoted character");

    /* ---- separators ------------------------------------------------- */
    /*
     * Bytes 0x01..0x20, so a tab separates like a space, runs collapse,
     * and a trailing "\r" from a CRLF client is stripped rather than
     * becoming the last byte of the last argument -- which would make
     * every path from such a client not exist.
     */
    ok_tok("add\tfoo", "add|foo");
    ok_tok("add   foo    bar", "add|foo|bar");
    ok_tok("add foo\r", "add|foo");
    ok_tok("add foo ", "add|foo");
    ok_tok("status\r", "status");

    /* ---- quoted arguments ------------------------------------------- */
    /*
     * The whole reason this file is not remoteproto_parse(): a path with
     * a space in it. The backslash escapes WHATEVER FOLLOWS -- so `\n` is
     * the letter n, not a newline, which is the case a C programmer
     * writes wrong by reflex.
     */
    ok_tok("add \"foo bar.mp3\"", "add|foo bar.mp3");
    ok_tok("add \"a\\\"b\"", "add|a\"b");          /* \" -> " */
    ok_tok("add \"a\\\\b\"", "add|a\\b");          /* \\ -> \ */
    ok_tok("add \"a\\nb\"", "add|anb");            /* \n -> n, NOT a newline */
    ok_tok("add \"\"", "add|");                    /* an empty argument */
    ok_tok("add \"a b\" \"c d\"", "add|a b|c d");
    ok_tok("add \"a b\"\t\"c\"", "add|a b|c");
    /* An unquoted argument may follow a quoted one and vice versa. */
    ok_tok("add \"a b\" c", "add|a b|c");
    ok_tok("find Artist \"The Fall\"", "find|Artist|The Fall");

    bad_tok("add \"abc", MPD_ACK_UNKNOWN, "Missing closing '\"'");
    /* A trailing backslash eats the NUL check, and MPD calls that a
     * missing quote rather than a bad escape. */
    bad_tok("add \"abc\\", MPD_ACK_UNKNOWN, "Missing closing '\"'");
    /* The closing quote must be followed by whitespace or end of line,
     * so this is ONE malformed argument and not two tokens. */
    bad_tok("add \"abc\"def", MPD_ACK_UNKNOWN, "Space expected after closing '\"'");
    bad_tok("add \"abc\"\"def\"", MPD_ACK_UNKNOWN, "Space expected after closing '\"'");

    /* ---- the argument ceiling --------------------------------------- */
    {
        /* MPD's is 2 + tag types * 2 and refuses the one past it with
         * ACK_ERROR_ARG "Too many arguments" -- an argument fault, unlike
         * every parse failure above, which are ACK_ERROR_UNKNOWN. */
        char line[512] = "find";
        for (int i = 0; i < MPDPROTO_MAX_ARGS; i++)
            strncat(line, " x", sizeof(line) - strlen(line) - 1);
        ok_parse(line, MPD_CMD_FIND, MPDPROTO_MAX_ARGS);

        strncat(line, " x", sizeof(line) - strlen(line) - 1);
        bad_tok(line, MPD_ACK_ARG, "Too many arguments");
    }

    /* ---- the line ceiling ------------------------------------------- */
    {
        char *big = malloc(MPDPROTO_LINE_MAX + 8);
        memset(big, 'a', MPDPROTO_LINE_MAX + 4);
        big[MPDPROTO_LINE_MAX + 4] = '\0';
        bad_tok(big, MPD_ACK_UNKNOWN, "Line too long");
        free(big);
    }

    /* ---- the table and its arities ---------------------------------- */
    ok_parse("status", MPD_CMD_STATUS, 0);
    ok_parse("currentsong", MPD_CMD_CURRENTSONG, 0);
    ok_parse("play", MPD_CMD_PLAY, 0);
    ok_parse("play 3", MPD_CMD_PLAY, 1);
    ok_parse("pause", MPD_CMD_PAUSE, 0);
    ok_parse("pause 1", MPD_CMD_PAUSE, 1);
    ok_parse("seek 0 12", MPD_CMD_SEEK, 2);
    ok_parse("seekcur 12", MPD_CMD_SEEKCUR, 1);
    ok_parse("setvol 50", MPD_CMD_SETVOL, 1);
    ok_parse("idle", MPD_CMD_IDLE, 0);
    ok_parse("replay_gain_status", MPD_CMD_REPLAY_GAIN_STATUS, 0);
    ok_parse("channels", MPD_CMD_CHANNELS, 0);
    ok_parse("getvol", MPD_CMD_GETVOL, 0);                          /* 5218 */
    ok_parse("password x", MPD_CMD_PASSWORD, 1);
    ok_parse("crossfade 0", MPD_CMD_CROSSFADE, 1);
    bad_parse("password", MPD_ACK_ARG,
              "ACK [2@0] {password} wrong number of arguments for \"password\"\n");
    bad_parse("channels x", MPD_ACK_ARG,
              "ACK [2@0] {channels} wrong number of arguments for \"channels\"\n");
    ok_parse("subscribe x", MPD_CMD_SUBSCRIBE, 1);                  /* 5230 */
    ok_parse("sendmessage x \"hello there\"", MPD_CMD_SENDMESSAGE, 2);
    bad_parse("readmessages x", MPD_ACK_ARG,
              "ACK [2@0] {readmessages} wrong number of arguments for \"readmessages\"\n");
    ok_parse("replay_gain_mode track", MPD_CMD_REPLAY_GAIN_MODE, 1);
    bad_parse("replay_gain_mode", MPD_ACK_ARG,
              "ACK [2@0] {replay_gain_mode} wrong number of arguments for \"replay_gain_mode\"\n");
    bad_parse("replay_gain_status x", MPD_ACK_ARG,
              "ACK [2@0] {replay_gain_status} wrong number of arguments for \"replay_gain_status\"\n");
    ok_parse("idle player mixer playlist", MPD_CMD_IDLE, 3);
    ok_parse("add foo", MPD_CMD_ADD, 1);
    ok_parse("addid foo", MPD_CMD_ADDID, 1);
    ok_parse("addid foo 2", MPD_CMD_ADDID, 2);
    ok_parse("move 3 0", MPD_CMD_MOVE, 2);
    ok_parse("plchanges 4", MPD_CMD_PLCHANGES, 1);
    ok_parse("lsinfo", MPD_CMD_LSINFO, 0);
    ok_parse("lsinfo sd", MPD_CMD_LSINFO, 1);
    ok_parse("shuffle", MPD_CMD_SHUFFLE, 0);

    /* min < 0: MPD checks nothing, so extra arguments are accepted and
     * ignored rather than refused. */
    ok_parse("close", MPD_CMD_CLOSE, 0);
    ok_parse("close a b c", MPD_CMD_CLOSE, 3);

    /* The three arity messages, which differ by whether the verb takes a
     * fixed count -- and the brace carries the verb here, because the
     * lookup has already succeeded. */
    bad_parse("seek 1", MPD_ACK_ARG,
              "ACK [2@0] {seek} wrong number of arguments for \"seek\"\n");
    bad_parse("status x", MPD_ACK_ARG,
              "ACK [2@0] {status} wrong number of arguments for \"status\"\n");
    bad_parse("addid", MPD_ACK_ARG,
              "ACK [2@0] {addid} too few arguments for \"addid\"\n");
    bad_parse("addid a b c", MPD_ACK_ARG,
              "ACK [2@0] {addid} too many arguments for \"addid\"\n");
    bad_parse("play 1 2", MPD_ACK_ARG,
              "ACK [2@0] {play} too many arguments for \"play\"\n");

    /* `add` is 1..1 HERE and 1..2 in MPD: the position argument arrived
     * in 0.24 and MPDPROTO_VERSION claims 0.20. Narrower than MPD on
     * purpose, so the case is here rather than absent. */
    bad_parse("add foo 2", MPD_ACK_ARG,
              "ACK [2@0] {add} wrong number of arguments for \"add\"\n");

    /* An unknown verb: named in the message, and the brace EMPTY, because
     * MPD only sets current_command once the lookup succeeds. */
    bad_parse("bogus", MPD_ACK_UNKNOWN,
              "ACK [5@0] {} unknown command \"bogus\"\n");
    /* 5240: albumart is 0.21 and answered now. readpicture is 0.22, the
     * picture inside the file, and stays unknown: a client must be told
     * it does not exist rather than be invited to ask. */
    ok_parse("albumart foo 0", MPD_CMD_ALBUMART, 2);
    bad_parse("albumart foo", MPD_ACK_ARG,
              "ACK [2@0] {albumart} wrong number of arguments for \"albumart\"\n");
    /* 5249: and readpicture is 0.22, answered now. */
    ok_parse("readpicture foo 0", MPD_CMD_READPICTURE, 2);
    ok_parse("binarylimit 8192", MPD_CMD_BINARYLIMIT, 1);
    bad_parse("readpicture foo 0 x", MPD_ACK_ARG,
              "ACK [2@0] {readpicture} wrong number of arguments for \"readpicture\"\n");
    /* `noidle` is not in MPD's command table (src/command/
     * AllCommands.cxx has "idle" and no "noidle"): the bare word is
     * caught as a raw line before tokenising, and anything that misses
     * that -- an argument after it -- is looked up and not found. 5153
     * had it as a verb of arity 0, which made this "wrong number of
     * arguments" and listed it in `commands`. */
    bad_parse("noidle", MPD_ACK_UNKNOWN,
              "ACK [5@0] {} unknown command \"noidle\"\n");
    bad_parse("noidle now", MPD_ACK_UNKNOWN,
              "ACK [5@0] {} unknown command \"noidle\"\n");

    /* ---- the table's own consistency -------------------------------- */
    {
        /*
         * A duplicate verb makes the second unreachable and a duplicate
         * kind makes mpdproto_verb() answer for the wrong one; neither
         * shows up as a failing command, only as a verb that quietly
         * stopped working. Checked through the public surface: every verb
         * this suite can name must round-trip.
         */
        static const char *const verbs[] = {
            "ping", "close", "commands", "notcommands", "tagtypes",
            "urlhandlers", "decoders", "status", "stats", "currentsong",
            "clearerror", "play", "playid", "pause", "stop", "next",
            "previous", "seek", "seekid", "seekcur", "setvol", "volume",
            "outputs", "replay_gain_mode", "replay_gain_status", "channels", "idle", "repeat", "random", "single",
            "consume", "add", "addid", "delete", "deleteid", "move",
            "moveid", "clear", "shuffle", "playlistinfo", "playlistid",
            "playlist", "plchanges", "plchangesposid", "lsinfo", "listall",
            "listallinfo", "find", "search", "list", "count", "update",
            "rescan", "listplaylists", "listplaylist", "listplaylistinfo",
            "load", "save", "rm",
            /* 5200 */
            "playlistadd", "playlistdelete",
            /* 5180 */
            "listpartitions", "partition", "newpartition", "delpartition",
            "moveoutput", "listmounts", "mount", "unmount", "listneighbors",
            /* 5218 */
            "getvol", "password", "crossfade",
            /* 5219 */
            "enableoutput", "disableoutput", "toggleoutput",
            /* 5220 */
            "swap", "swapid",
            /* 5221 */
            "findadd", "searchadd", "searchaddpl",
            /* 5222 */
            "playlistfind", "playlistsearch",
            /* 5223 */
            "playlistclear", "playlistmove", "rename",
            /* 5228 */
            "listfiles",
            /* 5230 */
            "subscribe", "unsubscribe", "readmessages", "sendmessage",
            /* 5231 */
            "prio", "prioid", "rangeid", "addtagid", "cleartagid", "readcomments", "mixrampdb", "mixrampdelay", "kill", "config", "sticker",
            /* 5240 */
            "albumart",
            /* 5249 */
            "readpicture", "binarylimit",
        };
        const size_t n = sizeof(verbs) / sizeof(verbs[0]);
        for (size_t i = 0; i < n; i++) {
            /* Give each the arguments it needs, so the arity check does
             * not mask the lookup: enough of them for any verb here. */
            char line[64];
            snprintf(line, sizeof(line), "%s a b c", verbs[i]);
            char *l = dup_line(line);
            mpd_cmd_t c;
            const bool r = mpdproto_parse(l, &c);
            /* Either it parsed, or it failed on ARITY and not on the
             * lookup -- an unknown verb here means the table lost one. */
            CHECK(r || c.ack == MPD_ACK_ARG,
                  "[%s] is not in the table", verbs[i]);
            if (r) {
                const char *const back = mpdproto_verb(c.kind);
                CHECK(back && strcmp(back, verbs[i]) == 0,
                      "[%s] round-tripped to [%s]", verbs[i], back ? back : "(null)");
            }
            free(l);
        }
        CHECK(mpdproto_verb(MPD_CMD_UNKNOWN) == NULL,
              "MPD_CMD_UNKNOWN named a verb");
    }

    /* ---- command lists ---------------------------------------------- */
    {
        mpd_list_t st;
        mpd_cmd_t c;
        char *l;

        memset(&st, 0, sizeof(st));

        /* `command_list_end` with no list open is what ACK_ERROR_NOT_LIST
         * exists for, and it is code 1. */
        l = dup_line("command_list_end");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_ERR, "a stray end was accepted");
        CHECK(c.ack == MPD_ACK_NOT_LIST, "a stray end gave ack %d", (int)c.ack);
        free(l);

        /* The plain form: no list_OK. */
        l = dup_line("command_list_begin");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_LIST_BEGIN, "begin not recognised");
        CHECK(st.active, "begin did not open the list");
        CHECK(!st.verbose, "the plain form asked for list_OK");
        free(l);

        l = dup_line("status");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_CMD, "a command in a list");
        CHECK(c.kind == MPD_CMD_STATUS, "the command in a list was lost");
        free(l);

        /* A nested begin is NOT nesting: it goes to the table, which does
         * not hold it, and comes back unknown -- which is what MPD
         * answers, because MPD has queued it as an ordinary line. */
        l = dup_line("command_list_begin");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_ERR, "a nested begin was accepted");
        CHECK(c.ack == MPD_ACK_UNKNOWN, "a nested begin gave ack %d", (int)c.ack);
        CHECK(st.active, "a nested begin closed the list");
        free(l);

        l = dup_line("command_list_end");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_LIST_END, "end not recognised");
        CHECK(!st.active, "end did not close the list");
        free(l);

        /* The _ok_ form differs in exactly one thing. */
        l = dup_line("command_list_ok_begin");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_LIST_BEGIN, "ok_begin not recognised");
        CHECK(st.active && st.verbose, "ok_begin did not ask for list_OK");
        free(l);
        l = dup_line("command_list_end");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_LIST_END, "ok form did not end");
        CHECK(!st.verbose, "end left list_OK on");
        free(l);

        /* A list verb with an argument is not a list verb: the three are
         * compared as whole lines, so this reaches the table. */
        memset(&st, 0, sizeof(st));
        l = dup_line("command_list_begin x");
        CHECK(mpdproto_line(l, &st, &c) == MPD_LINE_ERR, "begin with an argument opened a list");
        CHECK(!st.active, "begin with an argument opened a list");
        CHECK(c.ack == MPD_ACK_UNKNOWN, "begin with an argument gave ack %d", (int)c.ack);
        free(l);
    }

    /* ---- framing ---------------------------------------------------- */
    {
        char b[64];
        CHECK(mpdproto_ok(b, sizeof(b)) == 3 && strcmp(b, "OK\n") == 0,
              "OK is [%s]", b);
        CHECK(mpdproto_list_ok(b, sizeof(b)) == 8 && strcmp(b, "list_OK\n") == 0,
              "list_OK is [%s]", b);

        /* Never half a terminator: a client that reads "O" is
         * desynchronised for the life of the connection. */
        CHECK(mpdproto_ok(b, 3) == 0 && b[0] == '\0', "OK was truncated into 3 bytes");
        CHECK(mpdproto_ok(b, 4) == 3, "OK did not fit its exact size");
        CHECK(mpdproto_list_ok(b, 8) == 0 && b[0] == '\0', "list_OK was truncated");

        /*
         * The greeting, which clients gate features on. It must be
         * exactly "OK MPD <version>\n" -- a client that cannot read the
         * version treats the connection as not MPD at all -- and the
         * version must be the one constant, so that raising it raises the
         * greeting with it.
         */
        const char *const g = MPDPROTO_GREETING;
        const size_t gl = strlen(g);
        CHECK(strncmp(g, "OK MPD ", 7) == 0, "the greeting is [%s]", g);
        CHECK(gl > 8 && g[gl - 1] == '\n', "the greeting is not one line: [%s]", g);
        CHECK(strncmp(g + 7, MPDPROTO_VERSION, strlen(MPDPROTO_VERSION)) == 0,
              "the greeting does not carry MPDPROTO_VERSION: [%s]", g);
        CHECK(gl == 7 + strlen(MPDPROTO_VERSION) + 1,
              "the greeting carries something besides the version: [%s]", g);
    }

    {
        char b[128];
        size_t n;

        n = mpdproto_ack(MPD_ACK_ARG, 0, "add", "too few arguments", b, sizeof(b));
        CHECK(n == strlen(b) && strcmp(b, "ACK [2@0] {add} too few arguments\n") == 0,
              "ack is [%s]", b);

        /* The sub-command's index inside a command list is the whole
         * reason the list state is per connection. */
        n = mpdproto_ack(MPD_ACK_NO_EXIST, 3, "lsinfo", "No such directory", b, sizeof(b));
        CHECK(n && strcmp(b, "ACK [50@3] {lsinfo} No such directory\n") == 0,
              "ack with an index is [%s]", b);

        /* No verb: MPD writes an empty brace. */
        n = mpdproto_ack(MPD_ACK_UNKNOWN, 0, NULL, "No command given", b, sizeof(b));
        CHECK(n && strcmp(b, "ACK [5@0] {} No command given\n") == 0,
              "ack with no verb is [%s]", b);

        /*
         * A newline in the message would end the response early and leave
         * the rest to be read as the next one. Replaced, not escaped, and
         * not dropped -- the message stays the same length so a human
         * reading it still sees the words.
         */
        n = mpdproto_ack(MPD_ACK_NO_EXIST, 0, "add", "no such file\nOK\n", b, sizeof(b));
        CHECK(n && strcmp(b, "ACK [50@0] {add} no such file OK \n") == 0,
              "a newline survived the message: [%s]", b);
        CHECK(strstr(b, "\nOK") == NULL, "the message framed a fake OK");

        /* A tag or a path off a card can hold anything below 0x20. */
        n = mpdproto_ack(MPD_ACK_NO_EXIST, 0, "add", "a\tb\rc\x01\x7F", b, sizeof(b));
        CHECK(n && strcmp(b, "ACK [50@0] {add} a b c  \n") == 0,
              "control bytes survived: [%s]", b);

        /* Nothing above 0x7F is touched: a UTF-8 path arrives intact. */
        n = mpdproto_ack(MPD_ACK_NO_EXIST, 0, "add", "caf\xC3\xA9", b, sizeof(b));
        CHECK(n && strcmp(b, "ACK [50@0] {add} caf\xC3\xA9\n") == 0,
              "UTF-8 was mangled: [%s]", b);

        /* Too small is no ACK rather than an ACK with no newline. */
        for (size_t cap = 1; cap < 34; cap++) {
            char s[64];
            const size_t m = mpdproto_ack(MPD_ACK_ARG, 0, "add", "too few arguments",
                                          s, cap);
            CHECK(m == 0 && s[0] == '\0', "ack fitted %zu bytes it should not", cap);
        }
        CHECK(mpdproto_ack(MPD_ACK_ARG, 0, "add", "too few arguments", b, 35) ==
              strlen("ACK [2@0] {add} too few arguments\n"),
              "ack did not fit its exact size");
    }

    /* ---- malformed bytes, because a line comes off a socket ---------- */
    {
        /*
         * Not a corpus with expected answers -- there is nothing to
         * expect -- but a check that no input crashes the walks and that
         * a success is always self-consistent. The alphabet is the one
         * that matters: quotes, backslashes, separators, the apostrophe
         * that is neither, high bytes, and a NUL that ends the line
         * early.
         */
        static const char alpha[] = "ab \"\\'\t\r\x01\xC3\xA9/.0_\x7F";
        const size_t na = sizeof(alpha) - 1;
        unsigned seed = 20250927u;

        for (int iter = 0; iter < 40000; iter++) {
            char line[40];
            const size_t len = (size_t)(seed % (sizeof(line) - 1));
            for (size_t i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                line[i] = alpha[(seed >> 16) % na];
            }
            line[len] = '\0';
            seed = seed * 1103515245u + 12345u;

            char *l = dup_line(line);
            mpd_cmd_t c;
            const bool r = mpdproto_parse(l, &c);
            if (r) {
                checks++;
                if (c.argc < 0 || c.argc > MPDPROTO_MAX_ARGS ||
                    c.kind == MPD_CMD_UNKNOWN || !c.verb) {
                    failures++;
                    printf("FAIL %s:%d: incoherent success on [%s]\n",
                           __FILE__, __LINE__, line);
                } else {
                    for (int i = 0; i < c.argc; i++) {
                        if (!c.argv[i]) {
                            failures++;
                            printf("FAIL %s:%d: null arg %d on [%s]\n",
                                   __FILE__, __LINE__, i, line);
                            break;
                        }
                    }
                }
            } else {
                /* Every refusal carries something a client can read, and
                 * a code that is one of MPD's. */
                checks++;
                if (!c.err || c.err[0] == '\0' ||
                    (c.ack != MPD_ACK_UNKNOWN && c.ack != MPD_ACK_ARG)) {
                    failures++;
                    printf("FAIL %s:%d: bare refusal (ack %d) on [%s]\n",
                           __FILE__, __LINE__, (int)c.ack, line);
                }
                /* And the ACK it builds is always framed. */
                char s[256];
                const size_t m = mpdproto_ack_cmd(&c, 0, s, sizeof(s));
                if (m && (s[m - 1] != '\n' || strlen(s) != m)) {
                    checks++; failures++;
                    printf("FAIL %s:%d: unframed ack on [%s]\n",
                           __FILE__, __LINE__, line);
                }
            }
            free(l);
        }
    }

    /* ---- one key: value line ---------------------------------------- */
    {
        char b[256];
        size_t n;

        n = mpdproto_kv("Artist", "The Fall", b, sizeof(b));
        CHECK(n == strlen(b) && strcmp(b, "Artist: The Fall\n") == 0,
              "kv is [%s]", b);
        /* An empty value is a line with nothing after the space: the
         * caller decides whether to emit it, not this function. */
        CHECK(mpdproto_kv("Album", "", b, sizeof(b)) && strcmp(b, "Album: \n") == 0,
              "an empty value gave [%s]", b);

        /*
         * A NEWLINE IN A VALUE WOULD END THE LINE and the rest would be
         * read as another key -- a tag containing "\nOK\n" would end the
         * whole response early, which is the ACK builder's fault in a
         * different place. Replaced with a space, not escaped: MPD's
         * format has no escape, a value runs to the newline.
         */
        n = mpdproto_kv("Title", "one\ntwo", b, sizeof(b));
        CHECK(n && strcmp(b, "Title: one two\n") == 0,
              "a newline survived a value: [%s]", b);
        n = mpdproto_kv("Title", "x\nOK\n", b, sizeof(b));
        CHECK(n && strcmp(b, "Title: x OK \n") == 0, "value framing: [%s]", b);
        CHECK(strstr(b, "\nOK") == NULL, "a value framed a fake OK");
        n = mpdproto_kv("Title", "a\tb\rc\x01\x7F", b, sizeof(b));
        CHECK(n && strcmp(b, "Title: a b c  \n") == 0,
              "control bytes survived: [%s]", b);

        /*
         * Valid UTF-8 passes through untouched; invalid UTF-8 becomes
         * U+FFFD. A Latin-1 ID3 tag is invalid UTF-8 and 5127 found this
         * device has plenty of them, and an MPD client shows mojibake for
         * it or drops the response.
         */
        n = mpdproto_kv("Artist", "Caf\xC3\xA9", b, sizeof(b));
        CHECK(n && strcmp(b, "Artist: Caf\xC3\xA9\n") == 0,
              "valid UTF-8 was altered: [%s]", b);
        n = mpdproto_kv("Artist", "Caf\xE9", b, sizeof(b));   /* Latin-1 e-acute */
        CHECK(n && strcmp(b, "Artist: Caf\xEF\xBF\xBD\n") == 0,
              "a Latin-1 byte was not repaired: [%s]", b);
        /* The cases remoteproto_utf8_len() exists to get right, which a
         * second copy of it would have drifted on. */
        n = mpdproto_kv("A", "\xC0\xAF", b, sizeof(b));           /* overlong */
        CHECK(n && strcmp(b, "A: \xEF\xBF\xBD\xEF\xBF\xBD\n") == 0,
              "an overlong passed: [%s]", b);
        n = mpdproto_kv("A", "\xED\xA0\x80", b, sizeof(b));       /* surrogate */
        CHECK(n && strcmp(b, "A: \xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\n") == 0,
              "a surrogate passed: [%s]", b);
        n = mpdproto_kv("A", "\xE2\x82", b, sizeof(b));           /* truncated */
        CHECK(n && strcmp(b, "A: \xEF\xBF\xBD\xEF\xBF\xBD\n") == 0,
              "a truncated sequence passed: [%s]", b);

        /* Refusal rather than a half line. */
        CHECK(mpdproto_kv("Artist", "The Fall", b, 17) == 0 && b[0] == '\0',
              "kv truncated into 17 bytes");
        CHECK(mpdproto_kv("Artist", "The Fall", b, 18) == 17,
              "kv did not fit its exact size");
    }

    /* ---- one song --------------------------------------------------- */
    {
        char b[MPDPROTO_SONG_MAX];
        mpd_song_t s;

        /* A queue entry: everything known. MPD's order is file first,
         * then tags, then Time/duration, then Pos/Id -- and both Time and
         * duration are sent, with MPD's capitalisation. */
        memset(&s, 0, sizeof(s));
        s.uri = "Artist/Album/01 Song.flac";
        s.title = "Song"; s.artist = "Artist"; s.album = "Album";
        s.duration_ms = 245678;
        s.pos = 3; s.id = 17;
        CHECK(mpdproto_song(&s, b, sizeof(b)) &&
              strcmp(b,
                     "file: Artist/Album/01 Song.flac\n"
                     "Title: Song\n"
                     "Artist: Artist\n"
                     "Album: Album\n"
                     "Time: 246\n"
                     "duration: 245.678\n"
                     "Pos: 3\n"
                     "Id: 17\n") == 0, "song is [%s]", b);

        /*
         * A LIBRARY FILE HAS NO LENGTH AND NO POSITION. The catalog holds
         * no duration (mediacat.h), and a file in the library is not at a
         * position in the queue, so lsinfo emits neither -- absent, not
         * zero, because a client shows "Time: 0" as a zero-length track.
         */
        memset(&s, 0, sizeof(s));
        s.uri = "Artist/Album/01 Song.flac";
        s.title = "Song";
        s.duration_ms = -1;
        s.pos = -1;
        CHECK(mpdproto_song(&s, b, sizeof(b)) &&
              strcmp(b, "file: Artist/Album/01 Song.flac\n"
                        "Title: Song\n") == 0, "an lsinfo song is [%s]", b);

        /* An absent tag is absent, not an empty line: a client shows an
         * empty Artist as an artist named "", where a missing one falls
         * back to the filename. */
        memset(&s, 0, sizeof(s));
        s.uri = "a.flac"; s.artist = ""; s.title = NULL;
        s.duration_ms = -1; s.pos = -1;
        CHECK(mpdproto_song(&s, b, sizeof(b)) && strcmp(b, "file: a.flac\n") == 0,
              "empty tags were emitted: [%s]", b);

        /* Time rounds rather than truncates, as MPD's RoundS() does. */
        memset(&s, 0, sizeof(s));
        s.uri = "a.flac"; s.pos = -1;
        s.duration_ms = 12600;
        CHECK(mpdproto_song(&s, b, sizeof(b)) &&
              strstr(b, "Time: 13\n") && strstr(b, "duration: 12.600\n"),
              "12.6s gave [%s]", b);
        s.duration_ms = 12400;
        CHECK(mpdproto_song(&s, b, sizeof(b)) && strstr(b, "Time: 12\n"),
              "12.4s gave [%s]", b);
        /* Sub-second and exact-second, where the "%.3f" of an integer has
         * to keep its zeros or a client parses a different number. */
        s.duration_ms = 500;
        CHECK(mpdproto_song(&s, b, sizeof(b)) && strstr(b, "duration: 0.500\n"),
              "half a second gave [%s]", b);
        s.duration_ms = 7000;
        CHECK(mpdproto_song(&s, b, sizeof(b)) && strstr(b, "duration: 7.000\n"),
              "seven seconds gave [%s]", b);
        s.duration_ms = 61001;
        CHECK(mpdproto_song(&s, b, sizeof(b)) && strstr(b, "duration: 61.001\n"),
              "61.001s gave [%s]", b);

        /* Pos 0 is a position and must not be read as "none"; only a
         * negative omits it. */
        memset(&s, 0, sizeof(s));
        s.uri = "a.flac"; s.duration_ms = -1; s.pos = 0; s.id = 1;
        CHECK(mpdproto_song(&s, b, sizeof(b)) &&
              strstr(b, "Pos: 0\n") && strstr(b, "Id: 1\n"),
              "position zero was dropped: [%s]", b);

        /* No uri is no song. */
        memset(&s, 0, sizeof(s));
        s.duration_ms = -1; s.pos = -1;
        CHECK(mpdproto_song(&s, b, sizeof(b)) == 0, "a song with no uri was emitted");
        CHECK(mpdproto_song(NULL, b, sizeof(b)) == 0, "NULL was emitted");

        /*
         * The ceiling has to hold the worst case, or a legal track at the
         * far end of a card is unlistable and nothing says why: a maximum
         * uri and three maximum tags of bytes that each treble under the
         * repair.
         */
        {
            static char uri[MPDURI_MAX + 1];
            static char tag[65];
            memset(uri, 'a', sizeof(uri) - 1); uri[sizeof(uri) - 1] = '\0';
            memset(tag, '\xE9', sizeof(tag) - 1); tag[sizeof(tag) - 1] = '\0';
            memset(&s, 0, sizeof(s));
            s.uri = uri; s.title = tag; s.artist = tag; s.album = tag;
            s.duration_ms = 3599999; s.pos = 1023; s.id = 4294967295u;
            const size_t n = mpdproto_song(&s, b, sizeof(b));
            CHECK(n > 0, "the worst-case song did not fit MPDPROTO_SONG_MAX");
            CHECK(n < MPDPROTO_SONG_MAX, "the worst-case song exactly filled the buffer");
        }
    }

    /* ---- a directory ----------------------------------------------- */
    {
        char b[MPDPROTO_SONG_MAX];
        CHECK(mpdproto_directory("Artist/Album", b, sizeof(b)) &&
              strcmp(b, "directory: Artist/Album\n") == 0, "directory is [%s]", b);
        CHECK(mpdproto_directory(NULL, b, sizeof(b)) == 0, "NULL was a directory");
    }

    /* ---- status ----------------------------------------------------- */
    {
        char b[MPDPROTO_STATUS_MAX];
        mpd_status_t s;

        /*
         * Stopped. NO TIME FIELDS AT ALL: a stopped player has no elapsed
         * time, and reporting 0 puts a client's progress bar at the start
         * of a track it is not playing, which reads as paused.
         */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_STOP;
        s.volume = 40;
        s.playlist_version = 1;
        s.playlist_length = 0;
        s.song = -1; s.next_song = -1;
        s.elapsed_ms = -1; s.duration_ms = -1; s.bitrate = -1;
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strcmp(b,
                     "volume: 40\n"
                     "repeat: 0\n"
                     "random: 0\n"
                     "single: 0\n"
                     "consume: 0\n"
                     "playlist: 1\n"
                     "playlistlength: 0\n"
                     "state: stop\n") == 0, "stopped status is [%s]", b);
        CHECK(strstr(b, "elapsed") == NULL && strstr(b, "time:") == NULL,
              "a stopped player reported a time: [%s]", b);

        /* 5180: the partition, after consume, as MPD 0.22 orders it; and
         * absent for "" as for NULL. */
        s.partition = "default";
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strstr(b, "consume: 0\npartition: default\nplaylist: 1\n"),
              "partition not after consume: [%s]", b);
        s.partition = "";
        CHECK(mpdproto_status(&s, b, sizeof(b)) && !strstr(b, "partition"),
              "an empty partition was written: [%s]", b);
        s.partition = NULL;

        /* Playing, everything known. MPD's field order, and nextsong
         * last, which is where MPD puts it. */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_PLAY;
        s.volume = 100;
        s.repeat = true; s.random = false; s.single = true; s.consume = false;
        s.playlist_version = 9; s.playlist_length = 12;
        s.song = 3; s.songid = 44;
        s.next_song = 4; s.next_songid = 45;
        s.elapsed_ms = 12345; s.duration_ms = 245678;
        s.bitrate = 992;
        s.sample_rate = 44100; s.bits = 16; s.channels = 2;
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strcmp(b,
                     "volume: 100\n"
                     "repeat: 1\n"
                     "random: 0\n"
                     "single: 1\n"
                     "consume: 0\n"
                     "playlist: 9\n"
                     "playlistlength: 12\n"
                     "state: play\n"
                     "song: 3\n"
                     "songid: 44\n"
                     "time: 12:246\n"
                     "elapsed: 12.345\n"
                     "duration: 245.678\n"
                     "bitrate: 992\n"
                     "audio: 44100:16:2\n"
                     "nextsong: 4\n"
                     "nextsongid: 45\n") == 0, "playing status is [%s]", b);
        /* 5241: single oneshot is a word, not a number (MPD 0.21). */
        s.single_oneshot = true;
        CHECK(mpdproto_status(&s, b, sizeof(b)) && strstr(b, "single: oneshot\n") &&
              !strstr(b, "single: 1"), "oneshot status is [%s]", b);
        s.single_oneshot = false;

        s.state = MPD_STATE_PAUSE;
        CHECK(mpdproto_status(&s, b, sizeof(b)) && strstr(b, "state: pause\n"),
              "paused status is [%s]", b);
        /* A paused player still has an elapsed time; only a stopped one
         * does not. */
        CHECK(strstr(b, "elapsed: 12.345\n") != NULL,
              "a paused player lost its elapsed time");

        /* Unknown volume is omitted, which is what MPD does rather than
         * reporting 0 -- a client showing a slider at zero for "unknown"
         * is a client that will set it there. */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_STOP; s.volume = -1;
        s.song = -1; s.next_song = -1;
        s.elapsed_ms = -1; s.duration_ms = -1; s.bitrate = -1;
        CHECK(mpdproto_status(&s, b, sizeof(b)) && strstr(b, "volume") == NULL,
              "an unknown volume was reported: [%s]", b);
        /* Volume 0 is a volume, not "unknown". */
        s.volume = 0;
        CHECK(mpdproto_status(&s, b, sizeof(b)) && strstr(b, "volume: 0\n"),
              "volume zero was dropped: [%s]", b);

        /* Song 0 is a song. */
        s.song = 0; s.songid = 7;
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strstr(b, "song: 0\n") && strstr(b, "songid: 7\n"),
              "song zero was dropped: [%s]", b);

        /* A playing track of unknown length: time's total is 0 and there
         * is no duration line, which is how a stream reports. */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_PLAY; s.volume = 50;
        s.song = 0; s.next_song = -1;
        s.elapsed_ms = 5000; s.duration_ms = -1; s.bitrate = 128;
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strstr(b, "time: 5:0\n") && strstr(b, "duration") == NULL,
              "an unknown length gave [%s]", b);

        /* The reindex, and an error, both omitted when absent. */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_STOP; s.volume = -1;
        s.song = -1; s.next_song = -1;
        s.elapsed_ms = -1; s.duration_ms = -1; s.bitrate = -1;
        s.updating_db = 3; s.error = "something broke";
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strstr(b, "updating_db: 3\n") && strstr(b, "error: something broke\n"),
              "status with a job and an error is [%s]", b);
        /* And an error is a value like any other, so it cannot frame. */
        s.error = "broke\nOK\n";
        CHECK(mpdproto_status(&s, b, sizeof(b)) &&
              strstr(b, "error: broke OK \n") && strstr(b, "\nOK\n") == NULL,
              "an error framed a fake OK: [%s]", b);

        /* Fields this device does not have are absent rather than zero:
         * a field reporting 0 for a missing feature is a claim. */
        CHECK(strstr(b, "mixrampdb") == NULL && strstr(b, "xfade") == NULL &&
              strstr(b, "partition") == NULL, "status claimed a feature: [%s]", b);

        CHECK(mpdproto_status(NULL, b, sizeof(b)) == 0, "NULL was a status");

        /* Refusal rather than a status with a line missing from the
         * middle, which is worse than none: `over` is sticky. */
        memset(&s, 0, sizeof(s));
        s.state = MPD_STATE_PLAY; s.volume = 100;
        s.song = 3; s.songid = 44; s.next_song = 4; s.next_songid = 45;
        s.elapsed_ms = 12345; s.duration_ms = 245678; s.bitrate = 992;
        s.sample_rate = 44100; s.bits = 16; s.channels = 2;
        const size_t full = mpdproto_status(&s, b, sizeof(b));
        CHECK(full > 0, "the fullest status did not fit MPDPROTO_STATUS_MAX");
        for (size_t cap = 1; cap < full + 1; cap++) {
            char t[MPDPROTO_STATUS_MAX];
            CHECK(mpdproto_status(&s, t, cap) == 0 && t[0] == '\0',
                  "status fitted %zu of %zu bytes", cap, full);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
