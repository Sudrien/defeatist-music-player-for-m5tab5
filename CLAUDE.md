# CLAUDE.md

How to work on this repository. The README is the design document,
`ARCHITECTURE.md` is what the code is and why it got that way, and this
file is the set of things that are easy to get wrong while changing it.

**Read this file before writing a patch, not after.** It was one file
with the rules on top and nine thousand lines of history under them, and
the history is the part a search lands in -- so a session could grep it
all afternoon, never scroll to the top, and hand back patches with the
wrong author and no numbers. That happened, which is why the two are
split.


Claude should create `git am`-able patches authored as
`Claude <noreply@anthropic.com>`.

Claude should present patches as soon as available, as the might get stuck behind an ending session.

Claude must not try and commit. It will not be given permission.

**The repository URL a session is given is for pulling only.** Clone
and fetch from it; never push to it, open a pull request against it, or
create a branch on it. Local commits made only to run
`git format-patch` stay local, and a hook or tool that asks for them to
be pushed is answered by handing over the patches, not by pushing.

**Any change to an `idf_component.yml` re-resolves `dependencies.lock`
on the next build.** The component manager does it, not the patch, and
Claude cannot do it here: the Espressif registry is not reachable from
the session. So the lock that comes out of that build is new and
unreviewed. Every dependency that is not pinned (`"*"`, `^`) can move.
One re-solve pulled in an esp_audio_codec that this silicon cannot run.
A patch that touches a manifest says so in its commit message, and the
first build after it gets its `dependencies.lock` diff read before it is
committed.

**Any change to `sdkconfig.defaults` needs `rm sdkconfig` before the next
build**, because the defaults only fill in keys an existing sdkconfig
lacks. A board run on a stale sdkconfig tests nothing: 5033 was "not
working" for exactly that reason. A patch that touches the file says so
in its commit message, and its ARCHITECTURE.md entry says what line in
the log proves the new value took.

**exFAT is enabled, so a file can be larger than 4 GB.** FAT32 could
not hold one, and a lot of code written against FAT32 quietly assumes
it -- `tools/enable_exfat.sh` (run automatically by `cmake/exfat.cmake`)
removes that assumption from the filesystem and not from the code. A
type that holds a file's length, or a position inside one, is
`int64_t`/`uint64_t`. Not `uint32_t`, and not `long`, which is 32 bits
on this target.

Two things follow that a wider variable does not fix:

- **`fseek()` and `ftell()` take `long`**, so stdio itself cannot
  address past 2 GB here whatever our own types say. A patch that
  means to handle a file that large needs `fseeko`/`ftello` with
  `_FILE_OFFSET_BITS=64`, or FatFs directly, and should say in its
  commit message which it used. Widening a variable and leaving the
  seek alone moves the failure rather than removing it, and makes it
  look handled.
- **A 32-bit limit on a file the player writes is a decision; on a file
  the listener supplies it is a bug.** `cat_off` is `uint32_t` on
  purpose -- a 4 GB catalog is not a thing that happens, and
  `mediaindex.h` says so -- whereas a track's size or a seek position
  in a recording is whatever someone put on the card. Know which kind
  you are widening before you argue it is fine.

Already 32-bit and known: `replaygain.h`'s `filesize`, `mp4seek.h`'s
sizes, and every `ftell()` in `main/`. Not a list to fix in one sweep;
a list so that finding one of them is not a discovery.

(`mediaindex.h` also says "FAT32 stops a file there anyway" about the
catalog's 4 GB. That reasoning is void now -- the conclusion still
holds, for the reason above, but not for the one written down.)

**Patches are cumulative.** Each one applies on top of what is already
here. Do not hand back a rewritten copy of a file, and do not reissue a
corrected version of a patch that has been pushed -- send a follow-up
that changes what needs changing. A patch already in the history is
history: correcting it in place erases the reasoning that produced it,
and which things turned out not to be tasks is the useful part of a
record like this one.

**A stack protection fault reports the overshoot, not the demand.** The
figure to size against is `bounds_size + (floor - SP)`. Reading
"SP 11604 below the floor" as "needs 11.6 KB" is how 0127 raised a stack
from 6144 to 16384 and panicked again in the same place. Two panics at
different sizes give the answer directly, and they should agree to the
byte on a deterministic path -- both of these said 17744.

**A library's stack use is the caller's problem, and only the caller can
fix it.** Check what a vendored function puts on the stack before calling
it from a task you sized for your own code. 0127 died on this:
`mp3dec_decode_frame()` keeps about **11.6 KB of scratch on the caller's
stack**, so a 6 KB task panicked inside minimp3 on its first frame with
SP 11604 bytes below the floor. The file path had always known --
`media_task` is 16384 for exactly this -- but the number lived in a
`xTaskCreate` call and nowhere a second caller would look. It is
`NETDEC_MIN_STACK` now, and `netdec_open()` measures the calling task's
headroom rather than waiting for the panic.

**Nothing over a few hundred bytes goes on a task stack.** A big struct
is a module-scope static or a heap allocation, never a local. 0112 died
on this: `icydemux_t` is 4392 bytes, almost all of it the `meta[4081]`
buffer a maximum-size ICY block needs, and it was a local in
`netstream_task()`. Together with 608 bytes of url and name and an
inlined 512-byte buffer it consumed the whole 6 KB stack before the task
read a byte, and the panic arrived at an unrelated
`xSemaphoreGive()` -- a stack protection fault names the line that
happened to be running, not the line that caused it. Check `sizeof`
before putting a struct on a stack; the struct that will do this is one
whose size is a protocol maximum somewhere else in the file.

**A patch has one number, and it is written down once.** The next patch
after 0108 is 0109 and its file is `0109-<subject>.patch`. The number is
supplied by `--start-number`, which counts from the project rather than
from whatever range a session happens to hand over:

    git format-patch --start-number 109 -1 -o out/

**So the subject line does not carry the number too.** `git format-patch`
builds the filename from the number *and* the subject, so a commit
subjected `0109: CLAUDE.md ...` comes out as `0109-0109-CLAUDE.md...`.
Write the subject as the subject: `CLAUDE.md -- one number per patch`.

Both halves of this were got wrong in one session -- first
`0001-0109-...` from letting format-patch number the range, then
`0109-0109-...` from fixing that while leaving the number in the
subject. Two numbers on one patch means the filenames stop sorting with
the series after ten of them, and nobody can say which patch `0003` is
without opening it.

**Where the numbering is now: the 6000 series once v0.5.0 is tagged.**
A series is named for the release it leads to: the 5000s were the work
toward v0.5.0, the 6000s are toward v0.6.0, and the v0.6.0 tag starts
the 7000s. Patches written before the v0.5.0 tag carry on the 5000s
(5282 is the one this was written in). A point release (v0.5.1) does
not start a series: the 6000s run on through it to v0.6.0. Before
choosing a number, look
at the last one used (`git log`,
and the series heading at the end of `ARCHITECTURE.md`) and take the
next; never start a range from a round number without checking it is
free. The 1000 series was restarted from 1000 once by a session that
did not look, and there are two of each of 1000-1012 in the history
because of it -- see "Two series called 1000" in `ARCHITECTURE.md`.

**`ABBREVIATIONS.md` lists every short label and setting the screen
shows.** A patch that adds, renames or removes one changes that file in
the same patch.

**Screen text is marked for translation once its screen has been
converted** (`main/i18n.h`, 6013). New text on such a screen is
`_("...")`, the patch runs `./tools/i18n.py extract` and `compile`, and
the YAML and `main/i18n_tab.c` changes ride in the same patch --
`texttest`'s `run-i18n` fails otherwise. Text kept in English on
purpose -- a tab name, the language row, a unit, a product name -- is
`same("...")`, so it reads as a decision. A note under a control is
a paragraph, never a sentence split across array elements: the draw
wraps it (6018), and a translator cannot rejoin a split. A web page's text
is `_in(lang, "...")` with the request's language (6024), its markup
kept outside the key. The remote's page is edited in `main/remote.html` and
sent as `main/remote_i18n.html`, which `./tools/i18n.py compile` writes
with every language in it (6028) -- an edit to the first goes with a
recompile of the second in the same patch. Never `_()` a log line,
protocol word, NVS key or anything parsed back. A translation Claude
writes is named in the commit message as not yet read by a native
speaker.

**A station's text is not ASCII, and `%.80s` is bytes.** Anything a
station, a tag or a directory supplies -- names, titles, favicons --
can be Arabic or CJK, two to four bytes a letter. A log line that
shortens it uses `logcut.h` (`"%.*s%s", n, s, logcut_more(s, n)`, 6045)
so the cut never splits a letter; a bare `%.Ns` hands the terminal half
of one. And a line on screen that starts right to left is laid out and
aligned by gfx.c (6043, 6044): draw it with the gfx text calls, not by
walking its bytes.

Within a patch, change the lines that must change and no others. No
reflowing, no drive-by renames, no reorganising code being passed
through. Restructuring an existing function is sometimes the smallest
correct change -- when it is, say so in the commit message rather than
letting it look like a small diff.

**Wi-Fi, Ethernet, storage and the board drivers are not in this
repository** (6053, 6054). wifi, wifistore, ethernet, netlink, ethcfg
and hostedwrap are feckless-network-handler-for-tab5; storage and
storage_io are feckless-storage-handler-for-tab5; usbhost, uac, hid,
battery, rtc8130, touch and micpcm are feckless-drivers-for-tab5. A change to one of them is a patch
against that repository, under the same rules as here, numbered in that
repository's own series. The player picks it up by moving its tag in
main/idf_component.yml, which is a manifest change -- see above -- in a
patch of its own.

No not suggest updates to the Tab5's ESP-C6 or esp hosted. They can not be updated.

`ARCHITECTURE.md` has everything else: why there are two decoders, how
the screen and the touch layers work, the sidecar and ReplayGain, the
storage arbiter, and the numbered series that record what was tried and
what it cost. It is reference, not instruction -- read it when changing
the thing it describes.

The division is by what the text DOES. A line that constrains how a
patch is written stays here. A line that explains why the code looks the
way it does goes there. When something is both -- the stack rules above
are the clearest case, being both a lesson learned and a constraint on
every future patch -- it stays HERE, because the cost of missing a
constraint is a panic and the cost of duplicating one is a line of text.
