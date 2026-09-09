<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->

# Ports

*(Formerly "audio streams". Generalised once it became clear the same
mechanism already carries OSC, MIDI and gamepad events, not only audio.)*

## The observation

Clockwork already moves audio between the outside world and the audio thread
in at least seven places, and every one of them invented its own plumbing:

| | direction | lines | mechanism |
|---|---|---|---|
| `RecordWriter` | sink | 48 + .cpp | lock-free FIFO → `TimeSliceThread` → `clockwork_audio_file.h` |
| `shm_audio_buffer` | sink | 283 | SPSC ring in shared memory, monotonic write cursor — now the two audio taps clockwork writes at the device edge (docs/ARENA.md) |
| `shm_scope_stream` | sink | 332 | SPSC ring per scope slot, same protocol plus extras |
| Link aux publish | sink | (in the 232) | bus range → Link, per block |
| device output | sink | — | the backend's own callback |
| device input | source | — | the backend's own callback |
| Link peer audio | source | (in the 232) | drained into private buses — no longer reachable: the verb that set it up is refused, because the boundary exposes no bus pool |

And an eighth that was discussed and never written: **streaming a sample from
disk**, for material too large to hold — the looper and long-sample case.

Every one of these is the same object: frames crossing between the audio
thread and something that is not the audio thread, on a slot, which must
degrade to silence rather than stale data when it cannot keep up.

## Generalising: it is not only audio

Clockwork already runs two ring disciplines, and they were built
independently:

- **event rings** (`ring/ring.h`) — variable-length framed payloads, a 16-byte
  header, a padding marker rather than a frame straddling the wrap. Carries
  OSC ingress and egress, the shm peer plane, the transports.
- **frame rings** (`shm_audio_buffer`, `shm_scope_stream`) — fixed-size
  interleaved float frames, a monotonic write cursor, lossless catch-up reads.

They share everything that is hard: single producer, single consumer,
lock-free, never block the audio thread, degrade rather than stall, count what
was lost, and appear and disappear at runtime. They differ only in what a
payload is.

So the generic object is a **port**: a slot, a direction, a payload
discipline, and a policy. Then every one of these is the same thing wearing
different clothes —

| port | direction | payload |
|---|---|---|
| device in / out | both | frames |
| Link peer audio / aux publish | both | frames |
| a sample streamed from disk | in | frames (pull-controlled) |
| session recording, scope, capture | out | frames |
| OSC ingress / egress | both | events |
| MIDI in / out | both | events |
| gamepad | in | events |
| **a hosted plugin** | both | frames + events, in another process (see [TRACKS.md](TRACKS.md)) |

The substrate is one mechanism with two payload disciplines, not one type
forced over both: frames want a fixed cadence and silence on underrun, events
want variable size and a counted drop on overflow. Pretending those are the
same would be the over-generalisation that makes such things useless.

## The shape

A **port** is:

- a **slot** — a stable index for the lifetime of the thing occupying it. When
  a stream ends its slot goes silent and stays reserved. Renumbering would
  hand the DSP somebody else's audio on a channel it is already processing,
  which is a fault that presents as a mystery rather than an error. This rule
  already exists for channels (see `dsp_api.h`); the point of a substrate is
  to write it once rather than seven times.
- a **channel count**, fixed for the life of the port (`clockwork_ports.h`). The
  count the DSP sees per block does change — that is clockwork's channel
  total, not any one port's.
- a **lock-free ring**, single producer, single consumer.
- a **party off the audio thread** — a disk reader, a network peer, a file
  writer, a GUI.
- a **policy for not keeping up**, which differs by discipline and is the
  whole reason the two are not one type: frames degrade to **silence, never
  stale**; events **drop and count**. Both publish the count, and neither ever
  blocks the audio thread. Uniform per discipline, because every one of the
  paths above answers this question slightly differently today.

## How a DSP models a stream is its own business

Clockwork delivers a stream as channels and announces what they are; what
the DSP builds from that is not clockwork's concern, and the difference
between guests here is real rather than hypothetical.

**One mono process per stream channel** is one answer, with
aggregation processes composing them. That resolves the awkward constraint
rather than working around it — if a process's output block is pinned stereo,
so a 32-channel session would not fit one process; with a process per channel,
multichannel becomes *containment*. A stereo pair
is two mono processes panned hard apart inside one parent, and the mix pass
sums them with no new mechanism. It also means each channel of a peer can be
routed, effected or muted on its own.

**And a distinction worth keeping**: a *live* source (a Link peer, device
input) is process-shaped — it exists in time, you never seek it, you route it.
*Streamed material* — a long sample off disk — is cell-shaped: you seek in it,
play it at a rate, loop it. That is a playhead, which belongs to a synth and
not to a container. One
substrate, two models, and clockwork holds no opinion about either.

That is why identity has to reach the DSP per channel. "Channels 8-9 appeared"
is not enough to spawn two mono processes that know which is which; "port 7,
kick.wav, 2 channels, from channel 8" is. That is what the channel map carries:
`clockwork_port_bus_attach` writes the entry before it publishes the binding, so
a channel is described no later than the audio it holds, and a rebuilt DSP is
caught up the moment it is handed the pointer — there is nothing to replay
because the map is state rather than a message
(`ClockworkChannelMapState` in `src/shared_memory.h`, `docs/BOUNDARY.md`).

## Sources meet the DSP as input channels

That is the whole integration. A Link peer's audio and a streamed sample
arrive as input channels on stable slots, which `dsp_process` already carries
per block. The DSP needs no concept of Link, of disk, or of streaming — it
sees channels appearing and disappearing, which it must handle anyway because
a device switch does the same thing.

This is also what would let `/clockwork/clock/audio/input/add` work again. It currently
refuses: the old implementation needed the DSP's whole bus pool and scsynth's
`[outputs][inputs][private]` layout, neither of which survives the boundary, and
refusing loudly was better than registering a subscription that would silently
never sound.

The plugin bridge is the proof that this generalises to a process boundary.
`TrackControl` opens a sink (`tracks/send`) and a source (`tracks/return`)
over rings in a shared-memory segment with `clockwork_port_open_shared`, binds
both at the lane base, and the block loop pushes and pulls them exactly as
it does Link's. The bridge, a separate process, opens the same rings from
the other side — the send as ITS source, the return as ITS sink — and
renders between them. The port table's signal (`clockwork_port_set_signal`) is
where the doorbell rings. Nothing in the block loop, the port substrate or
the DSP knows that a plugin is on the far end; a track that has no bridge
is a source that reads silence.

## Push and pull

The one real asymmetry, and it is worth being precise about.

- Device input and Link peers are **push**: frames arrive when they arrive and
  you take what is there.
- A streamed sample is **pull**: the DSP chooses position and rate, so it can
  seek, loop and vary speed.

These unify if pull is expressed as push plus a control channel: the DSP says
"frames from position P at rate R", a worker fills the ring, and a seek is a
flush and a refill. That is how PS2-era streaming worked and how game audio
still does it. The cost is seek latency, which is inherent to streaming rather
than a flaw in the arrangement — the alternative is holding the whole thing in
memory, which is the case this exists to avoid.

## The other discipline: event sinks

There are **two producers of outgoing MIDI and OSC, and they want opposite
things.**

A client sends "play this note in one second". It arrives via ingress, must be
held, and fires later — the delay is the entire point, and for clockwork the
scheduler sits between ingress and the output to provide it.

A DSP that is itself a scheduler has already decided the moment. It knows
the block and the frame; when it emits a note the note is due **now**, and
anything between it and the port is latency added to a decision already
correctly made. Outgoing MIDI never goes through ingress in that model, because
it did not come from outside.

So the scheduler is not a service the outputs depend on. It is an **optional
stage in front of them**, used by one producer and bypassed by the other —
which is why making it optional removes a stage rather than a feature.

```
client ──ingress──▶ [ scheduler ]──▶ ┐
                    (optional)       ├──▶ sink ──▶ MIDI port / OSC destination
DSP ─────────────────────────────────┘
```

Both write the same sinks by different routes. A sink is the event-discipline
counterpart of a frame port: a slot, an identity, a policy for not keeping up,
and an endpoint off the audio thread.

**The DSP emits to a handle, not to an address.** Routing its output back
through ingress would make it address-match a string on the audio thread to
rediscover a destination it already knew — and would put outgoing MIDI through
ingress, which is exactly what it is not. The `/clockwork/` prefix stays what it
is: the rule for things arriving from *outside*.

**Client replies are not sinks.** `DspHost::emit_osc` already answers a
request or notifies listeners, and carries reply-routing semantics (an origin
token, or zero for broadcast) that a sink has no concept of. Sinks are for
*external destinations* — a MIDI port, an OSC endpoint. Answering a client and
sending to a MIDI port look alike and are not the same act.

**One consequence to build in rather than discover.** If the DSP holds the
schedule and writes sinks directly, a build with no clockwork scheduler has
exactly one clock and one schedule, which is strictly better than two. But
clockwork can then no longer see when anything was *meant* to happen, so a sink
must record what went out and when, or timing complaints become unfalsifiable —
the same reason ports count underruns rather than merely handling them.

**What the implementation had to settle, and did.** Four things this section
was silent on, now normative in `src/clockwork_event_sink.h`:

- **Order costs the audio thread nothing.** `clockwork_sink_send` appends in ARRIVAL
  order — one compare-exchange and a `memcpy` — and the drain sorts, off the
  audio thread, on `(when, arrival)`. Arrival is the tiebreak, so two messages
  due at the same instant leave in the order they were sent and a note-off
  cannot overtake its note-on. The guarantee is exactly "messages present at
  the same drain leave in `when` order"; one that arrives after an
  earlier-timed message has already gone out is due already, goes at once, and
  is counted `late`.
- **`late` is counted at FIRST SIGHT, not at delivery.** Read literally, a sink
  that holds a message until its moment releases it a hair afterwards and would
  count every message late. So the test is applied when the drain first sees
  one: already overdue then means the sink never had the chance. A working sink
  reports zero, which is what makes the number worth watching.
- **A message has a maximum size.** `capacity` counts messages and the memory
  is taken at open, so a cell has a fixed width. A longer message is refused
  whole and counted `dropped` — never truncated, because a shortened MIDI
  message is a different message.
- **`when` is signed at the ABI and unsigned in meaning.** 2026 is more than
  2^31 seconds after 1900, so every real OSC timetag is a negative `int64_t`
  today. Compared signed, "immediately" (1) sorts after next Tuesday.

## What it deliberately cannot do

A sink can only see what crosses the boundary: the DSP's outputs. It cannot tap an
arbitrary internal signal — which is exactly why the scope went blind when
buses died. Internal taps stay the DSP's own business, written into the shm
window `DspConfig` already hands it. A generic sink that pretended otherwise
would drag clockwork back into knowing what a bus is.

## Open questions

Marked with what the implementation decided, where it decided anything. A
decision made by code that nobody argued for is still open.

1. **Where do sinks tap?** ~~Pre- or post-~~ **Answered by construction: post,
   and only post.** A sink is written from `static_audio_bus` immediately after
   `dsp_process` returns, so it records what actually left clockwork. That is
   the right answer for recording and the only answer available: a pre-tap
   would have to be inside the DSP, and clockwork cannot see in there. A
   scope that wants a pre-tap still has to write it into the shm window itself,
   exactly as "What it deliberately cannot do" says.
2. **Rate and format conversion.** Recording at 44.1 kHz while the device runs
   at 48 implies resampling. Whose job — the sink, the substrate, or refused?
   **Still open, and it now decides behaviour.** The disk source does no conversion: a
   44.1 kHz file streamed into a 48 kHz session plays fast. It reports the
   file's rate (`clockwork_disk_source_sample_rate`) so a caller can notice, and
   stops there. The substrate is the wrong place — it would have to hold a
   resampler's state per port, on the audio thread — but "every endpoint
   resamples for itself" means writing it more than once.
3. **How much buffer, and who chooses?** Disk streaming needs enough lookahead
   to survive a seek and a slow read; too much wastes memory and adds latency.
4. **Sample-accurate starts.** If a streamed sample must begin on an exact
   frame, how does a pull request align to a block boundary — and what happens
   when the ring is not ready in time? (Probably: it does not start, and says
   so. Silently starting late is the worse answer.)
5. **One slot namespace or two?** ~~Sources and sinks could share numbering or
   be independent.~~ **Answered: one, shared.** A `ClockworkPort` names a slot in a
   single table whichever way it runs, and `clockwork_port_direction()` — which the
   header did not originally have — says which. Two namespaces would mean two
   tables, two full checks and a handle that means different things depending
   on where it came from. A loopback is now a source and a sink with a thread
   between them, and costs nothing structural.
