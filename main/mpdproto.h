/*
 * mpdproto.h -- the MPD protocol's grammar, with no network code in it.
 *
 * MPD.md step 8, first half. Split from the mpd.c that will own the
 * socket for the reason remoteproto.c is split from remote.c: every byte
 * read here came from anyone on the network, so it is tested on a host
 * under ASan and UBSan (texttest/mpdprototest.c) rather than only on a
 * board.
 *
 * WHAT IS HERE: the greeting, the tokeniser, the command table with its
 * arities, OK/ACK framing, and the command-list state machine. What is
 * NOT here is any response body -- status, currentsong, playlistinfo and
 * lsinfo are serialisers over player state and come next -- and any
 * decision about what a verb DOES. This file answers "is that a command,
 * and what are its arguments", and nothing else.
 *
 * WHY THE TOKENISER IS NOT remoteproto_parse(). The remote page's
 * grammar is one verb and at most one word (remoteproto.h:12-21); MPD
 * quotes arguments with `"` and escapes with `\` inside them, so a path
 * containing a space is sayable and the parse is no longer a memcmp.
 * The rules below are MPD's own, from src/util/Tokenizer.cxx, and are
 * transcribed deliberately rather than approximated: a client that sends
 * a legal line and gets an ACK looks like a broken server, and the
 * clients that send quoted paths are the ordinary ones.
 *
 * TOKENISING IS IN PLACE, which is also MPD's choice. The verb and every
 * argument point into the caller's line buffer, which is written to --
 * separators become NUL and a quoted argument is unescaped over itself,
 * which only ever shortens it. That buffer belongs to the caller and per
 * CLAUDE.md it does not go on a task stack: MPDPROTO_LINE_MAX is 2048.
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
 * THE VERSION TO CLAIM, in one place, as MPD.md asks -- clients gate
 * features on the greeting, and claiming a version then ACKing half of
 * what it implies is worse than claiming a lower one.
 *
 * 0.20.0 is the lowest that covers the planned verb list. What forces it
 * upward, in order: `idle` is 0.14, `single` and `consume` are 0.15
 * (MPD's doc/protocol.rst footnotes), and `status`'s `duration` line --
 * fractional seconds, which every modern client prefers to `time` -- is
 * 0.20. Nothing planned needs more.
 *
 * WHAT THE CEILING IS FOR. 0.21 is deliberately not claimed, because two
 * things arrive with it that this device will not do: `albumart`, which a
 * client that believes in it will ask for, and the filter-expression form
 * of `find`/`search` -- `find "(Artist == \"x\")"` -- as against the
 * `find TYPE VALUE` form MPD.md step 12 plans. Raising this number means
 * checking both, not just adding a verb.
 *
 * Revisit when step 12 closes the verb list; until then it is the number
 * the table below justifies.
 */
#define MPDPROTO_VERSION    "0.20.0"

/* The first thing written to a new connection, newline included. */
#define MPDPROTO_GREETING   "OK MPD " MPDPROTO_VERSION "\n"

/*
 * The longest line worth reading. A path is MPDQ_PATH_MAX (512), every
 * byte of it can need a backslash inside quotes, and a verb and two
 * quotes sit in front -- so a legal line can approach 1040 and 2048 is
 * the round number above it. The buffer is the caller's; see the file
 * comment about where it may not live.
 */
#define MPDPROTO_LINE_MAX   (2048)

/*
 * How many arguments one line may carry.
 *
 * MPD HAS A CEILING TOO, and it is worth copying its shape rather than
 * picking a round number: `COMMAND_ARGV_MAX = 2 + TAG_NUM_OF_ITEM_TYPES
 * * 2` (src/command/AllCommands.cxx:49). The widest line in the protocol
 * is a `find`/`search`, which is a TYPE VALUE pair per tag type it can
 * filter on, plus a trailing pair such as `window 0:10`. So the bound is
 * a function of how many tags are searchable, and MPD's is large because
 * MPD has thirty-odd.
 *
 * This device has four: the fields the search file carries -- title,
 * artist, album and path (MEDIA-INDEX.md point 2, and the format in
 * 5129-5138). So the same formula gives ten, and
 * `search Artist a Album b Title c file d window 0:10` is exactly ten.
 * Raising the searchable tag count raises this, which is the point of
 * writing it as the formula.
 */
#define MPDPROTO_TAG_TYPES  (4)
#define MPDPROTO_MAX_ARGS   (2 + MPDPROTO_TAG_TYPES * 2)

/*
 * MPD's ACK codes, with MPD's values (src/protocol/Ack.hxx).
 *
 * MPD.md says "the five ACK error codes", which is the low block and not
 * the whole enum: there are twelve, in two ranges, and the high ones are
 * the ones a queue and a card raise -- NO_EXIST for a path that is not
 * there, PLAYLIST_MAX for a queue at MPDQ_MAX. All twelve are named here
 * because a client reads the number, so a wrong one is a client
 * reporting the wrong fault; the ones this device never raises cost a
 * line of enum each.
 */
typedef enum {
    MPD_ACK_NOT_LIST        = 1,
    MPD_ACK_ARG             = 2,
    MPD_ACK_PASSWORD        = 3,
    MPD_ACK_PERMISSION      = 4,
    MPD_ACK_UNKNOWN         = 5,
    MPD_ACK_NO_EXIST        = 50,
    MPD_ACK_PLAYLIST_MAX    = 51,
    MPD_ACK_SYSTEM          = 52,
    MPD_ACK_PLAYLIST_LOAD   = 53,
    MPD_ACK_UPDATE_ALREADY  = 54,
    MPD_ACK_PLAYER_SYNC     = 55,
    MPD_ACK_EXIST           = 56,
} mpd_ack_t;

/*
 * The verbs. Every one of these is in the table; whether it is ANSWERED
 * is mpd.c's business and arrives over MPD.md steps 9-12, so a verb here
 * with no handler yet is answered with an ACK by the dispatcher rather
 * than by being missing from the grammar. The comment on each names the
 * step it belongs to, so the table reads as the plan.
 */
typedef enum {
    MPD_CMD_UNKNOWN = 0,

    /* step 9: the connection, state, transport, volume */
    MPD_CMD_PING, MPD_CMD_CLOSE, MPD_CMD_COMMANDS, MPD_CMD_NOTCOMMANDS,
    MPD_CMD_TAGTYPES, MPD_CMD_URLHANDLERS, MPD_CMD_DECODERS,
    MPD_CMD_STATUS, MPD_CMD_STATS, MPD_CMD_CURRENTSONG, MPD_CMD_CLEARERROR,
    MPD_CMD_PLAY, MPD_CMD_PLAYID, MPD_CMD_PAUSE, MPD_CMD_STOP,
    MPD_CMD_NEXT, MPD_CMD_PREVIOUS,
    MPD_CMD_SEEK, MPD_CMD_SEEKID, MPD_CMD_SEEKCUR,
    MPD_CMD_SETVOL, MPD_CMD_VOLUME,
    MPD_CMD_OUTPUTS,

    /* step 10 */
    MPD_CMD_IDLE, MPD_CMD_NOIDLE,

    /* step 7's four modes, asked over step 11's transport */
    MPD_CMD_REPEAT, MPD_CMD_RANDOM, MPD_CMD_SINGLE, MPD_CMD_CONSUME,

    /* step 11: the queue */
    MPD_CMD_ADD, MPD_CMD_ADDID, MPD_CMD_DELETE, MPD_CMD_DELETEID,
    MPD_CMD_MOVE, MPD_CMD_MOVEID, MPD_CMD_CLEAR, MPD_CMD_SHUFFLE,
    MPD_CMD_PLAYLISTINFO, MPD_CMD_PLAYLISTID, MPD_CMD_PLAYLIST,
    MPD_CMD_PLCHANGES, MPD_CMD_PLCHANGESPOSID,

    /* step 12: browsing and search */
    MPD_CMD_LSINFO, MPD_CMD_LISTALL, MPD_CMD_LISTALLINFO,
    MPD_CMD_FIND, MPD_CMD_SEARCH, MPD_CMD_LIST, MPD_CMD_COUNT,
    MPD_CMD_UPDATE, MPD_CMD_RESCAN,

    /* step 13, optional: stored playlists */
    MPD_CMD_LISTPLAYLISTS, MPD_CMD_LISTPLAYLIST, MPD_CMD_LISTPLAYLISTINFO,
    MPD_CMD_LOAD, MPD_CMD_SAVE, MPD_CMD_RM,
} mpd_cmd_kind_t;

/*
 * One tokenised line.
 *
 * `verb` and `argv` point INTO the caller's buffer and are NUL-
 * terminated there. They are valid until that buffer is reused, which is
 * the next line read -- no copy is made and none is needed, because
 * mpd.c acts on a command before reading the next one.
 *
 * On failure `kind` is MPD_CMD_UNKNOWN and the four `err*` fields carry
 * what an ACK needs. They are split that way because MPD's messages
 * quote the verb -- `unknown command "foo"`, `too few arguments for
 * "add"` -- so the wording alone is not the message, and building the
 * whole string here would mean a buffer in this struct. Instead `err` is
 * a static string that never came off the wire, and `err_quote` is the
 * one piece that did; mpdproto_ack_cmd() puts them together.
 *
 * `err_cmd` is the ACK's {current_command}, and it is NOT always the
 * verb: MPD sets it only after the lookup succeeds, so an unknown verb
 * gives `{}` with the name in the message, and an arity fault gives
 * `{add}` and names it again in the message. Both are MPD's, and a
 * client that prints the brace shows the difference.
 */
typedef struct {
    mpd_cmd_kind_t kind;
    char          *verb;                        /* NULL on a blank line */
    int            argc;
    char          *argv[MPDPROTO_MAX_ARGS];
    mpd_ack_t      ack;
    const char    *err;         /* static wording, no trailing detail */
    const char    *err_quote;   /* quoted after err, or NULL; into the line */
    const char    *err_cmd;     /* {current_command}, or NULL for {} */
} mpd_cmd_t;

/*
 * Tokenise one line, MPD's rules exactly (src/util/Tokenizer.cxx):
 *
 *   - THE VERB is ASCII alpha first, then alnum or '_'. Not a digit, not
 *     a dash: `2play` and `play-now` are both "Invalid word character".
 *   - AN ARGUMENT is quoted when it starts with '"', and then runs to
 *     the next unescaped '"', with '\' escaping whatever follows it --
 *     any byte, not a named set. The closing quote must be followed by
 *     whitespace or end of line, so `"a"b` is an error and not two
 *     tokens.
 *   - AN UNQUOTED argument is any run of bytes above 0x20 except '"' and
 *     '\''. Above 0x20 includes every UTF-8 continuation byte, so a path
 *     with accents needs no quoting. A bare apostrophe does: MPD accepts
 *     '\'' as a quote character only inside a filter expression, never
 *     here, so `don't` is "Invalid unquoted character" rather than an
 *     unterminated string.
 *   - SEPARATORS are bytes 0x01..0x20, so a tab separates and a trailing
 *     "\r" left by a CRLF client is stripped rather than becoming part
 *     of the last argument.
 *
 * `line` is NUL-terminated and WRITABLE, with the newline already
 * removed by the caller. Returns false on a line that is not a command,
 * with `out->ack` and `out->err` set; a blank line is false with
 * MPD_ACK_UNKNOWN and "No command given", which is also the line MPD
 * closes the connection over.
 *
 * A TOKENISER FAILURE IS MPD_ACK_UNKNOWN, NOT MPD_ACK_ARG, which reads
 * backwards for something that is plainly a syntax error and is what MPD
 * sends: a bad verb goes through `r.Error(ACK_ERROR_UNKNOWN, e.what())`
 * and a bad argument through the generic handler to the same code
 * (src/command/AllCommands.cxx:401, :436). MPD_ACK_ARG is reserved for
 * the two things it means there -- "Too many arguments" and an arity
 * mismatch. Getting this round the wrong way makes a client report a bad
 * argument where the line was not parseable at all.
 *
 * MPD also CLOSES the connection on both of these, on the grounds that
 * the peer is not speaking the protocol. Whether to do that is mpd.c's
 * call; the code is the same either way.
 *
 * This does NOT look the verb up; mpdproto_parse() does both.
 */
bool mpdproto_tokenise(char *line, mpd_cmd_t *out);

/*
 * Tokenise, look the verb up, and check its arity. Returns false with
 * `out->ack`/`out->err` set for an unknown verb (MPD_ACK_UNKNOWN,
 * `unknown command`) or the wrong number of arguments (MPD_ACK_ARG, and
 * MPD's three different messages -- "wrong number of", "too few", "too
 * many" -- which differ by whether the verb takes a fixed count).
 *
 * The arities are MPD's, from src/command/AllCommands.cxx, EXCEPT where
 * this device claims less than MPD does: `add` is 1..1 here and 1..2
 * there, because the second argument is a 0.24 insert position and
 * MPDPROTO_VERSION claims 0.20. An arity looser than the claim invites a
 * client to send something the dispatcher will only ACK later, with a
 * worse message.
 */
bool mpdproto_parse(char *line, mpd_cmd_t *out);

/* The verb for a kind, for `commands` and for an ACK's {current_command}.
 * NULL for MPD_CMD_UNKNOWN. Static storage. */
const char *mpdproto_verb(mpd_cmd_kind_t kind);

/* ---- command lists ------------------------------------------------- */

/*
 * The command-list state for one connection.
 *
 * `command_list_begin` and `command_list_ok_begin` differ in one thing:
 * the second sends `list_OK` after each sub-command that succeeded, so a
 * client can tell which answer belongs to which command. Both send one
 * `OK` at the end and both halt at the first failure, whose ACK carries
 * the failing sub-command's 0-based index in the [error@N] field -- that
 * index is the whole reason this state is per connection rather than a
 * bool.
 *
 * WHETHER THE LIST IS ACCUMULATED OR RUN AS IT ARRIVES IS mpd.c's
 * DECISION, not this file's, and it is not a free one: MPD reads every
 * line into a vector and runs them at `command_list_end`, which needs a
 * buffer whose size a client chooses. Running each as it is read costs
 * nothing and is equivalent for a client that finishes the list --
 * MPD halts at the first failure having already run what came before,
 * so the effects are the same -- and differs only when the connection
 * breaks mid-list, where MPD would have run nothing. That tradeoff
 * belongs in mpd.c's comment when it makes it.
 */
typedef struct {
    bool active;    /* between a begin and its end */
    bool verbose;   /* the _ok_ form: list_OK after each */
    int  index;     /* 0-based, for [error@N] */
} mpd_list_t;

/* What a line turned out to be. */
typedef enum {
    MPD_LINE_CMD = 0,       /* run out->kind; st->index is its [error@N] */
    MPD_LINE_LIST_BEGIN,    /* nothing to run; st is updated */
    MPD_LINE_LIST_END,      /* the list is over: answer OK */
    MPD_LINE_ERR,           /* ACK with out->ack and out->err */
} mpd_line_t;

/*
 * Classify one line against `st`, parsing it when it is a command.
 *
 * The three list verbs are handled here and never reach the command
 * table, which is where MPD keeps them too. A `command_list_end` with no
 * list open is MPD_LINE_ERR with MPD_ACK_NOT_LIST -- the code that
 * exists for exactly this -- and a `command_list_begin` inside a list is
 * not nesting: it goes to the table, which does not hold it, and comes
 * back `unknown command`, which is what MPD answers.
 *
 * `st` is zeroed by the caller for a new connection.
 */
mpd_line_t mpdproto_line(char *line, mpd_list_t *st, mpd_cmd_t *out);

/* ---- framing ------------------------------------------------------- */

/*
 * The three terminators. Each writes a NUL-terminated string and returns
 * its length, or 0 when it did not fit -- never a partial line, because
 * half a terminator desynchronises a client for the life of the
 * connection.
 */
size_t mpdproto_ok(char *out, size_t cap);
size_t mpdproto_list_ok(char *out, size_t cap);

/*
 * ACK [error@command_listNum] {current_command} message_text
 *
 * `cmd` may be NULL or empty, which gives `{}` as MPD does on a line
 * with no readable verb. `text` is a message; it may be NULL.
 *
 * BOTH ARE SANITISED, and that is not defensive habit. A newline in
 * either would end the response early and leave the rest of the message
 * to be read as the next one, which is a desynchronised client rather
 * than a bad error string -- and a `text` that names a path carries
 * whatever bytes were on the card. Bytes below 0x20 and 0x7F become
 * spaces; nothing above is touched, because a UTF-8 path should arrive
 * intact and MPD does not escape one here either.
 *
 * Returns the length, or 0 when it did not fit.
 */
size_t mpdproto_ack(mpd_ack_t code, int list_num, const char *cmd,
                    const char *text, char *out, size_t cap);

/*
 * The ACK for a line that mpdproto_parse() or mpdproto_line() refused,
 * assembled from the fields it set: MPD's wording, the verb quoted after
 * it when MPD quotes one, and {current_command} filled only when MPD
 * fills it. `list_num` is the sub-command's index inside a command list
 * and 0 outside one.
 *
 * This is the call mpd.c should make on a false return. mpdproto_ack()
 * is the primitive under it, for the errors a handler raises later --
 * MPD_ACK_NO_EXIST on a path, MPD_ACK_PLAYLIST_MAX on a full queue --
 * which name things this file knows nothing about.
 */
size_t mpdproto_ack_cmd(const mpd_cmd_t *c, int list_num,
                        char *out, size_t cap);

#ifdef __cplusplus
}
#endif
