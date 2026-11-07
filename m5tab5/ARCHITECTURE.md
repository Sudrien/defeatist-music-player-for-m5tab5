
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
