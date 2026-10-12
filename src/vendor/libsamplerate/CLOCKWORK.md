<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->

# libsamplerate in clockwork

libsamplerate **0.2.2** (Erik de Castro Lopo, BSD-2-Clause, `COPYING`), from
<https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz>,
sha256 `3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893`
(the checksum Homebrew's formula records for the same tarball).

The Link Audio input renderer (`src/native/vendor/LinkAudioInputRenderer.hpp`)
reads a peer's stream between its samples through its medium-quality sinc
converter: at another sample rate, and bent by up to a hundredth while it is
steered. The 4-point cubic it replaced put a stream's high frequencies back
out as tones of their own, 22 dB under a 12 kHz sine: faint inharmonic bells
over every hi-hat. Through this converter nothing else in the output is louder
than 100 dB under the sine.

The files here are the release's, byte for byte, where the release has them:
`include/samplerate.h`; `src/common.h`, `src/samplerate.c`, `src/src_sinc.c`,
`src/src_linear.c`, `src/src_zoh.c` and `src/mid_qual_coeffs.h`. Only
`include/` is on clockwork's include path, so `common.h` is not on an
embedder's. Left out: the fastest and
best sinc tables (`fastest_coeffs.h`, and `high_qual_coeffs.h`, 8.8 MB of
source), the build files, the tests and the docs. It is compiled with
`ENABLE_SINC_MEDIUM_CONVERTER` and `HAVE_STDBOOL_H` defined, `PACKAGE` and
`VERSION` for `src_get_version`, and without `HAVE_CONFIG_H`, so the sources
read no `config.h`.

Nothing allocates once a converter is made (`src_new`): reading from it
(`src_process`, `src_set_ratio`, `src_reset`) does not, which the renderer
relies on. It uses `src_process` rather than the callback API: given a stream
as it arrives, a callback with nothing more to hand over is taken as the
stream's end, and `src_callback_read` asks for more at the start of every
read whether it needs it or not.

A distribution can link its own package instead: `CLOCKWORK_SYSTEM_SAMPLERATE`
takes `samplerate` from pkg-config (Debian's `libsamplerate0-dev`) and this
copy is not compiled. The renderer works out how far ahead the converter reads
from this release's medium table, and its tests run against whichever library
is linked: one whose table differs is caught there.
