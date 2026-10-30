<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Threat model

Where untrusted bytes enter and leave clockwork, what checks them, and what is
not covered yet. [SECURITY.md](../SECURITY.md) says how to report a
vulnerability; this document is the reference for triaging a report and the
input to a product's own risk assessment under the EU Cyber Resilience Act.

## Who is trusted

| Party | Trust | Why |
|---|---|---|
| The embedding host (SuperSonic's main, a GUI, the NIF's BEAM) | full | it is the process |
| A guest DSP (`dsp_api.h`) | full | native code in the process, on the audio thread |
| A CLAP or VST3 plugin the user loads | full | loading a plugin runs it; it runs in the plugin-bridge process, so a crash takes the bridge down and not the engine |
| A client on the command transport | **control of the engine** | it can schedule, load, route and stop anything. Every command port therefore admits only a client whose first frame is the host's per-boot secret; the host generates the secret and passes it to each client it admits (stdin, a file or a pipe — the host's choice) |
| A peer on the shared-memory plane | control of the engine | a trusted co-located process, handed the segment by the attach endpoint |
| An Ableton Link peer on the LAN | **none** | anyone on the network segment |
| A sample file | **none** | any file a user or client names |
| A MIDI or gamepad device | **none** | hardware or a virtual port another program opened |
| A page or script on the web build's origin | as the host | the worklet and the arena share the page |

The defences below are for the parties marked *none*. A *control* client is
trusted to drive the engine (stopping the audio with a `/clockwork/...` command
is use, not attack) but must not be able to corrupt memory or run code through
a malformed message.

## Every seam, in and out

Every point where bytes cross the process boundary. For each: who can reach
it, which code parses (in) or encodes (out) the bytes, what checks exist, and
which tests or fuzz targets exercise them. Risk levels: *high* — reachable by
an unauthenticated party on the network; *medium* — reachable by any local
process or device; *control* — reachable only by an authenticated client,
which already controls the engine; *low* — reachable only by the user's own
machine or an already-trusted process.

| Seam | Direction | Reached by | Code | Language | What defends it | Proved by | Risk |
|---|---|---|---|---|---|---|---|
| OSC commands | in | a client, over TCP, a Unix stream socket, a named pipe, a shared-memory ring or postMessage | `rust/clockwork-osc` (`decode_packet`), `rust/clockwork-comms`, `src/vendor/oscpack`, `src/host/osc_reader.h` | safe Rust parser (`clockwork-osc` forbids unsafe); C++ at the engine's oscpack reader and `osc_reader.h` | bounds-checked decode; the per-boot secret required on every stream transport, one client at a time; frame size capped by `-B`; binds loopback unless an address is given; a bundle element's size is checked before it is read; a refusal echoes at most `len` bytes of the address | `osc_packet`, `fuzz_osc_received`, `fuzz_osc_reader`; `fuzz_clockwork_sys`: the audio thread's verbs and the refusal with arbitrary frames, under ASan, every reply decoded; `test_ingest_overload.cpp` for a flood | control; high if bound to a non-loopback address |
| OSC command framing | in | the same clients | TCP length prefix and reassembly in `rust/clockwork-comms/src/stream.rs`; ring record headers in `src/ring/` | unsafe Rust at the sockets (`clockwork-comms`); C++ ring headers | the first frame must be the secret's auth packet, compared in constant time, within 2 s; a frame past `-B` or of zero length closes the connection; a ring header is validated by the drain | ring unit tests and `test_ring_exhaustive.cpp`; `fuzz_ring_drain`: the drain over a ring and cursors a peer wrote, under ASan, checked to read nothing outside the ring and deliver only frames inside it; `stream_frames`: the secret gate and the framing given the same bytes in one read, in reads cut anywhere, and a byte at a time, checked against a reference walk of the same bytes; the stream tests in `stream.rs` and `test_transports.cpp` refuse a missing, wrong or late secret | control |
| `/clockwork/schedule` | in | a client | `rust/clockwork-schedule` (`parse`) | Rust; the store behind the parser carries unsafe | never reads past `size`; a failed parse changes nothing | `schedule_parse` | control |
| OSC cues | in | anyone who can reach the cue port: loopback by default, the LAN when a client opens it | the cue server in `rust/clockwork-osc-net` (`decode_packet`, then re-framed as `/external-osc-cue` and broadcast) | safe Rust parser; unsafe Rust only at the C ABI (`ffi.rs`) | 64 KiB datagram; the parser's nesting cap; a panic drops one datagram; a rate limit (`rust/clockwork-osc-net/src/limit.rs`: token buckets for the port and for each of 64 sender addresses, 2000/s and 1000/s by default, `/clockwork/osc/cue-server/limit` sets or disables it) drops a datagram over it before decoding and counts every dropped cue in `cuesDropped`; the broadcast ends in counted drops when a ring is full | the parser by `osc_packet`; `cue_reframe`: each message in a datagram, bundles flattened, comes out as one cue carrying the sender, its port, the address and the arguments unchanged; `limit.rs` unit tests with a driven clock (burst, refill, per-sender, the table's bound and eviction, 0 = none); `ffi.rs` and `test_osc.cpp` flood a bound port and check the limit's worth arrives, the rest is counted, and `0 0` lets everything through; `test_osc.cpp` checks a command on the cue port is never run; the broadcast by `test_egress_router.cpp` and the reply check below | medium on a LAN bind: unauthenticated, but data only and rate-limited |
| OSC replies and notifications | out | the requesting client, or every subscribed client | `src/comms/EgressRouter.h`, `rust/clockwork-client` rings, the transports' send paths | C++ router; unsafe Rust rings (`clockwork-client`) | a full egress ring drops the frame and counts it; the audio thread never blocks on a slow client | `test_egress_router.cpp`, `test_client_egress.cpp`; every reply any native case receives is checked against the client parser (`clockwork_osc_decodes`, in `EngineFixture` and `LanesFixture`), so the 60 reply shapes the suite provokes are proved to decode | low |
| `osc_send` | out | any host and port a client names | `rust/clockwork-sinks`, `rust/clockwork-osc-net/src/out.rs` | Rust with unsafe in the queue and registry | the hostname resolves at open or the sink is refused; bounded queue per sink, counted drops | `test_event_sinks.cpp`; `test_osc.cpp` checks a command sent to the cue port comes back as a cue and is never run | control: a client can send OSC anywhere, by design |
| Sample files, read | in | any file a user or client names | `rust/clockwork-audio-decode` (symphonia; its FLAC decoder vendored with a wrapping predictor, `external/symphonia-bundle-flac`) behind `src/clockwork_audio_file.cpp` | safe Rust (symphonia); unsafe only at the C ABI; C++ reads the file | bounds-checked decoding; length bounded by the bytes for an uncompressed file and by the decode limit for a compressed one (1 GiB by default); never on the audio thread; a file is refused past 64 channels | `audio_decode` and its corpus, replayed built as the fuzzer builds; every sample Sonic Pi ships decoded against ffmpeg (`scripts/check-samples.sh`) | high: files come from untrusted sources |
| Sample files, written | out | whatever software reads the file afterwards | `src/clockwork_audio_file.cpp` (WAV), `src/flac_encoder.cpp` | C++ | a channel count or rate the header cannot hold is refused at open; samples beyond full scale are clamped | `fuzz_audio_encode`, checking the written header against what was written | low |
| MIDI | in | any device or virtual port, so any program on the machine | `external/midir` (fork of 0.11.0), `rust/clockwork-midi/src/message.rs`, SysEx reassembly in `ffi.rs` | safe Rust parser; unsafe Rust in the midir fork at the platform APIs | safe Rust above the platform APIs | unit tests on well-formed messages; `midi`: a port's bytes parsed, encoded and parsed back to the same message; every parse is total (no panic path) | medium |
| MIDI | out | a device, as a client's command | `rust/clockwork-midi/src/schema.rs` (`decode_out`), scheduled on the engine clock | safe Rust decoder; midir to the device | typed reads, refused on a wrong type | unit tests; `midi`: a client's `/clockwork/midi/*` command decoded, a decoded send encodes | control |
| Gamepad | in | any controller the machine sees | `external/gilrs` through `rust/clockwork-gamepad/src/device.rs` | Rust (gilrs, unsafe at the platform APIs) | gilrs hands over typed events, no byte parser of ours | `test_gamepad.cpp`; no scripted event-sequence test | low |
| Gamepad | out | a controller's rumble, as a client's command | `rust/clockwork-gamepad/src/schema.rs` (`decode_out`) | safe Rust decoder | typed reads, refused on a wrong type | unit tests; `gamepad`: a client's `/clockwork/gamepad/*` command decoded | control |
| Link session | in | anyone on the network segment, once Link is on | Ableton Link 4.1.1 (fetched under `CLOCKWORK_LINK`, off by default) and five patches in `external/`, one of which bounds-checks the string deserializer (`link-string-bounds.patch`) | C++ (upstream) | upstream's parsing; a string length is checked against the datagram (our patch) | `fuzz_link_wire` over the discovery and measurement sockets, with its regressions | high: reachable by any host on the LAN once Link is on |
| Link session | out | the same peers | upstream's `UdpMessenger` | C++ (upstream) | upstream's encoder, unchanged | upstream's tests | low |
| Link Audio | in | a peer on the session that publishes a channel | upstream's `link_audio/Receivers.hpp` and `SinkProcessor.hpp` into `src/native/vendor/LinkAudioInputRenderer.hpp` | C++ (upstream and our renderer) | upstream's parsing; the renderer reads only frames that have arrived | `test_link_audio_*.cpp` on real peers; `fuzz_link_wire` over the Link Audio socket | high, as above |
| Link Audio | out | not applicable: clockwork receives Link Audio and does not publish it | — | — | — | — | none |
| Local IPC | in | the user's own account | `--uds` (mode 0600), `--pipe` (owner-only DACL, no remote clients), the shared-memory attach endpoint | unsafe Rust at the OS (`clockwork-comms`) | the operating system's access control, and the per-boot secret on every one of them; see [TRANSPORTS.md](TRANSPORTS.md) | `test_server_shm_security.cpp`, `test_shm_attach.cpp`, the stream tests in `stream.rs`; the named pipe by the corpus replay in gap 2 | low |
| Plugin bridge verbs | in | the bridge process | `src/plugin_bridge_verbs.cpp` | C++ | oscpack typed reads, refused on a wrong type | via `fuzz_osc_received` | low |
| Audio devices | in and out | the hardware the user chose | `smoothie/` (JUCE 7.0.12's four ISC modules, pruned to what the drivers use) | C++ and Objective-C++ | the device layer's own buffers; a dead device is detected and recovered | the device suites, hardware cases opt-in; the macOS, Windows and Linux drivers by the native matrix, iOS and Bela compiled on every push (`scripts/check-drivers.sh`); Android is kept but not yet ported to the pruned core | low |

CI replays every fuzz target over its corpus (the seeds and every past
crasher) on each push, and fuzzes each target under libFuzzer for two hours
nightly (`.github/workflows/security.yml`). A new crasher is encrypted to the
maintainer's key before it is uploaded. The C and C++ parsers above also run
under ASan and UBSan, and the threaded code under TSan, in the `sanitize` jobs
of `.github/workflows/ci.yml`.

## Known gaps

Open items, most important first.

1. **The peer plane's ring drain is C++.** `src/lanes/ring_drain.h` reads
   ring headers a peer process wrote; it is fuzzed by `fuzz_ring_drain` under
   ASan. Moving it to Rust would move the engine's own ring reads with it and
   is a separate decision.
2. **The named pipe's read loop has no fuzz run on Windows.** It shares the secret's gate and the framing with the socket servers, which `stream_frames` fuzzes; what is the pipe's own is the Win32 read loop around them (`serve_client` in `rust/clockwork-comms/src/pipe.rs`). The `rust (windows-x64)` job runs `the_corpus_through_a_real_pipe_leaves_the_server_serving`: every file of the comms corpus written to a real pipe by a client, cut at several places, and a client with the secret admitted after each. No fuzz job runs on Windows; the corpus is the stream fuzzer's, replayed.
3. **The sample decoders run in the host's process.** They are safe Rust,
   bounded in what they read and what they hand back, and fuzzed nightly; a
   fault in them is a refused file, not a corruption. They are not isolated: a
   separate process or a sandbox would contain even a decoder bug, and is the
   next step for a product that loads untrusted files.
4. **A crash in the NIF takes the whole BEAM down with it.** This is inherent
   to a NIF, so an Erlang host that wants isolation should run the engine as a
   separate OS process.

## Supply chain

Everything clockwork contains that it did not write is listed in
[components.cdx.json](components.cdx.json): the vendored C and C++ (with the
upstream version, its licence and what clockwork changed), the libraries
CMake fetches, and the fork of midir. The Rust crates and npm packages are
not listed there, because their lockfiles already say exactly what they are.

`scripts/sbom.mjs` checks that list and builds the SBOMs:

- `node scripts/sbom.mjs check` fails when a directory under `src/vendor`,
  `src/native/vendor` or `external`, or a `FetchContent_Declare` in a
  `CMakeLists.txt`, has no entry, when an entry's version is not what its
  source or its fetched tag says, or when an entry does not say which targets
  and CMake options compile it in.
- `node scripts/sbom.mjs target all` writes one CycloneDX document per CI
  target (linux-x64, linux-arm64, linux-x86, macos-arm64, macos-x64,
  windows-x64, web): the crates cargo resolves for that triple with the
  default build's features, the web client's runtime npm packages, and the
  vendored entries that target's default options compile in. One platform's
  build contains about a fifth of the crates in the lockfile, so the SBOM is
  per target. The documents are reproducible from a commit, so a project that
  pins clockwork regenerates them for its pin and references them from its
  own SBOM.
- `node scripts/sbom.mjs scan build/sbom/*.cdx.json` looks every component
  of those documents up and fails on any advisory. Crates and npm packages
  are looked up in OSV by purl. Vendored code is looked up in OSV by the
  upstream commit it was taken from (`clockwork:upstream_commit` in the list;
  for a fork such as Smoothie, oscpack, midir or symphonia-bundle-flac, the commit it forked from),
  which OSV matches against the git ranges in CVE records. Code with no
  public repository (the ASIO SDK) is looked up by a keyword search of the
  NVD. A component the scan cannot look up fails the scan rather than being
  skipped: vendored code is what a lockfile scanner misses. A fork's own
  changes are first-party code that no database covers; the tests, the fuzz
  targets and CodeQL cover them. An advisory that does not apply is accepted
  only by a statement in
  [vex.cdx.json](vex.cdx.json), one per advisory and component, saying why:
  OSV matches a whole repository at a commit, so an advisory against a file
  the tree does not carry is a statement, not a skip.

`.github/workflows/security.yml` runs all three on every push (the documents
are the run's artifacts) and checks the scan itself against
`test/fixtures/sbom-canary.cdx.json`, a document that must fail. It runs them
again daily, so a new advisory against an unchanged dependency fails CI; a
failed scheduled run is mailed to the maintainer.
