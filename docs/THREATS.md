<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Threat model

What clockwork trusts, where untrusted bytes come in, what stands in their way,
and what is still open. [SECURITY.md](../SECURITY.md) says how to report a
vulnerability. This file is what a report is measured against, and what a
product shipping clockwork needs for its own risk assessment under the EU
Cyber Resilience Act.

## Who is trusted

| Party | Trust | Why |
|---|---|---|
| The embedding host (SuperSonic's main, a GUI, the NIF's BEAM) | full | it is the process |
| A guest DSP (`dsp_api.h`) | full | native code in the process, on the audio thread |
| A CLAP or VST3 plugin the user loads | full | loading a plugin runs it; it runs in the plugin-bridge process, so a crash takes the bridge down and not the engine |
| A client on the command transport | **control of the engine** | it can schedule, load, route and stop anything; it cannot be given less, so the transport must only admit clients the user means |
| A peer on the shared-memory plane | control of the engine | a trusted co-located process, handed the segment by the attach endpoint |
| An Ableton Link peer on the LAN | **none** | anyone on the network segment |
| A sample file | **none** | it comes from users, the web, other people's projects |
| A MIDI or gamepad device | **none** | hardware or a virtual port another program opened |
| A page or script on the web build's origin | as the host | the worklet and the arena share the page |

So clockwork's defences are about the parties marked *none*, and about not
letting a *control* client turn that control into memory corruption or code
execution. A client that sends `/clockwork/...` to stop the audio is using
the engine, not attacking it.

## Where untrusted bytes come in

| Surface | Code | What defends it | Fuzzed by |
|---|---|---|---|
| OSC datagrams and streams from clients | `rust/clockwork-osc` (`decode_packet`), `rust/clockwork-comms` | bounds-checked decode; stream framing capped by `-B`; `--max-connections` admission cap | `osc_packet`, corpus `rust/clockwork-osc/corpus` |
| OSC as the engine reads it | `src/vendor/oscpack` (`osc::ReceivedPacket`), `src/host/osc_reader.h` | oscpack 1.1.0 with the malformed-message throw restored and a bundle element's size checked before the pointer moves (`CLOCKWORK-CHANGES.md`); the host reader is bounds-checked and allocation-free | `fuzz_osc_received`, `fuzz_osc_reader`, corpus `test/fuzz/corpus` |
| `/clockwork/schedule` | `rust/clockwork-schedule` (`parse`) | never reads past `size`; a failed parse is wholly inert | `schedule_parse`, corpus `rust/clockwork-schedule/corpus` |
| Sample files | `rust/clockwork-audio-decode` (symphonia, pure Rust) behind `src/clockwork_audio_file.cpp` | bounds-checked decoding; an uncompressed file's length is bounded by its bytes, a compressed one's by the decode limit (`clockwork_audio_set_decode_limit`, 1 GiB by default); decoders run on the client side, never on the audio thread | `audio_decode`, corpus `rust/clockwork-audio-decode/corpus`; and every sample Sonic Pi ships against ffmpeg (`scripts/check-samples.sh`) |
| Link peers | Ableton Link 4.1 (fetched, `CLOCKWORK_LINK`, off by default) and four patches in `external/` | upstream's own parsing; LAN only | not yet |
| MIDI input | `external/midir` (fork of 0.11.0), `rust/clockwork-midi` | safe Rust above the platform APIs | not yet |
| Local IPC | `--uds`, `--uds-dgram` (mode 0600), `--pipe` (owner-only DACL, no remote clients), the attach endpoint | the operating system's access control; see [TRANSPORTS.md](TRANSPORTS.md) | n/a |
| Plugin bridge verbs | `src/plugin_bridge_verbs.cpp` | oscpack typed reads, refused on a wrong type | via `fuzz_osc_received` |

Every target is replayed over its corpus, the seeds and every crasher ever
found, on every push, and runs under libFuzzer for two hours every night
(`.github/workflows/security.yml`); a crasher is sealed to the maintainer's
key before it leaves the runner. Every parser above that is written in C or
C++ also runs under ASan and UBSan in CI (`sanitize` in
`.github/workflows/ci.yml`), and the threaded code runs under TSan.

## Known gaps

These are the open items, most important first. Each one is either a change to
make or a decision to write down.

1. **No transport authenticates on the LAN.** UDP and TCP accept any packet
   that reaches the port, and a client has control of the engine. That is
   scsynth's model and the reason the local transports exist. Every network
   transport therefore binds loopback unless given an address, and a host
   that binds every interface asked for it by name and gets one log line
   saying so (see [TRANSPORTS.md](TRANSPORTS.md)). What is still missing is a
   way to authenticate a client that is meant to reach the engine over a
   network: a connection token for TCP is the planned shape, UDP cannot be
   authenticated without a handshake, so a LAN UDP bind stays an explicit
   opt-in.
2. **Link and MIDI input are not fuzzed.** Link's discovery packets come from
   anyone on the LAN.
3. **The decoders run in the client's process.** They are Rust now, bounded
   in what they read and what they hand back, and fuzzed nightly; a fault in
   them is a refused file, not a corruption. What they are not is isolated: a
   separate process or a sandbox would keep even a decoder bug out of the
   host, and is the step after this one for a product that loads strangers'
   files.
4. **A crash in the NIF takes the whole BEAM down with it.** This is inherent
   to a NIF, so an Erlang host that wants isolation should run the engine as a
   separate OS process.

## Supply chain

Everything clockwork contains that it did not write is listed in
[components.cdx.json](components.cdx.json): the vendored C and C++ (with the
upstream version, its licence and what clockwork changed), the libraries
CMake fetches, and the fork of midir. The Rust crates and npm packages are
not listed there, because their lockfiles already say exactly what they are.

`scripts/sbom.mjs` keeps that list honest and builds the SBOMs:

- `node scripts/sbom.mjs check` fails when a directory under `src/vendor`,
  `src/native/vendor` or `external`, or a `FetchContent_Declare` in a
  `CMakeLists.txt`, has no entry, when an entry's version is not what its
  source or its fetched tag says, or when an entry does not say which targets
  and CMake options compile it in.
- `node scripts/sbom.mjs target all` writes one CycloneDX document per CI
  target (linux-x64, linux-arm64, linux-x86, macos-arm64, macos-x64,
  windows-x64, web): the crates cargo resolves for that triple with the
  default build's features, the web client's runtime npm packages, and the
  vendored entries that target's default options compile in. A build of
  clockwork on one platform contains a fifth of the crates the lockfile names
  for every platform, so the SBOM is per target, which is per product. The
  documents are reproducible from a commit, so a project that pins clockwork
  regenerates them for its pin and references them from its own SBOM.
- `node scripts/sbom.mjs scan build/sbom/*.cdx.json` looks every component
  of those documents up and fails on any advisory. Crates and npm packages
  are looked up in OSV by purl. Vendored code is looked up in OSV by the
  upstream commit it was taken from (`clockwork:upstream_commit` in the list;
  for a fork such as Smoothie, oscpack or midir, the commit it forked from),
  which OSV matches against the git ranges in CVE records. Code with no
  public repository (the ASIO SDK) is looked up by a keyword search of the
  NVD. A component the scan cannot look up fails the scan: the vendored code
  is exactly what a lockfile scanner never sees, so nothing is skipped. A
  fork's own changes are first-party code that no database covers; they are
  the job of the tests, the fuzz targets and CodeQL. An advisory that does
  not apply is accepted only by a statement in
  [vex.cdx.json](vex.cdx.json), one per advisory and component, saying why
  (OSV matches the whole stb repository at the vendored commit, so advisories
  against stb_image.h match a tree that carries only stb_vorbis.c).

`.github/workflows/security.yml` runs all three on every push (the
documents are the run's artifacts) and proves the scan looks by running it
over `test/fixtures/sbom-canary.cdx.json`, which must fail. It runs them
again every day, so that a new advisory against an unchanged dependency still
turns CI red, and a failed scheduled run is mailed to the maintainer.
