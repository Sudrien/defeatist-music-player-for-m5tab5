
### 8100 -- DSD: read a run with one seek, and log the timing where it can be seen

**What 8099's run said.** Still scratchy, and the numbers were the same as
before 8099's four running sums: after a seek, DSD64 stereo decodes at
about 21,600 and 22,300 frames/s (41, 15 s: 663,001 -> 707,033 -> 751,065
over 2.04 s and 1.97 s) and 22,000 and 23,200 (41, 2 s: 89,561 ->
134,617 -> 177,113), and the bar advances one second per two. The four
sums did not change it, which is information: it is not the chain of
float adds, or not only. There is no `dsd timing` line in that log
because the track was never closed after the seeks; that was the
logging's fault (8099 printed at close only), and it printed nowhere
useful.

**The pattern in the rates.** Per `dsd_next()` call the time is about 23
ms for a stereo DSD64 block (8 KB, 512 frames), 14 ms for a mono one
(4 KB, 512 frames) and 20 ms for a DSD128 stereo block (8 KB, 256
frames). That is about the same number of milliseconds per KB read in
all three (3 ms/KB, 3.4, 2.5), and not the same per frame, per lookup or
per channel -- so it looks like the time goes on the read, which the
filters' cost per frame would not predict. That is a reading of five
numbers and not a measurement.

**A suspect, from the storage layer's source (feckless_storage_handler's
`storage_io.c`, v0.9.2).** `dsd_next()` read every block with
`storage_io_read_at()`, which is an `fseek()` in a lease of its own and
then the read in another. The other decoders read a run the way
`aiff_read()` does: one `fseek()` and then `storage_io_fread()` for each
block after it. A stdio `fseek()` on a handle with a 16 KB buffer
(`STDIO_BUF_SIZE`), 4-8 KB at a time, can throw the buffer away and fetch
it again, so a block costs a card read of the buffer's size rather than
the block's. Whether newlib does that here depends on its flags for a
stream given `setvbuf()`, and this was not tested: the host cannot show
it, because the reads there are not the card's. It is the one thing in
the DSD path that differs from the paths that play at 705 kbps without
trouble, and it is the whole of the change.

**The change.** `dsd_rd_t` has `at`, the file offset the stream is
positioned at, `-1` after a seek plan. `dsd_next()` calls
`storage_io_fread()` when the next block starts where the last ended, and
`storage_io_read_at()` otherwise (the first block after a seek, which is
one `fseek()`). The decoded samples are identical -- the host test passes
unchanged, with a `storage_io_fread()` stub added -- so this is I/O
only.

**And the logging, so the answer is in the log.** `dsd timing` is printed
when a track's decode reaches its end (which is early, while the track is
still playing), at every seek, for the stretch each covers, and at close,
and it now says how many card reads there were and how long each took:

    dsd timing: <audio> ms of audio; filters <n> ms (<x>x realtime), card reads <n> ms (<x>x) in <n> reads, <t> ms each

It zeroes itself each time. If "filters" is under 1.00x on the board, it
is the filter; if "card reads" is, it is the card, and `ms each` is the
figure to compare with `aiff`'s or a FLAC's.

*Checked, on a host:* `dsdtest` unchanged and passing under ASan/UBSan
with the new read path, including the seeks (the first block after one
takes the `storage_io_read_at()` branch); the glue in `decoder.c`
compiled against stubs and printing the new line. *Not checked:* any of
it on the board -- the suspect above in particular, which is why the
timing line is there, and whether it is read amplification at all.

### 8101 -- DSD: the first read after a seek starts on a 16 KB boundary

**What 8100's timing line said** (41, DSD64 stereo, one seek):

| stretch | audio | filters | card reads |
| --- | --- | --- | --- |
| 0 s to the seek at 8 s | 19,713 ms | 8,295 ms, 2.37x realtime | 1,989 ms, 9.90x, 1,698 reads, 1.1 ms each |
| 3 s to the end of the file | 27,004 ms | 11,588 ms, 2.33x realtime | 32,147 ms, **0.83x**, 2,326 reads, **13.8 ms each** |

So it is not the filter: 2.33x realtime is a margin, and the four running
sums of 8099 were not it, as that run already suggested. It is the card,
and not the card being slow in general: the same file, the same code and
the same 8 KB reads cost 1.1 ms each before the seek and 13.8 after it,
for the rest of the file (`track playback`: 54% of the wall time held,
against about 10% in the first run). 8100's one-seek-per-run change was
not it either.

**The same slowdown is in the other formats.** `mp2` after a seek had
904 and 1,028 KB/s (`track playback`, worst hold 350-369 ms) in the
second and third runs against 6,827 KB/s from the start of the file in
the first. A low-bitrate file does not mind, and DSD64 stereo needs 705
KB/s of the card and DSD128 1,411, so it is the first thing here that
does. Which means this is probably not DSD's to fix, and the fix below
is a workaround in `dsd.h` for something in the layer under it.

**The mechanism is a guess.** 13.8 ms for 16 sectors is 0.86 ms a sector,
the cost of a card read one sector at a time rather than as a burst,
which is what ESP-IDF's SD driver does when it is handed a buffer it
cannot DMA into directly. FatFs hands it one when a read starts partway
through a sector: the sector's tail comes out of its own window, and the
whole sectors after it land at `buffer + (512 - offset)`, which is not on
a cache line. A DSF's audio starts at byte 92, so a stream positioned
there, or at any 92 + n * 8192, refills its 16 KB stdio buffer from
unaligned file offsets for as long as it runs. Why the first stretch is
fast on the same offsets is not explained by this: newlib may align a
seek near the start of a file to the buffer size and not one further in,
or something else is different about a stream that has never been
sought. It was not tested; the host cannot show it.

**The change.** On the first read after a seek, `dsd_next()` positions the
stream on a `DSD_READ_ALIGN` (16 KB, the stdio buffer's size) boundary at
or before the block, reads up to the block and throws that away, then
reads the block, so every refill after it starts on a sector boundary the
way a read from the start of a file does. It costs up to 16 KB of reading
a seek. After that it is `storage_io_fread()` as in 8100. The samples are
identical; `dsdtest` now also checks that the one `fseek()` a seek makes
lands on a 16 KB boundary and that there is only one.

**If it works** the next patch is `storage_io_read_at()` in feckless-libs'
`feckless_storage_handler` doing this for everyone, since every
seek-then-read in the player goes through it and every format after a
seek is on the slow side of it; this patch would then be redundant and
should go. **If it does not**, the `dsd timing` line will still say 13.8
ms each, and it was not alignment, and what remains is the seek itself
(`f_lseek` and a cluster chain on a file of 20 MB and more) or the card.

**The other numbers worth keeping.** DSD128 stereo: filters 2,547 ms for
3,668 ms of audio, 1.43x realtime, with reads at 1.1 ms each. That is 70%
of a core to decode it, and little margin; DSD64 at 2.33x is 43%.
`DSD_MAX_MULT 1` takes DSD128 away.

*Checked, on a host:* `dsdtest` passes (74 checks, ASan/UBSan, no leaks),
including the new alignment checks, and the glue in `decoder.c` compiled
against stubs. *Not checked:* anything on the board; that is the point of
it. Whether newlib or FatFs behaves as described above is not known.

### 8102 -- DSD: seek by reopening the file; the read times in full

**8101 did not work.** With the first read after a seek on a 16 KB
boundary, the reads after a seek cost 14.8 and 14.0 ms each, against 1.1 ms
before it. The alignment hypothesis is refuted, and 8101's code is taken
out again.

**What this adds**, both for the next log:

- `dsd reads:` line after each `dsd timing:` line: the longest read, how
  many were over 5 ms, and the first 16 reads after a seek in 0.1 ms. That
  says whether the slowness is a one-off at the seek, a steady cost on
  every read, or a few very long ones.
- `DSD_SEEK_REOPEN` (default 1): `dsd_seek()` closes the file and opens it
  again instead of keeping the stream it already has. A stream that has
  never been sought reads at 1.1 ms. If the line says 1 ms after a seek
  again, the cost is in the stream or in the position FatFs keeps for it
  after `f_lseek`, and reopening is the workaround; if it still says 14 ms,
  it is not, and `-DDSD_SEEK_REOPEN=0` puts `fseek` back.

*Checked:* `dsdtest` (65 checks, ASan/UBSan clean). *Not checked:* any of it
on the board. The reopen is an experiment, not a fix, until a log says so.

### 8103 -- DSD: no seek on the stream; read up to the target from byte 0

**8102's log** (reopen on seek, build -47). Reads after a seek were as slow
as before: 14.7 and 14.0 ms each on 41, 7.6 ms on the mono 46, and the first
reads after a seek were ~40 ms every 16 KB fill, with the second 8 KB slice
of each fill at 0.1 ms. What it rules out:

- *the stream's state*: the file was reopened and it did not matter;
- *alignment*: 8101 did not matter either;
- *position*: 46 played from the start reads the same bytes at 0.6 ms, then
  opened again and sought to 1 s reads them at ~7 ms;
- *time or a card gone slow*: the DSD128 track opened right after the slow
  stretch, with no seek, read at 1.7 ms;
- *CPU*: the short tracks decode in a burst at full CPU at 0.6-1.1 ms.

What is left is that any lseek on the stream makes the reads after it slow.
Every stream that read fast never made one: opening at byte 92 is inside the
first 16 KB buffer, so stdio moves a pointer. mp2 being slow after a seek
fits (for a format that needs no bandwidth it only shows as a stall).

**The change:** `dsd_seek()` reopens the file as in 8102 and leaves the
stream at byte 0 (`rd.at = 0`); `dsd_next()` reads up to the target with
`storage_io_fread()` and discards, so no lseek is ever made on it. Cost: a
seek reads the file up to the target, about 9 MB/s on the board, so ~1 s for
the far end of a 5-minute DSD64 stereo file (twice that for DSD128).
`-DDSD_SEEK_REOPEN=0` restores the plain seek.

*Checked:* `dsdtest` (83 checks, ASan/UBSan clean): a skip lands on the same
samples as the seek, with no seek. *Not checked:* any of it on the board; if
the next log still says ~14 ms the cause is not the seek either and this
goes.

### 8104 -- DSD: a forward seek skips from where the stream is

**8103's log:** the crackle is gone, reads after a seek are 1.1-1.7 ms
again, so an lseek on the stream is what made them slow. What it costs: the
first read after a seek took 0.9, 1.4 and 1.9 s, because every seek reopened
the file and read up to the target from byte 0. (`CONFIG_FATFS_USE_FASTSEEK`
is on in `sdkconfig.defaults` and stays on; whether it is the cause was not
tested, as other projects need it.)

**The change:** `dsd_seek()` keeps the stream when its position (`rd.at`) is
at or before the first byte the new plan reads (`dsd_rd_offset()`), and
`dsd_next()` skips the gap by reading, so a forward seek costs the distance
and no more. Only a backward seek, or one before the first read, reopens and
reads from 0.

*Checked:* `dsdtest` (101 checks, ASan/UBSan clean), forward skips land on
the same samples with no seek. *Not checked:* on the board.

### 8107 -- APE: Monkey's Audio, decoded in `ape.h`

**What this is.** `.ape` files play: `ape.h` (header-only, like `dsd.h`) is a
decoder for Monkey's Audio file versions 3990 and later, which is what every
encoder has written since 3.99. It is written from the format, tested against
files made by the reference encoder (`mac`, 48-56), and uses the reference's
constants and filter structure; the licence note is in the file's header.
Older files, 8- and 32-bit, more than two channels, a file with an ID3v2 prefix
and anything over 4 GB are refused with the reason in the log.

**The format, as far as it mattered here.** A 52-byte descriptor, a 24-byte
header, a seek table of 32-bit file offsets (one per frame), then the frames.
Frames are independent: predictors, Rice state and history reset at each one,
so a seek is "open the frame the target is in and decode up to it". A frame
is a CRC, optionally a word of special codes (silence, pseudo-stereo), and a
range-coded stream of residuals. Two things cost time to find: the stream is
little-endian 32-bit words read from the top byte, so each group of four
bytes is reversed (aligned to the first frame's offset, not the file's); and
24-bit files use the same 32-bit wrapping predictor as 16-bit ones, not a
wider one. The decoded frame is checked against its own CRC in the host test.

**The decoder.** Per channel: up to three NN filters (16/11 Normal, 64/11 High,
32/10 and 256/13 Extra High, 16/11 256/13 1280/15 Insane; none for Fast), then
a two-stage predictor over the channel and its partner, then the Rice-like
range decode. Stereo is mid/side by the reference's order (Y first, then X).
24-bit is folded to 16 as `pcmfold_24_to_16` does. All the memory is PSRAM:
about 40 KB a channel at Insane, a 16 KB read buffer.

**In `decoder.c`.** `BACKEND_APE`; `ape_open` logs version, level, bits,
channels, rate and frame size. `ape_read` decodes straight into the caller's
buffer and stops at the header's block count (a tag after the audio is
never read: `data_end`). `ape_seek` opens the frame and decodes and drops
the rest, exact to the block; a frame is 73,728 blocks up to High and four
times that for Extra High and Insane. `duration.c` reads the length from the
header. **`ape timing:`** is logged when a track ends or a seek begins:
audio ms, decode ms and its realtime factor (audio seconds per second of
decoding alone, card reads excluded), card ms and factor, reads, and bad
frames. Under 1.00x on the decode figure means that level cannot play on this
board in plain C.

**Seek and the card.** A frame open is an lseek, and 8100-8104 found that
after one the card reads on this board cost 14 ms an 8 KB block. APE needs
60-110 KB/s, which even that rate covers, so APE seeks plainly and does not do
DSD's read-and-discard. If `ape timing` shows slow reads after a seek and the
decode factor is fine, that is the 8100 effect again.

**Expected, not measured:** NN taps a sample per channel are 16, 64, 288 and
1,552 for Normal, High, Extra High and Insane. Plain C on a 400 MHz RISC-V
should manage Normal and High easily and probably Extra High; Insane is the
one to watch, and file 53 (noisy, `-c5000`) is the worst case. If it does not
keep up, the dot product and the adapt step are the PIE SIMD candidates.

*Checked, on a host:* `apetest` (173 checks, ASan/UBSan clean): all nine
files bit-exact with the reference decode, every frame's CRC good, 16-bit
files equal to 01's samples, seeks into three frames land on the same
samples as playing through, the refusals, and damaged data caught.
`decoder.c` does not build on a host, so the integration was syntax-checked
on its own with stubs. *Not checked:* any of it on the board.


### 8109 -- APE: a smaller working set, and the filter in one pass

**8108's log** (decode time alone, card reads excluded): Fast 9.3x
realtime, Normal 5.4x, High 3.2x, Extra High 1.09x, Insane 0.24x (52) and
0.21x (54-noisy file 53). 24-bit Normal 4.6x, mono 22 kHz 21x. No bad
frames anywhere. Normal and High are comfortable; Extra High is too close to
play beside the UI, and Insane is four to five times too slow. That is about
12 cycles a filter tap against the 2-3 the loop should cost, which points at
the memory, not the arithmetic: the roll buffers held 4096 samples, so
Insane's two channels walked over roughly 200 KB of PSRAM-backed data a
second sample after sample, far past the data cache.

**The change:** `APE_WINDOW` 4096 -> 512 (the buffers roll sooner; the
roll is a memmove of the history, so the cost is tiny and the output is
the same), which puts a channel's working set at about 10 KB. And
`ape_nn_run()` does the dot product and the weight update in one pass over
the arrays instead of two.

*Checked:* `apetest` unchanged and passing, 173 checks, bit-exact.
*Not checked:* the speed, which is a board number. If Insane is still under
1.0x after this, the next step is PIE SIMD for the dot product and update.
