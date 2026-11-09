# SPDX-License-Identifier: MIT
#
# boxwin (u-defeatist-0002): the player's own decoders as an ELF for a
# feckless box, to measure how long they compute between host calls. A
# project of its own, with its own manifest and lock, so that nothing here
# moves the player's pins: it follows feckless-libs' umode branch, the
# player a tag. See main/boxwin.h and README.md.
#
#     idf.py set-target esp32p4
#     idf.py elf                  ->  build/defeatist_boxwin.app.elf
#
# `elf` is elf_loader's target; `idf.py build` would go on to link a
# firmware, which this is not.
cmake_minimum_required(VERSION 3.16)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The player's configuration, so the decoders are compiled as the player's
# are; its partition table by full path, as ../elf/CMakeLists.txt names it
# (the defaults give a name relative to the project, which is not this one).
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.boxwin.defaults"
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.boxwin.defaults")

# minimp3, fetched at its pinned commit and SHA-256 checked, before the
# component scan: as the top level does.
include(${_m5}/cmake/vendored.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(defeatist_boxwin)

# Nothing but main and feckless_box_libc goes into the ELF; minimp3 is
# headers. feckless_abi_box_elf() fails the build on any import a box does
# not answer (boxcheck), which for this app is the first result.
feckless_abi_box_elf(defeatist_boxwin)
