<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# The arena

The arena is the one span of memory an engine and everything that observes
it share. Natively it is a block of process memory, or the blob in a mapped
segment when a UDP port is set; in a browser it is a static buffer in wasm
memory that the page sees through a SharedArrayBuffer; in a DAW it is a
static block in the host's process. Whichever it is, its first bytes are a
table that says what it holds. This document is that table's contract.
docs/MEMORY.md is the model the table follows.

(Not to be confused with `DspConfig::arena`, the guest's own working
memory, which lives outside this span. See BOUNDARY.md.)

## Five areas, two halves

```
0 ┌──────────────────────────────┐
  │ header: magic, version,      │  4 KB reserved
  │ the areas, the regions       │
  ├──────────────────────────────┤
  │ CLOCKWORK'S HALF             │  formats clockwork defines; the same under every guest
  │  clockwork-state             │  ── public state any client reads
  │   metrics · native stats     │
  │   clock anchors · clock state│
  │   sample clock · channel map │
  ├ · · · · · · · · · · · · · · ·┤
  │  clockwork-audio             │  ── audio streams any client reads
  │   scope · taps               │
  ├ · · · · · · · · · · · · · · ·┤
  │  clockwork-transport         │  ── messages, through the client code only
  │   ring cursors · node id     │
  │   in ring · RT out · NRT out │
  ├──────────────────────────────┤ clockwork_bytes == guest_offset
  │ THE GUEST'S HALF             │  space clockwork reserves; the guest fills it
  │  guest-public                │  ── the guest's window, in the guest's format
  │   window                     │
  ├ · · · · · · · · · · · · · · ·┤
  │  guest-private               │  ── the guest's own
  │   config · persist           │
  └──────────────────────────────┘ arena_bytes
```

Four more areas lie outside the arena and are listed in the same table so
a reader finds all of memory from one place: **assets** (bulk the client
hands the guest, read in place: samples), **returns** (bulk the guest hands
the client), **guest-memory-fast** (the guest's fast memory, which it lays
out by hand) and **guest-memory-bulk** (the guest's bulk memory, the span
clockwork's allocator runs over for it; neither is read by any client, and
both are listed so a map can draw them). Three more are the program's own,
placed where the host can say (a wasm module can; a native host says
unknown until it is taught): **static-data** (natively the arena lies
inside it), **stack** (the engine thread's call stack: the linker's
reservation, which on the web is the audio worklet's, since the worklet
instantiates the module), and **malloc** (what the code took with `malloc`
and `new` up to the frontier at boot; on the web the engine's memory lies
inside it). Three more are the engine's: the **rust-heap**, the block the
RT router serves, from which clockwork's Rust subsystems allocate whenever
an engine entry point is on the stack (a sink's or a port's queues), taken
once at boot for the life of the process; and **engine-memory**, the one
block the guest's spans are carved from (on the web it holds the arena at
its start too; natively the arena is static or a segment, and the router's
block is the process's, so neither is in it).
An area inside the arena is an offset from the arena base; one
outside it is placed in a *space* — the segment, or the engine's process —
and a reader that is not in that space cannot reach it and says so.

The line between the halves is the fence. The guest is in-process, on the
audio thread, and a SharedArrayBuffer has no page protection, so ownership
is a matter of which pointers are handed out: the guest receives base and
length of the regions in its half (`DspConfig::guest_config`, `shm_window`,
`persistent`), plus read-only pointers to two things in clockwork's half it
needs every block (`clock`, `channel_map`). It is never given a pointer it
may write through into clockwork's half. Its tap streams it writes through
`DspHost::tap_write`, and clockwork does the writing.

## The five facts

Every region's entry states five things, and a tool that draws a memory
map reads them from the table rather than inferring them:

| fact | values | meaning |
|---|---|---|
| **owner** | clockwork, guest, client, host | who writes it |
| **audience** | public, transport, guest, host (a bit set) | who reads it |
| **cadence** | block, change, boot | how often the engine thread touches it |
| **tier** | fast, bulk | the RAM speed it needs; follows cadence |
| **lifetime** | arena, cold boot, rebuild | when its bytes are reset |

A **public** region is a data contract: stable, versioned by this table,
read by hand by any reader in a client process — a GUI, a spider, the web
client, a Rust host. A **transport** region is touched only through the
client code (the client ABI, or `js/lib/ring.js` on a browser thread
outside the worklet) and never parsed by anyone else. The areas say the
same thing for whole runs of memory: everything in `clockwork-state`,
`clockwork-audio` and `guest-public` is public, nothing else is, and
everything in `clockwork-transport` is transport. `clockwork_arena_check`
refuses a table where the areas and the facts disagree.

The regions:

| region | area | owner | audience | cadence | tier | holds |
|---|---|---|---|---|---|---|
| `clockwork-metrics` | state | clockwork | public | block | fast | the counters the HUD shows |
| `clockwork-native-stats` | state | clockwork | public | block | fast | DSP load, overruns, control-thread timings (zeros on the web) |
| `clockwork-clock-anchors` | state | host | public | block | fast | NTP start time, drift, global offset |
| `clockwork-clock-state` | state | clockwork | public + guest | block | fast | the Link timeline: tempo, beat origin, transport, meter |
| `clockwork-sample-clock` | state | clockwork | public | block | fast | sample position to DAC time, seqlocked |
| `clockwork-channel-map` | state | clockwork | public + guest | change | fast | live channel widths |
| `clockwork-scope` | audio | clockwork | public | block | fast | what left for the device and what arrived, every tick |
| `clockwork-taps` | audio | clockwork | public | block | bulk | tap streams: the guest's slots, then the track slots |
| `clockwork-ring-cursors` | transport | clockwork | transport | block | fast | each ring's head, tail, read cursor and sequence; the ingress lock; the status flags |
| `clockwork-node-id-counter` | transport | client | transport | change | fast | one atomic the clients share |
| `clockwork-in-ring` | transport | client | transport | block | fast | OSC into the engine |
| `clockwork-rt-out-ring` | transport | clockwork | transport | block | fast | replies written inside the tick |
| `clockwork-nrt-out-ring` | transport | clockwork | transport | change | fast | replies from the control thread |
| `guest-window` | guest-public | guest | public | change | bulk | whatever the guest publishes (SuperSonic: its node tree); its identity is in the entry's geometry |
| `guest-config` | guest-private | host | guest + host | boot | bulk | the host's settings for the guest, copied at boot |
| `guest-persist` | guest-private | guest | guest | change | bulk | what the guest keeps across a rebuild |

**The guest window's identity.** The window's layout is the guest's, not
clockwork's, so the table cannot describe its inside. It can say whose it
is: the guest declares a magic and a layout version
(`DspInfo::window_magic`, `window_version`) and clockwork copies them into
the window entry's geometry (`CLOCKWORK_GEOM_WINDOW_MAGIC`,
`CLOCKWORK_GEOM_WINDOW_VERSION`) when the guest binds. A client that reads
the window by hand checks them before it casts a pointer; zeros mean the
guest has not declared, or has not bound yet.

**The taps** are one region and one index space: a header, then the
`TAP_SLOTS` slots the guest claims through `DspHost::tap_open`, then the
`TAP_TRACK_SLOTS` the engine writes from plugin-track returns, every slot
the same shape (`shm_tap_stream`), numbered from 0 with the guest's first.
The entry's geometry carries both counts and the first track index; a
reader resolves any index as `offset + header + index × slot`
(`ShmReaderLayout::tapSlotAt`, `getTap` in JS, `slot_ptr` in the tap
crate). A build that carves no track slots says 0.

**The scope** is clockwork's half's reason to exist for most clients: two
`shm_audio_buffer` slots, `CLOCKWORK_TAP_OUT` (what left for the device
this block, after the guest and after clockwork's own routing) and
`CLOCKWORK_TAP_IN` (what arrived from the device, as the guest saw it),
written inside every tick on every host at the device's live channel count
up to the slot's ceiling. A client draws the master mix, records it, or
meters the input from these and asks the guest for nothing. A reader keeps
its own cursor and catches up losslessly; the ring holds a second.

## The table

`src/clockwork_arena.h` is pure C and is the definition. Its shape:

| field | meaning |
|---|---|
| `magic`, `version` | `'CWAR'`; the version this table was written to |
| `header_bytes` | the reservation at the front; the first region starts here |
| `instance_id` | which engine, for a process that will one day hold more than one; 0 |
| `arena_bytes`, `clockwork_bytes`, `guest_offset`, `guest_bytes` | the halves; they meet, and cover the arena |
| `area_count`, `area_bytes` | the area table |
| `entry_count`, `entry_bytes` | the region table |
| `state` | `PUBLISHED` once the table may be trusted |
| `generation` | the guest instance's generation as one word: `2g+1` with no instance, `2g` with one; 1 in a new arena; moved by the engine at every teardown and build (docs/RESTARTS.md) |
| `areas[16]` | `{area, space, base, bytes}`, base and bytes 64-bit |
| `entries[48]` | `{id, offset, bytes, owner, audience, cadence, tier, lifetime, geom[8]}` |

Regions and areas are looked up **by id**, never by position. The
geometry words carry what a reader needs to walk a region without the
engine's constants: a ring's frame magic and header size, a slot array's
count and stride, the tap ring's frames per slot. `ClockworkArenaGeom`
names each word per region.

## Who writes it, and when

Whoever creates an arena writes the header first, before anything is placed
in it: `init_memory` for the engine's own arena, the segment creator for a
mapped one. `state` is stored last behind a release fence. A reader checks
magic and version, acquires, and only then trusts an offset. Offsets never
move for the life of an arena — a device restart rebuilds the guest, not
the map — so a reader that resolved a region at open holds it until close.

Two things are written after publication, each with its own release, and
a reader that took the table earlier sees zeros that mean "not yet" and
asks again: the guest window's identity, when the guest binds
(`arenaSetGuestWindowIdentity`), and the outside areas, when the
engine knows where they are (`arenaSetArea`). A segment's creator places
the assets and the returns as segment offsets, which every process that
maps the segment can resolve; an arena in plain memory, the web's
included, gets their addresses in the engine's process at `init_memory`;
the guest's spans and the rust heap are placed once they are carved,
before `dsp_new`.

## Who reads it

Everyone, the same way:

- **In-process C++** (`clockwork_client_open_memory`) reads the table even
  though it shares the engine's build, because an embedder's client may not.
- **A segment client** (`shm_segment_client`) reads the segment header for
  where the blob is, then the blob's own table for what is in it.
- **The worklet** (`js/lib/arena.js`) reads it from the SharedArrayBuffer at
  boot and derives the constants the JS runtime consumes. The other browser
  threads find the rings from those constants (`engineRings` in
  `js/lib/ring.js`).
- **Rust** (`clockwork-abi::arena`) declares the same structs, held to the C
  header by the compiled probe.
- **A host on the lanes ABI** gets it from `clockwork_arena_header()`.
- **A memory map** draws the areas from the area table and colours every
  region from its five facts, inferring nothing.

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

Version 2 is this shape: the areas, the five facts, ids in memory order,
one tap region. Version 1 numbered regions as they were added, carried the
audience in a geometry word, kept the guest's taps and the track taps as two
regions in two halves, and had the client slots as 13.
