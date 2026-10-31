/*
 * mpdidletest.c -- MPD's `idle`, as MPD 0.20 does it.
 *
 * Every expectation here is written from MPD's source -- IdleFlags.cxx,
 * ClientIdle.cxx, ClientProcess.cxx, OtherCommands.cxx's handle_idle,
 * and the idle_add() sites in player/, mixer/, db/update/ and Partition
 * -- and not from mpdidle.c, per texttest/README.md. The expected answers
 * are whole byte strings, spelled out, because the order of `changed:`
 * lines is a property a client-facing test should pin and a test that
 * builds its expectation with the same loop proves nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../main/mpdidle.h"
#include "../main/mpdproto.h"

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

/* Parse a space-separated list, as the tokeniser would hand it over. */
static bool parse(const char *words, uint32_t *mask, int *bad)
{
    static char buf[256];
    char *argv[16];
    int argc = 0;
    snprintf(buf, sizeof(buf), "%s", words);
    for (char *p = strtok(buf, " "); p && argc < 16; p = strtok(NULL, " "))
        argv[argc++] = p;
    return mpdidle_parse(argc, argv, mask, bad);
}

static void answer_is(mpd_idle_t *st, const char *want, const char *what)
{
    char out[MPDIDLE_ANSWER_MAX];
    const size_t n = mpdidle_answer(st, out, sizeof(out));
    CHECK(n == strlen(want) && memcmp(out, want, n) == 0,
          "%s: got \"%.*s\", want \"%s\"", what, (int)n, out, want);
}

static mpd_idle_view_t view(int state, uint32_t id, int32_t elapsed_ms)
{
    const mpd_idle_view_t v = {
        .state = state, .id = id, .version = 7, .volume = 40,
        .modes = 0, .updating = false, .elapsed_ms = elapsed_ms,
    };
    return v;
}

int main(void)
{
    /* ---- names and bits: MPD's IdleFlags, value for value ------------- */
    CHECK(MPD_IDLE_DATABASE == 0x1 && MPD_IDLE_STORED_PLAYLIST == 0x2 &&
          MPD_IDLE_PLAYLIST == 0x4 && MPD_IDLE_PLAYER == 0x8 &&
          MPD_IDLE_MIXER == 0x10 && MPD_IDLE_OUTPUT == 0x20 &&
          MPD_IDLE_OPTIONS == 0x40 && MPD_IDLE_STICKER == 0x80 &&
          MPD_IDLE_UPDATE == 0x100 && MPD_IDLE_SUBSCRIPTION == 0x200 &&
          MPD_IDLE_MESSAGE == 0x400 && MPD_IDLE_NEIGHBOR == 0x800 &&
          MPD_IDLE_MOUNT == 0x1000, "a bit value differs from IdleFlags.hxx");
    CHECK(mpdidle_name(MPD_IDLE_PLAYER) && strcmp(mpdidle_name(MPD_IDLE_PLAYER), "player") == 0,
          "player's name");
    CHECK(mpdidle_name(MPD_IDLE_STORED_PLAYLIST) &&
          strcmp(mpdidle_name(MPD_IDLE_STORED_PLAYLIST), "stored_playlist") == 0,
          "stored_playlist's name");
    CHECK(mpdidle_name(MPD_IDLE_NEIGHBOR) && strcmp(mpdidle_name(MPD_IDLE_NEIGHBOR), "neighbor") == 0,
          "neighbor is spelled MPD's way");
    CHECK(mpdidle_name(0) == NULL, "no name for zero");
    CHECK(mpdidle_name(MPD_IDLE_PLAYER | MPD_IDLE_MIXER) == NULL, "no name for two bits");
    CHECK(mpdidle_name(1u << 13) == NULL, "no fourteenth subsystem");

    /* ---- idle's arguments (handle_idle) ------------------------------- */
    {
        uint32_t m = 0;
        int bad = -1;
        CHECK(parse("", &m, &bad) && m == MPD_IDLE_ALL, "bare idle is everything, ~0");
        CHECK(parse("player", &m, &bad) && m == MPD_IDLE_PLAYER, "one name");
        CHECK(parse("player mixer playlist", &m, &bad) &&
              m == (MPD_IDLE_PLAYER | MPD_IDLE_MIXER | MPD_IDLE_PLAYLIST), "three names");
        CHECK(parse("PLAYER Mixer", &m, &bad) && m == (MPD_IDLE_PLAYER | MPD_IDLE_MIXER),
              "names are case-insensitive (StringEqualsCaseASCII)");
        CHECK(parse("player player", &m, &bad) && m == MPD_IDLE_PLAYER, "a repeat is harmless");
        CHECK(parse("sticker mount neighbor", &m, &bad) &&
              m == (MPD_IDLE_STICKER | MPD_IDLE_MOUNT | MPD_IDLE_NEIGHBOR),
              "subsystems this device never raises are still accepted");
        bad = -1;
        CHECK(!parse("player bogus mixer", &m, &bad) && bad == 1,
              "the first unknown name is reported by index (got %d)", bad);
        bad = -1;
        CHECK(!parse("players", &m, &bad) && bad == 0, "a prefix-plus is not a name");
        CHECK(!parse("playe", &m, &bad) && bad == 0, "a prefix is not a name");
        CHECK(!parse("pl\xc3\xa4yer", &m, &bad), "only ASCII folds");
    }

    /* ---- noidle as a raw line (ClientProcess.cxx) --------------------- */
    CHECK(mpdidle_is_noidle("noidle"), "noidle");
    CHECK(mpdidle_is_noidle("noidle\r"), "a CRLF client's noidle (StripRight)");
    CHECK(mpdidle_is_noidle("noidle  \t "), "trailing whitespace");
    CHECK(!mpdidle_is_noidle(" noidle"), "leading space is not stripped");
    CHECK(!mpdidle_is_noidle("noidle x"), "an argument makes it a (missing) verb");
    CHECK(!mpdidle_is_noidle("NOIDLE"), "the raw compare is case-sensitive");
    CHECK(!mpdidle_is_noidle("noidl"), "short");
    CHECK(!mpdidle_is_noidle("noidlex"), "long");
    CHECK(!mpdidle_is_noidle(""), "blank");

    /* ---- the latch ---------------------------------------------------- */
    {
        mpd_idle_t st;
        mpdidle_init(&st);

        /* A change while NOT idling is kept, and delivered on the next
         * idle -- the stale-queue-forever failure MPD.md names. */
        CHECK(!mpdidle_add(&st, MPD_IDLE_PLAYLIST), "not idling: nothing to send yet");
        CHECK(mpdidle_wait(&st, MPD_IDLE_ALL), "a latched change answers idle at once");
        answer_is(&st, "changed: playlist\nOK\n", "latched playlist");
        CHECK(!st.waiting, "answering leaves the wait");

        /* Idle with nothing pending waits; a change then wakes it. */
        CHECK(!mpdidle_wait(&st, MPD_IDLE_ALL), "nothing pending: wait silently");
        CHECK(st.waiting, "waiting");
        CHECK(mpdidle_add(&st, MPD_IDLE_PLAYER), "a change wakes an idle client");
        answer_is(&st, "changed: player\nOK\n", "woken by player");

        /* BIT order, not arrival order. */
        mpdidle_add(&st, MPD_IDLE_UPDATE);
        mpdidle_add(&st, MPD_IDLE_MIXER);
        mpdidle_add(&st, MPD_IDLE_DATABASE);
        mpdidle_add(&st, MPD_IDLE_PLAYER);
        CHECK(mpdidle_wait(&st, MPD_IDLE_ALL), "four pending");
        answer_is(&st, "changed: database\nchanged: player\nchanged: mixer\nchanged: update\nOK\n",
                  "IdleFlags order");

        /* A subscription filters what is SAID and what WAKES. */
        CHECK(!mpdidle_wait(&st, MPD_IDLE_PLAYER), "subscribe to player only");
        CHECK(!mpdidle_add(&st, MPD_IDLE_MIXER), "mixer does not wake a player-only idle");
        CHECK(st.waiting, "still waiting");
        CHECK(mpdidle_add(&st, MPD_IDLE_PLAYER), "player does");
        answer_is(&st, "changed: player\nOK\n", "mixer is not named to a player-only client");

        /* ...and the unsubscribed mixer was DISCARDED with it, which is
         * MPD's `idle_flags = 0` and not the obvious design. */
        CHECK(!mpdidle_wait(&st, MPD_IDLE_ALL), "mixer did not survive the answer");
        CHECK(mpdidle_add(&st, MPD_IDLE_OPTIONS), "options wakes");
        answer_is(&st, "changed: options\nOK\n", "only options");

        /* noidle while waiting: a bare OK, and pending events KEPT. */
        char out[16];
        CHECK(!mpdidle_wait(&st, MPD_IDLE_PLAYER), "idle player");
        mpdidle_add(&st, MPD_IDLE_MIXER);
        size_t n = mpdidle_noidle(&st, out, sizeof(out));
        CHECK(n == 3 && memcmp(out, "OK\n", 3) == 0, "noidle answers OK");
        CHECK(!st.waiting, "and leaves the wait");
        CHECK(mpdidle_wait(&st, MPD_IDLE_ALL), "mixer is still latched after noidle");
        answer_is(&st, "changed: mixer\nOK\n", "kept through noidle");

        /* noidle while NOT waiting: no response at all. */
        n = mpdidle_noidle(&st, out, sizeof(out));
        CHECK(n == 0, "noidle outside idle says nothing (got %zu bytes)", n);
        CHECK(!st.waiting, "and changes nothing");

        /* A buffer too small leaves the latch alone. */
        mpdidle_add(&st, MPD_IDLE_PLAYER);
        mpdidle_wait(&st, MPD_IDLE_ALL);
        char tiny[8];
        CHECK(mpdidle_answer(&st, tiny, sizeof(tiny)) == 0, "too small: 0");
        CHECK(st.waiting && (st.pending & MPD_IDLE_PLAYER), "too small: latch untouched");
        answer_is(&st, "changed: player\nOK\n", "then answered properly");

        /* The worst case fits the stated maximum. */
        mpdidle_add(&st, 0x1FFFu);
        mpdidle_wait(&st, MPD_IDLE_ALL);
        char big[MPDIDLE_ANSWER_MAX];
        n = mpdidle_answer(&st, big, sizeof(big));
        CHECK(n > 0 && n <= MPDIDLE_ANSWER_MAX, "all thirteen fit (%zu)", n);
        CHECK(n >= 3 && memcmp(big + n - 3, "OK\n", 3) == 0, "and end in OK");

        /* A new connection hears nothing from before it arrived. */
        mpd_idle_t fresh;
        memset(&fresh, 0xAB, sizeof(fresh));
        mpdidle_init(&fresh);
        CHECK(!mpdidle_wait(&fresh, MPD_IDLE_ALL), "a fresh connection has nothing pending");
    }

    /* ---- what counts as a change -------------------------------------- */
    {
        mpd_idle_track_t t;
        memset(&t, 0, sizeof(t));
        const int64_t s = 1000000;     /* a second, in us */
        mpd_idle_view_t v = view(MPD_STATE_PLAY, 5, 10000);

        CHECK(mpdidle_changes(&t, &v, 100 * s, false, false) == 0,
              "the first view reports nothing");

        /* Playing normally: a second later, a second on. */
        v.elapsed_ms = 11000;
        CHECK(mpdidle_changes(&t, &v, 101 * s, false, false) == 0, "a normal tick");
        /* Passes faster than the position's resolution: same second. */
        CHECK(mpdidle_changes(&t, &v, 101 * s + 40000, false, false) == 0, "40 ms, same second");
        v.elapsed_ms = 12000;
        CHECK(mpdidle_changes(&t, &v, 101 * s + 80000, false, false) == 0,
              "the second ticks over 80 ms after the pass before");

        /* A seek forward and back. */
        v.elapsed_ms = 60000;
        CHECK(mpdidle_changes(&t, &v, 101 * s + 120000, false, false) == MPD_IDLE_PLAYER,
              "a forward jump is a seek");
        v.elapsed_ms = 3000;
        CHECK(mpdidle_changes(&t, &v, 101 * s + 160000, false, false) == MPD_IDLE_PLAYER,
              "a backward jump is a seek");

        /* A STALL is not a seek, however long, pass to pass. */
        int64_t now = 102 * s;
        mpdidle_changes(&t, &v, now, false, false);
        uint32_t any = 0;
        for (int i = 0; i < 300; i++) {     /* 30 s of 100 ms passes, stuck */
            now += 100000;
            any |= mpdidle_changes(&t, &v, now, false, false);
        }
        CHECK(any == 0, "a 30 s stall raised 0x%x", (unsigned)any);

        /* A long gap between views (ui_task skipped publishing while a
         * page was open) with the position keeping pace is not a seek. */
        v.elapsed_ms += 30000;
        now += 30 * s;
        CHECK(mpdidle_changes(&t, &v, now, false, false) == 0, "30 s gap, 30 s on");

        /* Paused: any movement is a seek; standing still is nothing. */
        v.state = MPD_STATE_PAUSE;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_PLAYER,
              "pausing is player");
        CHECK(mpdidle_changes(&t, &v, now += 5 * s, false, false) == 0,
              "paused, still: nothing however long");
        v.elapsed_ms += 1000;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_PLAYER,
              "paused, moved by a second: a seek");
        v.state = MPD_STATE_PLAY;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_PLAYER,
              "resuming is player");

        /* A new song is player, and its version move is playlist. */
        v.id = 6;
        v.version = 8;
        v.elapsed_ms = 0;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) ==
              (MPD_IDLE_PLAYER | MPD_IDLE_PLAYLIST), "new song: player and playlist");

        /* A stream's tags changing in place: the same id, a version
         * move, and PLAYER as well -- MPD raises both. */
        v.version = 9;
        CHECK(mpdidle_changes(&t, &v, now += 100000, true, false) ==
              (MPD_IDLE_PLAYER | MPD_IDLE_PLAYLIST), "ICY title: player and playlist");

        /* The rest, one at a time. */
        v.volume = 41;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_MIXER, "volume");
        v.modes = 2;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_OPTIONS, "modes");
        v.updating = true;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_UPDATE,
              "a reindex starting is update");
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == 0, "and running is not");
        v.updating = false;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, true) ==
              (MPD_IDLE_UPDATE | MPD_IDLE_DATABASE), "ending with changes: update and database");
        v.updating = true;
        mpdidle_changes(&t, &v, now += 100000, false, false);
        v.updating = false;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_UPDATE,
              "ending with no changes: update only (RunDeferred's `if (modified)`)");

        /* Stopped: no position, and nothing to seek. */
        v.state = MPD_STATE_STOP;
        v.id = 0;
        v.elapsed_ms = -1;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_PLAYER, "stop");
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == 0, "stopped stays quiet");

        /* A position appearing on a playing song -- a stream's first
         * stats -- is new player state. */
        v.state = MPD_STATE_PLAY;
        v.id = 7;
        v.version = 10;
        mpdidle_changes(&t, &v, now += 100000, false, false);
        v.elapsed_ms = 0;
        CHECK(mpdidle_changes(&t, &v, now += 100000, false, false) == MPD_IDLE_PLAYER,
              "a position appearing");

        /* OUTPUT is never raised: nothing here can change it. */
        CHECK(!(mpdidle_changes(&t, &v, now += 100000, true, true) & MPD_IDLE_OUTPUT),
              "output is never raised");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
