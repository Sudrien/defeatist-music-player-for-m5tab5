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
