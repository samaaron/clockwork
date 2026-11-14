<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->

# Proposal: one host, one client, the same on every target

**Status: a proposal for review. Nothing here is built except where it says so.**
Written 2026-11-13 from two reads of this working copy: a survey of how each
target does its IO, and an earlier audit of code that assumes several clients.
Claims about the code come from those reads; eight of the survey's citations
and four of the audit's were checked by hand, the rest were not.

## The constraints

1. **One host and one client.** A host embeds Clockwork and drives its blocks.
   At most one client talks to it at a time. Any number of readers may read
   the published memory, because a reader can change nothing.
2. **The same on every target.** A concept that exists on one target exists on
   all of them, unless something outside Clockwork makes that impossible.
3. **The whole system.** Clockwork is timing, Link, MIDI, game controllers,
   OSC and parameters as much as it is a place to run DSP.

## Where the code is today

There are three kinds of host. They differ in what sits beside the audio
thread to do the work that is not audio.

| | Full engine | Web | Bare core |
|---|---|---|---|
| Used by | native app, embed boot, the NIF | the worklet and its page | embed attach (every plugin), a bare lanes host |
| Who calls each block | Smoothie | the browser | the host |
| Beside the audio thread | `ClockworkEngine`'s control thread | `HostFront` on the page | nothing |
| Clock commands | control thread | audio thread | audio thread (built this week) |
| Timeline set by | clients, Link peers | clients; the page also writes the memory itself | the host, as one more source (built this week) |
| MIDI in | Clockwork opens ports | the page opens Web MIDI | none |
| MIDI out | sink, drain thread, midir | handed to the page | refused |
| Game controllers | Clockwork's crate | the page's Gamepad API | none |
| OSC to other programs | UDP in and out | none | none |
| Notices to the client | by subscription, per connection | all of them | host timeline moves only |

Boards (ESP32, Teensy) have memory sizes and a heap backend in this
repository and no host, so they are not in the table.

What is already the same everywhere: the tick, the engine's clock rule, the
rings and the client handle, the scheduler, the parameter table, assets, the
guest's callbacks, the timeline and sample clock as memory, and the route
that carries an incoming MIDI or controller event to the client and the guest.

### What conflicts with one client

All of it is in the full engine's control thread.

- Five subscriber lists keyed by connection, with broadcast routes and
  `/clockwork/notify/clear`. *(Gone.)*
- Replies routed by a message's origin; origin 0 standing for "the
  subscribers", so some replies for the host can reach the remote client.
  *(Gone.)*
- Plugin tracks that answer the caller and broadcast as well.
- Nothing refuses a second command peer; the shared-memory peer is drained
  beside whatever transport is installed.
- Device-switch results are broadcast only; an asset's release goes to "the
  connection that committed it".
- Docs and test names that still speak of several clients.

The web and the bare core have none of this. They are one-client systems
already.

### What native and the web already share

More than the table suggests. MIDI and controller messages are one Rust core
(`clockwork-midi`, `clockwork-gamepad`), built natively and to wasm, with the
port and device replies in one wire form. The clock's commands are one
handler. What differs is who opens the port, which the platform decides, and
the glue that calls the shared core: `MidiControl.cpp` and
`GamepadControl.cpp` natively, `midi_manager.js`, `gamepad_manager.js` and
`host_front.js` on the web.

So the gap is not three designs. It is that the bare core, which every
plugin uses, has no glue at all and no way through `clockwork_embed.h` to
hand those jobs to its host.

### What does differ

- Re-blocking between the host's buffers and the engine's block: separate
  code natively and in the embed; the web needs none.
- MIDI clock in: a timeline natively, a tempo estimate on the web.
- Some MIDI clock out commands are refused on the web.
- Fades around a gap: out and in natively, in only on the web, none in the
  embed.

## The proposed shape

**Every target is the core, a host and at most one client.**

### The core

The same code everywhere, and all a guest ever sees: the tick, the clock,
the rings, the scheduler, the parameter table, the timeline, the event route,
the sinks.

It answers on the audio thread every command that needs no thread of its
own. That includes the clock commands, which the web build already answers
there. A plugin then answers them too, and native and web run one handler on
one thread.

### The host

A host supplies what the core cannot reach by itself. For each service it
either supplies it or says it has none.

| Service | The core does | A host supplies |
|---|---|---|
| Audio | runs the guest a block at a time | the buffers, and the call for each block |
| Engine time | keeps one clock from the frame count | its frame count, and a wall clock if it has one |
| Timeline | keeps tempo, meter, playing and beat for every reader | nothing, or its own transport, as one more source beside clients and Link peers (built this week) |
| MIDI and controllers in | carries each event to the client and the guest, with its time | the events, written to the core |
| MIDI out | holds a timed send until its time | the far end: a port, or its own event list |
| Link | mirrors the session into the timeline | the session, where the target can have one |
| OSC to other programs | the same, as a sink | the socket, where the target has one |

The four hosts are then four lists of what is supplied:

- **Native:** Smoothie for audio; Clockwork's own MIDI, controller, Link and
  UDP code for the rest. This is today's `ClockworkEngine`, described as a
  host and not as the engine.
- **Web:** the browser for all of it, through `HostFront`.
- **Plugin:** the DAW for audio, transport and MIDI; none for devices.
- **Board:** its codec and pins.

The switch that makes this possible exists: `clockwork_host_forward` hands
the host the commands the core does not answer and the guest's MIDI sends.
The web uses it. The embed's header does not expose it.

### The client

One, or none.

- No subscriber lists and no subscribe commands. Every reply and every notice
  goes to the client.
- One command replaces what subscribing answered today (the version, the
  ports there now, the timeline): the client asks, and is told.
- The engine refuses a second command peer.
- Readers of the published memory are not clients and are not limited.

## What it costs downstream

Read in SuperSonic and Sonic Pi on 2026-11-13 by search, not by a full audit.

- **SuperSonic:** its own source sends none of the subscribe commands. Its
  docs (`OSC_API.md`, `NATIVE.md`, `EMBEDDING.md`, `NIF.md`) and four native
  tests describe them.
- **Sonic Pi, native:** Spider sends `/clockwork/notify` at boot and again
  after every device switch, and uses its reply as the sign that the engine
  is up and for its version (`scsynthexternal.rb`, `studio.rb`, `server.rb`).
  `link_api.rb` subscribes to the clock's notices. With one client the
  re-registering after a device switch goes away.
- **Sonic Pi, web:** the page subscribes to MIDI and controllers and reads
  the ports from the reply (`app/web/app/src/main.js`).
- **Sonic Pi, plugin:** listens for the clock's notices and subscribes to
  nothing.

## Decisions this needs

1. **The NIF** sends each reply to every registered Erlang process. Is that
   one client fanning out on its own side, or several clients?
2. *(Decided 2026-11-13, and built.)* There is no leader. The timeline
   starts at its default, as a Link session does, and a host's transport is
   one more source of it: the host says it with every render, so what
   another sets lasts until it does, and what it never says is left to the
   others. Nothing is refused. A flag in the clock state says a host has a
   transport, for a reader that would wait for it.
3. **Events inside a block.** No target can place an incoming event at a
   frame within a block. A DAW gives a frame for every MIDI event.
4. **Engine time in a plugin.** The embed's header promises time from frames
   alone; the code feeds it the wall clock. Offline rendering needs the first.
5. **Link in a plugin.** Off in today's build. If on, it can only send the
   DAW's tempo out.
6. **The page writing the timeline's memory**, beside the commands that do
   the same: two ways to set one thing.
7. **Boards.** Where their host code is, and whether it should be read into
   this.

## Order of work

1. Land the host's transport with the parameter table.
2. *(Built 2026-11-13.)* Clock commands answered by the core on every
   target with no control thread: an attached core answers them as the
   worklet does. A client's set of the timeline goes the way a host's
   transport does, and is announced the same way. The full engine still
   answers on its control thread; Link's own session verbs need one.
3. *(Built 2026-11-13 and 14.)* One client, and nothing decided about who
   a message is for. The subscriber lists are gone: what the client asked to
   be told is five switches kept by the core (`clockwork_client_wants`,
   `lanes.h`), the same on every host, and a notice is written only while
   its switch is on. The subscribe verbs stay and turn them. Everything on
   the egress goes to the client: `IOscTransport` is one call, `deliver`,
   and an egress frame is the OSC with no route word. The engine's lifecycle
   is told to a client that sent `/clockwork/notify`, a host in the same
   program included. The web front's own copy of the switches is removed.
   `/clockwork/notify/clear` is removed. SuperSonic's host, its front and
   the NIF follow.

   Left for later, on size and not on merit:
   - **The origin.** Removed (2026-11-14): `dsp_osc` and `emit_osc` carry
     none, the scheduler stores none, and no answer brings one back. A
     client that matches answers to requests carries an id in its own
     messages. What stays is the ring header's field, as the writer label:
     a sender's send call sets it (`writer`), the web labels the writing
     thread with it, and a tap or the traffic log reads it.
   - **Subscribing on a full engine** is taken by the control thread, by the
     control ring, while events are routed on the audio thread. An event
     sent straight behind a subscribe can pass before the switch is on. The
     web and a plugin turn the switch on the audio thread, in order.
   - **A device switch or reopen the client asked for** reports its result
     only to a client that also sent `/clockwork/notify`.
   - **Sorting what is SuperSonic's or Sonic Pi's from what is Clockwork's**:
     the subscribe-and-resend acknowledgement, the ignored reply port on
     `devices/report`, the front seam, the notify handshake and snapshot.
4. A host's MIDI through the embed, in and out, with a frame for each event.
5. Engine time from frames for a host that renders.
6. The plugin formats (CLAP, VST3, AU) as one host over 1 to 5.
7. Describe `ClockworkEngine` as the native host; fold the duplicates
   (re-blocking, fades, MIDI clock in) into the core as they are met.
