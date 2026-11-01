# 8087: the player as an ELF for esp_elf -- main/'s sources, linked
# position-independent with the libraries that are not the ABI's linked in
# (minimp3 is a header in decoder.c; cJSON; feckless_image_handler's
# decoders; esp_audio_codec's decoders) and every other call left
# undefined: the feckless ABI's, with its storage, nvs, network, image and
# crypto sections (feckless_abi, feckless_abi_storage, feckless_abi_nvs,
# feckless_abi_network, feckless_abi_image, feckless_abi_crypto from
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
# from the registry have the manager's name (espressif__esp_audio_codec,
# espressif__cjson); IDF 5's cJSON is its own `json`. Whichever it is, it is
# the component whose name ends so, as Errant finds it. feckless_image_handler
# is a git dependency, so its bare name.
set(ELF_COMPONENTS feckless_image_handler)
idf_build_get_property(_comps BUILD_COMPONENTS)
foreach(_c IN LISTS _comps)
    if(_c MATCHES "(^|__)(c?json|esp_audio_codec|esp_jpeg)$")
        list(APPEND ELF_COMPONENTS "${_c}")
    endif()
endforeach()

# 8090: the archives a component brings that are not its own code. The
# ELF is linked from the components' libraries only, so esp_audio_codec's
# decoders -- precompiled .a files the component names in its link
# interface, not libespressif__esp_audio_codec.a itself -- were left out and
# showed as undefined (esp_*_dec_register, esp_audio_simple_dec_*). What a
# component in ELF_COMPONENTS asks to be linked with, as flags and
# archive paths, and the bare names that are no target (-l<name>), goes on
# the ELF's link line (ELF_LIBS, which project_elf() takes). Printed, since this is read from the component manager's CMake
# without being run: the line says what was found.
set(_elf_extra "")
foreach(_c IN LISTS ELF_COMPONENTS)
    idf_component_get_property(_lib ${_c} COMPONENT_LIB)
    set(_prev_ll "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_ll ${_lib} ${_prop})
        # IDF copies a component's link interface into both; once is enough.
        if(NOT _ll OR _ll STREQUAL _prev_ll)
            continue()
        endif()
        set(_prev_ll "${_ll}")
        set(_prev "")
        foreach(_l IN LISTS _ll)
            if(_l MATCHES "^-" OR _l MATCHES "\\.a$" OR _prev STREQUAL "-L")
                list(APPEND _elf_extra "${_l}")
            elseif(_l MATCHES "^[A-Za-z0-9_.+]+$" AND NOT TARGET "${_l}")
                # A bare name that is no target is a library: -l<name>.
                # (8091: esp_audio_codec names its decoders so, after a
                # "-L <dir>"; the flags alone found no archive.)
                list(APPEND _elf_extra "-l${_l}")
            endif()
            set(_prev "${_l}")
        endforeach()
    endforeach()
endforeach()
# In order and not de-duplicated: a --start-group needs its --end-group.
list(APPEND ELF_LIBS ${_elf_extra})
message(STATUS "defeatist_player: ELF_COMPONENTS ${ELF_COMPONENTS}; extra link line: ${_elf_extra}")
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
