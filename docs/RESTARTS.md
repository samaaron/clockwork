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

## Three events

| event | the guest instance | the arena | said to clients |
|---|---|---|---|
| **hot restart** | survives | untouched, every region | statechange only |
| **cold restart** (a rebuild) | destroyed and built again | regions of lifetime `rebuild` reset; the rest kept | statechange `restarting` with the generation closing, then statechange `running` and `setup` with the one opened |
| **cold boot** (a new arena) | built | every region reset, including lifetime `cold boot` | statechange `booting`, then `running` |

A hot restart is a device change the instance can take without being
rebuilt: the device stops, is reopened or replaced at the same rate and
channel count — or at another, when the instance declared it survives
that kind of change and is told — and audio resumes. Nothing a client
tracks has changed, so clockwork says statechange and nothing more.

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
`init_memory`. The regions a rebuild keeps start over here.

## The generation

Every guest instance has a generation. It starts at 1 and moves on at every
cold restart. It is the key to everything below: a region of lifetime
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
and `hasInstance` in js/lib/arena.js. Clockwork keeps no other count:
ClockworkEngine reads the same word, and the web's worklet reports it at
the boot handshake for a page with no shared view of the arena.

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
| metrics | rebuild | kept | every counter and gauge to 0; the gauges are rewritten within a block | n/a |
| native stats | rebuild | kept | to 0 | n/a |
| sample clock | rebuild | kept, continues | emptied, then rewritten whole by the first block on the new device | its seqlock |
| channel map | cold boot | kept | kept: the streams bound over channels outlive the instance; the device's own words are rewritten when the instance is built, as any attach rewrites an entry | its seqlock |
| scope | rebuild | kept, continues | reformatted at the new device's channels; the write position restarts at 0 | a reader's cursor starts again |
| taps | rebuild | kept | every slot free | the slot state |
| ring cursors, status flags | rebuild | kept | fresh epoch, every ring empty | n/a |
| in ring | rebuild | kept, drained as usual | every unread message for the guest refused to its sender, clockwork's own carried, then the ring is fresh | `/clockwork/error` to the token |
| RT out ring, NRT out ring | rebuild | kept | drained to every client first, then fresh | the replies themselves |
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
   | `rebuilding` | it was waiting in the in ring when the instance was torn down, or a cold swap's purge found it there | the teardown's walk of the ring; the purge's drain |
   | `stalled` | it was waiting while the audio thread was stalled, and is stale now | the wake's purge, in the drain |

   Clockwork has the address and the token, which is all a refusal needs;
   it never needs to know what the message meant. A client reads every one
   as "wait for `/clockwork/setup`", never as an error to show.

   The boundary is the same function for every writer, and the flag it
   reads is in the arena (`STATUS_NO_INSTANCE` in the ring cursors, raised
   at the start of `destroy_dsp`, lowered when `dsp_new` succeeds), so a
   client in another process reads the same word. Where a door can reach
   the asker's egress it writes the refusal there and also says so to its
   caller; where it cannot, the caller speaks:

   | door | to the asker | to the caller |
   |---|---|---|
   | `clockwork_client_send` (C, Rust, the shm peers) | the status is the word | `CLOCKWORK_E_NO_INSTANCE` / `Error::NoInstance` |
   | `ClockworkEngine::ingest` (every socket transport, `sendOSC`, the NIF) | `/clockwork/error` to the token | the status |
   | `clockwork_ingress_write` (the lanes ABI) | `/clockwork/error` on the NRT egress for the source | `false` |
   | `OscChannel.send` (the browser) | `/clockwork/error` through the client's inbound path, from the main thread's channel; `onRefused` and `lastRefusal` on any channel | `false` |

   A refusal that cannot be made — no room on the egress, or an address
   too long to encode — is counted (`clockwork_sys_refusals_dropped`), as
   every refusal that cannot be made is.
2. **Clockwork's own verbs keep working.** Device, state, notify and the
   rest of the `/clockwork/` namespace are answered throughout a restart;
   a restart depends on some of them. A control verb never touches the in
   ring on a host with a control pass: it has a ring of its own ("The
   control ring" below), of lifetime `arena`, that a restart leaves alone.
   One of clockwork's audio-thread verbs — a ping, a sync — found in the
   in ring at a teardown is carried into the new epoch, first and in its
   order, rather than refused. The carry is bounded (64 KB); one that does
   not fit is refused like the guest's, and counted. A bundle is never
   clockwork's (clockwork_prefix.h), so a bundle is refused.
3. **The old generation's replies all leave before `setup`.** The egress
   rings are drained to every client after the teardown and again just
   before the rings reset (`drainEgressNow` in the cold path), so the
   teardown's refusals, and anything said in the window between, reach
   their clients, and a client never hears a reply from a dead generation
   after it has been told about the new one.
4. **The schedule is empty from `restarting`.** Timed messages held for
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
is that those answers leave on the rings before the rings go (rule 3).
What the guest holds is its business; the next guest owes the same and
nothing else.

And, if it declares it survives a device change (`DspInfo::survives`):
**take it** when told (`dsp_reconfigure`), or refuse and be rebuilt.

A guest keeps across a rebuild only what it writes into `guest persist`.
Everything else it holds is the instance's and goes with it.

## What a client does

- On `restarting`, stop sending to the guest. Read every `rebuilding`
  refusal as "wait for `setup`", never as an error to show.
- On `setup`, rebuild in one pass against the generation it names. A retry
  budget is counted against that generation alone: a pass that could not
  reach the engine at all, because the machine slept or the device was
  down, is not a failed pass of this generation.
- Nothing is restored for a client, and nothing in flight is silently
  lost, so a client needs no timers to guess with.

## Where it is pinned

- test/test_restarts.cpp: every region's row and the transport rules, at
  the lanes level, against the mechanism itself (`destroy_dsp` +
  `rebuild_dsp`).
- test/test_restart_refusals.cpp and the `[ColdSwap]` client ABI cases:
  the same rules through a whole engine's cold swap, where the order of
  the teardown, the drains and the rebuild is the engine's.
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

Nothing of this document is unimplemented on clockwork's side. What a
guest owes at teardown is the guest's to do: scsynth's unanswered asks
still die with its world until SuperSonic answers them in `dsp_free`.
