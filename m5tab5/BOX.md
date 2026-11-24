# The player in a feckless box

The `umode` branch's player: main/'s sources, built into an ELF for a
feckless box -- RISC-V user mode, one block of PSRAM, every call out of
it an `ecall` the launcher checks -- and a .bin whose `standalone.c` is a
launcher that runs it in one. (Until u-defeatist-0028 this was a project
of its own, `boxed/`, beside a trusted build; it is now the only build.)

## Building

ESP-IDF 6.1, in `m5tab5/`, as ever:

    idf.py set-target esp32p4
    idf.py build

makes both, side by side:

    build/defeatist_player.app.elf                 the player, for a box
    build/defeatist_player.app.elf.debug           the same, unstripped, for addr2line
    build/defeatist_music_player_for_m5tab5.bin    a launcher that runs it in one

The ELF's build (`elf/`) ends with two checks. `feckless_abi` holds every
import to the ABI's core list and the box, storage, NVS and audio
sections; `feckless_box_libc` (boxcheck) holds every import to what a box
answers. Both must pass.

The manifest (`main/idf_component.yml`) follows feckless-libs' `umode`
branch; `dependencies.lock` records the commit.

## What runs, and what does not

What a box answers (feckless-libs' umode branch, to u-feckless-0053): the
screen and touch, gfx, the SD card and USB storage (reading and writing),
audio output through the codec or USB, the battery and the RTC, FreeRTOS's
locks, queues, stream buffers and notifications, threads (eight besides
app_main: the offline player uses three, a recording two more, a reindex
and a GIF one each), esp_audio_codec (0052) and NVS (0053).

So the player's local playback: browsing the card, the library, playing
MP3, MP2, WAV, AIFF, APE and DSD (DSF, plain DFF) with its own decoders,
ReplayGain, the Fx sheet, recording to the card.

Settings the player keeps in NVS -- volume, brightness, rotation, NTP,
the screen's dim and off times, the language, the clock's correction --
are kept through the box (u-feckless-0053), in the namespace `defeatist`,
if the launcher names it for this app (`feckless_box_calls_nvs()`). If it
does not, NVS reads as empty and the player starts from its defaults and
the card's settings file, as it would on a device with nothing stored.

FLAC, AAC/M4A, Vorbis, Opus and ALAC go through esp_audio_codec, which a
box answers where the launcher links it (u-feckless-0052, u-defeatist-0023);
the ELF then requires the ABI's audio section, and a launcher without the
codec refuses it by that name. For such a launcher, build with

    idf.py -DDEFEATIST_BOXED_CODEC=OFF build

which puts the codec in `main/absent_codec.c` as absent: those formats say
"not supported", and the rest play. CMake keeps the setting: a later plain
`idf.py build` is still built without the codec until
`idf.py -DDEFEATIST_BOXED_CODEC=ON build`.

What it does not answer is in `main/absent.c`, as the app's own functions
that fail the way the player already copes with:

| absent | so |
|---|---|
| the setup portal's DHCP options and captive DNS | nothing a person reaches: every network feature works, over a cable, Wi-Fi or the player's own hotspot -- MPD, web radio, the benchmark, podcast search and downloads, remote control, NTP, and adding a network, typed on the screen (u-defeatist-0042, 0044, 0045, 0046, 0047, 0048, 0049) |
| esp_ota, esp_partition | the table says "hosted" (u-defeatist-0025): Exit to Launcher shows; the app ends asking for the launcher (u-defeatist-0055), and the launcher goes there |
| raw I2C, deep sleep | the app cannot give the power-off pulse itself: power off ends it asking to power off (u-defeatist-0055), and the launcher does |
| USB class registration | an app's own USB class drivers; Defeatist's are the launcher's (storage, audio, Ethernet), and a headset's or keyboard's media keys are answered (u-defeatist-0054) |

The hardware JPEG decoder, the crypto section (approve.c's hash) and HID
were once in this table; the box answers them now.

**Power off, and the low-battery power off, are the launcher's to do.**
The pulse to the power controller is raw I2C, which the app cannot
reach. The app saves its settings and ends, saying it wants the power
off (`feckless_box_end_wish`, u-feckless-0087), and a launcher that
reads it (`feckless_box_calls_wish()`) gives the pulse. The .bin's own
launcher does; a launcher that does not read it is left to guess, and
whatever protects a low battery from there is that launcher's.

Each comes out of `absent.c` when a box answers it: the box's call then
replaces it, and the checks say which are left.

## On a board

**The .bin on its own.** Flash it as the player always was. It brings up
the console, NVS, the bus, USB, storage and the network, then the panel
on core 0, and runs the player in a box on core 1: the largest PSRAM
block less 2 MB, at most 24 MB, NVS in the namespace `defeatist`. Power
off and Exit to Launcher both end the player in a box, and it says which
it meant (u-defeatist-0055): the .bin powers the board off itself for a
power off (the PMS150's pulse, which the box cannot give), even with
M5Launcher installed, and goes back to M5Launcher for Exit to Launcher.
A fault restarts it.

**Under Avoidant's umode branch.** The ELF on the card or a USB drive as
`defeatist_player.app.elf`, or a release's
`defeatist_<version>_music_player_for_m5tab5.elf`: Avoidant names the NVS
namespace from the file name up to the first `_`, so either gets
`defeatist`. Exit to Launcher, or power off, ends it and Avoidant's menu
comes back. The player says which it meant (u-defeatist-0055); until
Avoidant reads `feckless_box_calls_wish()`, the board stays on for both.

The box must be as large as it allows: the ELF and its statics are
1.35 MB, and the player's three PCM rings 10.5 MB of the box's heap
before anything else it allocates.

What to look for in the log: `standalone: the player in a box of N KB`
(the .bin) or Avoidant's own line; `=== Defeatist Music Player ===`, then
the player's start-up; `no PCM ring` if the box was too small for the
rings; any `thread N ended: reason` line from feckless_umode, with the pc
(`riscv32-esp-elf-addr2line -pfiaC -e build/defeatist_player.app.elf.debug`;
the `.app.elf` itself is stripped by elf_loader and answers `??`). The
same file places feckless_umode's long windows: the address after "from"
in `ending at a call to X from 0x... in the app` is a link address.
