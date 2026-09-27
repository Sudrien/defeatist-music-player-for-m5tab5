/*
 * remote.h -- the browser remote: a single-page app on port 8080 and a
 * WebSocket that carries the player's state out and presses back in.
 *
 * WHAT IT IS. The transport bar, again, in a browser: the cover, the
 * envelope with the position on it, title/album/artist, prev, the switch,
 * next, the star and the volume. It is a second input to the same
 * ui_task switch the touch panel and the HID keys feed -- remote_take()
 * is read beside s_hid_action -- so a press from a phone is the same
 * press as one on the glass, with the same rules. And a second reader of
 * the same ui_state_t the panel draws from, so the two cannot disagree.
 *
 * WHAT IT IS NOT. It cannot record (remoteproto.h says why), it has no
 * settings page and no network setup -- the portal is where the network
 * is changed, from a connection that is not the one being changed -- and
 * it has no chooser yet. It renders nothing on the device: the page
 * draws the cover from the original bytes, the envelope from the levels,
 * and counts the clock itself between updates.
 *
 * NO PASSWORD. Anyone on the network the player is on can use it. That
 * is why it is off by default and why the panel says so under the
 * switch.
 *
 * PORT 8080, not 80, so that it never meets the setup portal: the two
 * are separate httpd instances with separate control ports and can run
 * at once. The portal keeps port 80 because a phone on its access point
 * is sent there by the captive-portal check.
 *
 * THREADS. remote_poll() and remote_publish() are ui_task's; the server's
 * handlers run on the httpd task and reach the player only through a
 * queue (presses) and a mutex-held copy of the last state (for a page
 * that has just connected). Sends to open sockets are queued onto the
 * httpd task with httpd_queue_work(), which is the only task allowed to
 * write to them.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_PORT     (8080)

/* Once, from app_main(), before ui_task. Allocates the queue and the
 * buffers; starts nothing. */
void remote_init(void);

/*
 * Every ui_task pass. Starts the server when `want` is true and the
 * player has an address, stops it when either stops being true. Cheap
 * when nothing changes.
 */
void remote_poll(bool want);

/* Whether the server is up, and the address to type into a browser. */
bool remote_running(void);
bool remote_url(char *out, size_t out_size);

/*
 * The state ui_draw() is about to draw, plus what ui_state_t does not
 * carry: the path whose cover /art serves, and the record countdown.
 * Sent to every open page when it changes -- the position only when it
 * jumps or every few seconds, since the page counts it itself.
 */
void remote_publish(const ui_state_t *st, const char *art_path, int rec_count);

/* A press from a page, as the touch panel would have produced it. False
 * when there is none. */
bool remote_take(ui_action_t *out);

#ifdef __cplusplus
}
#endif
