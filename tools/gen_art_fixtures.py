#!/usr/bin/env python3
"""
gen_art_fixtures.py -- the pictures texttest/arttest.c decodes.

Every fixture is the same 64x48 picture, four flat quadrants, so a
decode can be checked by sampling the middle of each:

    red   | green
    ------+------
    blue  | white

(the alpha fixtures make the white quadrant fully transparent instead).
Flat colour is what lossy codecs reproduce best, so one tolerance works
for every format and the test is about the decoder, not the encoder.

One file per flavour a cover is found in: JPEG baseline 4:2:0 and
4:4:4, progressive, greyscale, with an EXIF segment before the SOF;
PNG RGB, RGBA, palette, palette with transparency, greyscale, grey with
alpha, 16 bits a channel, and Adam7-interlaced.

Usage:  ./tools/gen_art_fixtures.py [outdir]   (default texttest/fixtures/art)
Needs Pillow. The outputs are committed; rerun only to change them.

SPDX-License-Identifier: MIT
"""
import os, sys, struct, zlib
from PIL import Image

W, H = 64, 48
RED, GREEN, BLUE, WHITE = (220, 30, 30), (30, 200, 40), (40, 50, 210), (255, 255, 255)

def quads(alpha=False):
    im = Image.new('RGBA', (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            c = (RED if x < W // 2 else GREEN) if y < H // 2 else (BLUE if x < W // 2 else WHITE)
            a = 0 if (alpha and c == WHITE) else 255
            px[x, y] = c + (a,)
    return im

def png16(path):
    # Pillow writes 16-bit only for single-channel modes; write RGB16 by hand.
    rgb = quads().convert('RGB')
    raw = b''
    for y in range(H):
        raw += b'\0' + b''.join(struct.pack('>HHH', *(v * 257 for v in rgb.getpixel((x, y)))) for x in range(W))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', W, H, 16, 2, 0, 0, 0)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'texttest', 'fixtures', 'art')
    os.makedirs(out, exist_ok=True)
    p = lambda n: os.path.join(out, n)
    rgb = quads().convert('RGB')
    rgb.save(p('base420.jpg'), quality=92, subsampling=2)
    rgb.save(p('base444.jpg'), quality=92, subsampling=0)
    rgb.save(p('prog.jpg'), quality=92, progressive=True)
    rgb.convert('L').save(p('gray.jpg'), quality=92)
    exif = Image.Exif(); exif[0x0131] = 'gen_art_fixtures.py'   # Software
    rgb.save(p('exif.jpg'), quality=92, exif=exif.tobytes())
    rgb.save(p('rgb.png'))
    quads(alpha=True).save(p('rgba.png'))
    rgb.quantize(8).save(p('palette.png'))
    pt = rgb.quantize(8); pal_idx = pt.getpixel((W - 1, H - 1))
    pt.save(p('palette_trns.png'), transparency=pal_idx)
    rgb.convert('L').save(p('gray.png'))
    quads(alpha=True).convert('LA').save(p('gray_alpha.png'))
    png16(p('rgb16.png'))
    # Pillow cannot write Adam7; write it by hand from the RGB rows.
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    raw = b''
    for x0, y0, dx, dy in passes:
        for y in range(y0, H, dy):
            row = b''.join(bytes(rgb.getpixel((x, y))) for x in range(x0, W, dx))
            if row: raw += b'\0' + row
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(p('interlaced.png'), 'wb').write(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 1)) +
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

if __name__ == '__main__':
    main()
