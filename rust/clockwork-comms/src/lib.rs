// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! clockwork's command transports — a CLIENT library.
//!
//! An engine has no sockets. Whoever embeds one decides what, if anything,
//! listens on the wire, and opens it from here: the length-prefix-framed
//! stream servers (TCP and Unix socket), the Windows named-pipe analogue
//! behind the same stream ABI, the owner-only Unix datagram socket, and the
//! peer side of the shared-memory command plane. Each delivers a received
//! packet to the host, which writes it through the client boundary; each is
//! handed replies by the host, resolved by origin token. The C ABI the C++
//! transports link against is in `src/comms/clockwork_comms_abi.h`.
//!
//! This crate depends on `clockwork-osc-net` only for what the engine itself
//! also uses — the outbound UDP sender and a receive-error predicate. The
//! engine never depends on this crate: it is included by the native umbrella
//! under the `comms` feature so a host that links the comms library finds the
//! symbols, and left out by one that does not.
#![cfg(not(target_arch = "wasm32"))]

pub mod pipe;
pub mod stream;
pub mod uds;
// Cross-process SHM peer client (the peer side of the SHM command plane) — used
// by the transport harness to drive --shm-commands end-to-end. Not a C ABI.
pub mod shm;
