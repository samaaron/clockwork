<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->

# oscpack in clockwork

The `osc/` half of oscpack **release 1.1.0** (Ross Bencina, MIT), with five
changes. They are why clockwork carries this copy rather than linking a
packaged oscpack (Debian's `liboscpack-dev` is 1.1.0 unmodified), and a
packager wanting to unvendor it should know each one:

1. **64-bit detection on every 64-bit target.** Upstream sizes `int64`
   through `#if defined(__x86_64__) || defined(_M_X64)`; on aarch64 and
   other LP64 targets it therefore takes the 32-bit branch and mis-sizes
   the type. `OscTypes.h`, `OscOutboundPacketStream.h` and
   `OscReceivedElements.h` add `__aarch64__`, `_M_ARM64` and `__LP64__`.
   Not in upstream master as of 2026.

2. **Malformed messages are actually refused.** In 1.1.0,
   `ReceivedMessage::Init` constructs `MalformedMessageException("arguments
   exceed message size")` without throwing it, so an argument list that runs
   past the packet is accepted. `OscReceivedElements.cpp` throws. Upstream
   master has the same fix; 1.1.0 does not.

3. **`argument_` → `argumentPtr_`** in the argument accessors of
   `OscReceivedElements.cpp`, a rename that goes with the previous change.

4. **A bundle element's size is checked against what is left before the
   pointer advances.** `ReceivedBundle::Init` in 1.1.0 computes
   `p += 4 + elementSize` in unsigned 32-bit arithmetic and only then checks
   `p > end_`; an element size near 2^32 wraps the sum to a small number, the
   pointer does not move, and the loop never ends. Found by
   `test/fuzz/fuzz_osc_received` (2026-10-26); pinned by `test/test_oscpack.cpp`.
   Not in upstream master as of 2026.

5. **`int64` on 64-bit Windows under GCC (MinGW).** Upstream's
   `defined(__x86_64__)` branch makes `int64` a `long`, which is 32 bits on
   Windows whatever the compiler: MSVC takes its own `__int64` branch first,
   GCC does not, and `OscReceivedElements.h` then fails its
   `sizeof(osc::int64) == 8` assertion at run time (Sonic Pi's runtime host,
   built by MSYS2 on Windows, 2026-11-12). `OscTypes.h` leaves Windows out of
   that branch, so it takes `long long`, and checks every type's size with
   `static_assert`, so a mis-sized type fails the build on any target;
   `scripts/check-oscpack-types.sh` compiles the header for each target the
   engine builds for. Not in upstream master as of 2026.

Everything else is 1.1.0 byte for byte. The `ip/` half (sockets, the
listener) is not carried at all: clockwork's transports are its own
(`rust/clockwork-comms`).
