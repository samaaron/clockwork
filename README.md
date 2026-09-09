<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# clockwork
```
  ▄████▄ ██      ▄█████▄  ▄████▄ ██  ▄█▀ ██     ██ ▄█████▄ ██████▄ ██  ▄█▀
 ██   ▀▀ ██      ██   ██ ██   ▀▀ ██▄█▀   ██     ██ ██   ██ ██   ██ ██▄█▀
 ██      ██      ██   ██ ██      ███▄    ██ ▄█▄ ██ ██   ██ ██████▀ ███▄
 ██   ▄▄ ██      ██   ██ ██   ▄▄ ██ ▀█▄  ███▀ ▀███ ██   ██ ██ ▀█▄  ██ ▀█▄
  ▀████▀ ███████ ▀█████▀  ▀████▀ ██   ██ ██     ██ ▀█████▀ ██   ██ ██   ██

```
Clockwork-like infrastructure for hosting realtime DSP engines.

Run your own DSP engine in a browser, natively, on embedded hardware, or in the
BEAM as a NIF.

Clockwork powers-up your DSP engine with dynamic audio device handling, a session
clock with support for Ableton Link, an OSC-based API, and four main forms of
low-latency IO lanes connecting your DSP with the outside world:

* Events - MIDI, OSC and gamepad. Play notes from a keyboard, move params from
  a control surface, use a gamepad as an instrument, drive the engine from
  another app over the network. Schedule any of it ahead of time.

* Binary blobs - audio samples and the like. Load a sample library, a wavetable
  or an impulse response without it touching the audio thread.

* Audio frames - multichannel, and Link audio. Stream a long file off disk into
  the engine, record its output back out, take a live stream from an Ableton
  Link peer.

* One-way shared memory regions - metrics and scopes. Draw a waveform or a
  spectrum, watch CPU load, dropouts and clock drift while the engine runs.

Your DSP engine gains access to all of this by plugging into a small C ABI: [`src/dsp_api.h`](src/dsp_api.h),
described in [docs/BOUNDARY.md](docs/BOUNDARY.md).

## Building

```sh
cmake -B build .
cmake --build build --parallel 2
```

See [docs/BUILDING.md](docs/BUILDING.md).

## AI

This project was developed with Claude Code.

## Licensing

AGPL-3.0-or-later, or a commercial licence - see [LICENSE](LICENSE).
