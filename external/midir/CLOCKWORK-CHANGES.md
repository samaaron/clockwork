<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->

# midir in clockwork

A fork of [midir](https://github.com/Boddlnagg/midir) 0.11.0 (Patrick Reisert,
MIT), consumed by `rust/clockwork-midi` as a cargo path dependency. It is a
fork, not a pinned copy: the crates.io release cannot replace it without
losing the following, which is why a distro build vendors it beside the
other crates rather than taking `librust-midir-dev`.

- **A shared ALSA sequencer client** (`src/backend/alsa/shared.rs`). Stock
  midir opens one ALSA client, and one timestamping queue per input, per
  port. ALSA caps both system-wide, so an application that opens every port
  exhausts them and later `snd_seq_open` calls from other programs fail.
  `SharedInput` and `SharedOutput` host many ports in one client, as RtMidi
  does, growing only ports as devices are opened.
- **Timestamped CoreMIDI send** (feature `coremidi_send_timestamped`): an
  output message carries a host-time deadline into `MIDISend`, so the
  driver schedules it rather than the caller busy-waiting.
- **`avoid_timestamping`** (feature): input without the ALSA queue, for a
  host that stamps arrivals itself.
- Web MIDI changes for the browser build of the subsystem, and the smaller
  fixes visible in `git diff` against the 0.11.0 crate.

The crate keeps its upstream name, version and licence, and the
`[workspace]` table at the top of `Cargo.toml` is clockwork's (the browser
example is a member).
