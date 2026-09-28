/*
 * uireqtest.c -- uireq.c, the one way into ui_task (MPD.md step 5).
 *
 * Written from uireq.h's promises, not from the ring inside uireq.c:
 * what each producer had before the move is what it must still have.
 *
 *  - each source keeps its own eight, and a full one does not take the
 *    other's room (remote.c's and mpd.c's queues were 8 each);
 *  - presses come out in the order they went in, across sources;
 *  - a sequence number is never 0, and is serviced only once the pass
 *    that took it has published -- not when a later press is queued,
 *    and not when it is merely taken;
 *  - the open slot is one slot: the second choice replaces the first,
 *    it is cleared by taking, and a path too long is refused whole
 *    rather than stored cut.
 *
 * One thread, and shim.h's mutex is a no-op, so nothing here tests the
 * locking; it tests what the lock protects.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uireq.h"

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } \
} while (0)

static ui_action_t act(ui_action_kind_t k, int v)
{
    const ui_action_t a = { .kind = k, .value = v };
    return a;
}

static void drain(void)
{
    ui_action_t a;
    while (uireq_take_press(&a)) { }
    uireq_published();
}

static void drain_edits(void)
{
    uireq_edit_t e;
    ui_action_t a;
    for (;;) {
        if (uireq_take_edit(&e, NULL, 0)) continue;
        if (uireq_take_press(&a)) continue;
        break;
    }
    uireq_published();
}

static void before_init(void)
{
    ui_action_t a = act(UI_ACTION_NEXT, 0);
    CHECK(uireq_press(UIREQ_REMOTE, &a) == 0);
    CHECK(!uireq_take_press(&a));
    CHECK(!uireq_open("/sdcard/a", 9, false));
    char p[16];
    CHECK(!uireq_take_open(p, sizeof(p), NULL));
}

static void order_across_sources(void)
{
    const ui_action_t r1 = act(UI_ACTION_PAUSE, 0), m1 = act(UI_ACTION_SEEK, 40),
                      r2 = act(UI_ACTION_VOLUME, 70);
    CHECK(uireq_press(UIREQ_MPD, &m1) != 0);
    CHECK(uireq_press(UIREQ_REMOTE, &r1) != 0);
    CHECK(uireq_press(UIREQ_REMOTE, &r2) != 0);
    ui_action_t a;
    CHECK(uireq_take_press(&a) && a.kind == UI_ACTION_SEEK && a.value == 40);
    CHECK(uireq_take_press(&a) && a.kind == UI_ACTION_PAUSE);
    CHECK(uireq_take_press(&a) && a.kind == UI_ACTION_VOLUME && a.value == 70);
    CHECK(!uireq_take_press(&a));
    drain();
}

static void per_source_room(void)
{
    const ui_action_t n = act(UI_ACTION_NEXT, 0);
    for (int i = 0; i < UIREQ_PER_SOURCE; i++) CHECK(uireq_press(UIREQ_REMOTE, &n) != 0);
    /* The remote's ninth is dropped, as its own queue dropped it... */
    CHECK(uireq_press(UIREQ_REMOTE, &n) == 0);
    /* ...and MPD still has all eight of its own. */
    for (int i = 0; i < UIREQ_PER_SOURCE; i++) CHECK(uireq_press(UIREQ_MPD, &n) != 0);
    CHECK(uireq_press(UIREQ_MPD, &n) == 0);

    /* Taking one gives the room back to the source it came from. */
    ui_action_t a;
    CHECK(uireq_take_press(&a));                    /* a remote press */
    CHECK(uireq_press(UIREQ_MPD, &n) == 0);
    CHECK(uireq_press(UIREQ_REMOTE, &n) != 0);
    CHECK(uireq_press(UIREQ_REMOTE, &n) == 0);

    CHECK(uireq_press(UIREQ_SOURCES, &n) == 0);     /* not a source */
    CHECK(uireq_press(UIREQ_MPD, NULL) == 0);
    drain();

    /* Round the ring more than once, so its wrap is on the path. */
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < UIREQ_PER_SOURCE; i++) {
            const ui_action_t v = act(UI_ACTION_VOLUME, round * 10 + i);
            CHECK(uireq_press(i & 1 ? UIREQ_MPD : UIREQ_REMOTE, &v) != 0);
        }
        for (int i = 0; i < UIREQ_PER_SOURCE; i++)
            CHECK(uireq_take_press(&a) && a.value == round * 10 + i);
    }
    drain();
}

static void serviced(void)
{
    const ui_action_t p = act(UI_ACTION_PLAY, 0);
    const uint32_t s1 = uireq_press(UIREQ_MPD, &p);
    const uint32_t s2 = uireq_press(UIREQ_REMOTE, &p);
    const uint32_t s3 = uireq_press(UIREQ_MPD, &p);
    CHECK(s1 && s2 && s3 && s1 != s2 && s2 != s3);
    CHECK(!uireq_serviced(s1));

    /* Publishing with nothing taken services nothing new. */
    uireq_published();
    CHECK(!uireq_serviced(s1));

    ui_action_t a;
    CHECK(uireq_take_press(&a));
    CHECK(!uireq_serviced(s1));         /* taken is not published */
    uireq_published();
    CHECK(uireq_serviced(s1));
    CHECK(!uireq_serviced(s2));
    CHECK(!uireq_serviced(s3));

    /* A remote press between two MPD presses: taking it does not service
     * the MPD press behind it. */
    CHECK(uireq_take_press(&a));
    uireq_published();
    CHECK(uireq_serviced(s2));
    CHECK(!uireq_serviced(s3));
    CHECK(uireq_take_press(&a));
    uireq_published();
    CHECK(uireq_serviced(s3));
    CHECK(uireq_serviced(s1));          /* and stays serviced */
}

static void open_slot(void)
{
    char p[UIREQ_PATH_MAX];
    bool folder = true;
    CHECK(!uireq_take_open(p, sizeof(p), &folder));

    /* Not terminated at len: the remote's path points into its frame. */
    const char frame[] = "/sdcard/Music/A.flacTRAILING";
    CHECK(uireq_open(frame, 20, false));
    CHECK(uireq_open("/sdcard/Music", 13, true));   /* replaces it */
    CHECK(uireq_take_open(p, sizeof(p), &folder));
    CHECK(strcmp(p, "/sdcard/Music") == 0 && folder);
    CHECK(!uireq_take_open(p, sizeof(p), &folder)); /* cleared */

    CHECK(uireq_open(frame, 20, false));
    CHECK(uireq_take_open(p, sizeof(p), NULL));
    CHECK(strcmp(p, "/sdcard/Music/A.flac") == 0);

    /* The longest that fits, and one past it. */
    char *big = malloc(UIREQ_PATH_MAX);
    memset(big, 'x', UIREQ_PATH_MAX);
    CHECK(uireq_open(big, UIREQ_PATH_MAX - 1, false));
    CHECK(uireq_take_open(p, sizeof(p), NULL) && strlen(p) == UIREQ_PATH_MAX - 1);
    CHECK(uireq_open("/sdcard/kept", 12, false));
    CHECK(!uireq_open(big, UIREQ_PATH_MAX, true));  /* refused whole... */
    CHECK(uireq_take_open(p, sizeof(p), &folder));
    CHECK(strcmp(p, "/sdcard/kept") == 0 && !folder); /* ...not stored */

    /* A short buffer gets a terminated prefix, never an overrun. */
    char small[8];
    CHECK(uireq_open("/sdcard/Music", 13, true));
    CHECK(uireq_take_open(small, sizeof(small), NULL));
    CHECK(strcmp(small, "/sdcard") == 0);
    free(big);
}

/* 5173: edits, in the ring with the presses. */
static void edits(void)
{
    uireq_edit_t e;
    char p[UIREQ_PATH_MAX];

    /* An edit at the head is taken by take_edit, not take_press. */
    const uireq_edit_t add = { .kind = UIREQ_EDIT_ADD };
    const char frame[] = "/sd/a.mp3TRAILING";
    const uint32_t s1 = uireq_edit(UIREQ_REMOTE, &add, frame, 9);
    CHECK(s1 != 0);
    ui_action_t a;
    CHECK(!uireq_take_press(&a));
    CHECK(uireq_take_edit(&e, p, sizeof(p)) && e.kind == UIREQ_EDIT_ADD && strcmp(p, "/sd/a.mp3") == 0);
    CHECK(!uireq_take_edit(&e, p, sizeof(p)));
    uireq_published();
    CHECK(uireq_serviced(s1));

    /* Order is kept across the two: edit, press, edit. The press stops
     * the edits behind it until it is taken. */
    const uireq_edit_t del = { .kind = UIREQ_EDIT_DELETE, .id = 42 };
    const uireq_edit_t mov = { .kind = UIREQ_EDIT_MOVE, .id = 9, .pos = 3 };
    const ui_action_t n = act(UI_ACTION_NEXT, 0);
    const uint32_t e1 = uireq_edit(UIREQ_MPD, &del, NULL, 0);
    const uint32_t pr = uireq_press(UIREQ_REMOTE, &n);
    const uint32_t e2 = uireq_edit(UIREQ_REMOTE, &mov, NULL, 0);
    CHECK(e1 && pr && e2);
    CHECK(uireq_take_edit(&e, p, sizeof(p)) && e.kind == UIREQ_EDIT_DELETE && e.id == 42 && p[0] == '\0');
    CHECK(!uireq_take_edit(&e, p, sizeof(p)));      /* the press is next */
    uireq_published();
    CHECK(uireq_serviced(e1) && !uireq_serviced(pr) && !uireq_serviced(e2));
    CHECK(uireq_take_press(&a) && a.kind == UI_ACTION_NEXT);
    CHECK(uireq_take_edit(&e, NULL, 0) && e.kind == UIREQ_EDIT_MOVE && e.id == 9 && e.pos == 3);
    uireq_published();
    CHECK(uireq_serviced(pr) && uireq_serviced(e2));

    /* Refusals: an add with no path, or one too long; and edits count
     * against the source's room like presses. */
    CHECK(uireq_edit(UIREQ_REMOTE, &add, NULL, 0) == 0);
    CHECK(uireq_edit(UIREQ_REMOTE, &add, "/sd/x", 0) == 0);
    char *big = malloc(UIREQ_PATH_MAX);
    memset(big, 'x', UIREQ_PATH_MAX);
    CHECK(uireq_edit(UIREQ_REMOTE, &add, big, UIREQ_PATH_MAX) == 0);
    CHECK(uireq_edit(UIREQ_REMOTE, &add, big, UIREQ_PATH_MAX - 1) != 0);
    for (int i = 1; i < UIREQ_PER_SOURCE; i++) CHECK(uireq_edit(UIREQ_REMOTE, &add, "/sd/y", 5) != 0);
    CHECK(uireq_edit(UIREQ_REMOTE, &add, "/sd/y", 5) == 0);   /* full: the copy is freed (ASan) */
    CHECK(uireq_press(UIREQ_REMOTE, &n) == 0);
    CHECK(uireq_edit(UIREQ_MPD, &del, NULL, 0) != 0);         /* the other's room */
    CHECK(uireq_take_edit(&e, p, sizeof(p)) && strlen(p) == UIREQ_PATH_MAX - 1);
    /* A short buffer: a terminated prefix. */
    char small[4];
    CHECK(uireq_take_edit(&e, small, sizeof(small)) && strcmp(small, "/sd") == 0);
    drain_edits();
    free(big);
}

/* 5175: outcomes, and the seq take_edit hands back to report them by. */
static void outcomes(void)
{
    uireq_edit_t e;
    const uireq_edit_t add = { .kind = UIREQ_EDIT_ADD, .pos = 3 };
    const uint32_t s1 = uireq_edit(UIREQ_MPD, &add, "/sd/a.mp3", 9);
    const uint32_t s2 = uireq_edit(UIREQ_MPD, &add, "/sd/b.mp3", 9);
    CHECK(s1 && s2);
    uireq_done_t how = UIREQ_DONE_GONE;
    uint32_t id = 99;
    CHECK(!uireq_edit_outcome(s1, &how, &id));          /* not yet */
    CHECK(uireq_take_edit(&e, NULL, 0) && e.seq == s1 && e.pos == 3);
    uireq_edit_done(e.seq, UIREQ_DONE_OK, 1234);
    CHECK(uireq_edit_outcome(s1, &how, &id) && how == UIREQ_DONE_OK && id == 1234);
    CHECK(!uireq_edit_outcome(s2, &how, &id));
    CHECK(uireq_take_edit(&e, NULL, 0) && e.seq == s2);
    uireq_edit_done(e.seq, UIREQ_DONE_FULL, 0);
    CHECK(uireq_edit_outcome(s2, &how, &id) && how == UIREQ_DONE_FULL && id == 0);
    CHECK(!uireq_edit_outcome(0, &how, &id));
    uireq_published();

    /* Kept for UIREQ_OUTCOMES edits, then overwritten -- never answered
     * for the wrong edit. */
    const uireq_edit_t clr = { .kind = UIREQ_EDIT_CLEAR };
    const uint32_t first = uireq_edit(UIREQ_MPD, &clr, NULL, 0);
    CHECK(uireq_take_edit(&e, NULL, 0));
    uireq_edit_done(e.seq, UIREQ_DONE_OK, 7);
    for (int i = 0; i < UIREQ_OUTCOMES; i++) {
        CHECK(uireq_edit(UIREQ_MPD, &clr, NULL, 0) != 0);
        CHECK(uireq_take_edit(&e, NULL, 0));
        uireq_edit_done(e.seq, UIREQ_DONE_GONE, 0);
        if (i == UIREQ_OUTCOMES - 2) CHECK(uireq_edit_outcome(first, &how, &id) && id == 7);
    }
    CHECK(!uireq_edit_outcome(first, &how, &id));        /* its slot reused */
    uireq_published();
}

int main(void)
{
    before_init();
    uireq_init();
    uireq_init();       /* twice is once */
    order_across_sources();
    per_source_room();
    serviced();
    open_slot();
    edits();
    outcomes();
    if (s_fail) {
        fprintf(stderr, "uireqtest: %d failed\n", s_fail);
        return 1;
    }
    printf("uireqtest: ok\n");
    return 0;
}
