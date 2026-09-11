<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# Building

Needs CMake 3.24+, a C++20 compiler and a Rust toolchain. On Debian and
Ubuntu the device layer wants `libasound2-dev libudev-dev`.

Audio files need nothing installed: reading and writing them is
`src/clockwork_audio_file.h`, built from vendored single-file decoders and
clockwork's own FLAC encoder, so the formats are the same on every platform.

```sh
cmake -B build .
cmake --build build --parallel 3
```

That produces the library, its plugin bridge, and the test suite. There is no
standalone server: an engine is a library with no process shape of its own,
and the process around it — the command line, the sockets, the run loop — is
the client's. clockwork-supersonic's `host/` is one such process, composed
from the comms client library (`src/comms/`).

**Keep the parallelism modest.** An unbounded `make -j` will exhaust memory on a
small machine: this build compiles four JUCE modules and a Rust workspace.

## Tests

`-DBUILD_TESTS=ON` adds the Catch2 suite. Run it in both Link configurations —
the Link cases are `#if CLOCKWORK_LINK` inside the same files, so a build with
Link off compiles them away and passes without ever executing them:

```sh
cmake -B build -DBUILD_TESTS=ON .
cmake --build build --parallel 3
./build/test/clockwork_tests && ./build/test/clockwork_lanes_reserved_tests \
  && ./build/test/clockwork_attach_tests && ./build/test/clockwork_engine_tests

cmake -B build-link -DBUILD_TESTS=ON -DCLOCKWORK_LINK=ON -DCLOCKWORK_LINK_AUDIO=ON .
cmake --build build-link --parallel 3
./build-link/test/clockwork_tests && ./build-link/test/clockwork_lanes_reserved_tests \
  && ./build-link/test/clockwork_attach_tests && ./build-link/test/clockwork_engine_tests
```

`clockwork_lanes_reserved_tests` is a separate binary because it boots with
channels reserved above the device (where Link peer audio lands), and
clockwork is a process singleton.

The engine cases boot a whole engine, whose control plane is run by the
engine's own gateway thread unless the host takes it (`Config::
hostDrivesControl`, `ClockworkEngine::controlPass()`). Run them a second time
with every fixture in that shape, so a case that quietly depended on the
engine's thread shows itself:

```sh
CLOCKWORK_TEST_HOST_DRIVES_CONTROL=1 ./build/test/clockwork_engine_tests
```

The BEAM NIF has a suite of its own, in Elixir, under `test/nif`. It drives
the built library through `src/nif/clockwork.erl`, so build that first:

```sh
cmake -B build-nif -DCLOCKWORK_NIF=ON -DBUILD_TESTS=OFF .
cmake --build build-nif --target clockwork_nif
cd test/nif && CLOCKWORK_NIF_PATH=../../build-nif mix test
```

`CLOCKWORK_HEADLESS=1` boots without an audio device; `CLOCKWORK_QUIET=1`
silences the per-boot lifecycle lines.

## Choosing a DSP

`-DCLOCKWORK_DSP=<name>` selects a directory under `dsp/` holding a
`CMakeLists.txt` that defines a target of its own name and implements the entry
points in [`src/dsp_api.h`](../src/dsp_api.h). Clockwork and the DSP call each
other in both directions — clockwork runs blocks and hands over OSC, the DSP
answers through `DspHost` — so they are linked as a group.

`dsp/dummy` is the default and is meant to be deleted once a real DSP is
attached. See [BOUNDARY.md](BOUNDARY.md#5-the-dummy-dsp).

## Options

| option | default | |
|---|---|---|
| `BUILD_TESTS` | OFF | the Catch2 suite |
| `CLOCKWORK_DEVICE` | ON | the native audio device layer (smoothie/JUCE). OFF builds the substrate for a host that owns its own audio callback — a plugin, a worklet, an embedded target |
| `CLOCKWORK_DSP` | `dummy` | which DSP to link |
| `CLOCKWORK_MIDI` | ON | the Rust/midir MIDI subsystem |
| `CLOCKWORK_GAMEPAD` | ON | the Rust/gilrs gamepad subsystem |
| `CLOCKWORK_OSC` | ON | the Rust/std::net OSC subsystem |
| `CLOCKWORK_SCHEDULER` | ON | the timed-event store. OFF forwards every message immediately, carrying its timetag |
| `CLOCKWORK_CLIENT_VERBS` | ON | the client half of the verb surface: `midi/out/*`, `midi/clock/beat`, `osc/send`. OFF compiles them out and refuses them by name — for a guest that drives its own output through `clockwork_sink_send`. See [SURFACE.md](SURFACE.md) |
| `CLOCKWORK_PLUGINS` | ON | host CLAP and VST3 plugins. Uses installed SDK headers when found (`CLAP_INCLUDE_DIR`, `VST3_PLUGINTERFACES_INCLUDE_DIR`), else fetches the pinned MIT copies. Native only |
| `CLOCKWORK_NIF` | OFF | the BEAM NIF shared library |
| `CLOCKWORK_STANDALONE` | ON when top-level | the scheduler host (`clockwork-scheduler-host`) and this suite |
| `CLOCKWORK_CARGO_OFFLINE` | OFF | build the Rust subsystems with `--offline` (`--locked` is always passed) |
| `CLOCKWORK_RUST` | ON | build the Rust subsystems' umbrella staticlib with cargo. OFF for a Rust embedder (`rust/clockwork-sys`), which takes the subsystem crates itself; the targets that link a binary here — `CLOCKWORK_STANDALONE`, `BUILD_TESTS`, `CLOCKWORK_PLUGINS`, `CLOCKWORK_NIF` — must be OFF with it |
| `CLOCKWORK_SYSTEM_ZLIB` | OFF | link the system zlib instead of compiling the copy inside smoothie (distro builds) |
| `CLOCKWORK_SYSTEM_STB` | OFF | take stb_vorbis from the system stb (pkg-config `stb`; Debian's `libstb-dev`) instead of the vendored copy (distro builds) |
| `CLOCKWORK_PLUGIN_BRIDGE_DIR` | empty | a directory the engine also searches for the plugin bridge, after "beside its own executable" — for an installed layout that keeps the bridge out of PATH (`/usr/libexec/<pkg>`) |
| `CLOCKWORK_LINK` | **OFF** | Ableton Link tempo, transport and peers — GPL-2.0-or-later, see below |
| `CLOCKWORK_LINK_AUDIO` | **OFF** | Link peer audio in and publish out. Needs `CLOCKWORK_LINK` |
| `CLOCKWORK_ASIO` | **OFF** | Windows ASIO from the vendored Steinberg SDK, taken under GPLv3 |

## Two flags bring in third-party code

`CLOCKWORK_LINK` fetches Ableton Link (GPL-2.0-or-later); `CLOCKWORK_ASIO`
compiles the vendored Steinberg ASIO SDK, taken under GPLv3. Both OFF by
default. Everything else
clockwork links is permissive, the audio codecs included — see
[LICENSE](../LICENSE).

## Distro builds

A distribution build wants the archive's libraries where one exists, no
network at build time, and helper programs out of the user's PATH. The
options for that, and what stays vendored and why:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCLOCKWORK_SYSTEM_ZLIB=ON -DCLOCKWORK_SYSTEM_STB=ON \
      -DCLOCKWORK_CARGO_OFFLINE=ON \
      -DCLOCKWORK_PLUGINS=OFF \
      -DCLOCKWORK_PLUGIN_BRIDGE_DIR=/usr/libexec/<pkg> .
```

| in the tree | distro switch | if there is none, why not |
|---|---|---|
| zlib (inside smoothie) | `CLOCKWORK_SYSTEM_ZLIB` → `zlib1g-dev` | |
| stb_vorbis (`src/vendor/stb`, v1.22 unmodified) | `CLOCKWORK_SYSTEM_STB` → `libstb-dev` | |
| the Rust crates | `CLOCKWORK_CARGO_OFFLINE` with a `cargo vendor` tree, or the archive's `librust-*` packages | |
| CLAP and VST3 SDKs | `CLOCKWORK_PLUGINS=OFF` | fetched at configure time; no distro packages them |
| smoothie (`smoothie/`) | — | clockwork's own fork of four JUCE 7.0.12 modules, ISC; there is no packaged JUCE it could build against (JUCE 8 is AGPL and not taken). See `smoothie/README.md` |
| oscpack (`src/vendor/oscpack`) | — | release 1.1.0 plus three fixes the packaged 1.1.0 lacks (64-bit detection on aarch64, malformed messages refused). See `src/vendor/oscpack/CLOCKWORK-CHANGES.md` |
| midir (`external/midir`) | — | a fork: shared ALSA client, timestamped CoreMIDI send. See `external/midir/CLOCKWORK-CHANGES.md` |
| dr_wav, dr_flac, dr_mp3 (`src/vendor/dr_libs`) | — | single-file, MIT-0/Unlicense; no distro packages them |
| Ableton Link (`CLOCKWORK_LINK`) | `FETCHCONTENT_SOURCE_DIR_ABLETONLINK` pointing at a Link 4 tree with `external/link-*.patch` applied | Debian's `ableton-link-dev` is 3.x and lacks the patches |
| the OTP NIF headers (`external/erlang_headers`) | — | only compiled with `CLOCKWORK_NIF`, which a distro build leaves off |

The plugin bridge is a separate executable the engine spawns. It looks for it
beside its own binary first, then in `CLOCKWORK_PLUGIN_BRIDGE_DIR`, then
gives up (the `CLOCKWORK_PLUGIN_BRIDGE` environment variable overrides both).
Install it there and the engine in `bin/` and they find each other. CI builds
this shape as `config (distro)`.

## Building without a device layer

`-DCLOCKWORK_DEVICE=OFF` drops smoothie/JUCE and everything in `src/native/`
that opens, drives, watches, recovers or records an audio device. What remains
is the lanes ABI, the clock, the rings, the scheduler, ports, sinks, MIDI,
gamepad, OSC and the DSP boundary — for a host that is handed a callback rather
than opening a device.

`CLOCKWORK_PLUGINS` and `CLOCKWORK_NIF` are themselves device hosts and must be
OFF with it; the configure refuses the combination rather than letting it fail
to link — but the test suite is, and is the point:

```sh
cmake -B build-nodev -DBUILD_TESTS=ON \
      -DCLOCKWORK_DEVICE=OFF -DCLOCKWORK_PLUGINS=OFF -DCLOCKWORK_NIF=OFF .
cmake --build build-nodev --parallel 3
./build-nodev/test/clockwork_tests && ./build-nodev/test/clockwork_lanes_reserved_tests \
  && ./build-nodev/test/clockwork_attach_tests
```

The device-policy and recording cases are absent there, since what they test
is. CI runs this configuration as `config (no-device)`.

## Command transports

A host links the comms client library and chooses: UDP, or `--tcp`, `--uds`,
`--uds-dgram`, `--pipe`, `--shm-commands` in SuperSonic's host. See
[TRANSPORTS.md](TRANSPORTS.md).

## Embedding from Rust

A C++ host links `clockwork_bundle` and CMake carries the rest. A Rust host
takes the engine as a cargo dependency instead — `rust/clockwork-sys` — and
never builds the umbrella staticlib, which carries a copy of std the binary
already has. Its build script configures and builds the tree named by
`CLOCKWORK_SYS_SOURCE` (this one by default; a project that embeds clockwork
as a subdirectory names its root, as tau does from its `.cargo/config.toml`)
with `CLOCKWORK_RUST=OFF`, and links `clockwork`, the DSP the tree was
configured with, `smoothie` when the device layer is on, and each
platform's system libraries. The Rust half — the subsystems the C++ calls
back into, and the guest — comes as crates: tau's `tau-next`, or for
clockwork alone the subsystem crates in `rust/`, with the features
`DEP_CLOCKWORK_FEATURES` names for the tree's options. Both are named with
`extern crate` from the binary, which is what gets a crate linked.

Two things matter to a Rust host and to nothing else. `clockwork-scope`'s
`host` feature leaves out the hostless defaults its own tests link, which
would otherwise sit ahead of the engine on the line and answer first. And
the guest wants whole-program LTO in the host's release profile: its kernels
are called across crate boundaries on every sample, and without cross-crate
inlining the render can miss the callback's budget — a build-time cost that
is heard, not seen. (The link itself needs no LTO: the C++ archives call back
into some hundred and fifty `#[no_mangle]` symbols in the same graph, and
that resolves as rustc lays the line out — verified with link.exe and with
GNU ld in both profiles.) `CLOCKWORK_SYS_BUILD_DIR` shares an existing build
tree; `CLOCKWORK_SYS_CONFIG` picks the CMake configuration (Release whatever
the cargo profile). rerezzed is the worked example.

What such a host then CALLS is `rust/clockwork-client`: the client boundary
(`clockwork_client.h`) and the embed door (`clockwork_embed.h`) as safe
Rust — `Embed::boot` or `Embed::attach`, then `send`, `poll`, `tap`,
`region`, `metrics` (by name, from the schema), `clock` and `scope` on the
client it owns, every handle closing itself. It stands on `clockwork-abi`,
which the layout probe holds to the headers, and carries the crate's only
`unsafe`, each block with its reason. Its tests are a Rust host of exactly
this shape — the dummy DSP attached headless through `clockwork-sys`, with
`clockwork-native`'s `host` feature for the subsystems — so `cargo test`
in `rust/` builds the engine's CMake tree once and exercises the whole
boundary against it: pings answered, rings filled and drained, taps
watching without taking, a scope slot claimed and read back. A build
without a device layer skips only the boot case.

## Web

`scripts/build-web.sh` produces the WASM engine, the worklet and the workers as a
self-contained distribution. Plugin hosting is not in it: WASM cannot load native
code.

It needs `emcc` on the path, a nightly Rust toolchain (the engine's shared
memory wants `-Z build-std`), `npm install` for the bundler, and — for the
MIDI and gamepad cores — a `wasm-bindgen` CLI of the exact version the
`wasm-bindgen` crate has in `rust/Cargo.lock`. The build looks for one beside
the tree first, so it can be pinned without touching the box:

```sh
rustup run nightly cargo install wasm-bindgen-cli --version <lock version> --locked \
    --root rust/target/tools
npm run build:web
```

### MIDI and gamepad on the web

The worklet cannot own a MIDI port or a game controller: Web MIDI and the
Gamepad API exist on the main thread and nowhere else. So on the web those two
subsystems live in the client — `js/lib/midi_manager.js` and
`js/lib/gamepad_manager.js` do the I/O — and drive the SAME Rust crates the
native engine links, `clockwork-midi` and `clockwork-gamepad`, compiled by
`scripts/build-web-subsystems.sh` to wasm-bindgen modules (`dist/midi/`,
`dist/gamepad/`, their wasm beside the engine's in `dist/wasm/`). One core,
two hosts: the `/clockwork/midi/*` and `/clockwork/gamepad/*` contracts are
identical to the byte, replies included.

A page turns them on at construction — Web MIDI asks the user's permission,
so nothing asks until a page says so:

```js
new Clockwork({ baseURL: "/dist/", midi: true, gamepad: true })
```

and then speaks the same verbs a native client does: `/clockwork/midi/ports/list`,
`/clockwork/midi/out/enable`, `/clockwork/midi/out/note_on`, `/clockwork/schedule`
around any of them, `/clockwork/gamepad/notify/subscribe`. How a verb gets from
the worklet back to the main thread is [SURFACE.md](SURFACE.md#whose-thread).
Without the option, a MIDI or gamepad verb is refused by name with a reason that
says which option to set. A guest's own sends — a self-directed engine's sink
(`clockwork_event_sink.h`) — reach the same port the same way, and open it on
demand; they need `midi: true` and nothing else. And what the keyboard plays
goes the other way by one route too: into the engine, out to the clients that
subscribed, and to a guest that asked for events (`DspInfo::wants_events`),
each event carrying the moment it arrived — see
[SURFACE.md](SURFACE.md#inbound-events).

### Testing the web build

```sh
npm test            # the node suites: the client pieces, and the MIDI/gamepad
                    # boundaries against the real cores with fake devices
npm run test:web    # Playwright, both transports: boot, the round trips, and
                    # MIDI/gamepad end to end with fake devices installed over
                    # navigator.requestMIDIAccess / navigator.getGamepads
```

Both run against whatever `build:web` last produced; the node cases that need
the wasm skip, and say so, when it is not there.
