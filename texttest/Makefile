# texttest -- host builds of the player's modules.
#
# 6055: the gfx text-path test this directory was named for moved to
# feckless-graphics-handler-for-m5tab5 with gfx.c; i18ntest below still
# builds against that gfx.c, through FECKLESS_GRAPHICS.
#
# Same bargain as seektest: the sanitizers are the point, so they are not
# optional flags a hurried run can drop. -fsanitize=address,undefined with
# -fno-sanitize-recover means the first overrun or signed overflow aborts
# with a stack trace rather than being counted and continued past.
#
#   make        build and run
#   make clean
#
# This does NOT replace a flash. ARCHITECTURE.md is emphatic about that and it
# has been right every time: host testing cannot see the DSI panel, PSRAM
# timing, or the ESP-IDF build. It catches layout arithmetic, which is
# what changed here, and nothing else.
#
# SPDX-License-Identifier: MIT

CC      ?= cc

# A second, non-sanitised pass at -O2 -Werror, run before the real build.
#
# ASan needs -O1 or the reports lose their frames, but several warnings --
# -Wformat-truncation is the one that caught us -- only fire after the
# constant propagation that -O2 does. The firmware is built at -O2 with
# warnings as errors, so a file could pass every check here and fail the
# real build on a diagnostic this harness structurally could not produce.
# That happened, twice, on code that had been "checked".
#
# Warnings only: no binary is kept and nothing is run from it.
# 6053: storage.h, storage_io.h, wifi.h and ethernet.h moved into the two
# feckless libraries, and files here still include them. The component
# manager puts a git dependency in managed_components/ under its bare
# name, so these are right after the first idf.py build or reconfigure;
# before one, point them at clones:
#   make FECKLESS_NET=../../feckless-network-handler-for-m5tab5 \
#        FECKLESS_STORAGE=../../feckless-storage-handler-for-m5tab5
# Their own tests (wifistoretest, netlinktest) moved with them.
# 6054: the same for feckless-drivers (FECKLESS_DRIVERS); micpcmtest
# moved with micpcm.h. 6055: and feckless-graphics-handler
# (FECKLESS_GRAPHICS), which took the gfx text test and bidilinetest,
# logcuttest and brightnesstest with it.
FECKLESS_NET     ?= ../managed_components/feckless_network_handler
FECKLESS_STORAGE ?= ../managed_components/feckless_storage_handler
FECKLESS_DRIVERS ?= ../managed_components/feckless_drivers
FECKLESS_GRAPHICS ?= ../managed_components/feckless_graphics_handler
FECKLESS_INC     := -I$(FECKLESS_NET)/include -I$(FECKLESS_STORAGE)/include \
                    -I$(FECKLESS_DRIVERS)/include \
                    -I$(FECKLESS_GRAPHICS)/include \
                    -I$(FECKLESS_GRAPHICS)/fonts/ark12 -I$(FECKLESS_GRAPHICS)/fonts/arabixel

WARNCFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
              -D_GNU_SOURCE -I. -Ifake -I../main $(FECKLESS_INC)

CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -fno-omit-frame-pointer \
           -I. -Ifake -I../main $(FECKLESS_INC)
LDFLAGS := -fsanitize=address,undefined -lm

# 6055: gfx.c and the two font tables, for i18ntest.
GFX_SRCS := $(FECKLESS_GRAPHICS)/src/gfx.c $(FECKLESS_GRAPHICS)/fonts/ark12/ark12.c \
            $(FECKLESS_GRAPHICS)/fonts/arabixel/arabixel_tab.c \
            $(FECKLESS_GRAPHICS)/fonts/arabixel/arabixel_shape.c

# The USB control-transfer ownership handoff, which needs threads and its
# own main(). Separate binary rather than another file in BIN.
CTRL_BIN := ctrltest

# The screen's translations (6013): i18n.c against fixtures, then against
# the committed table, after tools/i18n.py check says that table is what
# the source and the YAML make.
I18N_BIN := i18ntest

# The clock floor. Its own binary for the same reason the others are.
CLOCK_BIN := clocktest

# The four screen angles, and the control square that rides on them.
# gfx.c's logical->panel map and touch.c's inverse have to agree exactly
# or every control is at its own mirror image, which on the board looks
# like a miscalibrated digitiser rather than like arithmetic. Neither
# file builds on a host -- one needs the DPI driver, the other
# esp_lcd_touch -- so both maps are transcribed there, and the layout
# arithmetic they feed is checked with them.
ROTATE_BIN := rotatetest
# The clock floor read off a card's root, before NTP can run. Four
# comparisons standing between foreign timestamps and a permanent
# one-way latch: too loose and a file dated 2107 refuses every real NTP
# reply for the life of the device, too tight and it silently never
# helps. Header-only, like tailplantest, so this compiles cardtime.h
# itself and there is nothing to drift from.
CARDTIME_BIN := cardtimetest

# The settings pages' one scrollbar. The only way to reach the sleep
# timer in landscape, where 860 px of page sit in a 504 px viewport, so
# an off-by-one at either end of its travel reads as a control that
# cannot be set rather than as a scrollbar that is wrong. Header-only,
# like cardtimetest.
MENUSCROLL_BIN := menuscrolltest

# The media index's path order and reconcile step. An order that
# disagrees with the walk tombstones and re-adds folders on every walk
# without ever crashing, so it is checked here or nowhere. Header-only,
# like cardtimetest.
MEDIAINDEX_BIN := mediaindextest
MEDIALIST_BIN := medialisttest
MEDIASEARCH_BIN := mediasearchtest
MPDQUEUE_BIN := mpdqueuetest

# The MPD protocol grammar: tokeniser, command table, OK/ACK, command
# lists, and the status/song serialisers. It links remoteproto.c for
# remoteproto_utf8_len(), which 5155 exported rather than copy: MPD.md
# predicted both files would be linked here, for path_ok() rather than
# for the UTF-8 repair, and the prediction came true the other way round.
MPDPROTO_BIN := mpdprototest

# An MPD URI is an index path, and which volume holds it. Links
# remoteproto.c: this is where MPD.md's reuse of remoteproto_path_ok()
# actually fits, on the path AFTER the mapping rather than before it.
MPDURI_BIN := mpduritest
MPDFILTER_BIN := mpdfiltertest

# play_order_t against MPD's four flags. Needs playlist.h, whose esp_err.h
# the fake/ shim supplies, so the table is host-tested like the rest.
MPDMODE_BIN := mpdmodetest
MPDIDLE_BIN := mpdidletest
# MPD.md step 5: the one way into ui_task, which the remote and the MPD
# server both feed.
UIREQ_BIN := uireqtest
M3ULINE_BIN := m3ulinetest
URLCLEAN_BIN := urlcleantest
PLAYLIST_BIN := playlisttest
SCREENDIM_BIN := screendimtest
ARTPLACE_BIN := artplacetest

# The media walk, over a real directory tree in /tmp: the order the
# index needs, and completeness-or-failure. Compiles the real
# mediawalk.c; the ESP-IDF surface it calls is stubbed in the test.
MEDIAWALK_BIN := mediawalktest

# One reconcile end to end: the real mediasync.c reading and writing
# real index files in /tmp, with the catalog, tags and walk supplied by
# the test. What a failure must leave untouched is the point.
MEDIASYNC_BIN := mediasynctest

# Fade and end-of-audio detection. Links the real loudness.c, and builds
# it once more at -O2 -Werror as an object -- not -fsyntax-only, which
# stops before the optimiser that the -O2-only warnings come out of.
FADE_BIN := fadetest

# The track-boundary rules. Header-only, so this compiles tailplan.h
# itself and there is nothing to drift from.
# The "same station?" rule behind the gold star. Header-only, like
# tailplantest, so this compiles favmatch.h itself and there is nothing
# to drift from.
FAVMATCH_BIN := favmatchtest

# The recorder's FLAC encoder (5104), compiled as is and round-tripped
# through a decoder written in the test from the format.
FLACENC_BIN := flacenctest

# The two-microphone beam (5109): sources simulated at angles to a 34 mm
# pair, through main/beam.c as is.
BEAM_BIN := beamtest

TAIL_BIN := tailplantest

# The setup portal's form parsing, HTML escaping and DNS replies: the
# code that reads bytes from anyone in radio range.
PORTAL_BIN := portalwebtest

# 5117: the browser remote's commands and state JSON -- commands from
# anyone on the network, strings from any tag on a card.
REMOTE_BIN := remoteprototest

# Fourteen older checks, written alongside the patches they cover and
# never added here, so nothing ran them after the session that wrote
# them. Each has its own main() and reports "all passed" or "FAILURES",
# returning non-zero on the second.
#
# Only cachetest compiles a file the firmware builds (mediacache.c, with
# shim.c for heap_caps_*). The rest TRANSCRIBE a rule out of player.c --
# the release gate, the ring order, the crossfade exits -- or, for
# pooltest, arithmetic out of esp_jpeg's tjpgd.c. They can drift from the
# code they describe; running them at least keeps them from drifting from
# themselves unnoticed.
OLD_STANDALONE := committest exittest kepttest orderingtest pooltest \
                  postest releasetest ringwaittest setstest stalltest \
                  tailtest trimtest xfadetest
CACHE_BIN := cachetest
OLD_BINS  := $(CACHE_BIN) $(OLD_STANDALONE)

# Screens that cannot run on a host but can be compiled against the real
# gfx.h and the fake headers here, so a -Werror at -O2 is caught before
# idf.py -- the error 0018 shipped was exactly that kind.
UI_CHECK := ../main/sleeppage.c

# The sleep timer's arithmetic.
SLEEP_BIN := sleeptimertest

# The stream probe's byte sniffing and ICY title parsing.
SNIFF_BIN := streamsnifftest

# The ICY demultiplexer. Header-only, so this compiles icydemux.h itself
# and there is nothing to drift from. The property it exists for --
# feeding a body in pieces gives the same answer as feeding it whole --
# cannot be observed on hardware, where metaint is 16000 and the reads
# are 2048, without waiting for the one read in eight that splits a
# metadata block.
ICY_BIN := icydemuxtest

# The connection policy: which statuses are followed, retried and given
# up on, and the backoff schedule. netstream.c cannot be host-tested --
# it is esp_http_client and a task -- but its decisions are a table, and
# the table is the part that will be wrong.
NETPLAN_BIN := netplantest

# stations.m3u. Every other file in the network series reads bytes from
# a server; this one reads bytes from a person with a text editor, which
# is a wider input and a worse one.
STATION_BIN := stationlisttest

# 24-bit PCM down to the 16 everything downstream is built from. The
# arithmetic ran untested from 0803 until 0003 moved it to a header: it
# was a static in decoder.c, and decoder.c does not build on a host.
PCMFOLD_BIN := pcmfoldtest

# When a stream is allowed to make a sound. Two thresholds and not one,
# because a single one flaps at the watermark and that is heard as
# chopping rather than as a pause.
BUFPLAN_BIN := bufferplantest

# 5062: stb_image as the last JPEG decoder -- progressive covers and the
# flavours TJpgDec calls JDR_FMT3. stb_image.h is gitignored; fetched
# here, pinned and hash-checked, the same as cmake/vendored.cmake.
STBJPEG_BIN := stbjpegtest

# 5068: a station URL with an address in place of its host, and the Host
# header that puts the name back.
ADDRPIN_BIN := addrpintest
STB_H   := ../components/stbjpeg/stb_image.h
STB_URL := https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
STB_SHA := 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3

# Which decoder a stream's bytes go to. The bytes win over Content-Type,
# because handing AAC to minimp3 does not fail -- it produces noise, and
# noise from a live stream is hard to tell from a bad connection.
CODEC_BIN := codecplantest

# HLS playlists (6036): which variant, which segment, the live edge.
HLSPLAY_BIN := hlsplaytest

# HLS segments (6038): ID3 tags off every segment, TS and fMP4 named.
HLSSEG_BIN := hlssegtest

# MPEG-TS (6039): the audio PID's PES payload, and nothing else.
TSDEMUX_BIN := tsdemuxtest

# Folder cover art (6042): folderart.c against real folders, with fakes
# for the storage arbiter and the cover cache.
FOLDERART_BIN := folderarttest

# MPEG frame lengths. An MP3 header does not carry its frame length, so a
# wrong table entry desynchronises the stream and shows up as bytes lost
# -- the figure the probe uses to certify icydemux and the ring. The test
# computes the lengths a second time, longhand.
MP3_BIN := mp3counttest

# The window between the ring and the decoder. Losing bytes at a refill
# boundary does not crash -- the decoder resynchronises and the stream
# plays with a click every few seconds, which from outside is "internet
# radio is a bit crackly" with no file to compare against.
FRAMEWIN_BIN := framewintest

# What play_stream() shows and what a press does. The screen has to pick
# between two state machines that disagree on purpose -- a reconnect
# behind a full PCM ring is not a message, an empty ring on a healthy
# link is -- and that choice is a table rather than an if chain in a
# loop nobody can step through.
STREAMPLAN_BIN := streamplantest

# Moving through the station list. The parser has its own test; this is
# the index arithmetic, which is where (i - 1) % n bites.
STATIONNAV_BIN := stationnavtest

# The radio-browser request, and the server's own M3U through the parser
# that was chosen for it. Links stationlist.h as well as radiobrowser.h on
# purpose: the encoder and the parser are checked against each other.
RADIOBROWSER_BIN := radiobrowsertest

# The minute-wide level strip: bucket arithmetic over a ring, plus the
# reserve-to-columns conversion the drawing will use.
LEVELHIST_BIN := levelhisttest

# Cue sheets. Header-only; the test reads the five sheets out of
# ../test_audio_files so the bytes under test are the bytes on the card.
CUE_BIN := cuesheettest

all: run-remote run-ctrl run-clock run-rotate run-cardtime run-mediaindex run-medialist run-mediasearch run-mpdqueue run-mpdproto run-mpduri run-mpdfilter run-mpdmode run-mpdidle run-uireq run-m3uline run-urlclean run-playlist run-screendim run-artplace run-mediawalk run-mediasync run-menuscroll run-fade run-tail run-portal run-old check-ui check-netbudget run-sleep run-sniff run-icy run-netplan run-stations run-pcmfold run-bufplan run-stbjpeg run-addrpin run-codec run-hlsplay run-hlsseg run-tsdemux run-folderart run-mp3 run-framewin run-streamplan run-stationnav run-radiobrowser run-levelhist run-favmatch run-cue run-tagextra run-flacenc run-beam run-i18n

$(CTRL_BIN): ctrltest.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -o $@ ctrltest.c $(LDFLAGS) -pthread

run-ctrl: $(CTRL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CTRL_BIN)

$(I18N_BIN): i18ntest.c ../main/i18n.c ../main/i18n.h ../main/i18n_tab.c $(GFX_SRCS)
	$(CC) $(WARNCFLAGS) -c ../main/i18n.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/i18n_tab.c -o /dev/null
	$(CC) $(CFLAGS) -DI18NTEST_FIXTURE -o $@-fixture i18ntest.c ../main/i18n.c $(LDFLAGS)
	$(CC) $(CFLAGS) -o $@ i18ntest.c ../main/i18n.c ../main/i18n_tab.c $(GFX_SRCS) shim.c $(LDFLAGS)

run-i18n: $(I18N_BIN)
	python3 ../tools/i18n.py check
	./$(I18N_BIN)-fixture
	./$(I18N_BIN)

$(CLOCK_BIN): clocktest.c
	$(CC) $(WARNCFLAGS) -c clocktest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ clocktest.c $(LDFLAGS)

run-clock: $(CLOCK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CLOCK_BIN)

$(ROTATE_BIN): rotatetest.c
	$(CC) $(WARNCFLAGS) -c rotatetest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ rotatetest.c $(LDFLAGS)

run-rotate: $(ROTATE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ROTATE_BIN)
$(CARDTIME_BIN): cardtimetest.c ../main/cardtime.h
	$(CC) $(WARNCFLAGS) -c cardtimetest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ cardtimetest.c $(LDFLAGS)

run-cardtime: $(CARDTIME_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CARDTIME_BIN)

$(MEDIAINDEX_BIN): mediaindextest.c ../main/mediaindex.h
	$(CC) $(WARNCFLAGS) -c mediaindextest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediaindextest.c $(LDFLAGS)

run-mediaindex: $(MEDIAINDEX_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIAINDEX_BIN)

$(MEDIALIST_BIN): medialisttest.c ../main/medialist.h ../main/mediaindex.h
	$(CC) $(WARNCFLAGS) -c medialisttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ medialisttest.c $(LDFLAGS)

run-medialist: $(MEDIALIST_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIALIST_BIN)

$(MEDIASEARCH_BIN): mediasearchtest.c ../main/mediasearch.h ../main/mediacat.h ../main/casefold.h ../main/casefold_tab.c
	$(CC) $(WARNCFLAGS) -c mediasearchtest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediasearchtest.c ../main/casefold_tab.c $(LDFLAGS)

run-mediasearch: $(MEDIASEARCH_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIASEARCH_BIN)

$(MPDQUEUE_BIN): mpdqueuetest.c ../main/mpdqueue.c ../main/mpdqueue.h
	$(CC) $(WARNCFLAGS) -c ../main/mpdqueue.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpdqueuetest.c ../main/mpdqueue.c $(LDFLAGS)

run-mpdqueue: $(MPDQUEUE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDQUEUE_BIN)

$(MPDPROTO_BIN): mpdprototest.c ../main/mpdproto.c ../main/mpdproto.h ../main/remoteproto.c
	$(CC) $(WARNCFLAGS) -c ../main/mpdproto.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpdprototest.c ../main/mpdproto.c ../main/remoteproto.c $(LDFLAGS)

run-mpdproto: $(MPDPROTO_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDPROTO_BIN)

$(MPDURI_BIN): mpduritest.c ../main/mpduri.c ../main/mpduri.h ../main/mediaindex.h ../main/remoteproto.c
	$(CC) $(WARNCFLAGS) -c ../main/mpduri.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpduritest.c ../main/mpduri.c ../main/remoteproto.c $(LDFLAGS)

run-mpduri: $(MPDURI_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDURI_BIN)

# 5238: MPD 0.21's filter expressions.
$(MPDFILTER_BIN): mpdfiltertest.c ../main/mpdfilter.c ../main/mpdfilter.h ../main/casefold.h ../main/casefold_tab.c
	$(CC) $(WARNCFLAGS) -c ../main/mpdfilter.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpdfiltertest.c ../main/mpdfilter.c ../main/casefold_tab.c $(LDFLAGS)

run-mpdfilter: $(MPDFILTER_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDFILTER_BIN)

$(MPDMODE_BIN): mpdmodetest.c ../main/mpdmode.c ../main/mpdmode.h ../main/playlist.h
	$(CC) $(WARNCFLAGS) -c ../main/mpdmode.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpdmodetest.c ../main/mpdmode.c $(LDFLAGS)

run-mpdmode: $(MPDMODE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDMODE_BIN)

$(MPDIDLE_BIN): mpdidletest.c ../main/mpdidle.c ../main/mpdidle.h ../main/mpdproto.h
	$(CC) $(WARNCFLAGS) -c ../main/mpdidle.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ mpdidletest.c ../main/mpdidle.c $(LDFLAGS)

run-mpdidle: $(MPDIDLE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MPDIDLE_BIN)

$(M3ULINE_BIN): m3ulinetest.c ../main/m3uline.c ../main/m3uline.h
	$(CC) $(WARNCFLAGS) -c ../main/m3uline.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ m3ulinetest.c ../main/m3uline.c $(LDFLAGS)

run-m3uline: $(M3ULINE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(M3ULINE_BIN)

$(URLCLEAN_BIN): urlcleantest.c ../main/urlclean.c ../main/urlclean.h
	$(CC) $(WARNCFLAGS) -c ../main/urlclean.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ urlcleantest.c ../main/urlclean.c $(LDFLAGS)

run-urlclean: $(URLCLEAN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(URLCLEAN_BIN)

$(UIREQ_BIN): uireqtest.c ../main/uireq.c ../main/uireq.h shim.h shim.c
	$(CC) $(WARNCFLAGS) -c ../main/uireq.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ uireqtest.c ../main/uireq.c shim.c $(LDFLAGS)

run-uireq: $(UIREQ_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(UIREQ_BIN)

# playlist.c against a real temporary directory, with the decoder's
# extension check, the storage helpers, cuedir and esp_random() stood in
# for in the test. Written before MPD.md step 4b so the switch-over has
# something to pass unchanged.
# 5165: and against mpdqueue.c, which holds the list now. The test's
# source is 5164's, unchanged -- that is the point of it.
$(PLAYLIST_BIN): playlisttest.c ../main/playlist.c ../main/playlist.h ../main/mpdqueue.c ../main/mpdqueue.h shim.c shim.h fake/esp_random.h
	$(CC) $(WARNCFLAGS) -D_GNU_SOURCE -c ../main/playlist.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ playlisttest.c ../main/playlist.c ../main/mpdqueue.c shim.c $(LDFLAGS)

run-playlist: $(PLAYLIST_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(PLAYLIST_BIN)

$(SCREENDIM_BIN): screendimtest.c ../main/screendim.h
	$(CC) $(WARNCFLAGS) -c screendimtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ screendimtest.c $(LDFLAGS)

run-screendim: $(SCREENDIM_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SCREENDIM_BIN)

$(ARTPLACE_BIN): artplacetest.c ../main/artplace.h
	$(CC) $(WARNCFLAGS) -c artplacetest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ artplacetest.c $(LDFLAGS)

run-artplace: $(ARTPLACE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ARTPLACE_BIN)

$(MEDIAWALK_BIN): mediawalktest.c ../main/mediawalk.c ../main/mediawalk.h ../main/mediadir.h ../main/mediaindex.h shim.c shim.h
	$(CC) $(WARNCFLAGS) -c mediawalktest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediawalk.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediawalktest.c ../main/mediawalk.c shim.c $(LDFLAGS)

run-mediawalk: $(MEDIAWALK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIAWALK_BIN)

$(MEDIASYNC_BIN): mediasynctest.c ../main/mediasync.c ../main/mediasync.h ../main/mediaindex.h ../main/mediacat.h
	$(CC) $(WARNCFLAGS) -c mediasynctest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediasync.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediasynctest.c ../main/mediasync.c $(LDFLAGS)

run-mediasync: $(MEDIASYNC_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIASYNC_BIN)

$(MENUSCROLL_BIN): menuscrolltest.c ../main/menuscroll.h
	$(CC) $(WARNCFLAGS) -c menuscrolltest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ menuscrolltest.c $(LDFLAGS)

run-menuscroll: $(MENUSCROLL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MENUSCROLL_BIN)

$(FADE_BIN): fadetest.c ../main/loudness.c ../main/loudness.h
	$(CC) $(WARNCFLAGS) -D_GNU_SOURCE -I../main -c ../main/loudness.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ fadetest.c ../main/loudness.c $(LDFLAGS)

run-fade: $(FADE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FADE_BIN)

# The stream levelling carrier, its gate and its slew. Links the real
# main/streamgain.c, and builds it once more at -O2 -Werror for the
# reason FADE_BIN does.
# The one-field JSON reader. Header-only, so the test is the only
# compile it gets -- hence the -Werror pass as well.
JSONPICK_BIN := jsonpicktest

$(JSONPICK_BIN): jsonpicktest.c ../main/jsonpick.h
	$(CC) $(WARNCFLAGS) -c jsonpicktest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ jsonpicktest.c $(LDFLAGS)

run-jsonpick: $(JSONPICK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(JSONPICK_BIN)

SGAIN_BIN := streamgaintest

$(SGAIN_BIN): streamgaintest.c ../main/streamgain.c ../main/streamgain.h
	$(CC) $(WARNCFLAGS) -c ../main/streamgain.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c streamgaintest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ streamgaintest.c ../main/streamgain.c $(LDFLAGS)

run-sgain: $(SGAIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SGAIN_BIN)

$(FAVMATCH_BIN): favmatchtest.c ../main/favmatch.h
	$(CC) $(WARNCFLAGS) -c favmatchtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ favmatchtest.c $(LDFLAGS)

run-favmatch: $(FAVMATCH_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FAVMATCH_BIN)

$(FLACENC_BIN): flacenctest.c ../main/flacenc.c ../main/flacenc.h
	$(CC) $(WARNCFLAGS) -c flacenctest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/flacenc.c -o /dev/null
	$(CC) $(WARNCFLAGS) -std=gnu17 -include stdio.h -include stdlib.h -c ../main/flacenc.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ flacenctest.c ../main/flacenc.c $(LDFLAGS)

run-flacenc: $(FLACENC_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FLACENC_BIN)

$(BEAM_BIN): beamtest.c ../main/beam.c ../main/beam.h
	$(CC) $(WARNCFLAGS) -c beamtest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -std=gnu17 -c ../main/beam.c -o /dev/null
	$(CC) $(CFLAGS) -O2 -o $@ beamtest.c ../main/beam.c $(LDFLAGS)

run-beam: $(BEAM_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BEAM_BIN)

# 5262: the ID3v1 genre table and the genre-number fix.
TAGEXTRA_BIN := tagextratest
$(TAGEXTRA_BIN): tagextratest.c ../main/tagextra.c ../main/tagextra.h
	$(CC) $(WARNCFLAGS) -c tagextratest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/tagextra.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ tagextratest.c ../main/tagextra.c $(LDFLAGS)

run-tagextra: $(TAGEXTRA_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(TAGEXTRA_BIN)

$(CUE_BIN): cuesheettest.c ../main/cuesheet.h
	$(CC) $(WARNCFLAGS) -c cuesheettest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ cuesheettest.c $(LDFLAGS)

run-cue: $(CUE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CUE_BIN)

$(TAIL_BIN): tailplantest.c ../main/tailplan.h
	$(CC) $(WARNCFLAGS) -c tailplantest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ tailplantest.c $(LDFLAGS)

run-tail: $(TAIL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(TAIL_BIN)

$(PORTAL_BIN): portalwebtest.c ../main/portalweb.c ../main/portalweb.h ../main/dnsreply.c ../main/dnsreply.h
	$(CC) $(WARNCFLAGS) -c ../main/portalweb.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/dnsreply.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ portalwebtest.c ../main/portalweb.c ../main/dnsreply.c $(LDFLAGS)

run-portal: $(PORTAL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(PORTAL_BIN)

$(REMOTE_BIN): remoteprototest.c ../main/remoteproto.c ../main/remoteproto.h
	$(CC) $(WARNCFLAGS) -c ../main/remoteproto.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ remoteprototest.c ../main/remoteproto.c $(LDFLAGS)

run-remote: $(REMOTE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(REMOTE_BIN)

$(OLD_STANDALONE): %: %.c
	$(CC) $(WARNCFLAGS) -c $< -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -o $@ $< $(LDFLAGS) -pthread

$(CACHE_BIN): cachetest.c ../main/mediacache.c ../main/mediacache.h shim.c shim.h
	$(CC) $(WARNCFLAGS) -c cachetest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediacache.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ cachetest.c ../main/mediacache.c shim.c $(LDFLAGS)

# One target for the fourteen, stopping at the first that fails.
run-old: $(OLD_BINS)
	@for t in $(OLD_BINS); do \
		echo "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$$t"; \
		ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$$t || exit 1; \
	done

clean:
	rm -f netbudget_probe.c $(CTRL_BIN) $(I18N_BIN) $(I18N_BIN)-fixture $(CLOCK_BIN) $(FADE_BIN) $(TAIL_BIN) $(PORTAL_BIN) $(REMOTE_BIN) $(OLD_BINS) $(SLEEP_BIN) $(SNIFF_BIN) $(ICY_BIN) $(NETPLAN_BIN) $(STATION_BIN) $(BUFPLAN_BIN) $(CODEC_BIN) $(HLSPLAY_BIN) $(HLSSEG_BIN) $(TSDEMUX_BIN) $(FOLDERART_BIN) $(MP3_BIN) $(FRAMEWIN_BIN) $(STREAMPLAN_BIN) $(STATIONNAV_BIN) $(RADIOBROWSER_BIN) $(LEVELHIST_BIN) $(SGAIN_BIN) $(JSONPICK_BIN) $(FAVMATCH_BIN) $(FLACENC_BIN) $(BEAM_BIN) $(CUE_BIN) $(TAGEXTRA_BIN) $(ROTATE_BIN) $(CARDTIME_BIN) $(MEDIAINDEX_BIN) $(MEDIALIST_BIN) $(MEDIASEARCH_BIN) $(MPDQUEUE_BIN) $(MPDPROTO_BIN) $(MPDURI_BIN) $(MPDFILTER_BIN) $(MPDMODE_BIN) $(MPDIDLE_BIN) $(UIREQ_BIN) $(M3ULINE_BIN) $(URLCLEAN_BIN) $(PLAYLIST_BIN) $(SCREENDIM_BIN) $(ARTPLACE_BIN) $(MEDIAWALK_BIN) $(MEDIASYNC_BIN) $(MENUSCROLL_BIN) $(STBJPEG_BIN) $(ADDRPIN_BIN)
	rm -rf $(CTRL_BIN).dSYM $(CLOCK_BIN).dSYM $(FADE_BIN).dSYM $(TAIL_BIN).dSYM $(PORTAL_BIN).dSYM
	rm -rf $(addsuffix .dSYM,$(OLD_BINS))

$(SLEEP_BIN): sleeptimertest.c ../main/sleeptimer.h
	$(CC) $(WARNCFLAGS) -c sleeptimertest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ sleeptimertest.c $(LDFLAGS)

run-sleep: $(SLEEP_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SLEEP_BIN)

$(SNIFF_BIN): streamsnifftest.c ../main/streamsniff.h
	$(CC) $(WARNCFLAGS) -c streamsnifftest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ streamsnifftest.c $(LDFLAGS)

run-sniff: $(SNIFF_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SNIFF_BIN)

$(ICY_BIN): icydemuxtest.c ../main/icydemux.h ../main/streamsniff.h
	$(CC) $(WARNCFLAGS) -c icydemuxtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ icydemuxtest.c $(LDFLAGS)

run-icy: $(ICY_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ICY_BIN)

$(NETPLAN_BIN): netplantest.c ../main/netplan.h
	$(CC) $(WARNCFLAGS) -c netplantest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ netplantest.c $(LDFLAGS)

run-netplan: $(NETPLAN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(NETPLAN_BIN)

$(STATION_BIN): stationlisttest.c ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c stationlisttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ stationlisttest.c $(LDFLAGS)

$(PCMFOLD_BIN): pcmfoldtest.c ../main/pcmfold.h
	$(CC) $(WARNCFLAGS) -c pcmfoldtest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ pcmfoldtest.c $(LDFLAGS)

run-pcmfold: $(PCMFOLD_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(PCMFOLD_BIN)

run-stations: $(STATION_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STATION_BIN)

$(BUFPLAN_BIN): bufferplantest.c ../main/bufferplan.h
	$(CC) $(WARNCFLAGS) -c bufferplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ bufferplantest.c $(LDFLAGS)

run-bufplan: $(BUFPLAN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BUFPLAN_BIN)

$(STB_H):
	curl -fsSL "$(STB_URL)" -o $@.tmp
	echo "$(STB_SHA)  $@.tmp" | sha256sum -c - >/dev/null || (rm -f $@.tmp; echo "stb_image.h: hash mismatch"; exit 1)
	mv $@.tmp $@

$(STBJPEG_BIN): stbjpegtest.c ../components/stbjpeg/stbjpeg.c ../components/stbjpeg/stbjpeg.h $(STB_H)
	$(CC) $(WARNCFLAGS) -I../components/stbjpeg -c stbjpegtest.c -o /dev/null
	$(CC) $(CFLAGS) -I../components/stbjpeg -Wno-sign-compare -Wno-unused-function \
	    -Wno-unused-but-set-variable -Wno-type-limits -Wno-shift-negative-value \
	    -Wno-implicit-fallthrough -fwrapv -fno-sanitize=signed-integer-overflow -o $@ stbjpegtest.c ../components/stbjpeg/stbjpeg.c $(LDFLAGS)

run-stbjpeg: $(STBJPEG_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STBJPEG_BIN)

$(ADDRPIN_BIN): addrpintest.c ../main/addrpin.h
	$(CC) $(WARNCFLAGS) -c addrpintest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ addrpintest.c $(LDFLAGS)

run-addrpin: $(ADDRPIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ADDRPIN_BIN)

$(LEVELHIST_BIN): levelhisttest.c ../main/levelhist.h
	$(CC) $(WARNCFLAGS) -c levelhisttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ levelhisttest.c $(LDFLAGS)

run-levelhist: $(LEVELHIST_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(LEVELHIST_BIN)

$(RADIOBROWSER_BIN): radiobrowsertest.c ../main/radiobrowser.h ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c radiobrowsertest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ radiobrowsertest.c $(LDFLAGS)

run-radiobrowser: $(RADIOBROWSER_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(RADIOBROWSER_BIN)

$(STATIONNAV_BIN): stationnavtest.c ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c stationnavtest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ stationnavtest.c $(LDFLAGS)

run-stationnav: $(STATIONNAV_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STATIONNAV_BIN)

$(STREAMPLAN_BIN): streamplantest.c ../main/streamplan.h ../main/bufferplan.h ../main/netplan.h
	$(CC) $(WARNCFLAGS) -c streamplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ streamplantest.c $(LDFLAGS)

run-streamplan: $(STREAMPLAN_BIN) $(STATIONNAV_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STREAMPLAN_BIN) $(STATIONNAV_BIN)

$(CODEC_BIN): codecplantest.c ../main/codecplan.h ../main/streamsniff.h ../main/hlsplay.h
	$(CC) $(WARNCFLAGS) -c codecplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ codecplantest.c $(LDFLAGS)

run-codec: $(CODEC_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CODEC_BIN)

$(HLSPLAY_BIN): hlsplaytest.c ../main/hlsplay.h
	$(CC) $(WARNCFLAGS) -c hlsplaytest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ hlsplaytest.c $(LDFLAGS)

run-hlsplay: $(HLSPLAY_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(HLSPLAY_BIN)

$(HLSSEG_BIN): hlssegtest.c ../main/hlsseg.h
	$(CC) $(WARNCFLAGS) -c hlssegtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ hlssegtest.c $(LDFLAGS)

run-hlsseg: $(HLSSEG_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(HLSSEG_BIN)

$(TSDEMUX_BIN): tsdemuxtest.c ../main/tsdemux.h
	$(CC) $(WARNCFLAGS) -c tsdemuxtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ tsdemuxtest.c $(LDFLAGS)

run-tsdemux: $(TSDEMUX_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(TSDEMUX_BIN)

$(FOLDERART_BIN): folderarttest.c ../main/folderart.c ../main/folderart.h shim.c shim.h
	$(CC) $(WARNCFLAGS) -c folderarttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ folderarttest.c shim.c $(LDFLAGS)

run-folderart: $(FOLDERART_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FOLDERART_BIN)

$(MP3_BIN): mp3counttest.c ../main/mp3count.h
	$(CC) $(WARNCFLAGS) -c mp3counttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mp3counttest.c $(LDFLAGS)

run-mp3: $(MP3_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MP3_BIN)

$(FRAMEWIN_BIN): framewintest.c ../main/framewin.h ../main/mp3count.h
	$(CC) $(WARNCFLAGS) -c framewintest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ framewintest.c $(LDFLAGS)

run-framewin: $(FRAMEWIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FRAMEWIN_BIN)

check-ui:
	@for f in $(UI_CHECK); do \
		echo "$(CC) -c $$f"; \
		$(CC) $(WARNCFLAGS) -c $$f -o /dev/null || exit 1; \
	done

# 5157: the socket budget's guard, checked both ways.
#
# netbudget.h's #error IS the proof that a sdkconfig.defaults change took
# (CLAUDE.md), so a guard that has quietly stopped firing would remove the
# only thing standing between a stale sdkconfig and a board running on the
# old ceiling -- which is what 5033 was. So this asserts it REFUSES below
# the census as well as accepting at it, because an #error that never
# fires and an #error that always passes look identical from a green
# build.
#
# NETBUDGET_NEEDED is 22; the boundary is checked at 21 and 22 rather than
# at a round number, and unset is checked because unset is the stale case.
NETBUDGET_H := ../main/netbudget.h

check-netbudget: $(NETBUDGET_H)
	@echo '#include "netbudget.h"' > netbudget_probe.c
	@echo 'int main(void){return 0;}' >> netbudget_probe.c
	@for v in 1 10 21; do \
		if $(CC) -I../main -DCONFIG_LWIP_MAX_SOCKETS=$$v -c netbudget_probe.c \
			-o /dev/null 2>/dev/null; then \
			echo "FAIL: netbudget.h accepted CONFIG_LWIP_MAX_SOCKETS=$$v"; \
			rm -f netbudget_probe.c; exit 1; \
		fi; \
	done
	@if $(CC) -I../main -c netbudget_probe.c -o /dev/null 2>/dev/null; then \
		echo "FAIL: netbudget.h accepted an UNSET CONFIG_LWIP_MAX_SOCKETS"; \
		rm -f netbudget_probe.c; exit 1; \
	fi
	@for v in 22 24 61; do \
		if ! $(CC) -I../main -DCONFIG_LWIP_MAX_SOCKETS=$$v -c netbudget_probe.c \
			-o /dev/null 2>/dev/null; then \
			echo "FAIL: netbudget.h refused CONFIG_LWIP_MAX_SOCKETS=$$v"; \
			rm -f netbudget_probe.c; exit 1; \
		fi; \
	done
	@rm -f netbudget_probe.c
	@echo "netbudget: refuses below 22, accepts 22 and up, refuses unset -- all passed"

.PHONY: all run-pcmfold run-ctrl run-clock run-rotate run-cardtime run-mediaindex run-medialist run-mediasearch run-mpdqueue run-mpdproto run-mpduri run-mpdmode run-mpdidle run-uireq run-m3uline run-urlclean run-playlist run-screendim run-artplace run-mediawalk run-mediasync run-menuscroll run-fade run-tail run-portal run-old check-ui check-netbudget run-sleep run-sniff run-icy run-netplan run-stations run-pcmfold run-bufplan run-stbjpeg run-addrpin run-codec run-hlsplay run-hlsseg run-tsdemux run-folderart run-mp3 run-framewin run-streamplan run-stationnav run-radiobrowser run-levelhist run-favmatch run-cue run-tagextra run-flacenc run-beam run-i18n clean
