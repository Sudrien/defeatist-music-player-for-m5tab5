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

/* The second line of a link, from `i`, into `title`: cut to fit, never on
 * half a letter, and empty if there is none. */
static inline void podpath_link_title(const char *text, size_t n, size_t i, char *title, size_t cap)
{
    if (!title || !cap) return;
    while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;
    size_t t = 0;
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
    podpath_link_title(text, n, i, title, cap);
    if (feed) *feed = id;
    return true;
}

/*
 * 8129: a podcast the index does not list is kept by its feed's address,
 * in the same file and the same two lines:
 *
 *   url <the feed's address>
 *   <the podcast's title>
 *
 * The address is one unbroken http or https line (no space or control
 * character), which is also what the remote's field accepts. A negative
 * return is a refusal to write one that is not.
 */
#define PODPATH_LINK_URL_MAX 400

static inline int podpath_link_format_url(const char *url, const char *title, char *out, size_t cap)
{
    if (!url) return -1;
    const size_t n = strlen(url);
    const size_t h = !strncmp(url, "https://", 8) ? 8 : !strncmp(url, "http://", 7) ? 7 : 0;
    if (!h || n <= h || n >= PODPATH_LINK_URL_MAX) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)url[i] <= ' ' || url[i] == 0x7f) return -1;
    return snprintf(out, cap, "url %s\n%s\n", url, title ? title : "");
}

/* Either kind. True with *feed > 0 and url[0] == '\0' for "feed N", or
 * *feed == 0 and the address in `url` for "url ..."; False for neither.
 * `url` may be NULL when only the feed form matters (the address is then
 * not copied, but still checked). */
static inline bool podpath_link_parse_any(const char *text, size_t n, int64_t *feed,
                                          char *url, size_t url_cap, char *title, size_t cap)
{
    if (url && url_cap) url[0] = '\0';
    if (podpath_link_parse(text, n, feed, title, cap)) return true;
    if (!text || n < 12 || memcmp(text, "url ", 4) != 0) return false;
    size_t i = 4, e = 4;
    while (e < n && text[e] != '\n' && text[e] != '\r') e++;
    const size_t len = e - i;
    if (len < 8 || len >= PODPATH_LINK_URL_MAX) return false;
    char a[PODPATH_LINK_URL_MAX];
    memcpy(a, text + i, len);
    a[len] = '\0';
    const size_t h = !strncmp(a, "https://", 8) ? 8 : !strncmp(a, "http://", 7) ? 7 : 0;
    if (!h || len <= h) return false;
    for (size_t k = 0; k < len; k++) if ((unsigned char)a[k] <= ' ' || a[k] == 0x7f) return false;
    if (url && url_cap) {
        if (len >= url_cap) return false;
        memcpy(url, a, len + 1);
    }
    podpath_link_title(text, n, e, title, cap);
    if (feed) *feed = 0;
    return true;
}

/*
 * 8029: where an episode is while it downloads -- not in its podcast's
 * folder, which is made only when an episode is whole, so a cancel or a
 * failure leaves no folder behind. "<mount>/Podcasts/<podcast>" and a
 * base give "<mount>/Podcasts/.partial/<podcast>/<base>.part" (a dotted
 * folder, hidden as every dotfile is), and with base NULL the folder
 * alone. False, `out` empty, if `dir` has no '/' or it will not fit.
 */
#define PODPATH_PARTIAL ".partial"

static inline bool podpath_part(const char *dir, const char *base, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!dir) return false;
    const char *slash = strrchr(dir, '/');
    if (!slash || !slash[1]) return false;
    const int n = base
        ? snprintf(out, cap, "%.*s/" PODPATH_PARTIAL "/%s/%s.part", (int)(slash - dir), dir, slash + 1, base)
        : snprintf(out, cap, "%.*s/" PODPATH_PARTIAL "/%s", (int)(slash - dir), dir, slash + 1);
    if (n <= 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}
