<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->

# oscpack in clockwork

The `osc/` half of oscpack **release 1.1.0** (Ross Bencina, MIT), with three
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

Everything else is 1.1.0 byte for byte. The `ip/` half (sockets, the
listener) is not carried at all: clockwork's transports are its own
(`rust/clockwork-comms`).
