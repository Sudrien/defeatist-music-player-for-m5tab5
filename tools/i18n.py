#!/usr/bin/env python3
"""
i18n.py -- the screen's translations, lv_i18n style: strings are marked
in the C source, extracted into one YAML file per locale, and compiled
into main/i18n_tab.c.

    ./tools/i18n.py extract     # main/*.c -> i18n/*.yml (adds new, keeps old)
    ./tools/i18n.py compile     # i18n/*.yml -> main/i18n_tab.c
    ./tools/i18n.py check       # fails if either step would change a file

THE MARKERS (main/i18n.h):

    _("Same album")             translated where it is used
    N_("Same album")            only marked -- for a static initialiser,
                                which cannot call a function; pass the
                                pointer through _() where it is drawn
    _p("%d tracks", n)          plural; the YAML holds one/other
    same("NET")                 deliberately English: a landmark, a name,
                                a unit. Not extracted; marks a decision

The key is the English text itself, so an unmarked or untranslated
string costs nothing and draws as English: _() of a string the table
does not hold returns its argument. That is what lets a screen be
converted a section at a time.

THE YAML is a fixed subset, so this needs nothing but the standard
library, the same bargain gen_ark12.py makes. Keys and values are
double-quoted with JSON escapes, which YAML's double-quoted scalars
accept, or ~ for "not translated yet":

    zh-CN:
      singular:
        "Same album": "同一专辑"
        "Record from": ~
      plural:
        "%d tracks":
          one: ~
          other: "%d 首"

An English file exists too, with every value ~ unless the English wants
plural forms or a different wording from the key. A ~ falls back to the
key on the board; `compile` reports how many there are per locale.

WHAT COMPILE REFUSES: a translation whose printf conversions differ from
its key's -- "%d s" translated as "%s 秒" would hand an int to %s on the
board. Count, order and conversion letter must match.

WHY COMMITTED: main/i18n_tab.c goes into git, like casefold_tab.c, so a
build never needs Python beyond what ESP-IDF already has, and a change to
the table shows up in review as the C it becomes. `check` says when it is
stale.

SPDX-License-Identifier: MIT
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "main")
YML_DIR = os.path.join(ROOT, "i18n")
OUT_C = os.path.join(SRC_DIR, "i18n_tab.c")

# Order is the i18n_lang_t order in main/i18n.h, and index 0 is the
# fallback. Adding a locale is a line here, an enum entry there, and a
# YAML file -- `extract` creates the file.
LOCALES = ["en", "zh-CN", "ja", "es"]

# Plural categories per locale (CLDR). Chinese and Japanese have one
# form; English and Spanish two. (CLDR gives Spanish a third, "many", for
# exact millions -- "1 000 000 de pistas" -- which no count here reaches.) i18n.c's rule function has to agree with this.
PLURAL_FORMS = {"en": ["one", "other"], "zh-CN": ["other"], "ja": ["other"],
                "es": ["one", "other"]}


# ---------------------------------------------------------------- C side

# Comments become spaces and string/char literals pass through, so a
# _("x") in a comment is not extracted and a "//" inside a string is not
# a comment.
_LEX = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  re.S)
_STR = r'"(?:\\.|[^"\\\n])*"'
_CALL = re.compile(r'(?<![A-Za-z0-9_])(_p|N_|_)\(\s*((?:' + _STR + r'\s*)+)([,)])')
_ANY = re.compile(r'(?<![A-Za-z0-9_])(_p|N_|_)\(')
_SAME = re.compile(r'(?<![A-Za-z0-9_])same\(\s*((?:' + _STR + r'\s*)+)\)')

_C_ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'",
          "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def _strip_comments(text):
    def keep(m):
        s = m.group(0)
        if s.startswith("/"):
            return re.sub(r"[^\n]", " ", s)       # keep line numbers
        return s
    return _LEX.sub(keep, text)


def _c_literal(lit):
    """One C string literal (with quotes) -> str. UTF-8 source assumed;
    \\x and octal escapes are bytes, which is what the compiler does."""
    body = lit[1:-1].encode("utf-8")
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != 0x5C:
            out.append(c)
            i += 1
            continue
        e = chr(body[i + 1])
        if e == "x":
            j = i + 2
            while j < len(body) and chr(body[j]) in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 2:j], 16) & 0xFF)
            i = j
        elif e in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and chr(body[j]) in "01234567":
                j += 1
            out.append(int(body[i + 1:j], 8) & 0xFF)
            i = j
        else:
            if e not in _C_ESC:
                raise ValueError(f"unknown escape \\{e}")
            out += _C_ESC[e].encode()
            i += 2
    return out.decode("utf-8")


def scan_sources():
    """-> ({msgid: [where]}, {plural msgid: [where]}). Exits on a marker
    whose argument is not a literal: the extractor cannot see it, so it
    would silently never be translated."""
    singular, plural, bad, same = {}, {}, [], {}
    for name in sorted(os.listdir(SRC_DIR)):
        if not name.endswith((".c", ".h")) or name in ("i18n_tab.c", "i18n.h"):
            continue
        path = os.path.join(SRC_DIR, name)
        with open(path, encoding="utf-8") as f:
            text = _strip_comments(f.read())
        found = set()
        for m in _SAME.finditer(text):
            msgid = "".join(_c_literal(s) for s in re.findall(_STR, m.group(1)))
            same.setdefault(msgid, name)
        for m in _CALL.finditer(text):
            found.add(m.start())
            kind = m.group(1)
            lits = re.findall(_STR, m.group(2))
            msgid = "".join(_c_literal(s) for s in lits)
            line = text.count("\n", 0, m.start()) + 1
            where = name                  # no line: it would churn the YAML
            if kind == "_p" and m.group(3) != ",":
                bad.append(f"{name}:{line}: _p() needs a count")
                continue
            lst = (plural if kind == "_p" else singular).setdefault(msgid, [])
            if where not in lst:
                lst.append(where)
        # _(ptr) and _p(ptr, n) are how an N_()-marked table entry is
        # drawn, so only N_() insists on a literal: N_(x) of anything
        # else marks nothing and is always a mistake.
        for m in _ANY.finditer(text):
            if m.group(1) == "N_" and m.start() not in found:
                line = text.count("\n", 0, m.start()) + 1
                # Inside a string literal is not a call ("ab_(" in text).
                pre = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
                if pre.count('"') % 2:
                    continue
                bad.append(f"{name}:{line}: N_() of something that is not a "
                           "string literal")
    if bad:
        sys.exit("i18n: the extractor cannot read these:\n  " + "\n  ".join(bad))
    both = sorted(k for k in same if k in singular or k in plural)
    if both:
        sys.exit("i18n: marked same() in one place and translated in another: "
                 + ", ".join(repr(k) for k in both))
    for k in plural:
        if k in singular:
            sys.exit(f"i18n: {k!r} is used both as _() and _p()")
    return singular, plural


# ---------------------------------------------------------------- YAML side

def _q(s):
    return json.dumps(s, ensure_ascii=False)


def _scalar(tok, where):
    tok = tok.strip()
    if tok == "~":
        return None
    if tok.startswith('"'):
        try:
            return json.loads(tok)
        except json.JSONDecodeError as e:
            sys.exit(f"{where}: {e}")
    sys.exit(f"{where}: expected a double-quoted string or ~, got {tok!r}")


_KEYLINE = re.compile(r'^(\s*)("(?:\\.|[^"\\])*"|[A-Za-z][A-Za-z-]*):(.*)$')


def read_yml(path, locale):
    """-> (singular {msgid: str|None}, plural {msgid: {form: str|None}})."""
    sing, plur = {}, {}
    section = None
    cur_plural = None
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            where = f"{os.path.relpath(path, ROOT)}:{n}"
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = _KEYLINE.match(line)
            if not m:
                sys.exit(f"{where}: cannot parse {line!r}")
            ind, key, rest = len(m.group(1)), m.group(2), m.group(3)
            if ind == 0:
                if key != locale:
                    sys.exit(f"{where}: top key is {key!r}, file is for {locale!r}")
            elif ind == 2:
                if key not in ("singular", "plural") or rest.strip():
                    sys.exit(f"{where}: expected singular: or plural:")
                section = key
            elif ind == 4:
                msgid = _scalar(key, where)
                if msgid is None:
                    sys.exit(f"{where}: a key cannot be ~")
                if section == "singular":
                    sing[msgid] = _scalar(rest, where)
                elif section == "plural":
                    if rest.strip():
                        sys.exit(f"{where}: a plural key holds one:/other: lines")
                    cur_plural = plur.setdefault(msgid, {})
                else:
                    sys.exit(f"{where}: entry outside singular:/plural:")
            elif ind == 6 and section == "plural" and cur_plural is not None:
                if key not in ("zero", "one", "two", "few", "many", "other"):
                    sys.exit(f"{where}: {key!r} is not a plural category")
                cur_plural[key] = _scalar(rest, where)
            else:
                sys.exit(f"{where}: unexpected indentation")
    return sing, plur


HEADER = """\
# {locale} -- the screen's text. Edit the values; the keys are the
# English in the source and are rewritten by ./tools/i18n.py extract.
# ~ means not translated: the board shows the key. Then run
# ./tools/i18n.py compile and commit main/i18n_tab.c with this file.
# Format: tools/i18n.py's docstring.
"""


def write_yml(path, locale, sing, plur, where_s, where_p):
    out = [HEADER.format(locale=locale), f"{locale}:", "  singular:"]
    for k in sorted(sing):
        if k in where_s:
            out.append(f"    # {', '.join(where_s[k][:3])}")
        else:
            out.append("    # not in the source any more")
        v = sing[k]
        out.append(f"    {_q(k)}: {'~' if v is None else _q(v)}")
    out.append("  plural:")
    for k in sorted(plur):
        if k not in where_p:
            out.append("    # not in the source any more")
        out.append(f"    {_q(k)}:")
        for form in PLURAL_FORMS[locale]:
            v = plur[k].get(form)
            out.append(f"      {form}: {'~' if v is None else _q(v)}")
    return "\n".join(out) + "\n"


def yml_path(locale):
    return os.path.join(YML_DIR, f"{locale}.yml")


# ---------------------------------------------------------------- commands

def cmd_extract(args, write=True):
    where_s, where_p = scan_sources()
    changed = []
    for loc in LOCALES:
        path = yml_path(loc)
        sing, plur = read_yml(path, loc) if os.path.exists(path) else ({}, {})
        for k in where_s:
            sing.setdefault(k, None)
        for k in where_p:
            plur.setdefault(k, {})
        gone = [k for k in sing if k not in where_s] + [k for k in plur if k not in where_p]
        if args.prune:
            for k in gone:
                sing.pop(k, None)
                plur.pop(k, None)
        elif gone and write:
            print(f"i18n: {loc}: {len(gone)} not in the source (kept; --prune drops them)")
        text = write_yml(path, loc, sing, plur, where_s, where_p)
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if text != old:
            changed.append(path)
            if write:
                os.makedirs(YML_DIR, exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    if write:
        print(f"i18n: {len(where_s)} strings, {len(where_p)} plurals; "
              f"{len(changed)} file(s) updated")
    return changed


# printf conversions, flags/width/precision/length included; %% is not one.
_FMT = re.compile(r"%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|j|z|t|L)?([diouxXeEfFgGaAcspn]))")


def _convs(s):
    return [m.group(0) for m in _FMT.finditer(s) if m.group(0) != "%%"]


def _conv_kind(c):
    # Width and flags may differ between languages; the argument type may not.
    m = re.match(r"%[-+ #0]*(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|j|z|t|L)?(.)", c)
    star = (m.group(1) == "*", m.group(2) == "*")
    return (star, m.group(3) or "", m.group(4))


def _c_str(s):
    # UTF-8 as itself, so the table reads in review; only the characters
    # a C literal cannot hold raw are escaped (octal: no hex run-on).
    out = []
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\{ord(ch):03o}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def build_c():
    tabs = {loc: read_yml(yml_path(loc), loc) for loc in LOCALES
            if os.path.exists(yml_path(loc))}
    missing = [loc for loc in LOCALES if loc not in tabs]
    if missing:
        sys.exit(f"i18n: no YAML for {', '.join(missing)}; run extract first")
    keys = sorted(set().union(*(t[0] for t in tabs.values())),
                  key=lambda s: s.encode("utf-8"))     # strcmp order
    pkeys = sorted(set().union(*(t[1] for t in tabs.values())),
                   key=lambda s: s.encode("utf-8"))
    errs, report = [], []

    def checked(loc, key, val):
        if val is None:
            return None
        if [_conv_kind(c) for c in _convs(key)] != [_conv_kind(c) for c in _convs(val)]:
            errs.append(f"{loc}: {key!r} -> {val!r}: printf conversions differ")
        return val

    L = ['/*', ' * i18n_tab.c -- GENERATED by ./tools/i18n.py compile from i18n/<locale>.yml.',
         ' * Do not edit; edit the YAML. Sorted by strcmp() for i18n.c\'s bsearch.',
         ' * NULL is "not translated": i18n_get() hands back the key.',
         ' *', ' * SPDX-License-Identifier: MIT', ' */', '',
         '#include <stddef.h>', '#include "i18n.h"', '']
    L.append(f"const unsigned i18n_count = {len(keys)};")
    L.append("const char *const i18n_keys[] = {")
    L += [f"    {_c_str(k)}," for k in keys] or ["    NULL,    /* C has no empty arrays */"]
    L.append("};")
    L.append("")
    L.append("/* [lang * i18n_count + key] */")
    L.append("const char *const i18n_vals[] = {")
    for loc in LOCALES:
        sing = tabs[loc][0]
        vals = [checked(loc, k, sing.get(k)) for k in keys]
        n_miss = sum(v is None for v in vals)
        if loc != LOCALES[0]:
            report.append(f"{loc}: {len(keys) - n_miss}/{len(keys)} translated")
        L.append(f"    /* {loc} */")
        L += [f"    {'NULL' if v is None else _c_str(v)}," for v in vals]
    L.append("};")
    L.append("")
    L.append(f"const unsigned i18n_pcount = {len(pkeys)};")
    L.append("const char *const i18n_pkeys[] = {")
    L += [f"    {_c_str(k)}," for k in pkeys] or ["    NULL,    /* C has no empty arrays */"]
    L.append("};")
    L.append("")
    L.append("/* [(lang * i18n_pcount + key) * 2 + form], form 0 one, 1 other -- the two")
    L.append(" * categories en, zh, ja and es use between them. */")
    L.append("const char *const i18n_pvals[] = {")
    for loc in LOCALES:
        plur = tabs[loc][1]
        L.append(f"    /* {loc} */")
        for k in pkeys:
            forms = plur.get(k, {})
            extra = set(forms) - set(PLURAL_FORMS[loc])
            if extra:
                errs.append(f"{loc}: {k!r}: {loc} has no {', '.join(sorted(extra))} form")
            one = checked(loc, k, forms.get("one"))
            other = checked(loc, k, forms.get("other"))
            L.append(f"    {'NULL' if one is None else _c_str(one)}, "
                     f"{'NULL' if other is None else _c_str(other)},")
    if not pkeys:
        L.append("    NULL,                      /* C has no empty arrays */")
    L.append("};")
    if errs:
        sys.exit("i18n: compile refused:\n  " + "\n  ".join(errs))
    return "\n".join(L) + "\n", report


def cmd_compile(args):
    text, report = build_c()
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write(text)
    print("i18n: wrote main/i18n_tab.c; " + "; ".join(report))


def cmd_check(args):
    args.prune = False
    stale = cmd_extract(args, write=False)
    if stale:
        sys.exit("i18n: YAML out of date with the source (run extract): "
                 + ", ".join(os.path.relpath(p, ROOT) for p in stale))
    text, report = build_c()
    old = open(OUT_C, encoding="utf-8").read() if os.path.exists(OUT_C) else None
    if text != old:
        sys.exit("i18n: main/i18n_tab.c is out of date (run compile)")
    print("i18n: up to date; " + "; ".join(report))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract")
    e.add_argument("--prune", action="store_true",
                   help="drop strings the source no longer uses")
    sub.add_parser("compile")
    sub.add_parser("check")
    args = p.parse_args()
    {"extract": cmd_extract, "compile": cmd_compile, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    main()
