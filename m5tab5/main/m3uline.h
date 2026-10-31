/*
 * m3uline.h -- one line of a playlist file, as written by something else.
 *
 * 5198. The player writes its own .m3u files (starred, stations, MPD's
 * stored playlists) and reads them back, so until now nothing had to
 * cope with what other programs write. Playlists exported from a desktop
 * player differ in three ways this fixes, and one it does not:
 *
 *  - THE EXTENSION. `.m3u8` is the same format, declared UTF-8. It is
 *    listed and loaded like `.m3u` (m3u_is_name()).
 *
 *  - THE SEPARATOR. Windows players write `Album\01.mp3`, and some
 *    escape it, `Album\\01.mp3`. A run of backslashes is one '/'. FAT
 *    and exFAT cannot hold a backslash in a name, so no real path is
 *    lost by this.
 *
 *  - THE ENCODING. `.m3u8` is UTF-8 by its name. A plain `.m3u` has no
 *    declared encoding and, historically, was Latin-1 (Winamp wrote the
 *    system code page); `#EXTENC:` is the extended-M3U directive that
 *    declares one, and is honoured when present. Without either, a line
 *    that is valid UTF-8 is taken as UTF-8 -- which is what every modern
 *    writer, this player included, produces -- and a line that is not is
 *    taken as Latin-1 and converted, since Latin-1 is the one legacy
 *    encoding in which every byte means something. Other code pages
 *    (1252's curly quotes aside, which land as C1 controls) are not
 *    guessed at.
 *
 *  - NOT FIXED: a line relative to the playlist's own folder. Lines are
 *    still read from a volume's root (or as `sd/...`, `usb/...`, or a
 *    VFS path), which is what this player writes.
 *
 * PURE: no filesystem, no IDF; host-tested in texttest/m3ulinetest.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    M3U_ENC_UNKNOWN = 0,    /* guess per line: UTF-8 if valid, else Latin-1 */
    M3U_ENC_UTF8,
    M3U_ENC_LATIN1,
} m3u_enc_t;

/* Whether a file name ends in .m3u or .m3u8, any case. */
bool m3u_is_name(const char *name);

/* A file's encoding from its name: UTF-8 for .m3u8, unknown otherwise. */
m3u_enc_t m3u_enc_of_name(const char *name);

/*
 * An `#EXTENC:` line -- "#EXTENC: UTF-8", "#EXTENC:ISO-8859-1" -- sets
 * *enc and returns true. Any other line returns false and leaves *enc.
 * Call it on the line as read, before m3u_line_clean().
 */
bool m3u_directive(const char *line, m3u_enc_t *enc);

/*
 * Clean one line in place: the line ending and trailing blanks gone, a
 * UTF-8 byte-order mark at its start gone (a file's first line), every
 * run of backslashes one '/', and the bytes converted to UTF-8 per `enc`.
 * `cap` is the buffer's size; Latin-1 can double a line's length. The
 * new length, or -1 when the converted line would not fit -- a line cut
 * short would name a different file.
 */
int m3u_line_clean(char *line, size_t cap, m3u_enc_t enc);

#ifdef __cplusplus
}
#endif
