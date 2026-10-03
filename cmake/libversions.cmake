# 5283: the libraries this build was made from, for the settings panel's
# BUILD tab -- written as a header, libversions.h, that main includes.
#
# From the records the build itself uses, so the screen cannot drift
# from what was compiled:
#
#   dependencies.lock        every managed component and the version the
#                            component manager resolved (git ones by commit)
#   cmake/vendored.cmake     the commits minimp3, pngle and stb_image are
#                            fetched at (MINIMP3_COMMIT etc., set before
#                            project() and so in scope here)
#   components/usb_host_*/   the vendored USB class drivers' own manifests;
#     idf_component.yml      both carry fixes past that version (5026, 5035)
#   tools/gen_ark12.py       the Ark Pixel commit the font was cut from
#
# Include AFTER project(). Sets LIBVERSIONS_DIR to the directory holding
# libversions.h. The header is rewritten only when its text changes, so
# an unchanged lock does not rebuild panel.c; a changed one reconfigures
# (CMAKE_CONFIGURE_DEPENDS below), which is when the component manager
# would have changed it anyway.

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_root "${_root}" ABSOLUTE)
# 6031: the lock this build used, which is dependencies.lock.idf6 on IDF 6
# (see the top-level CMakeLists.txt). IDF sets the property's default.
idf_build_get_property(_lock DEPENDENCIES_LOCK)
if(NOT _lock)
    set(_lock "${_root}/dependencies.lock")
endif()
set(_libs "")

# A 40-hex git commit as its first seven; anything else as written.
function(_lv_short out ver)
    string(LENGTH "${ver}" _len)
    if(_len EQUAL 40 AND ver MATCHES "^[0-9a-f]+$")
        string(SUBSTRING "${ver}" 0 7 ver)
    endif()
    set(${out} "${ver}" PARENT_SCOPE)
endfunction()

# 5284: a component's licence, from the licence file in its directory --
# the copy that was compiled, so a licence change in a new version shows.
# The file, not idf_component.yml's `license:` field: of the components
# here only esp_hosted and esp_usbh_asix fill the field in, and every
# one ships a file. The field is the fallback.
function(_lv_license out dir)
    set(_lic "")
    file(GLOB _files LIST_DIRECTORIES false
         "${dir}/license.txt" "${dir}/LICENSE" "${dir}/LICENSE.*"
         "${dir}/LICENCE" "${dir}/LICENSE-*")
    foreach(_f IN LISTS _files)
        file(STRINGS "${_f}" _head LIMIT_COUNT 15)
        string(JOIN "\n" _head ${_head})
        if(_head MATCHES "(Valid|SPDX)-License-Identifier: *([A-Za-z0-9.+-]+)")
            set(_lic "${CMAKE_MATCH_2}")
        elseif(_head MATCHES "Espressif Modified MIT")
            set(_lic "Espressif MIT")
        elseif(_head MATCHES "Apache License" AND _head MATCHES "Version 2\\.0")
            set(_lic "Apache-2.0")
        elseif(_head MATCHES "MIT License")
            set(_lic "MIT")
        elseif(_head MATCHES "CC0 1\\.0")
            set(_lic "CC0-1.0")
        endif()
        if(_lic)
            break()
        endif()
    endforeach()
    if(NOT _lic AND EXISTS "${dir}/idf_component.yml")
        file(STRINGS "${dir}/idf_component.yml" _f REGEX "^license:")
        if(_f MATCHES "^license: *[\"']?([^\"']+)")
            set(_lic "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT _lic)
        set(_lic "licence not found")
    endif()
    set(${out} "${_lic}" PARENT_SCOPE)
endfunction()

# Where the component manager put a lock entry: "espressif/esp_jpeg" is
# managed_components/espressif__esp_jpeg, a git one by its bare name.
set(_managed "${_root}/managed_components")

if(EXISTS "${_lock}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lock}")
    file(STRINGS "${_lock}" _lines)
    set(_name "")
    set(_in_deps FALSE)
    foreach(_l IN LISTS _lines)
        if(_l STREQUAL "dependencies:")
            set(_in_deps TRUE)
        elseif(_l MATCHES "^[a-z_]+:")          # direct_dependencies, target, ...
            set(_in_deps FALSE)
        elseif(_in_deps AND _l MATCHES "^  ([^ ][^:]*):$")
            set(_name "${CMAKE_MATCH_1}")
        elseif(_in_deps AND _name AND _l MATCHES "^    version: '?([^']*)'?$")
            # Four spaces: the component's own version. A dependency's
            # requirement is indented further and does not match.
            set(_ver "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^espressif/" "" _short "${_name}")
            if(NOT _short STREQUAL "idf" AND NOT _short STREQUAL "cmake_utilities")
                _lv_short(_ver "${_ver}")
                string(REPLACE "/" "__" _dir "${_name}")
                _lv_license(_lic "${_managed}/${_dir}")
                # esp_jpeg's own code is Apache-2.0; the TJpgDec it wraps
                # is ChaN's, under his terms, whose notice must travel
                # with the binary (README, Licensing).
                if(_short STREQUAL "esp_jpeg")
                    string(APPEND _lic " + TJpgDec (ChaN)")
                endif()
                list(APPEND _libs "${_short} ${_ver} -- ${_lic}")
            endif()
            set(_name "")
        endif()
    endforeach()
else()
    list(APPEND _libs "dependencies.lock missing")
endif()

foreach(_c usb_host_msc usb_host_uac)
    set(_yml "${_root}/components/${_c}/idf_component.yml")
    if(EXISTS "${_yml}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_yml}")
        file(STRINGS "${_yml}" _v REGEX "^version:")
        string(REGEX REPLACE "^version: *\"?([^\"]*)\"?.*$" "\\1" _v "${_v}")
        _lv_license(_lic "${_root}/components/${_c}")
        list(APPEND _libs "${_c} ${_v}, patched -- ${_lic}")
    endif()
endforeach()

# These three are fetched as single headers, without their licence files,
# so their licences are written here -- checked at these commits (5284):
# minimp3's LICENSE is CC0 1.0, pngle's MIT, and stb_image.h ends with
# its two alternatives, MIT or the Unlicense. A new commit in
# cmake/vendored.cmake means checking the licence again.
foreach(_pair "minimp3;MINIMP3_COMMIT;CC0-1.0" "pngle;PNGLE_COMMIT;MIT"
              "stb_image;STB_COMMIT;MIT or Unlicense")
    list(GET _pair 0 _n)
    list(GET _pair 1 _var)
    list(GET _pair 2 _lic)
    if(DEFINED ${_var})
        _lv_short(_v "${${_var}}")
        list(APPEND _libs "${_n} ${_v} -- ${_lic}")
    endif()
endforeach()

set(_ark "${_root}/tools/gen_ark12.py")
if(EXISTS "${_ark}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ark}")
    file(STRINGS "${_ark}" _v REGEX "^ARK_COMMIT *=")
    string(REGEX REPLACE "^ARK_COMMIT *= *\"([0-9a-f]+)\".*$" "\\1" _v "${_v}")
    _lv_short(_v "${_v}")
    # OFL-1.1, and components/ark12 with it -- see its README and
    # LICENSE-OFL, which has to ship with the firmware.
    list(APPEND _libs "Ark Pixel font ${_v} -- OFL-1.1")
endif()

list(LENGTH _libs _n)
set(_h "/* Generated by cmake/libversions.cmake (5283). Do not edit. */\n")
string(APPEND _h "#pragma once\n#define LIBVERSIONS_COUNT (${_n})\n")
string(APPEND _h "static const char *const k_libversions[LIBVERSIONS_COUNT] = {\n")
foreach(_l IN LISTS _libs)
    string(APPEND _h "    \"${_l}\",\n")
endforeach()
string(APPEND _h "};\n")

set(LIBVERSIONS_DIR "${CMAKE_BINARY_DIR}/libversions")
file(WRITE "${LIBVERSIONS_DIR}/libversions.h.tmp" "${_h}")
configure_file("${LIBVERSIONS_DIR}/libversions.h.tmp"
               "${LIBVERSIONS_DIR}/libversions.h" COPYONLY)
message(STATUS "libversions: ${_n} libraries for the BUILD tab")
