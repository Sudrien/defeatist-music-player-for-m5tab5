# boxed -- the player in a feckless box

The `umode` branch's second project (u-defeatist-0022): the whole player,
main/'s sources unchanged, built into an ELF for a feckless box -- RISC-V
user mode, one block of PSRAM, every call out of it an `ecall` the launcher
checks. Where ../elf/ makes the trusted ELF a launcher runs as itself, this
makes the one a launcher runs boxed.

## Building

ESP-IDF 6.1, from this directory:

    idf.py set-target esp32p4
    idf.py elf                  # -> build/defeatist_boxed.app.elf

The build ends with two checks. `feckless_abi` holds every import to the
ABI's core list and the box and storage sections; `feckless_box_libc`
(boxcheck) holds every import to what a box answers. Both must pass; the
first build to pass them said

    -- feckless_abi: 195 symbols, all in feckless_abi_v1 + feckless_abi_audio_v1 + feckless_abi_box_v1 + feckless_abi_nvs_v1 + feckless_abi_storage_v2
    -- feckless_box_libc: boxed, 195 imports, none of them in-app names, all answered by the box

(188, without the audio section, with `-DDEFEATIST_BOXED_CODEC=OFF`.)

The manifest (`main/idf_component.yml`) follows feckless-libs' `umode`
branch, which must have u-feckless-0045 to 0049 and 0053, and 0052 for the codec. Its `dependencies.lock` is
written by the first configure and is this project's own; the player's
manifest and lock are not touched.

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

    idf.py -DDEFEATIST_BOXED_CODEC=OFF elf

which puts the codec in `main/absent_codec.c` as absent: those formats say
"not supported", and the rest play. CMake keeps the setting: a later plain
`idf.py elf` is still built without the codec until
`idf.py -DDEFEATIST_BOXED_CODEC=ON elf`.

What it does not answer is in `main/absent.c`, as the app's own functions
that fail the way the player already copes with:

| absent | so |
|---|---|
| the network: Wi-Fi, Ethernet, sockets, HTTP client and server | an offline player: no radio, podcasts, remote, MPD, NTP |
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

Needs a launcher on feckless-libs' umode branch with u-feckless-0045 to
0049 and 0053, naming `defeatist` as this app's NVS namespace, and 0052 and esp_audio_codec linked for the default build (Avoidant's
umode branch, rebuilt), and a box as large as it allows:
the ELF and its statics are 1.35 MB, and the player's three PCM rings
10.5 MB of the box's heap before anything else it allocates.

Avoidant's umode branch has all of that in its source (u-avoidant-0001 to
0004: boxes, core 1, faults placed in the ELF, the namespace from the file
name, so `defeatist_boxed.app.elf` gets `defeatist`), but its committed
`dependencies.lock` holds feckless-libs at f6991f3, from before
u-feckless-0045: built from that lock, the box answers none of what this
ELF needs past the core, and the load refuses it naming the first missing
import. Move the lock first, in Avoidant's m5tab5/:

    idf.py update-dependencies

and read its diff (every feckless component to a umode commit with
u-feckless-0053). Its box is the largest PSRAM block less 2 MB, at most
16 MB: 4 MB or so for the player's heap after the rings -- covers, the
library, the thread stacks.

The file goes on the card or a USB drive as `defeatist_boxed.app.elf`
(the name is the namespace: keep it), in any folder Avoidant's menu
browses; it runs the ELF boxed because it imports `feckless_abi_box_v1`. Exit to Launcher, or power off, ends it.

What to look for in the log: `=== Defeatist Music Player ===`, then the
player's own start-up; `no PCM ring` if the box was too small for the
rings; any `thread N ended: reason` line from feckless_umode, with the pc
(u-feckless-0043 turns it into an address in this ELF:
`riscv32-esp-elf-addr2line -e build/defeatist_boxed.app.elf`).
