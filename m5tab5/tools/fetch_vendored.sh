#!/usr/bin/env sh
#
# Fetch the libraries this project vendors at a pinned commit:
#
#   minimp3   -- MP3 decode (Layers I/II/III, free format, gapless). CC0-1.0
#
# (pngle, stb_image and J40 are feckless_image_handler's since 8069, and
# fetched by its own cmake/vendored.cmake.)
#
# The fonts are not here: they are generated and committed in
# feckless-libs-for-m5tab5/components/feckless_graphics_handler (fonts/, tools/gen_ark12.py).
#
# You do not normally need to run this: cmake/vendored.cmake fetches the
# same files, from the same pinned commits, at configure time. This is
# here for fetching them ahead of a build, or on a machine with network
# access so they can be carried to one without.
#
# The files are gitignored, not committed.
#
# --revert removes them.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MINIMP3="$ROOT/components/minimp3"

# Same pins as cmake/vendored.cmake. If you change one, change both --
# CMake verifies a SHA256 per file and will refetch anything this script
# leaves behind that does not match.
MINIMP3_REF=ea99364f61c14656440e8d77e9c233ccf3124633

if [ "${1:-}" = "--revert" ]; then
    rm -f "$MINIMP3/minimp3.h" "$MINIMP3/minimp3_ex.h"
    echo "removed vendored headers (CMakeLists.txt kept)"
    exit 0
fi

fetch() {
    echo "  $2"
    curl -fsSL "$1" -o "$2"
}

echo "minimp3 -> components/minimp3/"
mkdir -p "$MINIMP3"
BASE="https://raw.githubusercontent.com/lieff/minimp3/$MINIMP3_REF"
fetch "$BASE/minimp3.h"    "$MINIMP3/minimp3.h"
fetch "$BASE/minimp3_ex.h" "$MINIMP3/minimp3_ex.h"

cat <<'MSG'

Done. These files are gitignored -- do not commit them.

Pinned to commits rather than a branch because neither project tags
releases, and because an unpinned minimp3 can reintroduce the symbol
collision that components/minimp3/minimp3_prefix.h exists to fix.
MSG
