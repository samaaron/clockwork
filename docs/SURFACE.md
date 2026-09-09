<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# Two kinds of verb

Clockwork answers about two hundred OSC addresses under `/clockwork/`. They are
not one surface. They divide by **who is in charge**, and a guest that is in
charge of itself needs only half of them.

## Transport — every guest needs this

Clockwork owns the devices. Nothing in a guest can enumerate a MIDI port,
open a socket, or notice a device being unplugged, because the guest is behind
`dsp_api.h` and has no operating system under it. So this half cannot move:

    midi/ports, midi/ports/get, midi/ports/list, midi/ports.reply
    midi/in/enable, midi/out/enable
    midi/notify/subscribe, midi/notify/unsubscribe
    midi/in/*                     inbound events, as they arrive
    midi/clock/sync, midi/clock/tick
    midi/clock/follow, midi/clock/unfollow, midi/clock/followers
                                  a port clocked continuously from a timeline
    osc/cue-server/*, osc/notify/*
    devices/*, drivers/*, clock/*, notify, debug, statechange

Plus the part with no OSC address at all: `clockwork_sink_send(sink, bytes, when)`,
which is how a guest gets bytes onto a wire with a time attached. That is the
whole of what a self-directed guest needs from this side.

## Client verbs — only a guest that is not doing it itself

    midi/out/note_on, note_off, control_change, pitch_bend, program_change,
             channel_pressure, poly_pressure, raw, sysex,
             clock, start, stop, continue
    midi/clock/beat
    osc/send
    schedule, sched/flush

Every one of these exists because something OUTSIDE clockwork is driving it
event by event. They are conveniences for a client that speaks OSC to
clockwork directly, and they are the shape of a language runtime driving clockwork.

## Why the line is there

Engines sit on opposite sides of it.

**clockwork.** The client is in charge. It sends `/clockwork/midi/out/
note_on` and `/clockwork/schedule`, and clockwork holds and emits on its
behalf. Both halves are needed.

**A self-directed engine.** The VM is in charge. It holds its own schedule, and every MIDI
and OSC message it sends is one it decided to send — reaching a wire through
`clockwork_sink_send` with its own `when`, never through ingress. The client half is
not merely unused there; it is a second way to do something the VM is already
doing, with worse timing, because a message that goes out through ingress and
clockwork's queue arrives at the device later than one handed straight to the
platform with a timestamp on it.

This is the same split `CLOCKWORK_SCHEDULER` already makes for the timed
store, for the same reason and along the same boundary. `holds_schedule` in
`dsp_api.h` is the DSP declaring which side it is on.

## The option

The client half is compilable out, as the timed store already is:
`CLOCKWORK_CLIENT_VERBS=OFF` builds none of `midi/out/*` (`midi/out/enable`
is the transport's and stays), `midi/clock/beat` or `osc/send`, so a
VM-shaped build does not carry a surface it will never answer. `schedule` and
`sched/flush` follow `CLOCKWORK_SCHEDULER`, the same split made earlier along
the same line.

A client that sends one of them to such a build is refused BY NAME —
`/clockwork/error <address> "client verb: this build has none
(CLOCKWORK_CLIENT_VERBS=OFF)"` — rather than the verb falling through as
unknown, which would say the verb was never real instead of that this build
dropped it. The list is one function, `clockwork_sys_is_client_verb()` in
`src/clockwork_sys.h`, so a reader can see which verbs went rather than
inferring it.

The transport half stays in every build, because owning a device is the one
thing a guest cannot do for itself. That is, in the end, what clockwork is
for: it owns the hardware and the clock, and it hands both to whatever is in
charge.

## Inbound events

What a MIDI port or a game controller sent — `midi/in/*`, `gamepad/in/*`,
and the `midi/ports` / `gamepad/devices` pushes — is one thing wherever it
came from. A native subsystem's callback writes it into the IN ring; on the
web the client's front writes it there from the main thread, carrying the
moment it arrived as a trailing timetag. From there ONE route on the audio
thread (`clockwork_event_route`) sends it out over the egress to the clients
subscribed to that subsystem, and hands it to the guest if the guest asked
(`DspInfo::wants_events`) — the one thing under the prefix that reaches a
DSP, and only because it asked. A client subscribes the same way on both
hosts; a guest that sets the flag never learns which host it is on.

## Whose thread

Both halves are answered off the audio thread by one control pass: the
control-ring drain, the peer command plane, the MIDI clock producer, the
egress drain. Whose thread runs it is the host's choice. By default the
engine's own gateway thread runs it every audio block; a host that owns the
process sets `Config::hostDrivesControl` and calls
`ClockworkEngine::controlPass()` from a thread of its own, and the engine
starts no thread for it — the same way `Config::hostDrainsEgress` makes the
host the consumer of the egress rings.

On the web there is no pass to run: the worklet has no NRT thread, and no
device to answer a MIDI or gamepad verb with — Web MIDI and the Gamepad API
exist on the main thread only. So the audio thread FORWARDS every verb it
does not answer itself (ping, echo, the clock snapshot, `sched/flush`, the
asset hand-off) back out over the egress to the client that sent it, wrapped
in a bundle whose timetag is the call's time: 1 for a verb that never waited,
the scheduled moment for one the scheduler fired
(`clockwork_host_forward_route`, `clockwork_host_forward` in `lanes.h`). The
client's front (`js/lib/host_front.js`) is the far end: it answers
`midi/` and `gamepad/` through the managers, hands a timed send to the
browser with its moment as the timestamp, and refuses anything else under
the prefix by name — the same shape as the native gateway's far end, on the
one thread the web has for it.

A guest that is in charge of itself never sends a verb: it opens a sink and
sends with its own `when` (`clockwork_event_sink.h`). On the web the host is
that sink's endpoint too — installed by the same switch — so a guest's send
goes out the same way, `/clockwork/midi/sink/send <port> <bytes>` inside a
bundle carrying its time, and the front hands the browser the bytes with the
moment. The port is opened on demand, as opening a sink onto a port opens it
natively. Nothing about the guest changes between the two hosts: it said
when it meant, and the edge did the rest.
