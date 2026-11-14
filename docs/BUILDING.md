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

That builds the library and, with `CLOCKWORK_PLUGINS` (on by default), its
plugin bridge. When clockwork is the top-level project it also builds the
scheduler host, `clockwork-scheduler-host` (`src/host/main.cpp`): an OSC and
MIDI event scheduler with no synthesis engine, which drives the timed store
off the wall clock behind a TCP control port. The test suite is built only
when asked for (`-DBUILD_TESTS=ON`, below).

There is no standalone engine server: an engine is a library with no process
shape of its own, and the process around it — the command line, the sockets,
the run loop — is the client's. SuperSonic's `host/` is one such process,
composed from the comms client library (`src/comms/`).

**Keep the parallelism modest.** An unbounded `make -j` will exhaust memory on a
small machine: this build compiles four JUCE modules and a Rust workspace.

## The web module

The same tree, under `emcmake`, produces the AudioWorklet module: the same
lists of sources and the same guest, with the subsystems a worklet cannot
have (the device, MIDI, the socket, the plugin host, Link) switched off before
their options are read. It needs the Emscripten SDK on the shell's path, node
(the memory sizes are read from `js/memory_layout.js` at configure time), and
a Rust nightly with `rust-src` — `std` is rebuilt with atomics for the shared
heap, which only nightly can do; `CLOCKWORK_RUST_NIGHTLY` names the toolchain
(default `nightly-2026-07-02`, falling back to a floating `nightly`).

```sh
emcmake cmake -B build/web -DCMAKE_BUILD_TYPE=Release -DCLOCKWORK_DSP=dummy
cmake --build build/web --parallel 3      # → dist/wasm/clockwork.wasm
```

`scripts/build-web.sh` wraps that for this tree: it configures `build/web`,
writes `dist/wasm/clockwork.wasm`, and bundles the JavaScript client and
workers into `dist/`. A product sets these on its own `emcmake cmake` ("Your
own guest", below): its module's name (`CLOCKWORK_WEB_MODULE_NAME`) and where
it lands (`CLOCKWORK_WEB_DIST_DIR`), the scheduler's size for its worklet
(`CLOCKWORK_SCHEDULER_SLOT_COUNT`, `CLOCKWORK_SCHEDULER_DATA_POOL_SIZE`), and
exports of its own (`CLOCKWORK_WEB_EXTRA_EXPORTS`); the exports the worklet
calls are clockwork's list in `CMakeLists.txt`.

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

CI runs each suite twice, because each way sees what the other cannot:

```sh
ctest --test-dir build --output-on-failure -j 4      # each case in a process of its own
./build/test/clockwork_tests --order rand             # every case in one process, shuffled
```

A case that only passes after another has run — reading something an earlier
case left in the process — fails under `ctest` and nowhere else. A case that
leaves something behind for the next fails in the shuffled run and nowhere
else. A shuffled run prints its seed first ("Randomness seeded to");
`--order rand --rng-seed <seed>` runs the same order again.

`clockwork_lanes_reserved_tests` is a separate binary because it boots with
channels reserved above the device (where Link peer audio lands), and every
case in a binary is written against its fixture's one geometry.

The engine cases boot a whole engine, whose control plane is run by the
engine's own gateway thread unless the host takes it (`Config::
hostDrivesControl`, `ClockworkEngine::controlPass()`). Run them a second time
with every fixture in that shape, so a case that quietly depended on the
engine's thread shows itself:

```sh
CLOCKWORK_TEST_HOST_DRIVES_CONTROL=1 ./build/test/clockwork_engine_tests
```

The device cases run on fake devices (`test/FakeAudioDevice.h`), and on each
real driver's behaviour: what CoreAudio, WASAPI, ASIO, ALSA, JACK and
PipeWire each name, report and pair (`fake_audio::behaveLike`). A run on any
one machine covers what every platform's driver does, which holds only while
the engine decides by asking the driver, not by the platform it was built
for. After each step a case checks that the engine agrees with itself about
its device (`test/DeviceInvariants.h`): a device is open, its callback is the
engine's source and the engine is not waiting, or none of the three, and an
open device keeps rendering.

The BEAM NIF has a suite of its own, in Elixir, under `test/nif`. It drives
the built library through `src/nif/clockwork.erl`, so build that first:

```sh
cmake -B build-nif -DCLOCKWORK_NIF=ON -DBUILD_TESTS=OFF .
cmake --build build-nif --target clockwork_nif
cd test/nif && CLOCKWORK_NIF_PATH=../../build-nif mix test
```

clang-tidy runs over Clockwork's own sources with the checks in `.clang-tidy`
(the crash classes the engine has had: owning raw memory, empty catches,
exceptions out of threads, destructors that should be virtual) against the
compile database of a build:

```sh
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
scripts/tidy.sh build                                  # every file
scripts/tidy.sh build src/native/ClockworkEngine.cpp   # one file
```

CI runs it on Linux and macOS and puts the counts per check in the job
summary; what fails the job is `WarningsAsErrors` in `.clang-tidy`. Vendored
code and smoothie are not tidied: smoothie is JUCE's code, fixed in tree
only where Clockwork touches it.

`CLOCKWORK_HEADLESS=1` boots without an audio device. The engine logs through
`clockwork_log` only (`clockwork_log::log!` on the Rust side), onto its debug
ring: a host prints those lines (or not), the test fixture captures them and
shows them when a test fails, and the engine itself writes nothing to stdio.
`scripts/check-logging.sh` enforces that, and CI runs it.

The audio file path is tried on files from the world as well as the
fixtures: `scripts/check-samples.sh` takes every sample Sonic Pi ships (the
`supersonic-scsynth-samples` npm package, at a pinned version, cached under
`build/`) the whole way round — decoded by clockwork and by ffmpeg and
compared sample for sample, played through the pumped engine, written back
out and read by both (`test/test_samples_through_engine.cpp`). Run it after a
build and before pushing a change to the decoders, the writers or the audio
path; CI runs the same script. Needs npm and ffmpeg.

## Your own guest

A guest is a CMake target that implements the entry points in
[`src/dsp_api.h`](../src/dsp_api.h) ([BOUNDARY.md](BOUNDARY.md)).
`-DCLOCKWORK_DSP=<name>` names the target, and `CLOCKWORK_DSP_DIR` the
directory holding its `CMakeLists.txt`, wherever it is; left empty, that is
`dsp/<name>` in this tree. Clockwork and the guest call each other in both
directions — clockwork runs blocks and hands over OSC, the guest answers
through `DspHost` — so they are linked as a group.

`dsp/dummy` is the default, there so this tree can build and test itself
([BOUNDARY.md](BOUNDARY.md#the-dummy-dsp)). A product leaves it in place and
points the two variables at its own guest.

| variable | default | |
|---|---|---|
| `CLOCKWORK_DSP` | `dummy` | the guest's CMake target, which must exist by that name |
| `CLOCKWORK_DSP_DIR` | empty: `dsp/<CLOCKWORK_DSP>` here | the directory holding the guest's `CMakeLists.txt` |
| `CLOCKWORK_RUST_WORKSPACE` | clockwork's `rust/` | the cargo workspace cargo runs in, whose `target/` the archive is read from |
| `CLOCKWORK_RUST_PACKAGE` | `clockwork-native` | the umbrella package cargo builds (`-p`) |
| `CLOCKWORK_RUST_LIB_NAME` | `clockwork_native` | the base name of the staticlib that package makes |
| `CLOCKWORK_WEB_MODULE_NAME` | `clockwork` | the web module's file name, without `.wasm` |
| `CLOCKWORK_WEB_DIST_DIR` | clockwork's `dist/wasm` | where the web module is written |

`scripts/build-web.sh` passes the first five through: `CLOCKWORK_DSP` from
the environment or `--dsp=<name>` (default `dummy`), and the other four from
the environment, clearing each one that is unset. It does not pass
`CLOCKWORK_WEB_MODULE_NAME` or `CLOCKWORK_WEB_DIST_DIR`: it always writes
clockwork's `dist/wasm/clockwork.wasm` and bundles into clockwork's `dist/`.
A product that names its own module runs `emcmake cmake` itself, as below.

### A C or C++ guest

```
myproduct/
  CMakeLists.txt
  clockwork/                   this repository, as a submodule
  dsp/mysynth/CMakeLists.txt
  dsp/mysynth/mysynth.cpp      the nine entry points
```

```cmake
# myproduct/CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(myproduct LANGUAGES C CXX)
set(CLOCKWORK_DSP     "mysynth" CACHE STRING "" FORCE)
set(CLOCKWORK_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dsp/mysynth" CACHE PATH "" FORCE)
add_subdirectory(clockwork)
add_executable(myproduct main.cpp)
target_link_libraries(myproduct PRIVATE clockwork_bundle)
```

```cmake
# myproduct/dsp/mysynth/CMakeLists.txt
add_library(mysynth STATIC mysynth.cpp)
target_link_libraries(mysynth PRIVATE clockwork)   # for dsp_api.h and the headers it includes
```

`clockwork_bundle` is the one target a product links: clockwork, the guest
and the Rust archive, in the order the link needs. As a subdirectory,
clockwork builds neither its tests nor the scheduler host
(`CLOCKWORK_STANDALONE` is off).

### A Rust guest

A binary may link exactly one Rust staticlib, since each carries its own copy
of std. So a Rust guest makes no staticlib of its own: the product builds one
umbrella crate that holds clockwork's Rust subsystems and the guest, and
points CMake at it.

```
myproduct/
  CMakeLists.txt
  clockwork/
  dsp/mysynth/CMakeLists.txt       a target named mysynth, still required
  dsp/mysynth/mysynth_guest.cpp
  crates/mysynth-dsp/              the guest: the nine entry points, #[no_mangle] extern "C"
  crates/mysynth-native/           the umbrella: Cargo.toml, Cargo.lock, src/lib.rs
```

The umbrella depends on `clockwork-native` with its default features off,
and forwards the feature names CMake passes it. CMake builds it with
`--no-default-features` and `--features` from the build's options: `midi`
(and `winrt` on Windows), `gamepad`, `osc` with `comms`, and `schedule`.

```toml
# crates/mysynth-native/Cargo.toml
[package]
name = "mysynth-native"
version = "0.1.0"
edition = "2021"
publish = false

[lib]
crate-type = ["staticlib"]

[features]
default = []
midi = ["clockwork-native/midi"]
gamepad = ["clockwork-native/gamepad"]
osc = ["clockwork-native/osc"]
comms = ["clockwork-native/comms"]
schedule = ["clockwork-native/schedule"]
winrt = ["clockwork-native/winrt"]

[dependencies]
clockwork-native = { path = "../../clockwork/rust/clockwork-native", default-features = false }
mysynth-dsp = { path = "../mysynth-dsp" }

# clockwork's workspace patches one crate (rust/Cargo.toml). A patch applies
# only from the workspace root, so the umbrella repeats it.
[patch.crates-io]
symphonia-bundle-flac = { path = "../../clockwork/external/symphonia-bundle-flac" }

[profile.release]
opt-level = 3
lto = true
```

```rust
// crates/mysynth-native/src/lib.rs
pub use clockwork_native;
pub use mysynth_dsp;
```

Naming both crates is what puts each in the archive: clockwork's subsystems
with its global allocator, and the guest's entry points. The guest crate
takes the boundary's types from `clockwork-abi` (`clockwork_abi::dsp_api`),
whose function types check a definition's signature
(`const _: clockwork_abi::dsp_api::DspNewFn = dsp_new;`).

```cmake
# myproduct/CMakeLists.txt, beside the two CLOCKWORK_DSP lines above
set(CLOCKWORK_RUST_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/crates/mysynth-native" CACHE PATH "" FORCE)
set(CLOCKWORK_RUST_PACKAGE   "mysynth-native" CACHE STRING "" FORCE)
set(CLOCKWORK_RUST_LIB_NAME  "mysynth_native" CACHE STRING "" FORCE)
```

```cmake
# myproduct/dsp/mysynth/CMakeLists.txt
add_library(mysynth STATIC mysynth_guest.cpp)
target_link_libraries(mysynth PRIVATE clockwork)
```

What CMake assumes of the umbrella:

- **A committed `Cargo.lock`.** cargo always runs with `--locked`, which
  fails when the lock is missing or would change. Run `cargo generate-lockfile`
  in the umbrella once and commit the result.
- **Its release profile.** CMake passes `--release` for every build type but
  Debug, so the umbrella workspace's `[profile.release]` builds the
  subsystems and the guest, not clockwork's. Copy clockwork's
  (`opt-level = 3`, `lto = true`): the guest's kernels are called across
  crate boundaries on every sample.
- **The library's path.** CMake reads
  `<CLOCKWORK_RUST_WORKSPACE>/target/<profile>/lib<CLOCKWORK_RUST_LIB_NAME>.a`
  (`<name>.lib` on Windows), and `target/<triple>/<profile>/` for the web,
  iOS and a sanitizer build. So `CLOCKWORK_RUST_WORKSPACE` is the workspace
  root, where `target/` is, and nothing may move that directory
  (`CARGO_TARGET_DIR`, `build.target-dir`).
- **A CMake target named `${CLOCKWORK_DSP}`.** clockwork links the guest's
  target into the group whatever language the guest is written in. For a
  Rust guest it is a static library of one C++ file that includes
  `dsp_api.h` and defines one function, so the library is not empty.

### For the web

The same guest builds into the web module. The Rust half needs the nightly
toolchain `CLOCKWORK_RUST_NIGHTLY` names (default `nightly-2026-07-02`, else
a floating `nightly`), with `rust-src`, and the target:

```sh
rustup toolchain install nightly-2026-07-02 --component rust-src
rustup target add wasm32-unknown-emscripten --toolchain nightly-2026-07-02
```

CMake rebuilds std with atomics for the shared heap: cargo runs with
`--target wasm32-unknown-emscripten -Z build-std=std,panic_abort`, and with
`RUSTFLAGS=-C target-feature=+atomics,+bulk-memory,+mutable-globals` in its
environment. cargo takes `RUSTFLAGS` in place of any `rustflags` the
product's own `.cargo/config.toml` sets, not beside them, so those do not
reach this build.

```sh
emcmake cmake -S . -B build/web -DCMAKE_BUILD_TYPE=Release \
      -DCLOCKWORK_WEB_MODULE_NAME=mysynth \
      -DCLOCKWORK_WEB_DIST_DIR="$PWD/dist/wasm"
cmake --build build/web --target clockwork_web --parallel 3   # → dist/wasm/mysynth.wasm
```

The JavaScript client and its workers are bundled as `scripts/build-web.sh`
does in its last section: esbuild over `clockwork/js/clockwork.js` and each
of `clockwork/js/workers/*.js`. The page names the module with `wasmUrl`
("A web page", below).

## Options

| option | default | |
|---|---|---|
| `BUILD_TESTS` | OFF | the Catch2 suite |
| `CLOCKWORK_DEVICE` | ON | the native audio device layer (smoothie/JUCE). OFF builds the substrate for a host that owns its own audio callback — a plugin, a worklet, an embedded target |
| `CLOCKWORK_DSP` | `dummy` | which DSP to link; see "Your own guest" for it and the variables beside it |
| `CLOCKWORK_MIDI` | ON | the Rust/midir MIDI subsystem |
| `CLOCKWORK_GAMEPAD` | ON | the Rust/gilrs gamepad subsystem |
| `CLOCKWORK_OSC` | ON | the Rust/std::net OSC subsystem |
| `CLOCKWORK_SCHEDULER` | ON | the timed-event store. OFF forwards every message immediately, carrying its timetag |
| `CLOCKWORK_CLIENT_VERBS` | ON | the client half of the verb surface: `midi/out/*`, `midi/clock/beat`, `osc/send`. OFF compiles them out and refuses them by name — for a guest that drives its own output through `clockwork_sink_send`. See [SURFACE.md](SURFACE.md) |
| `CLOCKWORK_PLUGINS` | ON | host CLAP and VST3 plugins. Uses installed SDK headers when found (`CLAP_INCLUDE_DIR`, `VST3_PLUGINTERFACES_INCLUDE_DIR`), else fetches the pinned MIT copies. Native only |
| `CLOCKWORK_NIF` | OFF | the BEAM NIF shared library |
| `CLOCKWORK_STANDALONE` | ON when top-level | the scheduler host (`clockwork-scheduler-host`), and this tree's suite when `BUILD_TESTS` is on too |
| `CLOCKWORK_CARGO_OFFLINE` | OFF | build the Rust subsystems with `--offline` (`--locked` is always passed) |
| `CLOCKWORK_RUST` | ON | build the Rust subsystems' umbrella staticlib with cargo. OFF for a Rust embedder (`rust/clockwork-sys`), which takes the subsystem crates itself; the targets that link a binary here — `CLOCKWORK_STANDALONE`, `BUILD_TESTS`, `CLOCKWORK_PLUGINS`, `CLOCKWORK_NIF` — must be OFF with it |
| `CLOCKWORK_PLUGIN_BRIDGE_DIR` | empty | a directory the engine also searches for the plugin bridge, after "beside its own executable" — for an installed layout that keeps the bridge out of PATH (`/usr/libexec/<pkg>`) |
| `CLOCKWORK_LINK` | **OFF** | Ableton Link tempo, transport and peers — GPL-2.0-or-later, see below |
| `CLOCKWORK_LINK_AUDIO` | **OFF** | Link peer audio in and publish out. Needs `CLOCKWORK_LINK` |
| `CLOCKWORK_ASIO` | **OFF** | Windows ASIO from the vendored Steinberg SDK, taken under GPLv3 |

## Two flags bring in third-party code

`CLOCKWORK_LINK` fetches Ableton Link (GPL-2.0-or-later); `CLOCKWORK_ASIO`
compiles the vendored Steinberg ASIO SDK, taken under GPLv3. Both OFF by
default. Everything else clockwork links is permissive or, for the symphonia
audio decoders, MPL-2.0 (file-scoped, so it touches none of clockwork's own
files) — see [LICENSE](../LICENSE).

## Distro builds

A distribution build wants the archive's libraries where one exists, no
network at build time, and helper programs out of the user's PATH. The
options for that, and what stays vendored and why:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCLOCKWORK_CARGO_OFFLINE=ON \
      -DCLOCKWORK_PLUGINS=OFF \
      -DCLOCKWORK_PLUGIN_BRIDGE_DIR=/usr/libexec/<pkg> .
```

| in the tree | distro switch | if there is none, why not |
|---|---|---|
| the Rust crates | `CLOCKWORK_CARGO_OFFLINE` with a `cargo vendor` tree, or the archive's `librust-*` packages | |
| CLAP and VST3 SDKs | `CLOCKWORK_PLUGINS=OFF` | fetched at configure time; no distro packages them |
| smoothie (`smoothie/`) | — | clockwork's own fork of four JUCE 7.0.12 modules, ISC; there is no packaged JUCE it could build against (JUCE 8 is AGPL and not taken). See `smoothie/README.md` |
| oscpack (`src/vendor/oscpack`) | — | release 1.1.0 plus three fixes the packaged 1.1.0 lacks (64-bit detection on aarch64, malformed messages refused). See `src/vendor/oscpack/CLOCKWORK-CHANGES.md` |
| midir (`external/midir`) | — | a fork: shared ALSA client, timestamped CoreMIDI send. See `external/midir/CLOCKWORK-CHANGES.md` |
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

A host links the comms client library and chooses one of `--tcp`, `--uds`,
`--pipe`, `--shm-commands` in SuperSonic's host. See
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

Two things matter to a Rust host and to nothing else. `clockwork-taps`'s
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
`region`, `metrics` (by name, from the schema), `clock` and `tap` on the
client it owns, every handle closing itself. It stands on `clockwork-abi`,
which the layout probe holds to the headers, and carries the crate's only
`unsafe`, each block with its reason. Its tests are a Rust host of exactly
this shape — the dummy DSP attached headless through `clockwork-sys`, with
`clockwork-native`'s `host` feature for the subsystems — so `cargo test`
in `rust/` builds the engine's CMake tree once and exercises the whole
boundary against it: pings answered, rings filled and drained, taps
watching without taking, a tap slot claimed and read back. A build
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

A second checkout of clockwork on the same box (every product carries one as
a submodule) need not install a second copy: the build also looks in
`$CLOCKWORK_TOOLS/bin`, `~/.cargo/bin` and on the path, and takes a
`$WASM_BINDGEN` path outright — only a copy whose version matches the lock is
used. And the two subsystems are optional: without a wasm-bindgen, or with
`CLOCKWORK_WEB_SUBSYSTEMS=0`, the build stubs their glue so the client still
bundles, and a page that asks for `midi: true` is told at init that the
subsystem was not built (it boots without it, as below).

### A web page

**Two headers.** The SAB transport needs the page cross-origin isolated, so
serve it with

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

and serve anything it loads from another origin with
`Cross-Origin-Resource-Policy: cross-origin`, or with CORS.
`scripts/serve.mjs` sends all three.

**A secure context.** The AudioWorklet exists only over https, or over plain
http on `localhost` or `127.0.0.1`. Plain http from any other address boots
nothing.

**Without isolation** the client runs in postMessage mode, and the page has
no view of the engine's memory: the tap readers (`getTap`, `getTaps`,
`getTapActivations`) answer from copies the worklet posts 60 times a second,
and nothing until the first has come; `region` answers null and `regions`
an empty list; audio capture is off, and `wasmMemory` is null. Metrics and the parameter
table arrive in snapshots the worklet posts every `snapshotIntervalMs`
(150 ms), the clock's time every few blocks, and `writeAssets` copies across
rather than writing in place. With no `mode` given, the client picks SAB on an isolated
page and postMessage elsewhere.

**A press starts the audio.** A browser lets an AudioContext start only
inside a user gesture. `init()` starts its context synchronously, before it
awaits anything, so call it from the press. A browser that will not start it
there (iOS, a page's autoplay rules) leaves it suspended: `init()` waits a
second for it, then resolves with the context still suspended
(`clockwork.audioContext.state`). Call `resume()` from the next press;
`audiocontext:resumed` says when the audio runs.

What a page passes to `new Clockwork({...})`:

| option | |
|---|---|
| `baseURL` | where the build is: `clockwork.js`, `workers/` and `wasm/` under it. Or each part: `coreBaseURL` (the worklet and `wasm/`), `workerBaseURL`, `wasmBaseURL` |
| `wasmUrl` | the engine module. Pass it: the default is `wasmBaseURL + "clockwork-engine.wasm"`, and the build writes `clockwork.wasm` (or `<CLOCKWORK_WEB_MODULE_NAME>.wasm`) |
| `workletUrl` | the worklet; default `<coreBaseURL>workers/clockwork_audio_worklet.js` |
| `mode` | `"sab"` or `"postMessage"`; default SAB on an isolated page. `"copies"`, with `copies: { fetch }`, is for a page inside a host that runs the engine itself and hands the page copies of its memory: nothing boots in the page |
| `audio` | `{ outputChannels, inputChannels }`; default 2 and 0 |
| `audioContextOptions` | for `new AudioContext`; default `{ latencyHint: "interactive", sampleRate: 48000 }` |
| `audioContext` | a context of the page's own, used in place of one the client makes |
| `memory` | sizes over `js/memory_layout.js`. `memArenaSize` (32 MB) is the engine's memory, which holds the guest's two spans: a guest that declares more needs more |
| `midi`, `gamepad` | the main-thread subsystems (below); off by default |
| `guestMetrics`, `guestMetricsPanels` | the guest's metrics ([BOUNDARY.md](BOUNDARY.md)) |
| `schedulerLateLimitMs` | drop timed events later than this; default 0, which plays them all |
| `pageLifecycle` | `{ pagehide: "shutdown" \| "none", hidden: "keep" \| "suspend" }`; default `shutdown` and `keep` |

A product whose guest takes a config block subclasses `Clockwork` and
overrides `encodeGuestConfig(ctx)` to return the bytes.

A page that boots this tree's build, with the dummy guest, from the
directory that holds `dist/`, and gets its audio back when the browser takes
it away (below):

```html
<!doctype html>
<meta charset="utf-8">
<button id="start">Start</button>
<p id="paused" hidden>Audio paused. <button id="resume">Tap to resume</button></p>
<script type="module">
  import { Clockwork } from "/dist/clockwork.js";

  const clockwork = new Clockwork({
    baseURL: "/dist/",
    wasmUrl: "/dist/wasm/clockwork.wasm",
  });
  const paused = document.querySelector("#paused");
  clockwork.on("audiocontext:suspended", () => { paused.hidden = false; });
  clockwork.on("audiocontext:interrupted", () => { paused.hidden = false; });

  document.querySelector("#start").addEventListener("click", async () => {
    // From the press: the browser starts audio only inside one.
    await clockwork.init();
    clockwork.send("/dummy/tone");
  });
  document.querySelector("#resume").addEventListener("click", async () => {
    // From the press too: recover() makes what it may need before it awaits.
    if (await clockwork.recover()) paused.hidden = true;
  });
</script>
```

### Keeping the audio

On a phone the browser takes the audio away from a running page often: the
screen locks, another app or a call takes it, the tab goes to the background,
or memory runs short and the worklet is killed. Only a press may start audio
again, so a page needs a way back. A page without one goes silent and stays
silent, and nothing reports it.

- **`audiocontext:suspended`** or **`audiocontext:interrupted`**: the audio
  has stopped. Show something to press.
- **On that press, call `recover()`**, before awaiting anything. It resumes
  the context when the worklet still answers. Otherwise it reloads the engine
  onto a new context it makes inside the press, because iOS, after an
  interruption, can hand back a context that says it is running and renders
  nothing. It resolves `true` when the audio runs.
- **`audiocontext:resumed`**, **`resumed`**, or **`audiocontext:statechange`**
  with `state: "running"`: the audio is back, so take the prompt away. While a
  `recover()` is under way, leave the prompt until it resolves: the context can
  report running before anything renders.
- **`reload:complete` `{ success, resumed }`**: with `resumed` the instance
  carried on and nothing was lost. Otherwise a new instance was built: a
  subclass's `restoreClientState()` ran first to put back what the product
  holds, then `setup` was emitted. The parameter table can be back at its
  defaults (its generation moved), so a client that set values sets them
  again. With `success: false` nothing could be built: offer to load the page
  again.

To try a page's way back: `clockwork.audioContext.suspend()` stops the audio
as the browser does, and `clockwork.reload()` rebuilds the engine as a killed
worklet does.

### Serving it

`scripts/serve.mjs` serves a directory with the headers above, for any
product's page:

```sh
node scripts/serve.mjs                                        # this directory, http://127.0.0.1:8000/
node scripts/serve.mjs --root <dir> --host 0.0.0.0 --https    # another directory, to the LAN over TLS
```

`--root` is the directory the page and its `dist/` are in; the default is the
current one. `--https` makes a self-signed certificate once (in
`~/.clockwork/cert`); accept it once in each browser. `sync()` needs nothing
declared: the barrier is clockwork's own verb, answered for every guest.

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
new Clockwork({ baseURL: "/dist/", wasmUrl: "/dist/wasm/clockwork.wasm", midi: true, gamepad: true })
```

and then speaks the same verbs a native client does: `/clockwork/midi/ports/list`,
`/clockwork/midi/out/enable`, `/clockwork/midi/out/note_on`, `/clockwork/schedule`
around any of them, `/clockwork/gamepad/notify/subscribe`. How a verb gets from
the worklet back to the main thread is [SURFACE.md](SURFACE.md#whose-thread).
Without the option, a MIDI or gamepad verb is refused by name with a reason that
says which option to set. With it, a subsystem the platform cannot provide (a
headless Linux Chromium has no Web MIDI backend) is absent rather than fatal:
the client boots, `midi` / `gamepad` is null, and `midiError` / `gamepadError`
says why. A guest's own sends — a self-directed engine's sink
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
