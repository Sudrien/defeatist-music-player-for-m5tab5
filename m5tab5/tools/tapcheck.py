#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
tapcheck.py -- is a stutter in the file, in the player's samples, or after
them?  Reads a tap (u-defeatist-0035) against the track it was made from.

WHY THIS EXISTS

Some files stutter on the jack and the speaker, the I2S DMA never ran dry
for it (u-feckless-0066 would have logged it), and the writer was not
late (u-defeatist-0032). That leaves three places, and listening cannot
tell them apart:

  1. the file      -- it is damaged, and stutters on a PC too;
  2. the samples   -- the file is fine, but what the writer handed to
                      audio_out_write() has a hole, a repeat or a jump;
  3. downstream    -- the samples are clean, and the driver, the codec or
                      the analog side does it.

The tap (/sd/.tap on the card) saves the first 15 s the writer sends the
output as /sd/tap-<ms>.wav. This compares that against the PC's own
decode of the same file and says which of the three it is, with times.

WHAT IT CHECKS

  - the source, decoded end to end by ffmpeg with errors on: a decode
    error there is (1), whatever the tap says;
  - where in the source the tap starts (it begins at the first write
    after boot, so a resume or a seek puts it mid-track);
  - then the tap block by block (20 ms) against the source at that
    point, with the gain fitted per block -- volume, ReplayGain and the
    edge ramps change the level, not the shape:
      SKIP    the tap jumps forward in the source (audio dropped)
      REPEAT  the tap jumps back (audio played twice)
      SILENT  digital zeros where the source has sound
      BAD     the right place in the source, but the wrong samples
              (a click, a garbled block) -- well below this tap's own
              typical match
  - each problem with its time in the tap and in the source, so it can
    be found in either file with an editor.

A tap that matches end to end is (3). Anything flagged is (2), unless
ffmpeg found the file damaged at the same place.

WHAT IT CANNOT SEE

Turntable speed other than 1.00x, ECHO, DUST and the other effects
change the samples on purpose; with them on, everything reads as BAD.
Turn them off for the tap. A stutter after the first 15 s is not in the
tap: play the file from where it stutters as the first thing after boot.

Needs ffmpeg on the PATH and numpy (pip install numpy).

USAGE

    python3 tools/tapcheck.py tap-61234.wav "/path/to/the track.flac"
    python3 tools/tapcheck.py --block-ms 10 tap.wav track.mp3
"""

import argparse
import shutil
import subprocess
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("tapcheck: needs numpy (pip install numpy)")


def read_tap(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 2:
            sys.exit(f"tapcheck: {path} is not 16-bit stereo; is it a tap?")
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    pcm = np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(np.float64)
    return rate, pcm


def decode(src, rate):
    """The source as the PC decodes it, at the tap's rate, plus ffmpeg's
    complaints about the file (empty when it decoded clean)."""
    if not shutil.which("ffmpeg"):
        sys.exit("tapcheck: needs ffmpeg on the PATH")
    p = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", src, "-vn",
         "-ac", "2", "-ar", str(rate), "-f", "s16le", "-"],
        capture_output=True)
    if p.returncode != 0 and not p.stdout:
        sys.exit("tapcheck: ffmpeg could not decode the source:\n" +
                 p.stderr.decode(errors="replace"))
    pcm = np.frombuffer(p.stdout, dtype="<i2").reshape(-1, 2)
    return pcm.astype(np.float64), p.stderr.decode(errors="replace").strip()


def xcorr_best(needle, hay, near=None):
    """Lag of needle in hay with the highest normalised correlation. With
    `near`, of the lags within a hair of the best, the one closest to it:
    a tone, or a bar of music that repeats, matches at many lags, and the
    nearest of those is the jump that happened."""
    n = len(needle)
    if len(hay) < n:
        return None, 0.0
    size = 1
    while size < len(hay) + n:
        size *= 2
    c = np.fft.irfft(np.fft.rfft(hay, size) *
                     np.conj(np.fft.rfft(needle, size)), size)[:len(hay) - n + 1]
    # Energy of each window of hay, for the normalisation.
    cs = np.concatenate(([0.0], np.cumsum(hay * hay)))
    e = cs[n:] - cs[:-n]
    en = float(np.dot(needle, needle))
    score = c / np.sqrt(np.maximum(e * en, 1e-9))
    lag = int(np.argmax(score))
    best = float(score[lag])
    if near is not None:
        ok = np.flatnonzero(score >= best - 0.002)
        lag = int(ok[np.argmin(np.abs(ok - near))])
    return lag, float(score[lag])


def match(t, r):
    """Fitted-gain match of tap block t against source block r, both
    (frames, 2): SNR in dB of the residual after the best gain."""
    rr = float(np.sum(r * r))
    if rr < 1e-6:
        return float("inf") if float(np.sum(t * t)) < 1e-6 else -float("inf")
    g = float(np.sum(t * r)) / rr
    res = t - g * r
    rs = float(np.sum(res * res))
    sig = float(np.sum(t * t))
    if rs < 1e-9:
        return 120.0
    if sig < 1e-9:
        return -float("inf")
    return 10.0 * np.log10(sig / rs)


def zero_run(b):
    """Longest run of frames where both channels are exactly zero."""
    z = np.concatenate(([0], (np.abs(b).sum(axis=1) == 0).astype(np.int8), [0]))
    d = np.diff(z)
    starts, ends = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    return int((ends - starts).max()) if len(starts) else 0


def fmt_t(frames, rate):
    s = frames / rate
    return f"{int(s // 60)}:{s % 60:06.3f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tap", help="tap-<ms>.wav from the card")
    ap.add_argument("source", help="the track the tap was made from")
    ap.add_argument("--block-ms", type=float, default=20.0)
    ap.add_argument("--bad-db", type=float, default=15.0,
                    help="a block this far below the tap's median match "
                         "is BAD (default 15)")
    a = ap.parse_args()

    rate, tap = read_tap(a.tap)
    src, errs = decode(a.source, rate)
    print(f"tap     {a.tap}: {fmt_t(len(tap), rate)} at {rate} Hz")
    print(f"source  {a.source}: {fmt_t(len(src), rate)} decoded at {rate} Hz")
    if errs:
        print("\nffmpeg found the SOURCE damaged:")
        for line in errs.splitlines()[:20]:
            print("   ", line)
    else:
        print("ffmpeg decoded the source with no errors: the file is sound.")

    tm = tap.mean(axis=1)
    sm = src.mean(axis=1)

    # Where the tap starts: the first loud second of the tap, found in the
    # whole source. Leading silence (the writer's first chunks, an edge
    # ramp) would match anywhere, so skip it.
    loud = np.flatnonzero(np.abs(tm) > 64)
    if len(loud) == 0:
        sys.exit("tapcheck: the tap is silent")
    t0 = int(loud[0])
    probe = tm[t0:t0 + rate]
    lag, score = xcorr_best(probe, sm)
    if lag is None or score < 0.5:
        sys.exit(f"tapcheck: the tap's start is not in the source (best "
                 f"match {score:.2f}); wrong file, or speed/effects on?")
    off = lag - t0          # source frame = tap frame + off
    print(f"tap starts {fmt_t(max(off, 0), rate)} into the source "
          f"(match {score:.3f})\n")

    blk = max(64, int(rate * a.block_ms / 1000))
    search = rate // 2      # how far a jump is looked for, each way
    rows = []               # (tap frame, kind, detail)
    snrs = []
    # From the first sound: what the writer sent before it (its start, an
    # edge ramp up from nothing) is not in the source to compare with.
    i = t0
    while i + blk <= len(tap):
        tb = tap[i:i + blk]
        j = i + off
        sb = src[j:j + blk] if 0 <= j and j + blk <= len(src) else None
        snr = match(tb, sb) if sb is not None else -float("inf")
        t_quiet = float(np.max(np.abs(tb))) < 1.0
        s_loud = sb is not None and float(np.max(np.abs(sb))) > 256.0

        zr = zero_run(tb)
        if s_loud and (t_quiet or zr >= rate // 1000):
            rows.append((i, "SILENT", f"{zr} frames of zeros where the "
                         f"source is at {fmt_t(j, rate)}"))
            i += blk
            continue
        if snr < 20.0 and i + 2 * blk <= len(tap):
            # Lost the place? A jump inside this block leaves it half one
            # thing and half another, so look for the NEXT block, which
            # is wholly after it, near where it should be in the source.
            k = i + blk
            nb = tap[k:k + blk]
            if float(np.max(np.abs(nb))) >= 1.0:
                lo = max(0, k + off - search)
                hi = min(len(src), k + off + blk + search)
                nl, ns = xcorr_best(tm[k:k + blk], sm[lo:hi],
                                    near=k + off - lo)
                if nl is not None and ns > 0.9:
                    d = lo + nl - (k + off)
                    here = match(nb, src[k + off:k + off + blk]) \
                        if k + off + blk <= len(src) else -float("inf")
                    there = match(nb, src[k + off + d:k + off + d + blk])
                    if d != 0 and there > here + 6.0:
                        kind = "SKIP" if d > 0 else "REPEAT"
                        rows.append((i, kind,
                                     f"{abs(d)} frames ({1000 * abs(d) / rate:.1f} ms) "
                                     f"{'dropped' if d > 0 else 'played twice'}; "
                                     f"source {fmt_t(j, rate)} -> "
                                     f"{fmt_t(j + d, rate)}"))
                        off += d
                        i += blk        # this block is the seam itself
                        continue
        if np.isfinite(snr):
            snrs.append((i, snr))
        i += blk

    if snrs:
        med = float(np.median([s for _, s in snrs]))
        for fi, s in snrs:
            if s < med - a.bad_db:
                rows.append((fi, "BAD", f"{s:.1f} dB against this tap's "
                             f"{med:.1f} dB; source {fmt_t(fi + off, rate)}"))
    else:
        med = float("nan")

    rows.sort()
    # Collapse runs of the same kind in adjacent blocks into one line.
    merged = []
    for fi, kind, det in rows:
        if merged and merged[-1][1] == kind and kind in ("SILENT", "BAD") \
                and fi - merged[-1][3] <= blk:
            merged[-1][3] = fi
            continue
        merged.append([fi, kind, det, fi])

    print(f"typical match {med:.1f} dB over {len(snrs)} blocks of {blk} frames")
    if med < 30.0:
        print("  (low for a clean decode: speed, an effect, or a resampler "
              "is changing the samples, and BAD below means less)")
    if not merged:
        print("\nNo skip, repeat, silence or bad block in the tap.")
        print("The samples handed to the output are the file's: if this "
              "stretch stuttered, it is DOWNSTREAM of audio_out_write() "
              "(driver, codec, analog).")
        return 0
    print()
    for fi, kind, det, last in merged:
        span = f"-{fmt_t(last + blk, rate)}" if last != fi else ""
        print(f"  tap {fmt_t(fi, rate)}{span}  {kind:6}  {det}")
    print(f"\n{len(merged)} problem(s) in the samples handed to the output: "
          "UPSTREAM of the driver" +
          (" -- and the file itself is damaged, so compare the times."
           if errs else " -- and the file decodes clean, so it is the player."))
    return 1


if __name__ == "__main__":
    sys.exit(main())
