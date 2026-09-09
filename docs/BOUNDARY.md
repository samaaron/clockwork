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

Calls in the other direction go through `DspHost`, six callbacks. **Every one
may be NULL, so a DSP must check before calling.**

```
emit_osc(ctx, origin,             send OSC out: a reply, or a notification
         bytes, len)
log(ctx, level, text)             diagnostics, syslog severities
open_sink(ctx, kind,              open a MIDI or OSC destination
          target, capacity)
send_sink(ctx, sink, bytes,       send to an opened sink, at a time
          len, when)
free_bytes(ctx, ptr)              hand back memory clockwork allocated
asset_release(ctx, id)            done with an asset taken through dsp_asset;
                                  the client's slot goes back
```

Not all callbacks can be called from all threads:

| callback     | allowed threads                                         |
|--------------|---------------------------------------------------------|
| `emit_osc`   | any, including the audio thread                         |
| `send_sink`  | any, including the audio thread                         |
| `free_bytes` | any except the audio thread; freeing may defer           |
| `log`        | **not** the audio thread                                |
| `open_sink`  | control thread only                                     |
| `asset_release` | `dsp_process` and `dsp_osc` — one short message, nothing else |

`emit_osc` and `send_sink` both return non-zero on accept; a full ring or sink
drops and counts rather than blocking, so a refusal is visible.

`emit_osc` sends OSC back to clients, and its `origin` argument decides which
of them. It is callable from the audio thread, which makes calls back into
clockwork during a block part of the contract rather than an exception to it.
(`send_sink` is the other way out, and reaches MIDI and OSC destinations the
DSP opened itself rather than clients.)

### Replies

Echo back the `origin` the incoming message arrived with, and clockwork routes
the reply to that client. A DSP therefore needs no client registry of its own —
it only has to hand back the `origin` it was given.

### Notifications

`origin` 0 addresses the notification audience: the clients that asked to be
notified by sending `/clockwork/notify`. The transport sends one unicast
per subscriber. Three things follow:

- **It may reach nobody.** An empty subscriber list is a normal state, and
  `emit_osc` still returns non-zero — that return says the frame was queued on
  the egress ring, not that anything received it.
- **It does not reach the caller.** Sending a request does not make a client a
  notification subscriber. Answer a request with the `origin` you were handed;
  0 is for unsolicited events.
- **The audience is shared with clockwork's own traffic.** Engine state
  changes, device reports and `/clockwork/debug` go to the same
  subscribers, so a DSP emitting on 0 is talking into that stream.

On web there is one client and the worklet strips the route word, so both cases
arrive identically. The distinction is a native, multi-client one.

### Bulk

Bulk does not go through `emit_osc`: sending copies the payload into the egress
ring, which is a large `memcpy` in the audio callback. Write it into
`DspConfig::outbox` and send a short message saying where it landed.

`DspConfig` hands the DSP three memory regions, and **each has exactly one
writer**:

| region   | written by | read by    |
|----------|------------|------------|
| `arena`  | the DSP    | the DSP    |
| `inbox`  | the client | the DSP    |
| `outbox` | the DSP    | the client |

Each side gets the constness its role implies: `DspConfig::inbox` is a
`const void*`, so a DSP that writes to it does not compile, and the client's
view of the outbox is const in the same way.

`arena` is the DSP's own memory: clockwork allocates it, zeroes it at every
`dsp_new`, and never touches a byte after that. No client can reach it — it is
not in the shared segment, so there is nothing for a client to map. It is there
so a DSP can take working memory without calling an allocator, which matters
because `dsp_process` may not allocate at all.

#### Two tiers, and what the DSP says it needs

A device with two kinds of RAM — the ESP32-S3 has ~512 KiB of internal SRAM
behind 8 MiB of slower PSRAM — hands the DSP two arenas. `arena` is placed in
the **fast** tier and `DspConfig::arena_bulk` in the **bulk** tier, so a DSP
carves its hot state (VM slots, kernel state, wires) from one and its cold,
large state (loop cells, sample data, scope rings) from the other, and which
RAM is which is the host's memory profile's decision rather than something
each DSP crate works out for itself. Both follow `arena`'s rules: the DSP's
alone, zeroed at every `dsp_new`.

On a single-region host — desktop, web, the NIF — `arena_bulk` is NULL/0. There
is one kind of memory, so there is nothing to place differently; whatever the
DSP would have put in bulk goes in `arena`. Test `arena_bulk` for NULL, never
the platform.

The DSP declares what it needs of each tier **statically**, in `DspInfo`:
`arena_bytes_wanted` and `arena_bulk_bytes_wanted`. A DSP that carves
everything at `dsp_new` knows the figure before any instance exists, and a host
should learn it before booting audio rather than from a failed carve halfway
through `dsp_new`. clockwork checks the wants against what it can offer — on a
tiered host each want against its own tier, on a single-region host both
against `arena` — and **refuses the boot with a logged reason when a want
cannot be met. It never spills.** A hot pool that silently landed in PSRAM is a
performance cliff with no error, which is worse than a boot that says why it
stopped. Zero means "no claim": take what is offered, including nothing.

Where the bytes come from: the fast arena is the span the host reserved (the
JS layout on web, `CLOCKWORK_ARENA_BYTES` on native), or, when the host passed
none and the DSP wants some, a span clockwork takes from its own fast tier once
and keeps across rebuilds. The bulk arena exists only on a tiered build, from
clockwork's bulk tier, sized by the want or by `CLOCKWORK_ARENA_BULK_BYTES`.

`inbox` and `outbox` are the bulk channel for _binary blobs_ in and out. A client writes bytes straight into
`inbox` from its own thread and sends a short message saying where they are, so
the DSP receives an offset rather than a payload, and the DSP does the same in
reverse through `outbox`. Nothing large crosses the audio thread in either
direction — which matters because every entry point in this header is called on
the audio thread, and on web that is forced: the DSP exists only inside the
AudioWorklet, so there is no off-thread door to use instead.


**Offsets, never pointers**, in any message naming a position in one of these.
A reader maps the segment wherever the kernel puts it, which is not the address
the engine sees, so a raw pointer names the wrong bytes on the other side — and
does so without faulting.

That leaves the DSP a choice about a range published in the inbox, and both
answers are permitted:

- **Read it in place.** Nothing is copied. Right whenever the bytes are used as
  they arrived — sample playback, a wavetable, an impulse response. The range
  belongs to the DSP from the moment it is published and must be handed back
  explicitly: the client must not touch it until the DSP says it is done.
- **Copy it into the arena.** Do this when you need to *mutate* the data —
  resample it, normalise it, convert its format — because the inbox is
  read-only to you. The range belongs to the client again as soon as `dsp_osc`
  returns, so nothing has to be tracked. The copy costs about 5% of a 128-frame
  block per megabyte, worst case, which is nothing for a sample and not nothing
  for a library.

Native carves the inbox and the outbox out of a shared segment when there is
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
  DSP does that arithmetic; only it knows what acting at a frame means.

Audio thread: no allocation, no locks, no IO.

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
| `clockwork_client_tap_*`                   | watch a ring without taking from it              |
| `clockwork_client_region`                  | address metrics, the guest's window, the rings   |
| `clockwork_client_clock` / `_beat_at`      | the tempo grid, so every binding agrees to the bit |
| `clockwork_audio_decode_*` / `_writer_*`   | decode and encode audio, from a file or memory   |

**The engine's own transports are clients of this.** A UDP datagram from
another machine does not enter by a private door: the receiving thread mints an
origin for the sender and calls `clockwork_client_send`, exactly as an
in-process GUI does. One ingress path, one implementation of the ring
arithmetic.

Polling TAKES — the egress ring has one read cursor, shared by every handle, so
two handles polling take from each other. A reader that wants to watch rather
than take opens a tap, which carries a cursor of its own and may be lapped.

Nothing above that: no notion of a synth, a node, a sample or a track. Those
belong to whatever guest is loaded.

## The guest's vocabulary: the DSP profile

The JavaScript client needs two OSC addresses from the engine — a sync barrier
and its reply — and a list of verbs it should refuse to send. Both differ
between engines.

| what the client needs         | scsynth           | clockwork's own guest                 |
|-------------------------------|-------------------|---------------------------------------|
| sync barrier / its reply      | `/sync` `/synced` | `/clockwork/sync` `/clockwork/synced` |
| verbs the client should block | —                 | `/clockwork/quit`                     |

It takes them as a **DSP profile** (`js/lib/dsp_profile.js`), a plain object:

```js
new Clockwork({ dsp: {
  syncVerb:     "/clockwork/sync",
  syncedVerb:   "/clockwork/synced",
  blockedVerbs: { "/clockwork/quit": "Use destroy() to shut down." },
}})
```

scsynth would pass `/sync` and `/synced`.

A guest may also supply `metrics` and `metricsPanels`, in the shape the metrics
schema's layout uses, so its numbers appear in the metrics UI beside
clockwork's.

A missing profile is a working state: clockwork runs and refuses `sync()`
rather than guessing a verb. A malformed one throws where it is supplied —
`syncVerb` and `syncedVerb` come as a pair, and half a barrier would otherwise
wait forever on a reply nobody sends. `test/dsp_profile.test.mjs` exercises both
vocabularies.

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
/clockwork/no-such-verb     clockwork — REFUSED (/clockwork/error), never forwarded
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
