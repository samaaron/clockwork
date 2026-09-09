// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Outbound OSC: one pair of sockets, and the family choice they make.
//!
//! Extracted from `ClockworkOsc` when the event sinks arrived (`src/clockwork_event_sink.h`)
//! and needed to send to a `host:port` without owning the cue server. There is
//! exactly one interesting decision in it and it should not be made twice: a
//! hostname can resolve to several addresses in either family, so a sender
//! binds one socket per family and uses whichever the first usable resolved
//! address wants. Written a second time, the second copy is the one that
//! quietly only does IPv4.

use std::net::{Ipv6Addr, ToSocketAddrs, UdpSocket};

/// The largest UDP payload there is, and the send buffer the sockets ask
/// for. macOS refuses a datagram wider than the socket's send buffer with
/// EMSGSIZE, and its default is 9216 bytes (`net.inet.udp.maxdgram`), so
/// without this a 10 KB OSC message would be accepted by the sink and lost
/// at the socket. Linux and Windows honour a datagram up to the protocol
/// limit regardless; the setting is harmless there.
pub const MAX_DATAGRAM_BYTES: usize = 65507;

fn widen(sock: UdpSocket) -> UdpSocket {
    // Best effort: a kernel that refuses the size leaves its default, and the
    // limit then shows up as a delivery failure the sink counts.
    let _ = socket2::SockRef::from(&sock).set_send_buffer_size(MAX_DATAGRAM_BYTES + 4096);
    sock
}

/// Outbound sockets with ephemeral source ports, one per family, bound once.
///
/// Separate from any receive socket on purpose: outbound works regardless of
/// whether anything is listening, and a socket never sends to its own bound
/// port.
pub struct OscSender {
    send4: Option<UdpSocket>,
    send6: Option<UdpSocket>,
}

impl OscSender {
    /// Bind both families. Succeeds if EITHER binds — a v6-only-down host
    /// still reaches IPv4, and a v4-only-down host still reaches IPv6.
    /// `None` only if neither would bind at all.
    pub fn new() -> Option<OscSender> {
        let send4 = UdpSocket::bind(("0.0.0.0", 0)).ok().map(widen);
        let send6 = UdpSocket::bind((Ipv6Addr::UNSPECIFIED, 0)).ok().map(widen);
        if send4.is_none() && send6.is_none() {
            return None;
        }
        Some(OscSender { send4, send6 })
    }

    /// Send one datagram. Returns whether it reached a socket — not whether it
    /// arrived, which UDP cannot tell anyone. A name that does not resolve, or
    /// a family with no socket, is a false.
    pub fn send(&self, host: &str, port: u16, data: &[u8]) -> bool {
        let resolved = match (host, port).to_socket_addrs() {
            Ok(it) => it,
            Err(_) => return false,
        };
        // The first resolved address whose family socket exists — so a
        // dual-stack host works, and a v6-only-down host still reaches IPv4.
        for addr in resolved {
            let sock = if addr.is_ipv4() { self.send4.as_ref() } else { self.send6.as_ref() };
            if let Some(s) = sock {
                return s.send_to(data, addr).is_ok();
            }
        }
        false
    }

    /// Does `host:port` resolve to something this sender could reach? Used at
    /// open time by a caller that would rather refuse a destination than
    /// silently send into nothing later.
    pub fn resolves(host: &str, port: u16) -> bool {
        (host, port).to_socket_addrs().map(|mut i| i.next().is_some()).unwrap_or(false)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn a_whole_datagram_leaves_the_socket() {
        // 60 000 bytes: above macOS's 9216-byte default per-socket ceiling,
        // below the protocol's. Without the widened send buffer this send
        // returns false on macOS and the sink would count a drop.
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let port = listener.local_addr().unwrap().port();
        let sender = OscSender::new().expect("a socket binds");
        let body = vec![0xABu8; 60_000];
        assert!(sender.send("127.0.0.1", port, &body), "the socket must take it whole");
        let mut buf = vec![0u8; MAX_DATAGRAM_BYTES];
        let (n, _) = listener.recv_from(&mut buf).expect("it arrives");
        assert_eq!(n, 60_000);
        assert!(buf[..n].iter().all(|&b| b == 0xAB));
    }
}
