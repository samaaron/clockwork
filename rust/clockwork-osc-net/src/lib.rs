// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! clockwork's OSC networking subsystem (native-only).
//!
//! Owns the external-facing OSC sockets — the cue server (inbound external OSC,
//! re-framed to `/external-osc-cue` and emitted to the engine) and the outbound
//! user-OSC sender (`osc_send` / scheduled `osc`). Uses `std::net`, which
//! resolves a hostname in the socket's own
//! address family, so `localhost` reaches the cue server on whichever family it
//! resolves to. The C ABI the engine links against is in `cpp/clockwork_osc.h`; the
//! engine integration + Ruby-facing transport stay in C++.

#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;

// Outbound UDP, on its own so more than one subsystem can send: the cue
// server's C ABI (`clockwork_osc_send`) and the event sinks (`src/clockwork_event_sink.h`)
// both use it.
#[cfg(not(target_arch = "wasm32"))]
pub mod out;

// The command transports — UDS datagram, the TCP/UDS stream servers, the
// Windows named pipe, and the SHM peer client — are NOT here any more. They
// are a client's concern (an engine has no sockets; whoever embeds it opens
// them) and live in the clockwork-comms crate, which this crate knows nothing
// about. What stays is what the ENGINE itself needs a socket for: the cue
// server and outbound OSC.

/// Does this receive error leave the socket usable, so the loop should poll
/// again rather than tear the connection down?
///
/// `WouldBlock`/`TimedOut` are the read-timeout tick every receive loop polls
/// on. `Interrupted` is EINTR: std does not retry `read`/`recv_from` for us
/// (unlike `write_all`/`read_exact`, which do), so it surfaces here on any
/// signal delivered to that thread — a live client, not a dead one.
#[cfg(not(target_arch = "wasm32"))]
pub fn recv_retryable(e: &std::io::Error) -> bool {
    use std::io::ErrorKind;
    matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted)
}
