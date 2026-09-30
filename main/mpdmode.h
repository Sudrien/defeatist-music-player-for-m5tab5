/*
 * mpdmode.h -- play_order_t against MPD's four flags, as a table.
 *
 * MPD.md step 7. The device has ONE enum of four states
 * (`playlist.h:23-28`, cycled by a button in `browser.c:1410`) and MPD
 * has four independent booleans, which is sixteen states. The mapping is
 * lossy in one direction and not the other, and MPD.md asked for it
 * "written down as a table with the states that have no analogue named
 * explicitly" rather than improvised per command -- so it is a table, in
 * the .c, indexed rather than nested.
 *
 * PURE. No IDF beyond `playlist.h`'s own include, which the host shim
 * supplies, so the table is tested on a host (texttest/mpdmodetest.c).
 *
 * THE DIRECTION THAT IS EXACT: all five orders have an exact MPD
 * equivalent. Nothing the glass can be set to is unsayable in MPD's
 * flags, which is the useful half and is worth knowing before reading the
 * rest of this file.
 *
 * THE DIRECTION THAT IS NOT: eight combinations of random/repeat/single
 * onto four orders, so four of the eight are approximations, and `consume`
 * is a fifth loss on top of any of them.
 *
 *   random repeat single   order         exact?
 *   ------ ------ ------   -----------   ------
 *      0      0      0     ALL           yes
 *      0      0      1     ONE           yes
 *      0      1      0     ALL           NO -- "repeat the folder"
 *      0      1      1     REPEAT_ONE    yes
 *      1      0      0     SHUFFLE       yes
 *      1      0      1     ONE           NO -- random is dropped
 *      1      1      0     SHUFFLE       NO -- "reshuffle at the end"
 *      1      1      1     REPEAT_ONE    NO -- random is dropped
 *
 * **The missing state that matters is repeat-all**, row three: keep
 * playing the folder from the top when it ends. It is probably the most
 * commonly set option in any MPD client and this device has no such mode
 * -- `PLAY_ORDER_ALL` stops at the end of the folder. It is named here
 * because "we support repeat" is what a reader assumes from a `repeat`
 * flag existing at all.
 *
 * The two rows where random is dropped look worse than they are and are
 * still not exact. With `single` set the next track is never chosen
 * automatically, so a random ORDER is never used -- but MPD's `next`
 * would still pick randomly where `PLAY_ORDER_ONE`'s goes to the
 * following track (`playlist.h:125`: ONE is mapped to ALL for the skip
 * button, because a press is not the end of a track). So the difference
 * survives in the one place a listener would notice it.
 *
 `consume` IS PLAY_ORDER_EAT (5252), a fifth order: ALL, removing each
 * track as playback leaves it forward. consume alone is EAT exactly;
 * consume + repeat is EAT with repeat springing back (everything is
 * eaten, so MPD has nothing to repeat either); consume with random or
 * single is dropped, as every consume was before 5252 -- the device has
 * no shuffled or stop-after eating.
 *
 * WHAT `status` REPORTS AFTER A CLIENT SETS SOMETHING UNREPRESENTABLE:
 * the flags of the order the device will ACTUALLY traverse in, which is
 * `mpdmode_normalise()`. So a client that sends `repeat 1` sees its toggle
 * spring back to 0.
 *
 * That is deliberate and it is the whole reason this file returns a
 * normalisation rather than storing what it was told. A toggle that
 * springs back says "this device cannot do that", which is true and which
 * a listener can act on. A `repeat` that reads 1 while the player stops
 * at the end of the folder is a field that is confidently wrong -- the
 * thing 5155 refused to do with `Last-Modified`, for the same reason.
 * Neither option is pleasant; only one of them lies.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool repeat;
    bool random;
    bool single;
    bool consume;
} mpd_modes_t;

/*
 * The flags that describe what `o` actually does. Exact for all five;
 * `consume` is EAT's alone.
 */
mpd_modes_t mpdmode_from_order(play_order_t o);

/*
 * The order that comes closest to `m`. Lossy; the table above says where.
 * `consume` is EAT when random and single are off, else ignored.
 */
play_order_t mpdmode_to_order(const mpd_modes_t *m);

/*
 * Whether `m` is exactly what this device will do -- that is, whether it
 * survives the round trip. False for the four approximating rows and for
 * `consume` with anything else set.
 *
 * This is what a caller uses to decide whether to log that a client asked
 * for something it will not get. It is NOT a reason to ACK: MPD clients
 * do not expect `repeat 1` to fail, and a refusal is a worse answer than
 * a toggle that springs back.
 */
bool mpdmode_exact(const mpd_modes_t *m);

/*
 * What `status` should report after a client asked for `m`: the flags of
 * the order the device will traverse in.
 *
 * IDEMPOTENT, and that is a property rather than an accident -- a client
 * reads `status`, sees flags, and may send them back. If normalising
 * twice differed from normalising once, a client echoing what it read
 * would walk the setting somewhere else on every round, which is a
 * setting that drifts while nobody touches it.
 */
mpd_modes_t mpdmode_normalise(const mpd_modes_t *m);

/*
 * ReplayGain (5161): MPD's four modes against this device's one switch.
 *
 * MPD has `off`, `track`, `album` and `auto` (src/ReplayGainMode.cxx),
 * matched case-sensitively. This device has ReplayGain on or off
 * (settings_rg_enabled()), and "on" is TRACK gain: the loudness is
 * measured per track as it plays and stored beside it (replaygain.h), and
 * nothing measures an album. So:
 *
 *   mode     device   status reports   exact?
 *   -----    ------   --------------   ------
 *   off      off      off              yes
 *   track    on       track            yes
 *   album    on       track            NO -- no album gain exists here
 *   auto     on       track            NO -- MPD's auto is album-or-track
 *
 * `album` and `auto` are honoured as "on" rather than refused, and
 * `replay_gain_status` then says `track`: 5156's rule, the toggle that
 * springs back to what the device will actually do rather than a field
 * that is confidently wrong. And a client MUST be able to set `track`,
 * because Cantata reads the mode, saves it, and sends it back on every
 * connect after the first.
 *
 * The device's switch "takes effect at the next track" (settings.h),
 * where MPD's applies to what is already playing. Named, not fixed: it
 * is the device's own rule for a reason written down there.
 */

/* 1 for a mode that turns ReplayGain on, 0 for `off`, -1 for anything
 * that is not one of MPD's four names -- which is ACK_ERROR_ARG,
 * "Unrecognized replay gain mode". */
int mpdmode_rg_from_name(const char *name);

/* What `replay_gain_status` reports: "track" or "off". */
const char *mpdmode_rg_name(bool on);

/*
 * 5166: `nextsong` -- the position that follows `cur` in a list of `n`,
 * under order `o`, or -1 for none.
 *
 * MPD's rule is playlist::GetNextPosition(): single+repeat is the same
 * song, otherwise the next position, otherwise the first if repeat is
 * set, otherwise none. Taken through this device's four orders, which
 * are each exact in MPD's flags (the table above):
 *
 *   ALL, EAT     cur+1, or none at the end
 *   ONE          cur+1, or none -- single WITHOUT repeat still names the
 *                next song, and it is where Next goes (playlist.h maps
 *                ONE to ALL for the button, for the same reason)
 *   REPEAT_ONE   cur
 *   SHUFFLE      none, and the one row that is a loss. MPD shuffles an
 *                order in advance and can name the next song; this device
 *                picks when it gets there (playlist.h says why), so there
 *                is no next song to name. A client that keys its Next
 *                button on `nextsongid` -- Cantata does -- greys it under
 *                shuffle, where Next in fact works.
 *
 * -1 as well for a `cur` that is not a position.
 */
int mpdmode_next_pos(play_order_t o, int cur, int n);

#ifdef __cplusplus
}
#endif
