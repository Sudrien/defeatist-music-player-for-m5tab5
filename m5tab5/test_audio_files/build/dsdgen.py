# SPDX-License-Identifier: MIT
"""dsdgen.py -- write a DSF or DFF test file from landmark.wav.

ffmpeg can decode DSD but has no encoder or muxer for it, so this does
both: ffmpeg resamples landmark.wav to the DSD rate, sdm (sdm.c, a 2nd
order sigma-delta modulator) makes the bits, and this wraps them.

  dsdgen.py FORMAT RATE SECONDS CHANNELS OUT [--samples N] [--dst]

FORMAT dsf|dff.  RATE is the 1-bit rate in Hz (2822400 = DSD64).
--samples trims to an exact per-channel bit count (DSF records it, so a
length that is not a multiple of the block size is testable).
--dst writes a DFF whose CMPR says 'DST ' over junk frames: a file the
player must refuse by name, not play as noise.
"""
import struct, subprocess, sys
import numpy as np

def pad(b): return b + b"\0" if len(b) & 1 else b

def modulate(rate, secs, nch):
    ff = subprocess.Popen(
        ["ffmpeg", "-loglevel", "error", "-i", "landmark.wav", "-t", str(secs),
         "-af", f"aresample={rate}:resampler=swr:filter_size=64:phase_shift=8",
         "-ac", "2", "-f", "f32le", "-"], stdout=subprocess.PIPE)
    sd = subprocess.run(["./sdm", str(nch)], stdin=ff.stdout, stdout=subprocess.PIPE, check=True)
    ff.wait()
    return np.frombuffer(sd.stdout, dtype=np.uint8)   # MSB first, byte per channel, interleaved

def dsf(data, rate, nch, bits, out):
    per = data.reshape(-1, nch).T                      # [ch][byte]
    nbytes = (bits + 7) // 8
    per = per[:, :nbytes].copy()
    if bits % 8:                                       # clear the unused tail bits
        per[:, -1] &= np.uint8((0xFF << (8 - bits % 8)) & 0xFF)
    rev = np.unpackbits(per[:, :, None], axis=2, bitorder="big")
    per = np.packbits(rev, axis=2, bitorder="little")[:, :, 0]   # DSF is LSB first
    blk = 4096
    nblk = (nbytes + blk - 1) // blk
    full = np.full((nch, nblk * blk), 0x69, dtype=np.uint8)      # idle pattern as padding
    full[:, :nbytes] = per
    body = full.reshape(nch, nblk, blk).transpose(1, 0, 2).tobytes()
    ctype = {1: 1, 2: 2}[nch]
    fmt = struct.pack("<4sQIIIIIIQII", b"fmt ", 52, 1, 0, ctype, nch, rate, 1, bits, blk, 0)
    dat = struct.pack("<4sQ", b"data", 12 + len(body)) + body
    total = 28 + len(fmt) + len(dat)
    hdr = struct.pack("<4sQQQ", b"DSD ", 28, total, 0)
    open(out, "wb").write(hdr + fmt + dat)

def dff(data, rate, nch, bits, out, dst=False):
    nbytes = (bits + 7) // 8
    raw = data.reshape(-1, nch)[:nbytes]
    ids = b"SLFTSRGT" if nch == 2 else b"C   "
    chnl = struct.pack(">4sQH", b"CHNL", 2 + len(ids), nch) + ids
    name = b"DST Encoded" if dst else b"not compressed"
    cmpr = struct.pack(">4sQ4sB", b"CMPR", 4 + 1 + len(name), b"DST " if dst else b"DSD ", len(name)) + name
    cmpr = pad(cmpr)
    fs = pad(struct.pack(">4sQI", b"FS  ", 4, rate))
    prop = b"SND " + fs + chnl + cmpr
    prop = pad(struct.pack(">4sQ", b"PROP", len(prop)) + prop)
    fver = struct.pack(">4sQI", b"FVER", 4, 0x01050000)
    if dst:   # a DST chunk of junk: 'DSTF' frame header with a bogus length, no real frames
        junk = struct.pack(">4sQ", b"DSTF", 64) + bytes(range(64))
        body = struct.pack(">4sQ", b"DSTI", 0) + junk
        name_ = b"DST "
    else:
        body = raw.tobytes()
        name_ = b"DSD "
    chunk = pad(struct.pack(">4sQ", name_, len(body)) + body)
    inner = b"DSD " + fver + prop + chunk
    open(out, "wb").write(struct.pack(">4sQ", b"FRM8", len(inner)) + inner)

if __name__ == "__main__":
    a = sys.argv[1:]
    fmt, rate, secs, nch, out = a[0], int(a[1]), float(a[2]), int(a[3]), a[4]
    bits = int(a[a.index("--samples") + 1]) if "--samples" in a else int(rate * secs)
    data = modulate(rate, secs + 0.01, nch)
    (dsf if fmt == "dsf" else dff)(data, rate, nch, bits, out) if not "--dst" in a else dff(data, rate, nch, bits, out, True)
