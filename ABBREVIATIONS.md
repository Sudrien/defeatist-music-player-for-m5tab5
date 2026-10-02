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
is on, dark grey is off (WIFI also has yellow). Each has its own switch
somewhere else.

| On screen | Green when | Switched from |
|---|---|---|
| `USB` | The USB-A port is powered | USB tab, USB power |
| `WIFI` | Connected, with an address. **Yellow**: switched on but not connected yet -- starting, scanning, joining, or failing to | NET tab, Wi-Fi |
| `MPD` | The MPD server is running | NET tab, MPD server |
| `HTTPS` | The browser remote is running | NET tab, Remote control |
| `SLEEP xxM` | The sleep timer is set; minutes left, rounded up. Just `SLEEP` when it is not | Sleep page |
| `IDLE` | Power off is set and counting down: nothing is playing, streaming, recording or indexing. **Yellow**: set, but held -- something is going on, so the wait keeps restarting. Grey when Power off is Never. No minutes (6020) | Sleep page, Power off |

At the right end of the same line, the battery (there is no battery
icon):

| On screen | Means | Colour |
|---|---|---|
| `BATT 73%` | Running on the battery | Light grey; red at 20% and under |
| `CHRG 73%` | Power is coming in and charging the battery | Green |
| `CHARGED` | The battery is full | Green |
| `NO BATT` | No battery fitted; running from USB-C | Dark grey |
| `BATT` | The battery gauge gives no reading | Light grey |

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

What is running, and the screen's language.

| Setting | Values | Notes |
|---|---|---|
| Language | `English` / `简体中文` / `日本語` / `Español` | Tap to cycle. Each language is named in itself, and the row's label and the tab names are never translated, so the way back is findable from any of them. Kept in the device, not on the card. Only some screens are translated so far; the rest stay English. |

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

## In other languages (6019)

The labels above in Mandarin, Japanese and Spanish, chosen on the BUILD
tab. Each translates the full word the label stands for, re-shortened
where a button is narrower than the word: chooser buttons are 112 px
(8 Latin or 4 CJK characters), the settings pills 124 px (5 or 3). The
play-order words follow Cantata's MPD vocabulary where it has one --
重复/随机/单曲 and 播完删除 (from 播放后删除), リピート/ランダム,
Repetir -- and ON/OFF
follow the 开/关 the Audio tab has used since 6013.

Kept in English in every language, by `same()`: the tab names (SD, USB,
BUILD, AUDIO, NET, RADIO), USB, WIFI, MPD, HTTPS, UAC, RG, and the
units.

| Where | English | 简体中文 | 日本語 | Español |
|---|---|---|---|---|
| Status line | `SLEEP` | 睡眠 | 睡眠 | SUEÑO |
| Status line | `SLEEP %dM` | 睡眠%d分 | 睡眠%d分 | SUEÑO %dM |
| Status line | `IDLE` | 空闲 | 待機 | INAC |
| Status line | `BATT` | 电池 | 電池 | BAT |
| Status line | `BATT %d%%` | 电池 %d%% | 電池 %d%% | BAT %d%% |
| Status line | `CHRG %d%%` | 充电 %d%% | 充電 %d%% | CARG %d%% |
| Status line | `CHARGED` | 已充满 | 満充電 | CARGADA |
| Status line | `NO BATT` | 无电池 | 電池なし | SIN BAT |
| Now playing | `LIVE` | 直播 | ライブ | VIVO |
| Now playing | `REC` | 录音 | 録音 | GRAB |
| Now playing | `MUTED` | 静音 | 無音 | MUDO |
| Chooser footer | `UP` | 上级 | 上へ | SUBIR |
| Chooser footer | `RLOD` | 重新加载 | 再読込 | RECARGAR |
| Chooser footer | `FLDR` | 播放目录 | フォルダ | CARPETA |
| Chooser footer | `UP^` | 上页 | 前頁 | RE PÁG |
| Chooser footer | `DN` | 下页 | 次頁 | AV PÁG |
| Play order | `ONE` | 单曲 | 単曲 | ÚNICA |
| Play order | `ALL` | 顺序 | 全曲 | TODAS |
| Play order | `RPT` | 重复 | リピート | REPETIR |
| Play order | `EAT` | 播完删除 | 消費 | BORRAR |
| Play order | `RND` | 随机 | ランダム | AZAR |
| Play order | `RPT1` | 单曲重复 | 1曲反復 | REPITE 1 |
| Play order | `ONE1` | 单曲1 | 単曲1 | ÚNICA1 |
| Play order | `EAT1` | 删除1 | 消費1 | BORRAR1 |
| Settings | `ON` | 开 | オン | SÍ |
| Settings | `OFF` | 关 | オフ | NO |
| Settings | `IN USE` | 使用中 | 使用中 | EN USO |
| Settings | `REINDEX` | 重新索引 | 再索引 | REINDEXAR |
| Settings | `INDEXING` | 索引中 | 索引中 | INDEXANDO |
| Settings | `BUSY` | 进行中 | 作業中 | OCUP. |
| Settings | `START` | 开始 | 開始 | ABRIR |
| Settings | `STOP` | 停止 | 停止 | PARAR |
| Settings | `RUN` | 运行 | 実行 | MEDIR |
| Settings | `RESET` | 重置 | 戻す | VOLVER |
| Settings | `CLOSE` | 关闭 | 閉じる | CERRAR |
| Record from | `MONO` | 单声道 | モノラル | MONO |
| Record from | `STEREO` | 立体声 | ステレオ | ESTÉREO |
| Record from | `FOCUSED` | 定向 | フォーカス | ENFOCADO |
| Record from | `HEADSET` | 耳麦 | ヘッドセット | AURICULAR |
| Record from | `AUTO` | 自动 | 自動 | AUTO |

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
