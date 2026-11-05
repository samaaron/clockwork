<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Many engines in one process

A DAW loads every instance of a plugin into one process and renders them on
several threads, often one thread rendering many instances in turn. On iOS,
every instance of an Audio Unit extension shares one extension process. The
audio thread can be taken away at any moment (a mobile browser switching
tabs), and the host sets the sample rate and the ticks. Clockwork is built
for that host. An engine is a core. No core knows another exists. A host
makes as many as it wants, and the same code runs on every target.

## The model

### An engine is a core

A `ClockworkCore` (`src/clockwork_core.h`) is one engine: its memory, rings,
scheduler, clock, ports, sinks, taps and heap. `clockwork_core_new` makes
one, `clockwork_core_close` ends it, and every call in the lanes ABI
(`src/lanes/lanes.h`) takes the core it acts on. `clockwork_embed_attach`
and `clockwork_embed_boot` each make a core for their handle; a native
`ClockworkEngine` boots one. Closing a core ends its engine. A host that
wants to survive a rate change keeps its handle and reconfigures it, the way
a plugin instance is deactivated and activated again. A web worklet made
again over the same memory picks up the core that memory already holds.

Two cores never share anything. Each core is laid out from its own profile
(`src/clockwork_profile.h`): the target's default, or the plugin preset, so
a plugin core costs what its guest touches rather than a native app's whole
arena. We zero the guest's memory spans by giving their pages back to the
system (`src/os_pages.h`), so an untouched span costs nothing.

### The handle owns the lifetime

We never free a core's handle. Anything a thread may need to touch without
knowing whether the core still exists lives on the handle: the gate, the
count of threads inside without the gate, the closing flag, and the words
those threads sleep on.

- **The gate** (`core_state.h`, `GateHold`). One thread at a time works on
  the guest and its pools. A block takes the gate and never waits; a block
  that finds it held renders silence and counts the miss. A control call
  (init, rebuild, teardown) waits for the block in flight to finish. The
  holder's entry records that it holds the gate, so any call it makes
  inside takes the gate for free.
- **The entry** (`clockwork_core_enter`). Each thread records which core it
  is working inside and what kind of call put it there. The allocator
  routes by it and log lines name their core by it; nothing else reads it.
  Entering a core without the gate holds the core until you leave.
- **Close.** `clockwork_core_close` sets `closing` once (a second close
  returns immediately), wakes everyone waiting on the core (a client's wait
  returns `E_CLOSED`; a thread waiting for the gate learns the core closed),
  takes the gate, waits until nobody is inside, then frees the state and
  the core. A block that races a close takes the gate first and only then
  reads the state: closing or gone means `CLOCKWORK_TICK_NONE`.

### Memory

The heap (`rust/clockwork-heap`) is one allocator on every target. Each core
has a guest pool over the guest's bulk memory and an own pool for what
Clockwork's Rust subsystems allocate inside the core's doors. An address
map names, chunk by chunk, which heap owns a block, so a free from any
thread finds its pool. The global allocator routes by the thread's entry:
inside a core, that core's pools; outside, the system allocator. A core's
own pool must be empty when the core goes. If it isn't, we keep the span,
mark its chunks with a tombstone, and drop and count any late free
(`clockwork_heap_outlived_frees`) instead of handing span memory to the
system allocator.

### Logging

Every log line names its core: `clockwork_log(core, ...)` in C and C++,
`clockwork_log::log!(core, ...)` in Rust. The comms servers, the cue port,
the sink table and the MIDI service all receive the core when they are made.
An engine's log carries its own lines and nobody else's. A line that
belongs to no engine (JUCE's single logger, the reference host) goes to the
console. `scripts/check-logging.sh` fails any call that names no core.

### What stays one per process

Each shared service is counted. The first core that needs it starts it, the
last one to go stops it, and it must start again cleanly, because hosts
never unload the binary.

| what | why one | how it serves many |
|---|---|---|
| the global allocator | one per crate graph | routes by the thread's entry |
| the address map and the core registry | they are the lookup | — |
| the sink drain thread | one thread can drain every table | walks every live core's sinks |
| MIDI's OS client | ALSA gives a process few clients | one counted client; every core hears every port |
| the gamepad poller | one set of devices; gilrs cannot be created twice | fans events out to every core; never stops |
| the log sink | one stateless function | every line names its core |
| the device platform (JUCE, booted engines only) | `MessageManager`, `startPlatform`, its logger | started once; each booted engine opens its own device session |

We do not offer many engines on one device. That is a mixer, a feature
above cores, and it stays out until a host asks for it.

### The guest

`DspInfo::instances` says how many instances of the guest one process may
hold. 0 (what a zeroed `DspInfo` gives you) means one: a guest that says
nothing is assumed to keep process-wide state. `DSP_INSTANCES_ANY` is a
promise that it keeps none, and two checks hold the guest to it
(docs/BOUNDARY.md): its built objects contain no mutable static
(`scripts/check-guest-globals.sh`), and two instances running at once
render, bit for bit, what each renders alone. A core keeps its place while
its own instance is freed and rebuilt. A build past the count is refused
with the reason, and an embed attach or boot whose guest did not start is
refused with `E_NO_INSTANCE`. scsynth declares any (SuperSonic, since
2026-11-05: its statics went into the World, into constants and behind a
leading `World*`; its sync guide says how).

Whether a guest renders the same twice is the guest's own affair: a seed
for its random numbers is one of its options, given in its config text
before the build like any other (scsynth's `randomSeed`). Clockwork has
no seed of its own.

### Plugin binaries

A plugin built on Clockwork exports its entry points and nothing else
(`clockwork_plugin_module()` in CMakeLists.txt, `CLOCKWORK_PLUGIN_ENTRY` in
`src/clockwork_plugin.h`), so two plugins that each carry a Clockwork never
bind to each other's in one host. No panic crosses the C boundary: every
`extern "C"` function in the Rust sources is wrapped in the fence
(`rust/clockwork-fence`), and `scripts/check-ffi.sh` checks that.

### Targets

One code path. The gate and the wait words come from `clockwork-wait`,
whose waits are each platform's own; nothing here makes a thread or assumes
a timing. Two targets still hold one core per process: ESP-IDF, whose arena
is static (an external constraint), and the web, where a wasm instance's
memory holds one arena today. The web limit is our own choice, and the one
place the property is not yet uniform.

## What keeps it true

- `scripts/check-globals.sh`: every mutable variable with static storage in
  Clockwork's own code is on `scripts/globals.list` with its class and its
  reason, and none belongs to an engine. Runs on macOS and Linux in CI.
- `scripts/check-guest-globals.sh`: a guest that says any has no mutable
  static at all (C and C++ objects; we don't read Rust guests yet).
- `scripts/check-layout.sh`, `scripts/check-ffi.sh`,
  `scripts/check-logging.sh`, `scripts/check-threads.sh`, each with a canary
  and a count.
- `test/test_reference.cpp`: each scenario alone against a recorded trace
  (`test/reference/<platform>/`, darwin-arm64 so far), then the same
  scenarios beside each other (in turn, on threads, with a seeded pool,
  churning between blocks), each core held to its trace alone. The threaded
  cases run under the sanitizers in CI.
- `test/test_core_close.cpp`: a client parked on a core that closes, a block
  racing a close, two closes at once. `test/test_embed.cpp`: two engines
  side by side render what each renders alone. `test/test_guest_once.cpp`: a
  guest held once, with the second core and the second attach refused.
- `test/test_plugin_module.cpp`: two copies of a plugin binary in one
  process, each running its own Clockwork, and no Clockwork name visible
  from outside either.

## Open

- Lift the web's one core per instance, so a worklet can run several.
- Two address-to-core lookups (`clockwork_core_of_memory` and the heap's
  map): fold them into one.
- Reference traces for Linux and Windows; the globals guard on Windows (the
  runner needs a demangler).
- `scripts/globals.list` carries "to be labelled" lines for constants that
  should be `constexpr`.
- `reset_taps` clears the whole taps region at every build; clearing the
  slot headers would do.
