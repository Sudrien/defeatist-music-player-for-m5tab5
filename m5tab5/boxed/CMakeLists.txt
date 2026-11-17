# SPDX-License-Identifier: MIT
#
# u-defeatist-0022: the player as an ELF for a feckless box -- RISC-V user
# mode, every call out of it an ecall the launcher checks. The same sources
# as ../elf/ (main/'s), built against feckless-libs' umode branch with its
# own manifest and lock, so nothing here moves the pins a release is built
# from. See README.md.
#
#     idf.py set-target esp32p4
#     idf.py elf                  ->  build/defeatist_boxed.app.elf
#
# `elf` is elf_loader's target; `idf.py build` would go on to link a
# firmware, which this is not.
cmake_minimum_required(VERSION 3.16)
# u-defeatist-0027: CMake 4.5's policy CMP0225 (ExternalProject passes
# CMAKE_INSTALL_PREFIX=<INSTALL_DIR>), NEW, for ESP-IDF's own
# ExternalProject calls -- the bootloader's -- which otherwise warn at every
# configure. Set before project.cmake: its functions keep the policies of
# its cmake_minimum_required(), which takes the default from here. Older
# CMake ignores it; nothing is installed either way.
set(CMAKE_POLICY_DEFAULT_CMP0225 NEW)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The player's configuration, so the sources compile as the player's do,
# and its partition table by full path (the defaults name it relative to
# the project, which is not this one).
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.boxed.defaults"
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.boxed.defaults")

# minimp3, fetched at its pinned commit and SHA-256 checked, before the
# component scan, as the top level does. MINIMP3_COMMIT stays in scope for
# cmake/libversions.cmake below.
include(${_m5}/cmake/vendored.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(defeatist_boxed)

# Into the ELF, as ../elf/ puts them: the libraries no launcher exports.
set(ELF_COMPONENTS feckless_image_handler)
idf_build_get_property(_comps BUILD_COMPONENTS)
foreach(_c IN LISTS _comps)
    if(_c MATCHES "(^|__)(c?json|esp_jpeg)$")
        list(APPEND ELF_COMPONENTS "${_c}")
    endif()
endforeach()
# feckless_abi_box_elf() links feckless_box_libc in as well, and fails the
# build on any import a box does not answer (boxcheck).
feckless_abi_box_elf(defeatist_boxed)

# 5283: libversions.h for panel.c's BUILD tab, from this project's lock.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${_m5}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")

include(${_m5}/cmake/defaults_check.cmake)
target_compile_definitions(${main_lib} PRIVATE "SDKCONFIG_DRIFT=\"${SDKCONFIG_DRIFT}\"")
