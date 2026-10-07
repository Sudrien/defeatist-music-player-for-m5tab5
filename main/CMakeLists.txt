# 6031: IDF 6.0 moved cJSON out of the tree. On 6.x it is the registry's
# espressif/cjson (idf_component.yml, behind an idf_version rule), whose
# component is `cjson`; on 5.x it is IDF's own `json`. `usb` keeps one
# name on both: IDF's component on 5.x, espressif/usb on 6.x, pulled in
# by usb_host_msc and usb_host_uac's manifests. esp_driver_ledc and
# esp_driver_usb_serial_jtag are named because IDF 6's `driver` no longer
# brings them along; 5.x has both too.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DMP_JSON cjson)
else()
    set(DMP_JSON json)
endif()

idf_component_register(
    SRCS "player.c" "decoder.c" "albumart.c" "covertag.c" "folderart.c" "tagextra.c" "mediacache.c" "ui.c" "audio_out.c"
         "playlist.c" "settings.c"
       "browser.c" "panel.c" "i18n.c" "i18n_tab.c" "duration.c" "cbrseek.c" "flacseek.c" "oggseek.c" "tsseek.c" "mp4seek.c" "waveform.c" "replaygain.c" "loudness.c"
         "portal.c" "portalweb.c" "dnsreply.c"
         "sleeppage.c" "streamprobe.c" "netstream.c" "netdec.c" "stations.c" "favorites.c" "starred.c" "streamgain.c" "radiobrowser.c" "bench.c" "cuedir.c" "cardtime.c" "mediacat.c" "mediawalk.c" "mediasync.c" "medialib.c" "mediadir.c" "rateconv.c" "polyrsp.c" "radiokeep.c" "icoimg.c" "heapmap.c" "flacenc.c" "recorder.c" "beam.c"
         "mpdqueue.c" "mpdproto.c" "mpdfilter.c" "casefold_tab.c" "mpduri.c" "mpdmode.c" "mpdidle.c" "mpd.c" "uireq.c" "m3uline.c" "urlclean.c" "remote.c" "remoteproto.c" "certgen.c" "devcert.c" "wifijoin.c" "nvsns.c" "launcher_import.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "remote_i18n.html"    # 6028: generated from remote.html
    EMBED_FILES "favicon.ico"
    REQUIRES driver esp_app_format esp_driver_i2c esp_driver_i2s esp_driver_sdmmc
             esp_driver_jpeg esp_driver_ledc esp_driver_usb_serial_jtag esp_lcd esp_lcd_touch fatfs sdmmc
             usb usb_host_msc usb_host_uac esp_timer lwip ${DMP_JSON}
             nvs_flash esp_netif esp_event esp_http_server esp_https_server esp_http_client mbedtls
             esp_usbh_asix iot_eth iot_usbh_ecm iot_usbh_cdc
             app_update spi_flash   # launcher_import.c: esp_ota_ops + esp_partition
             feckless_network_handler feckless_storage_handler   # 6053
             feckless_drivers   # 6054
             feckless_graphics_handler   # 6055: gfx, lcd, ark12, arabixel
    PRIV_REQUIRES pngle minimp3 stbjpeg
)

# Heap integrity checkpoints, off by default. Build with
#   idf.py -DHEAPCHECK=1 build
# to have main/heapcheck.h's HEAP_CHECK() name the last subsystem to run
# before the heap went bad. See its header for why that is the question
# worth asking. Pair with CONFIG_HEAP_POISONING_COMPREHENSIVE to catch
# the write itself rather than the next free.
# The waveform scan, off by default as of 0101. Build with
#   idf.py -DWAVEFORM=1 build
# to put it back. It reads the whole file to draw the seek bar's envelope,
# which on a Xing-less MP3 is the second whole-file read of the same track
# -- decoder_open()'s index build is the first. Off until that duplication
# is resolved; the code is intentionally still compiled, see do_walk().
if(WAVEFORM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WAVEFORM_SCAN=1)
    message(STATUS "waveform envelope scan ENABLED")
endif()

# 5092's --wrap of eh_host_port_dma_alloc moved to
# feckless-network-handler's CMakeLists.txt with hostedwrap.c (6053).

# 5277: every file in main at -O3, off by default. Build with
#   idf.py -DO3CHECK=1 build
# to have GCC's stronger -O3 analysis -- deeper inlining, more unrolling,
# more paths followed -- look at all of main, not only decoder.c (5276).
# A CHECK, not a shipping setting: 5276 measured -O3 no faster here and
# 13 KB bigger for minimp3 alone. IDF builds with -Werror=all, so what it
# finds stops the build. Not cached away: a later plain `idf.py build`
# keeps O3CHECK in CMakeCache.txt until `idf.py -DO3CHECK=0 build`.
#
# Per source file, APPENDed, and not target_compile_options(): GCC takes
# the last -O it is given, and a source file's options come after the
# component's, where IDF's own -O2 is -- the order 5245's -Os relied on.
# APPEND keeps decoder.c's own options (5276).
if(O3CHECK)
    get_target_property(_o3_srcs ${COMPONENT_LIB} SOURCES)
    set_property(SOURCE ${_o3_srcs} APPEND PROPERTY COMPILE_OPTIONS -O3)
    message(STATUS "O3CHECK: main compiled at -O3 (a warning check, not a release build)")
endif()

# 5279: minimp3's decode cost in CPU cycles, logged every ~30 s of audio
# and at the end of each MP3 track. Off by default. Build with
#   idf.py -DDECBENCH=1 build
# and compare the "DECBENCH track" lines between builds. Like O3CHECK and
# HEAPCHECK it stays in CMakeCache.txt until `-DDECBENCH=0`.
if(DECBENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DECBENCH=1)
    message(STATUS "DECBENCH: minimp3 decode cycles logged")
endif()

if(HEAPCHECK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HEAPCHECK=1)
    message(STATUS "heap integrity checkpoints ENABLED (slow)")
endif()

# minimp3 is a header-only library compiled into decoder.c, so its
# warnings land on our source. Upstream is clean under -Wall but trips
# -Wunused-function when MINIMP3_ONLY_SIMD-style paths are compiled out,
# which they are on RISC-V.
set_source_files_properties(decoder.c PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function")

# 5276: decoder.c at -O3 -- minimp3, the one decoder compiled from source.
# Here and not in the top-level CMakeLists.txt: a source file property
# reaches only targets made in the same directory, and this component's
# target is made here, so from the top level it never arrived and the
# file built at -O2. -O3 trips -Wstringop-overflow on minimp3's iscf[40]
# (minimp3.h:696-697); a false positive -- the largest index the loop can
# reach is 38 -- so it is kept a warning rather than an error.
set_source_files_properties(decoder.c PROPERTIES COMPILE_OPTIONS
    "-O3;-Wno-error=stringop-overflow")
