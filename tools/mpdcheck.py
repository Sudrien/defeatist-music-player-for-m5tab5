#!/usr/bin/env python3
"""
mpdcheck.py -- drive the player's MPD server from a PC and check every
verb it answers, the way a client would use it.

WHY THIS EXISTS

The MPD server (main/mpd.c, MPD.md) grew one board report at a time, and
most of its verbs were only ever tried by hand, from Cantata or mpc, once.
MPD.md's "Where it stands" lists what has never been on the board at all.
This is a single run that exercises every verb `commands` lists, checks
each answer against what MPD's protocol documentation says it should be,
and says pass or fail per check, so a build can be tested in a minute
rather than an afternoon of clicking.

It checks MPD's behaviour, and where this player deliberately differs
(MPD.md, ARCHITECTURE.md) it checks the player's: `stop` is a pause,
there is one output that cannot be turned off, one partition, crossfade
only at 0, stored-playlist positions as `listplaylist` counts them.

WHAT IT CHANGES, AND PUTS BACK (5226)

By default it works on things it made itself:

  - the queue: three test entries are appended after yours, every edit
    is made on those, and they are deleted at the end. Your entries keep
    their order and their song ids, and the run checks that they did;
  - playback: the test entries are played, which interrupts what was
    playing. Afterwards the song you had is played again by its id,
    sought to where it was (to about 1% of the track -- the player seeks
    by percent) and paused if it was paused. A station that was playing
    is not queued and is not restarted;
  - volume: moved one step and back. Modes and replay gain: set to what
    they already are. Each may be written to the card's settings;
  - stored playlists: only its own, named "__mpdcheck*". If any exist
    already (an interrupted run), that section is skipped rather than
    deleting them.

`--destructive` adds what cannot be scoped: `clear` and `shuffle` act
on the whole queue, so the queue is saved as "__mpdcheck_saved" first
and loaded back (new ids, same order), and the volume, modes and
replay gain are cycled through their values. Nothing else runs
`clear` or `shuffle` (5235: not even on a queue that looks empty).

If a run is interrupted, the test entries are the ones from your
queue's old length on (`delete N:`), and with --destructive the queue
is in "__mpdcheck_saved". `--read-only` runs only the checks that
change nothing. `update` and `rescan` start a reindex and are only
sent with --reindex.

The library needs at least three playable files, indexed. Tags are
checked only where the catalog has them.

USAGE

    ./tools/mpdcheck.py 192.168.1.50
    ./tools/mpdcheck.py 192.168.1.50 --read-only
    ./tools/mpdcheck.py 192.168.1.50 --destructive
    ./tools/mpdcheck.py 192.168.1.50 --volume sd      # the SD even with a USB drive in
    ./tools/mpdcheck.py 192.168.1.50 --port 6600 -v

Exit status is the number of failed checks (0 is a clean run), capped
at 100. Standard library only.

SPDX-License-Identifier: MIT
"""

import argparse
import re
import socket
import sys
import time

ACK_RE = re.compile(r"^ACK \[(\d+)@(\d+)\] \{([^}]*)\} (.*)$")

# The player's own names and prefixes.
SAVED = "__mpdcheck_saved"
PL_A = "__mpdcheck_a"
PL_B = "__mpdcheck_b"
PL_C = "__mpdcheck_c"
PL_R = "__mpdcheck_r"      # 5251: relative positions

# 5236: the id bit the player gives a window of one (mpd.c MPD_WINDOW_ID):
# a station, or the track still playing after a clear.
WINDOW_ID = 0x40000000

# Every verb the server answers as of 5224 -- `commands` must list each.
EXPECTED_COMMANDS = """
ping close commands notcommands tagtypes urlhandlers decoders status stats
currentsong clearerror play playid pause stop next previous seek seekid
seekcur setvol volume outputs replay_gain_mode replay_gain_status channels
idle repeat random single consume add addid delete deleteid move moveid
clear shuffle playlistinfo playlistid playlist plchanges plchangesposid
lsinfo listall listallinfo find search list count update rescan listplaylists
listplaylist listplaylistinfo load save rm playlistadd playlistdelete
listpartitions partition delpartition moveoutput listmounts listneighbors
getvol password crossfade enableoutput disableoutput toggleoutput swap
swapid findadd searchadd searchaddpl playlistfind playlistsearch
playlistclear playlistmove rename
listfiles subscribe unsubscribe readmessages sendmessage prio prioid rangeid
addtagid cleartagid readcomments mixrampdb mixrampdelay kill config sticker
albumart readpicture binarylimit
""".split()

# 5241: the version this script's checks are written against.
EXPECTED_VERSION = "0.23.3"


class Ack(Exception):
    def __init__(self, code, idx, cmd, msg):
        super().__init__(f"ACK [{code}@{idx}] {{{cmd}}} {msg}")
        self.code, self.idx, self.cmd, self.msg = code, idx, cmd, msg


class Conn:
    """One MPD connection. `cmd()` returns the answer's lines, OK not
    included, or raises Ack."""

    def __init__(self, host, port, timeout, verbose):
        self.verbose = verbose
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.f = self.s.makefile("rb")
        self.greeting = self._line()

    def _line(self):
        raw = self.f.readline()
        if not raw:
            raise ConnectionError("connection closed by the player")
        line = raw.decode("utf-8", "replace").rstrip("\n")
        if self.verbose > 1:
            print(f"      < {line}")
        return line

    def send(self, line):
        if self.verbose:
            print(f"      > {line}")
        self.s.sendall(line.encode("utf-8") + b"\n")

    def read_answer(self):
        out = []
        while True:
            line = self._line()
            if line == "OK":
                return out
            m = ACK_RE.match(line)
            if m:
                raise Ack(int(m.group(1)), int(m.group(2)), m.group(3), m.group(4))
            out.append(line)

    def cmd(self, line):
        self.send(line)
        return self.read_answer()

    def binary(self, line):
        """5240: a command answered with a binary chunk -- `size:`,
        `binary: n`, n bytes, a newline, OK. Returns (size, bytes); 5249:
        and the `type:` line, if any, in self.last_type."""
        self.send(line)
        size, data = None, b""
        self.last_type = None
        while True:
            l = self._line()
            m = ACK_RE.match(l)
            if m:
                raise Ack(int(m.group(1)), int(m.group(2)), m.group(3), m.group(4))
            if l.startswith("size: "):
                size = int(l[6:])
            elif l.startswith("type: "):
                self.last_type = l[6:]
            elif l.startswith("binary: "):
                n = int(l[8:])
                data = self.f.read(n)
                self.f.read(1)                          # the newline after it
            elif l == "OK":
                return size, data

    def close(self):
        try:
            self.send("close")
        except OSError:
            pass
        self.s.close()


def q(s):
    """An argument, quoted as MPD's tokeniser wants it."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def pairs(lines):
    """`key: value` lines into a list of (key, value)."""
    out = []
    for l in lines:
        k, sep, v = l.partition(": ")
        if sep:
            out.append((k, v))
    return out


def kv(lines):
    """`key: value` lines into a dict (the last of a repeated key wins)."""
    return dict(pairs(lines))


def songs(lines):
    """Song blocks, each a dict, split on `file:`."""
    out = []
    for k, v in pairs(lines):
        if k == "file":
            out.append({"file": v})
        elif out:
            out[-1][k] = v
    return out


def sq(s):
    """5239: a value inside a filter expression, single-quoted, with
    MPD's backslash escapes."""
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def up4(name):
    """5244: the first four characters of a name in upper case, for the
    case-folding checks -- a character at a time, and one whose upper case
    is more than one character (ß) left alone, since simple folding, the
    player's and MPD's, maps one to one."""
    return "".join(c.upper() if len(c.upper()) == 1 else c for c in name[:4])


def files_of(lines):
    return [v for k, v in pairs(lines) if k == "file"]


class Checker:
    def __init__(self, args):
        self.a = args
        self.passed = 0
        self.failed = []
        self.skipped = []
        self.c = None

    # ---- bookkeeping ----------------------------------------------------

    def ok(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            if self.a.verbose:
                print(f"  pass  {name}")
        else:
            self.failed.append(name)
            print(f"  FAIL  {name}" + (f": {detail}" if detail else ""))
        return cond

    def skip(self, name, why):
        self.skipped.append(name)
        print(f"  skip  {name}: {why}")

    def section(self, title):
        print(f"\n== {title}")

    def connect(self):
        return Conn(self.a.ip, self.a.port, self.a.timeout, self.a.verbose)

    def expect_ok(self, name, line, c=None):
        """Run a command that should succeed; its lines, or None on ACK."""
        try:
            r = (c or self.c).cmd(line)
            self.ok(name, True)
            return r
        except Ack as e:
            self.ok(name, False, str(e))
            return None

    def expect_ack(self, name, line, code, c=None):
        try:
            (c or self.c).cmd(line)
            self.ok(name, False, f"expected ACK {code}, got OK")
        except Ack as e:
            self.ok(name, e.code == code, f"expected ACK {code}, got {e}")

    def status(self):
        return kv(self.c.cmd("status"))

    def queue(self):
        return files_of(self.c.cmd("playlistinfo"))

    def ids(self):
        return [s.get("Id") for s in songs(self.c.cmd("playlistinfo"))]

    # ---- the checks ------------------------------------------------------

    def connection(self):
        self.section("connection and framing")
        self.ok("greeting is OK MPD <version>",
                re.match(r"^OK MPD \d+\.\d+\.\d+$", self.c.greeting), self.c.greeting)
        ver = self.c.greeting[7:].split(".")
        self.ok(f"greeting claims at least {EXPECTED_VERSION}",
                len(ver) == 3 and tuple(map(int, ver)) >= tuple(map(int, EXPECTED_VERSION.split("."))),
                self.c.greeting)
        self.expect_ok("ping", "ping")
        self.expect_ok("clearerror", "clearerror")
        self.expect_ack("unknown verb is ACK 5", "bogusverb", 5)
        self.expect_ack("arity: setvol with no argument is ACK 2", "setvol", 2)
        self.expect_ack("arity: ping with an argument is ACK 2", "ping x", 2)
        self.expect_ack("integer expected is ACK 2", "setvol abc", 2)
        # Quoting: a quoted, escaped argument parses (the partition does
        # not exist, so NO_EXIST rather than a tokeniser error).
        self.expect_ack('quoted argument with \\" parses', 'partition "a \\"b\\" c"', 50)

        # command lists
        self.c.send("command_list_begin")
        self.c.send("ping")
        self.c.send("status")
        self.c.send("command_list_end")
        try:
            r = self.c.read_answer()
            self.ok("command_list_begin/end", any(l.startswith("state:") for l in r), repr(r))
        except Ack as e:
            self.ok("command_list_begin/end", False, str(e))

        self.c.send("command_list_ok_begin")
        self.c.send("ping")
        self.c.send("ping")
        self.c.send("command_list_end")
        try:
            r = self.c.read_answer()
            self.ok("command_list_ok_begin gives list_OK per command",
                    r.count("list_OK") == 2, repr(r))
        except Ack as e:
            self.ok("command_list_ok_begin", False, str(e))

        self.c.send("command_list_begin")
        self.c.send("ping")
        self.c.send("bogusverb")
        self.c.send("ping")
        self.c.send("command_list_end")
        try:
            self.c.read_answer()
            self.ok("a failing list ACKs with its index", False, "got OK")
        except Ack as e:
            self.ok("a failing list ACKs with its index", e.idx == 1, str(e))

    def introspection(self):
        self.section("commands, tags, outputs, partitions, mounts")
        r = self.expect_ok("commands", "commands") or []
        listed = {v for k, v in pairs(r) if k == "command"}
        missing = [v for v in EXPECTED_COMMANDS if v not in listed]
        self.ok("commands lists every answered verb", not missing, " ".join(missing))
        nr = self.expect_ok("notcommands", "notcommands") or []
        both = listed & {v for k, v in pairs(nr) if k == "command"}
        self.ok("commands and notcommands do not overlap", not both, " ".join(sorted(both)))

        r = self.expect_ok("tagtypes", "tagtypes") or []
        tags = {v for k, v in pairs(r) if k == "tagtype"}
        self.ok("tagtypes has Artist, Album, Title", {"Artist", "Album", "Title"} <= tags, str(tags))
        r = self.expect_ok("urlhandlers", "urlhandlers") or []
        self.ok("urlhandlers has http:// and https://",
                {"http://", "https://"} <= {v for k, v in pairs(r) if k == "handler"}, repr(r))
        self.expect_ok("decoders", "decoders")
        self.expect_ok("channels", "channels")
        r = self.expect_ok("stats", "stats") or []
        self.ok("stats has uptime", "uptime" in kv(r), repr(r))

        r = self.expect_ok("outputs", "outputs") or []
        o = kv(r)
        self.ok("outputs: one, id 0, enabled", o.get("outputid") == "0" and o.get("outputenabled") == "1", repr(r))
        self.expect_ok("enableoutput 0", "enableoutput 0")
        self.expect_ack("disableoutput 0 is refused (ACK 5)", "disableoutput 0", 5)
        self.expect_ack("toggleoutput 0 is refused (ACK 5)", "toggleoutput 0", 5)
        # If either was taken after all, the output is back on before
        # anything else runs -- a server with no output pauses.
        self.c.cmd("enableoutput 0")
        self.expect_ack("enableoutput 7 is NO_EXIST", "enableoutput 7", 50)
        if o.get("outputname"):
            self.expect_ok("moveoutput to the output's own partition", f"moveoutput {q(o['outputname'])}")
        self.expect_ack("moveoutput unknown is NO_EXIST", 'moveoutput "no such output"', 50)

        r = self.expect_ok("listpartitions", "listpartitions") or []
        self.ok("one partition, default", kv(r).get("partition") == "default", repr(r))
        self.expect_ok("partition default", "partition default")
        self.expect_ack("partition other is NO_EXIST", "partition other", 50)
        self.expect_ack("newpartition is refused", "newpartition x", 5)
        self.expect_ack("delpartition default is ARG", "delpartition default", 2)
        self.expect_ack("delpartition other is NO_EXIST", "delpartition other", 50)

        self.expect_ok("listmounts", "listmounts")
        self.expect_ack("mount is refused", "mount x y", 5)
        self.expect_ack("unmount is refused", "unmount x", 5)
        self.expect_ok("listneighbors", "listneighbors")

        self.expect_ack("password is ACK 3", "password secret", 3)
        self.expect_ok("crossfade 0", "crossfade 0")
        self.expect_ack("crossfade 5 is refused", "crossfade 5", 5)
        self.expect_ack("crossfade -1 is ARG", "crossfade -1", 2)

    def state_reads(self):
        self.section("status and friends")
        st = kv(self.expect_ok("status", "status") or [])
        for key in ("volume", "repeat", "random", "single", "consume", "playlist",
                    "playlistlength", "state"):
            self.ok(f"status has {key}", key in st, repr(sorted(st)))
        self.ok("status state is play/pause/stop", st.get("state") in ("play", "pause", "stop"), st.get("state"))
        r = self.expect_ok("getvol", "getvol") or []
        self.ok("getvol matches status volume", kv(r).get("volume") == st.get("volume"),
                f"{r!r} vs {st.get('volume')}")
        self.expect_ok("currentsong", "currentsong")
        r = self.expect_ok("replay_gain_status", "replay_gain_status") or []
        self.ok("replay_gain_status is off or track",
                kv(r).get("replay_gain_mode") in ("off", "track"), repr(r))

    def idle(self):
        self.section("idle")
        self.c.send("idle")
        time.sleep(0.3)
        self.c.send("noidle")
        try:
            self.c.read_answer()
            self.ok("idle then noidle answers OK", True)
        except (Ack, OSError) as e:
            self.ok("idle then noidle answers OK", False, str(e))
        self.expect_ack("idle with an unknown event is ARG", "idle nonsense", 2)

    def idle_wakes(self, other_change, event, name):
        """Idle on a second connection while this one changes something."""
        try:
            w = self.connect()
        except OSError as e:
            self.ok(name, False, f"second connection: {e}")
            return
        try:
            w.send(f"idle {event}")
            time.sleep(0.3)
            other_change()
            w.s.settimeout(5)
            r = w.read_answer()
            self.ok(name, f"changed: {event}" in r, repr(r))
        except (Ack, OSError) as e:
            self.ok(name, False, str(e))
        finally:
            w.close()

    # ---- the library -------------------------------------------------------

    def find_files(self, want=3, max_dirs=60, pool=40):
        """A breadth-first walk of lsinfo for the files the tests use.

        5242: from the USB drive when the player lists one (--volume auto,
        the default), else the SD; --volume sd|usb chooses. Up to `pool`
        candidates are gathered and the tagged ones -- Title, Artist and
        Album all present -- put first, so the tag checks have something
        to find. The first file's folder is the one the folder checks use.
        """
        roots = [v for k, v in pairs(self.c.cmd("lsinfo")) if k == "directory"]
        vol = self.a.volume
        if vol == "auto":
            vol = "usb" if "usb" in roots else "sd" if "sd" in roots else ""
        elif vol not in roots:
            print(f"  note  --volume {vol}: the player lists no such volume ({roots}); "
                  "searching the whole library")
            vol = ""
        self.volume = vol
        found, dirs, seen = [], [vol], 0
        while dirs and len(found) < pool and seen < max_dirs:
            d = dirs.pop(0)
            seen += 1
            try:
                r = self.c.cmd("lsinfo " + q(d) if d else "lsinfo")
            except Ack:
                continue
            for k, v in pairs(r):
                if k == "directory":
                    dirs.append(v)
            for song in songs(r):
                if not song["file"].startswith(("http://", "https://")):
                    found.append(song)
        tagged = [x for x in found if x.get("Title") and x.get("Artist") and x.get("Album")]
        rest = [x for x in found if x not in tagged]
        chosen = (tagged + rest)[:want]
        if chosen:
            print(f"  info  test files from {vol or 'the library'}"
                  f"{'' if not tagged else f', {len(tagged)} tagged of {len(found)} looked at'}:")
            for x in chosen:
                print(f"        {x['file']}")
        return [x["file"] for x in chosen]

    def library(self):
        self.section("library")
        r = self.expect_ok("lsinfo /", "lsinfo") or []
        top = [v for k, v in pairs(r) if k == "directory"]
        self.ok("lsinfo / lists a volume folder", bool(top), repr(r[:6]))
        self.expect_ack("lsinfo of a missing folder is NO_EXIST", 'lsinfo "sd/__no_such_folder__"', 50)
        self.files = self.find_files()
        if not self.ok("library has at least three files", len(self.files) >= 3, repr(self.files)):
            return False
        f0 = self.files[0]
        folder = f0.rsplit("/", 1)[0]
        r = self.expect_ok("listall <folder>", f"listall {q(folder)}") or []
        self.ok("listall lists the file", f0 in files_of(r), f0)
        r = self.expect_ok("listallinfo <folder>", f"listallinfo {q(folder)}") or []
        self.ok("listallinfo lists the file", f0 in files_of(r), f0)

        r = self.expect_ok("find file <uri>", f"find file {q(f0)}") or []
        hits = songs(r)
        self.ok("find file gives exactly that file", [s["file"] for s in hits] == [f0], repr(r))
        self.tags = hits[0] if hits else {"file": f0}

        base = f0.rsplit("/", 1)[1]
        r = self.expect_ok("search file <part of name>", f"search file {q(base[:max(3, len(base) // 2)])}") or []
        self.ok("search file finds it", f0 in files_of(r))
        r = self.expect_ok("search file <PART OF NAME, upper case>", f"search file {q(up4(base))}") or []
        self.ok("search folds case", f0 in files_of(r))
        r = self.expect_ok("find base <folder>", f"find base {q(folder)}") or []
        self.ok("find base finds it", f0 in files_of(r))
        r = self.expect_ok("search with window 0:1", f"search file {q(base[:3])} window 0:1") or []
        self.ok("window 0:1 gives at most one", len(files_of(r)) <= 1, repr(r))
        r = self.expect_ok("count file <uri>", f"count file {q(f0)}") or []
        self.ok("count gives songs: 1", kv(r).get("songs") == "1", repr(r))
        self.expect_ok("find genre x (a tag not held) is empty OK", 'find genre "x"')
        self.expect_ack("find with an odd argument count is ARG", "find artist", 2)
        # 5239: MPD 0.21's filter expressions. The protocol quotes the
        # whole expression; q() does that, and the expression quotes its
        # own value with single quotes.
        def fx(e):
            return q(e)
        r = self.expect_ok("find (file == uri)", f"find {fx(f'(file == {sq(f0)})')}") or []
        self.ok("expression find gives exactly that file", files_of(r) == [f0], repr(r))
        r = self.expect_ok("search (file contains PART) folds case",
                           f"search {fx(f'(file contains {sq(up4(base))})')}") or []
        self.ok("expression search finds it", f0 in files_of(r))
        r = self.expect_ok("find (file contains PART) is exact",
                           f"find {fx(f'(file contains {sq(up4(base))})')}") or []
        self.ok("expression find does not fold case",
                up4(base) == base[:4] or f0 not in files_of(r), repr(r[:4]))
        r = self.expect_ok("find (base folder)", f"find {fx(f'(base {sq(folder)})')}") or []
        self.ok("expression base finds it", f0 in files_of(r))
        r = self.expect_ok("find ((base) AND (!(file == uri)))",
                           f"find {fx(f'((base {sq(folder)}) AND (!(file == {sq(f0)})))')}") or []
        self.ok("AND and NOT leave the file out", f0 not in files_of(r) and len(files_of(r)) >= 0)
        r = self.expect_ok("count (base folder)", f"count {fx(f'(base {sq(folder)})')}") or []
        want = kv(self.c.cmd(f"count base {q(folder)}")).get("songs")
        self.ok("expression count matches the pair form", kv(r).get("songs") == want, repr(r))
        r = self.expect_ok("find (genre != x) with window 0:1",
                           "find " + fx("(genre != 'x')") + " window 0:1") or []
        self.ok("window pages an expression's answer", len(files_of(r)) <= 1, repr(r))
        self.expect_ok("list artist (base folder)", f"list artist {fx(f'(base {sq(folder)})')}")
        self.expect_ack("expression syntax error is ARG", "find " + fx("(artist == 'x'"), 2)
        self.expect_ack("unknown operator is ARG", "find " + fx("(artist like 'x')"), 2)
        # The player refuses these, with a reason; stock MPD has them.
        self.expect_ack("a regex is refused (not supported)", "find " + fx("(artist =~ 'x')"), 5)
        self.expect_ack("modified-since is refused (not supported)",
                        "find " + fx("(modified-since '2020-01-01T00:00:00Z')"), 5)

        for t in ("artist", "album", "title"):
            if self.tags.get(t.capitalize()):
                r = self.expect_ok(f"find {t} <its {t}>", f"find {t} {q(self.tags[t.capitalize()])}") or []
                self.ok(f"find {t} finds it", f0 in files_of(r))
            else:
                self.skip(f"find {t}", f"the file has no {t} tag")
        r = self.expect_ok("list artist", "list artist") or []
        if self.tags.get("Artist"):
            self.ok("list artist has the file's artist", self.tags["Artist"] in [v for k, v in pairs(r)])
        self.expect_ok("list album", "list album")
        self.expect_ok("list album group albumartist", "list album group albumartist")
        self.expect_ok("list genre (not held) is empty OK", "list genre")

        if self.a.reindex:
            # 5236: a job id, and the run waited out -- while it runs the
            # library answers "being indexed", and every check after
            # this one reads it.
            # 5242: and the volume the test files came from, by name --
            # the path that reindexes one volume rather than both.
            verbs = ["update", "rescan"] + ([f"update {self.volume}"] if self.volume else [])
            for verb in verbs:
                r = self.expect_ok(verb, verb) or []
                self.ok(f"{verb} answers updating_db: N", "updating_db" in kv(r), repr(r))
                # 5237: done is status without updating_db AND the library
                # answering -- the board's first status after update said
                # no run (a stale snapshot, fixed in 5237) and the checks
                # after it met "being indexed". Either alone can be early.
                t, done = time.time(), False
                while not done and time.time() - t < 60:
                    time.sleep(0.5)
                    if "updating_db" in self.status():
                        continue
                    try:
                        self.c.cmd("count base " + q(self.files[0].rsplit("/", 1)[0]))
                        done = True
                    except Ack as e:
                        if e.code != 52:
                            done = True
                self.ok(f"the {verb} run finishes", done)
        else:
            self.skip("update/rescan", "starts a reindex; pass --reindex")
        return True

    # ---- snapshot and restore ----------------------------------------------

    # 5226: the queue is not saved and cleared any more. The tests append
    # their own entries after the listener's, work only on those, and
    # delete them; the listener's entries keep their order and their ids.
    # Only --destructive (clear, shuffle, full-range volume and modes)
    # still saves the queue to SAVED and reloads it.

    def snapshot(self):
        st = self.status()
        self.saved_status = st
        self.orig_files = self.queue()
        # 5235: the list itself, not status's count of it -- a board run
        # had status say 0 over a loaded folder, and everything after
        # took the listener's entries for the test's.
        self.base = len(self.orig_files)
        self.consistent = int(st.get("playlistlength", "-1")) == self.base
        self.orig_ids = self.ids()
        self.saved_rg = kv(self.c.cmd("replay_gain_status")).get("replay_gain_mode", "off")
        self.queue_saved = False
        if self.a.destructive and self.base > 0:
            try:
                self.c.cmd(f"rm {q(SAVED)}")
            except Ack:
                pass
            try:
                self.c.cmd(f"save {q(SAVED)}")
                self.queue_saved = True
            except Ack as e:
                print(f"  note  could not save the queue ({e}); it will not be restored")

    def trim_tests(self):
        """Delete everything after the listener's own entries."""
        left = songs(self.c.cmd("playlistinfo"))
        n = len(left)
        # 5236: a window of one (the playing track after a clear, 5179) is
        # not queue positions and cannot be deleted; there is nothing of
        # the test's in it.
        if n == 1 and int(left[0].get("Id", "0")) & WINDOW_ID:
            return
        if n > self.base:
            try:
                self.c.cmd(f"delete {self.base}:")
            except Ack as e:
                print(f"  WARN  could not remove the test entries: {e}")

    def restore(self):
        self.section("putting things back")
        st = self.saved_status
        for pl in (PL_A, PL_B, PL_C, PL_R):
            try:
                self.c.cmd(f"rm {q(pl)}")
            except Ack:
                pass
        try:
            if self.queue_saved:
                self.c.cmd("clear")
                self.c.cmd(f"load {q(SAVED)}")
                self.c.cmd(f"rm {q(SAVED)}")
            else:
                self.trim_tests()
            for m in ("repeat", "random", "single", "consume"):
                self.c.cmd(f"{m} {st.get(m, '0')}")
            self.c.cmd(f"setvol {st.get('volume', '50')}")
            self.c.cmd(f"replay_gain_mode {self.saved_rg}")

            # What was playing, where it was: by id when the queue was not
            # reloaded (ids survive), by position when it was.
            if st.get("song") is not None:
                self.c.cmd(f"playid {st['songid']}" if not self.queue_saved else f"play {st['song']}")
                el = float(st.get("elapsed", "0") or 0)
                if el > 1:
                    try:
                        self.c.cmd(f"seekcur {el:.1f}")
                    except Ack:
                        pass
                if st.get("state") != "play":
                    self.c.cmd("pause 1")
            elif st.get("state") == "play":
                self.c.cmd("play")
            else:
                self.c.cmd("pause 1")
            print("  done  test entries removed; volume, modes, replay gain and the "
                  "playing song put back")
        except (Ack, OSError) as e:
            print(f"  WARN  restore incomplete: {e}")
            if self.queue_saved:
                print(f'        the queue is in the stored playlist "{SAVED}"')
            else:
                print(f"        entries from position {self.base} on are the test's; "
                      f"`delete {self.base}:` removes them")

        if not self.queue_saved:
            try:
                self.ok("the listener's entries are untouched (files)",
                        self.queue() == self.orig_files)
                self.ok("the listener's entries are untouched (ids)",
                        self.ids() == self.orig_ids)
            except (Ack, OSError) as e:
                self.ok("the listener's entries are untouched", False, str(e))

    # ---- the queue -----------------------------------------------------------

    def tail(self):
        return self.queue()[self.base:]

    def tail_ids(self):
        return self.ids()[self.base:]

    def refill(self):
        self.trim_tests()
        for i in range(3):
            self.c.cmd(f"add {q(self.files[i])}")

    def queue_edits(self):
        self.section("queue (on appended entries; yours are not touched)")
        f, b = self.files, self.base
        for i in range(3):
            self.expect_ok(f"add file {i}", f"add {q(f[i])}")
        self.ok("add appends in order", self.tail() == f[:3], repr(self.tail()))
        self.expect_ack("add of a missing file is NO_EXIST", 'add "sd/__no_such_file__.mp3"', 50)

        r = self.expect_ok("addid at a position", f"addid {q(f[2])} {b}") or []
        new_id = kv(r).get("Id")
        self.ok("addid answers Id", new_id is not None, repr(r))
        self.ok("addid lands at that position", self.tail()[:1] == [f[2]], repr(self.tail()))

        st = self.status()
        self.ok("status playlistlength follows", st.get("playlistlength") == str(b + 4), st.get("playlistlength"))
        v0 = st.get("playlist")

        r = self.expect_ok("playlistid <id>", f"playlistid {new_id}") or []
        self.ok("playlistid gives that entry", songs(r) and songs(r)[0].get("Id") == new_id, repr(r))
        self.expect_ack("playlistid of a missing id is NO_EXIST", "playlistid 999999", 50)
        r = self.expect_ok("playlistinfo range", f"playlistinfo {b + 1}:{b + 3}") or []
        self.ok("playlistinfo range gives two", len(songs(r)) == 2, repr(r))
        r = self.expect_ok("playlist", "playlist") or []
        self.ok("playlist lines are N:file: uri",
                all(re.match(r"^\d+:file: ", l) for l in r) and len(r) == b + 4, f"{len(r)} lines")
        self.expect_ack("playlistinfo past the end is ARG", f"playlistinfo {b + 10}:{b + 12}", 2)

        self.expect_ok("deleteid", f"deleteid {new_id}")
        self.ok("deleteid removes it", self.tail() == f[:3], repr(self.tail()))
        r = self.expect_ok("plchanges <old version>", f"plchanges {v0}") or []
        self.ok("plchanges reports changed entries", len(songs(r)) >= 1, repr(r))
        self.expect_ok("plchangesposid <old version>", f"plchangesposid {v0}")

        self.expect_ok("move", f"move {b} {b + 2}")
        self.ok("move", self.tail() == [f[1], f[2], f[0]], repr(self.tail()))
        ids = self.tail_ids()
        self.expect_ok("moveid", f"moveid {ids[2]} {b}")
        self.ok("moveid", self.tail() == [f[0], f[1], f[2]], repr(self.tail()))
        self.expect_ok("move range", f"move {b}:{b + 2} {b + 1}")
        self.ok("move range", self.tail() == [f[2], f[0], f[1]], repr(self.tail()))
        self.expect_ack("move past the end is ARG", f"move {b} {b + 9}", 2)

        self.expect_ok("swap", f"swap {b} {b + 2}")
        self.ok("swap", self.tail() == [f[1], f[0], f[2]], repr(self.tail()))
        ids = self.tail_ids()
        self.expect_ok("swapid", f"swapid {ids[0]} {ids[1]}")
        self.ok("swapid", self.tail() == [f[0], f[1], f[2]], repr(self.tail()))
        self.ok("ids survive a swap", sorted(self.tail_ids()) == sorted(ids))
        self.expect_ack("swap past the end is ARG", f"swap {b} {b + 9}", 2)
        self.expect_ack("swapid of a missing id is NO_EXIST", f"swapid {ids[0]} 999999", 50)
        self.expect_ack("shuffle of a range is refused", f"shuffle {b}:{b + 2}", 5)

        # clear and shuffle act on the whole queue: only with nothing of
        # the listener's in it, or with --destructive (the queue saved).
        # 5235: only with --destructive. "The queue looks empty" is not
        # enough: a board run saw it empty when it was not, and cleared
        # the listener's folder.
        if self.a.destructive:
            before = sorted(self.ids())
            self.expect_ok("shuffle", "shuffle")
            self.ok("shuffle keeps the same entries and ids", sorted(self.ids()) == before)
            self.expect_ok("clear", "clear")
            # 5236: MPD stops on clear; this player plays on, and shows
            # what is still playing as a window of one (5179) -- an entry
            # whose id is a window id, and the current song. Either is
            # an emptied queue.
            left = songs(self.c.cmd("playlistinfo"))
            win = (len(left) == 1 and int(left[0].get("Id", "0")) & WINDOW_ID and
                   self.status().get("songid") == left[0].get("Id"))
            self.ok("clear empties the queue", left == [] or win, repr(left[:2]))
            self.base = 0 if self.a.destructive else self.base
        else:
            self.skip("clear, shuffle", "they act on your whole queue; pass --destructive")
        self.refill()

        r = self.expect_ok("playlistfind file <uri>", f"playlistfind file {q(f[1])}") or []
        self.ok("playlistfind finds it at its position",
                str(self.base + 1) in [s.get("Pos") for s in songs(r)], repr(r))
        base = f[1].rsplit("/", 1)[1]
        r = self.expect_ok("playlistsearch file <PART>", f"playlistsearch file {q(up4(base))}") or []
        self.ok("playlistsearch finds it", f[1] in files_of(r), repr(r))
        r = self.expect_ok("playlistfind (file == uri)",                     # 5239
                           f"playlistfind {q(f'(file == {sq(f[1])})')}") or []
        self.ok("expression playlistfind finds it at its position",
                str(self.base + 1) in [s.get("Pos") for s in songs(r)], repr(r))
        r = self.expect_ok("playlistsearch (file contains PART)",
                           f"playlistsearch {q(f'(file contains {sq(up4(base))})')}") or []
        self.ok("expression playlistsearch finds it", f[1] in files_of(r), repr(r))
        r = self.expect_ok("playlistfind genre x (not held)", 'playlistfind genre "x"') or []
        self.ok("playlistfind on an unheld tag is empty", r == [], repr(r))
        self.expect_ack("playlistfind odd arguments is ARG", "playlistfind file", 2)

        n = self.base + 3
        self.expect_ok("findadd file <uri>", f"findadd file {q(f[0])}")
        self.ok("findadd appends one", self.tail() == f[:3] + [f[0]], repr(self.tail()))
        self.expect_ok("searchadd file <exact name>", f"searchadd file {q(f[1])}")
        self.ok("searchadd appends at least one", len(self.queue()) >= n + 2)
        self.expect_ok("delete range to the end", f"delete {n}:")
        self.ok("delete N: trims the end", self.tail() == f[:3], repr(self.tail()))
        self.expect_ok("delete one", f"delete {self.base}")
        self.ok("delete one", self.tail() == f[1:3], repr(self.tail()))
        self.expect_ack("delete past the end is ARG", f"delete {self.base + 9}", 2)
        self.refill()

        self.idle_wakes(lambda: self.c.cmd(f"add {q(f[0])}"), "playlist",
                        "idle playlist wakes on add from another client")
        self.c.cmd(f"delete {self.base + 3}")

    # ---- the transport ---------------------------------------------------------

    def wait_state(self, want, secs=4.0):
        t = time.time()
        st = {}
        while time.time() - t < secs:
            st = self.status()
            if want(st):
                return st
            time.sleep(0.25)
        return st

    def transport(self):
        self.section("transport (plays the appended entries; interrupts what is playing)")
        b = self.base
        p0, p1, p2 = str(b), str(b + 1), str(b + 2)
        ids = self.tail_ids()
        self.expect_ok("play <pos>", f"play {b}")
        st = self.wait_state(lambda s: s.get("state") == "play" and s.get("song") == p0)
        self.ok("play <pos> plays it", st.get("state") == "play" and st.get("song") == p0,
                f"{st.get('state')} song {st.get('song')}")
        self.ok("status has songid", st.get("songid") == ids[0], st.get("songid"))
        r = self.expect_ok("currentsong while playing", "currentsong") or []
        self.ok("currentsong is it", kv(r).get("file") == self.files[0], repr(r))

        self.expect_ok("pause 1", "pause 1")
        st = self.wait_state(lambda s: s.get("state") == "pause")
        self.ok("pause 1 pauses", st.get("state") == "pause", st.get("state"))
        self.expect_ok("pause 0", "pause 0")
        st = self.wait_state(lambda s: s.get("state") == "play")
        self.ok("pause 0 resumes", st.get("state") == "play", st.get("state"))
        self.expect_ok("pause (toggle)", "pause")
        st = self.wait_state(lambda s: s.get("state") == "pause")
        self.ok("bare pause toggles", st.get("state") == "pause", st.get("state"))
        self.expect_ok("play (resume)", "play")
        self.wait_state(lambda s: s.get("state") == "play")

        # next follows the play order; with random or single on it may not
        # be the next position, so it is judged only when both are off.
        mode = self.status()
        self.expect_ok("next", "next")
        st = self.wait_state(lambda s: s.get("song") != p0)
        if mode.get("random") == "0" and mode.get("single") == "0":
            self.ok("next goes to the next position", st.get("song") == p1, st.get("song"))
        self.expect_ok("previous", "previous")

        self.expect_ok("playid <id>", f"playid {ids[2]}")
        st = self.wait_state(lambda s: s.get("song") == p2)
        self.ok("playid plays it", st.get("song") == p2, st.get("song"))
        self.expect_ack("playid of a missing id is NO_EXIST", "playid 999999", 50)
        self.expect_ack("play past the end is ARG", f"play {b + 99}", 2)

        time.sleep(1.0)
        st = self.status()
        if "duration" in st and float(st.get("duration", "0")) > 10:
            self.expect_ok("seekcur 5", "seekcur 5")
            st = self.wait_state(lambda s: 3 <= float(s.get("elapsed", "0")) <= 9)
            self.ok("seekcur lands near 5 s (a percent of the track)",
                    3 <= float(st.get("elapsed", "0")) <= 9, st.get("elapsed"))
            self.expect_ok("seekcur +2", "seekcur +2")
            self.expect_ok("seekid <current> 1", f"seekid {st.get('songid')} 1")
            self.expect_ok("seek <current pos> 1", f"seek {st.get('song')} 1")
            # 5229: another song is started there.
            self.expect_ok("seek of another song", f"seek {b} 2")
            st = self.wait_state(lambda s: s.get("song") == p0, 5.0)
            self.ok("seek of another song plays it", st.get("song") == p0, st.get("song"))
        else:
            self.skip("seek, seekid, seekcur", "the song has no duration over 10 s")

        self.expect_ok("stop", "stop")
        st = self.wait_state(lambda s: s.get("state") in ("pause", "stop"))
        self.ok("stop is a pause here", st.get("state") in ("pause", "stop"), st.get("state"))

    def relative(self):
        """5251: MPD 0.23's +N/-N positions, relative to the playing song,
        on the three test entries with the middle one playing (paused)."""
        self.section("0.23: positions relative to the playing song")
        f, b = self.files, self.base
        self.refill()
        A, B, C = f[0], f[1], f[2]
        ids = self.tail_ids()
        self.c.cmd(f"playid {ids[1]}")
        self.wait_state(lambda s: s.get("songid") == ids[1])
        self.c.cmd("pause 1")
        if self.status().get("songid") != ids[1]:
            self.skip("relative positions", "could not make a test entry the playing song")
            return

        def check(name, line, want):
            r = self.expect_ok(name, line)
            self.ok(f"{name}: order", self.tail() == want, f"{self.tail()} != {want}")
            return r

        r = check("addid X +0 (right after the playing song)", f"addid {q(A)} +0", [A, B, A, C])
        self.c.cmd(f"deleteid {kv(r or []).get('Id')}")
        r = check("addid X -0 (right before it)", f"addid {q(A)} -0", [A, A, B, C])
        self.ok("-0 moves the playing song along", self.status().get("song") == str(b + 2))
        self.c.cmd(f"deleteid {kv(r or []).get('Id')}")
        self.expect_ack("addid X +99 is ARG", f"addid {q(A)} +99", 2)

        check("move <before it> +0", f"move {b} +0", [B, A, C])
        self.c.cmd(f"move {b + 1} {b}")
        check("moveid <after it> -0", f"moveid {ids[2]} -0", [A, C, B])
        self.c.cmd(f"moveid {ids[2]} {b + 2}")
        self.ok("back as it was", self.tail() == [A, B, C], repr(self.tail()))

        check("findadd ... position +0", f"findadd file {q(C)} position +0", [A, B, C, C])
        self.c.cmd(f"delete {b + 2}")

        have = {v for k, v in pairs(self.c.cmd("listplaylists")) if k == "playlist"}
        if PL_R in have:
            self.skip("load/playlistadd at a position", f"{PL_R} already exists")
            return
        for x in (A, B, C):
            self.c.cmd(f"playlistadd {q(PL_R)} {q(x)}")
        check("load NAME 1:3 +0", f"load {q(PL_R)} 1:3 +0", [A, B, B, C, C])
        self.c.cmd(f"delete {b + 2}:{b + 4}")
        r = self.expect_ok("load NAME 9:10 0 (past its end) adds nothing", f"load {q(PL_R)} 9:10 0")
        self.ok("load past the end changed nothing", self.tail() == [A, B, C], repr(self.tail()))
        self.expect_ok("playlistadd NAME URI 1", f"playlistadd {q(PL_R)} {q(C)} 1")
        self.ok("playlistadd at a position", self.listed(PL_R) == [A, C, B, C], repr(self.listed(PL_R)))
        self.expect_ack("playlistadd past the end is ARG", f"playlistadd {q(PL_R)} {q(C)} 99", 2)
        self.c.cmd(f"rm {q(PL_R)}")

    def consume(self):
        """5252: consume is the EAT play order. Played on the three test
        entries at the end of the queue; the modes are put back after."""
        self.section("consume (the EAT play order)")
        f = self.files
        self.refill()
        A, B, C = f[0], f[1], f[2]
        ids = self.tail_ids()
        was = self.status()
        self.expect_ok("consume 1", "consume 1")
        st = self.status()
        self.ok("status says consume 1, nothing else",
                (st.get("consume"), st.get("repeat"), st.get("random"), st.get("single")) == ("1", "0", "0", "0"),
                f"consume {st.get('consume')} repeat {st.get('repeat')} random {st.get('random')} single {st.get('single')}")
        self.c.cmd("repeat 1")
        st = self.status()
        self.ok("repeat 1 with consume: repeat springs back, consume stays",
                st.get("consume") == "1" and st.get("repeat") == "0", f"{st.get('consume')} {st.get('repeat')}")
        self.c.cmd("repeat 0")

        self.c.cmd(f"playid {ids[0]}")
        self.wait_state(lambda s: s.get("songid") == ids[0])
        self.expect_ok("next under consume", "next")
        st = self.wait_state(lambda s: s.get("songid") == ids[1])
        self.ok("next plays what followed", st.get("songid") == ids[1], st.get("songid"))
        self.ok("next eats the one left", self.tail() == [B, C], repr(self.tail()))
        self.expect_ok("playid under consume", f"playid {ids[2]}")
        self.wait_state(lambda s: s.get("songid") == ids[2])
        self.ok("choosing another entry eats nothing", self.tail() == [B, C], repr(self.tail()))
        self.expect_ok("next from the last entry", "next")
        st = self.wait_state(lambda s: s.get("state") != "play" and self.tail() == [B])
        self.ok("the last entry is eaten", self.tail() == [B], repr(self.tail()))
        self.ok("and playback stops", st.get("state") != "play", st.get("state"))

        self.c.cmd("random 1")
        st = self.status()
        self.ok("random 1 drops consume (no shuffled eating)",
                st.get("random") == "1" and st.get("consume") == "0", f"{st.get('random')} {st.get('consume')}")
        for m in ("random", "consume"):
            self.c.cmd(f"{m} {was.get(m, '0')}")

    def volume_modes(self):
        self.section("volume, modes, replay gain")
        v = int(self.saved_status.get("volume", "50"))
        # One step, and back: the listener's volume barely moves. A full
        # jump only with --destructive.
        w = 30 if self.a.destructive else (v + 1 if v < 100 else v - 1)
        self.expect_ok(f"setvol {w}", f"setvol {w}")
        self.ok(f"getvol reads {w}", kv(self.c.cmd("getvol")).get("volume") == str(w))
        d = -1 if w > v else 1
        self.expect_ok(f"volume {d:+d} (deprecated, relative)", f"volume {d:+d}")
        self.ok(f"volume {d:+d} gives {w + d}", kv(self.c.cmd("getvol")).get("volume") == str(w + d))
        self.expect_ack("setvol 101 is ARG", "setvol 101", 2)
        self.idle_wakes(lambda: self.c.cmd(f"setvol {w}"), "mixer", "idle mixer wakes on setvol")
        self.c.cmd(f"setvol {v}")

        st = self.status()
        for m in ("repeat", "random", "single", "consume"):
            if self.a.destructive:
                self.expect_ok(f"{m} 1", f"{m} 1")
                self.ok(f"status has {m} after setting it", m in self.status())
                self.expect_ok(f"{m} 0", f"{m} 0")
            else:
                # Setting a flag to what it is changes nothing.
                self.expect_ok(f"{m} {st.get(m, '0')} (as it is)", f"{m} {st.get(m, '0')}")
            self.expect_ack(f"{m} 2 is ARG", f"{m} 2", 2)
        if self.a.destructive:
            self.c.cmd("random 1")
            s = self.status()
            print(f"  info  random 1 -> repeat {s.get('repeat')} random {s.get('random')} "
                  f"single {s.get('single')} consume {s.get('consume')}")
            self.c.cmd("random 0")

        # 5241: single oneshot shows as a word, and single 0/1 ends it.
        was = self.status().get("single", "0")
        self.expect_ok("single oneshot", "single oneshot")
        self.ok("status says single: oneshot", self.status().get("single") == "oneshot",
                self.status().get("single"))
        self.expect_ok("single back as it was", f"single {was}")
        self.ok("single oneshot ends on single 0/1", self.status().get("single") == was,
                self.status().get("single"))
        self.expect_ack("single 2 is still ARG", "single 2", 2)

        for m in (("track", "off") if self.a.destructive else (self.saved_rg,)):
            self.expect_ok(f"replay_gain_mode {m}", f"replay_gain_mode {m}")
            got = kv(self.c.cmd("replay_gain_status")).get("replay_gain_mode")
            self.ok(f"replay_gain_status reads {m}", got == m, got)
        self.expect_ack("replay_gain_mode nonsense is ARG", "replay_gain_mode nonsense", 2)

    # ---- stored playlists --------------------------------------------------------

    def listed(self, name):
        return [l.split(": ", 1)[1] for l in self.c.cmd(f"listplaylist {q(name)}") if l.startswith("file: ")]

    def stored(self):
        self.section("stored playlists (its own, named __mpdcheck*)")
        f = self.files
        have = {v for k, v in pairs(self.c.cmd("listplaylists")) if k == "playlist"}
        clash = [pl for pl in (PL_A, PL_B, PL_C) if pl in have]
        if clash:
            self.skip("stored playlists", f"{', '.join(clash)} already exist; "
                      "remove them by hand if they are left from an interrupted run")
            return
        self.refill()

        self.expect_ok("save", f"save {q(PL_A)}")
        self.ok("save writes the queue", self.listed(PL_A) == self.queue(), repr(self.listed(PL_A)[-3:]))
        self.expect_ack("save onto an existing name is EXIST", f"save {q(PL_A)}", 56)
        r = self.expect_ok("listplaylists", "listplaylists") or []
        self.ok("listplaylists has it", PL_A in [v for k, v in pairs(r) if k == "playlist"])
        r = self.expect_ok("listplaylistinfo", f"listplaylistinfo {q(PL_A)}") or []
        self.ok("listplaylistinfo gives the songs", len(songs(r)) == len(self.queue()), repr(r[-6:]))
        self.expect_ok("rm", f"rm {q(PL_A)}")
        self.expect_ack("rm of a missing playlist is NO_EXIST", f"rm {q(PL_A)}", 50)
        self.expect_ack("listplaylist of a missing playlist is NO_EXIST", f"listplaylist {q(PL_A)}", 50)

        for i in range(3):
            self.expect_ok(f"playlistadd {i}", f"playlistadd {q(PL_B)} {q(f[i])}")
        self.ok("playlistadd appends", self.listed(PL_B) == f[:3], repr(self.listed(PL_B)))
        self.expect_ok("playlistmove 0 2", f"playlistmove {q(PL_B)} 0 2")
        self.ok("playlistmove", self.listed(PL_B) == [f[1], f[2], f[0]], repr(self.listed(PL_B)))
        self.expect_ok("playlistdelete 1", f"playlistdelete {q(PL_B)} 1")
        self.ok("playlistdelete", self.listed(PL_B) == [f[1], f[0]], repr(self.listed(PL_B)))
        self.expect_ack("playlistdelete past the end is ARG", f"playlistdelete {q(PL_B)} 9", 2)
        self.expect_ack("playlistmove past the end is ARG", f"playlistmove {q(PL_B)} 0 9", 2)

        self.expect_ok("rename", f"rename {q(PL_B)} {q(PL_C)}")
        r = [v for k, v in pairs(self.c.cmd("listplaylists")) if k == "playlist"]
        self.ok("rename moves the name", PL_C in r and PL_B not in r, repr(r))
        self.expect_ack("rename of a missing playlist is NO_EXIST", f"rename {q(PL_B)} {q(PL_A)}", 50)
        self.c.cmd(f"playlistadd {q(PL_A)} {q(f[0])}")
        self.expect_ack("rename onto an existing name is EXIST", f"rename {q(PL_C)} {q(PL_A)}", 56)

        # load appends: onto the end, after the listener's entries.
        self.trim_tests()
        self.expect_ok("load", f"load {q(PL_C)}")
        self.ok("load appends it", self.tail() == [f[1], f[0]], repr(self.tail()))
        self.trim_tests()
        self.expect_ack("load of a missing playlist is NO_EXIST", 'load "__mpdcheck_none"', 50)

        self.expect_ok("searchaddpl file <uri>", f"searchaddpl {q(PL_C)} file {q(f[2])}")
        self.ok("searchaddpl appends", self.listed(PL_C)[-1:] == [f[2]], repr(self.listed(PL_C)))
        self.expect_ok("playlistclear", f"playlistclear {q(PL_C)}")
        self.ok("playlistclear empties it", self.listed(PL_C) == [])
        self.expect_ack("playlistclear of a missing playlist is NO_EXIST", f"playlistclear {q(PL_B)}", 50)

        self.idle_wakes(lambda: self.c.cmd(f"playlistadd {q(PL_C)} {q(f[0])}"),
                        "stored_playlist", "idle stored_playlist wakes on playlistadd")
        for pl in (PL_A, PL_C):
            self.expect_ok(f"rm {pl}", f"rm {q(pl)}")

        self.expect_ok('listplaylist "[Radio Streams]"', 'listplaylist "[Radio Streams]"')
        self.expect_ack('playlistdelete "[Radio Streams]" is refused', 'playlistdelete "[Radio Streams]" 0', 5)
        self.expect_ack('searchaddpl "[Radio Streams]" is refused', 'searchaddpl "[Radio Streams]" file x', 2)

    # ---- 5232: the rest of 0.20 (5227-5231) --------------------------------------

    def rest_read_only(self):
        self.section("0.20's remaining verbs (nothing changed)")
        f0 = self.files[0] if getattr(self, "files", None) else None
        if f0:
            folder, name = f0.rsplit("/", 1)
            r = self.expect_ok("listfiles <folder>", f"listfiles {q(folder)}") or []
            self.ok("listfiles names the file", ("file", name) in pairs(r), repr(r[:6]))
            r = self.expect_ok("listfiles (root)", "listfiles") or []
            self.ok("listfiles root lists a folder", any(k == "directory" for k, v in pairs(r)), repr(r))
            try:
                self.c.cmd('listfiles "sd/__no_such__"')
                self.ok("listfiles of a missing folder is refused", False, "got OK")
            except Ack as e:
                # The player says NO_EXIST; stock MPD says SYSTEM, from storage.
                self.ok("listfiles of a missing folder is refused", e.code in (50, 52), str(e))
            r = self.expect_ok("readcomments <file>", f"readcomments {q(f0)}")
            self.expect_ack("readcomments of a missing file is NO_EXIST", 'readcomments "sd/__no_such__.mp3"', 50)
            r = self.expect_ok("count group artist", f"count base {q(folder)} group artist") or []
            n = sum(int(v) for k, v in pairs(r) if k == "songs")
            self.ok("count group adds up to the folder's count", n == int(kv(self.c.cmd(
                f"count base {q(folder)}")).get("songs", "-1")), repr(r[:6]))
        r = self.expect_ok("stats", "stats") or []
        st = kv(r)
        for k in ("artists", "albums", "songs"):
            self.ok(f"stats has {k}", k in st, repr(r))

        self.expect_ack("prio is refused", "prio 1 0:1", 5)
        self.expect_ack("prio 300 is ARG", "prio 300 0:1", 2)
        self.expect_ack("rangeid is refused", "rangeid 1 0:1", 5)
        self.expect_ack("addtagid on a file is refused (MPD's ACK 4)", "addtagid 1 artist x", 4)
        self.expect_ack("cleartagid on a file is refused (MPD's ACK 4)", "cleartagid 1", 4)
        self.expect_ok("mixrampdb 0 (off)", "mixrampdb 0")
        self.expect_ok("mixrampdelay nan (off)", "mixrampdelay nan")
        self.expect_ack("mixrampdelay 2 is refused", "mixrampdelay 2", 5)
        self.expect_ack("config is local-only (PERMISSION)", "config", 4)
        self.expect_ack("sticker: no database", 'sticker get song "x" y', 5)
        # `kill` is in the commands check and deliberately never sent: a
        # real MPD would stop.

    def albumart(self):
        """5240: a cover file beside the first test file, fetched whole in
        chunks, or MPD's "No file exists" when there is none."""
        f0 = self.files[0]
        try:
            size, got, off = None, b"", 0
            while True:
                size, chunk = self.c.binary(f"albumart {q(f0)} {off}")
                got += chunk
                off += len(chunk)
                if not chunk or off >= size:
                    break
            self.ok("albumart pages a cover in whole", size == len(got),
                    f"size {size}, got {len(got)}")
            _, at_end = self.c.binary(f"albumart {q(f0)} {size}")
            self.ok("albumart at the end is binary: 0", at_end == b"")
            try:
                self.c.binary(f"albumart {q(f0)} {size + 10}")
                self.ok("albumart past the end is ARG", False, "got OK")
            except Ack as e:
                self.ok("albumart past the end is ARG", e.code == 2, str(e))
        except Ack as e:
            self.ok("albumart with no cover is NO_EXIST", e.code == 50, str(e))
        self.expect_ack("albumart of a missing song is NO_EXIST",
                        'albumart "sd/__no_such__/x.flac" 0', 50)

    def tagtypes(self):
        """5241: 0.21's tagtypes subcommands, on this connection only."""
        f0 = self.files[0]
        def tags_of(line):
            return {k for k, v in pairs(self.c.cmd(line))} & {"Title", "Artist", "Album"}
        full = tags_of(f"find file {q(f0)}")
        self.expect_ok("tagtypes clear", "tagtypes clear")
        r = self.c.cmd("tagtypes")
        self.ok("tagtypes lists nothing after clear", r == [], repr(r))
        got = tags_of(f"find file {q(f0)}")
        self.ok("a song is sent with no tags after clear", got == set(), repr(got))
        self.expect_ok("tagtypes enable Title", "tagtypes enable Title")
        r = [v for k, v in pairs(self.c.cmd("tagtypes"))]
        self.ok("tagtypes lists Title alone", r == ["Title"], repr(r))
        self.ok("a song is sent with Title alone",
                tags_of(f"find file {q(f0)}") <= {"Title"})
        self.expect_ok("tagtypes all", "tagtypes all")
        self.ok("tagtypes all brings them back", tags_of(f"find file {q(f0)}") == full)
        self.expect_ok("tagtypes disable Artist Album", "tagtypes disable Artist Album")
        r = [v for k, v in pairs(self.c.cmd("tagtypes"))]
        self.ok("disable takes both away", "Artist" not in r and "Album" not in r, repr(r))
        self.expect_ok("tagtypes enable a tag the library has none of", "tagtypes enable Genre")
        self.expect_ack("tagtypes enable an unknown tag is ARG", "tagtypes enable NoSuchTag", 2)
        self.expect_ack("tagtypes with an unknown sub-command is ARG", "tagtypes frob", 2)
        self.c.cmd("tagtypes all")

    def readpicture(self):
        """5249: the picture inside a file, and binarylimit."""
        self.expect_ack("binarylimit 10 is ARG (below 64)", "binarylimit 10", 2)
        self.expect_ack("binarylimit x is ARG", "binarylimit x", 2)
        self.expect_ack("readpicture of a missing song is NO_EXIST",
                        'readpicture "sd/__no_such__.mp3" 0', 50)
        # A file with a picture, if the first few test files have one.
        with_pic = None
        for f in self.files:
            try:
                size, chunk = self.c.binary(f"readpicture {q(f)} 0")
            except Ack as e:
                self.ok(f"readpicture {f}", False, str(e))
                return
            if size:
                with_pic = (f, size, chunk, self.c.last_type)
                break
        if not with_pic:
            self.skip("readpicture paging", "none of the test files has an embedded picture")
            return
        f, size, first, mime = with_pic
        self.ok("readpicture gives the default 8192-byte chunk",
                len(first) == min(size, 8192), f"{len(first)} of {size}")
        self.ok("readpicture names an image type", bool(mime and mime.startswith("image/")), repr(mime))
        got, off = b"", 0
        while off < size:
            _, chunk = self.c.binary(f"readpicture {q(f)} {off}")
            if not chunk:
                break
            got += chunk
            off += len(chunk)
        self.ok("readpicture pages the picture in whole", len(got) == size, f"{len(got)} of {size}")
        self.ok("the picture starts as its type says",
                (mime != "image/jpeg" or got[:3] == b"\xff\xd8\xff") and
                (mime != "image/png" or got[:4] == b"\x89PNG"), got[:4].hex())
        try:
            self.c.binary(f"readpicture {q(f)} {size + 10}")
            self.ok("readpicture past the end is ARG", False, "got OK")
        except Ack as e:
            self.ok("readpicture past the end is ARG", e.code == 2, str(e))
        self.expect_ok("binarylimit 64", "binarylimit 64")
        _, small = self.c.binary(f"readpicture {q(f)} 0")
        self.ok("binarylimit 64 gives 64-byte chunks", len(small) == min(64, size), len(small))
        self.expect_ok("binarylimit 8192 (back to the default)", "binarylimit 8192")

    def messages(self):
        self.section("client messages")
        self.expect_ack("subscribe with a bad name is ARG", 'subscribe "no spaces"', 2)
        self.expect_ok("subscribe", "subscribe mpdcheck")
        self.expect_ack("subscribe twice is EXIST", "subscribe mpdcheck", 56)
        r = self.expect_ok("channels", "channels") or []
        self.ok("channels lists it", ("channel", "mpdcheck") in pairs(r), repr(r))
        self.expect_ack("sendmessage to nobody is NO_EXIST", 'sendmessage nobodyhere "x"', 50)
        try:
            w = self.connect()
        except OSError as e:
            self.ok("second connection", False, str(e))
            return
        try:
            w.cmd("subscribe mpdcheck2")
            w.send("idle message")
            time.sleep(0.3)
            self.expect_ok("sendmessage", 'sendmessage mpdcheck2 "hello, there"')
            w.s.settimeout(5)
            r = w.read_answer()
            self.ok("idle message wakes the subscriber", "changed: message" in r, repr(r))
            r = w.cmd("readmessages")
            self.ok("readmessages gives channel and text",
                    ("channel", "mpdcheck2") in pairs(r) and ("message", "hello, there") in pairs(r), repr(r))
            self.ok("readmessages empties the queue", w.cmd("readmessages") == [])
        except (Ack, OSError) as e:
            self.ok("messages between two connections", False, str(e))
        finally:
            w.close()
        self.expect_ok("unsubscribe", "unsubscribe mpdcheck")
        self.expect_ack("unsubscribe again is NO_EXIST", "unsubscribe mpdcheck", 50)

    def folders(self):
        self.section("adding a folder")
        folder = self.files[0].rsplit("/", 1)[0]
        self.trim_tests()
        self.expect_ok("add <folder>", f"add {q(folder)}")
        t = self.tail()
        self.ok("add <folder> queues its songs", self.files[0] in t and all(u.startswith(folder + "/") for u in t),
                f"{len(t)} added")
        self.trim_tests()
        self.expect_ack("add of a missing folder is NO_EXIST", 'add "sd/__no_such_folder__"', 50)
        have = {v for k, v in pairs(self.c.cmd("listplaylists")) if k == "playlist"}
        if PL_A in have:
            self.skip("playlistadd <folder>", f"{PL_A} already exists")
            return
        self.expect_ok("playlistadd <folder>", f"playlistadd {q(PL_A)} {q(folder)}")
        got = self.listed(PL_A)
        self.ok("playlistadd <folder> writes its songs, not the folder",
                self.files[0] in got and folder not in got, repr(got[:4]))
        self.c.cmd(f"rm {q(PL_A)}")

    # ---- the run -------------------------------------------------------------------

    def run(self):
        print(f"mpdcheck: {self.a.ip}:{self.a.port}" + ("  (read-only)" if self.a.read_only else "")
              + ("  (destructive)" if self.a.destructive else ""))
        try:
            self.c = self.connect()
        except OSError as e:
            print(f"cannot connect: {e}\nIs MPD switched on in the player's settings?")
            return 1
        print(f"  {self.c.greeting}")
        snapped = False
        try:
            # 5226: the state to put back is taken before anything is
            # sent that could change it.
            playing = kv(self.c.cmd("status")).get("state") == "play"
            if not self.a.read_only:
                self.snapshot()
                snapped = True
            self.connection()
            self.introspection()
            self.state_reads()
            self.idle()
            have_lib = self.library()
            self.rest_read_only()                                   # 5232
            if getattr(self, "files", None):
                self.albumart()                                     # 5240
                self.tagtypes()                                     # 5241
                self.readpicture()                                  # 5249
            self.messages()
            if self.a.read_only:
                if playing and kv(self.c.cmd("status")).get("state") != "play":
                    self.c.cmd("play")
                self.skip("queue, transport, volume, stored playlists", "--read-only")
            elif not have_lib:
                self.skip("queue, transport, stored playlists", "not enough files in the library")
            elif not self.consistent:
                # 5235: status and playlistinfo disagree about the queue,
                # so positions cannot be trusted to be the test's own.
                self.ok("status playlistlength matches playlistinfo", False,
                        f"status says {self.saved_status.get('playlistlength')}, "
                        f"playlistinfo lists {self.base}")
                self.skip("queue, transport, volume, stored playlists, folders",
                          "the queue's length is not agreed on; nothing that edits it is run")
            else:
                self.queue_edits()
                self.transport()
                self.relative()                                     # 5251
                self.consume()                                      # 5252
                self.volume_modes()
                self.stored()
                self.folders()                                      # 5232
        except (OSError, ConnectionError, Ack) as e:
            # An ACK nothing expected, or the connection lost: the run
            # stops here, and what was changed is still put back.
            self.ok("the run finished", False, str(e))
        finally:
            if snapped:
                try:
                    self.restore()
                except (OSError, ConnectionError):
                    # The connection went: one more, for the restore.
                    try:
                        self.c = self.connect()
                        self.restore()
                    except (OSError, ConnectionError) as e:
                        print(f"  WARN  could not put things back: {e}")
            try:
                self.c.close()
            except OSError:
                pass

        print(f"\n{self.passed} passed, {len(self.failed)} failed, {len(self.skipped)} skipped")
        for name in self.failed:
            print(f"  FAIL  {name}")
        return min(len(self.failed), 100)


def main():
    p = argparse.ArgumentParser(description="Check the player's MPD server from a PC.")
    p.add_argument("ip", help="the player's IP address")
    p.add_argument("--port", type=int, default=6600)
    p.add_argument("--timeout", type=float, default=15.0, help="seconds per answer (default 15)")
    p.add_argument("--read-only", action="store_true", help="only checks that change nothing")
    p.add_argument("--reindex", action="store_true", help="also send update and rescan")
    p.add_argument("--volume", choices=("auto", "sd", "usb"), default="auto",
                   help="where the test files come from: the USB drive if one is in "
                        "(auto, the default), or the one named")
    p.add_argument("--destructive", action="store_true",
                   help="also clear and shuffle the whole queue (saved and reloaded), "
                        "jump the volume, cycle the modes and replay gain")
    p.add_argument("-v", "--verbose", action="count", default=0,
                   help="-v: every command and pass; -vv: every line read")
    sys.exit(Checker(p.parse_args()).run())


if __name__ == "__main__":
    main()
