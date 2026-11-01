# 8087: the player as an ELF for esp_elf -- main/'s sources, linked
# position-independent with the libraries that are not the ABI's linked in
# (minimp3 is a header in decoder.c; cJSON; feckless_image_handler's
# decoders) and every other call left
# undefined: the feckless ABI's, with its storage, nvs, network, image,
# crypto and audio sections (feckless_abi, feckless_abi_storage, feckless_abi_nvs,
# feckless_abi_network, feckless_abi_image, feckless_abi_crypto,
# feckless_abi_audio from
# feckless-libs-for-m5tab5). The build checks the ELF against them. The
# shape is Errant's, Lothesome's, Unprofitable Mahjong's and Spurious's.
#
# The top-level build runs this one (main/CMakeLists.txt) and embeds what it
# makes. On its own:
#   idf.py -C elf -B build-elf build
# writes build-elf/defeatist_player.app.elf.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

# The configuration is the .bin's, copied whole when the top-level build
# passes it: struct layouts and inline functions in the headers depend on it
# (FatFs's FIL, httpd_req_t, the HTTP client's config), and the ELF must
# agree with the host it runs on. On its own, the defaults.
#
# Except partitions.csv, which is relative to the project and so would be
# looked for in elf/: made absolute, here and in the defaults (the later
# file wins).
get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_partitions_line "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"")
if(DEFEATIST_HOST_SDKCONFIG AND SDKCONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DEFEATIST_HOST_SDKCONFIG}")
    file(READ "${DEFEATIST_HOST_SDKCONFIG}" _cfg)
    string(REGEX REPLACE "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"\n]*\""
           "${_partitions_line}" _cfg "${_cfg}")
    file(WRITE "${SDKCONFIG}.host" "${_cfg}")
    configure_file("${SDKCONFIG}.host" "${SDKCONFIG}" COPYONLY)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults" "${_partitions_line}\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults")
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults.idf6")
endif()
list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults")

# minimp3 is two headers fetched into ../components/minimp3 by the top
# level (cmake/vendored.cmake, before its project()); on its own, here.
# MINIMP3_COMMIT stays in scope for cmake/libversions.cmake below.
include(${_m5}/cmake/vendored.cmake)

# main is ../main, minimp3 is ../components; only what main needs is built.
list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/main" "${_m5}/components")
set(COMPONENTS main)

# The .bin's version, which CI takes from the release tag (version.txt
# beside the .bin's CMakeLists.txt, not here), so the ELF's own
# esp_app_desc_t says the same. On its own, IDF's usual: git describe.
if(DEFEATIST_PROJECT_VER)
    set(PROJECT_VER "${DEFEATIST_PROJECT_VER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tells main/CMakeLists.txt which of its two builds this is.
idf_build_set_property(DEFEATIST_ELF 1)
# One manifest, so one lock: the top level's, per IDF major (6031).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    idf_build_set_property(DEPENDENCIES_LOCK "${_m5}/dependencies.lock.idf6")
else()
    idf_build_set_property(DEPENDENCIES_LOCK "${_m5}/dependencies.lock")
endif()

project(defeatist_player)

# Into the ELF, not the ABI: the libraries the ABI does not export, because
# nothing on the launcher's side is built from them. Components that come
# from the registry have the manager's name (espressif__esp_jpeg,
# espressif__cjson); IDF 5's cJSON is its own `json`. Whichever it is, it is
# the component whose name ends so, as Errant finds it. feckless_image_handler
# is a git dependency, so its bare name.
#
# 8093: not esp_audio_codec. Its decoders are precompiled archives built
# without -fPIC, which a position-independent link refuses (R_RISCV_HI20,
# "recompile with -fPIC"); the launcher links the codec and the ELF takes
# it through the ABI's audio section (feckless_abi_audio). 8090's loop,
# which put a component's link-interface archives on the ELF's link line
# for it, found only a -L (the archives are add_prebuilt_library()
# targets, not paths; 8091's -l<name> skips targets too) and is gone with it.
set(ELF_COMPONENTS feckless_image_handler)
idf_build_get_property(_comps BUILD_COMPONENTS)
foreach(_c IN LISTS _comps)
    if(_c MATCHES "(^|__)(c?json|esp_jpeg)$")
        list(APPEND ELF_COMPONENTS "${_c}")
    endif()
endforeach()
feckless_abi_elf(defeatist_player)

# 5283: libversions.h, the BUILD tab's list -- moved here from the top-level
# CMakeLists.txt with the code that includes it (panel.c). It reads the
# licences from the top level's managed_components/, which has everything
# the lock names.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${_m5}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")

# The keys of sdkconfig.defaults that this configuration does not have,
# for player.c to print under the banner. The configuration is the .bin's
# copied whole, so this is the answer the top level got too, which warns
# about it as well.
include(${_m5}/cmake/defaults_check.cmake)
target_compile_definitions(${main_lib} PRIVATE "SDKCONFIG_DRIFT=\"${SDKCONFIG_DRIFT}\"")
