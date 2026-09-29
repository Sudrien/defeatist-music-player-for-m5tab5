/*
 * mpduri.h -- an MPD URI is an index path, and the mapping to a VFS path.
 *
 * MPD.md said `remoteproto_path_ok()` would be reused verbatim for every
 * verb that names a thing on a card, and 5153 found it cannot be: that
 * function wants an ABSOLUTE path under a mount -- "/sd/..." or
 * "/usb/..." -- and an MPD URI is relative to one library root. This file
 * is the missing half, and the reuse happens here rather than nowhere:
 * the mapped result is handed to `remoteproto_path_ok()` before anything
 * touches a filesystem.
 *
 * THE QUESTION 5153 LEFT OPEN WAS ALREADY ANSWERED, and by the index
 * format. `mediaindex.h:10-16`:
 *
 *     PATHS are relative to the volume root, with no leading slash --
 *     "Artist/Album/01 Song.flac" ... Relative because SD and USB are
 *     shown to MPD as one library (SD preferred), and the only way
 *     `Artist/x.flac` on one volume can be recognised as `Artist/x.flac`
 *     on the other is by the part below the mount point.
 *
 * So the relative form was chosen in 5010-5019 BECAUSE MPD shows one
 * merged library, and `medialist.h` merges on exactly that key. An MPD
 * URI is therefore an index path, unchanged: no volume in it, no leading
 * slash. The alternative -- "sd" and "usb" as two top-level directories,
 * which is what the remote page's chooser shows at "/" (`remote.c:610`)
 * -- would contradict the stated reason the index paths are relative and
 * would make the same album on two cards two albums to a client.
 *
 * WHAT THAT COSTS, written down because it is a real loss and not a free
 * choice: a relative path present on BOTH volumes resolves to the SD
 * copy, and the USB copy is not addressable over MPD at all. That is not
 * a rule this file invents -- `medialist.h` already shows the preferred
 * volume's copy and hides the other -- so MPD inherits a decision
 * MEDIA-INDEX.md point 3 made deliberately rather than adding one. A
 * client cannot ask for the shadowed file because there is no URI that
 * means it.
 *
 * AND A DIRECTORY URI IS NEVER MAPPED TO A VFS PATH. That surprised the
 * first draft of this file, which had one function for both. Listing is
 * an INDEX operation -- `medialist_open()` takes the relative directory
 * and reads records, opening nothing -- so `lsinfo` needs no mount and
 * no volume. Only a FILE is opened, and only then does a volume have to
 * be chosen. That is why `mpduri_to_vfs()` is documented as files-only
 * rather than as the general mapping.
 *
 * 5191: SUPERSEDED. The URI now starts with its volume -- "sd/Artist/
 * x.flac", "usb/Artist/x.flac" -- and the library root holds two folders,
 * `sd` and `usb`, as MPD shows a mount (and as `listmounts` already said).
 * Asked for on the board: the USB copy of a path both volumes hold was
 * unreachable, and every add from Cantata was tried on the SD first and
 * logged "no such file" before the USB copy was found. The cost written
 * down above is now the other one: an album on both volumes is two
 * albums to a client. The index paths are unchanged -- relative to the
 * volume, which the prefix now names -- and so is everything below
 * mpduri_split(). What is said above about the index being the thing
 * listed, and a directory never being mapped to a VFS path, still holds.
 *
 * PURE, and host-tested against a synthetic index (texttest/mpduritest.c)
 * for the reason `medialist.h` is: everything comes through
 * `midx_src_t`'s callbacks, so nothing here opens a file or needs IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mediaindex.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A URI is an index path, so it is bounded by the index's own limit and
 * not by remoteproto.h's 512 -- which is MIDX_PATH_MAX plus room for a
 * mount, and the arithmetic below is why those two numbers differ.
 */
#define MPDURI_REL_MAX  MIDX_PATH_MAX               /* 506: below the volume */
#define MPDURI_MAX      (4 + MPDURI_REL_MAX)        /* 5191: "usb/" + that */

/*
 * The mounts, matching `remoteproto.c`'s vols[] because they are the same
 * two volumes. The VFS ceiling is the longest mount, a slash, a URI and
 * the NUL -- which comes to 512, the same as MPDQ_PATH_MAX, for the same
 * reason that file gives ("mediaindex.h's limit plus a mount").
 */
#define MPDURI_MOUNT_SD     "/sd"
#define MPDURI_MOUNT_USB    "/usb"
#define MPDURI_VFS_MAX      (5 + 1 + MPDURI_REL_MAX)    /* "/usb" + '/' + path + NUL */

/*
 * The volume slots, in `medialist.h`'s order, which is the order that
 * decides who wins: SD in 0, USB in 1, per MEDIA-INDEX.md point 3. A
 * volume that is not mounted is a NULL src in its slot.
 */
enum {
    MPDURI_VOL_SD  = 0,
    MPDURI_VOL_USB = 1,
    MPDURI_VOLS    = 2,
};

/* The mount for a slot, or NULL. Static storage. */
const char *mpduri_mount(int vol);

/* 5191: the slot's name at the top of a URI -- "sd", "usb" -- or NULL. */
const char *mpduri_name(int vol);

/*
 * 5191: the volume a URI names, and the index path below it (into `uri`;
 * "" when the URI is the volume itself). -1 for the root, and for a URI
 * whose first segment is neither volume -- which names nothing. `uri`
 * must already have passed mpduri_ok().
 */
int mpduri_split(const char *uri, const char **rel);

/*
 * Whether `uri` (NUL-terminated) is a URI this server may accept.
 *
 * The rules are `remoteproto_path_ok()`'s, minus the two that are about
 * being absolute: no leading slash (an index path has none), no trailing
 * slash, no empty segment, no "." or ".." segment, no byte below 0x20,
 * no 0x7F, no '\\', and nothing longer than MPDURI_MAX.
 *
 * '\\' IS EXCLUDED even though it is not a traversal character, for two
 * reasons worth naming: it is not a legal filename byte on FAT32 or
 * exFAT, so no real path contains one, and it is the protocol's escape
 * character -- a URI carrying one is a client that has failed to quote,
 * which is better refused than resolved.
 *
 * '#' IS NOT excluded, and must not be: a cue track's URI is
 * "<sheet>.cue#NN" (`mediaindex.h:16`), the name the rest of the player
 * already uses for one, so the '#' is part of the path and travels with
 * it.
 *
 * `allow_root` says whether the EMPTY uri passes. It is the library root,
 * which is what `lsinfo` with no argument means, and it is never a file
 * -- so a verb that opens something passes false and a verb that lists
 * passes true. An argument rather than two functions because the caller
 * knowing which kind of verb it is, is the whole of the distinction.
 */
bool mpduri_ok(const char *uri, bool allow_root);

/* Whether `uri` is the library root, which is the empty string. */
bool mpduri_is_root(const char *uri);

/*
 * A directory URI in the form `medialist_open()` wants: "" for the root,
 * otherwise the URI with a '/' appended. False when it is not a URI or
 * will not fit.
 */
bool mpduri_dir(const char *uri, char *out, size_t cap);

/*
 * Resolve a FILE uri to a VFS path, on the volume it names (5191; it
 * used to try each in slot order, SD first -- the shadowing described at
 * the top of this file). A DEAD record does not
 * count as found: a tombstone is a file that is no longer on the card
 * (`mediaindex.h` keeps them so reconcile can revive them), and resolving
 * to one would hand the player a path to open that is not there.
 *
 * The built path is checked with `remoteproto_path_ok()` before it is
 * returned, which is MPD.md's intended reuse arriving at the place it
 * actually fits. That check should never fail on a path built from a URI
 * this function already accepted; it is there because the alternative to
 * a redundant check on a path assembled from network input is no check.
 *
 * `out` needs MPDURI_VFS_MAX. `vol_out` may be NULL. False when the uri
 * is not one, when it is the root, when neither volume has a live record
 * for it, or when a read failed -- and the caller's answer to all but the
 * last is MPD_ACK_NO_EXIST.
 */
bool mpduri_to_vfs(const char *uri, midx_src_t *const src[MPDURI_VOLS],
                   char *out, size_t cap, int *vol_out);

/*
 * The URI for a VFS path: the volume's name, then the part below its
 * mount (5191: "/usb/a/b.flac" is "usb/a/b.flac").
 *
 * This is the direction `currentsong` and `playlistinfo` need, because
 * what the player holds is a VFS path and what a client must be told is a
 * URI -- and a client will send that URI straight back in a `playid` or
 * an `add`, so the two directions have to agree exactly.
 *
 * False when `vfs` is not under "/sd" or "/usb", which includes a stream
 * URL: a radio station is not a library file and has no URI here. The
 * caller's answer is to emit no `file:` line for it rather than to invent
 * one.
 */
bool mpduri_from_vfs(const char *vfs, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
