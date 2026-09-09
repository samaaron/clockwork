<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# The arena

The arena is the one span of memory an engine and everything that observes
it share. Natively it is a block of process memory, or a mapped segment when a
client lives in another process; in a browser it is the static buffer in the
wasm heap that the page sees through a SharedArrayBuffer; under an embedded
engine it is whatever the host handed over. Whichever it is, its first bytes
are a table that says what it holds. This document is that table's contract.

(Not to be confused with `DspConfig::arena`, the guest's own memory, which
lives outside this span — see BOUNDARY.md. The word predates both uses.)

## Two halves

```
0 ┌──────────────────────────────┐
  │ header: magic, version, table│  4 KB reserved
  ├──────────────────────────────┤
  │ THE CLOCKWORK BLOCK          │  what clockwork keeps for its hosts and clients
  │   control · metrics · stats  │
  │   clock anchors · clock state│
  │   sample clock · channel map │
  │   IN ring · OUT · NRT-OUT    │
  │   audio taps · track taps    │
  │   client slots               │
  ├──────────────────────────────┤ block_bytes == guest_offset
  │ THE GUEST REGION             │  what the guest is handed base and length to
  │   guest config · window      │
  │   scope streams · persist    │
  └──────────────────────────────┘ arena_bytes
```

The line between them is the fence. The guest is in-process, on the audio
thread, and a SharedArrayBuffer has no page protection, so ownership is a
matter of which pointers are handed out: the guest receives base and length
of the regions in its half (`DspConfig::guest_config`, `shm_window`,
`persistent`, and the scope slots it claims), plus read-only pointers to two
things in the block it needs every block (`clock`, `channel_map`). It is
never given a pointer it may write through into the block.

Every table entry names its **owner** — clockwork, the guest, a client, or
the host (which writes once, at boot or device start). A tool that colours a
memory map reads that from the table; so does a reviewer.

## The table

`src/clockwork_arena.h` is pure C and is the definition. Its shape:

| field | meaning |
|---|---|
| `magic`, `version` | `'CWAR'`; the version this table was written to |
| `header_bytes` | the reservation at the front; the first region starts here |
| `instance_id` | which engine, for a process that will one day hold more than one; 0 |
| `arena_bytes`, `block_bytes`, `guest_offset`, `guest_bytes` | the halves |
| `entry_count`, `entry_bytes`, `state` | the table, and whether it may be trusted |
| `entries[]` | `{id, offset, bytes, owner, geom[12]}` per region |

Regions are looked up **by id** (`ClockworkArenaRegion`), never by position.

The **audio taps** are the block's reason to exist for most clients: two
`shm_audio_buffer` slots, `CLOCKWORK_TAP_OUT` (what left for the device this
block, after the guest and after clockwork's own routing) and
`CLOCKWORK_TAP_IN` (what arrived from the device, as the guest saw it),
written inside every tick on every host — the native callback, the worklet's
quantum, a DAW's process — at the device's live channel count up to the
slot's ceiling (`CLOCKWORK_TAP_CHANNELS`, 8 on the desktop, 2 on the web). A
client draws the master mix, records it, or meters the input from these and
asks the guest for nothing. A reader keeps its own cursor and catches up
losslessly; the ring holds a second.

One region deserves a note. The scope-stream slots a client numbers are one
index space across two regions: the guest's `SCOPE` region carries indices
`[0, slots)` in the guest region, and the engine's `TRACK_TAPS` region — the
slots it writes from plugin-track returns, natively — carries
`[first_index, first_index + slots)` in the block. Same slot shape, different
owner, different half. A reader resolves an index through both
(`ShmReaderLayout::scopeSlotAt`, `getScope` in JS, `slot_ptr` in the scope
crate); a build that carves no track taps has no entry, which reads as zero.
The geometry words carry what a reader needs to walk a region without the
engine's constants: a ring's frame magic and header size, a slot array's
count and stride, the scope ring's frames per slot. `ClockworkArenaGeom`
names each word per region.

## Who writes it, and when

Whoever creates an arena writes the header first, before anything is placed
in it: `init_memory` for the engine's own arena, the segment creator for a
mapped one (both, and the bytes are identical). `state` is stored last behind
a release fence. A reader checks magic and version, acquires, and only then
trusts an offset. Offsets never move for the life of an arena — a device
restart rebuilds the guest, not the map — so a reader that resolved a region
at open holds it until close.

## Who reads it

Everyone, the same way:

- **In-process C++** (`clockwork_client_open_memory`) reads the table even
  though it shares the engine's build, because an embedder's client may not.
- **A segment client** (`shm_segment_client`) reads the segment header for
  where the blob is, then the blob's own table for what is in it. The
  segment header keeps only what is segment-relative: the blob, the peer
  plane, the inbox and outbox.
- **The worklet** (`js/lib/arena.js`) reads it from the SharedArrayBuffer at
  boot and derives the constants the JS runtime consumes.
- **Rust** (`clockwork-abi::arena`) declares the same structs, held to the C
  header by the compiled probe.
- **A host on the lanes ABI** gets it from `clockwork_arena_header()`.

`ShmReaderLayout` in `shared_memory.h` is the C++ reader's view of the
table (`from_arena`), with `check()` pinning what a reader cannot do
without: every region inside the arena, every fixed-shape struct at least as
large as the reader's definition of it.

## Versioning

`CLOCKWORK_ARENA_VERSION` changes when the header or entry shape changes, or
when a region's meaning does. Adding a region does not: a reader ignores ids
it does not know, and a reader that needs a region the arena lacks says which.
A reader refuses a version it was not written for rather than reading through
offsets that may mean something else — `CLOCKWORK_E_VERSION` at the client
ABI, an exception from a segment client, a thrown error in JS.

## What this replaced

Three tables that each drifted in their own way: compile-time constants for
in-process code, a positional struct exported to JavaScript and read as
`uint32View[n]`, and a segment header for out-of-process peers with region
fields appended release by release. The constants still lay the arena out —
"The arena map" in `shared_memory.h`, one list in memory order — but they
write the table once, and nothing reads them but the engine and its tests.
