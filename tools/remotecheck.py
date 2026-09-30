#!/usr/bin/env python3
"""
remotecheck.py -- drive the player's browser remote from a PC and check
what it answers, the way the page uses it.

WHY THIS EXISTS

mpdcheck.py does this for port 6600. The remote on 443 is the page in
main/remote.html talking to main/remote.c: one WebSocket at /ws carrying
the verbs in remoteproto.h, plus a few plain HTTPS GETs and POSTs
(/art, /stations, /station, /wifi). This speaks the same things the page
speaks, checks each answer against remoteproto.h and remote.c, and says
pass or fail per check.

WHAT IT CHANGES, AND PUTS BACK

By default:

  - the queue: one test file is added at the end and another next, one
    is moved, both are deleted. Your entries keep their ids and order,
    and the run checks that they did;
  - volume: moved one step and back. ReplayGain and the two crossfade
    settings (5269): set to what they already are. Each may be written
    to the card's settings;
  - the sleep timer (5269): only if it is off -- set to 15 min and off
    again. A running timer is left alone, since setting it restarts it;
  - stations: only forms the player refuses, so nothing is written.
    A valid station is never added -- the page has no way to remove one.

`--destructive` adds what cannot be scoped:

  - playback: `open`, `playdir`, `qplay`, `next`, `prev`, `pause`,
    `play` and `seek`. The song you had is played again by its queue id
    and sought back -- to the PERCENT, the page's seek being a percent,
    so within 1% of the track -- and paused if it was paused;
  - `star`, twice, so the favourite ends as it began;
  - ReplayGain and the crossfade settings cycled and put back; a running
    sleep timer is changed and then cleared (its old deadline cannot be
    restored, only its absence);
  - `qclear` is never sent. It is the one queue verb that cannot be put
    back from here.

`--read-only` runs only what changes nothing: hello, the state, the
listing, the refusals, /art, /stations' GET, /wifi's GET, the port-80
redirect. Wi-Fi scan and join are never sent.

TLS: the player's certificate is its own (devcert), so it is not
verified. This checks the player, not the certificate.

USAGE

    ./tools/remotecheck.py 192.168.1.50
    ./tools/remotecheck.py 192.168.1.50 --read-only
    ./tools/remotecheck.py 192.168.1.50 --destructive -v

The remote must be on (the NET tab, or settings "remote"). It serves
four sockets (REMOTE_SOCKETS); this uses one WebSocket and one short
HTTPS connection at a time, so close a page or two if it cannot connect.

Exit status is the number of failed checks (0 is a clean run), capped
at 100. Standard library only.

SPDX-License-Identifier: MIT
"""

import argparse
import base64
import http.client
import json
import os
import socket
import ssl
import struct
import sys
import time

CMD_MAX = 8 + 512           # REMOTEPROTO_CMD_MAX
XFADE_MAX = 12              # REMOTEPROTO_XFADE_MAX
SLEEP_STEPS = 8             # REMOTEPROTO_SLEEP_STEPS
AUDIO_EXT = (".mp3", ".flac", ".ogg", ".opus", ".m4a", ".mp4", ".aac", ".wav")


def tls_context():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


# ---- a WebSocket client, the part the page gets from the browser ----------

class WsClosed(Exception):
    pass


class Ws:
    def __init__(self, host, port, timeout, verbose):
        self.verbose = verbose
        raw = socket.create_connection((host, port), timeout=timeout)
        self.s = tls_context().wrap_socket(raw, server_hostname=host)
        self.s.settimeout(timeout)
        self.buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        self.s.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.s.recv(4096)
            if not chunk:
                raise WsClosed("closed during the handshake")
            head += chunk
        head, self.buf = head.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 101 " not in status + " ":
            raise WsClosed(f"handshake refused: {status}")

    def send_raw(self, payload, opcode=1):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | opcode])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.s.sendall(hdr + mask + body)

    def send(self, text):
        if self.verbose:
            print(f"    > {text}")
        self.send_raw(text.encode())

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise WsClosed("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        """One message as a dict, or None for a text frame that is not JSON."""
        data = b""
        while True:
            b0, b1 = self._need(2)
            op, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._need(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._need(8))[0]
            if b1 & 0x80:
                m = self._need(4)
                payload = bytes(b ^ m[i % 4] for i, b in enumerate(self._need(n)))
            else:
                payload = self._need(n)
            if op == 8:
                raise WsClosed("close frame")
            if op == 9:
                self.send_raw(payload, opcode=10)
                continue
            if op == 10:
                continue
            data += payload
            if b0 & 0x80:
                break
        text = data.decode("utf-8")      # remote.c promises valid UTF-8
        if self.verbose > 1:
            print(f"    < {text[:200]}")
        try:
            return json.loads(text)
        except ValueError:
            return None

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# ---- the checks ------------------------------------------------------------

class Checker:
    def __init__(self, args):
        self.a = args
        self.passed = self.failed = self.skipped = 0
        self.state = None
        self.queue = None           # (version, cur, [(id, name), ...])
        self.pending = []           # messages read while waiting for another kind

    # -- reporting --
    def ok(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            if self.a.verbose:
                print(f"  ok    {name}")
        else:
            self.failed += 1
            print(f"  FAIL  {name}" + (f" -- {detail}" if detail != "" else ""))
        return cond

    def skip(self, name, why):
        self.skipped += 1
        print(f"  skip  {name} ({why})")

    def section(self, title):
        print(title)

    # -- the socket --
    def connect(self):
        self.ws = Ws(self.a.ip, self.a.port, self.a.timeout, self.a.verbose)

    def take(self, m):
        if not isinstance(m, dict):
            return
        t = m.get("t")
        if t == "state":
            self.state = m
        elif t == "q":
            self._queue_frame(m)

    def _queue_frame(self, m):
        if m.get("from") == 0:
            self._qbuild = (m.get("v"), m.get("cur"), [])
        b = getattr(self, "_qbuild", None)
        if b is None or b[0] != m.get("v"):
            return
        b[2].extend((r[0], r[1]) for r in m.get("rows", []))
        if m.get("done"):
            self.queue = b
            self._qbuild = None

    def pump(self, secs):
        """Read everything that arrives for `secs`."""
        end = time.time() + secs
        self.ws.s.settimeout(0.2)
        try:
            while time.time() < end:
                try:
                    self.take(self.ws.recv())
                except (socket.timeout, TimeoutError, ssl.SSLError):
                    continue
        finally:
            self.ws.s.settimeout(self.a.timeout)

    def wait(self, pred, secs=4.0):
        """Read until pred(self) holds or `secs` pass; returns pred's last value."""
        end = time.time() + secs
        self.ws.s.settimeout(0.2)
        try:
            while True:
                v = pred(self)
                if v or time.time() >= end:
                    return v
                try:
                    self.take(self.ws.recv())
                except (socket.timeout, TimeoutError, ssl.SSLError):
                    pass
        finally:
            self.ws.s.settimeout(self.a.timeout)

    def listing(self, path, secs=10.0):
        """ls PATH: the rows, or the error string."""
        self.ws.send(f"ls {path}")
        rows, total, end = [], None, time.time() + secs
        while time.time() < end:
            m = self.ws.recv()
            if not isinstance(m, dict) or m.get("t") != "ls":
                self.take(m)
                continue
            if m.get("path") != path:
                continue
            if "error" in m:
                return m["error"]
            if m.get("from") != len(rows):
                return f"frame from {m.get('from')} after {len(rows)} rows"
            rows += m.get("rows", [])
            total = m.get("total")
            if m.get("done"):
                break
        if total is not None and total != len(rows):
            return f"total {total}, {len(rows)} rows"
        return rows

    def https(self, method, path, body=None, headers=None):
        c = http.client.HTTPSConnection(self.a.ip, self.a.port, timeout=self.a.timeout,
                                        context=tls_context())
        try:
            c.request(method, path, body=body, headers=headers or {})
            r = c.getresponse()
            return r.status, dict(r.getheaders()), r.read()
        finally:
            c.close()

    # -- sections --
    def check_hello(self):
        self.section("hello and the state")
        self.ws.send("hello")
        self.wait(lambda c: c.state is not None and c.queue is not None, 6.0)
        s = self.state
        if not self.ok("hello sends a state", s is not None):
            return False
        for k, typ in (("title", str), ("artist", str), ("album", str), ("art", str),
                       ("path", str), ("pos", int), ("len", int), ("valid", bool),
                       ("playing", bool), ("seek", bool), ("next", bool), ("vol", int),
                       ("muted", bool), ("fav", int), ("rec", bool), ("count", int),
                       ("recok", bool), ("recoff", bool), ("batt", int), ("chg", bool),
                       ("wave", int)):
            self.ok(f"state has {k} ({typ.__name__})", isinstance(s.get(k), typ)
                    and not (typ is int and isinstance(s.get(k), bool)), repr(s.get(k)))
        self.ok("vol is 0..100", 0 <= s.get("vol", -1) <= 100, s.get("vol"))
        self.ok("fav is 0..3", 0 <= s.get("fav", -1) <= 3, s.get("fav"))
        self.ok("pos is within len", not s.get("len") or s.get("pos", 0) <= s["len"] + 1,
                f"{s.get('pos')} / {s.get('len')}")
        self.has_settings = "rg" in s
        if self.has_settings:
            self.ok("xf is 0..XFADE_MAX", 0 <= s.get("xf", -1) <= XFADE_MAX, s.get("xf"))
            self.ok("sleep is 0..SLEEP_STEPS", 0 <= s.get("sleep", -1) <= SLEEP_STEPS, s.get("sleep"))
            self.ok("sleepleft only with a timer", bool(s.get("sleep")) or s.get("sleepleft") == 0,
                    s.get("sleepleft"))
        else:
            self.skip("the settings in the state", "a build before 5269")
        self.ok("hello sends the queue", self.queue is not None)
        if self.queue:
            v, cur, rows = self.queue
            self.ok("queue cur is -1 or a row", cur == -1 or 0 <= cur < len(rows), cur)
            ids = [r[0] for r in rows]
            self.ok("queue ids are unique and positive", len(set(ids)) == len(ids)
                    and all(isinstance(i, int) and i > 0 for i in ids))
        return True

    def check_refusals(self):
        self.section("refusals (ignored, and the socket stays up)")
        for bad in ("Play", "record", "vol 101", "vol -1", "seek 999", "ls", "ls sd",
                    "ls /sd/../etc", "ls /sd//a", "ls /sd/", "open /", "qdel 0",
                    "qmove 1", "rg 2", "xfade 13", "sleep 9", "xfalbum 2", "hello!"):
            self.ws.send(bad)
        self.state = None
        self.ws.send("hello")
        self.ok("still answering after 18 refused commands",
                self.wait(lambda c: c.state is not None, 4.0))
        # An oversized frame drops the socket (remote.c h_ws) -- on a
        # connection of its own, so this one carries on.
        try:
            w = Ws(self.a.ip, self.a.port, self.a.timeout, 0)
            w.send("ls /sd/" + "a" * (CMD_MAX + 8))
            dropped = False
            try:
                w.s.settimeout(3.0)
                while True:
                    w.recv()
            except (WsClosed, ConnectionError, ssl.SSLError, OSError):
                dropped = True
            self.ok("a frame past REMOTEPROTO_CMD_MAX drops that socket", dropped)
            w.close()
        except (OSError, WsClosed) as e:
            self.skip("oversized frame", f"no second socket: {e}")

    def check_listing(self):
        self.section("the file listing")
        root = self.listing("/")
        if not self.ok("ls / lists", isinstance(root, list), root):
            return None
        vols = [r["n"] for r in root]
        self.ok("ls / is volumes, as folders", all(r.get("d") == 1 for r in root)
                and set(vols) <= {"sd", "usb"}, vols)
        want = [v for v in vols if self.a.volume in ("auto", v)]
        if not want:
            self.skip("listing a volume", f"no {self.a.volume} mounted")
            return None
        vol = "usb" if "usb" in want and self.a.volume == "auto" else want[0]
        err = self.listing(f"/{vol}/__remotecheck_no_such_folder")
        self.ok("a missing folder is an error, not rows", isinstance(err, str), err)
        # Find a folder with two playable files, breadth first, bounded.
        todo, seen, found = [f"/{vol}"], 0, None
        while todo and seen < 40 and not found:
            path = todo.pop(0)
            rows = self.listing(path)
            seen += 1
            if not isinstance(rows, list):
                continue
            if seen == 1:
                self.ok(f"ls /{vol} lists", True)
                names = [r["n"] for r in rows]
                dirs = [r["n"] for r in rows if r.get("d")]
                self.ok("folders first, then files (ls_cmp)",
                        names[:len(dirs)] == dirs or not dirs, names[:6])
            files = [r for r in rows if not r.get("d") and not r.get("l")
                     and r["n"].lower().endswith(AUDIO_EXT)]
            if len(files) >= 2:
                found = (path, files[0]["n"], files[1]["n"])
            todo += [f"{path}/{r['n']}" for r in rows if r.get("d")]
        if not found:
            self.skip("queue and playback", "no folder with two playable files within 40")
        return found

    def check_http(self):
        self.section("the other endpoints")
        st, _, body = self.https("GET", "/")
        self.ok("GET / is the page", st == 200 and b"/ws" in body, st)
        st, _, _ = self.https("GET", "/art?k=00000000")
        self.ok("/art with a stale key is 404", st == 404, st)
        key = (self.state or {}).get("art")
        if key:
            st, h, body = self.https("GET", f"/art?k={key}")
            ct = {k.lower(): v for k, v in h.items()}.get("content-type", "")
            self.ok("/art with the state's key is the cover", st == 200 and ct.startswith("image/")
                    and (body[:3] == b"\xff\xd8\xff" or body[:8] == b"\x89PNG\r\n\x1a\n"),
                    f"{st} {ct} {len(body)} bytes")
        else:
            self.skip("/art of the cover", "nothing shown has one")
        st, _, body = self.https("GET", "/stations")
        try:
            j = json.loads(body)
            good = st == 200 and isinstance(j.get("max"), int) and isinstance(j.get("list"), list) \
                and len(j["list"]) <= j["max"]
        except ValueError:
            good, j = False, body[:80]
        self.ok("/stations is {max, list}", good, j if not good else "")
        st, _, body = self.https("GET", "/wifi")
        try:
            j = json.loads(body)
            good = st == 200 and all(k in j for k in ("current", "cable", "setup", "state", "msg"))
        except ValueError:
            good, j = False, body[:80]
        self.ok("/wifi is its status object", good, j if not good else "")
        try:
            c = http.client.HTTPConnection(self.a.ip, 80, timeout=self.a.timeout)
            c.request("GET", "/x?y=1")
            r = c.getresponse()
            loc = r.getheader("Location", "")
            c.close()
            self.ok("port 80 redirects to https, keeping the path",
                    r.status in (301, 302, 307, 308) and loc.startswith("https://")
                    and loc.endswith("/x?y=1"), f"{r.status} {loc}")
        except OSError as e:
            self.ok("port 80 answers", False, e)

    def check_station_refusals(self):
        self.section("stations: refused forms (nothing written)")
        hdr = {"Content-Type": "application/x-www-form-urlencoded"}
        for label, body in (("no url", "name=x&url="),
                            ("not http", "name=x&url=ftp%3A%2F%2Fa.b%2Fc"),
                            ("name starting #", "name=%23x&url=http%3A%2F%2Fa.b%2Fc"),
                            ("too large", "url=" + "a" * 1100)):
            try:
                st, _, raw = self.https("POST", "/station", body.encode(), hdr)
            except (OSError, http.client.HTTPException) as e:
                self.ok(f"station {label} is answered", False, e)
                continue
            try:
                j = json.loads(raw)
            except ValueError:
                j = {}
            self.ok(f"station {label} is refused", j.get("ok") is False and j.get("msg"), raw[:100])

    def set_and_see(self, verb, value, key, want, secs=4.0):
        self.ws.send(f"{verb} {value}")
        got = self.wait(lambda c: c.state and c.state.get(key) == want, secs)
        return self.ok(f"{verb} {value} -> {key} {want!r}", got,
                       (self.state or {}).get(key))

    def check_settings(self):
        self.section("volume and settings")
        s = dict(self.state)
        vol = s["vol"]
        other = vol - 1 if vol > 0 else 1
        self.set_and_see("vol", other, "vol", other)
        self.set_and_see("vol", vol, "vol", vol)
        if not self.has_settings:
            self.skip("rg, xfade, xfalbum, sleep", "a build before 5269")
            return
        rg, xf, xfa, sl = s["rg"], s["xf"], s["xfa"], s["sleep"]
        if self.a.destructive:
            self.set_and_see("rg", int(not rg), "rg", not rg)
            self.set_and_see("xfade", XFADE_MAX if xf != XFADE_MAX else 0, "xf",
                             XFADE_MAX if xf != XFADE_MAX else 0)
            self.set_and_see("xfalbum", int(not xfa), "xfa", not xfa)
        self.set_and_see("rg", int(rg), "rg", rg)
        self.set_and_see("xfade", xf, "xf", xf)
        self.set_and_see("xfalbum", int(xfa), "xfa", xfa)
        if sl == 0 or self.a.destructive:
            self.set_and_see("sleep", 1, "sleep", 1)
            left = self.state.get("sleepleft", 0)
            self.ok("sleep 1 is 15 minutes left", 14 * 60 <= left <= 15 * 60, left)
            self.set_and_see("sleep", 0, "sleep", 0)
            self.ok("sleep 0 leaves nothing left", self.state.get("sleepleft") == 0,
                    self.state.get("sleepleft"))
            if sl:
                print(f"  note  the sleep timer was at step {sl}; it is off now")
        else:
            self.skip("sleep", f"a timer is running (step {sl}); --destructive to change it")

    def qids(self):
        return [r[0] for r in self.queue[2]] if self.queue else []

    def check_queue(self, found):
        self.section("the queue")
        folder, fa, fb = found
        before = list(self.queue[2])
        ids0 = [r[0] for r in before]
        v0 = self.queue[0]
        self.ws.send(f"add {folder}/{fa}")
        self.wait(lambda c: c.queue and len(c.queue[2]) == len(before) + 1, 5.0)
        rows = self.queue[2]
        self.ok("add appends one entry", len(rows) == len(before) + 1, len(rows))
        self.ok("the queue's version moved", self.queue[0] != v0, self.queue[0])
        new_a = [r for r in rows if r[0] not in ids0]
        if not self.ok("add's entry is the file, at the end",
                       len(new_a) == 1 and rows[-1][0] == new_a[0][0] and new_a[0][1] == fa,
                       rows[-1:]):
            return
        ida = new_a[0][0]
        self.ws.send(f"addnext {folder}/{fb}")
        self.wait(lambda c: c.queue and len(c.queue[2]) == len(before) + 2, 5.0)
        rows = self.queue[2]
        new_b = [r for r in rows if r[0] not in ids0 and r[0] != ida]
        cur = self.queue[1]
        if self.ok("addnext adds one entry", len(new_b) == 1, len(rows)):
            idb = new_b[0][0]
            pos_b = [r[0] for r in rows].index(idb)
            self.ok("addnext's entry is after the playing one (or first)",
                    pos_b == (cur + 1 if cur >= 0 else 0) or pos_b == len(rows) - 1,
                    f"at {pos_b}, cur {cur}")
            # Move it to the very end, then delete both.
            self.ws.send(f"qmove {idb} {len(rows) - 1}")
            self.wait(lambda c: c.queue and c.qids()[-1:] == [idb], 5.0)
            self.ok("qmove puts it at the position", self.qids()[-1:] == [idb], self.qids()[-3:])
            self.ws.send(f"qdel {idb}")
            self.wait(lambda c: idb not in c.qids(), 5.0)
            self.ok("qdel removes it", idb not in self.qids())
        self.ws.send(f"qdel {ida}")
        self.wait(lambda c: ida not in c.qids(), 5.0)
        self.ok("qdel removes the first", ida not in self.qids())
        self.ws.send("qdel 2147483647")
        self.pump(0.5)
        self.ok("your entries kept their ids and order", self.qids() == ids0,
                f"{len(self.qids())} entries")

    def check_playback(self, found):
        self.section("playback (--destructive)")
        folder, fa, fb = found
        s0, q0 = dict(self.state), self.queue
        cur0 = q0[1]
        was_id = q0[2][cur0][0] if 0 <= cur0 < len(q0[2]) else None
        was_pct = int(100 * s0["pos"] / s0["len"]) if s0.get("len") else 0

        self.ws.send(f"open {folder}/{fa}")
        self.wait(lambda c: c.state and c.state.get("path", "").endswith("/" + fa)
                  and c.state.get("playing"), 8.0)
        self.ok("open plays the file", self.state.get("path", "").endswith("/" + fa),
                self.state.get("path"))
        self.ws.send("pause")
        self.ok("pause", self.wait(lambda c: c.state and not c.state["playing"], 4.0))
        self.ws.send("play")
        self.ok("play", self.wait(lambda c: c.state and c.state["playing"], 4.0))
        self.wait(lambda c: c.state.get("valid") and c.state.get("len", 0) > 0, 6.0)
        if self.state.get("seek") and self.state.get("len", 0) >= 20:
            L = self.state["len"]
            self.ws.send("seek 50")
            self.ok("seek 50 lands near the middle",
                    self.wait(lambda c: abs(c.state.get("pos", 0) - L // 2) <= 2 + L // 100, 5.0),
                    f"{self.state.get('pos')} of {L}")
        else:
            self.skip("seek", "the file is not seekable, or under 20 s")
        if self.state.get("next"):
            p = self.state.get("path")
            self.ws.send("next")
            self.ok("next changes the track", self.wait(lambda c: c.state.get("path") != p, 8.0))
            self.ws.send("prev")
            self.pump(1.0)
        self.ws.send(f"playdir {folder}")
        self.ok("playdir plays from that folder",
                self.wait(lambda c: c.state.get("path", "").startswith(folder + "/"), 8.0),
                self.state.get("path"))
        fav = self.state.get("fav", 0)
        if fav in (1, 2):
            self.ws.send("star")
            self.ok("star flips the favourite", self.wait(lambda c: c.state.get("fav") == 3 - fav, 5.0),
                    self.state.get("fav"))
            self.ws.send("star")
            self.ok("star again puts it back", self.wait(lambda c: c.state.get("fav") == fav, 5.0),
                    self.state.get("fav"))
        else:
            self.skip("star", "no star to toggle here")
        # And back to what was playing.
        if was_id is not None and was_id in self.qids():
            self.ws.send(f"qplay {was_id}")
            self.ok("qplay plays your song again",
                    self.wait(lambda c: c.state.get("path") == s0.get("path"), 8.0),
                    self.state.get("path"))
            if s0.get("seek") and was_pct:
                self.wait(lambda c: c.state.get("valid"), 6.0)
                self.ws.send(f"seek {was_pct}")
                self.pump(1.5)
            if not s0.get("playing"):
                self.ws.send("pause")
                self.wait(lambda c: not c.state["playing"], 4.0)
        else:
            print("  note  nothing of your queue was playing; it is not restarted")
            self.ws.send("pause")
            self.pump(1.0)

    def run(self):
        try:
            self.connect()
        except (OSError, WsClosed) as e:
            print(f"cannot reach the remote at {self.a.ip}:{self.a.port}: {e}")
            return 1
        try:
            if not self.check_hello():
                return self.failed
            self.check_refusals()
            found = self.check_listing()
            self.check_http()
            if not self.a.read_only:
                self.check_station_refusals()
                self.check_settings()
                if found:
                    self.check_queue(found)
                    if self.a.destructive:
                        self.check_playback(found)
        except (WsClosed, OSError) as e:
            self.ok("the WebSocket stayed up", False, e)
        finally:
            self.ws.close()
        print(f"\n{self.passed} passed, {self.failed} failed, {self.skipped} skipped")
        return self.failed


def main():
    p = argparse.ArgumentParser(description="Check the player's browser remote from a PC.")
    p.add_argument("ip", help="the player's IP address")
    p.add_argument("--port", type=int, default=443)
    p.add_argument("--timeout", type=float, default=15.0, help="seconds per answer (default 15)")
    p.add_argument("--read-only", action="store_true", help="only checks that change nothing")
    p.add_argument("--destructive", action="store_true",
                   help="also playback, star, and cycling the settings (see the top of the file)")
    p.add_argument("--volume", choices=("auto", "sd", "usb"), default="auto",
                   help="which volume the test files come from (auto: usb if present)")
    p.add_argument("-v", "--verbose", action="count", default=0,
                   help="-v every check and command, -vv every message too")
    a = p.parse_args()
    if a.read_only and a.destructive:
        p.error("--read-only and --destructive together")
    sys.exit(min(Checker(a).run(), 100))


if __name__ == "__main__":
    main()
