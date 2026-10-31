# minimp3: CC0-1.0, from https://github.com/lieff/minimp3
#
# Header-only. minimp3.h and minimp3_ex.h are fetched by
# cmake/vendored.cmake at configure time (or by tools/fetch_vendored.sh
# by hand), pinned to a commit and checked against a SHA256. They are
# gitignored, not committed -- see components/minimp3/README.md.
#
# No SRCS: the implementation is emitted into decoder.c by its
# MINIMP3_IMPLEMENTATION define. Registering the component with an empty
# source list is what makes INCLUDE_DIRS propagate.
idf_component_register(
    INCLUDE_DIRS "."
)
