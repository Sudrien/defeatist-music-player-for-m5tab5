# SPDX-License-Identifier: MIT
# apetag.py in.ape out.ape -- append an APEv2 tag (footer only) to an .ape.
# The encoder writes none, and a player has to know where the audio ends
# when something follows it: the descriptor's sizes say, the file length
# does not.
import struct, sys

items = [("Title", "ape-apev2-tag"), ("Artist", "Defeatist test files"),
         ("Album", "m5tab5 test_audio_files")]
body = b""
for k, v in items:
    vb = v.encode()
    body += struct.pack("<II", len(vb), 0) + k.encode() + b"\0" + vb
size = len(body) + 32                  # items + footer, as the format counts it
footer = b"APETAGEX" + struct.pack("<IIII", 2000, size, len(items), 0) + b"\0" * 8
open(sys.argv[2], "wb").write(open(sys.argv[1], "rb").read() + body + footer)
