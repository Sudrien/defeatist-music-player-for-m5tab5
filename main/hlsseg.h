/*
 * hlsseg.h -- one HLS segment's bytes, unwrapped before the ring.
 *
 * netdec decodes a byte stream that is MP3 or ADTS from its first byte
 * to its last. An HLS "packed audio" segment (RFC 8216 3.4) is that
 * stream with an ID3 tag in front -- a PRIV frame carrying the MPEG-2
 * timestamp of the segment's first sample -- and every segment has one.
 * netdec skips an ID3 tag at the start of a stream and never again, so
 * concatenated segments would hand it a fresh tag every few seconds,
 * mid-stream, as audio. This drops each one at its own segment's start.
 *
 * It also says what a segment is, from its first bytes after the tags,
 * so a format not decoded here is refused by name once rather than
 * resynced through as noise for ever:
 *
 *   0x47, and 0x47 again 188 bytes on   MPEG-2 TS       refused (6040)
 *   ftyp/styp/moof/sidx at offset 4      fragmented MP4  refused
 *   anything else                        audio, handed to netdec
 *
 * TWO SPANS OUT, NO COPY OF THE AUDIO. Only a prefix of a segment is
 * ever removed. Until the kind is decided the first payload bytes wait
 * in the struct's lookahead (at most HLSSEG_DECIDE_BYTES); the call that
 * decides returns that lookahead as the first span and the rest of its
 * own input as the second, and every call after returns its input as it
 * is. The same shape as netstream's splice_feed(), so the send loop is
 * the same loop.
 *
 * Host-tested: texttest/hlssegtest.c, under ASan and UBSan, with every
 * split of a segment across two reads.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    HLSSEG_UNKNOWN = 0,     /* not enough bytes yet */
    HLSSEG_AUDIO,           /* packed audio: pass it on */
    HLSSEG_TS,              /* MPEG-2 TS: needs a demuxer */
    HLSSEG_FMP4,            /* fragmented MP4: needs a box walker */
} hlsseg_kind_t;

static inline const char *hlsseg_kind_name(hlsseg_kind_t k)
{
    switch (k) {
    case HLSSEG_AUDIO: return "packed audio";
    case HLSSEG_TS:    return "MPEG-TS";
    case HLSSEG_FMP4:  return "fragmented MP4";
    default:           return "unknown";
    }
}

/* Bytes needed after the tags to tell the three apart: two TS sync bytes
 * 188 apart need 189. A shorter segment is decided by hlsseg_end(). */
#define HLSSEG_DECIDE_BYTES (189)

typedef struct {
    hlsseg_kind_t kind;
    uint64_t skip;              /* tag bytes still to drop */
    uint32_t tags;              /* ID3 tags dropped in this segment */
    uint64_t tag_bytes;
    uint8_t  hdr[10];           /* a possible tag header, gathering */
    uint8_t  hn;
    bool     past_tags;         /* the payload has started */
    uint8_t  look[HLSSEG_DECIDE_BYTES];
    uint16_t ln;
} hlsseg_t;

static inline void hlsseg_begin(hlsseg_t *s)
{
    memset(s, 0, sizeof(*s));
}

/* The full length of an ID3v2 tag from its ten header bytes, or 0 if
 * they are not one. Same rule as codecplan.h's id3_skip_bytes(): a size
 * byte with its high bit set is not syncsafe and is refused. */
static inline uint64_t hlsseg_id3_len(const uint8_t h[10])
{
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
    if (h[3] == 0xFF || h[4] == 0xFF) return 0;
    for (int i = 6; i < 10; i++) if (h[i] & 0x80) return 0;
    const uint64_t size = ((uint64_t)h[6] << 21) | ((uint64_t)h[7] << 14) |
                          ((uint64_t)h[8] << 7) | (uint64_t)h[9];
    return 10 + size + ((h[5] & 0x10) ? 10 : 0);
}

/* The kind, from the first `n` payload bytes. `final`: there are no
 * more, so decide with what there is. */
static inline hlsseg_kind_t hlsseg_classify(const uint8_t *b, size_t n, bool final)
{
    if (n == 0) return HLSSEG_UNKNOWN;
    if (b[0] == 0x47) {
        if (n >= 189) return b[188] == 0x47 ? HLSSEG_TS : HLSSEG_AUDIO;
        /* Shorter than two packets and over: one sync byte is all there
         * is, and neither an MP3 nor an ADTS frame starts with 0x47. */
        return final ? HLSSEG_TS : HLSSEG_UNKNOWN;
    }
    if (n >= 8) {
        if (memcmp(b + 4, "ftyp", 4) == 0 || memcmp(b + 4, "styp", 4) == 0 ||
            memcmp(b + 4, "moof", 4) == 0 || memcmp(b + 4, "sidx", 4) == 0)
            return HLSSEG_FMP4;
        return HLSSEG_AUDIO;
    }
    return final ? HLSSEG_AUDIO : HLSSEG_UNKNOWN;
}

/* Whether the bytes gathered so far could still become "ID3". */
static inline bool hlsseg_could_be_tag(const hlsseg_t *s)
{
    static const char id3[3] = { 'I', 'D', '3' };
    for (uint8_t k = 0; k < s->hn && k < 3; k++)
        if (s->hdr[k] != (uint8_t)id3[k]) return false;
    return true;
}

/* Into the lookahead, and decide if it can be decided. Returns how many
 * of `n` it took. */
static inline size_t hlsseg_look(hlsseg_t *s, const uint8_t *p, size_t n, bool final)
{
    size_t take = (size_t)(HLSSEG_DECIDE_BYTES - s->ln);
    if (take > n) take = n;
    memcpy(s->look + s->ln, p, take);
    s->ln = (uint16_t)(s->ln + take);
    s->kind = hlsseg_classify(s->look, s->ln, final || s->ln == HLSSEG_DECIDE_BYTES);
    return take;
}

/*
 * Feed `n` bytes of the segment's body. Up to two spans of audio come
 * back, in order, in seg[0..1] / segn[0..1]; either may be empty. The
 * total is returned. seg[0] may point into the struct, so send both
 * before calling again.
 *
 * Nothing comes back while the kind is UNKNOWN, and nothing ever comes
 * back once it is TS or FMP4: the caller looks at s->kind and stops.
 */
static inline size_t hlsseg_feed(hlsseg_t *s, const uint8_t *in, size_t n,
                                 const uint8_t *seg[2], size_t segn[2])
{
    seg[0] = seg[1] = NULL;
    segn[0] = segn[1] = 0;
    if (s->kind == HLSSEG_AUDIO) {
        seg[0] = in;
        segn[0] = n;
        return n;
    }
    if (s->kind != HLSSEG_UNKNOWN) return 0;

    size_t i = 0;
    while (i < n && !s->past_tags) {
        if (s->skip) {
            const uint64_t left = (uint64_t)(n - i);
            const uint64_t take = left < s->skip ? left : s->skip;
            i += (size_t)take;
            s->skip -= take;
            continue;
        }
        /* Gather up to ten bytes of a possible tag header, giving up the
         * moment they cannot be "ID3". */
        while (i < n && s->hn < 10 && hlsseg_could_be_tag(s))
            s->hdr[s->hn++] = in[i++];
        if (hlsseg_could_be_tag(s) && s->hn < 10) return 0;   /* need more */
        const uint64_t len = (s->hn == 10) ? hlsseg_id3_len(s->hdr) : 0;
        if (len) {
            s->tags++;
            s->tag_bytes += len;
            s->skip = len - 10;
            s->hn = 0;
            continue;                           /* tags can follow tags */
        }
        /* Not a tag: what was gathered is the start of the payload. */
        s->past_tags = true;
        memcpy(s->look, s->hdr, s->hn);
        s->ln = s->hn;
        s->hn = 0;
        s->kind = hlsseg_classify(s->look, s->ln, false);
    }
    if (!s->past_tags) return 0;

    if (s->kind == HLSSEG_UNKNOWN && i < n)
        i += hlsseg_look(s, in + i, n - i, false);
    if (s->kind != HLSSEG_AUDIO) return 0;

    seg[0] = s->look;
    segn[0] = s->ln;
    seg[1] = in + i;
    segn[1] = n - i;
    return segn[0] + segn[1];
}

/*
 * The segment's body has ended. A segment too short to have decided its
 * kind decides it now from what it has; if that is audio, the lookahead
 * comes back as one span. Also the place a tag that claimed to be longer
 * than its segment is noticed: s->skip is still non-zero.
 */
static inline size_t hlsseg_end(hlsseg_t *s, const uint8_t **out)
{
    *out = NULL;
    if (s->kind != HLSSEG_UNKNOWN) return 0;
    if (!s->past_tags && s->hn) {
        /* A few bytes that could have been "ID3" and were the end. */
        memcpy(s->look, s->hdr, s->hn);
        s->ln = s->hn;
        s->hn = 0;
        s->past_tags = true;
    }
    s->kind = hlsseg_classify(s->look, s->ln, true);
    if (s->kind != HLSSEG_AUDIO) return 0;
    *out = s->look;
    return s->ln;
}
