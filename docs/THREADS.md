<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Threads

How the threads around an engine reach it on the web, and why they need no
client slots.

**Agreed and built 2026-10-13.**

## What a thread outside the engine does

On the web, a thread other than the audio worklet does exactly four things,
to three rings:

- **write ingress.** The main thread and each worker given a channel send
  OSC to the engine.
- **drain RT egress and NRT egress.** The reply pump takes the engine's
  replies, and peels the route word off each one.
- **watch ingress.** The traffic logger reads what was sent without taking
  it, keeping its own place in the ring.

Everything else such a thread touches it already reads straight from
JavaScript: the metrics, the clock, the node id counter and the guest's
window.

## Why that needed slots

Those four things were done by clockwork's compiled client
(`clockwork_client`), and compiled C can only use the memory of the module it
runs in. To reach the rings it has to run in the engine's memory, so it needs
its stack there too. Each thread therefore started a second copy of the engine
module over the engine's memory, and claimed a *client slot*: 72 KB of the
engine's memory for its stack and handle.

That cost more than the job is worth:

- the slots region is in every arena. Native and embedded builds also reserve
  576 KB for it, and never use it.
- a second copy of the engine module on every such thread
- a stack inside the arena, with no guard page. An overflow runs into the
  next slot.
- a fixed number of slots, eight, which threads have to give back, including
  across a reload

## The change

The four operations are written in JavaScript, in `js/lib/ring.js`, following
the frame format in `src/ring/ring.h`:

| job | in `ring.js` | the C it restates |
|---|---|---|
| write ingress | `ringWrite` | `RingBufferWriter::write` |
| drain RT and NRT egress | `EgressReader` | `clockwork_client_poll` |
| watch ingress | `RingTap` | `clockwork_client_tap_poll` |
| (all three walk with) | `ringDrain` | `clockwork_drain_ring` |

A thread outside the engine still holds the engine's `SharedArrayBuffer`. It
no longer runs any compiled clockwork code. The worklet is unchanged: it runs
the engine's own module, so it calls the C.

Messages flow exactly as they did: the same rings, the same lock on ingress,
the same cursors, the same counters. Only the code that walks the rings
changed.

This restates about 150 lines of C. Clockwork had removed an earlier
JavaScript copy because the two could drift apart unnoticed. Two tests answer
that:

- **The example writes.** The C++ already replays the corpus of example
  writes, `test/fixtures/ring_wire.txt`, byte for byte. The JavaScript
  replays it too.
- **Against the real C.** A node test (`test/ring_cross.test.mjs`) boots the
  engine and runs both on the same rings:
  - C writes and JavaScript reads;
  - JavaScript writes both egress rings, and `EgressReader` and
    `clockwork_client_poll` read them, with the same deliveries, cursors and
    counts;
  - a C tap and a JavaScript tap watch the same ring, and must see the same
    frames and miss the same number.

  - every writer and reader position near the ring's ends, with every frame
    size up to 96 bytes, over bytes earlier frames left behind.

  Sizes are random, so the ring wraps many times. The random seed is fixed,
  so a failure happens again the same way.

Seven mistakes were planted in `ring.js` to check the tests catch them, and
every one failed a test. The corpus on its own caught only one of the first
three: it has few cases, and it starts from a zeroed ring.

`test/web_threads.test.mjs` then holds the whole path: what a channel writes
is what the engine processes, what the engine says is what the reply pump
reads, and a tap takes nothing from the engine.

The other way to remove the slots was to build the C a second time as a small
module with its own memory, reaching the rings through imported calls. That
means changing the C so every native caller goes through an accessor, adding a
second wasm target and writing glue code. For this much logic it is out of
proportion.

### What goes

- the client slots region
- `WasmClient`, and every second copy of the engine module
- `clockwork_client_open_memory_in` and `clockwork_client_sizeof`, which only
  the slots used. (Version 2 of the arena then renumbered its regions in memory
  order, so there is no gap where they were.)
- the engine's module and memory in a channel's transferable
- the limit of eight, and the bookkeeping that gives slots back

## Still more than one writer

This change leaves these as they are. They need one decision, taken for
native, embedded and the web together:

- **Ingress.** The main thread, workers, native transports and an embedding
  host all write ingress, and they share one lock. Without shared memory the
  worklet writes it too. A sender stopped halfway through a write leaves the
  lock held.
- **NRT egress.** Any engine worker that logs writes it.
- **The node id counter.** Every channel takes from it with `Atomics.add`.

Natively, the peer plane (`src/shm_peer_plane.h`) already shows the
alternative: a ring per sender, with one writer each and no lock.

## How it is checked

Every one of these runs after a change to the rings, on either side:

- clockwork's native suites, which replay the corpus through the C++
- clockwork's node tests: the corpus, the cross test and the whole path
- clockwork's browser suite, in both modes
- SuperSonic's suite, which builds on clockwork's client
- string_lid's conformance, which runs clockwork's suite through its tools
