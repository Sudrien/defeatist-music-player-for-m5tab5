/*
 * mpdqueuetest.c -- mpdqueue.c: ids, order, and the version.
 *
 * The version checks are written from MPD's own rule -- "plchanges V
 * returns the songs whose position or content changed after V" -- and
 * not from what the implementation happens to stamp, which is
 * texttest/README.md's rule. That distinction is the whole point here:
 * the bug this file exists to catch is stamping only the entry named
 * and not the ones it shifted, and a test written by watching the code
 * would bless exactly that.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mpdqueue.h"

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

/* The queue's paths, joined, so a whole order is one comparison. */
static void order(char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0; i < mpdq_count(); i++) {
        if (i) strncat(out, "|", n - strlen(out) - 1);
        strncat(out, mpdq_path(i), n - strlen(out) - 1);
    }
}

/* What plchanges(v) should return: the positions whose entry version is
 * later than v. Written as MPD's question, asked of the module. */
static void changed_since(uint32_t v, char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0; i < mpdq_count(); i++) {
        if (mpdq_entry_version(i) > v) {
            char b[32];
            snprintf(b, sizeof(b), "%s%d", out[0] ? "," : "", i);
            strncat(out, b, n - strlen(out) - 1);
        }
    }
}

static void fill(const char *const *paths, int n)
{
    mpdq_clear();
    for (int i = 0; i < n; i++) CHECK(mpdq_append(paths[i], NULL) == i,
                                     "append %d", i);
}

int main(void)
{
    char got[2048], ch[512];

    CHECK(mpdq_init(), "init failed");
    CHECK(mpdq_init(), "init was not idempotent");
    CHECK(mpdq_count() == 0, "a new queue holds %d", mpdq_count());

    /* ---- adding ----------------------------------------------------- */
    {
        uint32_t a = 0, b = 0, c = 0;
        CHECK(mpdq_append("one.flac", &a) == 0, "first append");
        CHECK(mpdq_append("two.flac", &b) == 1, "second append");
        CHECK(mpdq_insert(0, "zero.flac", &c) == 0, "insert at the front");

        order(got, sizeof(got));
        CHECK(strcmp(got, "zero.flac|one.flac|two.flac") == 0,
              "order is \"%s\"", got);

        CHECK(a && b && c, "an id came back 0");
        CHECK(a != b && b != c && a != c, "ids repeated: %u %u %u", a, b, c);

        /* Insert at count is "at the end", which is what append is. */
        CHECK(mpdq_insert(mpdq_count(), "end.flac", NULL) == 3, "insert at end");
        CHECK(mpdq_insert(99, "nope.flac", NULL) == -1,
              "a position past the end was accepted");
        CHECK(mpdq_insert(-1, "nope.flac", NULL) == -1,
              "a negative position was accepted");
        CHECK(mpdq_append("", NULL) == -1, "an empty path was accepted");
        CHECK(mpdq_append(NULL, NULL) == -1, "a null path was accepted");

        char toolong[MPDQ_PATH_MAX + 8];
        memset(toolong, 'a', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = '\0';
        CHECK(mpdq_append(toolong, NULL) == -1, "an over-long path was accepted");
    }

    /* ---- ids survive what happens around them ----------------------- */
    {
        static const char *const p[] = { "a", "b", "c", "d", "e" };
        fill(p, 5);

        const uint32_t id_c = mpdq_id(2);
        CHECK(mpdq_find_id(id_c) == 2, "find_id did not agree with id");

        /* A neighbour goes: c moves, its id does not change. */
        CHECK(mpdq_remove(0), "remove 0");
        CHECK(mpdq_find_id(id_c) == 1, "after a removal c is at %d",
              mpdq_find_id(id_c));
        CHECK(mpdq_id(1) == id_c, "c's id changed when a neighbour went");

        /* And no new entry inherits it. */
        uint32_t fresh = 0;
        mpdq_append("f", &fresh);
        CHECK(fresh != id_c, "a new entry reused a live id");

        /* A removed id is gone and is not handed out again. */
        const uint32_t id_b = mpdq_id(0);
        CHECK(mpdq_remove_id(id_b), "remove_id");
        CHECK(mpdq_find_id(id_b) == -1, "a removed id is still findable");
        uint32_t after = 0;
        mpdq_append("g", &after);
        CHECK(after != id_b, "a dead id was reused");

        CHECK(!mpdq_remove_id(99999), "removing an id that is not there");
        CHECK(!mpdq_remove(mpdq_count()), "removing past the end");
        CHECK(mpdq_find_id(0) == -1, "id 0 was found; 0 means no entry");
    }

    /* ---- move is a move, not a swap --------------------------------- */
    {
        static const char *const p[] = { "a", "b", "c", "d", "e" };

        fill(p, 5);
        CHECK(mpdq_move(0, 2), "move 0 -> 2");
        order(got, sizeof(got));
        CHECK(strcmp(got, "b|c|a|d|e") == 0, "forward move gave \"%s\"", got);

        fill(p, 5);
        CHECK(mpdq_move(3, 1), "move 3 -> 1");
        order(got, sizeof(got));
        CHECK(strcmp(got, "a|d|b|c|e") == 0, "backward move gave \"%s\"", got);

        fill(p, 5);
        CHECK(mpdq_move(2, 2), "a move to the same place should succeed");
        order(got, sizeof(got));
        CHECK(strcmp(got, "a|b|c|d|e") == 0, "a no-op move changed \"%s\"", got);

        CHECK(!mpdq_move(0, 5), "a move to a non-position was accepted");
        CHECK(!mpdq_move(-1, 0), "a move from a non-position was accepted");

        /* Ids follow their entries through a move. */
        fill(p, 5);
        const uint32_t id_a = mpdq_id(0);
        mpdq_move(0, 4);
        CHECK(mpdq_find_id(id_a) == 4, "the moved entry's id is at %d",
              mpdq_find_id(id_a));
        CHECK(strcmp(mpdq_path(4), "a") == 0, "position 4 holds \"%s\"",
              mpdq_path(4));
    }

    /* ---- the version, which is plchanges ---------------------------- */
    {
        static const char *const p[] = { "a", "b", "c", "d", "e" };
        fill(p, 5);

        const uint32_t v = mpdq_version();
        changed_since(v, ch, sizeof(ch));
        CHECK(ch[0] == '\0', "nothing changed yet, but plchanges says %s", ch);

        /*
         * MPD's rule: the songs whose POSITION or content changed. A
         * removal at the front moves everything behind it, so a client
         * at version v must be told about all of them -- being told only
         * about position 0 would leave it drawing four rows in the wrong
         * places.
         */
        mpdq_remove(0);
        changed_since(v, ch, sizeof(ch));
        CHECK(strcmp(ch, "0,1,2,3") == 0,
              "after removing the first of five, plchanges says %s", ch);
        CHECK(mpdq_version() > v, "the version did not rise");

        /* An insert in the middle moves what follows it and nothing
         * before it. */
        fill(p, 5);
        const uint32_t v2 = mpdq_version();
        mpdq_insert(2, "new", NULL);
        changed_since(v2, ch, sizeof(ch));
        CHECK(strcmp(ch, "2,3,4,5") == 0,
              "after inserting at 2 of 5, plchanges says %s", ch);

        /* A move touches the span between its ends, and nothing outside
         * it: 1 -> 3 leaves 0 and 4 alone. */
        fill(p, 5);
        const uint32_t v3 = mpdq_version();
        mpdq_move(1, 3);
        changed_since(v3, ch, sizeof(ch));
        CHECK(strcmp(ch, "1,2,3") == 0,
              "after moving 1 -> 3 of 5, plchanges says %s", ch);

        /* A move that changes nothing should not make a client re-read
         * the list. */
        fill(p, 5);
        const uint32_t v4 = mpdq_version();
        mpdq_move(2, 2);
        CHECK(mpdq_version() == v4,
              "a no-op move bumped the version from %u to %u",
              v4, mpdq_version());

        /* An append touches only the new entry. */
        fill(p, 5);
        const uint32_t v5 = mpdq_version();
        mpdq_append("f", NULL);
        changed_since(v5, ch, sizeof(ch));
        CHECK(strcmp(ch, "5") == 0, "after an append, plchanges says %s", ch);

        /* Clearing is news: a client holding five rows must learn there
         * are none, and the version is the only way it can. */
        fill(p, 5);
        const uint32_t v6 = mpdq_version();
        mpdq_clear();
        CHECK(mpdq_version() > v6, "clearing did not raise the version");
        CHECK(mpdq_count() == 0, "clear left %d", mpdq_count());

        /* The version never goes backwards across a mixed run. */
        fill(p, 5);
        uint32_t last = mpdq_version();
        mpdq_append("x", NULL);
        mpdq_move(0, 3);
        mpdq_remove(1);
        mpdq_shuffle();
        CHECK(mpdq_version() > last, "the version stalled over five edits");
        last = mpdq_version();
        CHECK(mpdq_version() == last, "reading the version changed it");
    }

    /* ---- shuffle is a permutation ----------------------------------- */
    {
        static const char *const p[] = { "a", "b", "c", "d", "e", "f", "g" };
        fill(p, 7);

        uint32_t ids[7];
        for (int i = 0; i < 7; i++) ids[i] = mpdq_id(i);

        mpdq_shuffle();
        CHECK(mpdq_count() == 7, "shuffle changed the count to %d",
              mpdq_count());

        /* Every id still present, exactly once, and still on its own
         * path -- a shuffle that renumbered would break every songid a
         * client is holding. */
        for (int i = 0; i < 7; i++) {
            const int pos = mpdq_find_id(ids[i]);
            CHECK(pos >= 0, "id %u vanished in the shuffle", ids[i]);
            if (pos >= 0) {
                CHECK(strcmp(mpdq_path(pos), p[i]) == 0,
                      "id %u now points at \"%s\", not \"%s\"",
                      ids[i], mpdq_path(pos), p[i]);
            }
        }

        /* Every path exactly once. */
        for (int i = 0; i < 7; i++) {
            int seen = 0;
            for (int j = 0; j < 7; j++) {
                if (strcmp(mpdq_path(j), p[i]) == 0) seen++;
            }
            CHECK(seen == 1, "\"%s\" appears %d times after a shuffle",
                  p[i], seen);
        }

        /* A shuffle of fewer than two entries is a no-op, and must not
         * raise the version -- a client re-reading the list for nothing
         * on every shuffle of a one-track queue. */
        mpdq_clear();
        mpdq_append("only", NULL);
        const uint32_t v = mpdq_version();
        mpdq_shuffle();
        CHECK(mpdq_version() == v, "shuffling one entry bumped the version");
    }

    /* ---- full ------------------------------------------------------- */
    {
        mpdq_clear();
        char b[32];
        for (int i = 0; i < MPDQ_MAX; i++) {
            snprintf(b, sizeof(b), "t%04d", i);
            CHECK(mpdq_append(b, NULL) == i, "append %d of %d", i, MPDQ_MAX);
        }
        CHECK(mpdq_count() == MPDQ_MAX, "count is %d", mpdq_count());
        CHECK(mpdq_append("over", NULL) == -1, "the queue took one too many");
        CHECK(mpdq_insert(0, "over", NULL) == -1, "insert past full");

        /* Room again after a removal. */
        CHECK(mpdq_remove(0), "remove from a full queue");
        CHECK(mpdq_append("fits", NULL) == MPDQ_MAX - 1, "append after room");

        mpdq_clear();
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
