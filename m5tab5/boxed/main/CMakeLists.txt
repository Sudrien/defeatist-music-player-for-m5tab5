# SPDX-License-Identifier: MIT
#
# u-defeatist-0022: the boxed player's one component. The sources are
# ../../main's, the ones ../elf/ builds, by path: this component has its
# own manifest (feckless-libs' umode branch), which main/'s pinned one
# cannot be. In their place:
#
#   boxed_main.c   app_main: elf_main.c's, requiring the storage section
#                  and the box rather than the sections a box does not
#                  have (network, NVS, image, crypto, audio).
#   absent.c       those sections' calls, and the few others a box does
#                  not answer, as the app's own functions that fail the
#                  way the player already handles: no network, nothing
#                  stored in NVS, no hardware JPEG.
#   absent_codec.c esp_audio_codec as absent, only with
#                  -DDEFEATIST_BOXED_CODEC=OFF (u-defeatist-0023); by
#                  default the box answers it (u-feckless-0052).
#
# and feckless_storage_handler's storage_list.c, in the app: its reader
# calls the app's filter for every entry, which a launcher cannot do.
set(_m "${CMAKE_CURRENT_LIST_DIR}/../../main")

# The player's sources, as main/CMakeLists.txt lists them for the ELF,
# less elf_main.c. Checked below against that file, so one added there
# and not here stops the configure instead of leaving an import.
set(_player_srcs
    player.c decoder.c albumart.c covertag.c folderart.c tagextra.c mediacache.c ui.c
    playlist.c settings.c
    browser.c panel.c i18n.c i18n_tab.c duration.c cbrseek.c flacseek.c oggseek.c tsseek.c mp4seek.c
    waveform.c replaygain.c loudness.c
    portal.c portalweb.c dnsreply.c
    sleeppage.c streamprobe.c netstream.c netdec.c stations.c favorites.c starred.c streamgain.c
    radiobrowser.c bench.c cuedir.c cardtime.c mediacat.c mediawalk.c mediasync.c medialib.c
    mediadir.c rateconv.c varispeed.c fx.c radiokeep.c heapmap.c flacenc.c recorder.c beam.c
    mpdqueue.c mpdproto.c mpdfilter.c casefold_tab.c mpduri.c mpdmode.c mpdidle.c mpd.c uireq.c
    m3uline.c urlclean.c remote.c approve.c podindex.c poddl.c remoteproto.c wifijoin.c launcher.c)

file(READ "${_m}/CMakeLists.txt" _main_cmake)
string(REGEX MATCHALL "\"[a-z0-9_]+\\.c\"" _main_srcs "${_main_cmake}")
string(REPLACE "\"" "" _main_srcs "${_main_srcs}")
list(REMOVE_DUPLICATES _main_srcs)
list(REMOVE_ITEM _main_srcs standalone.c shim_import.c nvsns.c elf_main.c)   # the shim's; the ELF's entry
set(_missing ${_main_srcs})
list(REMOVE_ITEM _missing ${_player_srcs})
set(_extra ${_player_srcs})
list(REMOVE_ITEM _extra ${_main_srcs})
if(_missing OR _extra)
    message(FATAL_ERROR "boxed/main/CMakeLists.txt and main/CMakeLists.txt list different sources: "
                        "not here [${_missing}], not there [${_extra}]")
endif()

set(_srcs "boxed_main.c" "absent.c")
# u-defeatist-0023: esp_audio_codec through the box (u-feckless-0052), on
# by default; off, absent_codec.c in the app, for a launcher without it.
if(NOT DEFINED DEFEATIST_BOXED_CODEC)
    set(DEFEATIST_BOXED_CODEC ON)
endif()
if(NOT DEFEATIST_BOXED_CODEC)
    list(APPEND _srcs "absent_codec.c")
endif()
foreach(_f IN LISTS _player_srcs)
    list(APPEND _srcs "${_m}/${_f}")
endforeach()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS "." "${_m}"
    EMBED_FILES "${_m}/remote_i18n.html.gz" "${_m}/favicon.ico"
    REQUIRES feckless_abi feckless_abi_storage feckless_abi_network
             feckless_abi_nvs feckless_abi_image feckless_abi_crypto feckless_abi_audio
             driver esp_app_format esp_driver_i2c esp_driver_i2s esp_driver_sdmmc
             esp_driver_ledc esp_driver_usb_serial_jtag esp_lcd esp_lcd_touch fatfs sdmmc
             usb usb_host_msc usb_host_uac esp_timer lwip cjson
             nvs_flash esp_netif esp_event esp_http_server esp_https_server esp_http_client mbedtls
             esp_usbh_asix iot_eth iot_usbh_ecm iot_usbh_cdc
             app_update spi_flash
             feckless_network_handler feckless_storage_handler feckless_nvs_handler
             feckless_drivers feckless_graphics_handler feckless_image_handler
             feckless_box_libc            # linked into the ELF
    PRIV_REQUIRES minimp3
)

if(DEFEATIST_BOXED_CODEC)
    set_property(SOURCE boxed_main.c APPEND PROPERTY COMPILE_DEFINITIONS DEFEATIST_BOXED_CODEC=1)
endif()

# storage_list.c from feckless_storage_handler, compiled into the app.
idf_component_get_property(_sh feckless_storage_handler COMPONENT_DIR)
target_sources(${COMPONENT_LIB} PRIVATE "${_sh}/src/storage_list.c")

# The player's per-file options (main/CMakeLists.txt says why each is so):
# 8031's -Os and 8032's warning for the files nothing waits on; decoder.c
# at -O3 with minimp3's two warnings (5276).
set(_os mpd.c mpdproto.c mpdfilter.c mpdidle.c mpdmode.c mpduri.c
        remote.c remoteproto.c portal.c portalweb.c dnsreply.c sleeppage.c
        poddl.c podindex.c radiobrowser.c stations.c favorites.c starred.c
        approve.c wifijoin.c launcher.c)
list(TRANSFORM _os PREPEND "${_m}/")
set_property(SOURCE ${_os} APPEND PROPERTY COMPILE_OPTIONS -Os -Wno-error=format-truncation)
set_source_files_properties("${_m}/decoder.c" PROPERTIES COMPILE_OPTIONS
    "-O3;-Wno-unused-function;-Wno-error=stringop-overflow")
