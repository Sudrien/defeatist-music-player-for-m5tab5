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
| the network's Wi-Fi control, feckless_fetch, NTP | a cable works -- MPD, web radio, the benchmark, podcast search, remote control (the box answers the routes, sockets, the HTTP client and server and the hash: u-defeatist-0042, 0044, 0045) -- but no Wi-Fi, podcast downloads or NTP |
| the hardware JPEG decoder | album art in software |
| esp_ota, esp_partition | the table says "hosted" (u-defeatist-0025): Exit to Launcher shows, and ends the app |
| raw I2C, deep sleep | no power-off pulse: power off ends the app, and the board stays on |
| HID, USB class registration | callbacks a box cannot make yet |
| the crypto section | approve.c's hash; it approves only network features |

**Power off, and the low-battery power off, do not cut the power in a
box.** The pulse to the power controller is raw I2C. The app saves its
settings, ends, and the launcher's menu comes back on a board still
running from its battery; the launcher puts the backlight back. Whatever
protects a low battery from there is the launcher's.

Each comes out of `absent.c` when a box answers it: the box's call then
replaces it, and the checks say which are left.

## On a board

**The .bin on its own.** Flash it as the player always was. It brings up
the console, NVS, the bus, USB, storage and the network, then the panel
on core 0, and runs the player in a box on core 1: the largest PSRAM
block less 2 MB, at most 24 MB, NVS in the namespace `defeatist`. Power
off and Exit to Launcher both end the player in a box; the .bin then goes
back to M5Launcher if one booted it from a slot, and otherwise powers the
board off itself (the PMS150's pulse, which the box cannot give). A fault
restarts it.

**Under Avoidant's umode branch.** The ELF on the card or a USB drive as
`defeatist_player.app.elf`, or a release's
`defeatist_<version>_music_player_for_m5tab5.elf`: Avoidant names the NVS
namespace from the file name up to the first `_`, so either gets
`defeatist`. Exit to Launcher, or power off, ends it and Avoidant's menu
comes back; the board stays on.

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
