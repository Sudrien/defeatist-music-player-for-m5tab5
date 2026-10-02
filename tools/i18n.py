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
    _in(lang, "Join")           6024: the web pages -- a lookup in the
    _pin(lang, "%d networks", n)    request's language; same YAML, but
                                drawn by a browser, so not held to Ark12
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
# 6024: the web pages' markers, with the request's language first.
_IN = re.compile(r'(?<![A-Za-z0-9_])(_in|_pin)\(\s*[^,()"]+,\s*((?:' + _STR + r'\s*)+)([,)])')
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


# 6025: remote.html, the browser remote. Its markup marks translatable
# text three ways, and its script one:
#   <span data-t>Queue</span>          the element's text is the key
#   <p data-k="Added to %s ...">       an explicit key, for text with
#                                      markup inside; data-arg fills %s
#   <input data-ta="placeholder" ...>  these attributes' values are keys
#   T("..."), P("...", n) in script    as _() and _p()
# Whitespace runs collapse to one space, as the browser renders them.
_HTML_T = re.compile(r'<(\w+)\b[^>]*\sdata-t(?=[\s>])[^>]*>([^<]*)</\1>')
_HTML_K = re.compile(r'\sdata-k="([^"]*)"')
_HTML_TA = re.compile(r'<[^>]*\sdata-ta="([^"]*)"[^>]*>')
_JS_T = re.compile(r'(?<![\w.$])([TP])\(\s*"((?:\\.|[^"\\\n])*)"')


def _ws(t):
    return " ".join(t.split())


def scan_html(path, name, singular, plural):
    import html as _html
    text = open(path, encoding="utf-8").read()
    def add(d, k):
        lst = d.setdefault(k, [])
        if name not in lst:
            lst.append(name)
    for m in _HTML_T.finditer(text):
        add(singular, _ws(_html.unescape(m.group(2))))
    for m in _HTML_K.finditer(text):
        add(singular, _ws(_html.unescape(m.group(1))))
    for m in _HTML_TA.finditer(text):
        tag = m.group(0)
        for attr in m.group(1).split():
            a = re.search(r'\s' + re.escape(attr) + r'="([^"]*)"', tag)
            if not a:
                sys.exit(f"i18n: {name}: data-ta names {attr} but the tag has none: {tag[:80]}")
            add(singular, _ws(_html.unescape(a.group(1))))
    for m in _JS_T.finditer(text):
        add(plural if m.group(1) == "P" else singular, json.loads('"' + m.group(2) + '"'))


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
        for m in list(_CALL.finditer(text)) + list(_IN.finditer(text)):
            found.add(m.start())
            kind = {"_in": "_", "_pin": "_p"}.get(m.group(1), m.group(1))
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
            if m.start() in found:
                continue
            # 6017: _("..." PRIu32 "...") -- a literal joined to a macro.
            # The compiler sees one string; the extractor cannot, and
            # would skip it without a word.
            if text[m.end():].lstrip().startswith('"'):
                line = text.count("\n", 0, m.start()) + 1
                bad.append(f"{name}:{line}: {m.group(1)}() of a literal joined to a "
                           "macro; spell the format out (%u, not PRIu32)")
                continue
            if m.group(1) == "N_":
                line = text.count("\n", 0, m.start()) + 1
                # Inside a string literal is not a call ("ab_(" in text).
                pre = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
                if pre.count('"') % 2:
                    continue
                bad.append(f"{name}:{line}: N_() of something that is not a "
                           "string literal")
    scan_html(os.path.join(SRC_DIR, "remote.html"), "remote.html", singular, plural)  # 6025
    if bad:
        sys.exit("i18n: the extractor cannot read these:\n  " + "\n  ".join(bad))
    # 6021: allowed, and only said. The same English can be a console
    # name in one place (same()) and a screen word in another (_()):
    # "off" is both. Lookup is by content, so neither use affects the
    # other; extract names them so a screen word marked same() by
    # mistake is still seen.
    scan_sources.both = sorted(k for k in same if k in singular or k in plural)
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
    if write and scan_sources.both:
        print("i18n: same() in one place, translated in another: "
              + ", ".join(repr(k) for k in scan_sources.both))
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


# 6024: files whose strings a browser draws, not gfx.c. Their keys are
# marked off-screen in i18n_tab.c, so i18ntest holds only the rest to
# Ark12's glyphs -- and holds these to having no HTML in them instead.
WEB_FILES = {"portal.c", "remote.html", "remote.c"}
# 6025: the keys the browser remote needs, sent to it inside the page.
REMOTE_FILES = {"remote.html", "remote.c"}


def _on_screen(where):
    """A key is on screen unless every place it is used is a web page.
    One no longer in the source counts as on screen: the strict side."""
    return not where or any(w not in WEB_FILES for w in where)


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
    where_s, where_p = scan_sources()

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
    L.append("/* 6024: 1 when the key is drawn by the device, 0 when only a web page")
    L.append(" * uses it -- i18ntest checks Ark12's glyphs for the first kind only. */")
    L.append("const unsigned char i18n_on_screen[] = {")
    def _cmt(k):        # a key as a C comment: never closing or opening one
        return repr(k[:40]).replace("*/", "* /").replace("/*", "/ *")
    L += [f"    {1 if _on_screen(where_s.get(k)) else 0},  /* {_cmt(k)} */" for k in keys] or ["    1,"]
    L.append("};")
    L.append("const unsigned char i18n_pon_screen[] = {")
    L += [f"    {1 if _on_screen(where_p.get(k)) else 0}," for k in pkeys] or ["    1,"]
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


# ---------------------------------------------------------------- remote page

# 6028: the browser remote with every language in it. remote.html is the
# source, English as written; this writes main/remote_i18n.html, which is
# what main/CMakeLists.txt embeds and remote.c sends unchanged:
#   - each data-t / data-k element's text becomes a span per language,
#     <span data-l="ja">...</span>, and a stylesheet shows the span whose
#     data-l is html[lang] -- switching language is changing that one
#     attribute;
#   - at <!--i18n--> (in <head>), window.I18N: langs, and t / p for what
#     cannot hold spans (an <option>, attributes, the script's own text),
#     and a few lines choosing the first language -- kept in localStorage,
#     else navigator.languages -- before anything is drawn.
# Committed, like i18n_tab.c; `check` says when it is stale.
REMOTE_SRC = os.path.join(SRC_DIR, "remote.html")
REMOTE_OUT = os.path.join(SRC_DIR, "remote_i18n.html")
LANG_NAMES = {"en": "English", "zh-CN": "简体中文", "ja": "日本語", "es": "Español"}

_PICK_JS = (
    "(function(){var L=window.I18N.langs.map(function(l){return l[0]}),c='en';"
    "try{var k=localStorage.getItem('lang');if(L.indexOf(k)>=0)c=k;else throw 0}catch(e){"
    "var n=navigator.languages||[navigator.language||'en'];"
    "for(var i=0;i<n.length;i++){var t=String(n[i]).toLowerCase();"
    "if(/^zh(-|$)/.test(t)){if(/-(hant|tw|hk|mo)(-|$)/.test(t))continue;if(L.indexOf('zh-CN')>=0){c='zh-CN';break}continue}"
    "var p=t.split('-')[0];if(L.indexOf(p)>=0){c=p;break}}}"
    "document.documentElement.lang=c})();")


def build_remote_html(tabs, where_s, where_p):
    import html as _html
    src = open(REMOTE_SRC, encoding="utf-8").read()

    def val(loc, k):
        v = tabs[loc][0].get(k) or tabs["en"][0].get(k)
        return v if v else k

    def spans(texts):
        return "".join(f'<span data-l="{loc}">{t}</span>' for loc, t in zip(LOCALES, texts))

    def fill(fmt, arg):         # %s, the only conversion data-k keys use
        return fmt.replace("%s", arg, 1)

    def t_sub(m):
        if m.group(1).lower() == "option":
            return m.group(0)                       # the script sets these
        k = _ws(_html.unescape(m.group(3)))
        return m.group(0)[:m.end(2) - m.start(0) + 1] + \
            spans([_html.escape(val(loc, k), quote=False) for loc in LOCALES]) + f"</{m.group(1)}>"

    out = re.sub(r'<(\w+)\b([^>]*\sdata-t(?=[\s>])[^>]*)>([^<]*)</\1>', t_sub, src)

    def k_sub(m):
        tag, attrs = m.group(1), m.group(2)
        k = _ws(_html.unescape(re.search(r'\sdata-k="([^"]*)"', attrs).group(1)))
        a = re.search(r'\sdata-arg="([^"]*)"', attrs)
        arg = _html.unescape(a.group(1)) if a else ""
        return f"<{tag}{attrs}>" + spans([fill(_html.escape(val(loc, k), quote=False), arg)
                                          for loc in LOCALES]) + f"</{tag}>"

    out = re.sub(r'<(\w+)\b([^>]*\sdata-k="[^"]*"[^>]*)>(.*?)</\1>', k_sub, out, flags=re.S)

    t = {}
    for k, w in sorted(where_s.items()):
        if "remote.html" in w or "remote.c" in w:
            t[k] = [None if val(loc, k) == k else val(loc, k) for loc in LOCALES]
    p = {}
    for k, w in sorted(where_p.items()):
        if "remote.html" in w or "remote.c" in w:
            row = []
            for loc in LOCALES:
                f, e = tabs[loc][1].get(k, {}), tabs["en"][1].get(k, {})
                one = f.get("one") or e.get("one") or f.get("other") or k
                other = f.get("other") or e.get("other") or k
                row.append([one, other])
            p[k] = row
    blob = json.dumps({"langs": [[l, LANG_NAMES[l]] for l in LOCALES], "t": t, "p": p},
                      ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    css = "[data-l]{display:none}" + ",".join(
        f'html[lang="{l}"] [data-l="{l}"]' for l in LOCALES) + "{display:inline}"
    head = (f"<style>{css}</style><script>window.I18N={blob};{_PICK_JS}</script>")
    if out.count("<!--i18n-->") != 1:
        sys.exit("i18n: remote.html needs exactly one <!--i18n--> in its <head>")
    out = out.replace("<!--i18n-->", head)
    return ("<!-- GENERATED by ./tools/i18n.py compile from remote.html; do not edit. -->\n"
            + out)


def _remote_inputs():
    tabs = {loc: read_yml(yml_path(loc), loc) for loc in LOCALES}
    where_s, where_p = scan_sources()
    return build_remote_html(tabs, where_s, where_p)


def cmd_compile(args):
    text, report = build_c()
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write(text)
    with open(REMOTE_OUT, "w", encoding="utf-8") as f:            # 6028
        f.write(_remote_inputs())
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
    old = open(REMOTE_OUT, encoding="utf-8").read() if os.path.exists(REMOTE_OUT) else None
    if _remote_inputs() != old:
        sys.exit("i18n: main/remote_i18n.html is out of date (run compile)")
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
