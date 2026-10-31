#!/usr/bin/env python3
"""
gen_ogg_fixtures.py -- the Ogg files texttest/oggseektest.c probes (6142).

One second of tone, Vorbis and Opus, each without a picture and with one
in METADATA_BLOCK_PICTURE. A picture in the comment header makes that
header span pages (libvorbis writes about 4 KB a page), and the larger
one is more than the 64 KB the probe used to read in one go.

The picture bytes are random: only their size matters here.

Needs ffmpeg and mutagen. Usage: ./tools/gen_ogg_fixtures.py [outdir]

SPDX-License-Identifier: MIT
"""
import base64, os, random, subprocess, sys
from mutagen.flac import Picture
from mutagen.oggopus import OggOpus
from mutagen.oggvorbis import OggVorbis

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'texttest', 'fixtures', 'ogg')
os.makedirs(out, exist_ok=True)
rnd = random.Random(6142)

def encode(path, codec):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    'sine=frequency=440:sample_rate=48000:duration=1', '-ac', '1',
                    '-fflags', '+bitexact', '-flags', '+bitexact'] + codec + [path], check=True)

def with_picture(cls, src, dst, nbytes):
    with open(src, 'rb') as a, open(dst, 'wb') as b:
        b.write(a.read())
    f = cls(dst)
    p = Picture(); p.type = 3; p.mime = 'image/jpeg'; p.desc = 'cover'
    p.data = bytes(rnd.getrandbits(8) for _ in range(nbytes))
    f['metadata_block_picture'] = [base64.b64encode(p.write()).decode('ascii')]
    f.save()

v = os.path.join(out, 'vorbis.ogg')
o = os.path.join(out, 'opus.opus')
encode(v, ['-c:a', 'libvorbis', '-q:a', '0'])
encode(o, ['-c:a', 'libopus', '-b:a', '24k'])
with_picture(OggVorbis, v, os.path.join(out, 'vorbis_pic20k.ogg'), 20000)
with_picture(OggVorbis, v, os.path.join(out, 'vorbis_pic70k.ogg'), 70000)
with_picture(OggOpus, o, os.path.join(out, 'opus_pic20k.opus'), 20000)
