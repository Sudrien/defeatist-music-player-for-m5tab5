/*
 * hlsplay.h -- an HLS playlist, read: which variant, which segment next.
 *
 * HTTP Live Streaming (RFC 8216) replaces the one endless body a radio
 * station normally sends with a text file that names short files, each
 * a few seconds of audio, and is rewritten every few seconds as new ones
 * are made. This file is the reading of that text and nothing else: no
 * HTTP, no FreeRTOS, no allocation. netstream fetches; this decides.
 *
 *     master playlist  --hls_pick_variant()-->  one media playlist URL
 *     media playlist   --hls_parse_media()--->  hls_media_t (segments)
 *     hls_media_t      --hls_next()---------->  the segment to fetch now
 *
 * WHAT IS KEPT, AND WHERE
 *
 * Segments are offsets into the caller's text, not copies. A live audio
 * playlist is a handful of lines per segment and the window is a minute
 * or so, so the text is a few KB and the caller already holds it; a copy
 * of every URI would be NETSTREAM_URL_MAX times HLS_SEG_MAX for nothing.
 * The consequence is the rule: **an hls_media_t is valid only while the
 * text it was parsed from is unchanged.** Resolve the URI you want
 * (hls_resolve()) before the next reload overwrites the buffer.
 *
 * hls_media_t is 2328 bytes on the host. CLAUDE.md: nothing over a few hundred
 * bytes on a task stack -- netstream keeps it at module scope.
 *
 * WHAT IS REFUSED, BY NAME
 *
 * Encrypted segments (EXT-X-KEY other than NONE) and fragmented MP4
 * (EXT-X-MAP) are reported as such rather than fetched and then failing
 * in the decoder as "unrecognised stream". Both are possible later; both
 * are a different patch.
 *
 * THE LIVE EDGE
 *
 * A live playlist is a sliding window. The spec says a client should not
 * start within three target durations of the end; starting three
 * segments back is that. After that the cursor holds a media sequence
 * number, not an index, because the index of a segment changes every
 * reload and its sequence number does not. Three things can happen to
 * that number against a fresh playlist, and hls_next() says which:
 *
 *   - it is in the window: play it;
 *   - it is past the end: wait for the next reload;
 *   - it fell off the front, because we were slower than the server
 *     for longer than the window: jump to the oldest segment still
 *     listed, and report how many were lost, so the log says "gap"
 *     instead of the audio quietly skipping.
 *
 * And a fourth that is not the client's fault: an encoder restart resets
 * the media sequence. A number far ahead of anything the server lists is
 * taken as that, and the cursor starts again at the live edge.
 *
 * Host-tested: texttest/hlsplaytest.c, under ASan and UBSan.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Segments kept from one media playlist. Live audio windows are 3-20
 * segments; a longer list keeps its last HLS_SEG_MAX, which is the end
 * a live client wants. */
#define HLS_SEG_MAX         (96)

/* Segments back from the end to start a live stream at. RFC 8216 6.3.3. */
#define HLS_START_BACK      (3)

/* A target duration outside this is treated as damage and clamped, so a
 * playlist claiming 0 s does not become a reload loop and one claiming an
 * hour does not become a hang. */
#define HLS_TARGET_MIN_MS   (1000)
#define HLS_TARGET_MAX_MS   (60000)

typedef enum {
    HLS_NOT = 0,        /* no #EXTM3U first line: not HLS at all */
    HLS_MASTER,         /* variants: pick one, fetch it */
    HLS_MEDIA,          /* segments */
} hls_kind_t;

typedef enum {
    HLS_OK = 0,
    HLS_ERR_NOT_HLS,        /* not a playlist */
    HLS_ERR_EMPTY,          /* a playlist with nothing playable in it */
    HLS_ERR_KEY,            /* encrypted segments */
    HLS_ERR_FMP4,           /* EXT-X-MAP: fragmented MP4 */
    HLS_ERR_URL,            /* a URI that will not resolve or fit */
} hls_err_t;

static inline const char *hls_err_name(hls_err_t e)
{
    switch (e) {
    case HLS_OK:          return "ok";
    case HLS_ERR_NOT_HLS: return "not an HLS playlist";
    case HLS_ERR_EMPTY:   return "no segments";
    case HLS_ERR_KEY:     return "encrypted HLS is not supported yet";
    case HLS_ERR_FMP4:    return "HLS in MP4 fragments is not supported yet";
    case HLS_ERR_URL:     return "a segment link is too long or malformed";
    default:              return "?";
    }
}

typedef struct {
    uint32_t off, len;      /* the URI line, in the text */
    uint32_t title_off, title_len;  /* EXTINF's title, may be 0 long */
    uint32_t dur_ms;
    bool     disc;          /* EXT-X-DISCONTINUITY before this segment */
} hls_seg_t;

typedef struct {
    uint64_t  first_seq;    /* media sequence of seg[0] */
    uint32_t  target_ms;    /* clamped */
    int       n;
    uint32_t  dropped;      /* segments before seg[0] not kept */
    bool      endlist;      /* EXT-X-ENDLIST: not live, plays to the end */
    hls_seg_t seg[HLS_SEG_MAX];
} hls_media_t;

/* ------------------------------------------------------------------ */
/* Lines                                                               */
/* ------------------------------------------------------------------ */

/* The next line from *pos, without its CR/LF. False at the end. */
static inline bool hls_line(const char *t, size_t len, size_t *pos,
                            size_t *lo, size_t *ln)
{
    size_t p = *pos;
    if (p >= len) return false;
    const size_t s = p;
    while (p < len && t[p] != '\n' && t[p] != '\r' && t[p] != '\0') p++;
    size_t e = p;
    if (p < len && t[p] == '\0') { *pos = len; }   /* a NUL ends the text */
    else {
        if (p < len && t[p] == '\r') p++;
        if (p < len && t[p] == '\n') p++;
        *pos = p;
    }
    /* Trailing blanks are an editor's, not the server's meaning. */
    while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
    *lo = s;
    *ln = e - s;
    return true;
}

static inline bool hls_starts(const char *t, size_t lo, size_t ln, const char *tag)
{
    const size_t n = strlen(tag);
    return ln >= n && memcmp(t + lo, tag, n) == 0;
}

/* What the text is. A UTF-8 BOM before #EXTM3U is allowed. */
static inline hls_kind_t hls_kind(const char *t, size_t len)
{
    if (!t) return HLS_NOT;
    size_t pos = 0, lo, ln;
    if (len >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB &&
        (unsigned char)t[2] == 0xBF) pos = 3;
    if (!hls_line(t, len, &pos, &lo, &ln) || !hls_starts(t, lo, ln, "#EXTM3U"))
        return HLS_NOT;
    while (hls_line(t, len, &pos, &lo, &ln)) {
        if (hls_starts(t, lo, ln, "#EXT-X-STREAM-INF:")) return HLS_MASTER;
        if (hls_starts(t, lo, ln, "#EXTINF:") ||
            hls_starts(t, lo, ln, "#EXT-X-TARGETDURATION:")) return HLS_MEDIA;
    }
    /* #EXTM3U and nothing either way: a media playlist with no segments
     * yet, which a live encoder can serve for its first seconds. */
    return HLS_MEDIA;
}

/* Whether text that sniffed as M3U carries HLS tags. For the codec
 * decision, which sees only the first bytes: any #EXT-X- tag is HLS, and
 * plain #EXTM3U/#EXTINF lists of station URLs are not. */
static inline bool hls_looks_like(const char *t, size_t len)
{
    if (!t || len < 7) return false;
    for (size_t i = 0; i + 6 <= len; i++)
        if (memcmp(t + i, "#EXT-X-", 6) == 0) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Numbers and attributes                                              */
/* ------------------------------------------------------------------ */

/* Decimal seconds ("9.984", "10") to ms. Stops at the first non-digit
 * after the fraction. Saturates rather than overflowing. */
static inline uint32_t hls_secs_ms(const char *t, size_t lo, size_t ln)
{
    uint64_t ms = 0;
    size_t i = 0;
    while (i < ln && t[lo + i] == ' ') i++;
    for (; i < ln && t[lo + i] >= '0' && t[lo + i] <= '9'; i++) {
        ms = ms * 10 + (uint64_t)(t[lo + i] - '0');
        if (ms > 100000000u) return UINT32_MAX;
    }
    ms *= 1000;
    if (i < ln && t[lo + i] == '.') {
        i++;
        uint32_t scale = 100;
        for (; i < ln && t[lo + i] >= '0' && t[lo + i] <= '9'; i++) {
            ms += (uint64_t)(t[lo + i] - '0') * scale;
            scale /= 10;
        }
    }
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static inline uint64_t hls_u64(const char *t, size_t lo, size_t ln)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < ln && t[lo + i] == ' ') i++;
    for (; i < ln && t[lo + i] >= '0' && t[lo + i] <= '9'; i++) {
        const uint64_t d = (uint64_t)(t[lo + i] - '0');
        if (v > (UINT64_MAX - d) / 10) return UINT64_MAX;
        v = v * 10 + d;
    }
    return v;
}

/*
 * An attribute's value from an attribute list ("A=1,B="x,y",C=z").
 * Quoted values may contain commas -- CODECS always does when there is
 * more than one -- so this walks the list rather than splitting on ','.
 * Names match whole: asking for "AUDIO" does not find "AUDIO-ONLY".
 * The value comes back without its quotes. False if absent.
 */
static inline bool hls_attr(const char *t, size_t lo, size_t ln, const char *name,
                            size_t *vo, size_t *vl)
{
    const size_t nn = strlen(name);
    size_t i = 0;
    while (i < ln) {
        while (i < ln && (t[lo + i] == ',' || t[lo + i] == ' ')) i++;
        const size_t ks = i;
        while (i < ln && t[lo + i] != '=' && t[lo + i] != ',') i++;
        const size_t kl = i - ks;
        if (i >= ln || t[lo + i] != '=') continue;
        i++;                                    /* '=' */
        size_t s, e;
        if (i < ln && t[lo + i] == '"') {
            s = ++i;
            while (i < ln && t[lo + i] != '"') i++;
            e = i;
            if (i < ln) i++;                    /* closing quote */
        } else {
            s = i;
            while (i < ln && t[lo + i] != ',') i++;
            e = i;
        }
        if (kl == nn && memcmp(t + lo + ks, name, nn) == 0) {
            *vo = lo + s;
            *vl = e - s;
            return true;
        }
    }
    return false;
}

static inline bool hls_val_is(const char *t, size_t vo, size_t vl, const char *s)
{
    const size_t n = strlen(s);
    return vl == n && memcmp(t + vo, s, n) == 0;
}

/* ------------------------------------------------------------------ */
/* URLs                                                                */
/* ------------------------------------------------------------------ */

/*
 * `ref` (ref_len bytes, not terminated) against `base`, into `out`.
 *
 *   "https://x/y"   absolute: copied
 *   "//x/y"         the base's scheme
 *   "/y"            the base's scheme and authority
 *   "y", "../y"     beside the base's last '/', with ".." walked back
 *
 * The base's query and fragment never carry onto a relative reference,
 * and the reference's own query is kept: tokenised CDNs put the token on
 * every segment URI, and losing it is a 403 per segment.
 *
 * False if anything will not fit or the base has no scheme; `out` is
 * then empty, never a cut URL. A cut URL fetches the wrong file.
 */
static inline bool hls_resolve(const char *base, const char *ref, size_t ref_len,
                               char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!base || !ref || ref_len == 0) return false;

    /* Absolute: a scheme is letters then "://". */
    for (size_t i = 0; i < ref_len && i < 16; i++) {
        const char c = ref[i];
        if (c == ':' && i > 0 && i + 2 < ref_len && ref[i + 1] == '/' && ref[i + 2] == '/') {
            if (ref_len + 1 > out_size) return false;
            memcpy(out, ref, ref_len);
            out[ref_len] = '\0';
            return true;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) break;
    }

    const char *sch = strstr(base, "://");
    if (!sch) return false;
    const size_t scheme_end = (size_t)(sch - base) + 1;       /* includes ':' */
    const size_t auth_start = scheme_end + 2;
    size_t auth_end = auth_start;
    while (base[auth_end] && base[auth_end] != '/' && base[auth_end] != '?' &&
           base[auth_end] != '#') auth_end++;

    size_t keep;                    /* bytes of base to keep before ref */
    size_t skip = 0;                /* bytes of ref already consumed */
    if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/') {
        keep = scheme_end;
    } else if (ref[0] == '/') {
        keep = auth_end;
    } else {
        /* The base's path, up to and including its last '/'. */
        size_t path_end = auth_end;
        while (base[path_end] && base[path_end] != '?' && base[path_end] != '#') path_end++;
        keep = auth_end;
        for (size_t i = auth_end; i < path_end; i++)
            if (base[i] == '/') keep = i + 1;
        if (keep == auth_end) {
            /* "https://host" with no path: the reference hangs off "/". */
            if (keep + 1 + ref_len + 1 > out_size) return false;
            memcpy(out, base, keep);
            out[keep] = '/';
            memcpy(out + keep + 1, ref, ref_len);
            out[keep + 1 + ref_len] = '\0';
            return true;
        }
        /* "./" is nothing; each "../" drops one directory, never past
         * the authority. */
        for (;;) {
            if (ref_len - skip >= 2 && ref[skip] == '.' && ref[skip + 1] == '/') {
                skip += 2;
            } else if (ref_len - skip >= 3 && ref[skip] == '.' && ref[skip + 1] == '.' &&
                       ref[skip + 2] == '/') {
                skip += 3;
                if (keep > auth_end + 1) {
                    size_t k = keep - 1;                /* the '/' ending keep */
                    while (k > auth_end && base[k - 1] != '/') k--;
                    keep = k;
                }
            } else {
                break;
            }
        }
    }
    const size_t rest = ref_len - skip;
    if (keep + rest + 1 > out_size) return false;
    memcpy(out, base, keep);
    memcpy(out + keep, ref + skip, rest);
    out[keep + rest] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Master playlist                                                     */
/* ------------------------------------------------------------------ */

/* Video codecs by RFC 6381 prefix. Anything else in CODECS is audio
 * (mp4a, ac-3, ec-3, mp3, opus, flac) or text, and neither costs a
 * radio the bandwidth a picture does. */
static inline bool hls_codecs_have_video(const char *t, size_t vo, size_t vl)
{
    static const char *const video[] = { "avc1", "avc3", "hvc1", "hev1", "vp09",
                                         "vp08", "av01", "dvh1", "dvhe", "mp4v" };
    size_t i = 0;
    while (i < vl) {
        while (i < vl && (t[vo + i] == ',' || t[vo + i] == ' ')) i++;
        const size_t s = i;
        while (i < vl && t[vo + i] != ',') i++;
        for (size_t k = 0; k < sizeof(video) / sizeof(video[0]); k++) {
            const size_t n = strlen(video[k]);
            if (i - s >= n && memcmp(t + vo + s, video[k], n) == 0) return true;
        }
    }
    return false;
}

typedef struct {
    uint32_t uri_off, uri_len;  /* the chosen playlist's URI, in the text */
    uint32_t bandwidth;
    int      variants;          /* how many STREAM-INF lines were seen */
    bool     audio_only;        /* by CODECS; false when CODECS was absent */
    bool     rendition;         /* the URI is an EXT-X-MEDIA audio rendition */
} hls_choice_t;

/*
 * Which variant a radio should play.
 *
 * In order of preference:
 *   1. the lowest-bandwidth variant whose CODECS lists no video;
 *   2. failing that, an audio rendition (EXT-X-MEDIA TYPE=AUDIO with a
 *      URI) -- a TV-style stream whose sound is a separate playlist,
 *      DEFAULT=YES first -- because that is the audio without the
 *      picture;
 *   3. failing that, the lowest-bandwidth variant with no RESOLUTION,
 *      which is usually audio that did not declare CODECS;
 *   4. failing that, the lowest-bandwidth variant at all. Its segments
 *      carry video too and the demuxer will have to find the audio.
 *
 * Lowest rather than highest because the link measured 0.97x beside
 * card playback, and an HLS client that cannot keep up does not stutter,
 * it falls off the window and loses whole segments.
 */
static inline hls_err_t hls_pick_variant(const char *t, size_t len, hls_choice_t *c)
{
    memset(c, 0, sizeof(*c));
    if (hls_kind(t, len) != HLS_MASTER) return HLS_ERR_NOT_HLS;

    /* Best of each preference class: offset/len/bandwidth. */
    struct { bool have; uint32_t off, len, bw; } cls[4];
    memset(cls, 0, sizeof(cls));
    bool rend_default = false;

    size_t pos = 0, lo, ln;
    bool pending = false;
    uint32_t p_bw = 0;
    int p_cls = 3;
    while (hls_line(t, len, &pos, &lo, &ln)) {
        if (ln == 0) continue;
        if (hls_starts(t, lo, ln, "#EXT-X-STREAM-INF:")) {
            const size_t a = lo + 18, al = ln - 18;
            size_t vo, vl;
            p_bw = hls_attr(t, a, al, "BANDWIDTH", &vo, &vl)
                 ? (uint32_t)hls_u64(t, vo, vl) : UINT32_MAX;
            if (hls_attr(t, a, al, "CODECS", &vo, &vl))
                p_cls = hls_codecs_have_video(t, vo, vl) ? 3 : 0;
            else
                p_cls = hls_attr(t, a, al, "RESOLUTION", &vo, &vl) ? 3 : 2;
            pending = true;
            c->variants++;
            continue;
        }
        if (hls_starts(t, lo, ln, "#EXT-X-MEDIA:")) {
            const size_t a = lo + 13, al = ln - 13;
            size_t vo, vl, uo, ul;
            if (hls_attr(t, a, al, "TYPE", &vo, &vl) && hls_val_is(t, vo, vl, "AUDIO") &&
                hls_attr(t, a, al, "URI", &uo, &ul) && ul > 0) {
                const bool dflt = hls_attr(t, a, al, "DEFAULT", &vo, &vl) &&
                                  hls_val_is(t, vo, vl, "YES");
                if (!cls[1].have || (dflt && !rend_default)) {
                    cls[1].have = true;
                    cls[1].off = (uint32_t)uo;
                    cls[1].len = (uint32_t)ul;
                    cls[1].bw = 0;
                    rend_default = dflt;
                }
            }
            continue;
        }
        if (t[lo] == '#') continue;
        if (pending) {
            /* The URI line after STREAM-INF. */
            if (!cls[p_cls].have || p_bw < cls[p_cls].bw) {
                cls[p_cls].have = true;
                cls[p_cls].off = (uint32_t)lo;
                cls[p_cls].len = (uint32_t)ln;
                cls[p_cls].bw = p_bw;
            }
            pending = false;
        }
    }
    for (int k = 0; k < 4; k++) {
        if (!cls[k].have) continue;
        c->uri_off = cls[k].off;
        c->uri_len = cls[k].len;
        c->bandwidth = cls[k].bw == UINT32_MAX ? 0 : cls[k].bw;
        c->audio_only = (k == 0);
        c->rendition = (k == 1);
        return HLS_OK;
    }
    return HLS_ERR_EMPTY;
}

/* ------------------------------------------------------------------ */
/* Media playlist                                                      */
/* ------------------------------------------------------------------ */

static inline hls_err_t hls_parse_media(const char *t, size_t len, hls_media_t *m)
{
    memset(m, 0, sizeof(*m));
    m->target_ms = 10000;               /* RFC's typical, if it is missing */
    const hls_kind_t k = hls_kind(t, len);
    if (k != HLS_MEDIA) return HLS_ERR_NOT_HLS;

    size_t pos = 0, lo, ln;
    uint64_t seq0 = 0;
    uint64_t count = 0;                 /* segments seen, kept or not */
    bool     have_inf = false, disc = false;
    uint32_t dur = 0, t_off = 0, t_len = 0;
    uint32_t ring_start = 0;            /* index of the oldest kept, when full */

    while (hls_line(t, len, &pos, &lo, &ln)) {
        if (ln == 0) continue;
        if (t[lo] == '#') {
            if (hls_starts(t, lo, ln, "#EXTINF:")) {
                dur = hls_secs_ms(t, lo + 8, ln - 8);
                t_off = t_len = 0;
                for (size_t i = 8; i < ln; i++) {
                    if (t[lo + i] == ',') {
                        t_off = (uint32_t)(lo + i + 1);
                        t_len = (uint32_t)(ln - i - 1);
                        break;
                    }
                }
                have_inf = true;
            } else if (hls_starts(t, lo, ln, "#EXT-X-TARGETDURATION:")) {
                uint64_t s = hls_u64(t, lo + 22, ln - 22);
                uint64_t ms = s > HLS_TARGET_MAX_MS ? HLS_TARGET_MAX_MS : s * 1000;
                if (ms < HLS_TARGET_MIN_MS) ms = HLS_TARGET_MIN_MS;
                if (ms > HLS_TARGET_MAX_MS) ms = HLS_TARGET_MAX_MS;
                m->target_ms = (uint32_t)ms;
            } else if (hls_starts(t, lo, ln, "#EXT-X-MEDIA-SEQUENCE:")) {
                seq0 = hls_u64(t, lo + 22, ln - 22);
            } else if (hls_starts(t, lo, ln, "#EXT-X-DISCONTINUITY") &&
                       !hls_starts(t, lo, ln, "#EXT-X-DISCONTINUITY-")) {
                disc = true;
            } else if (hls_starts(t, lo, ln, "#EXT-X-ENDLIST")) {
                m->endlist = true;
            } else if (hls_starts(t, lo, ln, "#EXT-X-KEY:")) {
                size_t vo, vl;
                if (hls_attr(t, lo + 11, ln - 11, "METHOD", &vo, &vl) &&
                    !hls_val_is(t, vo, vl, "NONE")) return HLS_ERR_KEY;
            } else if (hls_starts(t, lo, ln, "#EXT-X-MAP:")) {
                return HLS_ERR_FMP4;
            }
            continue;
        }
        /* A URI. One without an EXTINF is not a segment by the spec, but
         * is kept anyway with the target as its length: refusing it
         * would be pedantry against a server that otherwise works. */
        hls_seg_t s = {
            .off = (uint32_t)lo, .len = (uint32_t)ln,
            .title_off = t_off, .title_len = t_len,
            .dur_ms = have_inf ? dur : m->target_ms,
            .disc = disc,
        };
        if (m->n < HLS_SEG_MAX) {
            m->seg[m->n++] = s;
        } else {
            /* Full: overwrite the oldest, then straighten at the end. */
            m->seg[ring_start] = s;
            ring_start = (ring_start + 1) % HLS_SEG_MAX;
            m->dropped++;
        }
        count++;
        have_inf = disc = false;
        t_off = t_len = 0;
    }
    if (ring_start) {
        /* Rotate left by ring_start, in place, three reversals. */
        hls_seg_t *a = m->seg;
        const int n = m->n, r = (int)ring_start;
        for (int pass = 0; pass < 3; pass++) {
            int lo2 = pass == 0 ? 0 : pass == 1 ? r : 0;
            int hi2 = pass == 0 ? r - 1 : pass == 1 ? n - 1 : n - 1;
            while (lo2 < hi2) {
                hls_seg_t tmp = a[lo2]; a[lo2] = a[hi2]; a[hi2] = tmp;
                lo2++; hi2--;
            }
        }
    }
    m->first_seq = seq0 + m->dropped;
    (void)count;
    return m->n > 0 || !m->endlist ? HLS_OK : HLS_ERR_EMPTY;
}

/* ------------------------------------------------------------------ */
/* The cursor                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     started;
    uint64_t next_seq;      /* the media sequence number to play next */
} hls_cursor_t;

typedef enum {
    HLS_NEXT_PLAY = 0,      /* *index is the segment to fetch */
    HLS_NEXT_WAIT,          /* nothing new: reload after hls_reload_ms() */
    HLS_NEXT_ENDED,         /* ENDLIST and every segment played */
} hls_next_t;

/*
 * The segment to fetch now, against a freshly parsed playlist.
 *
 * `*lost` is set to the number of segments skipped because the cursor
 * fell off the front of the window (0 normally), and `*restarted` to
 * true when the sequence looked reset and the cursor went back to the
 * live edge. Both are for the log; the decision is already made.
 *
 * Call hls_advance() after the segment has been delivered, not before:
 * a fetch that fails should be retried against the next reload, and a
 * cursor already moved past it would skip it silently.
 */
static inline hls_next_t hls_next(hls_cursor_t *cur, const hls_media_t *m,
                                  int *index, uint64_t *lost, bool *restarted)
{
    *index = -1;
    if (lost) *lost = 0;
    if (restarted) *restarted = false;
    if (m->n == 0) return m->endlist ? HLS_NEXT_ENDED : HLS_NEXT_WAIT;

    const uint64_t first = m->first_seq;
    const uint64_t end = first + (uint64_t)m->n;            /* one past */

    if (!cur->started) {
        cur->started = true;
        const int back = m->endlist ? m->n : HLS_START_BACK;
        cur->next_seq = m->n > back ? end - (uint64_t)back : first;
    } else if (cur->next_seq > end + (uint64_t)m->n) {
        /* Far past anything listed: the encoder restarted its count. */
        if (restarted) *restarted = true;
        cur->next_seq = m->n > HLS_START_BACK ? end - HLS_START_BACK : first;
    } else if (cur->next_seq < first) {
        if (lost) *lost = first - cur->next_seq;
        cur->next_seq = first;
    }

    if (cur->next_seq >= end) return m->endlist ? HLS_NEXT_ENDED : HLS_NEXT_WAIT;
    *index = (int)(cur->next_seq - first);
    return HLS_NEXT_PLAY;
}

static inline void hls_advance(hls_cursor_t *cur) { cur->next_seq++; }

/*
 * How long to wait before reloading, in ms. RFC 8216 6.3.4: after a
 * reload that brought something new, the target duration; after one that
 * did not, half of it, because the server is due.
 */
static inline uint32_t hls_reload_ms(const hls_media_t *m, bool changed)
{
    return changed ? m->target_ms : m->target_ms / 2;
}
