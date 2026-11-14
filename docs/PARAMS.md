<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Parameters

One directory (`src/params/`), one arena region, four verbs, three calls on the
guest's host struct, its readers (`rust/clockwork-params`, `js/lib/params.js`) and
the conversions they share (`js/lib/params_manager.js`), four metrics, its tests
(`test/test_params*.cpp`, `test/params*.mjs`, the crate's), its fuzz target
(`test/fuzz/fuzz_params.cpp`, with its seeds) and this page. Removing the feature
is deleting those, and what names them elsewhere: the Rust declarations
(`rust/clockwork-abi/src/params.rs`, `Client::params`), the table's part of a
copy (`clockwork_client_copy`, `js/lib/copy.js`, the worklet's), its region id
and the routes that register `param/`, its sizes in `src/memory_profile.h`, and
the dummy guest's two parameters.

## Why

A plugin host automates knobs. A MIDI controller is mapped to them. A panel
shows them and moves them. A show controller sets them over OSC. A live
coder's run defines some as it goes. All of these want one thing: a list of
named values with ranges and units, that anyone may read and anyone may set.
Clockwork publishes what an engine
is doing for readers that cost it nothing: the metrics, the node tree, the
scope. Parameters are the same kind of thing in the other direction, and sit
beside them. A guest or a client declares the set once; hosts, controllers,
panels and readers are written once and work for every guest.

None of it is about music. A value is a number the definer gives meaning to, and
Clockwork never interprets one: a pump's speed, a sensor's reading, a robot joint
and a filter's cutoff are the same to the table. A unit of its own is a custom
unit's words; the note kind, the BPM unit and the roles are there for what wants
them, and cost the rest nothing.

The table is a superset of what the plugin formats and the other describers
(JUCE, CLAP, VST3, Audio Unit, LV2, OSCQuery) can say about a parameter. A bridge
converts down: it drops what its format cannot hold, and never adds or
reinterprets a fact. Every fact is data, so anything that reads the memory can
use it, where a function a plugin supplies could only serve its own process, and
a bridge can answer a format's callback from data.

## The model

A **parameter** is an entry in a table, one table per engine (a core,
docs/ENGINES.md). `src/params/clockwork_params.h` is the shape; this is what
it says.

| Fact | What it is |
|---|---|
| `id` | a u32 the definer chooses, non-zero, stable for the entry's life: what a host automates by, what a controller is mapped to |
| `name` | up to 31 bytes of UTF-8, never empty: the short one, for a small screen |
| `kind` | continuous; stepped, whose steps are the integers in its range; switch, 0 or 1; note, a MIDI note number; choice, an index from 0 into its value names; group, which holds no value |
| `min`, `max`, `def` | f32 |
| `unit` | none, percent, decibels, hertz, seconds, beats per minute, semitones, or custom, whose words are in its text. A value is in its unit as it stands: a percent of 50 is 50%, never 0.5, and seconds are seconds |
| `flags` | automatable; hidden (a host's bridge neither lists nor sets it, a panel may show it; the table itself treats it as any other); read-only (its definer writes it, nobody else); wraps (its range is a circle); meta (moving it may move others) |
| `group` | the id of the group it sits in, one of its definer's; 0 for none |
| `taper` | how a continuous entry sweeps a knob: 0 evenly, -1 logarithmically, or a power law, a position being `((value - min) / (max - min)) ^ taper` |
| `role` | what a host or a bridge wires to it, at most one of each per table: bypass, tempo, key note, key held; none for most |
| text | any of: a full name, up to 255 bytes; a doc; a custom unit's words, up to 31; a choice's value names, up to 31 each |

A **value** is an f32, held to the range on every write and rounded for the
integer kinds, in a slot of its own beside the entry. What a value means is
the definer's business; Clockwork never interprets one. It stays an f32 though
CLAP and VST3 say double, the one place the table holds less than a format does:
the smallest boards cannot store a double in one atomic write. A float holds whole
numbers exactly up to 16,777,216 and about seven digits otherwise.

The table has a **capacity**, fixed by the memory profile
(`CLOCKWORK_PARAM_SLOTS`: 256 on the desktop and the web, 64 on the Teensy,
16 on the ESP32), **room for text** (`CLOCKWORK_PARAM_TEXT_BYTES`: 32 KB, 4 KB,
1 KB, and `CLOCKWORK_PARAM_TEXT_RUN_BYTES` for one entry's: 2 KB, 512, 512), and a
**generation** that moves every time the set of entries changes. Values moving
does not move it. A define that does not fit is refused with the reason; nothing
is dropped quietly, so a small board refuses what a desktop takes and says why.

Entries are **declared, not discovered**, at any moment. An id belongs to
whoever defined it: the guest, the host in the engine's process, or the client,
and a define of it from another of the three is refused saying so. An engine has
one client at a time (docs/TRANSPORTS.md), so the table knows "the client" and
not which connection: a client that comes back finds its entries its own, to
redefine or retire. Defining an id again
replaces its facts and keeps its slot and value (held to the new range);
defining a new id takes the first free slot; defining an id with no facts
(kind 0) retires it, freeing the slot, which the same define may reuse. An id
never moves slot while it lives. A define is all or none: one bad entry and
the table is as it was. An id may appear once in a define. A redefine says
everything again: an entry defined with no text has none.

## Facts that agree

One field per fact, and a define that contradicts itself is refused whole, rather
than leaving each reader to pick a side:

- **Range.** `min` below `max`, `def` within them, all finite. The integer kinds
  hold whole numbers; a switch is 0 to 1; a note lies within 0 to 127; a choice
  starts at 0.
- **Flags.** Automatable with neither read-only nor hidden; meta without
  read-only; wraps on neither a switch nor a note.
- **Unit.** Only on a continuous or stepped entry. A custom unit has words, and
  nothing else does.
- **Value names.** A choice has exactly one per step, and nothing else has any.
  So a choice has no more steps than one entry's text could name (a third
  of its bytes): more is a bad range.
- **Taper.** Only on a continuous entry. Logarithmic only over a range above zero.
  Even is 0 and nothing else, so 1 is refused.
- **Role.** A bypass and a key held are switches, a key note is a note, a tempo is
  in BPM; none is read-only, because something else drives it. A table holds one
  of each.
- **Group.** A name and nothing else: no value, range, unit, flags, taper, role or
  value names. An entry's group is a live group of the same definer, no more than
  `CLOCKWORK_PARAM_GROUP_DEPTH` (8) deep, with no circle. A group that has members
  cannot be retired or become anything else, unless they go in the same define.
- **Text.** Valid UTF-8, each string non-empty and within its limit, at most one of
  each kind but value names, no tag Clockwork does not know.

A set of a group is a set of nothing, and is refused.

## The region

`CLOCKWORK_ARENA_CLOCKWORK_PARAMS` (19), in the clockwork-state area: written
by Clockwork, public to every reader, cadence change, fast RAM, reset at a
cold boot only. A rebuild keeps it, as it keeps the metrics, so every value
survives a guest's restart and a guest that declares again finds its slots
and values where it left them.

A page that cannot share the memory (postMessage mode, a plugin's web view) reads
the table from a copy: `CLOCKWORK_COPY_PARAMS`, a part of `clockwork_client_copy`
(clockwork_client.h, "Copies"), the region's bytes as they stood at one generation.
A copy made while a define kept writing carries an odd generation word, which a
reader takes as "read again", so every reader reads the same bytes on every route.
The part is copied only while the page reads the table.

Its bytes, each part 16-aligned, little-endian on every target the engine runs on:

1. **A header** (`ClockworkParamsHeader`, 64 bytes). A reader that finds another
   magic or version reads nothing.
2. **The values**: `capacity` f32, one per slot, nothing else. What the audio
   thread and a panel's reader touch, so small and apart.
3. **The entries**: `capacity` records of the facts (`ClockworkParamEntry`, 96
   bytes), each a slot, `live` saying whether it is taken.
4. **The text**: the runs the entries point into.

| Header at | Field | | Entry at | Field |
|---|---|---|---|---|
| 0 | `magic`, `CWPM` | | 0 | `live` |
| 4 | `version`, 2 | | 4 | `id` |
| 8 | `capacity` | | 8 | `kind` |
| 12 | `count` | | 12 | `unit` |
| 16 | `generation`, the seqlock word | | 16 | `flags` |
| 20 | `values_offset` | | 20 | `group` |
| 24 | `entries_offset` | | 24, 28, 32 | `min`, `max`, `def`, f32 |
| 28 | `entry_bytes` | | 36 | `definer` |
| 32 | `text_offset` | | 40 | `name`, 32 bytes, ending in a NUL |
| 36 | `text_capacity` | | 72 | `taper`, f32 |
| 40 | `text_used` | | 76 | `role` |
| 44 | reserved, 20 bytes | | 80, 84 | `text`, `text_bytes` |
| | | | 88 | reserved, 8 bytes |

An entry's **run** is at `text_offset + text`, `text_bytes` long: a list of
strings, each a tag byte (1 full name, 2 doc, 3 unit, 4 value name), UTF-8 and a
NUL. A reader skips a tag it does not know.

The generation word is a seqlock over the entries and the text: odd while a define
writes, even between; the generation a client sees is the word halved. A reader
maps the region read-only, reads the word, reads what it needs, reads the word
again, and reads again if it moved or was odd. A run's position read during a
define may say anything, so a reader checks it lies within the text before
following it, and the word then says whether to believe it. A value is one aligned
word, read at any moment. The gaps a retired run leaves are closed inside the
define that needs the room, under the generation, without allocating.

`test/fixtures/params_region.bin`, which `test_params.cpp` writes and checks, holds
every reader to one set of bytes: `rust/clockwork-params/tests/reader.rs` and
`test/params.test.mjs`.

### Reading it directly

A panel, a visualiser or a dashboard draws from the memory, keeps no copy of its
own, and sends no message. A frame costs one load to see that the set has not moved,
and one load per value:

```rust
let mut table = client.params()?;          // clockwork_client: the region, read-only
// each frame
table.refresh();                           // reads the facts only if the set moved
for p in table.params() {
    let v = table.value(p.slot());         // one load from the region
    draw(p.name(), p.position_of(v), p.text_of(v));
}
```

```js
const table = cw.paramsReader();           // shared memory, or the latest copy
// each frame
table.refresh();
for (const e of table.entries) draw(e, table.value(e.slot));
```

In C, `clockwork_client_region(c, CLOCKWORK_REGION_PARAMS, &r)` gives the bytes,
`clockwork_params_header` the table, and `clockwork_params_read_entry`,
`clockwork_params_read_text` and `clockwork_params_value` read it. Anything else
can parse the layout above. In a copy a value is as fresh as the copy: the metrics'
heartbeat in postMessage mode (`snapshotIntervalMs` by default), a frame of the page
in a host's copies; everywhere else it is the memory itself.

**The metrics** say whether to look, and keep the history: `paramsLive`, the entries
now; `paramsGeneration`, the times that set changed; `paramMoves`, the times a set
moved a value; `paramsRefused`, the parameter verbs and calls refused. They read as every other metric
does, in `clockwork_client_metrics`, `getMetrics()` and the metrics history.

### What a value means

One implementation, `clockwork-params` in Rust, for every bridge and panel, so they
cannot disagree: a value's place on a knob (`position_of`, `value_at`, undoing each
other by the entry's taper), its words (`text_of`), and words back to a value
(`value_of_text`, which anything `text_of` shows reads back through). A number shows
as many decimals as its range deserves: 0 to 1 shows 0.25, 0 to 0.01 shows 0.0030, 0
to 5000 shows 1201. Middle C, note 60, is C4. On the web it is the same Rust,
compiled by `scripts/build-web-subsystems.sh`, behind `cw.paramConversions()`.

## The verbs

Four, under `/clockwork/param/`, answered on the thread that ticks the core
on every host — native, web, embedded — beside `asset/` and `sched/flush`.
A host with no control thread has only that thread, so every host uses it, and
the table has one writer and needs no lock. Each verb decides in one pass over
the frame and writes in a second: a refusal changes nothing, nothing allocates,
and the work is bounded by the capacity.

| Verb | Arguments | Does | Answers |
|---|---|---|---|
| `define` | `,b` a blob of `ClockworkParamEntry` records, the same bytes the table holds, native byte order; `live` and `definer` ignored; kind 0 retires. `,bb` when any has text: `text` is then where its run starts in the second blob | declares, replaces or retires, all or none; moves the generation | `define.reply ,ii gen count`, or `/clockwork/error` naming the reason: no room, no room for text, a bad range, a bad name, no such kind, facts that contradict each other, bad text, a role or a group that does not fit, an id another definer holds, no such parameter (a retire of one not there), malformed |
| `list` | `,i first` slot, 0 if absent | | a page of at most 16 live entries from that slot: `list.reply ,iii gen count next`, then one `entry ,isifffiiiiffii id name kind min max default unit flags group slot value taper role text-bytes` per entry, every frame to the asker; `next` is where the next page starts, 0 at the last. A page fits the smallest ring |
| `text` | `,i id` | | `text.reply ,iib id gen run`, to the asker, empty when it has none; apart from `list` because a page of runs would not fit the smallest ring. `/clockwork/error` for no such parameter |
| `set` | `,if…` id value pairs | writes the values, held to range and rounded for the integer kinds, all or none; inside `/clockwork/schedule`, in the block of its sample | nothing on success; `/clockwork/error` for no such parameter, read-only, a value that is no finite number, malformed |

A `list` is a state and a `set` of many restores one: a preset is a named
list a client keeps.

**The table is read, never announced.** A verb answers its asker and nobody
else is told: a change costs the thread that ticks the core one store, however
many readers there are and however fast a host automates. A reader sees the set
of entries change by the generation and a value by its slot, and the
`paramMoves` metric says in one load whether a set moved any value since it
last looked (a define that changes a value moves the generation instead). The guest, and a language running inside it, read a slot on the audio
thread and need nothing else. Something that follows another's moves (a plugin
bridge keeping a host's parameter on a panel's) compares each value with the one
it last passed on. A controller with no view of the memory is served by a client
that has one.

A timed set rides `/clockwork/schedule <ntp> <blob>`, as every timed verb of
Clockwork's does (a `#bundle` is the guest's), and the scheduler delivers it
with what is due by the end of the block, before the guest renders. That is
automation's timing, block-accurate and at most a block early, with no work of
this feature's own. A set with no timetag lands at the next block.

## The guest's side

A guest owes nothing to this: one that declares no entries runs as every
guest does today. A guest that uses it has three calls on `DspHost`
(`src/dsp_api.h`), each NULL on a host that lacks the table:

1. **`param_define(ctx, entries, count, text, text_bytes, slots)`**: declare,
   replace or retire, as the verb does, the guest the definer; the slots come back.
   From `dsp_new`, or from the guest's own calls on the audio thread; never
   from another thread. A restart builds the instance with the audio thread
   stopped, and the table outlives it, so a guest that declares again finds
   its slots and values. Entries it no longer declares stay until it retires
   them.
2. **`param_value(ctx, slot)`**: the slot's value now, one load.
3. **`param_set(ctx, slot, value)`**: the guest moves a value itself, a
   preset loaded or a knob of its own, for any reader to see.

The guest's ids are its own: a client's define of one is refused. A client
may set and read them. A read-only entry of the guest's is the guest's
alone to write. The dummy guest (`dsp/dummy/dummy_dsp.cpp`) declares two,
and its `/dummy/param/…` verbs are how the tests drive the three calls.
`clockwork_params::Define` lays out entries and their text for a guest in Rust.

The same three exist as C functions for a host in the engine's process
(`clockwork_param_define`, `clockwork_param_value`, `clockwork_param_set`), which
it calls as `CLOCKWORK_PARAM_DEFINER_HOST` (the definer is the caller's to say:
the table takes whichever of the three it is given). `clockwork_param_value` is any
thread's. The other two are the ticking thread's: a plugin's render call may set
a value directly before it renders, and any other thread of the host uses the
verb, as the client does and as the client. `clockwork_client_core` gives a client in the engine's
process its core.

## The client's side

A client declares entries for what lives above the guest: playpad's Play,
Tempo and Beat / Arp are the Rust player's, not the guest's; Sonic Pi's
live values are a run's; a machine's controller may have nothing else. Same verbs,
same table, same readers. What the client defined is the client's, on
whichever connection it is now: the guest and the host may set it (unless it is
read-only) and may not redefine or retire it.

## Hosts and controllers, written once

Each bridge reads the table, converts down, and calls `clockwork-params` for what a
value means. What a format has no place for it drops: AU, VST3 and CLAP have no field
for a doc, so a doc reaches a panel, OSCQuery and a help page and no plugin host.
AU's curves are fixed, so a power-law taper takes the nearest; CLAP has no taper, so
its bridge may expose a position from 0 to 1 and print the real value.

- **Audio Unit.** One `AUParameter` per live entry that is not hidden, the id as its
  address, the kind and unit mapped to Apple's, a choice's names as `valueStrings`, a
  group as an `AUParameterGroup`, the taper as a display flag. A host's render event is
  `clockwork_param_set` on the render thread, before the block; a value observer
  a `set` verb; the main thread reads the table and passes a value that differs
  from the one it last passed on back to the `AUParameter`, so a host follows a panel. `AUParameter` keeps what a host
  last set, because a verb lands at the next block and a host reads back at once.
  knobs' `au/` is this mapping. It does not yet replace the tree when the
  generation moves, because its set never changes.
- **CLAP.** One parameter per live entry that is not hidden, the id as its id, a
  group as its members' `module`, a choice as an enum, a role as what CLAP has for
  it. A tapered entry goes out as its position from 0 to 1, and its words stay the
  real value's. A host's events are `clockwork_param_set` on the audio thread, keys
  go through the key roles, and what moved in memory goes back as output events,
  read from the table with no message. knobs' `crates/knobs-clap` is this mapping,
  and its test is a host. A rescan when the generation moves (structural changes
  only while deactivated, as CLAP requires) is not built: its guest's set never
  changes.
- **VST3.** Values normalised at the edge; a restart request when the count
  changes, which hosts honour unevenly, so a VST3 shim may keep a fixed
  bank of slots it relabels.
- **MIDI.** A controller number to an id is a client's mapping and a `set`
  per message; feedback to a controller with lights is that client reading the
  values and sending what moved.
- **OSC and OSCQuery.** `list` is the discovery, `set` the control; a bridge
  speaking OSCQuery is a client with a socket.
- **A panel.** Reads the region at frame rate and sets through the verb; what
  another moved is there at its next frame.

A bridge holds no fact of its own: each is a mapping of this table onto a host's
words. The one value it keeps is `AUParameter`'s, above, which Apple's host reads
back at once; the region's value is what it comes back to.

## Many engines

One table per core. A process with several instances has several tables,
each in its own arena, with no id shared across them. That is what lets a
host make any number of a plugin and automate each (docs/ENGINES.md).

## Later, and how

- **Ramps**: a target and a frame count per slot, a third array under a
  new region version; `ramp` a verb that writes it. Sets, readers and the
  guest's calls unchanged.
- **Sample offsets inside a block**: the scheduler knows each set's sample;
  one more guest call hands over the block's sets with their offsets. A
  guest reading slots at the block's start never notices.
- **A step**: a continuous value that lands on every 0.25, as JUCE's `interval` says,
  in one of the entries' reserved words, read as none while it is 0.
- **Modulation and per-note values**, as CLAP has: a flag when the engine can do
  them, and not before, because a flag would otherwise promise what nothing does.
- **A C door to `clockwork-params`**, when a client in C or C++ draws from the table.
- **A lookup by id**: a set scans the slots for its id, bounded by the capacity.
  An index beside the values, under a new region version, if a host with many
  parameters shows it in a profile.

Two things are settled now because they could not be changed later: values
apart from facts, because moving them changes every reader; and ids that
never renumber, because every lane and mapping outside Clockwork depends on
them.
