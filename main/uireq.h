/*
 * uireq.h -- the one way into ui_task from another task: MPD.md step 5,
 * "the shared mutation path".
 *
 * WHY ONE. The remote page and the MPD server each had their own pair:
 * a press queue and a take function (remote_take(), mpd_take()), and the
 * remote a one-slot mailbox for a chosen file (remote_take_open()). Each
 * was single-consumer and static to its file, so ui_task polled two sets
 * of take functions at two places, and a press from each arriving in the
 * same pass was settled by which line came first in player.c rather than
 * by which came first. Step 6 (the remote's queue verbs) and step 11
 * (MPD's) would each have grown a third and fourth. So the path is here,
 * owned by neither: both servers are producers, ui_task is the consumer,
 * and it drains in the same two places it drained before.
 *
 * NO NEW BEHAVIOUR is the point of this file, so what each producer had
 * it keeps:
 *
 *  - A PRESS is a ui_action_t, as the panel would have produced it.
 *    Each source may have UIREQ_PER_SOURCE of them waiting -- the depth
 *    each had as its own queue (8 and 8) -- so a burst from one cannot
 *    fill the room the other had. uireq_press() never blocks: the remote
 *    drops a press that does not fit, as it always did, and MPD retries
 *    for MPD_ASK_QUEUE_MS and then refuses, as it always did.
 *
 *  - A press is taken in the ORDER IT ARRIVED, across both sources. That
 *    is the one thing that changed: before, a remote press waiting in
 *    the same pass as an MPD press went first because player.c read
 *    remote_take() first. Arrival order is what "the same press as the
 *    glass" means, and nothing depended on the other.
 *
 *  - COMPLETION is MPD's: every press gets a sequence number, and
 *    uireq_serviced() says whether the pass that took it has published
 *    its effect (uireq_published(), from mpd_publish()). The remote does
 *    not wait and ignores the number.
 *
 *  - A CHOSEN FILE OR FOLDER is one slot, as it was: a second choice
 *    before ui_task took the first replaces it, as a second tap on the
 *    chooser would. Only the remote produces one today.
 *
 * THREADS. uireq_press(), uireq_serviced() and uireq_open() from any
 * task; uireq_take_press(), uireq_take_open() and uireq_published() from
 * ui_task only. Everything is under one mutex; nothing here blocks on
 * anything but it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UIREQ_REMOTE = 0,
    UIREQ_MPD,
    UIREQ_SOURCES,
} uireq_source_t;

/* Presses one source may have waiting: what remote.c's and mpd.c's own
 * queues each held. */
#define UIREQ_PER_SOURCE    (8)
#define UIREQ_DEPTH         (UIREQ_PER_SOURCE * UIREQ_SOURCES)

/* The longest chosen path, with its NUL: remoteproto.h's
 * REMOTEPROTO_PATH_MAX, which remote.c checks against it. */
#define UIREQ_PATH_MAX      (512)

/* Once, from app_main(), before remote_init(), mpd_init() and ui_task. */
void uireq_init(void);

/*
 * Queue a press. Its sequence number (never 0) when it fitted, 0 when
 * `src` already has UIREQ_PER_SOURCE waiting or before uireq_init().
 * Never blocks.
 */
uint32_t uireq_press(uireq_source_t src, const ui_action_t *act);

/* ui_task: the oldest waiting press, from either source. False when there
 * is none. */
bool uireq_take_press(ui_action_t *out);

/* ui_task: every press taken so far has had its effect published. Called
 * by mpd_publish() after it copies the state clients read. */
void uireq_published(void);

/* Whether press `seq` has been taken and its effect published. */
bool uireq_serviced(uint32_t seq);

/*
 * A file (`folder` false: BROWSER_PLAY_FILE) or folder (true:
 * BROWSER_PLAY_FOLDER) chosen elsewhere. `path` need not be terminated;
 * `len` is the truth. Replaces one not yet taken. False, and nothing
 * stored, when it does not fit UIREQ_PATH_MAX.
 */
bool uireq_open(const char *path, size_t len, bool folder);

/* ui_task: the chosen file or folder, and clears it. False when there is
 * none. */
bool uireq_take_open(char *path, size_t size, bool *folder);

#ifdef __cplusplus
}
#endif
