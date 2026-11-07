# SPDX-License-Identifier: MIT
# rawwav.py in.raw out.wav channels rate bits -- wrap raw PCM in a plain 44-byte
# WAV header. ffmpeg writes WAVE_FORMAT_EXTENSIBLE for 24-bit and a LIST chunk
# besides, and the Monkey's Audio encoder (mac) reads neither: it segfaults.
import sys, wave
raw, out, ch, sr, bits = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
w = wave.open(out, "wb"); w.setnchannels(ch); w.setsampwidth(bits // 8); w.setframerate(sr)
w.writeframes(open(raw, "rb").read()); w.close()
