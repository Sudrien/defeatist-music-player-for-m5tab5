
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

### 8110 -- APE: the filter's inner loop in PIE (ESP32-P4 vector instructions)

**Why.** 8109's log: no change from 8108 (Extra High 1.09x, Insane 0.24x), so
the cost is instructions, not cache. A scalar tap is a dozen cycles; Insane
needs 1,552 of them a sample a channel. PIE does eight 16-bit multiply-
accumulates in one instruction.

**What.** `ape_taps()` in `ape.h` is the dot product plus the weight update
that `ape_nn_run()` used to do in two loops. `ape_taps_pie()` is inline
assembly, eight taps a step: `esp.ld.128.usar.ip` + `esp.src.q` read the
sample ring and the delta ring (both move one sample a step, so they are
never 16-byte aligned; the weights are, and are allocated so), `esp.vmulas.s16.xacc`
accumulates the products, `esp.vadd.s16` or `esp.vsub.s16` moves the
weights by the delta (sign of the input picks which) and `esp.vst.128.ip`
puts them back; `esp.movx.r.xacc.l` reads the sum. Operands are pinned to
a0-a4 because PIE can only address a0-a5, s0-s1, s8-s11, t3-t6. A zero input
(rare) takes the C loop, which changes nothing. Orders are all multiples of 16.

**Not verified, and why that is handled.** The P4's PIE is thinly documented
and there is no toolchain or board here. I used the forms Espressif's esp-dl
reference shows, and `esp.movx.r.xacc.l` by analogy to the `.w.` forms. So:
(1) `ape_pie_selftest()` runs the vector filter against the C one on noise
at orders 16, 64, 256 and 1280, through ring rolls and every alignment, the
first time an APE opens, and switches the vector path off, with a log line
`APE: the PIE filter passed/FAILED its self-test`, if a single output or
weight differs. (2) `-DAPE_NO_PIE` builds without it. (3) The vector add and
subtract saturate where the C wraps: they differ only for a weight at
+-32767; the CRC check would show it. (4) `idf.py -DAPECRC=1 build` checks
every frame's CRC while playing, so `bad frames` means something.

*Checked, on a host:* `apetest` runs twice, with the C loop and with a C
model of the PIE one (`-DAPE_PIE_MODEL`, which models `usar`/`src.q` as
documented): both bit-exact on all nine files, 174 checks, ASan/UBSan
clean. That tests the pointer, chunk and ring arithmetic, **not the
instruction semantics.** *Not checked:* that it assembles, and everything
about its behaviour on the board.

### 8111 -- APE: PIE for a zero input too, and the loads grouped

**8110's log** (`-DAPECRC=1`, so `bad frames 0` is a real check now; the
PIE self-test passed, 78 ms, and it assembled first time): High 5.84x
(was 3.25), Extra High 2.73x (was 1.09), Insane 0.76x (was 0.24), noisy
Insane (53) 0.96x (was 0.21), Fast 10.9x, Normal 7.3x, 24-bit 6.9x, mono
31x. Every frame's CRC good. Extra High is now comfortable; Insane is
close and not there.

**Two things left on the table.** (1) A zero input to a filter took the C
loop: it needs no weight update, so 8110 sent it past the vector code. File
52 is tones and silence, where zeros are common, which is why it trails the
noisy file (53) that has none. (2) The loop loaded, shifted, then used, one
stream at a time, so each use waited on the instruction before it.

**The change.** A zero input has its own vector loop (sum only: no delta
stream, no store). And the two streams are read together: the sample ring
and the delta ring are both vector-aligned and read at the same offset, so
they share a `SAR_BYTE`, and the loop issues both loads, the weight load,
then both `esp.src.q`, which gives each shift two instructions of slack.
The model asserts the shared offset.

*Checked, on a host:* `apetest`, both runs, 173/174 checks, ASan/UBSan
clean. *Not checked:* that it assembles, or the speed. If the shared SAR is
wrong the self-test says so and the C loop is used.

### 8112 -- APE: time the PIE loop hot in the cache, to say what is slow

**8111's log:** Insane 0.83x (52) and 1.04x (53), Extra High 3.01x, High
6.47x, Normal 7.3x, Fast 11.5x, 24-bit 7.6x, mono 32.6x; every frame's CRC
good; seeks in a Normal file land in about 0.4 s. 8111's two changes bought
about 10% (0.76 -> 0.83, 0.96 -> 1.04): not what taking the stalls out of
the loop should have bought.

**What the numbers say.** Working back from the timings, an eight-tap step
costs about 23-25 cycles at Insane and Extra High alike, about three cycles
for each of the eight PIE instructions in it. That is either the vector unit
issuing slower than one instruction in three cycles, or the loop waiting on
memory: an Insane channel's filters, rings and predictor are some 30 KB,
two channels alternate, and the data cache is 64 KB shared with everything.
Those have different cures (fused load-and-accumulate instructions, or
internal RAM for the rings), so 8112 measures instead of guessing.

**The change.** At the first APE open, after the self-test, the player runs
the vector loop 400 times over one 1,280-tap filter held in the cache, and
logs `APE: PIE loop hot in cache: N ns an 8-tap step (about C cycles at
360 MHz)`. If C is near 24, the loop is instruction-bound and the next step
is fewer instructions per tap. If it is near 8-10, play is waiting on
memory and the next step is where the rings live.

*Checked, on a host:* the bench under the model, ASan/UBSan clean; `apetest`
unchanged. *Not checked:* on the board, which is the point.

### 8113 -- APE: where it stands after 8112's log

**The log:** the PIE loop is 26 ns an 8-tap step with the data hot in the
cache (about 9 cycles), and play now runs at: Extra High 4.25x, Insane 1.63x
(52) and 1.52x (53), Normal 6.7-7.7x, 24-bit 6.7x, mono 31x; every frame's
CRC good (`-DAPECRC=1`). Insane at 1.6x works out to about 11 cycles a step,
close to the hot figure.

**An unexplained jump.** 8111 measured Insane at 0.83x and 1.04x; this log,
with only 8112's logging added (which allocates and frees one filter at the
first open), 1.63x and 1.52x. Nothing in 8112 touches the decoder's speed,
so the likeliest cause is where the filters' arrays landed in the cache: a
channel's rings and weights are some 30 KB in PSRAM, in a 64 KB cache that is
not fully associative, and a layout that puts them on the same sets costs
the extra 13 cycles a step that 8111's log showed. If so it is luck, and the
next build or a different heap state could land the other way. Not proved:
one log of each. If Insane's numbers drop below about 1.2x on a later log,
the fix to try is placing each filter's three arrays at deliberately
staggered offsets inside the one allocation.

**Seeks.** A frame is the unit decoding can start at, and an Insane frame is
1,179,648 blocks, 26.7 s. A seek to the far end of one costs up to that much
decode: about 16 s at 1.6x. Extra High (6.7 s a frame) costs about 1.5 s;
the levels up to High cost under 0.5 s. Nothing here changes the format; the
fix, if wanted, is a message on the screen while a long skip runs.


### 8114 -- the cover dims and a spinner turns while a seek runs long

**Why.** An Insane-level APE seek can decode up to a 26-second frame to land
(see 8113), so a press near the far end sat on a still screen for as long as
sixteen seconds with nothing to say it had been heard.

**What.** Half a second into a seek, the cover goes to half brightness and a
spinner turns on a dark disc at the centre of the art band; when the seek
ends the cover is back. Seeks that finish inside half a second show nothing
at all.

- `ui_art_busy_begin/tick/end()` in `ui.c`. Begin copies the art band to
  PSRAM (up to 1.4 MB in landscape, held only for the seek) and halves
  every pixel in place, `(p >> 1) & 0x7BEF` as RGB565 pairs; tick paints the
  disc and `gfx_draw_spinner()` and blits only the rows the spinner is on;
  end copies the band back and blits. Putting the copy back, not
  repainting the cover, means no decode, no black flash and no dependence
  on the repaint flags. If the copy cannot be allocated, none of the
  three does anything.
- `decoder_set_seek_progress()`: a callback the decoder calls from inside a
  long seek, on the seeking task. Only `ape_seek()` calls it, once per
  2048-block piece it decodes and drops.
- `seek_busy_cb()` in `player.c`, set around `decoder_seek_sec_at_cs()` in
  the decode loop, which is also the framebuffer's one writer and is blocked
  in the seek, so nothing else is drawing. It waits `SEEK_BUSY_AFTER_MS`
  (500), draws a frame every `SEEK_BUSY_FRAME_MS` (80), and stays out of the
  way when the chooser, a panel or the sleep page is up, or the microphone
  has the band.

**Not covered.** The other backends' seeks do not call the callback, so they
show nothing: MP3, FLAC, Ogg and the rest land well inside half a second, and
DSD's cost is paid by the reads after the seek, not in it.

*Checked:* the dim and spinner code compiles on its own with stubs.
`ui.c`, `player.c` and `decoder.c` do not build on a host. *Not checked:* any
of it on a board. What to look for: seek to the far end of 52 (Insane): after
half a second the cover dims with a spinner, and returns whole when playback
starts; a seek in 48 shows nothing.

### 8115 -- DSD stage 1: half the lookups

The 8114-era DSD log: DSD64 stereo 2.35x realtime, DSD128 stereo 1.39x, mono
DSD128 4.6x. Stage 1 (bits to 176.4 kHz, `K` table lookups per output) is
nearly all of the time; the two halfbands are about 55 multiply-adds a frame.

**What changed.** `K` is now `DSD_KB * mult` with `DSD_KB` 16, not 32: 128
taps at DSD64, 256 at DSD128, so 16 and 32 lookups an output, and the table
is 16 KB and 32 KB instead of 32 and 64. Stage 1 only has to keep 0-22 kHz
and reject what folds into it (from 154 kHz up); the halfbands do the rest.
Measured offline against a 64-byte window (zero-phase, same decimation
phase, 5 s of file 41 and of 42): `K` 32: -141 / -154 dB re full scale;
16: -127 / -143 dB (0.015 and 0.002 LSB of 16 bits); 12: -88 / -104 dB,
which is where it breaks, so 16 is the floor. `latency` goes 39 to 38
frames (it follows the window). Two outputs, `S1` bytes apart, are summed per
pass (eight sums in flight, same per-output order, so the result is the same
whether paired or not).

*Checked:* `dsdtest`, 101 checks, SNR unchanged to the digit (the files'
own modulator noise is the floor, so the offline comparison above is the
real quality test). *Not checked:* speed on a board; the host is no guide.
Expect about 1.6-1.8x on the filter time: DSD128 stereo near 2.2-2.5x,
DSD64 near 3.5-4x. Send the `dsd timing` lines for 41, 42 and 45.

### 8116 -- DSD256 in, and the stage-1 window's alignment

8115's log: DSD64 stereo 3.46x, DSD128 stereo 2.35-3.36x, mono 6.8x. DSD256
needs the same 176.4 kHz output from four times the bits, which at a 48-byte
window is 48 lookups an output (DSD128 had 64 before 8115, at 1.39x), so
`DSD_MAX_MULT` is 4 and 43 plays.

**The window.** `DSD_K_BYTES(mult)`: 16 per unit up to DSD128 as in 8115,
48 bytes (12 per unit) at DSD256. Offline against a 128-byte window on file
43: 64 bytes -154 dB, 48 bytes -116 dB (0.05 LSB of 16 bits), 32 bytes -97
dB (0.5 LSB). Table 48 KB.

**A bug that 8115's window sizes hid.** The history after a reset was
`K - S1` bytes, which puts a window's centre `K/16` stage-1 samples from the
output instant, and the filters decimate by four more, so the output lands
on the 44.1 kHz grid only when `K/16` is a multiple of four: K a multiple of
16 * mult. 32/16/64 are; 48 at DSD256 is not, and came out time-shifted by a
fraction of a frame (31 LSB against a reference that agreed to 0.3 with 64
bytes; the earlier "K 24, K 20" offline diffs were the same thing). The
history is now `K/2 + 3*S1`, which is `K - S1` for the sizes that worked, so
those are bit for bit what they were, and `latency` takes a flat three stage-1
samples for the window: 38 frames at every rate. `dsd_conv_new()` refuses
a `K` under six hops or not a multiple of four.

**Not covered.** A seek in DSD256 reads forward and discards (the card gives
about 9 MB/s, and DSD256 stereo is 5.6 MB/s of file), so a seek to the far end
of a 5-minute file is four times DSD64's wait.

*Checked:* `dsdtest`, 127 checks including 43's decode (SNR 70 dB against file
01, 1.2 LSB error, better than 42's), seeks and forward skips on it. The seek
test's third target moved from 3 s to 2 s + 30001 frames, because 43 is 3 s
long. *Not checked:* on a board. Expect DSD256 stereo at roughly 1.6-2.3x.
Send the `dsd timing` line for 43.
