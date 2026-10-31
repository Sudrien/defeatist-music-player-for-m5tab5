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
alpha, 16 bits a channel, and Adam7-interlaced. 6145, GIF: plain
(GIF87a), interlaced, GIF89a with a transparent colour, and animated with a black second frame (the first
must be what shows).

6139, JPEG XL (needs pillow-jxl-plugin): lossy, lossless, with alpha,
the lossy codestream wrapped in a container as one `jxlc` box and again
as two `jxlp` boxes behind an `Exif` box, a 320x240 JPEG losslessly
recompressed (a container with `jbrd` and split `jxlp` boxes), and one
fixture for each kind J40 refuses (no_*.jxl, the recompressed JPEG
among them). Plus
flat pictures at awkward sizes for the size-header parser, one per
SizeHeader path (div8, each ratio, each U32 width).

Usage:  ./tools/gen_art_fixtures.py [outdir]   (default texttest/fixtures/art)
Needs Pillow. The outputs are committed; rerun only to change them.

SPDX-License-Identifier: MIT
"""
import os, sys, struct, zlib
from PIL import Image

W, H = 64, 48
RED, GREEN, BLUE, WHITE = (220, 30, 30), (30, 200, 40), (40, 50, 210), (255, 255, 255)

def quads(alpha=False, w=W, h=H):
    im = Image.new('RGBA', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = (RED if x < w // 2 else GREEN) if y < h // 2 else (BLUE if x < w // 2 else WHITE)
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
    # 6145: GIF, which stb_image reads now -- the first frame only.
    rgb.convert('P', palette=Image.ADAPTIVE, colors=8).save(p('gif.gif'))
    rgb.convert('P', palette=Image.ADAPTIVE, colors=8).save(p('gif_interlaced.gif'), interlace=True)
    first = rgb.convert('P', palette=Image.ADAPTIVE, colors=8)
    second = Image.new('RGB', (W, H), (0, 0, 0)).convert('P')
    first.save(p('gif_animated.gif'), save_all=True, append_images=[second], duration=200, loop=0)
    # 6148: a flat 2000x2000, the longest LZW chains a GIF this size can
    # build -- for texttest/gifstacktest.c, which runs it on GIF_STACK.
    Image.new('RGB', (2000, 2000), RED).convert('P', palette=Image.ADAPTIVE, colors=2).save(p('gif_flat2000.gif'))
    # GIF89a with a transparent colour: the white quadrant's index.
    first.save(p('gif_trns.gif'), transparency=first.getpixel((W - 1, H - 1)))
    jxl(out)

def box(t, payload):
    return struct.pack('>I', len(payload) + 8) + t + payload

JXL_SIG = b'\x00\x00\x00\x0cJXL \x0d\x0a\x87\x0a'
FTYP = box(b'ftyp', b'jxl \x00\x00\x00\x00jxl ')

def jxl(out):
    """
    J40 decodes some JPEG XL and not others, and which is which is what
    texttest/jxltest.c pins down. Found while writing it (6139, against
    libjxl 0.11 via pillow-jxl-plugin 1.3.8):
      - lossy at 256x256 or smaller: refused (one group; "rnge", "hufd")
      - lossy, one flat colour, 600 px and up: refused ("hufd")
      - lossless at effort 1, any size: refused ("rnge")
      - lossy above 256 px with detail, lossless at effort 3 and up: decoded
      - a JPEG recompressed into JPEG XL: sometimes. Busy pictures over
        256 px decoded in testing; this four-quadrant one does not
        ("Premature end of file"), nor did one 700x700 busy one
        ("Unimplemented feature")
    So the pictures that should decode are 320x240, and each refusal that
    has a pattern has a fixture of its own.
    """
    try:
        import pillow_jxl  # noqa: F401 -- registers the format with Pillow
    except ImportError:
        print('pillow-jxl-plugin not installed: no JPEG XL fixtures')
        return
    p = lambda n: os.path.join(out, n)
    big = quads(w=320, h=240).convert('RGB')
    big.save(p('lossy.jxl'), quality=90)
    big.save(p('lossless.jxl'), lossless=True, effort=7)
    quads(alpha=True, w=320, h=240).save(p('alpha.jxl'), lossless=True, effort=7)
    cs = open(p('lossy.jxl'), 'rb').read()
    assert cs[:2] == b'\xff\x0a'
    open(p('container.jxl'), 'wb').write(JXL_SIG + FTYP + box(b'jxlc', cs))
    half = len(cs) // 2
    exif = box(b'Exif', b'\x00\x00\x00\x00' + b'MM\x00\x2a\x00\x00\x00\x08\x00\x00')
    open(p('jxlp.jxl'), 'wb').write(JXL_SIG + FTYP + exif +
        box(b'jxlp', struct.pack('>I', 0) + cs[:half]) +
        box(b'jxlp', struct.pack('>I', 0x80000001) + cs[half:]))
    # The refusals.
    quads().convert('RGB').save(p('no_small_lossy.jxl'), quality=90)
    big.save(p('no_lossless_e1.jxl'), lossless=True, effort=1)
    Image.new('RGB', (700, 700), RED).save(p('no_flat_lossy.jxl'), quality=90)
    big.save(os.path.join(out, '_tmp320.jpg'), quality=92)
    Image.open(p('_tmp320.jpg')).save(p('no_from_jpeg.jxl'), lossless_jpeg=True)
    os.remove(p('_tmp320.jpg'))
    assert open(p('no_from_jpeg.jxl'), 'rb').read()[:12] == JXL_SIG
    # The size header, one file per SizeHeader shape.
    sizes = [(1, 1), (8, 8), (40, 8), (8, 40), (64, 48), (256, 144), (300, 200),
             (600, 500), (517, 313), (513, 1), (1, 513), (1024, 768), (8200, 2)]
    for w, h in sizes:
        Image.new('RGB', (w, h), (10, 20, 30)).save(p('size_%dx%d.jxl' % (w, h)), lossless=True, effort=3)

if __name__ == '__main__':
    main()
