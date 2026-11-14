<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# The boundary

clockwork is a realtime DSP engine host. For a DSP engine to be compatible with
clockwork it needs to implement the following C ABI:

```
dsp_describe()                    what this DSP is, and its terms
dsp_new(config, host, &err)       lifecycle
dsp_free(dsp)
dsp_process(dsp, in, n_in,        the tick — read `frames` from each input
            out, n_out,           channel, write `frames` to each output one
            frames, block_time)
dsp_osc(dsp, bytes, len,          messages, with the time they mean
        when, origin, block_time)
dsp_asset(dsp, asset)             a client's bytes — a sample, a wavetable —
                                  bound where they lie, never copied
```

Calls in the other direction go through `DspHost`, ten callbacks. **Every one
may be NULL, so a DSP must check before calling.**

```
emit_osc(ctx, origin,             send OSC out: a reply, or a notification
         bytes, len)
log(ctx, level, text)             diagnostics, syslog severities
open_sink(ctx, kind,              open a MIDI or OSC destination
          target, capacity)
send_sink(ctx, sink, bytes,       send to an opened sink, at a time
          len, when)
free_bytes(ctx, ptr)              hand back memory taken through alloc_bytes
asset_release(ctx, id)            done with an asset taken through dsp_asset;
                                  the client's slot goes back
alloc_bytes(ctx, bytes)           take memory from the DSP's bulk span, through
                                  clockwork's allocator
tap_open(ctx, index,              claim a tap slot to publish audio into
         channels, hnd)
tap_write(ctx, hnd, channels,     append a block to a claimed tap
          n_channels, frames)
tap_close(ctx, hnd)               give the slot back
flush_schedule(ctx)               drop the bundles clockwork holds for this DSP
                                  ahead of their time (its "clear the schedule")
param_define(ctx, entries, count, declare, replace or retire parameters
             text, text_bytes,    (docs/PARAMS.md)
             slots)
param_value(ctx, slot)            a parameter's value now
param_set(ctx, slot, value)       set a parameter's value as the guest
```

Not all callbacks can be called from all threads:

| callback     | allowed threads                                         |
|--------------|---------------------------------------------------------|
| `emit_osc`   | any, including the audio thread                         |
| `send_sink`  | any, including the audio thread; never allocates, locks or blocks |
| `alloc_bytes` | any. On the thread that holds the core — the audio thread during a block, the building thread during `dsp_new` — from the guest's pool, lock-free, answering NULL rather than blocking; on any other thread from the system allocator, on that thread |
| `free_bytes` | any, including the audio thread; a free from a thread that does not hold the core is parked for the next call that does |
| `log`        | any, including the audio thread, where it is lock-free and drops when the ring is full; formats nothing |
| `open_sink`  | `dsp_new`, never `dsp_process`: it allocates and may touch the device |
| `asset_release` | `dsp_process` and `dsp_osc` — one short message, nothing else |
| `tap_open` / `tap_write` / `tap_close` | any, including the audio thread; none allocates |
| `flush_schedule` | `dsp_process` and `dsp_osc`, where the schedule is driven |
| `param_define` | `dsp_new`, and the guest's own calls on the audio thread; never another thread |
| `param_value` / `param_set` | the audio thread; neither allocates or blocks |

`emit_osc` and `send_sink` both return non-zero on accept; a full ring or sink
drops and counts rather than blocking, so a refusal is visible.

`emit_osc` sends OSC out to the client. There is one client, so its `origin`
argument decides nothing: it rides with the message, for the client to match
an answer to its request. It is callable from the audio thread, which makes calls back into
clockwork during a block part of the contract rather than an exception to it.
(`send_sink` is the other way out, and reaches MIDI and OSC destinations the
DSP opened itself rather than clients.)

`tap_open`, `tap_write` and `tap_close` let a DSP publish audio without
learning the layout: clockwork owns the slots and the ring a client reads,
and a DSP that publishes one claims a slot and appends blocks. Only a guest
makes a tap live. A client reads one with `clockwork_client_audio_tap_*`
(src/clockwork_client.h), `Client::audio_tap` in Rust, or `getTap` in
JavaScript, which needs SAB mode.

### Lifecycle

| entry point | native thread | web thread | blocks meanwhile | called |
|---|---|---|---|---|
| `dsp_describe` | the thread building the instance | the worklet's | none | at every build |
| `dsp_memory_bytes`, `dsp_memory_bulk_bytes` | the thread building the instance: the host's boot thread, or a restart's | the worklet's, inside `clockwork_init` | none | before every `dsp_new` |
| `dsp_new` | the same | the same | none | at the first boot and at every cold restart |
| `dsp_reconfigure` | a restart's thread | the worklet's, when a new worklet comes back over the same memory | none; natively the device is stopped | only for a change `DspInfo::survives` names |
| `dsp_free` | a restart's thread, or the engine's shutdown | the worklet's, only when a new worklet over the same memory must rebuild the instance | none | before every rebuild, and at shutdown natively. Never when a page reloads into new memory or closes, or the browser kills the worklet: the memory goes with the page and nothing is called |
| `dsp_process` | the audio thread: the device's callback, or the thread a host ticks from | the worklet's, from `process()` | this is the block | every block while there is an instance |
| `dsp_osc` | the audio thread, before `dsp_process` in the same block | the worklet's | inside the block | for each message the block takes, and each held one that falls due |
| `dsp_asset` | the audio thread for a commit; the building thread when a new instance is handed the assets the old one held | the worklet's | inside the block, or none | for each commit, and for each held asset at a rebuild |

"None" means no block renders while the call runs: the calling thread holds
the core (`GateHold`, src/core_state.h), and a block that finds it held
renders silence and calls nothing. A web worklet makes all of these calls on
its own thread, the audio thread, between two blocks. Natively only
`dsp_process`, `dsp_osc` and `dsp_asset` run on the audio thread.

### What a guest includes

**These are the only doors.** A guest includes these of clockwork's headers
and nothing else: not the arena layout, not the pool, not the tap rings, not
a global.

| header | what for | language |
|---|---|---|
| `dsp_api.h` | the boundary; it includes `clockwork_event_sink.h` (sinks) and `clockwork_timetag.h` (timetags and their arithmetic) | C |
| `clockwork_clock_state.h` | `readClockworkClock()`, to read `DspConfig::clock` | C++ |
| `params/clockwork_params.h` | `ClockworkParamEntry`, for a guest that calls `param_define` | C |

SuperSonic's suite checks this against the scsynth guest's sources (its
`scripts/check-guest-boundary.sh`), because every reach past the header is a
breakage waiting for the day the layout moves.

Two things a guest is handed it cannot read through those headers today:

- **The clock, from C.** `clockwork_clock_state.h` is C++ (its fields are
  `std::atomic`), so a C guest has no reader for `DspConfig::clock`. A Rust
  guest has one: `clockwork_abi::clock::read`.
- **The channel map, from C or Rust.** `ClockworkChannelMapState` and the
  `CLOCKWORK_CH_*` kinds are declared in `shared_memory.h`, the internal C++
  header that lays out the whole arena. A C++ guest that reads the map
  includes it, as the dummy does. `clockwork-abi` declares the struct opaque
  and has no reader.

### Replies

Echo back the `origin` the incoming message arrived with. The client gets it
with the reply and can tell which request it answers. A DSP needs no client
registry of its own: there is one client, and everything a DSP sends reaches
it, on every host.

### Notifications

Pass `origin` 0 for a message nobody asked for. It goes to the client like
any other.

- **It may reach nobody.** With no client there, `emit_osc` still returns
  non-zero: that return says the frame was queued on the egress ring, not
  that anything received it. What reached no one is counted
  (`egress_frames_undelivered`).
- **Clockwork's own notices are asked for.** The engine's lifecycle
  (`/clockwork/statechange`, setup, device reports) is written only once the
  client has sent `/clockwork/notify`; the timeline's, a port's, a
  controller's and outside OSC only once it has subscribed to that kind
  (`clockwork_client_wants`, `lanes.h`). `/clockwork/debug` is a line for the
  host's log and is not sent to the client.

### Bulk

Bulk does not go through `emit_osc`: sending copies the payload into the egress
ring, which is a large `memcpy` in the audio callback. Write it into
`DspConfig::returns` and send a short message saying where it landed.

`DspConfig` hands the DSP two spans of its own and two bulk lanes, and
**each has exactly one writer**:

| region        | written by                          | read by    |
|---------------|-------------------------------------|------------|
| `memory`      | the DSP, by hand                    | the DSP    |
| `memory_bulk` | the DSP, through clockwork's allocator | the DSP |
| `assets`       | the client                          | the DSP    |
| `returns`      | the DSP                             | the client |

Each side gets the constness its role implies: `DspConfig::assets` is a
`const void*`, so a DSP that writes to it does not compile, and the client's
view of the returns area is const in the same way.

A DSP's memory, in four sentences. It gets two spans, one per speed:
`memory` is fast and `memory_bulk` is bulk. It says how much of each it
needs (`DspInfo::memory_bytes_wanted` and `memory_bulk_bytes_wanted`, plus
`dsp_memory_bytes` and `dsp_memory_bulk_bytes` for what its configuration
adds) and clockwork gives exactly that, or refuses the boot and says why. It
lays out its hot per-block state in `memory` by hand (scsynth: its tables
and its real-time pool); everything it allocates as it runs comes from
`memory_bulk`, through clockwork's allocator. There is nothing else.

Both are reserved before `dsp_new`, zeroed at every `dsp_new`, and no
client can reach them: they are not in the arena. `memory` is there so a
DSP can hold working memory without calling an allocator, which matters
because `dsp_process` may not allocate at all. `memory_bulk` is there so the
allocations a session makes — buffers, definitions — have one place, sized
by the DSP's own declaration or the host profile's default, whichever is
larger, and that never grows: what a DSP declared is what it has. The same
on every platform.

#### Two tiers, and what the DSP says it needs

A device with two kinds of RAM — the ESP32-S3 has ~512 KiB of internal SRAM
behind 8 MiB of slower PSRAM — hands the DSP two spans. `memory` is placed in
the **fast** tier and `DspConfig::memory_bulk` in the **bulk** tier, so a DSP
carves its hot state (VM slots, kernel state, wires) from one and its cold,
large state (loop cells, sample data, tap rings) from the other, and which
RAM is which is the host's memory profile's decision rather than something
each DSP crate works out for itself. Both follow `memory`'s rules: the DSP's
alone, zeroed at every `dsp_new`.

On a single-region host — desktop, web, the NIF — `memory_bulk` is NULL/0. There
is one kind of memory, so there is nothing to place differently; whatever the
DSP would have put in bulk goes in `memory`. Test `memory_bulk` for NULL, never
the platform.

The DSP declares what it needs of each tier **statically**, in `DspInfo`:
`memory_bytes_wanted` and `memory_bulk_bytes_wanted`. A DSP that carves
everything at `dsp_new` knows the figure before any instance exists, and a host
should learn it before booting audio rather than from a failed carve halfway
through `dsp_new`. clockwork checks the wants against what it can offer — on a
tiered host each want against its own tier, on a single-region host both
against `memory` — and **refuses the boot with a logged reason when a want
cannot be met. It never spills.** A hot pool that silently landed in PSRAM is a
performance cliff with no error, which is worse than a boot that says why it
stopped. Zero means "no claim": take what is offered, including nothing.

Where the bytes come from: clockwork takes the fast span from its own fast
tier (the web's placement arena, a desktop's malloc, a device's internal SRAM),
sized to the want, and keeps it across rebuilds while the want still fits. A
host that wants to place the span itself may pass one to `clockwork_init`,
and it is handed over as is. The bulk span exists only on a tiered build, from
clockwork's bulk tier, sized by the want or by `CLOCKWORK_GUEST_MEMORY_BULK_BYTES`.

`assets` and `returns` are the bulk channel for _binary blobs_ in and out. A client writes bytes straight into
`assets` from its own thread and sends a short message saying where they are, so
the DSP receives an offset rather than a payload, and the DSP does the same in
reverse through `returns`. Nothing large crosses the audio thread in either
direction — which matters because every entry point in this header is called on
the audio thread, and on web that is forced: the DSP exists only inside the
AudioWorklet, so there is no off-thread door to use instead.


**Offsets, never pointers**, in any message naming a position in one of these.
A reader maps the segment wherever the kernel puts it, which is not the address
the engine sees, so a raw pointer names the wrong bytes on the other side — and
does so without faulting.

That leaves the DSP a choice about a range published in the assets area, and both
answers are permitted:

- **Read it in place.** Nothing is copied. Right whenever the bytes are used as
  they arrived — sample playback, a wavetable, an impulse response. The range
  belongs to the DSP from the moment it is published and must be handed back
  explicitly: the client must not touch it until the DSP says it is done.
- **Copy it into the arena.** Do this when you need to *mutate* the data —
  resample it, normalise it, convert its format — because the assets area is
  read-only to you. The range belongs to the client again as soon as `dsp_osc`
  returns, so nothing has to be tracked. The copy costs about 5% of a 128-frame
  block per megabyte, worst case, which is nothing for a sample and not nothing
  for a library.

Native carves the assets area and the returns area out of a shared segment when there is
one, so the same routes work cross-process. **The segment publishes them; it
does not create them.** An engine embedded without a UDP port builds no segment
and allocates the lanes in-process, where its client — the host application —
reads them in place. Metrics, the scope and the window already work this way.

Check before using, and take the size from the matching `*_bytes` field.

### The tick

`dsp_process` reads `frames` from each of `n_in` input channels and writes
`frames` to each of `n_out` output channels. The channel counts arrive per block
and change while running; neither exceeds the ceiling in `DspConfig`.

- **Output is written, not accumulated.** A DSP with nothing to say writes
  silence.
- **`in` may be NULL** when the host captures no input.
- **A channel index is a stable slot** for the lifetime of the stream occupying
  it. A departing peer does not renumber those after it: its slot goes silent and
  stays reserved until reused. Slots above the live count are not written and
  must not be read.
- **`block_time` is the OSC timetag of the block's first frame** (NTP 32.32). A
  message due at T lands at frame `(T - block_time) * sample_rate / 2^32`. The
  DSP does that arithmetic; only it knows what acting at a frame means. The
  range and the rounding are under "Sending to the guest".

Audio thread: no allocation, no locks, no IO. What a block needs is laid out
in `memory`, or allocated before the block.

### Sending to the guest

Either side may own the schedule. One engine keeps a host-side timed queue
and fires per block; another parks programs in its own VM. Mandating either
would get in the other's way, so `DspInfo::holds_schedule` declares which a
guest wants: messages as they arrive, carrying their timetag, or when they
come due. A build with the timed store keeps a queue either way: timed MIDI
output and timed OSC forwarding are clockwork's own features.

What a client sends reaches the guest in one of three forms. The rules are the
same natively and on the web: one function decides (`ingest_frame` in
src/audio_processor.cpp).

**A message** — any address outside `/clockwork/` — goes to `dsp_osc` in the
block that takes it, with `when` OSC's "immediately".

**A bundle** (`#bundle` and a timetag) is the guest's, whole and unopened.
clockwork never splits one and never routes what is inside it, so a
`/clockwork/` verb inside a bundle reaches the guest with the rest and is not
applied (src/clockwork_prefix.h). The timetag decides when:

- `holds_schedule` 0: clockwork holds the bundle until it is due and hands it
  over in the block it falls in, with `when` the bundle's timetag and
  `block_time` the first frame of that block. A bundle whose time has passed,
  or that says "immediately", goes in the block that takes it.
- `holds_schedule` 1: the bundle goes to `dsp_osc` in the block that takes
  it, with `when` its timetag, however far ahead that is. The guest's own
  queue decides when it acts.

**`/clockwork/schedule <when> <blob>`** is clockwork's own verb, and clockwork
always holds it, whatever `holds_schedule` says. When it falls due its
payload is routed again as if it had arrived then: a `/clockwork/` verb to
clockwork, anything else — a message or a bundle — to `dsp_osc`, with `when`
the scheduled time. Its wire forms (src/scheduler/schedule_parse.h,
rust/clockwork-schedule/src/parse.rs):

| type tags | `<when>` |
|---|---|
| `,tb` | an OSC timetag |
| `,hb` | the same 64 bits, as an int64 |
| `,db` | NTP seconds, as a double |
| `,fb` | NTP seconds, as a float32. Accepted, but a float32 holds a time this far from 1900 only to the nearest 256 seconds: do not use it |

One that does not parse is refused to its sender (`/clockwork/error`,
`malformed`). A build without the timed store (`CLOCKWORK_SCHEDULER` off)
refuses the verb by name, and hands every bundle over at once, timetag and
all.

Which to use: a bundle for the guest's own timed messages — it is the form a
guest that holds its own schedule receives early. `/clockwork/schedule` to
time one of clockwork's verbs (`/clockwork/param/set`,
`/clockwork/midi/out/note_on`, `/clockwork/osc/send`), which a bundle cannot
do, or to make a guest message wait in clockwork for a guest that holds its
own schedule.

**At most 32 incoming messages are taken per block** (`MAX_MESSAGES_PER_FRAME`
in `process_audio`); the rest wait in the in ring for the next. A burst of
messages therefore takes several blocks to reach the guest, and a timed
bundle behind the burst is taken late: clockwork can fire a held bundle on
time only once it has it. A bundle is one message, so a burst for the guest
sent as one bundle is taken in one block.

clockwork's store holds at most `CLOCKWORK_SCHEDULER_SLOT_COUNT` messages
and `CLOCKWORK_SCHEDULER_DATA_POOL_SIZE` bytes of them (by default 512 and
512 KB natively, 8192 and 4 MB on the web; the metrics
`engineSchedulerCapacity` and `engineSchedulerPoolBytes`). One more is
dropped, logged and counted (`engineSchedulerDropped`).

#### Placing a message within the block

`block_time` is the timetag of the first frame of the block the message is
for and `when` the message's own, so the frame it acts at is

```
frame = round((when - block_time) * sample_rate / 2^32)
```

with `when - block_time` taken as a signed 64-bit difference:
`clockwork_timetag_offset(block_time, when)` in C (src/clockwork_timetag.h),
`Timetag::offset_from` in Rust (`clockwork-osc`). Its range is wider than the
block:

- clockwork hands a held message over in the first block whose end — the
  next block's start — is at or after its time: the store pops everything
  due at or before the next block's start (`pop_due` in
  rust/clockwork-schedule/src/store.rs). So one due exactly at the next
  block's start arrives a block early, at `frame == frames`, one past the
  last frame.
- A late one, whose time had passed when clockwork took it, gives a negative
  frame.
- "Immediately" has no offset: test it with `clockwork_timetag_is_immediate`
  and act at frame 0.
- With `holds_schedule` 1 the frame may be any distance ahead.

So round, then clamp:

```c
/* The frame of this block at which a message due at `when` acts. */
static uint32_t frame_in_block(ClockworkTimetag when, ClockworkTimetag block_time,
                               double sample_rate, uint32_t frames) {
    if (clockwork_timetag_is_immediate(when)) return 0;
    const double f = floor((double)clockwork_timetag_offset(block_time, when)
                           * sample_rate / 4294967296.0 + 0.5);
    if (f <= 0.0) return 0;                       /* late: at once */
    if (f >= (double)frames) return frames - 1;   /* due at the next block's start */
    return (uint32_t)f;
}
```

That plays a message due at the next block's start a frame early. A guest
that keeps events from one block to the next can hold it instead and act at
frame 0 of the next block.

A guest that keeps its own queue can carry an event at `frames` into the next
block, at frame 0, rather than play it one frame early.

#### Which "now" to stamp from

A client stamps a timed message from the engine's clock: the time its audio
thread has reached, in which `block_time` is measured. The wall clock drifts
from it (the metric `clockDriftUs`).

- JavaScript: `clockwork.clock.now()`, in NTP seconds; 0 until the first
  block. `Clockwork.osc.ntpNow()` and `clock.wallNow()` read the wall clock.
- Rust: `Client::sample_clock()` (rust/clockwork-client). The engine's time
  at the start of its latest block is `dac_ntp() - output_latency_frames() /
  sample_rate()`; `dac_ntp()` is when that frame is heard.
  `clockwork_client::timetag_now()` reads the wall clock.
- C: `clockwork_client_sample_clock`, with the same arithmetic
  (src/clockwork_client.h).

Stamp far enough ahead for the message to reach the engine before its block:
a block at least (2.7 ms at 128 frames and 48 kHz), plus the time the
message takes to arrive. One that arrives late plays at once, and one more
than a block late is counted (`engineSchedulerMaxLateMs`,
`engineSchedulerLastLateMs`). A host may drop events later than a limit
instead (`clockwork_set_timed_event_late_limit_ms`; the JavaScript client's
`schedulerLateLimitMs`); the default, 0, plays them all.

#### From JavaScript

`send(address, ...args)` types each number by itself: a whole number goes
as `i`, any other as `f`, a float32. Pass `{ type: "timetag", value }` (NTP
seconds) for a time and `{ type: "float", value }` where a verb wants a float
whatever the value: `/clockwork/param/set` takes `,if` and refuses a bare
`1`, and a bare NTP time is a float32, which `/clockwork/schedule` reads to
the nearest 256 seconds. A bundle is `Clockwork.osc.encodeBundle(time,
packets)`, sent with `sendOSC`.

```js
const t = clockwork.clock.now() + 0.1;
clockwork.sendOSC(Clockwork.osc.encodeBundle(t, [["/dummy/tone"]]));
clockwork.send("/clockwork/schedule", { type: "timetag", value: t },
  Clockwork.osc.encodeMessage("/clockwork/param/set", [3, { type: "float", value: 1 }]));
```

### The floating-point environment

Denormal flush-to-zero is a per-thread CPU flag, and the DSP does not own the
thread. clockwork's native device layer arms it on its audio thread every
callback; a web worklet cannot arm it at all; an embedded host may or may not.
Two targets that differ here render different tails from the same filter, so a
DSP that promises the same samples on every target must not depend on the
flag.

The host therefore **declares** it, with `clockwork_declare_fp_env` before
`clockwork_init`, and the DSP reads it as `DspConfig::fp_env`:
`DENORMALS_HONOURED`, `FLUSH_TO_ZERO`, or `UNKNOWN` when the host said nothing.
The web build reports `DENORMALS_HONOURED` regardless, because the answer is
known. A DSP treats `UNKNOWN` as `DENORMALS_HONOURED`: assuming a flush that is
not there is the failure that costs CPU; assuming none when there is one costs
nothing. A DSP that wants identical output everywhere flushes in software at
the points that matter, or keeps its state out of the denormal range by
construction, and uses the declaration only to know which it is paying for.

### Who owns time

clockwork does. Tempo, beat origin and transport are published as atomics.
JavaScript reads them directly; `DspConfig::clock` hands the DSP the same bytes.
Nothing pushes tempo at the DSP.

Read that block with `readClockworkClock()` (`shared_memory.h`). There is no
seqlock on it — coherence comes from store ordering, value first relaxed and the
key field last with release, across five writers. clockwork supplies the reader
so no DSP has to reproduce that.

### What survives a rebuild

`DspConfig::persistent` is a region of the arena, private to the DSP, that
clockwork neither touches nor zeroes between instances.

A DSP does not always get to say goodbye. A device switch destroys and rebuilds
it; a browser can kill the worklet outright, leaving no thread to run a save
callback on.

clockwork clears the region on a cold boot and leaves it alone on a rebuild.
Only clockwork can draw that line: a DSP cannot tell "restored after a device
switch" from "an earlier engine in this process left this behind".

The bytes may be torn (death mid-write) or stale (a crashed run), so stamp a
generation and validate it. scsynth uses the region to remember which origins
asked for `/notify`, which the World does not survive.

### How many in a process

A process may hold many cores, each with its own instance of the guest: a
DAW opens a plugin once per track. An instance keeps its state in what it
was handed — its memory, its arena regions, its context pointer — and so
two instances never meet.

`DspInfo::instances` says how many instances of the guest a process may
hold. 0, what a zeroed `DspInfo` says, is one: a guest that says nothing is
taken to keep process-wide mutable state — a static it writes, a library
that is one per process. clockwork refuses a build past the count with a
reason a person can read ("only one can run in a process, and one already
is"), as it refuses a build for anything else. A core rebuilding its own
instance is never refused.

`DSP_INSTANCES_ANY` is any number, and a promise that the guest keeps
nothing process-wide. Two checks hold a guest to it, and a guest that says
any owes both:

- its built objects have no mutable variable with static storage
  (`scripts/check-guest-globals.sh`, over an unoptimised build of the
  guest's C and C++);
- two instances at once, each told something different, render bit for
  bit what each renders alone (the dummy's is in `test/test_embed.cpp`).

### Who owns the schedule

Either side. One engine keeps a host-side timed queue and fires per block;
another parks programs in its own VM. Mandating either would get in the other's
way.

clockwork keeps a queue regardless — timed MIDI output and timed OSC forwarding
are its own features. `DspInfo::holds_schedule` declares which you want:
messages as they arrive, carrying their timetag, or when they come due.

### Which channels are which stream

`dsp_process` sees an anonymous range of channels. A stereo file's left and
right are indistinguishable from each other and from a microphone, so a DSP that
wants one process per stream channel cannot tell which channel is which.

`DspConfig::channel_map` says what each one is. It is state in the arena, not a
message, so there is nothing to subscribe to, nothing to replay after
`dsp_new`, and nothing to miss by starting late.

Two halves, because the two readers want different things:

```
map->in[c] / map->out[c]     one atomic word per channel: kind + stream slot
map->streams[slot - 1]       that stream's port, direction, first, count, name
map->device_in / device_out  where the device's own channels end
map->generation              odd while the stream table is being written
```

The **hot** half is a read at the point of use: a DSP about to process channel
*c* loads `in[c]`, gets `CLOCKWORK_CH_DEVICE`, `CLOCKWORK_CH_PORT` or
`CLOCKWORK_CH_NONE`, and for a port the slot naming it. Nothing to remember
between blocks, nothing to consume, no cached copy to invalidate.

The **cold** half is the stream table — names and ranges — guarded by
`generation`, which is odd while it is being written. It is not audio-thread
work, and a client is welcome to ignore the generation and be a poll interval
out of date.

A channel is described no later than the audio it carries: the map entry is
written before the binding is published, and cleared after it is unpublished.

The same bytes are the client's: the `CHANNEL_MAP` entry of the arena's table
(`clockwork_arena.h`, `docs/ARENA.md`), the same for every reader. So a client
places a stream by reading where the device ends, rather than choosing an index
and finding out by refusal.

## The client boundary: `clockwork_client.h`

`dsp_api.h` is what a guest implements. This is what a CLIENT gets — anything
driving clockwork from outside the audio thread: a GUI, a language binding, a
CLI, a browser tab running the same code compiled to WebAssembly.

| verb                                      | what it does                                     |
|-------------------------------------------|--------------------------------------------------|
| `clockwork_client_send`                    | put one OSC message on the ingress ring          |
| `clockwork_client_send_begin` / `_commit`  | build the message IN the ring, for an encoder that has no buffer of its own |
| `clockwork_client_poll`                    | take replies from the egress ring                |
| `clockwork_client_wait` / `_wake`          | sleep, on any thread, until the engine says something; end such a sleep |
| `clockwork_client_count_undelivered`       | count what the client could not deliver of what it took |
| `clockwork_client_tap_*`                   | watch a ring without taking from it              |
| `clockwork_client_region`                  | address metrics, the guest's window, the rings   |
| `clockwork_client_clock` / `_beat_at`      | the tempo grid, so every binding agrees to the bit |
| `clockwork_audio_decode_*` / `_writer_*`   | decode and encode audio, from a file or memory   |

**The engine's own transports are clients of this.** A UDP datagram from
another machine does not enter by a private door: the receiving thread mints an
origin for the sender and calls `clockwork_client_send`, exactly as an
in-process GUI does. One ingress path, one implementation of the ring
arithmetic.

**Except in a browser, outside the worklet.** The worklet runs the engine's
module, so it calls this. The page's other threads (the main thread, a worker
given a channel, the reply pump, the traffic logger) cannot run that code
without a stack inside the engine's memory, so they write, read and watch the
same rings with `js/lib/ring.js`, which restates this arithmetic in JavaScript
and is tested against it. THREADS.md says why that is the better trade.

Polling TAKES — the egress ring has one read cursor, shared by every handle, so
two handles polling take from each other. A reader that wants to watch rather
than take opens a tap, which carries a cursor of its own and may be lapped.

Nothing above that: no notion of a synth, a node, a sample or a track. Those
belong to whatever guest is loaded.

## What the client knows about the guest: nothing but its metrics

The JavaScript client boots the engine, moves OSC in and out, and treats every
message as opaque. It used to carry a **DSP profile** — the guest's word for a
sync barrier (`/sync` for scsynth, `/tau/sync` for tau), the verbs to refuse
on the guest's behalf, the address its refusals come on — so that `sync()`
could work with any engine. That went on 2026-09-13, on the observation that
the barrier was never the guest's fact to begin with:

- **The barrier is clockwork's.** `sync()` means "everything I sent before this
  has been handed to the guest, in order", and the order is the ingress
  ring's. So the engine answers `/clockwork/sync [i id]` with
  `/clockwork/synced [i id]` itself, on the audio thread, at the point the
  message is reached in the drain — for every guest, on every host,
  including the placeholder. A guest's own barrier verb, if it has one, is
  just another verb to it.
- **Refusing a guest's verbs is the product's business.** scsynth's `/quit`
  would stop the audio thread; SuperSonic's `send()` refuses it. Clockwork
  has no opinion.
- **The address a refusal comes on is the guest's word**, and `request()`
  takes it per call (`{ error: "/fail" }`); a product's own `request()` can
  default it.

What remains is a declaration, not a vocabulary: the guest's metrics.

```js
new Clockwork({
  guestMetrics: { bufferPoolGrowthCount: { slot: 3, type: "u32", description: "…" } },
  guestMetricsPanels: [ /* the metrics UI's panel shape */ ],
})
```

`slot` indexes the reserved guest range (js/lib/guest_metrics.js checks it
where it is supplied); the client fills values from `clientMetrics()`, and
`getMetricsSchema()` reports clockwork's schema with the guest's folded in.
`test/guest_metrics.test.mjs` exercises the check.

`request(verb, args, { reply, error, match, timeoutMs })` is the
request/reply shape every guest's verbs share (`/tau/spawn` → `/tau/spawned`,
`/d_recv` → `/done`): it resolves with the decoded reply, rejects on a
message at `error`, on a timeout, and narrows by `match(msg)` when several
replies share an address.

## The namespace boundary: `/clockwork/`

**An OSC address beginning `/clockwork/` belongs to clockwork; everything
else is forwarded to the DSP untouched.** One predicate, no table to maintain.

Enforced by construction. `OscSplit` (`src/clockwork_sys.h`) holds two
destinations and `clockwork_sys_claims()` between them, with no `registerRoute`
for a second predicate to go into. `ClockworkSysRoutes::add()` takes only the
part of an address that *follows* the prefix and prepends the prefix itself, so
a clockwork route cannot be spelled outside it. `OscIngress::registerRoute` is
called from exactly one place in the tree: inside `ClockworkSysRoutes::add`.

The prefix is spelled once per language:

- `CLOCKWORK_SYS_PREFIX_LIT` in `src/clockwork_prefix.h`
- the `clockwork_sys!` macro in `rust/clockwork-osc/src/lib.rs`
- `CLOCKWORK_SYS_PREFIX` in `js/lib/clockwork_sys.js`

Every clockwork address, every `memcmp` length and every wire offset derives
from those; changing the reserved string is three lines.

The prefix is deliberately ugly. `/host/` or `/sys/` is a word a DSP might want,
and every name clockwork claims is one the DSP cannot use. One unlikely string
leaves the rest of the namespace free.

`/clockwork` and `/clockwork-sys` are different address components: dispatch
matches the full `/clockwork/` including the trailing slash, never bare
`/clockwork`.

```
/clockwork/ping             clockwork — liveness, answered on the audio thread
/clockwork/clock/tempo/get  clockwork — the session clock
/clockwork/midi/ports       clockwork — MIDI devices
/clockwork/record/start     clockwork — session recording
/clockwork/no-such-verb     clockwork — REFUSED (/clockwork/error <address> <reason> <generation>),
                                        never forwarded
/anything-for-the-DSP, with no DSP instance up
                            clockwork — REFUSED at the boundary (/clockwork/error ... "no instance" ...),
                                        never written; wait for /clockwork/setup (docs/RESTARTS.md)
/clockwork-system-status        DSP — a near miss is not a match
/clockwork/synth                DSP — forwarded untouched
/d_recv                         DSP (scsynth) — forwarded untouched
```

## What is deliberately not here

- **Any DSP.** No ugens, no graph, no synthesis of any kind. DECODING AND
  ENCODING audio is here (`src/clockwork_audio_file.h`) — from a file or from
  bytes already in memory — and is not a counter-example: a container and a
  codec are a file format, not a signal path, and a client needs them to hand
  a guest samples at all.
- **Any GPL or GPL-derived source in the default build.** Ableton Link and its
  audio-input renderer are GPL-2.0-or-later. Both ship as source
  (`src/native/vendor/LinkAudioInputRenderer.hpp` keeps Ableton's notice) and
  neither compiles unless `CLOCKWORK_LINK=ON`, which produces a GPL binary and
  is off by default.

  Nor is anything LGPL. `RecordWriter` used to write through libsndfile, which
  dragged ogg, vorbis, FLAC and opus behind it; the codecs are now vendored
  public-domain decoders plus clockwork's own FLAC encoder, so the whole audio
  path is permissive and none of it is fetched at build time.

  JUCE is *not* a GPL dependency. `smoothie/` is a vendored ISC fork of the four
  permissive JUCE 7.0.12 modules (`juce_core`, `juce_audio_basics`,
  `juce_audio_devices`, `juce_events`), maintained directly. The GPL-dual
  `juce_audio_formats` was never vendored, which is why `RecordWriter` writes
  files through `src/clockwork_audio_file.h`.
- **Definition formats.** SCgf and ClockworkPatch each belong to their DSP.

## The dummy DSP

`dsp/dummy` implements the boundary and nothing else — no graph, no voices, no
definitions. It emits signal rather than silence, because a test asserting zeros
passes just as happily when nothing is connected at all.

Three modes, selected over OSC (`/dummy/pulse ,ii width_ms period_ms`,
`/dummy/tone`, `/dummy/ping`), which exercises the message path too.

**Pulse (default)** — 10 ms every 500 ms, and the timing instrument. Every edge
is a function of the absolute frame index, so a test can assert that pulse *k*
begins at exactly `k * period` frames whatever block size the host chose. Drift
shows as an edge in the wrong place, a dropped block as a missing pulse, a
duplicated one as a doubled pulse. None of that is visible in a continuous tone.

**Tone** — 440 Hz. Proves samples flow, and is pleasanter when checking a real
device.

**Echo** — output channel *c* is input channel *c*, verbatim. The generators are
deaf; feed each input something distinguishable and the outputs say which landed
where.

In every mode the right channel is the left inverted, so `left[i] + right[i] == 0`
is an exact invariant. One sample of inter-channel skew spikes the sum at every
edge while each channel alone still looks plausible; duplication sums to
`2*left`; a swap or a lost sign shows in which channel is positive.

It keeps no record of which channel is which, because there is nothing to
keep: `/dummy/stream/dump` reads `DspConfig::channel_map` when asked and
answers one `/dummy/stream/entry` per bound stream. That is a guest consuming
the map, and it is how a test sees every field — a count alone cannot catch a
swapped `first`/`count`, or a truncated name.

Those addresses are not under `/clockwork/`: that prefix never reaches a DSP.

It exists so this repository can build and test itself, and should be deleted
when a real DSP is attached. It is not a reference implementation.
