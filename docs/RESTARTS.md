<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Restarts

An engine restarts in two ways, and this document says what each one does
to every region of memory, what a client is told, and what the guest owes.
It is written in clockwork's terms only: a guest instance, the arena and
its regions (docs/ARENA.md), messages in with a token, replies out, a
device, an engine state and a generation. What the guest keeps in its
regions, and what its messages mean, is the guest's business and is not
described here. SuperSonic works to this contract today; the next guest
inherits it by implementing the one hook in "What the guest owes".

## One event, every route

Clockwork is built on the assumption that an instance is replaced at any
moment, and as often as anything cares to: a device unplugged mid-phrase,
a page reloaded, a plugin instantiated again by its host, a rebuild that
fails and goes back, two in a row before a block has run. So every rule
here holds on the least a host offers, not the most: the smallest rings
any profile builds (src/memory_profile.h: 4 KB for clockwork's replies on
an ESP32-S3), one thread for the audio and everything else, a browser with
no shared memory, an audio thread that never waits on a lock. A mechanism
that needs more than that is not one of these rules.

A guest instance is one conversation, and its generation is its name.
Replacing it ends one conversation and starts the next. However it is
replaced — a device change on a native engine, a reload in the browser, an
embedded host attaching again — it is the same event, told the same way:

1. **The old generation closes.** The generation word goes odd, and
   everything still waiting for the old instance is answered once, then:
   refused to its sender, naming the generation that closed, or, for a
   message about clockwork itself (a ping, a sync), carried to the next.
2. **The old instance's last words follow everything it said before,** on
   its ring.
3. **The new generation opens, and its first word on that ring is
   `setup`.** Every reader of the ring hears the old instance, then
   `setup`, then the new instance, because that is the order they sit in.
4. **Nothing a reader has not read is lost.** The reply rings outlive
   instances, and no reader is reset under a writer, nor a writer under a
   reader.

Two kinds of message go out, and only two. The instance's words go on the
instance's ring, and a reader places them by where they fall against
`setup`. Clockwork's own notices go on its ring, and each either belongs to
no instance (device reports, inputs, log lines, hosted tracks) or names its
generation (statechanges, refusals); none needs a place against `setup`.

"Between instances" is one fact with one representation: the generation
word's parity. A client needs no timer for any of it. It stops sending to
the guest on `restarting` or on its first refusal, rebuilds once on
`setup`, and a refusal tells it what to send again.

Where the code is still behind this, it is listed under "Not yet", with
the test that shows it.

## Three events

| event | the guest instance | the arena | said to clients |
|---|---|---|---|
| **hot restart** | survives | untouched, every region | statechange; and when a fact clients compute with has changed (the rate), a notice of it naming the same generation |
| **cold restart** (a rebuild: a native device change, a browser reload over the same memory, an embedded host attaching again) | destroyed and built again | regions of lifetime `rebuild` reset; the rest kept | statechange `restarting` with the generation closing, `setup` with the one opened, as the new instance's first word, and statechange `running` as the swap ends |
| **cold boot** (a new arena) | built | every region reset, including lifetime `cold boot` | statechange `booting`, then `running` |

A hot restart is a device change the instance can take without being
rebuilt: the device stops, is reopened or replaced at the same rate and
channel count — or at another, when the instance declared it survives
that kind of change and is told — and audio resumes. The instance and the
generation are the same; a client is told the facts that changed, and
nothing to rebuild.

A cold restart is `destroy_dsp` then `init_memory` on the same arena then
`dsp_new` (src/audio_processor.cpp). A driver change is always cold. A
rate change or a channel-count change is cold unless the guest declared
it survives that kind of change (`DspInfo::survives`, src/dsp_api.h): then
the device is reopened, the instance is told the new facts with audio
stopped (`dsp_reconfigure`), clockwork lays out again the regions of its
own that follow the device — the scope's format, the sample clock, the
channel map's device words — and nothing else moves: a hot restart, with
the instance and every client's picture of it kept. An instance that
refuses what it declared is destroyed and built again from there, the
cold restart it would have been. A guest built for one rate and one
channel count declares nothing and is not wrong to; scsynth is one.
Offsets never move: a reader that resolved a region at open holds it
across every restart.

A cold boot is a new arena: the first boot, or `teardown_memory` followed by
`init_memory`. The regions a rebuild keeps start over here. A cold boot
that replaces a running engine — the browser's reload into new memory, an
engine booted again in the same process — is still the one event above for
its clients: the old generation closes (in the browser, the old worklet
refuses what waited for it before its memory goes, `clockwork_close`), the
new arena carries the generation on, and its instance says `setup`.

`clockwork_init` on an arena that is already up is a rebuild, whoever calls
it: an embedded host attaching again, a page reloaded over the same memory
(whose new module finds the old one's state where it left it), a host's own
device change. The instance goes, what waited for it is marked, and the next
is built and says `setup`.

## The generation

Every guest instance has a generation. It starts at 1 and moves on at every
cold restart, and never goes back: a new arena carries it on from the last
generation its process opened, or from one its host hands over from memory
that has gone (`clockwork_carry_generation`: the page's reload into new
memory). It is the key to everything below: a region of lifetime
`rebuild` belongs to one generation, a refusal names the generation that
refused, and a client rebuilds against the generation `setup` named, with
nothing from an earlier one taken for the current one.

It lives in the arena header, the one region a rebuild keeps
(`ClockworkArenaHeader::generation`, src/clockwork_arena.h), as one word:
`2g+1` while there is no guest instance, `2g` while there is one, `g` the
generation the last build opened. A new arena holds 1: no instance, none
opened. The lanes move it, one step each way, at the two moments that
matter: even to odd at the start of `destroy_dsp`, before anything else of
the teardown, and odd to even once `dsp_new` has succeeded. A reader in any
process reads it with one load and knows "between instances" from the
bytes, with no event to miss: `clockwork_arena_generation` and
`clockwork_arena_has_instance` in C, `instance_generation` and
`has_instance` on the Rust header, `readGeneration`, `instanceGeneration`
and `hasInstance` in js/lib/arena.js. Clockwork keeps no other count and no
other flag: ClockworkEngine reads the same word, every boundary reads its
parity to know whether there is an instance, and the web's worklet reports
it at the boot handshake for a page with no shared view of the arena.

On the wire it is:

- on `setup` (`,iii rate buffer generation`): the generation opened;
- on `statechange` (`,ssi state reason generation`): on `restarting` the
  generation closing, on `running` after a rebuild the one opened,
  otherwise the one the instance has;
- on every refusal clockwork makes (`/clockwork/error ,ssi address reason
  generation`): the generation at the moment of refusing, whatever refused
  and wherever. One shape, so a client parses one thing. A refusal made
  between instances names the generation that closed, since none has
  opened.

The arena header's `instance_id` is a different axis: which engine, for a
process that will one day hold more than one. It stays 0 and is not a
generation.

## Every region, at every event

The lifetime fact in the arena table (docs/ARENA.md, `writeArenaHeader` in
src/shared_memory.h) is the rule, and test_arena.cpp pins the table. It is
also what runs: `init_memory` walks the table and resets each region at the
event its lifetime names, from one list of resets (`kRegionResets` in
src/audio_processor.cpp), so nothing else decides, and a region added to
the table without a reset fails test_restarts.cpp by name. An arena is zero
when it is made (static storage, a fresh mapping, the web's claim); a cold
boot on one that was used before is the walk's to reset. This is what the
rule means, region by region, with what a reader
or client is told. "Kept" means the bytes are exactly what they were.
"Reset" means re-initialised by clockwork, not necessarily zero — and for
the transport area, a fresh epoch in the cursors rather than bytes, since a
producer in another thread may be mid-write through a rebuild.

| region | lifetime | hot restart | cold restart | told how |
|---|---|---|---|---|
| header and table | arena | kept | kept, offsets never move; the generation word moves on (odd at the teardown, even at the build) | the word itself |
| clock anchors (NTP start, drift, global offset) | cold boot | kept | kept: the host's, not the instance's | n/a |
| clock state (tempo, origin, transport, meter) | cold boot | kept | kept: a device change does not change the session | n/a |
| node id counter | cold boot | kept | kept: ids are the clients' | n/a |
| guest config | cold boot | kept | kept, handed to the new instance as it was | n/a |
| guest persist | cold boot | kept | kept, handed to the new instance as it was | n/a |
| metrics | cold boot | kept | kept: a count of what a teardown lost is still there after it; the gauges are rewritten within a block | n/a |
| native stats | rebuild | kept | to 0 | n/a |
| sample clock | rebuild | kept, continues | emptied, then rewritten whole by the first block on the new device | its seqlock |
| channel map | cold boot | kept | kept: the streams bound over channels outlive the instance; the device's own words are rewritten when the instance is built, as any attach rewrites an entry | its seqlock |
| scope | rebuild | kept, continues | reformatted at the new device's channels; the write position restarts at 0 | a reader's cursor starts again |
| taps | rebuild | kept | every slot free | the slot state |
| ring cursors, status flags | rebuild | kept | the status flags: a fresh word; every ring's cursors go with their ring | n/a |
| in ring | cold boot | kept, drained as usual | kept: what waited for the old instance is refused by the next drain, below the teardown's mark, naming the generation that closed; clockwork's own is answered | `/clockwork/error` to the token |
| RT out ring, NRT out ring | cold boot | kept | kept: what the old instance said stays for its readers, and the new instance's `setup` follows it | the replies themselves |
| guest window | rebuild | kept | zeroed; the new instance lays it out again | the window's identity reads 0 until it does |

And the areas outside the arena, and the engine's own stores:

| area or store | hot restart | cold restart | told how |
|---|---|---|---|
| assets | kept | every slot released | `/clockwork/asset/released` per slot |
| returns | kept | what the instance published is gone | n/a |
| guest fast and bulk memory | kept | the pool is torn down; the new instance carves again from zero | n/a |
| scheduler store | kept | cleared | `restarting` means the schedule is empty from that generation |
| port routing | kept | kept; the new instance is told what it missed | the streams' own attach replies |
| guest sinks | kept | closed | n/a |

## The transport area, exactly

Messages are the one place a restart can lose something a client is
waiting on, so these are the rules a client may rely on:

1. **No message is dropped without a word.** A message for the guest that
   cannot reach a guest is answered to its token with `/clockwork/error`,
   the message's address, and one of three reasons, each a fact
   (`CLOCKWORK_REFUSED_*` in src/clockwork_sys.h):

   | reason | the fact | where it is said |
   |---|---|---|
   | `no instance` | it arrived while there was no instance: between a teardown and the next build, or after a build that failed | at the boundary, as it is written |
   | `rebuilding` | it was waiting in the in ring when the instance was torn down | the next drain, of everything below the mark the teardown set |
   | `stalled` | it was waiting while the audio thread was stalled, and is stale now | the next drain, of everything below the mark the wake's purge set |

   Clockwork has the address and the token, which is all a refusal needs;
   it never needs to know what the message meant. A client reads every one
   as "wait for `/clockwork/setup`", never as an error to show.

   The boundary is the same function for every writer
   (src/in_ring_door.h), and what it reads is the generation word's
   parity, under the ring's writer lock: odd from the start of
   `destroy_dsp` until `dsp_new` succeeds. A client in another process
   reads the same word. The teardown marks the in ring under the same
   lock once the word is odd: a writer that took the lock before wrote
   below the mark, and the next drain refuses its message as the closing
   generation's; one that takes it after reads the word odd and is
   refused at the door. The in ring is kept across a rebuild, so nothing
   is reset under a writer, and the drain refuses what is below a mark as
   the instance's ring has room for the refusals, holding everything after
   them until it has: thousands waiting are all answered, a block's worth
   at a time. Marks are set only where the drain is not running — by a
   teardown, with the audio thread stopped, and by the wake's purge, on
   it — and several may wait for one drain: an instance built and torn
   down before a block ran leaves its own as well as the one before it.

   A shm peer writes a ring of its own (shm_peer_plane.h), which the
   control pass takes to the in ring's door — a pass a cold restart parks,
   so the plane is taken only once the swap is over. The restart marks the
   plane as well (src/lanes/ring_marks.h, one design for both rings): where
   the teardown found it, as the closing generation's (`rebuilding`), and
   where the build found it, as written between instances (`no instance`).
   The pass refuses what it finds below either mark, naming the generation
   that closed, and takes the rest to the door, which refuses a message once
   if there is still no instance.

   Where a door can reach the asker's egress it writes the refusal there
   and also says so to its caller; where it cannot, the caller speaks:

   | door | to the asker | to the caller |
   |---|---|---|
   | `clockwork_client_send`, and `clockwork_client_send_begin`/`_commit` at the commit (C, Rust, the shm peers) | the status is the word | `CLOCKWORK_E_NO_INSTANCE` / `Error::NoInstance` |
   | `ClockworkEngine::ingest` (every socket transport, `sendOSC`, the NIF) | `/clockwork/error` to the token | the status |
   | `clockwork_ingress_write` (the lanes ABI) | `/clockwork/error` on the NRT egress for the source | `false` |
   | `OscChannel.send` (the browser) | `/clockwork/error` through the client's inbound path, from the main thread's channel; `onRefused` and `lastRefusal` on any channel | `false` |

   A refusal at a door that finds no room on clockwork's ring is lost, and
   counted as an egress frame refused; one too long to encode is counted
   (`clockwork_sys_refusals_dropped`). The metrics live as long as the
   arena, so a count made at a teardown is still there after the rebuild.
2. **Clockwork's own verbs keep working.** Device, state, notify and the
   rest of the `/clockwork/` namespace are answered throughout a restart;
   a restart depends on some of them. A control verb never touches the in
   ring on a host with a control pass: it has a ring of its own ("The
   control ring" below), of lifetime `arena`, that a restart leaves alone.
   One of clockwork's audio-thread verbs that is not the guest's business —
   a ping, an echo, a sync, the clock snapshot, a port listing, what plays
   a hosted track — found in the in ring below a teardown's mark is
   answered by the next drain, in its order, rather than refused. A verb
   that hands something to the guest — an asset, an inbound event, a
   scheduled message or the schedule's flush — was written for the old
   instance as much as the guest's own messages beside it, and is refused
   with them. A bundle is never clockwork's (clockwork_prefix.h), so a
   bundle is refused.
3. **Everything an instance says goes out on its ring, in order, and
   `setup` cuts it.** A reply carries no generation of its own, so a client
   tells which instance made it by where it falls against `setup`. The RT
   out ring is the instance's: the audio thread writes it, and while a
   restart has the audio thread stopped, the restart thread does
   (`destroy_dsp`, `rebuild_dsp` in src/audio_processor.cpp) — so what the
   old instance says as it goes (its last answers, its assets released)
   follows everything it said before, and `setup` is the last thing
   `rebuild_dsp` writes, naming the generation opened, before the new
   instance can say anything. Every reader of the ring — the engine's
   gateway, a host reading the rings itself, a tap — hears the old
   instance, then `setup`, then the new instance, because that is their
   order in the ring. A ring with no room for `setup` then (a reader far
   behind) holds the new instance silent until it has room: nothing of the
   instance's goes ahead of it.
4. **The NRT out ring carries nothing whose meaning depends on the cut.**
   Clockwork's own replies, device reports, inputs (MIDI, Link, gamepad,
   OSC), log lines and hosted tracks belong to no instance; statechanges,
   refusals and the notice of a fact a hot restart changed name their
   generation. So `restarting` and `running` need
   no place against `setup`: `running`, said as the swap ends, comes after
   it for the engine's own reader, which reads nothing until audio resumes,
   and may come either side of the new instance's first replies.
5. **Nothing is lost at the cut.** The reply rings outlive an instance
   (lifetime cold boot): what a reader had not read when the rebuild came
   is still there, ahead of `setup`. The engine's own reader, parked for
   the swap, is handed what has been said at the teardown, so clients hear
   `restarting` while a device opens. A reader that is let go stops before
   anything new is written for it to take: the page's readers are told
   to stop in memory they read every time they wake, and are heard to
   have stopped before the page lets them go (js/lib/sab_worker_loop.js),
   since one parked in `Atomics.wait` hears neither a message nor its own
   termination at once.
6. **The schedule is empty from `restarting`.** Timed messages held for
   the old instance are cleared, not refused one by one: the store can
   hold thousands, and a client that halts on `restarting` re-sends its
   future after `setup`.

A build with no guest at all (`CLOCKWORK_SYNTH` off) never lowers the flag:
every message for a guest is refused with `no instance`, which is the
truth, where before it was drained and dropped.

## The control ring

Clockwork's own verbs are of two kinds. The audio thread performs some
inside its block, because they are about the block: the liveness ping and
echo, the ordering barrier (`sync`) and the clock snapshot, the schedule and
its flush, an asset hand-off, an inbound event, what plays a hosted track.
Those ride the in ring with the guest's traffic, in order with it. Every
other verb of clockwork's is CONTROL — `notify`, `devices/*`, the session
tempo, a subsystem's verbs, a track's edits — and is answered by the
control pass. The rule is one function, `clockwork_sys_is_audio_verb`
(src/clockwork_prefix.h), read at every boundary and pinned in
test_clockwork_sys.cpp.

A control verb used to ride the in ring too, and be forwarded to the
control pass by the audio thread, which meant it waited for an audio
block: a device that stopped, a callback that stalled, a reopen blocked
inside a driver all silenced the engine's own voice, the boot handshake
with it (2026-10-15: Spider waited 30 s for `notify.reply` while the
device was mid-reconfiguration). Now a control verb has a ring of its own
in the arena (`CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING`, its cursors at
its front), of lifetime `arena`: a restart neither resets it nor has
anything to carry out of it. The control pass drains it directly, and its
wait is bounded to a millisecond rather than tied to the audio thread's
word, so a verb written while no block renders is answered within one.

The engine SERVES the ring: once its control pass drains it, it says so in
the cursors region (`STATUS_CONTROL_SERVED`), and every boundary — the
lanes ABI, the client ABI in any process — writes a control verb there
from then on. Until it is said, and in a build with no control pass (the
worklet, the lanes alone), every verb rides the in ring as it always did,
and the audio thread forwards control along its chain. Lowered at
shutdown. A control verb fired by the scheduler is the other kind of
arrival: it fires on the audio thread, inside its block, and the audio
thread still hands it to the control pass with the block's time.

Pinned in test_control_plane_liveness.cpp (`[control-ring]`: a control
verb answered while no audio block renders, an audio-thread verb not),
test_client_abi.cpp and test_restarts.cpp (which ring each kind of write
moves, with the word up and down), and test_clockwork_sys.cpp (the rule).

## What the guest owes

At teardown: **answer what you still hold**. Whatever the guest has
accepted and not yet answered is answered, in its own words, before
`dsp_free` returns (src/dsp_api.h says so on `dsp_free`). Clockwork's part
is that those answers go out on the instance's ring, ahead of `setup` (rule 3).
What the guest holds is its business; the next guest owes the same and
nothing else.

And, if it declares it survives a device change (`DspInfo::survives`):
**take it** when told (`dsp_reconfigure`), or refuse and be rebuilt.

A guest keeps across a rebuild only what it writes into `guest persist`.
Everything else it holds is the instance's and goes with it.

## What a client does

- On `restarting`, stop sending to the guest. Read every `rebuilding`
  refusal as "wait for `setup`", never as an error to show.
- On a notice of a fact a hot restart changed, take the new fact. Nothing
  is rebuilt: the instance is the one it was.
- On `setup`, rebuild in one pass against the generation it names. A retry
  budget is counted against that generation alone: a pass that could not
  reach the engine at all, because the machine slept or the device was
  down, is not a failed pass of this generation.
- Nothing is restored for a client, and nothing in flight is silently
  lost, so a client needs no timers to guess with.

## Where it is pinned

The model is pinned as a grid: every reader against every way an instance
is replaced or survives, one deterministic case per cell, each checked by
the same function (test/RestartModel.h). Around its event a cell has the old
instance answer something its reader has not yet read, has three messages
waiting — the guest's, a sync, an asset's commit — and asks the new instance
one more thing after. The native cells are test/test_restart_grid.cpp, on
fake devices that keep the case's clock; the embedded host's are
test/test_restart_grid_embed.cpp. A cell marked with a number is red until
that row of "Not yet" is done, and its test says so ([not-yet-N] and
[!shouldfail], `test.fail` in the page's): it passes while it fails, and
fails the day it passes, so the mark goes with the fix.

| reader | rate change | driver change | another device, survived | bigger buffer, survived | device that will not open | device that never calls back | sent between instances |
|---|---|---|---|---|---|---|---|
| the engine's own | ✓ | ✓ | ✓ | 13 | ✓ | ✓ | ✓ |
| a host reading the rings | ✓ | ✓ | ✓ | 13 | ✓ | ✓ | ✓ |
| a tap on the instance's ring | ✓ | ✓ | ✓ | 13 | ✓ | ✓ | |
| a peer on the segment's plane | ✓ | | ✓ | | | | ✓ |

And the cells that are not one event among others: thousands of messages
waiting at a rebuild, for the engine's own reader and a host reading the
rings (✓); a peer whose reply ring is nearly full when a rebuild comes (✓);
a peer writing while no instance comes up (✓); an embedded host attaching
again, at its rate and at another (✓); a client
writing its message in place between instances (✓, in
test/test_restarts.cpp); an engine booted again in the same process (✓, in
test/test_engine_lifecycle.cpp); and the page, in
test/restart_grid.spec.mjs and test/osc_in_reload.spec.mjs, at a reload over
the same memory and into new memory, and purging what waited while it was
suspended (✓). The NIF's cell comes with its fix (11).

The same tests run against the smallest rings a profile builds — a native
tree configured with the ESP32-S3's sizes (`-DCLOCKWORK_IN_RING_SIZE=32768
-DCLOCKWORK_RT_OUT_RING_SIZE=8192 -DCLOCKWORK_NRT_OUT_RING_SIZE=4096` in
the C and C++ flags) — where every rule must hold as well. They are sized
by what the ring holds, not by a desktop's.

- test/test_restarts.cpp: every region's row and the transport rules, at
  the lanes level, against the mechanism itself (`destroy_dsp` +
  `rebuild_dsp`).
- test/test_restart_refusals.cpp and the `[ColdSwap]` client ABI cases:
  the same rules through a whole engine's cold swap, where the order of
  the teardown, the drains and the rebuild is the engine's; there too,
  that a client hears `setup` before anything the new instance says, and
  that a host reading the rings itself gets, before `setup`, what the old
  instance said that it had not read — its answers and its assets
  released.
- test/test_restart_sequence.cpp: one sequence whatever started it — the
  user's switch, the watchdog's recovery, the rate-skew recovery.
- test/test_guest_reconfigure.cpp and the `[survives]` cases of
  test/test_device_policy.cpp: a change the instance survives is hot and
  told; one it refuses is the cold restart it would have been.
- test/test_control_plane_liveness.cpp `[control-ring]`,
  test/test_client_abi.cpp and test/test_restarts.cpp `[control-ring]`:
  a control verb answered while no audio block renders, and which ring
  each kind of write moves.
- test/osc_channel_no_instance.test.mjs and test/osc_channel.spec.mjs: the
  browser's boundary, in node over a real engine and in a page.
- rust/clockwork-abi/tests/layout.rs: the status value, held to the C
  header by the compiled probe.
- test/test_arena.cpp: the table every row's lifetime is read from, and
  the generation word's meaning; test/test_restarts.cpp `[generation]`
  and the engine case in test/test_restart_refusals.cpp: how it moves, and
  that `restarting`, `setup` and every refusal carry it; test/arena.test.mjs
  and the Rust header's tests: the same word read on the other sides.

## Not yet

What a guest owes at teardown is the guest's to do: scsynth's unanswered
asks still die with its world until SuperSonic answers them in `dsp_free`.

On clockwork's side, today's code is behind the model at these places.
Each is a suspect until a deterministic test shows it, and the ones the
grid above shows say so; the fix follows the test.

| # | where | against the model |
|---|---|---|
| 4 | the worklet's `checkStatus` | stores the status word whole, wiping the no-instance flag |
| 8 | the shm peer plane | its reply ring drops what does not fit when its peer is behind, at a rebuild or not; holding the engine's replies for one slow peer would hold every other client's, so it is left for a round of its own |
| 9 | the client ABI | a client in another process has no sanctioned read of the generation |
| 10 | the plugin bridge (parked with tracks) | refusals carry generation 0; a track's errors are another shape, broadcast; timed track events vanish with the schedule; track tap slots go dark when the taps region resets |
| 11 | the NIF | a send while there is no instance returns `not_running`, every status it does not name mapped to it (read, not yet tested) |
| 12 | the page's boundary | the page's channel and the worklet read `STATUS_NO_INSTANCE`, a second record of what the generation word's parity already says; every C door reads the word |
| 13 | a hot restart that changes a fact | told only as a statechange; an embedded host's cached rate goes stale. **Shown**: a switch to a bigger buffer, survived, says nothing of the buffer (the notice the grid expects is `/clockwork/setup/changed ,iii rate buffer generation`) |
| 14 | clients | Spider's one-second quiet period, its retries and timeouts; the GUI's fifteen-second re-enable; the browser's one-second settle; the page's sync sent six times because "the first message after a reload can go missing" (it could: the page's own reader, terminated and still running, took it — now stopped before it is let go): each covers a loss the engine should not have, and goes once the engine has none |
| 16 | Clockwork's ring through a long swap | the engine's own reader is parked for the whole swap, and the swap's own words (its log lines, the device reports) wait on Clockwork's ring until audio resumes; a desktop's 64 KB holds a long swap's, the smallest profile's 4 KB does not, and a few are lost. **Shown**, with the smallest rings: a device that never calls back, and a change the instance refuses |

"Generation" also names, elsewhere in clockwork, the bridge's spawn count,
the peer plane's attach count, the clock grid's version and the channel
map's seqlock. Each is renamed on the way past, so the word means one thing.
