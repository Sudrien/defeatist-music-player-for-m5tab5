# boxwin

How long the player's own decoders compute between host calls when they run
in a feckless box (u-defeatist-0002). It is a measurement, not part of the
player: the firmware does not build it and nothing in `main/` includes it.

## Why

A boxed app (feckless-libs' `umode` branch) runs in RISC-V user mode in
*windows*: from the return of one host call to the `ecall` of the next. A
window holds interrupts off on its core (u-feckless-0017). The panel's
interrupt is kept off the app's core by running the app on core 1
(u-feckless-0031), but the drivers the app brings up itself -- the player's
audio output, the USB host -- get their interrupts on the app's core, behind
DMA buffers of about 20 ms. So whether Defeatist can be boxed with windows
masked comes down to one number per format: the longest window its decoder
makes. This measures it on the board.

MP3 and MP2 (minimp3), APE (`ape.h`) and DSD (`dsd.h`) are the formats the
player decodes in its own code. FLAC, AAC, Vorbis, Opus and ALAC are
esp_audio_codec's, linked into the launcher; in a box they would be host
calls, which are not windows, and are not measured here.

## Build

Needs ESP-IDF 6.1 and feckless-libs' `umode` branch, which the manifest
names. A project of its own: its `dependencies.lock` is written here by the
first configure and is not the player's.

    cd m5tab5/boxwin
    idf.py set-target esp32p4
    idf.py elf

`build/defeatist_boxwin.app.elf` is the app. The build ends with `boxcheck`:
if it lists a name, a box does not answer something this imports, and the
list is the result.

## Run

1. Make a folder called `boxwin` at the top of the card and copy
   `m5tab5/test_audio_files/` into it, whole or just its
   `.mp3 .mp2 .ape .dsf .dff`. Files in `boxwin` itself and in the folders
   directly inside it are decoded; nothing deeper. Add any file of your own
   that is the worst you play; 30 s of each is decoded.
2. Put `defeatist_boxwin.app.elf` where Avoidant's `umode` branch lists apps,
   and run it. It must be a launcher that boxes: a trusted one refuses the
   ELF by name (`feckless_abi_box_v1`).
3. Read the log. The panel shows the longest window a file, in yellow from
   5 ms and red from 20 ms, for a board with no cable on it.

## The log

    boxwin: double arithmetic: hash 1beec1c3, the host's; an add 356 ns, ...
    boxwin: 64-bit division: hash 2bebcd67, the host's; a divide 300 ns, by 1000 151 ns
    boxwin: next: test_audio_files/51 ape-extrahigh.ape
    boxwin: 51 ape-extrahigh.ape: ape 4000 44100 Hz x2, 30000 ms in 18448 ms
            (1.62x); 1338 windows, longest 16.3 ms, 1316 >=5 ms, 0 >=20 ms;
            set-up 28 ms; 736 KB in 1242 ms; pcm 96460869 = host !

(a board's, u-defeatist-0011; one line in the log, folded here.) The two
arithmetic lines, then two lines a file from the C kernels, then `the PIE
self-tests next`, then the same files with the P4's PIE kernels, which is
what the player ships with. A line ends ` !` when its longest window is 5
ms or more and ` !!` at 20 ms or more. It is kept under 224 characters: a
box's log line is 256 with its prefix, and one that does not fit loses its
end and its newline.

- **The first two lines** are the box's arithmetic (u-defeatist-0010): a
  fixed run of every double operation and 64-bit division the compiler
  emits a call for, hashed, and the hash compared with a host's. `the
  host's` on both means every result was the right bits. The times are of
  one operation: a trap into the launcher with feckless-libs before
  u-feckless-0036 and 0038, the routine's own after.
- **pcm** is a hash of the samples that came out. ` = host`: what a host
  decodes. For APE, an integer decoder, it must be, C kernel or PIE;
  ` != HOST: WRONG` there is wrong audio. MP3 and DSD go through float, and
  the board's compiler fuses a multiply and an add where the host test is
  told not to, so theirs say ` != host (float)`. They are still the same
  from run to run and from the C kernel to the PIE one, which is what to
  compare when the box's kernel or its arithmetic changes.
- **longest** is the figure. Under 5 ms: no concern. 5 to 20 ms: the panel
  would flash if the app ran on the panel's core; fine on core 1. 20 ms or
  more: an audio underrun is possible with windows masked.
- **(N.NNx)** is decode speed against real time, with the reads left out:
  `30000 ms in 18448 ms` is that much audio decoded in that much computing. An
  APE window is 1024 samples, 23 ms of audio, so its length is 23 ms / speed;
  a smaller chunk in the player would shorten it in proportion. Under 1.00x
  the format cannot be played at all, whatever its windows.
- **set-up** is the probe and the tables, before the first window: for DSD,
  the filter design, which is double arithmetic and so thousands of host
  calls in a box. **KB in ms** is the reads while decoding, lease and
  seek included.
- **`next: <file>`** comes before each file. A fault ends the app with no
  line of its own, so the last `next:` is the file it happened in.
- **If the app ends at the PIE self-test**, a box does not turn the PIE unit
  on for user mode (the FPU needed u-feckless-0028 for the same reason). The
  first run that got that far ended exactly so, inside `dsd_conv_new()`
  (u-defeatist-0005). The C table above it is then what a boxed player would
  have, and PIE in a box is a request to feckless-libs.
- **`ended (reason 1)`** from Avoidant is a fault. Its cause, address and
  value are on the panel -- `The app stopped: <cause> at 0x... (0x...)` --
  and not in the log; copy the line down.
- Avoidant's own line at the end, `ended (reason N), C calls, I interrupts`,
  belongs with the table: with windows masked, I should be near 0.

A window recorded here is an upper bound: see `main/boxwin.h`.

## Host test

    make -C test FECKLESS_STORAGE=/path/to/feckless_storage_handler

`main/boxwin_run.c` as it goes into the box, over `../test_audio_files`,
under ASan and UBSan: every format decodes to its length, the one DST file
is refused, the window bookkeeping adds up, the arithmetic hashes are what
`boxwin_arith.h` says and each file's PCM hash what `boxwin_expect.h` says
(`make -C test expect` writes that file again). The times it prints are the
host's and mean nothing for the board.
