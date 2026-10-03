/*
 * tsdemux.h -- the audio out of an MPEG-2 transport stream.
 *
 * Al Jazeera's HLS segments are MPEG-TS (`.ts`, `video/mp2t`), as most
 * broadcasters' are: 188-byte packets, each tagged with a 13-bit PID,
 * carrying PES packets that carry the audio. AAC in TS (stream_type
 * 0x0F) is ADTS frames, and MPEG audio (0x03, 0x04) is MP3 frames -- the
 * two byte streams netdec already decodes. So demuxing here is only
 * taking things away: the TS headers, the adaptation fields, the PES
 * headers, and every PID that is not the audio. No reassembly buffer, no
 * timestamps, no clock recovery; netdec finds frame boundaries itself.
 *
 *     packets --PAT--> the PMT's PID --PMT--> the audio PID and type
 *             --audio PID--> PES header off --> ADTS / MP3 bytes
 *
 * WHAT IS PICKED
 *
 * The first program in the PAT, and in its PMT the first elementary
 * stream that is AAC-ADTS or MPEG audio. A radio segment has one of each
 * and nothing else but, at most, a timed-ID3 metadata stream (0x15),
 * which is skipped like any other PID. LATM AAC (0x11) and AC-3 (0x81)
 * are recognised and refused by name -- netdec decodes neither.
 *
 * WHAT IS ASSUMED, AND WHERE IT WOULD BREAK
 *
 * The PAT and the PMT each fit in one packet. A PAT is 16 bytes for one
 * program and a radio PMT is about 30, against 183 bytes of room, and
 * every packager seen does it; a section that claims more than its
 * packet holds is refused (TSDEMUX_ERR_SECTION) rather than half read.
 * A PES header may span packets, and that is handled, because nothing
 * stops a packager putting the adaptation field's stuffing first.
 *
 * STREAMING AND SPLIT-SAFE
 *
 * Reads do not respect packet boundaries; a partial packet waits in the
 * struct. Output for one call is at most the input plus the 187 bytes
 * that could have been waiting, so a caller's out buffer of n + 188 is
 * always enough (TSDEMUX_OUT_SLACK).
 *
 * Lost sync -- a packet not starting 0x47 -- is found again by scanning
 * for 0x47, counted, and never guessed through. A continuity-counter
 * gap on the audio PID is counted and the data kept: the bytes are still
 * real frames, netdec resyncs on the hole, and dropping the rest of the
 * PES would make one lost packet into a lost frame or two.
 *
 * Host-tested: texttest/tsdemuxtest.c, under ASan and UBSan, with every
 * split of a segment across two reads.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TS_PACKET           (188)
#define TS_SYNC             (0x47)
#define TSDEMUX_OUT_SLACK   (TS_PACKET)

typedef enum {
    TSDEMUX_SEARCHING = 0,  /* no audio PID yet */
    TSDEMUX_AAC,            /* stream_type 0x0F: ADTS frames */
    TSDEMUX_MP3,            /* stream_type 0x03/0x04: MPEG audio frames */
} tsdemux_audio_t;

typedef enum {
    TSDEMUX_OK = 0,
    TSDEMUX_ERR_LATM,       /* AAC in LATM/LOAS, 0x11 */
    TSDEMUX_ERR_AC3,        /* 0x81 / 0x06-with-descriptor */
    TSDEMUX_ERR_NO_AUDIO,   /* a PMT with no audio stream at all */
    TSDEMUX_ERR_SECTION,    /* a PAT or PMT that does not fit one packet */
} tsdemux_err_t;

static inline const char *tsdemux_err_name(tsdemux_err_t e)
{
    switch (e) {
    case TSDEMUX_OK:          return "ok";
    case TSDEMUX_ERR_LATM:    return "AAC in LATM inside MPEG-TS is not supported";
    case TSDEMUX_ERR_AC3:     return "AC-3 audio is not supported";
    case TSDEMUX_ERR_NO_AUDIO:return "the MPEG-TS segment has no audio stream";
    case TSDEMUX_ERR_SECTION: return "an MPEG-TS table spans packets";
    default:                  return "?";
    }
}

static inline const char *tsdemux_audio_name(tsdemux_audio_t a)
{
    switch (a) {
    case TSDEMUX_AAC: return "AAC (ADTS)";
    case TSDEMUX_MP3: return "MPEG audio";
    default:          return "none";
    }
}

typedef struct {
    uint8_t  pkt[TS_PACKET];
    uint16_t pn;                /* bytes of pkt gathered */
    int      pmt_pid;           /* -1 until the PAT names it */
    int      audio_pid;         /* -1 until the PMT names it */
    tsdemux_audio_t audio;
    tsdemux_err_t   err;        /* sticky: once set, nothing more comes out */
    int      cc;                /* last continuity counter on the audio PID, -1 none */
    bool     in_pes;            /* a PES has started on the audio PID */
    /* A PES header being read, possibly across packets. */
    uint8_t  pes_hdr[9];
    uint8_t  pes_hn;
    uint16_t pes_skip;          /* optional-header bytes still to drop */
    /* For the log. */
    uint32_t packets, audio_packets, resyncs, cc_gaps, bytes_lost;
} tsdemux_t;

static inline void tsdemux_init(tsdemux_t *d)
{
    memset(d, 0, sizeof(*d));
    d->pmt_pid = -1;
    d->audio_pid = -1;
    d->cc = -1;
}

/* A PSI section's start in a packet's payload, past the pointer field.
 * NULL if the section is not wholly inside the payload. */
static inline const uint8_t *tsdemux_section(const uint8_t *p, size_t n, bool pusi,
                                             size_t *len)
{
    if (!pusi || n < 1) return NULL;
    const size_t ptr = p[0];
    if (1 + ptr + 3 > n) return NULL;
    const uint8_t *s = p + 1 + ptr;
    const size_t slen = (((size_t)s[1] & 0x0F) << 8) | s[2];
    if (1 + ptr + 3 + slen > n) return NULL;
    *len = 3 + slen;
    return s;
}

static inline void tsdemux_pat(tsdemux_t *d, const uint8_t *p, size_t n, bool pusi)
{
    size_t len;
    const uint8_t *s = tsdemux_section(p, n, pusi, &len);
    if (!s) {
        if (pusi) d->err = TSDEMUX_ERR_SECTION;
        return;
    }
    if (s[0] != 0x00 || len < 12) return;
    /* Programs from byte 8 to the CRC. */
    for (size_t i = 8; i + 4 <= len - 4; i += 4) {
        const unsigned prog = ((unsigned)s[i] << 8) | s[i + 1];
        const int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
        if (prog != 0) {            /* 0 is the network PID, not a program */
            d->pmt_pid = pid;
            return;
        }
    }
}

static inline void tsdemux_pmt(tsdemux_t *d, const uint8_t *p, size_t n, bool pusi)
{
    size_t len;
    const uint8_t *s = tsdemux_section(p, n, pusi, &len);
    if (!s) {
        if (pusi) d->err = TSDEMUX_ERR_SECTION;
        return;
    }
    if (s[0] != 0x02 || len < 16) return;
    const size_t pinfo = (((size_t)s[10] & 0x0F) << 8) | s[11];
    tsdemux_err_t refused = TSDEMUX_ERR_NO_AUDIO;
    for (size_t i = 12 + pinfo; i + 5 <= len - 4; ) {
        const uint8_t type = s[i];
        const int pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        const size_t einfo = (((size_t)s[i + 3] & 0x0F) << 8) | s[i + 4];
        if (type == 0x0F || type == 0x03 || type == 0x04) {
            d->audio_pid = pid;
            d->audio = type == 0x0F ? TSDEMUX_AAC : TSDEMUX_MP3;
            return;
        }
        if (type == 0x11) refused = TSDEMUX_ERR_LATM;
        else if (type == 0x81 && refused == TSDEMUX_ERR_NO_AUDIO) refused = TSDEMUX_ERR_AC3;
        i += 5 + einfo;
    }
    d->err = refused;
}

/* The audio PID's payload: PES headers off, the rest out. */
static inline size_t tsdemux_audio_payload(tsdemux_t *d, const uint8_t *p, size_t n,
                                           bool pusi, uint8_t *out)
{
    size_t i = 0, o = 0;
    if (pusi) {
        d->in_pes = true;
        d->pes_hn = 0;
        d->pes_skip = 0;
    }
    if (!d->in_pes) {
        d->bytes_lost += (uint32_t)n;       /* joined mid-PES */
        return 0;
    }
    /* The fixed 9 bytes: 00 00 01, stream id, length, two flag bytes,
     * header data length. */
    while (d->pes_hn < 9 && i < n) d->pes_hdr[d->pes_hn++] = p[i++];
    if (d->pes_hn < 9) return 0;
    if (d->pes_hn == 9) {
        if (d->pes_hdr[0] != 0 || d->pes_hdr[1] != 0 || d->pes_hdr[2] != 1) {
            /* Not a PES start: drop until the next one. */
            d->in_pes = false;
            d->bytes_lost += (uint32_t)(n - i);
            return 0;
        }
        d->pes_skip = d->pes_hdr[8];
        d->pes_hn = 10;                     /* header read; skip pending */
    }
    if (d->pes_skip) {
        const size_t k = (n - i) < d->pes_skip ? (n - i) : d->pes_skip;
        i += k;
        d->pes_skip = (uint16_t)(d->pes_skip - k);
        if (d->pes_skip) return 0;
    }
    memcpy(out + o, p + i, n - i);
    o += n - i;
    return o;
}

static inline size_t tsdemux_packet(tsdemux_t *d, const uint8_t *pk, uint8_t *out)
{
    d->packets++;
    if (pk[1] & 0x80) return 0;             /* transport error indicator */
    const bool pusi = (pk[1] & 0x40) != 0;
    const int pid = ((pk[1] & 0x1F) << 8) | pk[2];
    const int afc = (pk[3] >> 4) & 3;
    const int cc = pk[3] & 0x0F;
    if (!(afc & 1)) return 0;               /* no payload */
    size_t off = 4;
    if (afc & 2) {
        off += 1 + (size_t)pk[4];
        if (off > TS_PACKET) return 0;      /* adaptation field overruns */
    }
    const uint8_t *p = pk + off;
    const size_t n = TS_PACKET - off;

    if (pid == 0) { tsdemux_pat(d, p, n, pusi); return 0; }
    if (pid == d->pmt_pid && d->audio_pid < 0) { tsdemux_pmt(d, p, n, pusi); return 0; }
    if (pid != d->audio_pid || d->audio_pid < 0) return 0;

    d->audio_packets++;
    if (d->cc >= 0) {
        if (cc == d->cc) return 0;          /* a duplicate packet, allowed once */
        if (cc != ((d->cc + 1) & 0x0F)) d->cc_gaps++;
    }
    d->cc = cc;
    return tsdemux_audio_payload(d, p, n, pusi, out);
}

/*
 * Feed `n` bytes of transport stream; audio bytes go to `out`, which
 * must have room for n + TSDEMUX_OUT_SLACK. Returns how many were
 * written. Nothing is written once d->err is set.
 */
static inline size_t tsdemux_feed(tsdemux_t *d, const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0, i = 0;
    while (i < n && d->err == TSDEMUX_OK) {
        if (d->pn == 0 && in[i] != TS_SYNC) {
            /* Out of sync: scan for the next candidate. */
            const size_t s = i;
            while (i < n && in[i] != TS_SYNC) i++;
            d->resyncs++;
            d->bytes_lost += (uint32_t)(i - s);
            continue;
        }
        size_t take = TS_PACKET - d->pn;
        if (take > n - i) take = n - i;
        memcpy(d->pkt + d->pn, in + i, take);
        d->pn = (uint16_t)(d->pn + take);
        i += take;
        if (d->pn < TS_PACKET) break;
        d->pn = 0;
        o += tsdemux_packet(d, d->pkt, out + o);
    }
    return o;
}
