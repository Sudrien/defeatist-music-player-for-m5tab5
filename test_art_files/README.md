# Cover art test set

Every way a cover reaches the screen, as short tracks to copy to a card
and play (6140). Each picture says what it is in large letters, so the
screen shows which picture was used without reading the log. A case
that must show nothing has no picture with its number on it, or one
that says MUST NOT SHOW.

The tracks are four seconds of a quiet tone. Rebuild them with
`build/make.py` (needs ffmpeg, Pillow, pillow-jxl-plugin and mutagen).

What happens when there is no picture to show is the format card, as
for any track without art.

## 1 Embedded

One folder with no picture beside the tracks, so only the picture
inside each file can show.

| Track | Picture | Decoder | Expected |
| --- | --- | --- | --- |
| 01 | MP3 APIC, JPEG baseline 600x600 | hardware | shows *01* |
| 02 | MP3 APIC, JPEG progressive | stb_image | shows *02* |
| 03 | MP3 APIC, PNG | pngle | shows *03* |
| 04 | MP3 APIC, PNG, bottom third transparent | pngle | shows *04*; the bottom third is the screen's background, not black boxes |
| 05 | FLAC PICTURE, JPEG | hardware | shows *05* |
| 06 | FLAC PICTURE, PNG | pngle | shows *06* |
| 07 | M4A covr, JPEG | hardware | shows *07* |
| 08 | M4A covr, PNG | pngle | shows *08* |
| 09 | Ogg Vorbis METADATA_BLOCK_PICTURE, JPEG | hardware | shows *09* |
| 10 | MP3 APIC, JPEG XL lossy 600x600 | J40 | shows *10*, after a few seconds |
| 11 | FLAC PICTURE, JPEG XL lossless | J40 | shows *11* |
| 12 | MP3 APIC, MIME says image/png, bytes are JPEG | hardware | shows *12*: the bytes decide, not the MIME type |
| 13 | MP3 APIC, JPEG progressive 1000x1000 | stb_image | shows *13*; about 10 MB of PSRAM for a moment |
| 14 | MP3 APIC, a JPEG recompressed into JPEG XL | J40 | shows *14* |
| 20 | none | -- | the format card |
| 21 | MP3 APIC, GIF | none | the format card: GIF is not decoded |
| 22 | MP3 APIC, JPEG XL lossy 200x200 | J40 refuses | no picture; the log names J40's reason |
| 24 | MP3 APIC, JPEG progressive 2500x2500 | stb_image refuses | no picture; the log says too many pixels |

## 2 Beside the tracks

A picture in the same folder as the tracks, for files with none inside
(or, in 2h and 2i, with one). Every folder has two tracks: the first
reads the picture from the card, and the second should take it from
the cover cache with no read (the log says which).

| Folder | Files there | Expected |
| --- | --- | --- |
| 2a | cover.jpg | shows *2a cover.jpg* |
| 2b | folder.png | shows *2b folder.png* |
| 2c | front.jxl | shows *2c front.jxl*, through J40 |
| 2d | Cover.JPG | shows it: FAT matches names without regard to case |
| 2e | folder.png, cover.jpg | *cover.jpg, right one*: the name decides before the format |
| 2f | folder.jpg, cover.png | *cover.png, right one* |
| 2g | cover.jxl, cover.jpg | *cover.jpg, right one*: JPEG before JPEG XL within a name |
| 2h | cover.jpg; the tracks carry a JPEG | *EMBEDDED, right one*: inside the file comes first |
| 2i | cover.jpg; the tracks carry a 200 px JPEG XL J40 refuses | **known gap**: the folder's cover.jpg is not tried after an embedded picture fails to decode, only after a file has none |
| 2j | AlbumArtSmall.jpg only | the format card: the 75 px Windows thumbnail is not looked for |
| 2k | cover.gif only | the format card: GIF is not looked for |
| 2l | nothing | the format card |

## What the decoders are

- **JPEG**: the P4's hardware engine (baseline, sizes divisible by 8),
  then TJpgDec (baseline), then stb_image (progressive, any size, within
  free PSRAM).
- **PNG**: pngle, streamed straight into the frame.
- **JPEG XL**: J40, since 6139. It refuses some files: lossy at 256 px
  or smaller, lossless at effort 1, flat lossy at 600 px and up, and
  some others. texttest/jxltest.c pins which.

The host tests for the same decoders are texttest/arttest.c (JPEG,
PNG) and texttest/jxltest.c (JPEG XL), on the small fixtures in
texttest/fixtures/art.
