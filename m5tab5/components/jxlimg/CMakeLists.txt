# jxlimg: JPEG XL cover art through J40 (Kang Seonghoon, public domain or
# MIT No Attribution, https://github.com/lifthrasiir/j40). See jxlimg.h.
#
# j40.h is fetched by cmake/vendored.cmake at configure time, pinned and
# hash-checked, and is gitignored like stb_image.h.
idf_component_register(
    SRCS "jxlimg.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES heap
)

# J40 is warning-clean at -Wall on most compilers; ESP-IDF's set is wider.
# -fwrapv for the reason stbjpeg has it: a damaged file is garbage pixels,
# not undefined behaviour.
target_compile_options(${COMPONENT_LIB} PRIVATE -fwrapv
    -Wno-unused-function -Wno-sign-compare -Wno-unused-but-set-variable
    -Wno-type-limits -Wno-implicit-fallthrough -Wno-unused-variable
    -Wno-maybe-uninitialized -Wno-missing-field-initializers
    # 6141: on RISC-V newlib int32_t is long, not int. J40 prints int32_t
    # with %d and passes an int * where it declares int32_t *: the same
    # 32 bits either way, and invisible on the host, where int32_t is
    # int. GCC 14 makes the pointer mismatch an error by default.
    -Wno-format -Wno-incompatible-pointer-types
    # And nothing in upstream's code stops the build: its warnings are
    # upstream's, and the file is fetched unmodified.
    -Wno-error)

# 8031: -Os. J40 is one 130 KB translation unit of macros that -O2 inlines
# into itself; -Os takes it to about 60%. A JPEG XL cover is rare and is
# decoded off the UI's path, so the extra time is the price. Per source file
# and in this directory, where the component's target is made (a property
# set from another directory never reaches it -- 5276), and after IDF's -O2.
set_property(SOURCE jxlimg.c APPEND PROPERTY COMPILE_OPTIONS -Os)
