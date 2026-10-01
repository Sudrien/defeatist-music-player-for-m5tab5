# Abbreviations and toggles

What the short words on the screen mean, and every setting the player
has, by where it lives. The panel is 720 px wide and most of these sit
in a button a fifth of that, so they are short on purpose.

Taken from the source (`browser.c`, `panel.c`, `sleeppage.c`, `ui.c`,
`settings.h`, `playlist.h`). If a label here and the screen disagree,
the screen is newer -- fix this file.

## The now-playing bar

| On screen | Means |
|---|---|
| `LIVE` | A radio stream. There is no position to seek to, so the seek bar becomes a minute of level history instead. |
| `REC` | Recording. The bar shows the input's level, on a decibel scale (-60 to 0 dBFS). |
| `MUTED` | Recording, but the input has been exact digital zero for 300 ms -- a muted headset, or a source sending nothing. |
| `RG` | Under the speaker: ReplayGain is adjusting this track. The yellow mark on the volume slider is where the volume effectively sits. |
| `UAC` | In place of the speaker icon: a USB Audio Class device has the output. |
| `FILE` | On the format card, for a file with no extension. Otherwise the card shows the extension (`MP3`, `FLAC`, ...). |

### The status line, under volume

Reports, not switches -- nothing here does anything when tapped. Green
is on, dark grey is off. Each has its own switch somewhere else.

| On screen | Green when | Switched from |
|---|---|---|
| `USB` | The USB-A port is powered | USB tab, USB power |
| `MPD` | The MPD server is running | NET tab, MPD server |
| `HTTPS` | The browser remote is running | NET tab, Remote control |
| `SLEEP xxM` | The sleep timer is set; minutes left, rounded up. Just `SLEEP` when it is not | Sleep page |

At the right end of the same line, the battery (there is no battery
icon):

| On screen | Means | Colour |
|---|---|---|
| `Battery 73%` | Running on the battery | Light grey; red at 20% and under |
| `Charging 73%` | Power is coming in and charging the battery | Green |
| `Charged` | The battery is full | Green |
| `No Battery` | No battery fitted; running from USB-C | Dark grey |
| `Battery` | The battery gauge gives no reading | Light grey |

## The chooser (folder button)

### Tabs

| On screen | Means |
|---|---|
| `microSD` | The microSD card. |
| `USB` | A USB drive in the USB-A port. |
| `RADIO` | Internet radio: `stations.m3u` and favourites. |

### Footer buttons

| On screen | Means |
|---|---|
| `UP` | Up one folder. |
| `RLOD` | On the RADIO tab's top level, in `UP`'s place: reload the station list from the card. |
| `FLDR` | Play this whole folder. |
| `UP^` | Scroll the list up a page. |
| `DN` | Scroll the list down a page. |
| `X` | Close the chooser. |
| *(order)* | The play order -- tap to cycle. See below. |

### Play order

Tapping the order button goes **ONE → ALL → RPT → EAT → RND → RPT1 →**
back to ONE.

| On screen | Means | MPD equivalent |
|---|---|---|
| `ONE` | Play this track, then stop. | `single 1` |
| `ALL` | On to the next track; stop at the end of the folder. | (all off) |
| `RPT` | `ALL`, starting again from the top at the end. | `repeat 1` |
| `EAT` | `ALL`, removing each track from the list as it is left. | `consume 1` |
| `RND` | Random order, no repeats until every track has played. | `random 1` |
| `RPT1` | This track again, until told otherwise. | `repeat 1` + `single 1` |
| `ONE1`, `EAT1` | `ONE` or `EAT` for one song only, because an MPD client asked for `single oneshot` or `consume oneshot`. Goes back to the bare name after it. | `oneshot` |

Each order is exactly one MPD combination. Going the other way, three
of MPD's eight random/repeat/single combinations have no exact order
here; `mpdmode.h` has the table and what each becomes.

## Settings (gear button)

Five tabs: `SD`, `USB`, `BUILD`, `AUDIO`, `NET`. NET is short for network.

### SD and USB

| Setting | Values | Notes |
|---|---|---|
| Reindex | `REINDEX` | Rebuild that volume's media index. Reads `INDEXING` while this volume's runs, `BUSY` while another's does -- one at a time. |
| USB power (USB tab) | `ON` / `OFF` | Power to the USB-A port. `IN USE` and greyed while a track is playing from the drive. The player also cuts it by itself after a track when nothing is attached. |

### BUILD

What is running. Nothing to set.

- `app`, `version`, `built`, `idf`, and free `heap`, `psram` and `uptime`.
- **Source**: the repository, github.com/Sudrien/m5tab5_defeatist_music_player.
- **Libraries**: every component and vendored library in this build, with
  its version, or its commit's first seven characters for a git one, and
  its licence after `--`. `licence not found` means the component shipped
  neither a licence file nor a licence field.
  Generated from `dependencies.lock` and the vendored pins when the build
  is configured, so it is what was compiled.

### AUDIO

| Setting | Values | Notes |
|---|---|---|
| ReplayGain | on / off | Level tracks to each other. Off stops the adjustment, not the measuring. |
| Crossfade | off, or seconds | Slider. |
| Same album | on / off | Crossfade between tracks of one album too. Off leaves album joins alone. One folder counts as one album. |
| Record from | see below | What the record button records. |

**Record from** -- tap to cycle. A choice is lit when its input is
there to record from.

| On screen | Records |
|---|---|
| `MONO` | The two built-in microphones, summed to mono. |
| `STEREO` | The two built-in microphones, left and right. |
| `FOCUSED` | The built-in pair as a beam aimed out of the screen. Mono. |
| `HEADSET` | The microphone in the headphone jack, mono. Lit when anything is plugged in -- the jack cannot tell a headset from headphones. |
| `UAC` | A USB microphone, at its own rate and channels. |
| `AUTO` | USB if a microphone is there, else the headset, else `MONO`. Always mono. |
| `OFF` | No recording: the transport switch loses its record position. |

All recordings are FLAC, in `Recordings/` on the SD card (the USB drive
if there is no card).

### NET

| Setting | Values | Notes |
|---|---|---|
| Wi-Fi | on / off | |
| Remote control | on / off | The browser remote, over HTTPS. |
| MPD server | on / off | For MPD clients and Home Assistant. |
| Network time | on / off | NTP. Greyed while Wi-Fi is off; the setting is kept. |
| Add a network | `START` / `STOP` | Opens a captive portal to join a Wi-Fi network from a phone. |
| Benchmark | `RUN` (`BUSY` while running) | How fast a station's stream can be pulled with nothing decoding it. |
| Clock | `RESET` | Forget the clock. Only available while the time has not been verified. |

## Sleep page (moon button)

| Setting | Values |
|---|---|
| Screen | Brightness, a percentage. |
| Rotation | `0`, `90`, `180`, `270`. |
| Dim screen | Never, or after 15 s to 2 min without a touch -- to half brightness. |
| Screen off | Never, or 30 s to 5 min. Never while this page is open. |
| Sleep timer | off, or 15 min to 2 h in 15-minute steps. The sound fades out over the last 20 seconds. |
| Power off | Never, or after 15 min, 30 min, 1 h or 2 h with nothing playing, recording or touched. The device turns itself off; the side button turns it on. Separate from the sleep timer, and never while music plays. |

## Build switches

Not on the screen -- `idf.py -D<NAME>=1 build`. Each stays in
`CMakeCache.txt` until set back to 0, so build a release from a clean
tree.

| Switch | Does |
|---|---|
| `HEAPCHECK` | Heap integrity checkpoints that name the last subsystem before a corruption. Slow. |
| `WAVEFORM` | The whole-file waveform scan. |
| `O3CHECK` | All of `main` at -O3, to catch warnings. A check, not a release setting. |
| `DECBENCH` | Logs minimp3's decode cost in CPU cycles per frame. |
