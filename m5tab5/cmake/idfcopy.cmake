# 6032: which ESP-IDF the copies under components/ came from.
#
# exfat.cmake and dpi_instrument.cmake copy IDF's fatfs and esp_lcd into
# components/ at the first configure, and after that leave them alone.
# That was safe while there was one IDF. There are two now (5.5.5 and
# 6.1), and a copy made from one shadows the other's component: an
# esp_lcd from 5.5.5 under a 6.1 build compiles main against the old
# esp_lcd_dpi_panel_config_t and fails far from here, or worse, does not.
#
# So each copy carries `.idf_version`, the version it was made from, and
# a configure under a different IDF stops and says so. It stops rather
# than recopying: esp_lcd's copy is meant to be edited while debugging
# (see dpi_instrument.cmake), and deleting someone's edits because they
# changed IDF is not this file's decision to make.
#
# Included before project.cmake, like the two files that use it, so the
# version comes from IDF's own version.cmake rather than from
# project.cmake, which sets the same three variables later.
#
# SPDX-License-Identifier: MIT

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
set(IDFCOPY_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

# After a copy has been made: record the IDF it came from.
function(idfcopy_mark dir)
    file(WRITE "${dir}/.idf_version" "${IDFCOPY_VERSION}\n")
endfunction()

# Before using a copy that is already there. `tag` is the caller's log
# prefix, `rel` the directory as a person would type it, `how` the way
# to make a new one.
function(idfcopy_check tag dir rel how)
    set(_stamp "${dir}/.idf_version")
    if(NOT EXISTS "${_stamp}")
        # Made before 6032, so nothing says which IDF. 6033: neither
        # script edits the component's CMakeLists.txt, and it differs
        # between 5.5.5 and 6.1 for both, so a copy whose CMakeLists.txt
        # is not this IDF's came from another one. That was the first
        # 6.1 build after 6032: 5.5.5's esp_lcd, taken as 6.1's, failing
        # on hal/lcd_types.h.
        get_filename_component(_name "${dir}" NAME)
        file(SHA256 "${dir}/CMakeLists.txt" _have)
        file(SHA256 "$ENV{IDF_PATH}/components/${_name}/CMakeLists.txt" _want)
        if(NOT _have STREQUAL _want)
            message(FATAL_ERROR
                "${tag}: ${rel} was copied from another ESP-IDF than this "
                "one (${IDFCOPY_VERSION}): its CMakeLists.txt is not this "
                "IDF's. ${how} Anything you changed inside it is yours to "
                "carry over first.")
        endif()
        message(WARNING
            "${tag}: ${rel} does not record which ESP-IDF it was copied "
            "from (it predates that record). It matches this IDF's, so taking "
            "it as ${IDFCOPY_VERSION}.")
        idfcopy_mark("${dir}")
        return()
    endif()
    file(READ "${_stamp}" _from)
    string(STRIP "${_from}" _from)
    if(NOT _from STREQUAL IDFCOPY_VERSION)
        message(FATAL_ERROR
            "${tag}: ${rel} was copied from ESP-IDF ${_from}, and this is "
            "ESP-IDF ${IDFCOPY_VERSION}. A copy of one IDF's component does "
            "not build against another's. ${how} Anything you changed "
            "inside it is yours to carry over first.")
    endif()
endfunction()
