# MPD support

Written as a plan, before anything was built; kept as the reasoning, so
the decisions already argued out do not have to be argued again.
**It is built now** -- every step below has a first version, and the
server has been driven from Cantata on the board (5176-5194). Where the
plan and the code part ways the step says so, and "Where it stands", at
the end of the staging, is the current state. `ARCHITECTURE.md` is the
code as it is, one entry per patch; `MEDIA-INDEX.md` is the library
this leans on.

The target is **full queue semantics** -- a control client that can
`add`, `delete`, `move`, `playid`, and see song ids and positions, not
just browse and press play. That decision is what makes this expensive,
and most of what follows is the consequence of it.

**And the queue is the device's, not MPD's.** The remote page on 443
gets the same verbs: it already browses the card over the socket (5123)
and the natural next thing to do with a listed file is enqueue it rather
than play it now. So the queue is a module both protocols are views of,
and MPD is the second consumer of it rather than the reason for it. That
ordering matters in two places -- it decides where the mutation path
lives (below), and it means the queue is justified even if nothing ever
listens on 6600.


## The one hard problem, stated first

**MPD's queue and `playlist.c` are not the same object, and cannot be
made into the same object by adding functions to `playlist.c`.**

What is there now (`playlist.h:2-8`) is a *folder*: `playlist_load_dir()`
reads one directory, filters it by `storage_is_hidden()`,
`decoder_supports()` and `cuedir_hides()`, expands cue sheets, and
`qsort`s the result. There is no append, no insert, no remove, no move.
Order is a function of the directory's contents, not a thing anyone
chose. `PLAYLIST_MAX` is 1024 pointers in PSRAM; the strings themselves
are `strdup()` into internal heap.

What MPD asks for is a *list someone built*: arbitrary tracks from
anywhere on either volume, in an order the client set, each with a
stable id that survives a move, plus `random`, `repeat`, `single` and
`consume` as four independent flags.

Three specific collisions, in the order they will bite:

**Order is derived, so it cannot be edited.** Every `playlist_load_dir()`
re-sorts (`playlist.c:117`). An MPD `move 3 0` has to survive the next
thing that touches the list, and today nothing survives a load.

**The four modes are one enum.** `play_order_t` (`playlist.h:23-28`) is
`ONE | ALL | SHUFFLE | REPEAT_ONE`, and it lives in `browser.c`, read
through `browser_order()`. MPD has four booleans, which is sixteen
states; the enum covers four of them. `consume` has no analogue at all
and changes the queue rather than the traversal. This mapping is lossy
in both directions and the mapping table is a thing to write down, not
to improvise per command.

**The playlist hands out pointers it later frees.** `playlist_path()`
admits the invalidation (`playlist.h:54-56`), and `playlist_peek_next()`
(`playlist.c:158-169`) returns a raw `s_paths[i]` that `prefetch_next()`
(`player.c:5512`) then carries across `storage_io_open()`,
`covertag_extract_art()` and `sidecar_prime()` -- hundreds of
milliseconds of card I/O. `playlist_clear()` (`playlist.c:37`) `free()`s
every entry. Today that is safe only because the sole mutator is
`ui_task` at rare user events. A queue a client can rewrite over TCP
while a track is playing turns it into a use-after-free, and the
`s_track_gen` checks (`player.c:5530, 5556, 5602`) do not help: they sit
*between* stages, not during a read.

**There is no lock.** Not a mutex, not a critical section, nowhere in
`playlist.c`. And the "only ui_task mutates" rule is already bent:
`playlist_next()` is called on `media_task` at `player.c:13626` and
`playlist_peek_next()` on `media_task` at `player.c:5514`.

So the queue is a new module with its own storage and its own ownership
rule. `playlist.c` stays what it is -- the chooser's folder -- and the
two are bridged, not merged. Which way that bridge runs is the next
section.


## The queue: one list, two producers

The proposal is `mpdqueue.c`, a list of entries

    { char *path;  uint32_t id;  }

in PSRAM, holding **the same 1024 ceiling** as `PLAYLIST_MAX` for now
(the reasons the existing number is 1024 have not changed: PSRAM is
already carrying the 256 KB netstream ring, the framebuffer, two
~142 KB cover entries and the decoders' scratch -- `MEDIA-INDEX.md`,
"Memory"). Ids are a monotonic `uint32_t` never reused within a boot,
which is what MPD means by a song id.

**And `playlist.c` becomes a view of it, not a peer.** This is the
decision to argue about, so here is the argument.

The alternative -- two lists, one for the glass and one for the
protocol, with "which is playing" arbitrated somewhere -- was
considered and rejected. It means two sources of truth for the next
track, and `player_loop()` at `player.c:13626` has exactly one line
that decides what plays next. Two lists means that line grows a mode
flag, and every one of the three rings (`ARCHITECTURE.md`, 1201/1204/
1210 -- the three places that each assumed the decode was one track
ahead) has to be re-reasoned for both. That is the shape of bug the
1200 series was spent on.

So: **a folder tap fills the queue.** `BROWSER_PLAY_FILE` and
`BROWSER_PLAY_FOLDER` (`player.c:7248-7275`) come to mean "replace the
queue with this folder, set current". The user-visible behaviour of the
glass does not change -- tapping a folder still plays that folder, in
that order, from the top -- but the thing being filled is the queue, and
an MPD client watching `idle playlist` sees it happen. `playlist.c`'s
internal array is then read *from* the queue rather than from `readdir`,
or, more likely, `playlist.c`'s array simply becomes the queue's array
and the file shrinks to the sort-and-filter half of what it does now.

**Entries are never freed while they may be read.** The pointer problem
above is solved by the queue owning its strings for the life of the
boot, or by copying rather than borrowing at the two places that
currently borrow (`playlist_peek_next()` into `prefetch_next()`, and
`playlist_path()` into `request_track()`). Copying is the smaller
change and 512 bytes on a heap allocation is not the expensive part of
prefetch; it should be done *before* anything can mutate the queue from
a socket, as its own patch, and it stands on its own merits even if
MPD is never finished.

**Ownership: ui_task mutates, everyone else asks.** The established
pattern is `remote.c`'s: a `SemaphoreHandle_t s_mu` (`remote.c:880`)
guarding a one-slot request (`remote.c:565-567, 746-751, 670-681`),
drained by `ui_task` at the top of its pass (`player.c:7004-7043`), plus
an `xQueueSend(..., 0)` for presses that is dropped when full
(`remote.c:757`). The MPD server task copies it exactly and **never
calls `playlist_*` or `request_track()` itself**.

But a one-slot mailbox is wrong for a queue. `add` three hundred times
is three hundred requests, and dropping them silently -- which is what
the transport queue does -- would be a client's queue quietly losing
tracks. Two consequences:

- the mutation request carries a **whole command**, not a keystroke, and
  a `command_list_begin`/`command_list_end` block arrives as one;
- the server task **waits for the request to be serviced** before
  answering `OK`, because MPD is synchronous: a client that gets `OK`
  from `add` and then `playlistinfo` expects to see the track. A
  `xQueueSend` with a real timeout, and an `xSemaphoreTake` on a
  completion, rather than fire-and-forget.

That is a blocking call on a socket task, which is fine there and would
not be on `media_task`.


## Two transports, one mutation path

The remote page and the MPD server both want to change the queue, and
`remote.c`'s existing handoff cannot simply be duplicated for the second
one. `remote_take()` (`remote.c:1005-1023`) and `remote_take_open()`
(`remote.c:670-682`) are **single-consumer by construction** -- they
read and clear -- and the queue and mailbox they drain are `static` to
`remote.c`. A second server growing its own pair means `ui_task` polls
two sets of take functions at `player.c:7012` and `:7692`, two mutexes,
and two orderings of requests that arrive in the same pass, with nothing
saying which wins.

So the mutation path is **exported once and called by both**: a small
module (or exported push helpers) owning the mutex, the request queue
and the completion, with `ui_task` draining it in one place at the top
of its pass, the way it drains `remote_take_open()` today. `remote.c`
and `mpd.c` become two producers into it and neither owns it.

Concretely on the remote side, `remote_cmd_kind_t`
(`remoteproto.h:46-57`) gains the queue verbs beside `REMOTE_CMD_LS`,
`_OPEN` and `_PLAYDIR`, the socket grammar gains `add|addnext <path>`,
`qdel N`, `qmove N M`, `qclear` and a `q` listing answered in frames the
way `ls` is (`remote.c:644-663`, `LS_FRAME` 24 KB), and `remote.html`
grows a queue section below the fold beside the chooser. The page is
27 KB now and every byte of it ships in the binary, so the queue view
should reuse the chooser's row rendering and its shared-prefix elision
rather than getting its own.

The ordering that follows: **the remote's queue verbs come before
MPD's**, because they exercise the same mutation path against a
transport that already exists, with a page that can be driven headless
by the fake socket the chooser was tested with. By the time `mpd.c`
opens a listener the queue has been used in anger.


## What the protocol layer looks like

`mpdproto.c` -- pure C, no ESP-IDF includes, host-testable -- and
`mpd.c`, the socket and the task. Split for the reason `remoteproto.c`
is split from `remote.c` (`remoteproto.h:8-12`), and so that the
grammar can be fuzzed and mutation-checked the way `texttest/README.md`
requires.

**In `mpdproto.c`:** the greeting, argument tokenising (MPD quotes with
`"` and escapes with `\`, which `remoteproto.c`'s "one verb, one word"
parser does not do), the command table, `OK`/`ACK` framing with the
`ACK` error codes, command lists (`command_list_begin` and
`command_list_ok_begin`, which differ in whether each sub-command gets
its own `list_OK`), and the `key: value\n` serialisers for `status`,
`currentsong`, `playlistinfo` and `lsinfo`.

**There are twelve `ACK` codes, not five.** This file said five until
5153 built the thing and read `src/protocol/Ack.hxx`: five is the low
block, and the high block -- `NO_EXIST`, `PLAYLIST_MAX`, `SYSTEM`,
`PLAYLIST_LOAD`, `UPDATE_ALREADY`, `PLAYER_SYNC`, `EXIST` -- is the one a
queue and a card actually raise. Corrected here rather than only in
`ARCHITECTURE.md`, because the wrong number in a plan gets built.

**`remoteproto_path_ok()` is reused after a mapping, not verbatim.** It
requires an ABSOLUTE VFS path under a mounted volume (`/sd/...` or
`/usb/...`) and **an MPD URI is relative to one library root**, so the
two agree only once something has bridged them. `mpduri.h` (5154) is
that bridge, and the reuse happens on the mapped path before anything
touches a filesystem.

**A URI is an index path, unchanged**, and this was never an open
question -- 5153 wrongly recorded it as one. `mediaindex.h:10-16` says
paths are relative to the volume root *because* SD and USB are shown to
MPD as one library, SD preferred, and that the part below the mount is
the only thing by which a track on one volume can be recognised as the
same track on the other. The index format was chosen for this, and
`medialist.h` merges on exactly that key. So: no volume in a URI, no
leading slash.

The cost, which is MEDIA-INDEX.md point 3's and not a new one: a
relative path on both volumes resolves to the SD copy and the USB copy
has no URI at all. And a consequence worth knowing before writing
`lsinfo`: **a directory URI is never mapped to a VFS path.** Listing is
an index operation -- `medialist_open()` takes the relative directory
and opens nothing -- so only a file being played forces a volume to be
chosen.

**Not reused: the JSON.** `remoteproto_state_json()` emits a browser's
object; MPD wants lines. But `remote_state_t` (`remoteproto.h:96-119`)
already carries every field `status` and `currentsong` need -- state,
volume, elapsed, duration, title, artist, album, path -- filled once per
`ui_task` pass at `player.c:8070-8085`. The right move is to export that
snapshot (a `remote_state_snapshot()`, or have `remote_publish()` hand
the filled struct to an `mpd_publish()` beside it) so there is one
place the player's state is read, not two.

**The UTF-8 repair in `put_str()` (`remoteproto.c:162-190`) is needed
here too.** A browser closes a WebSocket carrying invalid UTF-8; an MPD
client will not close the connection but will show mojibake or, in some
clients, drop the response. Latin-1 ID3 tags off a card produce exactly
this. Same function, different escaping.

### The version to claim

The greeting is `OK MPD <version>`, and clients gate features on it.
Claiming a high version and then `ACK`ing half of what it implies is
worse than claiming a low one: a client that believes in `albumart` and
`readpicture` will ask. The number to claim is the lowest that covers
the verb list actually implemented, decided when the list is final, and
written in one place with a comment saying which verb forced it.

### Ports, sockets, and the switch

Port 6600 collides with nothing. The 80/443 dance
(`remote.c:906-907`) is specific to the captive portal wanting port 80
(`portal.c:615-621`); MPD needs no part of it. The listener comes up on
the same edges as the remote -- `want && have_ip()`, polled from
`ui_task` (`player.c:7001`) -- behind its own settings switch copying
`settings_remote_enabled()` exactly (`settings.c:383-390`, three places:
the read, the key-parse and the write), off by default, card not NVS.

**Socket budget is the real constraint. SETTLED IN 5157: the limit was
raised.** And the census in this paragraph was wrong when it was written,
which is the more useful part.

It said: `CONFIG_LWIP_MAX_SOCKETS` unset, so IDF's default of 10, with
portal httpd 4, portal DNS 1, remote HTTPS 4 and remote plain 2 already
spoken for -- "eleven if everything were up, which is *why* the plain
server yields".

**An httpd instance costs `max_open_sockets + 3`**, three being reserved
for the server's own working (`esp_http_server.h`, enforced per instance
in `httpd_main.c`), so those figures are 7, 1, 7 and 5. The portal and the
plain server never run together, so the worst case is the setup phase --
portal 7 + DNS 1 + remote HTTPS 7 = **15 against a ceiling of 10**, before
SNTP, a stream or a radio-browser search adds one each. IDF checks each
instance and never the sum, and every server passes its own check, so
nothing warned. That was already broken with no MPD in the image.

`main/netbudget.h` now holds the census as arithmetic and
`sdkconfig.defaults` sets 24, which leaves MPD a listener and three
clients -- three rather than two because an idling client holds its socket
indefinitely and the realistic case is a phone, a desktop client and one
left open. Raising it was preferred to budgeting down because the cost is
the static socket table and not a buffer per socket.

The task: 4-6 KB, the portal/remote precedent (`portal.c:902`,
`remote.c:306`), priority 3. It must never call `netdec_open()` --
`NETDEC_MIN_STACK` is 24576 (`netdec.h:150`) and `netdec_open()`
measures its caller and refuses, which is the right failure but not one
to design into.


## `idle` is not optional, and it is where this gets subtle

`idle` is how every modern client avoids polling: the client sends
`idle`, the server says nothing until something changes, then names the
changed subsystems and the client re-reads. Getting it wrong makes MALP
and ncmpcpp feel broken in a way that looks like a network fault.

Three things follow:

- **An idling client holds a socket open indefinitely**, which is the
  other half of the socket budget above.
- **Events must be latched per connection, not broadcast.** A change
  that happens between a client's `OK` and its next `idle` must still be
  delivered on that `idle`; a client that misses a `playlist` event
  shows a stale queue forever. Each connection carries a bitmask of
  pending subsystems, set by the publisher, cleared when reported.
- **`noidle` must be answerable while the connection is blocked**, which
  means the read is the thing that wakes, not a sleep.

Subsystems this device can actually raise: `player`, `mixer`,
`playlist`, `options`, `update` (the reindex, `medialib_busy()`,
`medialib.h:95`), `database` (a completed reindex), and `output`. The
publisher is `remote_publish()`'s neighbour: it already computes "has
anything changed" once per `ui_task` pass with the position-slip rules
(`remote.c:977-993`), and that same diff feeds the idle mask.

**Corrected in 5160, which built it.** `output` is not one of them: there
is one output and nothing can switch it, so nothing raises it. `database`
is a reindex that *changed* something, not any completed one -- MPD raises
it only when an update modified the database. And the remote's slip rule
is the wrong one to reuse for seeks: it measures against a clock reset on
each send, so a stalled stream would read as a seek every two seconds.
`idle` compares one pass with the next instead (`mpdidle.h`).


## The library side: what MPD asks that the index already answers

`MEDIA-INDEX.md` settled the storage and 5010-5019 built it: the catalog
`.defeatist.cat` (`mediacat.h`), the fixed-width path-ordered index
`.defeatist.ix2` with `midx_seek()` / `midx_find()` / `midx_child()`
(`mediaindex.h:494, 519, 538`), the walk, reconcile, and a reindex on
mount.

What is **not** built is a query layer above it. There is no function
that answers "list this directory, both volumes merged, SD preferred" --
which is `lsinfo`, and which `MEDIA-INDEX.md` already decided the shape
of (point 3, "Two volumes: one merged library, SD preferred"). That
layer is the prerequisite for `lsinfo`, `listall`, `listallinfo` and
`playlistinfo`'s tag fields, and it is useful to the web UI on its own.

And the **search file does not exist** (`MEDIA-INDEX.md` point 2: one
plain line per track, lowercased tags and the catalog offset, scanned
without a JSON parser, rebuilt from the catalog like the index). MPD's
`search` and `find` are whole-card questions and `MEDIA-INDEX.md`'s own
closing paragraph says the index is justified by search and not by
browsing. So the search file is not a nicety here; it is the thing the
index was built for.

`listallinfo` is deprecated upstream and should be answered but not
optimised for. `lsinfo` being per-directory is the saving grace already
noted.

**5191 reversed the merge, for MPD only.** Asked for on the board: the
USB copy of a path both volumes hold could not be reached, and every
`add` was tried on the SD first. MPD's URIs now start with the volume --
`sd/Album/01.mp3`, `usb/Album/01.mp3` -- and the library root is two
folders, as MPD shows two mounts. The index, the search file, the
on-device chooser and the remote page are unchanged; `medialist.h`
still merges, and MPD now asks it for one volume at a time. The cost is
the one point 3 named: an album on both volumes is two albums to a
client.


## The staging

Numbers from **5124** (5123 is the last; check `git log` and the series
heading at the end of `ARCHITECTURE.md` before taking one, per
`CLAUDE.md` -- the 1000 series was restarted once by a session that did
not look). Each of these is a patch or a small run of them, and the
order is chosen so that every step is independently justifiable and
nothing is left half-built if the series stops.

1. **Copy, do not borrow, at the two prefetch/request sites.** Removes
   the use-after-free that a mutable queue would otherwise create, and
   is correct on its own merits today. No MPD in it at all.
2. **The query layer over the index**: merged two-volume directory
   listing and single-path lookup, SD preferred, in the order
   `midx_path_cmp()` defines (not `strcmp`, and not the chooser's
   folders-first order -- `MEDIA-INDEX.md`, "The order is not strcmp").
   Host-testable against a synthetic index.
3. **The search file**, derived and rebuilt beside `.ix2`, with the
   version-in-the-name rule (`medialib.h`, `MEDIALIB_OLD_INDEX_NAMES`).
4. **`mpdqueue.c`**: the queue with ids, and `playlist.c` reduced to a
   view of it. The glass behaves identically; nothing listens on a
   socket yet. This is the largest and riskiest patch in the series and
   probably wants to be two -- the queue, then the switch-over -- with
   the commit message saying plainly that restructuring was the smallest
   correct change (`CLAUDE.md`).

   **Done in three**: the queue in 5136, a test pinning `playlist.c` as it
   was in 5164, and the switch-over in 5165, which passes that test
   unchanged. The paths live in the queue; the cursor, the shuffle
   history and the folder's name stay in `playlist.c`, as `mpdqueue.h`
   always said they would. MPD still shows a window of one, because its
   task cannot read a queue with no lock -- that needs a copy handed over
   by `ui_task`, which is its own patch.

   **That copy is 5166**, and the reason it came next was a board report:
   Cantata greys its Next button unless `status` names a `nextsongid`, and
   a window of one never could. Clients now see the whole queue, read-only;
   adding to it and reordering it are step 11.
5. **The shared mutation path**, exported and drained once by
   `ui_task`, with `remote.c` moved onto it. No new behaviour; it is
   the refactor that lets there be two producers.

   **Done in 5170**, as `uireq.c`: one ring for presses from both, with
   MPD's sequence numbers and completion moved in, and the remote's
   one-slot choice beside it. `remote_take()`, `remote_take_open()` and
   `mpd_take()` are gone. The one difference is that presses from the
   two are taken in arrival order rather than remote-first.
6. **The remote page's queue verbs**: the socket grammar, the frames,
   the section below the fold. First real use of the queue, over a
   transport that already works, driven headless in test.

   **Done in 5171-5174**: cursor-keeping edits in `playlist.c` (5171),
   a lock on the list with copies off `ui_task` (5172), the verbs as far
   as `ui_task` (5173) -- by id, not position -- and the queue sent to
   the page with its controls (5174).
7. **The four modes.** The `play_order_t` ↔ `random`/`repeat`/`single`/
   `consume` mapping, written down as a table with the states that have
   no analogue named explicitly. Needed by MPD; the remote page can
   show them too, since it already has the order control.

   **Done in 5156**, out of order -- it needs no board and `status` had
   four booleans nothing filled. What came out of it: the four orders are
   each EXACT in MPD's flags, so the glass loses nothing on the way out;
   the reverse loses in four of eight rows; **repeat-all has no analogue**
   and is the setting a client is most likely to reach for; and `consume`
   is a loss on top of any row, being a change to the queue rather than
   to the walk over it. `status` reports what the device will actually do
   rather than what was asked for, so an unrepresentable toggle springs
   back -- the argument is in `ARCHITECTURE.md`.

   **And settable from a client in 5168**, once Cantata's buttons asked:
   `repeat`, `random`, `single` and `consume` go through the same table
   to a play order, applied on `ui_task`. A request the device cannot do
   springs back and raises `options` so the client re-reads.
8. **`mpdproto.c` and `mpdprototest`**: grammar, quoting, command lists,
   `OK`/`ACK`, and the serialisers, with nothing on a socket. `run-mpd`
   into `all:` and `.PHONY`, the binary into `clean`, both the
   warnings-only `-O2 -Werror` pass and the sanitiser pass
   (`texttest/Makefile:30-37, 394-399`), and the test written
   independently of the implementation rather than sharing its
   assumptions.
9. **`mpd.c`**: the listener, the task, the settings switch, the panel
   row, the socket budget decision. `status`, `currentsong`, the
   transport verbs, `setvol`.

   **Done in 5158**, before steps 4-6, so the queue a client sees is one
   entry long -- what is on screen -- until the switch-over. The socket
   budget was 5157's, taken first as its own patch; `MPD_CLIENTS` is
   `netbudget.h`'s number rather than a second copy of it.
10. **`idle`**, with the per-connection latch.

    **Done in 5160**, with `noidle`, from MPD 0.20's source. `noidle`
    turned out not to be a verb at all, and came out of `mpdproto.c`'s
    table.
11. **MPD's queue verbs**: `add`, `addid`, `delete`, `deleteid`,
    `move`, `moveid`, `playid`, `clear`, `shuffle`, `plchanges` --
    which by this point is a mapping onto step 5's path, not new
    machinery.

    **Done in 5175**, as that mapping, plus the one thing it lacked:
    an edit's outcome, so `addid` can answer with the new id and a
    refusal can be the right ACK. `playid` and `plchanges` were already
    there (5166). Not taken: adding a folder, shuffling part of the
    queue, and 0.23's relative positions.
12. **Browsing and search**: `lsinfo`, `listall`, `find`, `search`,
    `list`.

    **Browsing done in 5176-5177**: readers of the index that a reindex
    waits for (5176), then `lsinfo`, `listall` and `listallinfo` (5177).
    Search in 5180 (`search`, `find`, `count`, with partitions and
    mounts), and `list` in 5182: step 12 is done.
13. **Stored playlists**, if at all: `load`, `save`, `listplaylists`.
    The device has `starred.m3u` (1207-1209) and `stations.m3u`, so the
    format is not new, but this is the first thing on the list that is
    optional.

    **Done in 5184**, because Cantata asked on every connect: the six
    verbs of the table, over `Playlists/<name>.m3u` at a volume's root,
    beside `Recordings/`, with library URIs in them.

### Where it stands

Every step has a first version, and the patches after the staging were
board reports from Cantata 2.5 against it:

- **Tags on the queue** (5185, 5187): `playlistinfo` and friends read
  each entry's title, artist and album from the catalog; the playing
  entry falls back to it where the player has none. Genre, date, album
  artist and track number are not in the catalog, so a client shows
  them as unknown -- `tagtypes` says so.
- **The chooser lists stored playlists** (5186) in a volume's
  `Playlists` folder, and a tap loads one as MPD's `load` would.
- **Presses and edits land behind a page** (5188-5190, 5193-5194). A
  page on the glass -- the chooser opens itself at boot with nothing
  to play -- used to hold every MPD press, and every edit queued behind
  one, until it was closed. Now a pass behind the page takes them, runs
  the amplifier's idle check and publishes, and does so twice a second
  while a client is connected so `status` keeps time. The glass and a
  client can fight; the last press wins.
- **Two folders, not one library** (5191), above.
- **Taking a volume out** (5192) drops its entries from the queue and
  raises `database`, `mount` and `stored_playlist`.

Known and left:

**Every verb in MPD 0.20's command table is answered** (5227-5231,
checked against the list in src/command/AllCommands.cxx): each one
either does what MPD does or gives MPD's own refusal for a server
without the feature. What still separates this server from 0.20 is
behaviour, and all of it is outside mpd.c:

- **Seeking is by percent.** `status` reports elapsed in whole seconds,
  and every seek -- `seekcur`, and since 5229 `seek`/`seekid` of a
  song not playing, which is played and then sought -- lands on the
  nearest hundredth of the track. A client's progress bar can snap back
  by up to about a second. Needs a millisecond seek, and a
  start-at-offset, in the player.
- **The modes are the player's four orders** (MPD.md step 7): repeat
  alone (repeat-all) and consume spring back, and `status` says what
  the player will do. Needs the player to have those orders.
- **Tags the index does not keep**: genre, date, album artist (served
  as the artist), track and disc number. `list genre` is empty and a
  client shows them as unknown. An index-format change
  (MEDIA-INDEX.md).
- **No lengths in the catalog**, so `count` and `stats` say no
  playtime and a queue entry that is not playing has no duration.

Refused on purpose, each with its reason: turning the one output off,
crossfade and MixRamp other than off, a second partition, mounting,
`prio`/`prioid` (the player's random has no use for them), `rangeid`,
`addtagid`/`cleartagid` (MPD's own answer for a local file), `kill`,
`config` (local clients only, and every client is TCP), `sticker` (no
database), a shuffle of part of the queue, `load` of a range, `group`
on `find`. `listfiles` gives no `size` (a 32-bit off_t, 5228).

Newer than 0.20 and not claimed: filter expressions and `albumart`
(0.21), `readpicture`, `binarylimit`, `outputset` (0.22). Some newer
things are answered anyway because answering costs nothing -- `getvol`
(5218), `save`'s mode (0.24) -- but the greeting stays 0.20 until 0.21's
two are done.

Not yet driven on the board: `list` (5182), `search`/`find` (5180),
Cantata's stored-playlists view (5184), and everything from 5218 on.
`tools/mpdcheck.py <ip>` (5225-5226, extended in 5232) runs all of it
and puts the listener's queue, volume and song back.

## What would make this not worth building

Worth writing down, as `MEDIA-INDEX.md` did. **Full queue semantics is
the expensive half, and it is the half the device may not want.** This
is a player with a screen, and the screen's model is "a folder is what
plays". A queue a phone can reorder is a second model of what is
playing, and steps 1, 4, 5 and 7 above exist entirely to make the two
models one.

The question to ask is not "does MPD need a queue" -- it does -- but
**"does the remote page want one"**, and that one is answerable now,
without writing any protocol code. If the answer is yes, steps 1-6 are
justified by the page alone and MPD inherits them; if it is no, then
neither surface wants a queue and the whole expensive half comes out
together.

In that case steps 4, 5, 6, 7, 11 and 13 come out, `playlist.c` is
left alone, MPD's queue verbs are `ACK`ed as unsupported, and what
remains is steps 2, 3, 8, 9, 10 and 12 -- a browse-and-control MPD
server that is perhaps a third of the work and carries none of the
use-after-free risk. Step 1 stays either way, because it is a fault
waiting regardless.

Step 4 is the point of no return, and the remote page reaches it first.
