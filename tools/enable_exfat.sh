#!/usr/bin/env bash
#
# Enable exFAT in FatFs by vendoring a patched copy of IDF's fatfs
# component into components/.
#
# ESP-IDF ships FatFs with exFAT compiled out and offers no menuconfig
# option for it, so the only way in is a local copy of the component
# with ffconf.h edited. A component in components/ shadows the one in
# IDF, so nothing in the IDF tree is touched.
#
# Read the licensing note in the README before running this.
#
# Usage:  ./tools/enable_exfat.sh             from the project root
#         ./tools/enable_exfat.sh --revert
#         ./tools/enable_exfat.sh --no-clean
#
# cmake/exfat.cmake runs this automatically at configure time with
# --no-clean, so a fresh clone gets exFAT without anyone having to know
# this file exists. Running it by hand still works and is what --revert
# is for.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

DEST="components/fatfs"

# Whether to delete build/ and sdkconfig at the end. Doing that is right
# when a human runs this against a tree that has already been built, and
# is catastrophic when cmake runs it, since cmake is executing from
# inside the directory it would remove.
#
# The deletion is not needed on the cmake path anyway: exfat.cmake runs
# before project.cmake, so components/fatfs exists by the time IDF scans
# for components and its Kconfig is picked up in the same configure that
# created it. There is no stale sdkconfig to escape, because the
# sdkconfig is generated after this has run.
CLEAN=1
if [ "${1:-}" = "--no-clean" ]; then
    CLEAN=0
    shift || true
fi

if [ "${1:-}" = "--revert" ]; then
    rm -rf "$DEST" build sdkconfig
    echo "removed $DEST, build/ and sdkconfig; back to stock FAT-only FatFs"
    echo "(sdkconfig.defaults pins CONFIG_IDF_TARGET, so the target survives this)"
    echo "now run: idf.py build flash monitor"
    exit 0
fi

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH is not set. Source export.sh first." >&2
    exit 1
fi

SRC="$IDF_PATH/components/fatfs"
if [ ! -d "$SRC" ]; then
    echo "no fatfs component at $SRC" >&2
    exit 1
fi

# Already patched. An error for a human -- who asked for this and should
# be told it was a no-op -- and a silent success for cmake, which asks on
# every single configure and would otherwise fail every build after the
# first.
if [ -e "$DEST" ]; then
    if [ "$CLEAN" = "0" ]; then
        exit 0
    fi
    echo "$DEST already exists; delete it or run --revert first" >&2
    exit 1
fi

mkdir -p components
cp -r "$SRC" "$DEST"
chmod -R u+w "$DEST"

CONF="$DEST/src/ffconf.h"
if [ ! -f "$CONF" ]; then
    echo "expected $CONF; IDF layout may have changed" >&2
    exit 1
fi

# Set an FF_* option to a literal value, whatever whitespace or current
# value the line uses, then prove it took. A sed that matches nothing
# exits 0, so without the check a failed patch looks exactly like a
# successful one and the only symptom is exFAT volumes still refusing
# to mount at runtime.
set_ff() {
    local name="$1" want="$2"
    if ! grep -qE "^#define[[:space:]]+${name}[[:space:]]" "$CONF"; then
        echo "error: no ${name} line in $CONF; IDF layout may have changed" >&2
        exit 1
    fi
    sed -i -E "s|^#define[[:space:]]+${name}[[:space:]].*|#define ${name}\t${want}|" "$CONF"
    local got
    got=$(grep -E "^#define[[:space:]]+${name}[[:space:]]" "$CONF" | awk '{print $3}')
    if [ "$got" != "$want" ]; then
        echo "error: ${name} is '${got}', expected '${want}'" >&2
        exit 1
    fi
}

# exFAT itself.
set_ff FF_FS_EXFAT 1

# exFAT volumes routinely exceed the 32-bit sector addressing limit.
set_ff FF_LBA64 1

# The SD and MSC disk IO layers do not implement TRIM, and FatFs calls
# it during exFAT operations. Left on, it surfaces as
# ESP_ERR_INVALID_RESPONSE from the trim path.
set_ff FF_USE_TRIM 0

# ffconf.h maps most FF_* options straight onto Kconfig symbols, e.g.
#
#     #define FF_USE_LABEL    CONFIG_FATFS_USE_LABEL
#
# Kconfig emits nothing at all for a bool that is off -- not 0, nothing
# -- so FF_USE_LABEL expands to a bare undeclared identifier. Stock IDF
# never notices, because the only use of it in ff.c sits in code that
# compiles only when exFAT is enabled. Turning exFAT on is exactly what
# exposes it:
#
#     ff.c: In function 'dir_read':
#     ffconf.h:55: error: 'CONFIG_FATFS_USE_LABEL' undeclared
#
# Rewrite those lines so an unset symbol means 0 instead of a syntax
# error. Behaviour is unchanged: set stays set, unset becomes the 0 it
# was always meant to be. Every FF_* option defined directly as a
# CONFIG_* symbol has the same hazard, so fix them all rather than
# playing whack-a-mole as each new exFAT code path lights one up.
python3 - "$CONF" <<'PYEOF'
import re, sys

path = sys.argv[1]
text = open(path).read()

pattern = re.compile(
    r'^#define[ \t]+(FF_[A-Z0-9_]+)[ \t]+(CONFIG_[A-Z0-9_]+)[ \t]*$',
    re.MULTILINE)

fixed = []

def guard(m):
    ff, cfg = m.group(1), m.group(2)
    fixed.append(ff)
    return ('#ifdef %s\n#define %s\t%s\n#else\n#define %s\t0\n#endif'
            % (cfg, ff, cfg, ff))

text = pattern.sub(guard, text)
open(path, 'w').write(text)

if fixed:
    print('guarded against unset Kconfig bools: ' + ', '.join(fixed))
PYEOF

# 6035: say where FatFs found a damaged exFAT entry. load_xdir() checks a
# file's directory entry set -- order, sizes, checksum -- and returns
# FR_INT_ERR, which the VFS reports as EIO, the same errno as a failed
# read: a card with one torn entry and a dying card both said "I/O
# error". The definition is renamed and a wrapper of the old name calls
# it, so every caller is covered without touching one, and on failure
# calls ff_tab5_bad_entry() (weak here; main/storage.c logs it).
FFC="$DEST/src/ff.c"
python3 - "$FFC" <<'PYEOF'
import re, sys
path = sys.argv[1]
text = open(path).read()
head = 'static FRESULT load_xdir ('
if text.count(head) != 1:
    sys.exit('error: expected one "%s" in %s; FatFs changed' % (head, path))
start = text.index(head)
end = text.index('\n}\n', start) + 3
body = text[start:end].replace(head, 'static FRESULT load_xdir_checked (', 1)
# The checksum test is the last one, made once every name entry is in
# dirbuf. Flag it there, so the wrapper knows a name it prints is this
# entry's and not left over from the one before.
sumline = re.compile(r'if \(xdir_sum\(dirb\) != ld_(?:16|word)\(dirb \+ XDIR_SetSum\)\) return FR_INT_ERR;')
if len(sumline.findall(body)) != 1:
    sys.exit('error: expected one checksum test in load_xdir() in %s; FatFs changed' % path)
body = sumline.sub('if (xdir_sum(dirb) != (WORD)(dirb[XDIR_SetSum] | dirb[XDIR_SetSum + 1] << 8)) '
                   '{ tab5_sumfail[dp->obj.fs->pdrv] = 1; return FR_INT_ERR; }', body)
text = (text[:start]
        + '/* tools/enable_exfat.sh (6035): set by load_xdir_checked(), read by load_xdir(). */\n'
        + 'static BYTE tab5_sumfail[FF_VOLUMES];\n\n'
        + body
        + """
/* tools/enable_exfat.sh (6035): report where a damaged entry set is,
 * and its name when that is known to be its own: only when the checksum
 * test failed, which load_xdir_checked() flags. It is the last test,
 * made after every name entry is in dirbuf. An earlier failure leaves
 * the previous entry's name there (a host test printed exactly that),
 * so it gets none. */
void __attribute__((weak)) ff_tab5_bad_entry (BYTE pdrv, LBA_t sect, UINT ofs,
                                              const char* name, int in_root)
{
	(void)pdrv; (void)sect; (void)ofs; (void)name; (void)in_root;
}

static FRESULT load_xdir (FF_DIR* dp)
{
	const LBA_t sect = dp->sect;
	const UINT ofs = (UINT)(dp->dir - dp->obj.fs->win);
	tab5_sumfail[dp->obj.fs->pdrv] = 0;
	FRESULT res = load_xdir_checked(dp);
	if (res == FR_INT_ERR) {
		const BYTE *d = dp->obj.fs->dirbuf;
		char name[3 * 64 + 1];
		UINT n = 0;
		if (tab5_sumfail[dp->obj.fs->pdrv]) {
			UINT len = d[XDIR_NumName], k;
			if (len > 64) len = 64;
			for (k = 0; k < len; k++) {		/* UTF-16 (BMP) to UTF-8 */
				const UINT e = 2 * SZDIRE + (k / 15) * SZDIRE + 2 + (k % 15) * 2;
				const WCHAR c = (WCHAR)(d[e] | d[e + 1] << 8);
				if (c < 0x80) {
					name[n++] = (c >= 0x20) ? (char)c : '?';
				} else if (c < 0x800) {
					name[n++] = (char)(0xC0 | (c >> 6));
					name[n++] = (char)(0x80 | (c & 0x3F));
				} else {
					name[n++] = (char)(0xE0 | (c >> 12));
					name[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
					name[n++] = (char)(0x80 | (c & 0x3F));
				}
			}
		}
		name[n] = 0;
		ff_tab5_bad_entry(dp->obj.fs->pdrv, sect, ofs, n ? name : NULL,
		                  dp->obj.sclust == 0 || dp->obj.sclust == (DWORD)dp->obj.fs->dirbase);
	}
	return res;
}
""" + text[end:])
open(path, 'w').write(text)
print('wrapped load_xdir() in ' + path)
PYEOF

echo "patched $CONF:"
grep -E '^#define (FF_FS_EXFAT|FF_LBA64|FF_USE_TRIM)' "$CONF" | sed 's/^/  /'
echo
echo "exFAT needs long file names, which sdkconfig.defaults already"
echo "enables (CONFIG_FATFS_LFN_HEAP)."
echo
# A sdkconfig generated before components/fatfs existed does not carry
# that component's Kconfig symbols, and the build then compiles the
# vendored ffconf.h against the stale config header. Symptom is
# "CONFIG_FATFS_USE_LABEL undeclared". Force a regeneration rather than
# leaving it to be remembered.
if [ "$CLEAN" = "1" ]; then
    rm -rf build sdkconfig
    echo "removed build/ and sdkconfig so Kconfig is regenerated"
    echo "(sdkconfig.defaults pins CONFIG_IDF_TARGET, so the target survives this)"
    echo
    echo "now run: idf.py build flash monitor"
fi
