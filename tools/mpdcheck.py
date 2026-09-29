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

WHAT IT CHANGES, AND PUTS BACK

The queue, volume, the play modes, the replay-gain mode and stored
playlists are changed, then restored:

  - the queue is saved as the stored playlist "__mpdcheck_saved" first
    and loaded back at the end (a station playing is not queued, so
    it is not restored -- start it again by hand);
  - volume, repeat/random/single/consume and replay gain are set back;
  - every stored playlist it makes starts with "__mpdcheck" and is
    removed.

If a run is interrupted, "__mpdcheck_saved" is still on the card; `load`
it by hand. `--read-only` runs only the checks that change nothing.
`update` and `rescan` start a reindex and are only sent with --reindex.

The library needs at least three playable files, indexed. Tags are
checked only where the catalog has them.

USAGE

    ./tools/mpdcheck.py 192.168.1.50
    ./tools/mpdcheck.py 192.168.1.50 --read-only
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

# Every verb the server answers as of 5224 -- `commands` must list each.
EXPECTED_COMMANDS = """
ping close commands notcommands tagtypes urlhandlers decoders status stats
currentsong clearerror play playid pause stop next previous seek seekid
seekcur setvol volume outputs replay_gain_mode replay_gain_status channels
idle repeat random single consume add addid delete deleteid move moveid
clear shuffle playlistinfo playlistid playlist plchanges plchangesposid
lsinfo listall listallinfo find search list count listplaylists
listplaylist listplaylistinfo load save rm playlistadd playlistdelete
listpartitions partition delpartition moveoutput listmounts listneighbors
getvol password crossfade enableoutput disableoutput toggleoutput swap
swapid findadd searchadd searchaddpl playlistfind playlistsearch
playlistclear playlistmove rename
""".split()


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

    def find_files(self, want=3, max_dirs=60):
        """A breadth-first walk of lsinfo until `want` files are found."""
        found, dirs, seen = [], [""], 0
        while dirs and len(found) < want and seen < max_dirs:
            d = dirs.pop(0)
            seen += 1
            try:
                r = self.c.cmd("lsinfo " + q(d) if d else "lsinfo")
            except Ack:
                continue
            for k, v in pairs(r):
                if k == "directory":
                    dirs.append(v)
                elif k == "file" and not v.startswith(("http://", "https://")):
                    found.append(v)
        return found

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
        r = self.expect_ok("search file <PART OF NAME, upper case>", f"search file {q(base[:4].upper())}") or []
        self.ok("search folds case", f0 in files_of(r))
        r = self.expect_ok("find base <folder>", f"find base {q(folder)}") or []
        self.ok("find base finds it", f0 in files_of(r))
        r = self.expect_ok("search with window 0:1", f"search file {q(base[:3])} window 0:1") or []
        self.ok("window 0:1 gives at most one", len(files_of(r)) <= 1, repr(r))
        r = self.expect_ok("count file <uri>", f"count file {q(f0)}") or []
        self.ok("count gives songs: 1", kv(r).get("songs") == "1", repr(r))
        self.expect_ok("find genre x (a tag not held) is empty OK", 'find genre "x"')
        self.expect_ack("find with an odd argument count is ARG", "find artist", 2)
        self.expect_ack("filter expression is refused", 'find "(artist == \\"x\\")"', 5)

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
            self.expect_ok("update", "update")
            self.expect_ok("rescan", "rescan")
        else:
            self.skip("update/rescan", "starts a reindex; pass --reindex")
        return True

    # ---- snapshot and restore ----------------------------------------------

    def snapshot(self):
        st = self.status()
        self.saved_status = st
        try:
            self.c.cmd(f"rm {q(SAVED)}")
        except Ack:
            pass
        self.queue_saved = False
        if int(st.get("playlistlength", "0")) > 0:
            try:
                self.c.cmd(f"save {q(SAVED)}")
                self.queue_saved = True
            except Ack as e:
                print(f"  note  could not save the queue ({e}); it will not be restored")
        rg = kv(self.c.cmd("replay_gain_status")).get("replay_gain_mode", "off")
        self.saved_rg = rg

    def restore(self):
        self.section("putting things back")
        st = self.saved_status
        for pl in (PL_A, PL_B, PL_C):
            try:
                self.c.cmd(f"rm {q(pl)}")
            except Ack:
                pass
        try:
            self.c.cmd("clear")
            if self.queue_saved:
                self.c.cmd(f"load {q(SAVED)}")
                self.c.cmd(f"rm {q(SAVED)}")
                if st.get("song") is not None:
                    self.c.cmd(f"play {st['song']}")
                    if st.get("state") != "play":
                        self.c.cmd("pause 1")
            for m in ("repeat", "random", "single", "consume"):
                self.c.cmd(f"{m} {st.get(m, '0')}")
            self.c.cmd(f"setvol {st.get('volume', '50')}")
            self.c.cmd(f"replay_gain_mode {self.saved_rg}")
            print("  done  queue, volume, modes and replay gain restored")
        except (Ack, OSError) as e:
            print(f"  WARN  restore incomplete: {e}")
            if self.queue_saved:
                print(f'        the queue is in the stored playlist "{SAVED}"')

    # ---- the queue -----------------------------------------------------------

    def queue_edits(self):
        self.section("queue")
        f = self.files
        self.expect_ok("clear", "clear")
        self.ok("clear empties the queue", self.queue() == [])
        for i in range(3):
            self.expect_ok(f"add file {i}", f"add {q(f[i])}")
        self.ok("add appends in order", self.queue() == f[:3], repr(self.queue()))
        self.expect_ack("add of a missing file is NO_EXIST", 'add "sd/__no_such_file__.mp3"', 50)

        r = self.expect_ok("addid at position 0", f"addid {q(f[2])} 0") or []
        new_id = kv(r).get("Id")
        self.ok("addid answers Id", new_id is not None, repr(r))
        self.ok("addid at 0 lands first", self.queue()[:1] == [f[2]], repr(self.queue()))

        st = self.status()
        self.ok("status playlistlength follows", st.get("playlistlength") == "4", st.get("playlistlength"))
        v0 = st.get("playlist")

        r = self.expect_ok("playlistid <id>", f"playlistid {new_id}") or []
        self.ok("playlistid gives that entry", songs(r) and songs(r)[0].get("Id") == new_id, repr(r))
        self.expect_ack("playlistid of a missing id is NO_EXIST", "playlistid 999999", 50)
        r = self.expect_ok("playlistinfo 1:3", "playlistinfo 1:3") or []
        self.ok("playlistinfo range gives two", len(songs(r)) == 2, repr(r))
        r = self.expect_ok("playlist", "playlist") or []
        self.ok("playlist lines are N:file: uri", all(re.match(r"^\d+:file: ", l) for l in r) and len(r) == 4, repr(r))
        self.expect_ack("playlistinfo past the end is ARG", "playlistinfo 10:12", 2)

        self.expect_ok("deleteid", f"deleteid {new_id}")
        self.ok("deleteid removes it", self.queue() == f[:3], repr(self.queue()))
        r = self.expect_ok("plchanges <old version>", f"plchanges {v0}") or []
        self.ok("plchanges reports changed entries", len(songs(r)) >= 1, repr(r))
        self.expect_ok("plchangesposid <old version>", f"plchangesposid {v0}")

        self.expect_ok("move 0 2", "move 0 2")
        self.ok("move 0 2", self.queue() == [f[1], f[2], f[0]], repr(self.queue()))
        ids = self.ids()
        self.expect_ok("moveid <last> 0", f"moveid {ids[2]} 0")
        self.ok("moveid", self.queue() == [f[0], f[1], f[2]], repr(self.queue()))
        self.expect_ok("move range 0:2 1", "move 0:2 1")
        self.ok("move range", self.queue() == [f[2], f[0], f[1]], repr(self.queue()))
        self.expect_ack("move past the end is ARG", "move 0 9", 2)

        self.expect_ok("swap 0 2", "swap 0 2")
        self.ok("swap", self.queue() == [f[1], f[0], f[2]], repr(self.queue()))
        ids = self.ids()
        self.expect_ok("swapid", f"swapid {ids[0]} {ids[1]}")
        self.ok("swapid", self.queue() == [f[0], f[1], f[2]], repr(self.queue()))
        self.ok("ids survive a swap", sorted(self.ids()) == sorted(ids))
        self.expect_ack("swap past the end is ARG", "swap 0 9", 2)
        self.expect_ack("swapid of a missing id is NO_EXIST", f"swapid {ids[0]} 999999", 50)

        before = sorted(self.ids())
        self.expect_ok("shuffle", "shuffle")
        self.ok("shuffle keeps the same entries and ids", sorted(self.ids()) == before)
        self.expect_ack("shuffle of a range is refused", "shuffle 0:2", 5)
        self.c.cmd("clear")
        for i in range(3):
            self.c.cmd(f"add {q(f[i])}")

        r = self.expect_ok("playlistfind file <uri>", f"playlistfind file {q(f[1])}") or []
        self.ok("playlistfind finds it at Pos 1", [s.get("Pos") for s in songs(r)] == ["1"], repr(r))
        base = f[1].rsplit("/", 1)[1]
        r = self.expect_ok("playlistsearch file <PART>", f"playlistsearch file {q(base[:4].upper())}") or []
        self.ok("playlistsearch finds it", f[1] in files_of(r), repr(r))
        r = self.expect_ok("playlistfind genre x (not held)", 'playlistfind genre "x"') or []
        self.ok("playlistfind on an unheld tag is empty", r == [], repr(r))
        self.expect_ack("playlistfind odd arguments is ARG", "playlistfind file", 2)

        n = len(self.queue())
        self.expect_ok("findadd file <uri>", f"findadd file {q(f[0])}")
        self.ok("findadd appends one", self.queue() == f[:3] + [f[0]], repr(self.queue()))
        self.expect_ok("searchadd file <exact name>", f"searchadd file {q(f[1])}")
        self.ok("searchadd appends at least one", len(self.queue()) >= n + 2, repr(self.queue()))
        self.expect_ok("delete range to the end", f"delete {n}:")
        self.ok("delete N: trims the end", self.queue() == f[:3], repr(self.queue()))
        self.expect_ok("delete 0", "delete 0")
        self.ok("delete 0", self.queue() == f[1:3], repr(self.queue()))
        self.expect_ack("delete past the end is ARG", "delete 9", 2)
        self.c.cmd("clear")
        for i in range(3):
            self.c.cmd(f"add {q(f[i])}")

        self.idle_wakes(lambda: self.c.cmd(f"add {q(f[0])}"), "playlist",
                        "idle playlist wakes on add from another client")
        self.c.cmd("delete 3")

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
        self.section("transport")
        ids = self.ids()
        self.expect_ok("play 0", "play 0")
        st = self.wait_state(lambda s: s.get("state") == "play" and s.get("song") == "0")
        self.ok("play 0 plays song 0", st.get("state") == "play" and st.get("song") == "0",
                f"{st.get('state')} song {st.get('song')}")
        self.ok("status has songid", st.get("songid") == ids[0], st.get("songid"))
        r = self.expect_ok("currentsong while playing", "currentsong") or []
        self.ok("currentsong is song 0", kv(r).get("file") == self.files[0], repr(r))

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

        self.expect_ok("next", "next")
        st = self.wait_state(lambda s: s.get("song") == "1")
        self.ok("next goes to song 1", st.get("song") == "1", st.get("song"))
        self.expect_ok("previous", "previous")
        st = self.wait_state(lambda s: s.get("song") in ("0", "1"))
        self.ok("previous answers (restart or back)", st.get("song") in ("0", "1"), st.get("song"))

        self.expect_ok("playid <id of song 2>", f"playid {ids[2]}")
        st = self.wait_state(lambda s: s.get("song") == "2")
        self.ok("playid plays it", st.get("song") == "2", st.get("song"))
        self.expect_ack("playid of a missing id is NO_EXIST", "playid 999999", 50)
        self.expect_ack("play past the end is ARG", "play 99", 2)

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
            self.expect_ack("seek of another song is refused", f"seek {(int(st.get('song', '0')) + 1) % 3} 1", 5)
        else:
            self.skip("seek, seekid, seekcur", "the song has no duration over 10 s")

        self.expect_ok("stop", "stop")
        st = self.wait_state(lambda s: s.get("state") in ("pause", "stop"))
        self.ok("stop is a pause here", st.get("state") in ("pause", "stop"), st.get("state"))

    def volume_modes(self):
        self.section("volume, modes, replay gain")
        self.expect_ok("setvol 30", "setvol 30")
        self.ok("getvol reads 30", kv(self.c.cmd("getvol")).get("volume") == "30")
        self.expect_ok("volume +5 (deprecated, relative)", "volume +5")
        self.ok("volume +5 gives 35", kv(self.c.cmd("getvol")).get("volume") == "35")
        self.expect_ack("setvol 101 is ARG", "setvol 101", 2)
        self.idle_wakes(lambda: self.c.cmd("setvol 32"), "mixer", "idle mixer wakes on setvol")

        for m in ("repeat", "random", "single", "consume"):
            self.expect_ok(f"{m} 1", f"{m} 1")
            self.ok(f"status has {m} after setting it", m in self.status())
            self.expect_ok(f"{m} 0", f"{m} 0")
            self.expect_ack(f"{m} 2 is ARG", f"{m} 2", 2)
        # The four modes map onto the player's four play orders; what
        # comes back is what it will do (MPD.md), so report, do not judge.
        self.c.cmd("random 1")
        st = self.status()
        print(f"  info  random 1 -> repeat {st.get('repeat')} random {st.get('random')} "
              f"single {st.get('single')} consume {st.get('consume')}")
        self.c.cmd("random 0")

        for m in ("track", "off"):
            self.expect_ok(f"replay_gain_mode {m}", f"replay_gain_mode {m}")
            got = kv(self.c.cmd("replay_gain_status")).get("replay_gain_mode")
            self.ok(f"replay_gain_status reads {m}", got == m, got)
        self.expect_ack("replay_gain_mode nonsense is ARG", "replay_gain_mode nonsense", 2)

    # ---- stored playlists --------------------------------------------------------

    def listed(self, name):
        return [l.split(": ", 1)[1] for l in self.c.cmd(f"listplaylist {q(name)}") if l.startswith("file: ")]

    def stored(self):
        self.section("stored playlists")
        f = self.files
        for pl in (PL_A, PL_B, PL_C):
            try:
                self.c.cmd(f"rm {q(pl)}")
            except Ack:
                pass

        self.expect_ok("save", f"save {q(PL_A)}")
        self.ok("save writes the queue", self.listed(PL_A) == self.queue(), repr(self.listed(PL_A)))
        self.expect_ack("save onto an existing name is EXIST", f"save {q(PL_A)}", 56)
        r = self.expect_ok("listplaylists", "listplaylists") or []
        self.ok("listplaylists has it", PL_A in [v for k, v in pairs(r) if k == "playlist"])
        r = self.expect_ok("listplaylistinfo", f"listplaylistinfo {q(PL_A)}") or []
        self.ok("listplaylistinfo gives songs", len(songs(r)) == 3, repr(r))
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
        self.c.cmd(f"save {q(PL_A)}")
        self.expect_ack("rename onto an existing name is EXIST", f"rename {q(PL_C)} {q(PL_A)}", 56)

        self.c.cmd("clear")
        self.expect_ok("load", f"load {q(PL_C)}")
        self.ok("load fills the queue", self.queue() == [f[1], f[0]], repr(self.queue()))
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

    # ---- the run -------------------------------------------------------------------

    def run(self):
        print(f"mpdcheck: {self.a.ip}:{self.a.port}" + ("  (read-only)" if self.a.read_only else ""))
        try:
            self.c = self.connect()
        except OSError as e:
            print(f"cannot connect: {e}\nIs MPD switched on in the player's settings?")
            return 1
        print(f"  {self.c.greeting}")
        try:
            self.connection()
            self.introspection()
            self.state_reads()
            self.idle()
            have_lib = self.library()
            if self.a.read_only:
                self.skip("queue, transport, volume, stored playlists", "--read-only")
            elif not have_lib:
                self.skip("queue, transport, stored playlists", "not enough files in the library")
            else:
                self.snapshot()
                try:
                    self.queue_edits()
                    self.transport()
                    self.volume_modes()
                    self.stored()
                finally:
                    self.restore()
        except (OSError, ConnectionError) as e:
            self.ok("connection stayed up", False, str(e))
        finally:
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
    p.add_argument("-v", "--verbose", action="count", default=0,
                   help="-v: every command and pass; -vv: every line read")
    sys.exit(Checker(p.parse_args()).run())


if __name__ == "__main__":
    main()
