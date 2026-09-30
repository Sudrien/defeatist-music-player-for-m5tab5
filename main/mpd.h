/*
 * mpd.h -- an MPD server on port 6600: the listener, the task, and the
 * verbs of MPD.md step 9.
 *
 * WHAT IT IS. A third input to the same ui_task switch the touch panel,
 * the HID keys and the browser remote feed -- through uireq.h, beside the
 * remote's presses (MPD.md step 5) -- and a third reader of the same ui_state_t the panel
 * draws from. A `pause` from a phone is the press the glass would have
 * made, with the same rules, and `status` is what the screen is showing.
 *
 * WHAT IT ANSWERS (step 9): the connection verbs, `status`, `stats`,
 * `currentsong`, `outputs`, the transport -- `play`, `playid`, `pause`,
 * `stop`, `next`, `previous`, `seek`, `seekid`, `seekcur` -- and `setvol`
 * and `volume`. And the queue as it is, read-only: `playlistinfo`,
 * `playlistid`, `playlist`, `plchanges` and `plchangesposid`, with `play`
 * and `playid` able to start any entry (5166): see "THE QUEUE IS THE
 * QUEUE" in mpd.c. And since 5175 (step 11) the queue's edits: `add`,
 * `addid`, `delete`, `deleteid`, `move`, `moveid`, `clear` and
 * `shuffle`, through uireq.h as the remote page's are. Everything else in mpdproto.c's
 * table is ACKed with ACK_ERROR_UNKNOWN, "not supported by this player
 * yet", and listed by `notcommands`, so the two answers agree.
 *
 * And `idle` (step 10, 5160), with MPD's `noidle`: the rules are
 * mpdidle.h's, and what counts as a change is worked out in
 * mpd_publish() from the same snapshot `status` reads.
 *
 * The library (step 12): `lsinfo`, `listall`, `listallinfo` (5177),
 * `search`, `find` and `count` (5180), `list` (5182). One partition, `default`, and one
 * mount, the root, over whichever volumes are in (5180).
 *
 * Stored playlists (step 13, 5184): `listplaylists`, `listplaylist`,
 * `listplaylistinfo`, `load`, `save`, `rm`, as `Playlists/<name>.m3u` on
 * the card or the drive.
 *
 * WHAT IT IS NOT, YET. No `playlistadd`, `playlistdelete`, `rename` or
 * the other playlist editing verbs; save the queue instead. Adding a folder, part-queue `shuffle`, MPD 0.23's
 * relative positions ("+1") and 0.21's filter expressions are refused
 * and say so. No IPv6 listener.
 *
 * NO PASSWORD, as the remote has none, and for the same reason it is off
 * by default and the panel says so under the switch.
 *
 * THREADS. mpd_poll() and mpd_publish() are ui_task's. The server task
 * owns every socket and never calls into the player: it reads a
 * mutex-held copy of the last published state, and asks for presses
 * through uireq.h, which ui_task drains. See mpd.c for why a press WAITS
 * for ui_task where the remote's does not.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* sdkconfig.h before netbudget.h: that header's #if reads
 * CONFIG_LWIP_MAX_SOCKETS and does not include it, and an unset macro is 0
 * in an #if -- so reaching it first from a file that had not pulled in
 * sdkconfig.h by some other route would fail the build for no reason. */
#include "sdkconfig.h"
#include "netbudget.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MPD's registered port. Collides with nothing here: the portal and the
 * remote are on 53, 80 and 443 (MPD.md, "Ports, sockets, and the
 * switch"). */
#define MPD_PORT        (6600)

/*
 * How many clients at once: netbudget.h's number, not a second copy of it.
 * That header counted MPD's listener and three clients into the socket
 * ceiling in 5157, and its #error is what fails the build if this outgrows
 * the ceiling -- so the count lives there and is read here. A fourth
 * client is accepted and closed at once rather than left in the backlog,
 * so it fails fast instead of hanging; for that moment it holds a socket
 * the census does not count, which is what netbudget.h's two spare are.
 */
#define MPD_CLIENTS     (NETBUDGET_MPD_CLIENTS)

/* Once, from app_main(), before ui_task. Allocates; starts nothing. */
void mpd_init(void);

/*
 * Every ui_task pass. Starts the server when `want` is true and the
 * player has an address, stops it when either stops being true -- the
 * remote's rule (remote_poll()), copied. Cheap when nothing changes.
 */
void mpd_poll(bool want);

/* Whether the listener is up, and the address to give a client:
 * "a.b.c.d:6600". */
bool mpd_running(void);
bool mpd_address(char *out, size_t out_size);

/* How many clients are connected now. For the panel's note. */
int mpd_clients(void);

/*
 * What the screen is about to show, as `status` and `currentsong` will
 * report it. `path` is the file on screen (player.c's s_shown_path), and
 * `streaming` says it is a station instead, whose URL mpd.c reads from
 * stations.h. Also the point at which a press taken from uireq.h in this
 * pass counts as serviced (uireq_published()).
 */
void mpd_publish(const ui_state_t *st, const char *path, bool streaming);

/* 5192: ui_task, when a volume was put in or taken out: raises MPD's
 * database, mount and stored_playlist idle events. */
void mpd_media_changed(void);

/* 5260: whether a client's `consume oneshot` or `single oneshot` stands --
 * the play order is EAT or ONE for the song playing, and goes back when
 * it ends. For the chooser's order button, which otherwise reads the same
 * as a permanent EAT or ONE. Read on ui_task; set on the server task. */
bool mpd_oneshot(void);

#ifdef __cplusplus
}
#endif
