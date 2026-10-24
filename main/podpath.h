/*
 * podpath.h -- where a downloaded episode goes, and what it is called.
 *
 * 8020. <volume>/Podcasts/<podcast>/<YYYY-MM-DD> <episode>.<ext>
 *
 * The date first so the folder sorts oldest to newest, which is the
 * order a series is listened in -- the chooser sorts names, and the
 * date is the only part of a name that sorts that way. An episode with
 * no date has none, and sorts after the dated ones only by luck.
 *
 * Names come from publishers, so they are made safe for FAT and exFAT:
 * the nine characters neither allows ( \ / : * ? " < > | ) and control
 * characters become '_', leading spaces and trailing dots and spaces
 * go, and each name is cut at PODPATH_NAME_MAX bytes on a UTF-8
 * boundary. A name with nothing left is "_".
 *
 * The extension is the URL's, when it is one this player plays, else
 * the Content-Type's, else mp3 -- which is what most podcasts are, and
 * the decoder goes by the bytes anyway.
 *
 * NO FREERTOS, NO FILESYSTEM, so podpathtest covers all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PODPATH_NAME_MAX 96         /* bytes per folder or file name, before the extension */
#define PODPATH_DIR      "Podcasts"

/* Make `in` a safe file name into `out` (at least PODPATH_NAME_MAX + 1). */
static inline void podpath_name(const char *in, char *out, size_t cap)
{
    if (!out || cap < 2) return;
    size_t lim = cap - 1 < PODPATH_NAME_MAX ? cap - 1 : PODPATH_NAME_MAX;
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p == ' ') p++;
    while (*p) {
        const unsigned char c = *p;
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        for (size_t j = 1; j < k; j++) if (!p[j]) { k = j; break; }
        if (n + k > lim) break;                 /* cut, never split a letter */
        if (k == 1 && (c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c))) out[n++] = '_';
        else { memcpy(out + n, p, k); n += k; }
        p += k;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    if (n == 0) out[n++] = '_';
    out[n] = '\0';
}

static inline bool podpath_ext_ok(const char *e)
{
    static const char *const ok[] = { "mp3", "m4a", "mp4", "aac", "ogg", "oga", "opus", "flac", "wav" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (!strcasecmp(e, ok[i])) return true;
    return false;
}

/* The extension of the URL's path (no query, no fragment), lower case,
 * if it is one podpath_ext_ok() takes; else "". */
static inline void podpath_url_ext(const char *url, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!url) return;
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    if (!path) return;
    size_t len = strcspn(path, "?#");
    const char *dot = NULL;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') dot = NULL;
        else if (path[i] == '.') dot = path + i;
    }
    if (!dot) return;
    const size_t el = (size_t)(path + len - dot - 1);
    if (el == 0 || el >= cap || el > 4) return;
    char e[8];
    for (size_t i = 0; i < el; i++) {
        const char c = dot[1 + i];
        e[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    e[el] = '\0';
    if (podpath_ext_ok(e)) snprintf(out, cap, "%s", e);
}

/* From a Content-Type, or "" for one this does not know. */
static inline const char *podpath_type_ext(const char *ctype)
{
    if (!ctype) return "";
    static const struct { const char *t, *e; } map[] = {
        { "audio/mpeg", "mp3" }, { "audio/mp3", "mp3" }, { "audio/x-mpeg", "mp3" },
        { "audio/mp4", "m4a" }, { "audio/x-m4a", "m4a" }, { "audio/m4a", "m4a" },
        { "video/mp4", "mp4" }, { "audio/aac", "aac" }, { "audio/aacp", "aac" },
        { "audio/ogg", "ogg" }, { "audio/opus", "opus" }, { "audio/flac", "flac" },
        { "audio/wav", "wav" }, { "audio/x-wav", "wav" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const size_t tl = strlen(map[i].t);
        if (!strncasecmp(ctype, map[i].t, tl) && (ctype[tl] == '\0' || ctype[tl] == ';' || ctype[tl] == ' '))
            return map[i].e;
    }
    return "";
}

/* "<mount>/Podcasts/<podcast>" into `out`. False if it does not fit. */
static inline bool podpath_dir(const char *mount, const char *podcast, char *out, size_t cap)
{
    char p[PODPATH_NAME_MAX + 1];
    podpath_name(podcast, p, sizeof(p));
    const int n = snprintf(out, cap, "%s/" PODPATH_DIR "/%s", mount, p);
    return n > 0 && (size_t)n < cap;
}

/* "<YYYY-MM-DD> <episode>" (no extension) into `out`. UTC: the date the
 * episode was published, wherever the listener is. */
static inline void podpath_base(int64_t published, const char *episode, char *out, size_t cap)
{
    char t[PODPATH_NAME_MAX + 1];
    podpath_name(episode, t, sizeof(t));
    if (published > 0) {
        /* Days to a civil date (Howard Hinnant's days_from_civil, run
         * backwards), so this needs no gmtime_r() and no time zone. */
        const int64_t z = published / 86400 + 719468;
        const int64_t era = z / 146097;
        const int64_t doe = z - era * 146097;
        const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int64_t mp = (5 * doy + 2) / 153;
        const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
        const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
        const int y = (int)(yoe + era * 400 + (m <= 2));
        snprintf(out, cap, "%04d-%02d-%02d %s", y, m, d, t);
    } else {
        snprintf(out, cap, "%s", t);
    }
}

/*
 * 8021: the podcast a folder's episodes came from, kept beside them in
 * PODPATH_LINK -- a dotfile, so the chooser does not list it -- so the
 * folder can lead back to the podcast's episode list. Two lines:
 *
 *   feed <Podcast Index feed id>
 *   <the podcast's title>
 */
#define PODPATH_LINK ".podcast"

static inline int podpath_link_format(int64_t feed, const char *title, char *out, size_t cap)
{
    return snprintf(out, cap, "feed %lld\n%s\n", (long long)feed, title ? title : "");
}

/* False unless `text` is that shape with a positive id; the title is
 * the second line, cut to fit, and may be empty. */
static inline bool podpath_link_parse(const char *text, size_t n, int64_t *feed,
                                      char *title, size_t cap)
{
    if (!text || n < 6 || memcmp(text, "feed ", 5) != 0) return false;
    size_t i = 5;
    int64_t id = 0;
    while (i < n && text[i] >= '0' && text[i] <= '9' && id < 100000000000LL) id = id * 10 + (text[i++] - '0');
    if (id <= 0 || (i < n && text[i] != '\n' && text[i] != '\r')) return false;
    while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;
    size_t t = 0;
    if (title && cap) {
        while (i < n && text[i] != '\n' && text[i] != '\r' && t + 1 < cap) title[t++] = text[i++];
        /* Never end on half a letter: if the last letter's lead byte
         * promises more bytes than are there, drop it. */
        size_t lead = t;
        while (lead > 0 && ((unsigned char)title[lead - 1] & 0xC0) == 0x80) lead--;
        if (lead > 0) {
            const unsigned char c = (unsigned char)title[lead - 1];
            const size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (t - (lead - 1) < k) t = lead - 1;
        }
        title[t] = '\0';
    }
    if (feed) *feed = id;
    return true;
}
