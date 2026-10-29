// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Native C ABI for the OSC subsystem — the boundary the C++ engine links against.
//!
//! The engine creates one instance with an emit callback, then drives it:
//! * [`clockwork_osc_configure`] — (re)bind the cue server (port, loopback, on/off).
//! * [`clockwork_osc_send`] — send one OSC packet to an arbitrary host:port
//!   (off the audio thread).
//!
//! Dual-stack: the cue server binds both IPv4 and IPv6 (loopback → 127.0.0.1 +
//! ::1, all-interfaces → 0.0.0.0 + ::); outbound resolves the destination and
//! sends from the matching-family socket. `std::net` resolves a hostname in a
//! sane family order, so `localhost` (→ ::1 and/or 127.0.0.1) reaches the cue
//! server on whichever family it lands — no IPv4/IPv6 mismatch.
//!
//! Inbound external OSC is re-framed as
//! `/external-osc-cue <ip> <port> <address> <args...>` and handed back via the
//! emit callback (which may fire on a cue recv thread, so the host impl must be
//! thread-safe). The subsystem owns its UDP sockets + recv threads and never
//! touches the audio thread.

use std::ffi::c_void;
use std::net::{Ipv6Addr, SocketAddr, SocketAddrV6, UdpSocket};
use std::slice;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use socket2::{Domain, Protocol, Socket, Type};

use clockwork_osc::{decode, decode_packet, encode, OscArg, OscMessage, OscPacket};
// Emit-callback shape + panic fence, shared across the subsystem C ABIs.
pub use clockwork_osc::ffi::{no_unwind, EmitFn, EMIT_BROADCAST};

// Process-wide cap on "dropped malformed inbound OSC" log lines, so a junk
// flood can't spam the log.
static MALFORMED_LOGGED: AtomicU32 = AtomicU32::new(0);

/// The host emit callback + opaque context, made Send/Sync so the cue recv
/// threads can hold a copy.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
}
// SAFETY: `ctx` is an opaque token this crate never dereferences, only hands
// back to `emit`, and the C++ engine's contract is that it outlives the
// instance and the callback is thread-safe; a fn pointer is Send + Sync.
unsafe impl Send for Host {}
// SAFETY: as above.
unsafe impl Sync for Host {}

impl Host {
    fn emit(&self, osc: &[u8]) {
        (self.emit)(self.ctx, EMIT_BROADCAST, osc.as_ptr(), osc.len() as u32);
    }
}

/// A running cue server: one recv thread per bound socket (IPv4 + IPv6) sharing a
/// stop flag. Dropping it stops the threads (≤ the read timeout) and closes the
/// sockets.
struct CueServer {
    stop: Arc<AtomicBool>,
    joins: Vec<JoinHandle<()>>,
}

impl Drop for CueServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        for j in self.joins.drain(..) {
            let _ = j.join();
        }
    }
}

struct CueState {
    server: Option<CueServer>,
    port: u16,
    loopback: bool,
}

/// The opaque handle the C++ side owns.
pub struct ClockworkOsc {
    host: Host,
    // Outbound sockets (ephemeral source ports), one per family, bound once at
    // create. Separate from the cue sockets so outbound works regardless of cue
    // state and a socket never sends to its own bound port. In `out` rather
    // than inline because the event sinks send to a host:port too, and the
    // family choice below is the kind of thing that goes wrong in its second
    // copy.
    out: Option<crate::out::OscSender>,
    cues_on: Arc<AtomicBool>,
    cue: Mutex<CueState>,
}

// A dual-stack-safe IPv6 UDP bind: only_v6(true) so it coexists with the IPv4
// socket on the same port (without it, `::` is dual-stack on Linux and steals
// the v4 port).
fn bind_v6(addr: Ipv6Addr, port: u16) -> std::io::Result<UdpSocket> {
    let sock = Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;
    sock.set_only_v6(true)?;
    let sa = SocketAddr::from(SocketAddrV6::new(addr, port, 0, 0));
    sock.bind(&sa.into())?;
    Ok(sock.into())
}

// Say once per socket where an open bind landed: see `crate::note_bind`.
fn note_binds<'a>(what: &str, socks: impl IntoIterator<Item = &'a UdpSocket>, exposes: &str) {
    for s in socks {
        if let Ok(local) = s.local_addr() {
            crate::note_bind(what, local, exposes);
        }
    }
}

const CUES_EXPOSED: &str = "anyone who reaches it can send cues";

// Re-frame one inbound message as /external-osc-cue and emit it. Recurses into
// bundles, forwarding each contained message.
fn forward(emit: &mut impl FnMut(&[u8]), ip: &str, port: i32, pkt: &OscPacket) {
    match pkt {
        OscPacket::Message(m) => {
            let mut args = Vec::with_capacity(m.args.len() + 3);
            args.push(OscArg::Str(ip.to_string()));
            args.push(OscArg::Int(port));
            args.push(OscArg::Str(m.addr.clone()));
            args.extend(m.args.iter().cloned());
            emit(&encode("/external-osc-cue", &args));
        }
        OscPacket::Bundle(b) => {
            for el in &b.elements {
                forward(emit, ip, port, el);
            }
        }
    }
}

/// What the fuzzer does to a datagram from anyone who can reach the cue port:
/// decode it as the cue server does and re-frame it, from an IPv4 and an IPv6
/// sender. Each message in it, bundles flattened in order, must come out as
/// exactly one `/external-osc-cue` that decodes to the sender, its port, the
/// message's address and the message's arguments, encoded as they came in.
/// rust/fuzz's `cue_reframe` target calls this under libFuzzer; the corpus
/// test calls it over corpus/ on every push.
pub fn exercise(data: &[u8]) {
    fn flatten<'a>(pkt: &'a OscPacket, out: &mut Vec<&'a OscMessage>) {
        match pkt {
            OscPacket::Message(m) => out.push(m),
            OscPacket::Bundle(b) => b.elements.iter().for_each(|el| flatten(el, out)),
        }
    }
    let Some(pkt) = decode_packet(data) else { return };
    let mut messages = Vec::new();
    flatten(&pkt, &mut messages);

    for (ip, port) in [("203.0.113.7", 57120), ("2001:db8::7", 65535)] {
        let mut cues = Vec::new();
        forward(&mut |osc: &[u8]| cues.push(osc.to_vec()), ip, port, &pkt);
        assert_eq!(cues.len(), messages.len(), "not one cue per message");
        for (cue, m) in cues.iter().zip(&messages) {
            let cue = decode(cue).expect("a cue decodes");
            assert_eq!(cue.addr, "/external-osc-cue");
            let [OscArg::Str(from), OscArg::Int(from_port), OscArg::Str(addr), args @ ..] =
                cue.args.as_slice()
            else {
                panic!("a cue does not start with the sender, its port and the address");
            };
            assert_eq!((from.as_str(), *from_port, addr), (ip, port, &m.addr));
            assert_eq!(encode(addr, args), encode(&m.addr, &m.args), "the arguments changed");
        }
    }
}

fn run_cue_server(socket: UdpSocket, stop: Arc<AtomicBool>, cues_on: Arc<AtomicBool>, host: Host) {
    // Timed read so the loop wakes to check `stop`; ~0% idle CPU.
    let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
    let mut buf = vec![0u8; 65536];
    while !stop.load(Ordering::Relaxed) {
        match socket.recv_from(&mut buf) {
            Ok((n, src)) => {
                // Drain regardless, but only forward when cue reception is on.
                if cues_on.load(Ordering::Relaxed) {
                    match decode_packet(&buf[..n]) {
                        // Fence the codec/emit so a panic drops one datagram rather
                        // than silently killing the recv thread (server bound-but-deaf).
                        Some(pkt) => no_unwind((), || {
                            let (ip, port) = (src.ip().to_string(), src.port() as i32);
                            forward(&mut |osc: &[u8]| host.emit(osc), &ip, port, &pkt)
                        }),
                        // Don't silently swallow junk — surface the first few so a
                        // misbehaving external sender is debuggable (cap to avoid a
                        // flood).
                        None => {
                            if MALFORMED_LOGGED.fetch_add(1, Ordering::Relaxed) < 5 {
                                clockwork_log::log!("[osc] dropped malformed inbound OSC from {} ({n} bytes)", src.ip());
                            }
                        }
                    }
                }
            }
            Err(e) if crate::recv_retryable(&e) => {}
            Err(_) => break, // socket closed / fatal
        }
    }
}

impl ClockworkOsc {
    fn configure(&self, port: i32, loopback: bool, cues_on: bool) {
        self.cues_on.store(cues_on, Ordering::Relaxed);

        let mut cs = self.cue.lock().unwrap();
        if port <= 0 {
            cs.server = None; // unbind
            cs.port = 0;
            return;
        }
        let port = port as u16;
        if cs.server.is_some() && cs.port == port && cs.loopback == loopback {
            return; // only cues_on changed — no rebind
        }

        cs.server = None; // drop old first (joins threads, frees the ports)

        // Bind IPv4 + IPv6 listeners (each family is best-effort: a host with no
        // IPv6 still gets IPv4, and vice versa).
        let (v4ip, v6ip) = if loopback {
            ("127.0.0.1", Ipv6Addr::LOCALHOST)
        } else {
            ("0.0.0.0", Ipv6Addr::UNSPECIFIED)
        };
        let scope = if loopback { "loopback only" } else { "all interfaces" };

        let mut socks: Vec<(&str, UdpSocket)> = Vec::new();
        match UdpSocket::bind((v4ip, port)) {
            Ok(s) => socks.push(("IPv4", s)),
            Err(e) => clockwork_log::log!("[osc] cue server IPv4 bind on port {port} failed: {e}"),
        }
        match bind_v6(v6ip, port) {
            Ok(s) => socks.push(("IPv6", s)),
            Err(e) => clockwork_log::log!("[osc] cue server IPv6 bind on port {port} failed: {e}"),
        }
        if socks.is_empty() {
            return;
        }
        note_binds("cue server", socks.iter().map(|(_, s)| s), CUES_EXPOSED);

        let stop = Arc::new(AtomicBool::new(false));
        let mut joins = Vec::new();
        for (fam, sock) in socks {
            let (t_stop, t_on, t_host) = (stop.clone(), self.cues_on.clone(), self.host);
            if let Ok(j) = std::thread::Builder::new()
                .name("ss-osc-cue".into())
                .spawn(move || run_cue_server(sock, t_stop, t_on, t_host))
            {
                joins.push(j);
                clockwork_log::log!("[osc] cue server listening on port {port} {fam} ({scope})");
            }
        }
        cs.server = Some(CueServer { stop, joins });
        cs.port = port;
        cs.loopback = loopback;
    }

    // Whether the socket took it: false with no sender to take it.
    fn send(&self, host: &str, port: u16, data: &[u8]) -> bool {
        self.out.as_ref().is_some_and(|out| out.send(host, port, data))
    }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Create the OSC subsystem. Returns an owning pointer (null on failure); free
/// with [`clockwork_osc_destroy`]. `ctx`/`emit` must stay valid until then.
#[no_mangle]
pub extern "C" fn clockwork_osc_create(ctx: *mut c_void, emit: EmitFn) -> *mut ClockworkOsc {
    no_unwind(std::ptr::null_mut(), || {
        let out = crate::out::OscSender::new();
        if out.is_none() {
            clockwork_log::log!("[osc] failed to open any outbound socket");
        }
        Box::into_raw(Box::new(ClockworkOsc {
            host: Host { ctx, emit },
            out,
            cues_on: Arc::new(AtomicBool::new(false)),
            cue: Mutex::new(CueState { server: None, port: 0, loopback: true }),
        }))
    })
}

/// Destroy the subsystem: stops the cue recv threads and closes the sockets.
///
/// # Safety
/// `handle` is null or from [`clockwork_osc_create`], not yet destroyed, and
/// nothing uses it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_destroy(handle: *mut ClockworkOsc) {
    if handle.is_null() {
        return;
    }
    // SAFETY: the box `clockwork_osc_create` leaked, freed once, here.
    no_unwind((), || drop(unsafe { Box::from_raw(handle) }));
}

/// (Re)configure the cue server: bind `port` (0 = unbind) on loopback only
/// (`loopback` != 0) or all interfaces, and toggle inbound cue forwarding
/// (`cues_on` != 0). Off the audio thread.
///
/// # Safety
/// `handle` is null or a live handle from [`clockwork_osc_create`].
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_configure(handle: *mut ClockworkOsc, port: i32, loopback: i32, cues_on: i32) {
    if handle.is_null() {
        return;
    }
    // SAFETY: a live handle, per the contract.
    let me = unsafe { &*handle };
    no_unwind((), || me.configure(port, loopback != 0, cues_on != 0));
}

/// Send one OSC packet (`data`/`len`) to `host`:`port`. `host` is a byte string
/// (ptr+len, not NUL-terminated; an IPv4/IPv6 literal or a hostname, no brackets).
/// Off the audio thread (the deferred-event dispatch thread). 1 when the socket
/// took the datagram, 0 when it could not be sent.
///
/// # Safety
/// `handle` is null or a live handle; `host` and `data` are null or readable
/// for `host_len` and `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_send(
    handle: *mut ClockworkOsc,
    host: *const u8,
    host_len: u32,
    port: i32,
    data: *const u8,
    len: u32,
) -> i32 {
    if handle.is_null() || host.is_null() || data.is_null() || port <= 0 || port > 65535 {
        return 0;
    }
    // SAFETY: a live handle and two (pointer, length) byte strings, per the
    // contract; nulls were refused above.
    let (me, host_bytes, data) = unsafe {
        (
            &*handle,
            slice::from_raw_parts(host, host_len as usize),
            slice::from_raw_parts(data, len as usize),
        )
    };
    no_unwind(0, || match std::str::from_utf8(host_bytes) {
        Ok(h) => me.send(h, port as u16, data) as i32,
        Err(_) => 0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;
    use std::time::{Duration, Instant};
    use clockwork_osc::{decode, encode, OscArg, OscMessage};

    // Capture emitted /external-osc-cue packets via the C emit callback.
    struct Collector(Mutex<Vec<Vec<u8>>>);
    extern "C" fn collect(ctx: *mut c_void, _kind: i32, osc: *const u8, len: u32) {
        // SAFETY: `ctx` is the boxed `Collector` each test passes and keeps
        // alive until after destroy; `osc`/`len` is the callback's contract.
        let (c, bytes) = unsafe { (&*ctx.cast::<Collector>(), slice::from_raw_parts(osc, len as usize)) };
        c.0.lock().unwrap().push(bytes.to_vec());
    }
    // The C ABI, with the test's contract stated once: `h` is a live handle
    // from `clockwork_osc_create`, destroyed exactly once at the end.
    fn configure(h: *mut ClockworkOsc, port: u16, loopback: bool, cues_on: bool) {
        // SAFETY: see above.
        unsafe { clockwork_osc_configure(h, port as i32, loopback as i32, cues_on as i32) };
    }
    fn destroy(h: *mut ClockworkOsc) {
        // SAFETY: see above.
        unsafe { clockwork_osc_destroy(h) };
    }
    fn cues(c: &Collector) -> Vec<OscMessage> {
        c.0.lock().unwrap().iter().filter_map(|b| decode(b)).collect()
    }

    fn free_port() -> u16 {
        UdpSocket::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
    }
    fn v6_loopback_available() -> bool {
        UdpSocket::bind("[::1]:0").is_ok()
    }
    // Bounded only against what never comes: the recv thread takes the time
    // it takes on a loaded machine, and a passing case never waits it out.
    fn wait_until(mut f: impl FnMut() -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        false
    }
    fn send_osc(h: *mut ClockworkOsc, host: &str, port: u16, data: &[u8]) {
        // SAFETY: a live handle and two live slices.
        unsafe {
            clockwork_osc_send(h, host.as_ptr(), host.len() as u32, port as i32,
                        data.as_ptr(), data.len() as u32);
        }
    }

    // The cue server's `loopback = 0` is an open bind too, logged with what it
    // exposes: cues, never commands.
    #[test]
    fn cue_server_on_every_interface_is_logged() {
        clockwork_log::capture::install();
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = clockwork_osc_create(ctx, collect);
        let port = free_port();
        configure(h, port, false, true);
        assert!(clockwork_log::capture::logged(&format!("cue server bound to 0.0.0.0:{port}: {CUES_EXPOSED}")));
        destroy(h);
    }

    // INBOUND: external OSC on the cue port (IPv4 and IPv6) is forwarded as an
    // /external-osc-cue carrying the correct sender family.
    #[test]
    fn inbound_ipv4_and_ipv6() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = clockwork_osc_create(ctx, collect);
        let have_v6 = v6_loopback_available();

        let port = free_port();
        configure(h, port, true, true);
        std::thread::sleep(Duration::from_millis(150)); // let recv threads bind

        let s4 = UdpSocket::bind("127.0.0.1:0").unwrap();
        let s6 = if have_v6 { Some(UdpSocket::bind("[::1]:0").unwrap()) } else { None };
        let msg = encode("/hello", &[OscArg::Int(42), OscArg::Str("hi".into())]);

        let ok = wait_until(|| {
            let _ = s4.send_to(&msg, ("127.0.0.1", port));
            if let Some(s6) = &s6 {
                let _ = s6.send_to(&msg, ("::1", port));
            }
            let got = cues(&collector);
            let v4 = got.iter().any(|m| m.args.first().and_then(|a| a.as_str()) == Some("127.0.0.1"));
            let v6 = !have_v6
                || got.iter().any(|m| m.args.first().and_then(|a| a.as_str()).is_some_and(|s| s.contains(':')));
            v4 && v6
        });
        assert!(ok, "expected inbound cues for IPv4{}", if have_v6 { " and IPv6" } else { " (no v6 here)" });

        // Verify the re-frame shape: /external-osc-cue <ip> <port> <addr> <args...>
        let cue = cues(&collector).into_iter().find(|m| m.addr == "/external-osc-cue").unwrap();
        assert_eq!(cue.args.get(2).and_then(|a| a.as_str()), Some("/hello"));
        assert_eq!(cue.args.get(3).and_then(|a| a.as_i32()), Some(42));
        assert_eq!(cue.args.get(4).and_then(|a| a.as_str()), Some("hi"));

        destroy(h);
        drop(collector);
    }

    // OUTBOUND: clockwork_osc_send delivers to an IPv4 and an IPv6 listener.
    #[test]
    fn outbound_ipv4_and_ipv6() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = clockwork_osc_create(ctx, collect);
        let inner = encode("/bar", &[OscArg::Int(7)]);
        let mut buf = [0u8; 1024];

        let l4 = UdpSocket::bind("127.0.0.1:0").unwrap();
        l4.set_read_timeout(Some(Duration::from_secs(30))).unwrap();
        let p4 = l4.local_addr().unwrap().port();
        send_osc(h, "127.0.0.1", p4, &inner);
        let (n, _) = l4.recv_from(&mut buf).expect("IPv4 outbound delivery");
        assert_eq!(decode(&buf[..n]).unwrap().addr, "/bar");

        if v6_loopback_available() {
            let l6 = UdpSocket::bind("[::1]:0").unwrap();
            l6.set_read_timeout(Some(Duration::from_secs(30))).unwrap();
            let p6 = l6.local_addr().unwrap().port();
            send_osc(h, "::1", p6, &inner);
            let (n, _) = l6.recv_from(&mut buf).expect("IPv6 outbound delivery");
            let m = decode(&buf[..n]).unwrap();
            assert_eq!(m.addr, "/bar");
            assert_eq!(m.args.first().and_then(|a| a.as_i32()), Some(7));
        }

        destroy(h);
        drop(collector);
    }

    // SELF-LOOP: send to our own cue port via "localhost" — must round-trip back
    // as a cue regardless of which family localhost resolves to.
    #[test]
    fn self_loop_localhost() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = clockwork_osc_create(ctx, collect);
        let port = free_port();
        configure(h, port, true, true);
        std::thread::sleep(Duration::from_millis(150));

        let inner = encode("/selfcue", &[OscArg::Int(99)]);
        let ok = wait_until(|| {
            send_osc(h, "localhost", port, &inner);
            cues(&collector).iter().any(|m| m.args.get(2).and_then(|a| a.as_str()) == Some("/selfcue"))
        });
        assert!(ok, "localhost self-loop should round-trip");

        destroy(h);
        drop(collector);
    }

    // "Allow Incoming OSC" semantic: cues_on gates forwarding. Off → inbound is
    // received but dropped (no cue); flipping it on (no rebind) resumes cues.
    #[test]
    fn cues_on_gates_forwarding() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = clockwork_osc_create(ctx, collect);
        let port = free_port();
        configure(h, port, true, false);
        std::thread::sleep(Duration::from_millis(150)); // recv threads bind (and drop)

        let s = UdpSocket::bind("127.0.0.1:0").unwrap();
        let msg = encode("/probe", &[OscArg::Int(1)]);
        for _ in 0..8 {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(cues(&collector).is_empty(), "cues_on=0 must NOT forward inbound OSC");

        // Enable forwarding (same port/loopback → no rebind) and confirm cues flow.
        configure(h, port, true, true);
        let ok = wait_until(|| {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            !cues(&collector).is_empty()
        });
        assert!(ok, "cues_on=1 must forward inbound OSC");

        destroy(h);
        drop(collector);
    }

}
