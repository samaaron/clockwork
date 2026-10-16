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
| **cold restart** (a rebuild) | destroyed and built again | regions of lifetime `rebuild` reset; the rest kept | statechange `restarting`, then `setup` with the new generation |
| **cold boot** (a new arena) | built | every region reset, including lifetime `cold boot` | statechange `booting`, then `running` |

A hot restart is a device change the instance can take without being
rebuilt: the device stops, is reopened or replaced at the same rate and
channel count, and audio resumes. Nothing a client tracks has changed,
so clockwork says statechange and nothing more.

A cold restart is `destroy_dsp` then `init_memory` on the same arena then
`dsp_new` (src/audio_processor.cpp). Today every rate change, every
channel count change and every driver change is cold, because the guest
ABI gives a guest no way to say it can take one live. Offsets never move:
a reader that resolved a region at open holds it across every restart.

A cold boot is a new arena: the first boot, or `teardown_memory` followed by
`init_memory`. The regions a rebuild keeps start over here.

## The generation

Every guest instance has a generation. It starts at 1 and moves on at every
cold restart. It is the key to everything below: a region of lifetime
`rebuild` belongs to one generation, a refusal names the generation that
refused, and a client rebuilds against the generation `setup` named, with
nothing from an earlier one taken for the current one.

Today the generation lives in one atomic in ClockworkEngine and is on the
wire in `setup` alone. The contract is that it is also:

- published in `clockwork-state`, lifetime `arena`, odd while there is no
  guest instance and even while there is one, as the channel map's
  seqlock already does, so a reader knows "between instances" from the
  bytes with no event to miss;
- carried on `restarting` (the generation closing) as well as on `setup`
  (the one opened);
- stamped on every refusal clockwork makes on the guest's behalf.

The arena header's `instance_id` is a different axis: which engine, for a
process that will one day hold more than one. It stays 0 and is not a
generation.

## Every region, at every event

The lifetime fact in the arena table (docs/ARENA.md, `writeArenaHeader` in
src/shared_memory.h) is the rule, and test_arena.cpp pins the table. This
is what the rule means, region by region, with what a reader or client is
told. "Kept" means the bytes are exactly what they were. "Reset" means
re-initialised by clockwork, not necessarily zero.

| region | lifetime | hot restart | cold restart | told how |
|---|---|---|---|---|
| header and table | arena | kept | kept, offsets never move | n/a |
| clock anchors (NTP start, drift, global offset) | cold boot | kept | kept: the host's, not the instance's | n/a |
| clock state (tempo, origin, transport, meter) | cold boot | kept | kept: a device change does not change the session | n/a |
| node id counter | cold boot | kept | kept: ids are the clients' | n/a |
| guest config | cold boot | kept | kept, handed to the new instance as it was | n/a |
| guest persist | cold boot | kept | kept, handed to the new instance as it was | n/a |
| metrics | rebuild | kept | counters reset to 0 | n/a |
| native stats | rebuild | kept | reset | n/a |
| sample clock | rebuild | kept, continues | rebound to the new device, continues | n/a |
| channel map | rebuild | kept | rewritten for the new device | its seqlock |
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
   a restart depends on some of them. One of clockwork's own found in the
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

## What the guest owes

One hook, at teardown: **answer what you still hold**. Whatever the guest
has accepted and not yet answered is answered, in its own words, before
`dsp_free` returns. Clockwork's part is that those answers leave on the
rings before the rings go (rule 3). What the guest holds is its business;
the next guest implements the same hook and nothing else.

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
- test/osc_channel_no_instance.test.mjs and test/osc_channel.spec.mjs: the
  browser's boundary, in node over a real engine and in a page.
- rust/clockwork-abi/tests/layout.rs: the status value, held to the C
  header by the compiled probe.
- test/test_arena.cpp: the table every row's lifetime is read from.

## Not yet

The generation is not yet published in the arena, and `restarting` does
not yet carry it; the refusal carries it once it exists. The guest's
teardown hook ("What the guest owes") is not yet in the ABI: a guest's
own unanswered work still dies with it.
