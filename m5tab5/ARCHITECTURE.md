
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
