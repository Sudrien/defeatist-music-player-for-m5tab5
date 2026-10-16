#!/usr/bin/env python3
"""
make.py -- rebuild test_art_files/ (6140): every way a cover reaches
the screen, as files to copy to a card and play.

Every picture says what it is in large letters, so the screen shows
which picture was used without reading the log: the case number, the
format and where it came from. A case that should show nothing has no
picture with its number on it to show.

Needs: ffmpeg, Pillow, pillow-jxl-plugin (JPEG XL), mutagen (tags).
Usage:  ./make.py [outdir]      (default: the folder above this one)

SPDX-License-Identifier: MIT
"""
import io, os, shutil, struct, subprocess, sys, tempfile, base64

from PIL import Image, ImageDraw, ImageFont
import pillow_jxl  # noqa: F401 -- registers JPEG XL with Pillow
from mutagen.id3 import ID3, APIC, TIT2, TALB, TPE1, TRCK
from mutagen.flac import FLAC, Picture
from mutagen.mp4 import MP4, MP4Cover
from mutagen.oggvorbis import OggVorbis

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TMP = tempfile.mkdtemp()

COLOURS = [(150, 40, 40), (40, 110, 60), (40, 60, 140), (120, 80, 20), (90, 40, 120), (30, 100, 110)]

def picture(lines, size=600, n=0, alpha=False):
    """A labelled square: the case's lines in big type, a frame, corner marks."""
    bg = COLOURS[n % len(COLOURS)]
    im = Image.new('RGBA', (size, size), bg + (255,))
    d = ImageDraw.Draw(im)
    d.rectangle([4, 4, size - 5, size - 5], outline=(255, 255, 255, 255), width=6)
    for cx, cy in ((0, 0), (size - 60, 0), (0, size - 60), (size - 60, size - 60)):
        d.rectangle([cx + 14, cy + 14, cx + 46, cy + 46], fill=(255, 255, 255, 255))
    font = ImageFont.load_default(size=max(14, size // 12))
    y = size // 6
    for line in lines:
        w = d.textlength(line, font=font)
        d.text(((size - w) / 2, y), line, font=font, fill=(255, 255, 255, 255))
        y += int(size / 9)
    if alpha:   # the lower third transparent: the screen behind should show through
        px = im.load()
        for yy in range(size * 2 // 3, size):
            for xx in range(size):
                px[xx, yy] = px[xx, yy][:3] + (0,)
    return im

def enc(im, fmt, **kw):
    b = io.BytesIO()
    if fmt == 'JPEG' or (fmt == 'GIF'):
        im = im.convert('RGB')
    im.save(b, fmt, **kw)
    return b.getvalue()

def jpeg(im, **kw): return enc(im, 'JPEG', quality=88, **kw)
def png(im): return enc(im, 'PNG')
def jxl(im, **kw): return enc(im, 'JXL', **kw)
def gif(im): return enc(im, 'GIF')

def jpeg_to_jxl(jpg_bytes):
    """A JPEG recompressed losslessly into JPEG XL, as cjxl does to a .jpg."""
    b = io.BytesIO()
    Image.open(io.BytesIO(jpg_bytes)).save(b, 'JXL', lossless_jpeg=True)
    return b.getvalue()

# ---- audio: four seconds of a quiet tone, one base file per container ----

BASE = {}
def base(ext):
    if ext in BASE: return BASE[ext]
    path = os.path.join(TMP, 'base.' + ext)
    codec = {'mp3': ['-c:a', 'libmp3lame', '-b:a', '64k'],
             'flac': ['-c:a', 'flac'],
             'm4a': ['-c:a', 'aac', '-b:a', '64k'],
             'ogg': ['-c:a', 'libvorbis', '-q:a', '1']}[ext]
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    'sine=frequency=330:sample_rate=22050:duration=4', '-ac', '1',
                    '-af', 'volume=0.25'] + codec + [path], check=True)
    BASE[ext] = path
    return path

def track(folder, name, ext, title, album, n, img=None, mime='image/jpeg'):
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name + '.' + ext)
    shutil.copy(base(ext), path)
    if ext == 'mp3':
        t = ID3()
        t.add(TIT2(encoding=3, text=title)); t.add(TALB(encoding=3, text=album))
        t.add(TPE1(encoding=3, text='test_art_files')); t.add(TRCK(encoding=3, text=str(n)))
        if img is not None:
            t.add(APIC(encoding=3, mime=mime, type=3, desc='cover', data=img))
        t.save(path)
    elif ext == 'flac':
        f = FLAC(path)
        f['title'] = title; f['album'] = album; f['artist'] = 'test_art_files'; f['tracknumber'] = str(n)
        if img is not None:
            p = Picture(); p.type = 3; p.mime = mime; p.desc = 'cover'; p.data = img
            f.add_picture(p)
        f.save()
    elif ext == 'm4a':
        f = MP4(path)
        f['\xa9nam'] = title; f['\xa9alb'] = album; f['\xa9ART'] = 'test_art_files'; f['trkn'] = [(n, 0)]
        if img is not None:
            kind = MP4Cover.FORMAT_PNG if mime == 'image/png' else MP4Cover.FORMAT_JPEG
            f['covr'] = [MP4Cover(img, imageformat=kind)]
        f.save()
    elif ext == 'ogg':
        f = OggVorbis(path)
        f['title'] = title; f['album'] = album; f['artist'] = 'test_art_files'; f['tracknumber'] = str(n)
        if img is not None:
            p = Picture(); p.type = 3; p.mime = mime; p.desc = 'cover'; p.data = img
            f['metadata_block_picture'] = [base64.b64encode(p.write()).decode('ascii')]
        f.save()
    return path

def put(folder, name, data):
    os.makedirs(folder, exist_ok=True)
    open(os.path.join(folder, name), 'wb').write(data)

# ---- 1. Embedded: one track per way a picture rides inside a file ------

E = os.path.join(OUT, '1 Embedded')
EA = 'Art test: embedded'
cases = [
    # (number, ext, label lines, bytes, mime)
    (1,  'mp3',  ['01', 'MP3 APIC', 'JPEG baseline'],   lambda im: jpeg(im), 'image/jpeg'),
    (2,  'mp3',  ['02', 'MP3 APIC', 'JPEG progressive'], lambda im: jpeg(im, progressive=True), 'image/jpeg'),
    (3,  'mp3',  ['03', 'MP3 APIC', 'PNG'],             lambda im: png(im), 'image/png'),
    (4,  'mp3',  ['04', 'MP3 APIC', 'PNG + alpha', 'bottom third clear'], None, 'image/png'),
    (5,  'flac', ['05', 'FLAC PICTURE', 'JPEG'],        lambda im: jpeg(im), 'image/jpeg'),
    (6,  'flac', ['06', 'FLAC PICTURE', 'PNG'],         lambda im: png(im), 'image/png'),
    (7,  'm4a',  ['07', 'M4A covr', 'JPEG'],            lambda im: jpeg(im), 'image/jpeg'),
    (8,  'm4a',  ['08', 'M4A covr', 'PNG'],             lambda im: png(im), 'image/png'),
    (9,  'ogg',  ['09', 'OGG picture', 'JPEG'],         lambda im: jpeg(im), 'image/jpeg'),
    (10, 'mp3',  ['10', 'MP3 APIC', 'JXL lossy'],       lambda im: jxl(im, quality=90), 'image/jxl'),
    (11, 'flac', ['11', 'FLAC PICTURE', 'JXL lossless'], lambda im: jxl(im, lossless=True, effort=7), 'image/jxl'),
    (12, 'mp3',  ['12', 'MP3 APIC', 'MIME says PNG', 'bytes are JPEG'], lambda im: jpeg(im), 'image/png'),
    (13, 'mp3',  ['13', 'MP3 APIC', 'JPEG 1000x1000', 'progressive'], None, 'image/jpeg'),
    (14, 'mp3',  ['14', 'MP3 APIC', 'JXL from a JPEG'], lambda im: jpeg_to_jxl(jpeg(im)), 'image/jxl'),
]
for n, ext, lines, make, mime in cases:
    if n == 4:
        data = png(picture(lines, n=n, alpha=True))
    elif n == 13:
        data = jpeg(picture(lines, size=1000, n=n), progressive=True)
    else:
        data = make(picture(lines, n=n))
    track(E, '%02d %s' % (n, ' '.join(lines[1:]).replace('/', '-')), ext, ' '.join(lines), EA, n, data, mime)

# The ones that should show no picture: the format card instead.
track(E, '20 no picture', 'mp3', '20 no picture', EA, 20)
track(E, '21 GIF, not decoded', 'mp3', '21 GIF', EA, 21, gif(picture(['21', 'GIF', 'MUST NOT SHOW'], n=21)), 'image/gif')
track(E, '22 JXL small lossy, J40 refuses', 'mp3', '22 JXL 200px lossy', EA, 22,
      jxl(picture(['22', 'JXL 200px', 'MUST NOT SHOW'], size=200, n=22), quality=90), 'image/jxl')
track(E, '24 JPEG 2500x2500 progressive, too big', 'mp3', '24 JPEG too big', EA, 24,
      jpeg(picture(['24', '2500 px', 'progressive', 'MUST NOT SHOW'], size=2500, n=24), progressive=True), 'image/jpeg')

# ---- 2. Beside the tracks: the folder's picture ------------------------

def folder(name, files, ntracks=2, embedded=None):
    d = os.path.join(OUT, name)
    for fname, data in files:
        put(d, fname, data)
    for t in range(1, ntracks + 1):
        track(d, '%02d track' % t, 'mp3', '%s, track %d' % (name, t), name, t,
              embedded, 'image/jpeg' if embedded and embedded[:2] == b'\xff\xd8' else 'image/jxl')

folder('2a cover.jpg', [('cover.jpg', jpeg(picture(['2a', 'cover.jpg'], n=1)))])
folder('2b folder.png', [('folder.png', png(picture(['2b', 'folder.png'], n=2)))])
folder('2c front.jxl', [('front.jxl', jxl(picture(['2c', 'front.jxl'], n=3), quality=90))])
folder('2d Cover.JPG upper case', [('Cover.JPG', jpeg(picture(['2d', 'Cover.JPG'], n=4)))])
folder('2e cover.jpg beats folder.png', [
    ('folder.png', png(picture(['2e', 'folder.png', 'WRONG ONE'], n=5))),
    ('cover.jpg', jpeg(picture(['2e', 'cover.jpg', 'right one'], n=0)))])
folder('2f cover.png beats folder.jpg', [
    ('folder.jpg', jpeg(picture(['2f', 'folder.jpg', 'WRONG ONE'], n=1))),
    ('cover.png', png(picture(['2f', 'cover.png', 'right one'], n=2)))])
folder('2g cover.jpg beats cover.jxl', [
    ('cover.jxl', jxl(picture(['2g', 'cover.jxl', 'WRONG ONE'], n=3), quality=90)),
    ('cover.jpg', jpeg(picture(['2g', 'cover.jpg', 'right one'], n=4)))])
folder('2h embedded beats cover.jpg', [('cover.jpg', jpeg(picture(['2h', 'cover.jpg', 'WRONG ONE'], n=5)))],
       embedded=jpeg(picture(['2h', 'EMBEDDED', 'right one'], n=0)))
folder('2i refused embedded JXL and cover.jpg', [('cover.jpg', jpeg(picture(['2i', 'cover.jpg'], n=1)))],
       embedded=jxl(picture(['2i', 'EMBEDDED JXL', 'refused'], size=200, n=2), quality=90))
folder('2j AlbumArtSmall.jpg only', [('AlbumArtSmall.jpg', jpeg(picture(['2j', 'small', 'MUST NOT SHOW'], size=75, n=3)))])
folder('2k cover.gif only', [('cover.gif', gif(picture(['2k', 'cover.gif', 'MUST NOT SHOW'], n=4)))])
folder('2l no picture at all', [])

shutil.rmtree(TMP)
print('written to', os.path.abspath(OUT))
