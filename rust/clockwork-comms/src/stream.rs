// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Connection-oriented OSC servers — TCP and UDS-stream — sharing one framed
//! protocol and one connection registry.
//!
//! Wire format: each OSC packet is preceded by a 4-byte big-endian length
//! (the usual OSC-over-stream framing). A frame longer than [`MAX_FRAME`] or
//! of zero length is a protocol violation and closes the connection.
//!
//! Every server starts with the host's secret (`secret.rs`) and refuses to
//! start without one. The first frame on every connection must be the
//! secret's auth packet. Nothing is delivered before it; a connection whose
//! first frame is anything else, or that has not sent it within
//! [`AUTH_DEADLINE`], is closed and counted, and the host never hears of it.
//!
//! One client. A server has at most one connection that presented the
//! secret, and one more still waiting to. A connection made while a client is
//! in is closed at accept, and one that presents the secret once a client is
//! in is closed then, so the excess client sees a crisp reset instead of a
//! silent hang. A connection made while one is waiting is closed at accept
//! too: nothing displaces a connection, so no newcomer can push one out, and
//! a silent connection holds its slot for [`AUTH_DEADLINE`] at most. What
//! stands behind the one client, and how many it serves, is the host's.
//! Connection ids are
//! minted monotonically from 1 and never reused — the C++ transport uses them
//! directly as origin tokens (0 = in-process caller, so ids start at 1).
//!
//! On disconnect (EOF, error, or protocol violation) the connection leaves
//! the registry and, if it got past the secret, `on_closed` fires, so the
//! transport can drop that
//! client's notify subscriptions — subscription lifetime == connection
//! lifetime. `on_closed` is suppressed during server stop (the host is
//! tearing down and must not be re-entered).

use std::ffi::c_void;
use std::slice;

use crate::secret::ClockworkCommsSecret;

/// Longest accepted frame payload. Sized to admit whatever blob the DSP's
/// define verb carries (the native IN ring is hundreds of KB for that reason), matching
/// the SHM command plane's ring so every local command transport agrees on
/// the largest command it will carry; still a hard bound so a stream client
/// can't make a reader accumulate unboundedly. (The UDP transport keeps its
/// own 64 KiB datagram cap — a datagram can't exceed that anyway.)
pub const MAX_FRAME: usize = 256 * 1024;

/// Packet callback: (ctx, conn_id, osc, len). `osc` is one complete de-framed
/// OSC packet, valid only for the duration of the call. May fire on any
/// connection's reader thread.
pub type StreamPacketFn = extern "C" fn(*mut c_void, u32, *const u8, u32);
/// Disconnect callback: (ctx, conn_id). Fires once per connection after it
/// has left the registry; not fired during server stop.
pub type StreamClosedFn = extern "C" fn(*mut c_void, u32);

/// One running stream server behind the C ABI — socket-based (TCP/UDS) here,
/// named-pipe-based (Windows) in `pipe.rs`. The handle owns the accept and
/// reader threads; dropping it stops them all.
pub struct ClockworkOscStream(pub(crate) Box<dyn StreamServerImpl>);

/// What every stream server backend provides to the shared C ABI.
pub(crate) trait StreamServerImpl: Send {
    /// Frame and send one packet to a live connection.
    fn send(&self, conn_id: u32, data: &[u8]) -> bool;
    /// The actual bound TCP port (0 for path/name-addressed servers).
    fn port(&self) -> i32 {
        0
    }
    /// Connections closed for not presenting the secret.
    fn refused(&self) -> u64;
}

/// The host callbacks + opaque context, shared by every reader thread.
#[derive(Clone, Copy)]
pub(crate) struct Host {
    pub ctx: *mut c_void,
    pub on_packet: StreamPacketFn,
    pub on_closed: StreamClosedFn,
}
// SAFETY: `ctx` is an opaque token this crate never dereferences, only hands
// to the callbacks, and the C++ side's contract is that it outlives the
// server and the callbacks are thread-safe (the same contract as the UDP
// emit callback); fn pointers are Send + Sync.
unsafe impl Send for Host {}
// SAFETY: as above.
unsafe impl Sync for Host {}

impl Host {
    pub(crate) fn packet(&self, conn: u32, osc: &[u8]) {
        no_unwind((), || (self.on_packet)(self.ctx, conn, osc.as_ptr(), osc.len() as u32));
    }
    pub(crate) fn closed(&self, conn: u32) {
        no_unwind((), || (self.on_closed)(self.ctx, conn));
    }
}

/// Frame `data` for the wire: 4-byte big-endian length + payload.
pub(crate) fn frame_packet(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + data.len());
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}

/// Feed newly-read bytes into the accumulator and deliver every complete
/// frame. Returns false on a protocol violation (zero-length or oversize
/// frame) — the caller closes the connection.
pub(crate) fn drain_frames(acc: &mut Vec<u8>, mut deliver: impl FnMut(&[u8])) -> bool {
    loop {
        if acc.len() < 4 {
            return true;
        }
        let len = u32::from_be_bytes([acc[0], acc[1], acc[2], acc[3]]) as usize;
        if len == 0 || len > MAX_FRAME {
            return false;
        }
        if acc.len() < 4 + len {
            return true;
        }
        deliver(&acc[4..4 + len]);
        acc.drain(..4 + len);
    }
}

/// How long a new connection has to present the secret.
pub(crate) const AUTH_DEADLINE: Duration = Duration::from_secs(2);

/// Where a connection stands with the secret.
#[derive(Debug, PartialEq)]
pub(crate) enum Gate {
    /// Not enough bytes yet to tell.
    Waiting,
    /// The first frame was the secret's auth packet, and is consumed.
    Open,
    /// The first frame was something else; the connection closes.
    Refused,
    /// The secret was presented with the cap already full; the connection
    /// closes, and the host never hears of it.
    OverCap,
}

/// Look at the first frame in `acc`, before anything is delivered.
pub(crate) fn check_auth(acc: &mut Vec<u8>, secret: &ClockworkCommsSecret) -> Gate {
    if acc.len() < 4 {
        return Gate::Waiting;
    }
    let len = u32::from_be_bytes([acc[0], acc[1], acc[2], acc[3]]) as usize;
    if len != secret.auth_packet().len() {
        return Gate::Refused;
    }
    if acc.len() < 4 + len {
        return Gate::Waiting;
    }
    if !secret.admits(&acc[4..4 + len]) {
        return Gate::Refused;
    }
    acc.drain(..4 + len);
    Gate::Open
}

/// What the fuzzer does to the bytes a stream client sends, read the way the
/// server reads them: the secret first, then the framing. The low four bits
/// of the first byte say how many reads to cut the stream into, and bit 4
/// puts a fixed secret's auth frame in front of the stream, which the fuzzer
/// would never guess; without it, the stream's own first frame meets the
/// gate. The next two bytes per read give each read's length, and the rest
/// is the stream. It goes through [`check_auth`] and [`drain_frames`] three
/// ways: in one read, in the input's reads, and a byte at a time. Each way
/// must decide on the secret where a plain walk of the bytes does, deliver
/// nothing before it, deliver after it the frames the walk finds, refuse a
/// bad length where the walk does, and hold no more than one unfinished
/// frame between reads. rust/fuzz's `stream_frames` target calls this under
/// libFuzzer; the corpus test calls it over corpus/ on every push.
pub fn exercise(data: &[u8]) {
    let secret = ClockworkCommsSecret::from_bytes(std::array::from_fn(|i| i as u8 + 1))
        .expect("a secret that is not all zeros");
    let auth = frame_packet(&secret.auth_packet());
    let Some((&first, mut rest)) = data.split_first() else { return };
    let mut reads = Vec::new();
    for _ in 0..first & 0x0f {
        let Some((n, tail)) = rest.split_first_chunk::<2>() else { return };
        reads.push(usize::from(u16::from_be_bytes(*n)));
        rest = tail;
    }
    let with_secret;
    let stream = if first & 0x10 != 0 {
        with_secret = [&auth[..], rest].concat();
        &with_secret[..]
    } else {
        rest
    };

    let gate = match stream.get(..4) {
        None => Gate::Waiting,
        Some(head) if head != &auth[..4] => Gate::Refused,
        Some(_) => match stream.get(..auth.len()) {
            None => Gate::Waiting,
            Some(f) if f == &auth[..] => Gate::Open,
            Some(_) => Gate::Refused,
        },
    };
    let mut want = Vec::new();
    let mut pos = auth.len();
    let mut framed = true;
    while let Some(head) = stream.get(pos..pos + 4).filter(|_| gate == Gate::Open) {
        let len = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
        if len == 0 || len > MAX_FRAME {
            framed = false;
            break;
        }
        let Some(osc) = stream.get(pos + 4..pos + 4 + len) else { break };
        want.push(osc);
        pos += 4 + len;
    }

    let mut cut = Vec::new();
    let mut at = 0;
    for n in reads {
        let end = (at + n).min(stream.len());
        cut.push(&stream[at..end]);
        at = end;
    }
    cut.push(&stream[at..]);
    let bytes: Vec<&[u8]> = stream.chunks(1).collect();

    for reads in [&[stream][..], &cut, &bytes] {
        let mut acc = Vec::new();
        let mut got: Vec<Vec<u8>> = Vec::new();
        let mut at_gate = Gate::Waiting;
        let mut ok = true;
        for read in reads {
            acc.extend_from_slice(read);
            if at_gate == Gate::Waiting {
                at_gate = check_auth(&mut acc, &secret);
                match at_gate {
                    Gate::Refused | Gate::OverCap => break,
                    Gate::Waiting => {
                        assert!(acc.len() < auth.len(), "more than the secret's frame held for it");
                        continue;
                    }
                    Gate::Open => {}
                }
            }
            if !drain_frames(&mut acc, |osc| got.push(osc.to_vec())) {
                ok = false;
                break;
            }
            assert!(acc.len() < 4 + MAX_FRAME, "more than one unfinished frame held");
        }
        assert_eq!(at_gate, gate, "the secret decided at a different place");
        assert_eq!(got, want, "the frames delivered are not the stream's frames");
        if gate == Gate::Open {
            assert_eq!(ok, framed, "a bad length refused at a different place");
            if ok {
                assert_eq!(acc, &stream[pos..], "the unfinished frame held is not the stream's tail");
            }
        }
    }
}

// ── Socket backend (TCP + UDS stream) ────────────────────────────────────────

use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
#[cfg(unix)]
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use clockwork_osc::ffi::no_unwind;

/// Bound on a single blocking send. A client that hasn't drained its socket
/// within this long is treated as stuck: the write fails and the connection
/// is dropped, so one non-reading client can't wedge the gateway thread (the
/// sole caller of send/broadcast) indefinitely on a full kernel send buffer.
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);

/// How many times one frame may be re-offered to a socket that refused it
/// without taking any bytes. Bounded so a client whose refusal never clears
/// still ends up evicted instead of parking the gateway thread.
const WRITE_RETRIES: usize = 3;

/// Pause between those retries — long enough for a momentary kernel buffer
/// shortage to clear, short enough that the whole bounded sequence stays far
/// inside WRITE_TIMEOUT.
const WRITE_RETRY_PAUSE: Duration = Duration::from_millis(1);

/// A connection's write half: raw writes + a shutdown to unblock its reader.
/// Byte-level rather than write-all so the retry policy can tell "nothing went
/// out" from "half a frame went out", which decides whether framing survived.
trait ConnWriter: Send {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize>;
    fn set_write_timeout(&self, d: Duration);
    fn shutdown_both(&self);
}
impl ConnWriter for TcpStream {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.write(data)
    }
    fn set_write_timeout(&self, d: Duration) {
        let _ = TcpStream::set_write_timeout(self, Some(d));
    }
    fn shutdown_both(&self) {
        let _ = self.shutdown(Shutdown::Both);
    }
}
#[cfg(unix)]
impl ConnWriter for UnixStream {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.write(data)
    }
    fn set_write_timeout(&self, d: Duration) {
        let _ = UnixStream::set_write_timeout(self, Some(d));
    }
    fn shutdown_both(&self) {
        let _ = self.shutdown(Shutdown::Both);
    }
}

/// Is this write error worth re-offering the frame for?
///
/// `Interrupted` is EINTR — no bytes were taken and no state was lost.
/// `Other` is where a transient kernel refusal such as ENOBUFS lands, since
/// std has no ErrorKind for it. Everything else is either terminal (the peer
/// is gone) or WouldBlock/TimedOut, which on a blocking socket means
/// SO_SNDTIMEO expired: a client that stopped reading, and must be dropped
/// rather than retried.
fn write_retryable(e: &std::io::Error) -> bool {
    matches!(e.kind(), ErrorKind::Interrupted | ErrorKind::Other)
}

/// Push one whole frame at a connection, resuming after a retryable refusal.
/// An error means the connection is finished: either the peer is gone, or a
/// refusal outlasted the retry budget with part of a frame already on the
/// wire, which leaves the stream desynced.
fn write_frame(w: &mut dyn ConnWriter, frame: &[u8]) -> std::io::Result<()> {
    let (mut sent, mut retries) = (0usize, 0usize);
    while sent < frame.len() {
        match w.write_some(&frame[sent..]) {
            Ok(0) => return Err(std::io::Error::from(ErrorKind::WriteZero)),
            Ok(n) => sent += n,
            Err(e) if write_retryable(&e) && retries < WRITE_RETRIES => {
                retries += 1;
                // EINTR costs nothing to re-issue; a kernel refusal needs a
                // moment before it can clear.
                if e.kind() != ErrorKind::Interrupted {
                    std::thread::sleep(WRITE_RETRY_PAUSE);
                }
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// A connection's read half, abstracted over TCP/UDS.
trait ConnReader: Read + Send + 'static {
    fn set_timeout(&self, d: Duration);
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>>;
}
impl ConnReader for TcpStream {
    fn set_timeout(&self, d: Duration) {
        let _ = self.set_read_timeout(Some(d));
    }
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
        Ok(Box::new(self.try_clone()?))
    }
}
#[cfg(unix)]
impl ConnReader for UnixStream {
    fn set_timeout(&self, d: Duration) {
        let _ = self.set_read_timeout(Some(d));
    }
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
        Ok(Box::new(self.try_clone()?))
    }
}

/// A nonblocking listener, abstracted over TCP/UDS.
trait Acceptor: Send + 'static {
    type Conn: ConnReader;
    fn accept_one(&self) -> std::io::Result<Self::Conn>;
}
impl Acceptor for TcpListener {
    type Conn = TcpStream;
    fn accept_one(&self) -> std::io::Result<TcpStream> {
        // BSD/Windows accepted sockets inherit the listener's non-blocking
        // flag (Linux's don't). A non-blocking conn ignores SO_RCVTIMEO, so
        // the reader's 100ms-timeout read becomes an EAGAIN hot spin — one
        // full core per connection. Force blocking before handing it over.
        self.accept().map(|(s, _)| {
            let _ = s.set_nonblocking(false);
            // Replies are small frames on loopback: never let Nagle hold one
            // back waiting for an ACK. (Clients set NODELAY on their side too.)
            let _ = s.set_nodelay(true);
            s
        })
    }
}
#[cfg(unix)]
impl Acceptor for UnixListener {
    type Conn = UnixStream;
    fn accept_one(&self) -> std::io::Result<UnixStream> {
        self.accept().map(|(s, _)| {
            let _ = s.set_nonblocking(false);
            s
        })
    }
}

type Writer = Arc<Mutex<Box<dyn ConnWriter>>>;

/// One accepted connection: its writer, and whether it has presented the
/// secret, which is what the cap counts.
struct Conn {
    writer: Writer,
    open: bool,
}

/// Whether a connection that just presented the secret may open: under the
/// registry lock, so two cannot both become the client.
fn admit(conns: &Mutex<HashMap<u32, Conn>>, id: u32) -> bool {
    let mut conns = conns.lock().unwrap();
    if conns.values().any(|c| c.open) {
        return false;
    }
    if let Some(c) = conns.get_mut(&id) {
        c.open = true;
    }
    true
}

/// State shared between the accept loop, reader threads, and the C ABI.
struct Shared {
    stop: AtomicBool,
    conns: Mutex<HashMap<u32, Conn>>,
    next_id: AtomicU32, // conn ids from 1, never reused (0 = in-process)
    readers: Mutex<Vec<JoinHandle<()>>>,
    secret: ClockworkCommsSecret,
    refused: AtomicU64,
}

struct SocketServer {
    shared: Arc<Shared>,
    accept_join: Option<JoinHandle<()>>,
    port: i32,
    path: Option<PathBuf>, // UDS: unlink on drop
}

impl StreamServerImpl for SocketServer {
    fn send(&self, conn_id: u32, data: &[u8]) -> bool {
        // Clone the writer handle out of the registry, then write outside the
        // registry lock — a slow client must not stall unrelated sends.
        let writer = self.shared.conns.lock().unwrap().get(&conn_id).map(|c| c.writer.clone());
        let Some(writer) = writer else { return false };
        let err = match write_frame(&mut **writer.lock().unwrap(), &frame_packet(data)) {
            Ok(()) => return true,
            Err(e) => e,
        };
        // The frame did not make it and retrying is pointless: the peer is gone,
        // or a refusal outlasted the budget mid-frame and this connection's
        // framing is now suspect. Evict and shut it down; the reader wakes on
        // the shutdown and fires on_closed, pruning the client from every
        // audience. The logged error kind says which: WouldBlock/TimedOut is a
        // client that went WRITE_TIMEOUT without reading, anything else failed
        // outright.
        clockwork_log::log!("[osc] stream conn {conn_id} dropped — reply write failed: {err}");
        if let Some(c) = self.shared.conns.lock().unwrap().remove(&conn_id) {
            c.writer.lock().unwrap().shutdown_both();
        }
        false
    }
    fn port(&self) -> i32 {
        self.port
    }
    fn refused(&self) -> u64 {
        self.shared.refused.load(Ordering::Relaxed)
    }
}

impl Drop for SocketServer {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        // Shut every connection down so blocked readers return immediately
        // rather than waiting out their read timeout.
        for c in self.shared.conns.lock().unwrap().values() {
            c.writer.lock().unwrap().shutdown_both();
        }
        if let Some(j) = self.accept_join.take() {
            let _ = j.join();
        }
        for j in self.shared.readers.lock().unwrap().drain(..) {
            let _ = j.join();
        }
        if let Some(p) = &self.path {
            let _ = std::fs::remove_file(p);
        }
    }
}

fn run_reader<C: ConnReader>(mut conn: C, id: u32, shared: Arc<Shared>, host: Host) {
    conn.set_timeout(Duration::from_millis(100));
    let mut acc: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 8192];
    // Why this connection ended, when it wasn't the peer's own doing. A clean
    // EOF is the client's decision and stays quiet; anything else is the
    // server hanging up on a live client, so say so.
    let mut why: Option<String> = None;
    let opened = Instant::now();
    let mut gate = Gate::Waiting;
    while !shared.stop.load(Ordering::Relaxed) {
        if gate == Gate::Waiting && opened.elapsed() >= AUTH_DEADLINE {
            gate = Gate::Refused;
        }
        if gate == Gate::Refused || gate == Gate::OverCap {
            break;
        }
        match conn.read(&mut tmp) {
            Ok(0) => break, // EOF — peer gone
            Ok(n) => {
                acc.extend_from_slice(&tmp[..n]);
                if gate == Gate::Waiting {
                    gate = check_auth(&mut acc, &shared.secret);
                    if gate == Gate::Open && !admit(&shared.conns, id) {
                        gate = Gate::OverCap;
                    }
                    if gate != Gate::Open {
                        continue;
                    }
                }
                if !drain_frames(&mut acc, |osc| host.packet(id, osc)) {
                    why = Some("protocol violation (bad frame length)".into());
                    break;
                }
            }
            Err(e) if clockwork_osc_net::recv_retryable(&e) => {}
            Err(e) => {
                why = Some(format!("read failed: {e}"));
                break;
            }
        }
    }
    // Leave the registry, then tell the host — subscription lifetime ==
    // connection lifetime. Suppressed during stop: the host is tearing down.
    if let Some(c) = shared.conns.lock().unwrap().remove(&id) {
        c.writer.lock().unwrap().shutdown_both();
    }
    if gate == Gate::Refused {
        refuse(&shared.refused, id);
    } else if gate == Gate::OverCap {
        clockwork_log::log!("[osc] stream conn {id} closed — over the connection cap");
    } else if !shared.stop.load(Ordering::Relaxed) && gate == Gate::Open {
        if let Some(why) = why {
            clockwork_log::log!("[osc] stream conn {id} dropped — {why}");
        }
        host.closed(id);
    }
}

/// Count a connection closed for not presenting the secret, and say so for
/// the first few: anything on the machine can connect, so a stream of them
/// must not become a stream of log lines.
pub(crate) fn refuse(refused: &AtomicU64, id: u32) {
    if refused.fetch_add(1, Ordering::Relaxed) < 5 {
        clockwork_log::log!("[osc] stream conn {id} refused — its first frame was not the secret, or it sent none in time");
    }
}

// Join and discard the handles of readers that have already exited, so the
// registry tracks only live readers instead of growing with total-connections
// across a long-running server's reconnect churn. Live readers are left in
// place for the join-on-stop guarantee in SocketServer::drop.
fn reap_finished_readers(shared: &Shared) {
    let mut readers = shared.readers.lock().unwrap();
    let mut i = 0;
    while i < readers.len() {
        if readers[i].is_finished() {
            let _ = readers.swap_remove(i).join(); // already done — returns at once
        } else {
            i += 1;
        }
    }
}

fn run_accept<A: Acceptor>(listener: A, shared: Arc<Shared>, host: Host) {
    while !shared.stop.load(Ordering::Relaxed) {
        reap_finished_readers(&shared);
        match listener.accept_one() {
            Ok(conn) => {
                // Admission control: with a client in, or one still to present
                // the secret, drop at once, so the excess client sees a crisp
                // reset instead of a silent hang. Nothing displaces a
                // connection: a newcomer that could would be a way in for
                // whoever connects most.
                if !shared.conns.lock().unwrap().is_empty() {
                    drop(conn);
                    continue;
                }
                let id = shared.next_id.fetch_add(1, Ordering::Relaxed);
                let writer = match conn.clone_writer() {
                    Ok(w) => {
                        w.set_write_timeout(WRITE_TIMEOUT);
                        Arc::new(Mutex::new(w))
                    }
                    Err(_) => continue,
                };
                shared.conns.lock().unwrap().insert(id, Conn { writer, open: false });
                let (t_shared, t_host) = (shared.clone(), host);
                if let Ok(j) = std::thread::Builder::new()
                    .name("ss-osc-stream".into())
                    .spawn(move || run_reader(conn, id, t_shared, t_host))
                {
                    shared.readers.lock().unwrap().push(j);
                } else {
                    shared.conns.lock().unwrap().remove(&id);
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(_) => std::thread::sleep(Duration::from_millis(20)),
        }
    }
}

fn start_socket_server<A: Acceptor>(
    listener: A,
    host: Host,
    secret: ClockworkCommsSecret,
    port: i32,
    path: Option<PathBuf>,
) -> Option<ClockworkOscStream> {
    let shared = Arc::new(Shared {
        stop: AtomicBool::new(false),
        conns: Mutex::new(HashMap::new()),
        next_id: AtomicU32::new(1),
        readers: Mutex::new(Vec::new()),
        secret,
        refused: AtomicU64::new(0),
    });
    let t_shared = shared.clone();
    let accept_join = std::thread::Builder::new()
        .name("ss-osc-accept".into())
        .spawn(move || run_accept(listener, t_shared, host))
        .ok()?;
    Some(ClockworkOscStream(Box::new(SocketServer {
        shared,
        accept_join: Some(accept_join),
        port,
        path,
    })))
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// A copy of the secret a start function was given. A start without one says
/// so and returns null before it binds anything.
///
/// # Safety
/// `secret` is null or live.
pub(crate) unsafe fn take_secret(
    what: &str,
    secret: *const ClockworkCommsSecret,
) -> Option<ClockworkCommsSecret> {
    if secret.is_null() {
        clockwork_log::log!("[osc] {what} not started: no secret");
        return None;
    }
    // SAFETY: live, per the contract.
    Some(unsafe { &*secret }.clone())
}

/// The address a TCP server binds when given `addr`: loopback when given
/// nothing. The secret crosses the wire in the clear, so a port on the
/// network is something a host asks for by name.
fn bind_address(addr: &str) -> &str {
    if addr.is_empty() { "127.0.0.1" } else { addr }
}

/// Start a TCP OSC server on `port` bound to `bind_addr` (byte string,
/// ptr+len; empty = loopback, else an address literal — "0.0.0.0" for every
/// IPv4 interface, "::" for IPv6; the log notes an open bind). Pass port 0 to
/// bind an ephemeral port and read it back with [`clockwork_osc_stream_port`].
/// Returns an owning pointer (null on bind failure); free with
/// [`clockwork_osc_stream_stop`]. `ctx` and both callbacks must outlive it.
/// The server keeps its own copy of `secret`, and without one does not start.
///
/// # Safety
/// `secret` is null or live; `bind_addr` is null or readable for
/// `bind_addr_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_tcp_start(
    ctx: *mut c_void,
    on_packet: StreamPacketFn,
    on_closed: StreamClosedFn,
    secret: *const ClockworkCommsSecret,
    port: i32,
    bind_addr: *const u8,
    bind_addr_len: u32,
) -> *mut ClockworkOscStream {
    no_unwind(std::ptr::null_mut(), || {
        // SAFETY: null or live, per the contract.
        let Some(secret) = (unsafe { take_secret("TCP", secret) }) else {
            return std::ptr::null_mut();
        };
        if !(0..=65535).contains(&port) {
            return std::ptr::null_mut();
        }
        let addr = if bind_addr.is_null() {
            ""
        } else {
            // SAFETY: readable for `bind_addr_len` bytes, per the contract.
            let bytes = unsafe { slice::from_raw_parts(bind_addr, bind_addr_len as usize) };
            std::str::from_utf8(bytes).unwrap_or("")
        };
        let addr = bind_address(addr);
        let listener = match TcpListener::bind((addr, port as u16)) {
            Ok(l) => l,
            Err(e) => {
                clockwork_log::log!("[osc] TCP bind {addr}:{port} failed: {e}");
                return std::ptr::null_mut();
            }
        };
        if let Ok(local) = listener.local_addr() {
            clockwork_osc_net::note_bind("TCP", local, "the secret crosses the network in the clear");
        }
        let bound = listener.local_addr().map(|a| a.port() as i32).unwrap_or(port);
        if listener.set_nonblocking(true).is_err() {
            return std::ptr::null_mut();
        }
        let host = Host { ctx, on_packet, on_closed };
        match start_socket_server(listener, host, secret, bound, None) {
            Some(srv) => Box::into_raw(Box::new(srv)),
            None => std::ptr::null_mut(),
        }
    })
}

/// Start a UDS stream OSC server bound to `path` (byte string, ptr+len; the
/// socket file is created 0600, replacing a stale file — put it in a 0700
/// directory for the full owner-only guarantee). Null on failure, and always
/// null on Windows (see `pipe.rs` for the named-pipe analogue). Free with
/// [`clockwork_osc_stream_stop`]. Without a secret it does not start.
///
/// # Safety
/// `secret` is null or live; `path` is null or readable for `path_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_uds_stream_start(
    ctx: *mut c_void,
    on_packet: StreamPacketFn,
    on_closed: StreamClosedFn,
    secret: *const ClockworkCommsSecret,
    path: *const u8,
    path_len: u32,
) -> *mut ClockworkOscStream {
    #[cfg(unix)]
    {
        no_unwind(std::ptr::null_mut(), || {
            use std::os::unix::fs::PermissionsExt;
            // SAFETY: null or live, per the contract.
            let Some(secret) = (unsafe { take_secret("UDS stream", secret) }) else {
                return std::ptr::null_mut();
            };
            if path.is_null() {
                return std::ptr::null_mut();
            }
            // SAFETY: readable for `path_len` bytes, per the contract.
            let bytes = unsafe { slice::from_raw_parts(path, path_len as usize) };
            let Ok(path) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            if path.is_empty() {
                return std::ptr::null_mut();
            }
            let pb = PathBuf::from(path);
            let _ = std::fs::remove_file(&pb); // server owns its socket path
            let listener = match UnixListener::bind(&pb) {
                Ok(l) => l,
                Err(e) => {
                    clockwork_log::log!("[osc] UDS stream bind {path} failed: {e}");
                    return std::ptr::null_mut();
                }
            };
            // Owner-only; the bind→chmod window is closed by a 0700 parent dir.
            if std::fs::set_permissions(&pb, std::fs::Permissions::from_mode(0o600)).is_err()
                || listener.set_nonblocking(true).is_err()
            {
                let _ = std::fs::remove_file(&pb);
                return std::ptr::null_mut();
            }
            let host = Host { ctx, on_packet, on_closed };
            match start_socket_server(listener, host, secret, 0, Some(pb)) {
                Some(srv) => Box::into_raw(Box::new(srv)),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(unix))]
    {
        let _ = (ctx, on_packet, on_closed, secret, path, path_len);
        std::ptr::null_mut()
    }
}

/// The server's actual bound TCP port (useful when started with port 0), or
/// 0 for a path/name-addressed server or a null handle.
///
/// # Safety
/// `handle` is null or a live handle from a stream start function.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_stream_port(handle: *mut ClockworkOscStream) -> i32 {
    if handle.is_null() {
        return 0;
    }
    // SAFETY: a live handle, per the contract.
    unsafe { (*handle).0.port() }
}

/// How many connections this server has closed for not presenting the secret:
/// a first frame that was anything else, or none within the deadline. 0 for a
/// null handle.
///
/// # Safety
/// `handle` is null or a live handle from a stream start function.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_stream_refused(handle: *mut ClockworkOscStream) -> u64 {
    if handle.is_null() {
        return 0;
    }
    // SAFETY: a live handle, per the contract.
    unsafe { (*handle).0.refused() }
}

/// Send one OSC packet, framed, to connection `conn_id`. Returns 1 on send,
/// 0 for an unknown/closed connection or write failure. Safe from any
/// non-audio thread (registry + per-connection write locks).
///
/// # Safety
/// `handle` is null or a live handle; `data` is null or readable for `len`
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_stream_send(
    handle: *mut ClockworkOscStream,
    conn_id: u32,
    data: *const u8,
    len: u32,
) -> i32 {
    if handle.is_null() || data.is_null() {
        return 0;
    }
    // SAFETY: a live handle and a readable byte string, per the contract;
    // nulls were refused above.
    let (me, data) = unsafe { (&*handle, slice::from_raw_parts(data, len as usize)) };
    no_unwind(0, || me.0.send(conn_id, data) as i32)
}

/// Stop accepting, close every connection, join the threads, free the server.
/// `on_closed` does not fire for connections closed by the stop itself.
///
/// # Safety
/// `handle` is null or from a stream start function, not yet stopped, and
/// nothing uses it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_stream_stop(handle: *mut ClockworkOscStream) {
    if handle.is_null() {
        return;
    }
    // SAFETY: the box the start function leaked, freed once, here.
    no_unwind((), || drop(unsafe { Box::from_raw(handle) }));
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::io::{Read, Write};
    use std::net::TcpStream;
    use std::sync::Mutex;
    use std::sync::OnceLock;
    use std::time::{Duration, Instant};
    use clockwork_osc::{decode, encode, OscArg};

    // One secret for every server these cases start; `connect` presents it.
    pub(crate) fn test_secret() -> &'static ClockworkCommsSecret {
        static SECRET: OnceLock<ClockworkCommsSecret> = OnceLock::new();
        SECRET.get_or_init(|| ClockworkCommsSecret::generate().expect("an OS random source"))
    }

    // Capture (conn_id, packet) deliveries and closed conn ids.
    pub(crate) struct Cap {
        pub(crate) packets: Mutex<Vec<(u32, Vec<u8>)>>,
        pub(crate) closed: Mutex<Vec<u32>>,
    }
    impl Cap {
        pub(crate) fn new() -> Box<Self> {
            Box::new(Cap { packets: Mutex::new(Vec::new()), closed: Mutex::new(Vec::new()) })
        }
    }
    pub(crate) extern "C" fn on_packet(ctx: *mut c_void, conn: u32, osc: *const u8, len: u32) {
        // SAFETY: `ctx` is the boxed `Cap` each test keeps alive until after
        // the stop; `osc`/`len` is the callback's contract.
        let (c, bytes) = unsafe { (&*ctx.cast::<Cap>(), slice::from_raw_parts(osc, len as usize)) };
        c.packets.lock().unwrap().push((conn, bytes.to_vec()));
    }
    pub(crate) extern "C" fn on_closed(ctx: *mut c_void, conn: u32) {
        // SAFETY: as `on_packet`.
        let c = unsafe { &*ctx.cast::<Cap>() };
        c.closed.lock().unwrap().push(conn);
    }
    // The C ABI, with the test's contract stated once: `h` is a live handle
    // from a start function, stopped exactly once at the end.
    fn send(h: *mut ClockworkOscStream, conn: u32, data: &[u8]) -> i32 {
        // SAFETY: see above; `data` is a live slice.
        unsafe { clockwork_osc_stream_send(h, conn, data.as_ptr(), data.len() as u32) }
    }
    pub(crate) fn stop(h: *mut ClockworkOscStream) {
        // SAFETY: see above.
        unsafe { clockwork_osc_stream_stop(h) };
    }
    pub(crate) fn refused(h: *mut ClockworkOscStream) -> u64 {
        // SAFETY: see above.
        unsafe { clockwork_osc_stream_refused(h) }
    }

    // Waits and read timeouts in these cases are bounded only against what
    // never comes: on a loaded machine the reader thread takes the time it
    // takes, and a passing case never waits out the bound.
    pub(crate) fn wait_until(mut f: impl FnMut() -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        false
    }

    pub(crate) fn frame(osc: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(4 + osc.len());
        out.extend_from_slice(&(osc.len() as u32).to_be_bytes());
        out.extend_from_slice(osc);
        out
    }

    // Read one framed packet off a blocking stream (2s timeout set by caller).
    fn read_frame(s: &mut impl Read) -> Option<Vec<u8>> {
        let mut hdr = [0u8; 4];
        s.read_exact(&mut hdr).ok()?;
        let len = u32::from_be_bytes(hdr) as usize;
        let mut body = vec![0u8; len];
        s.read_exact(&mut body).ok()?;
        Some(body)
    }

    fn start_tcp_on(cap: &Cap, bind: &str) -> (*mut ClockworkOscStream, u16) {
        let ctx = cap as *const Cap as *mut c_void;
        // SAFETY: `bind` is a live str.
        let h = unsafe {
            clockwork_osc_tcp_start(ctx, on_packet, on_closed, test_secret(), 0,
                             bind.as_ptr(), bind.len() as u32)
        };
        assert!(!h.is_null(), "tcp server should start");
        // SAFETY: just started, not yet stopped.
        let port = unsafe { clockwork_osc_stream_port(h) };
        assert!(port > 0, "port-0 start should report the real bound port");
        (h, port as u16)
    }
    fn start_tcp(cap: &Cap) -> (*mut ClockworkOscStream, u16) {
        start_tcp_on(cap, "127.0.0.1")
    }

    // The secret crosses TCP in the clear. So no address means loopback,
    // reachable from this machine, and a bind on anything else is something
    // the host asked for by name, which the log says once.
    #[test]
    fn tcp_with_no_address_binds_loopback_and_an_open_bind_is_logged() {
        assert_eq!(bind_address(""), "127.0.0.1");
        assert_eq!(bind_address("0.0.0.0"), "0.0.0.0");
        clockwork_log::capture::install();
        let cap = Cap::new();
        let (h, port) = start_tcp_on(&cap, "");
        let mut c = connect(port);
        c.write_all(&frame(&encode("/clockwork/status", &[]))).unwrap();
        assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()), "reachable over loopback");
        // The capture is process-wide, so ask about this socket's own address.
        assert!(!clockwork_log::capture::logged(&format!("TCP bound to 127.0.0.1:{port}")), "a loopback bind is not open");
        stop(h);

        let (h, port) = start_tcp_on(&cap, "0.0.0.0");
        assert!(clockwork_log::capture::logged(&format!("TCP bound to 0.0.0.0:{port}: the secret crosses the network in the clear")));
        stop(h);
    }

    fn connect_without_secret(port: u16) -> TcpStream {
        let s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(30))).unwrap();
        s
    }
    fn connect(port: u16) -> TcpStream {
        let mut s = connect_without_secret(port);
        s.write_all(&frame(&test_secret().auth_packet())).unwrap();
        s
    }
    fn closed_by_server(c: &mut TcpStream) -> bool {
        let mut buf = [0u8; 1];
        let got = c.read(&mut buf);
        matches!(got, Ok(0)) || got.is_err()
    }

    // A first frame that is anything but the secret closes the connection,
    // whatever follows it: a command, or the auth packet of another secret.
    // Nothing is delivered, the refusal is counted, and the host never hears
    // of the connection.
    #[test]
    fn a_connection_that_does_not_start_with_the_secret_gets_nothing() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let other = ClockworkCommsSecret::generate().unwrap();
        for first in [frame(&encode("/clockwork/status", &[])), frame(&other.auth_packet())] {
            let mut c = connect_without_secret(port);
            c.write_all(&first).unwrap();
            let _ = c.write_all(&frame(&encode("/after", &[])));
            assert!(closed_by_server(&mut c));
        }
        assert!(wait_until(|| refused(h) == 2), "both refusals counted");
        assert!(cap.packets.lock().unwrap().is_empty(), "nothing delivered");
        assert!(cap.closed.lock().unwrap().is_empty(), "the host never heard of them");
        stop(h);
    }

    // The secret may arrive in any number of reads, and what follows it in
    // the same reads is delivered.
    #[test]
    fn the_secret_split_across_writes_opens_the_connection() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let mut c = connect_without_secret(port);
        let mut bytes = frame(&test_secret().auth_packet());
        bytes.extend_from_slice(&frame(&encode("/after-the-secret", &[])));
        for piece in bytes.chunks(7) {
            c.write_all(piece).unwrap();
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(wait_until(|| cap.packets.lock().unwrap().len() == 1));
        assert_eq!(decode(&cap.packets.lock().unwrap()[0].1).unwrap().addr, "/after-the-secret");
        assert_eq!(refused(h), 0);
        stop(h);
    }

    // A connection that says nothing holds its slot until the deadline, and
    // no longer.
    #[test]
    fn a_connection_that_sends_nothing_is_closed_at_the_deadline() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let t0 = Instant::now();
        let mut c = connect_without_secret(port);
        assert!(closed_by_server(&mut c));
        assert!(t0.elapsed() >= AUTH_DEADLINE, "closed before the deadline: {:?}", t0.elapsed());
        assert!(wait_until(|| refused(h) == 1));
        assert!(cap.closed.lock().unwrap().is_empty());
        stop(h);
    }

    #[test]
    fn a_server_without_a_secret_does_not_start() {
        let cap = Cap::new();
        let ctx = &*cap as *const Cap as *mut c_void;
        let bind = "127.0.0.1";
        // SAFETY: a null secret is what this case passes; `bind` is a live str.
        let h = unsafe {
            clockwork_osc_tcp_start(ctx, on_packet, on_closed, std::ptr::null(), 0,
                                    bind.as_ptr(), bind.len() as u32)
        };
        assert!(h.is_null());
        #[cfg(unix)]
        {
            let path = std::env::temp_dir().join(format!("ss-no-secret-{}.sock", std::process::id()));
            let p = path.to_str().unwrap();
            // SAFETY: as above; `p` is a live str.
            let h = unsafe {
                clockwork_osc_uds_stream_start(ctx, on_packet, on_closed, std::ptr::null(),
                                               p.as_ptr(), p.len() as u32)
            };
            assert!(h.is_null());
            assert!(!path.exists(), "nothing bound");
        }
    }

    // One client: framed packet in → on_packet with a conn id >= 1; framed
    // reply out via clockwork_osc_stream_send reaches the client.
    #[test]
    fn tcp_round_trip() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let mut c = connect(port);

        let msg = encode("/clockwork/status", &[]);
        c.write_all(&frame(&msg)).unwrap();
        let ok = wait_until(|| !cap.packets.lock().unwrap().is_empty());
        assert!(ok, "framed packet should be delivered");
        let (conn, bytes) = cap.packets.lock().unwrap()[0].clone();
        assert!(conn >= 1, "conn ids start at 1 (0 = in-process)");
        assert_eq!(decode(&bytes).unwrap().addr, "/clockwork/status");

        let reply = encode("/clockwork/status", &[OscArg::Int(1)]);
        let sent = send(h, conn, &reply);
        assert_eq!(sent, 1);
        let body = read_frame(&mut c).expect("framed reply");
        assert_eq!(decode(&body).unwrap().addr, "/clockwork/status");

        stop(h);
    }

    // A frame split across writes (even mid-header) reassembles into one packet.
    #[test]
    fn tcp_split_frame_reassembles() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let mut c = connect(port);

        let msg = encode("/s_new", &[OscArg::Str("sine".into()), OscArg::Int(1001)]);
        let framed = frame(&msg);
        c.write_all(&framed[..3]).unwrap(); // mid-header
        std::thread::sleep(Duration::from_millis(150));
        c.write_all(&framed[3..10]).unwrap(); // mid-body
        std::thread::sleep(Duration::from_millis(150));
        c.write_all(&framed[10..]).unwrap();

        let ok = wait_until(|| {
            cap.packets.lock().unwrap().iter().any(|(_, b)| {
                decode(b).map(|m| m.addr == "/s_new").unwrap_or(false)
            })
        });
        assert!(ok, "split frame should reassemble into one packet");
        assert_eq!(cap.packets.lock().unwrap().len(), 1);

        stop(h);
    }

    // Two packets in one write both arrive (no lost second frame).
    #[test]
    fn tcp_coalesced_frames_both_arrive() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let mut c = connect(port);

        let mut both = frame(&encode("/one", &[]));
        both.extend_from_slice(&frame(&encode("/two", &[])));
        c.write_all(&both).unwrap();

        let ok = wait_until(|| cap.packets.lock().unwrap().len() == 2);
        assert!(ok, "both coalesced frames should arrive");
        let addrs: Vec<String> = cap.packets.lock().unwrap().iter()
            .map(|(_, b)| decode(b).unwrap().addr).collect();
        assert_eq!(addrs, vec!["/one", "/two"]);

        stop(h);
    }

    // One client. A second connection that presents the secret while one is
    // in is closed then, and the one in place keeps working.
    #[test]
    fn a_second_client_is_closed_while_one_is_in() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let mut c1 = connect(port);
        c1.write_all(&frame(&encode("/first", &[]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().len() == 1));

        let mut c2 = connect(port);
        let _ = c2.write_all(&frame(&encode("/second", &[])));
        assert!(closed_by_server(&mut c2));
        assert_eq!(refused(h), 0, "the second client did present the secret");

        c1.write_all(&frame(&encode("/still-first", &[]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().len() == 2));
        assert!(cap.packets.lock().unwrap().iter().all(|(id, _)| *id == 1));
        stop(h);
    }

    // A connection that has not presented the secret holds its slot for at
    // most AUTH_DEADLINE, and nothing displaces it before then: a newcomer,
    // the client with the secret included, is closed at accept and tries
    // again. That is chosen over evicting the silent connection, which would
    // let whoever connects most push anyone out.
    #[test]
    fn a_silent_connection_holds_the_door_for_the_deadline_and_no_longer() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let t0 = Instant::now();
        let mut silent = connect_without_secret(port);
        // One accept thread takes connections in the order they were made.
        let mut meanwhile = connect(port);
        assert!(closed_by_server(&mut meanwhile), "a connection made while one waits is closed at accept");
        assert!(t0.elapsed() < AUTH_DEADLINE, "closed at accept, not at the deadline");

        assert!(closed_by_server(&mut silent), "the silent connection is closed at the deadline");
        assert!(t0.elapsed() >= AUTH_DEADLINE);
        assert!(wait_until(|| refused(h) == 1));

        let mut c = connect(port);
        c.write_all(&frame(&encode("/in-after-the-silent", &[]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().len() == 1), "the client with the secret gets in");
        assert!(cap.closed.lock().unwrap().is_empty(), "the host heard of neither of the others");
        stop(h);
    }

    // Disconnect fires on_closed with the connection's id, and the id is
    // no longer sendable.
    #[test]
    fn tcp_disconnect_fires_on_closed() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);

        let mut c = connect(port);
        c.write_all(&frame(&encode("/hello", &[]))).unwrap();
        assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
        let conn = cap.packets.lock().unwrap()[0].0;

        drop(c); // client goes away
        assert!(wait_until(|| cap.closed.lock().unwrap().contains(&conn)),
                "on_closed should fire with the dropped connection's id");

        let reply = encode("/late", &[]);
        let sent = send(h, conn, &reply);
        assert_eq!(sent, 0, "a closed conn id must not be sendable");

        stop(h);
    }

    // A frame length beyond MAX_FRAME is a protocol violation: the connection
    // is closed and on_closed fires.
    #[test]
    fn tcp_oversize_frame_closes_connection() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);

        let mut c = connect(port);
        c.write_all(&frame(&encode("/id-probe", &[]))).unwrap();
        assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
        let conn = cap.packets.lock().unwrap()[0].0;

        c.write_all(&0x7FFF_FFFFu32.to_be_bytes()).unwrap();
        assert!(wait_until(|| cap.closed.lock().unwrap().contains(&conn)),
                "oversize frame should close the connection");
        let mut buf = [0u8; 1];
        let got = c.read(&mut buf);
        assert!(matches!(got, Ok(0)) || got.is_err());

        stop(h);
    }

    // Conn ids are unique across reconnects — never reused.
    #[test]
    fn tcp_conn_ids_never_reused() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);

        let mut seen = Vec::new();
        for _ in 0..3 {
            let mut c = connect(port);
            let before = cap.packets.lock().unwrap().len();
            c.write_all(&frame(&encode("/again", &[]))).unwrap();
            assert!(wait_until(|| cap.packets.lock().unwrap().len() > before));
            seen.push(cap.packets.lock().unwrap().last().unwrap().0);
            drop(c);
        }
        seen.dedup();
        assert_eq!(seen.len(), 3, "each connection gets a fresh id: {seen:?}");

        stop(h);
    }

    // A ConnReader replaying a scripted sequence of reads, so the reader loop
    // can be driven through error kinds a real socket won't produce on demand.
    // An exhausted script reads as EOF, which ends run_reader.
    struct ScriptedReader(VecDeque<std::io::Result<Vec<u8>>>);
    impl Read for ScriptedReader {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            match self.0.pop_front() {
                Some(Ok(bytes)) => {
                    buf[..bytes.len()].copy_from_slice(&bytes);
                    Ok(bytes.len())
                }
                Some(Err(e)) => Err(e),
                None => Ok(0), // EOF
            }
        }
    }
    struct NullWriter;
    impl ConnWriter for NullWriter {
        fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
            Ok(data.len())
        }
        fn set_write_timeout(&self, _: Duration) {}
        fn shutdown_both(&self) {}
    }
    impl ConnReader for ScriptedReader {
        fn set_timeout(&self, _: Duration) {}
        fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
            Ok(Box::new(NullWriter))
        }
    }

    fn empty_shared() -> Arc<Shared> {
        Arc::new(Shared {
            stop: AtomicBool::new(false),
            conns: Mutex::new(HashMap::new()),
            next_id: AtomicU32::new(1),
            readers: Mutex::new(Vec::new()),
            secret: test_secret().clone(),
            refused: AtomicU64::new(0),
        })
    }

    // A ConnWriter replaying scripted write outcomes, recording everything the
    // wire actually accepted so a test can prove a frame arrived intact.
    struct ScriptedWriter {
        steps: VecDeque<std::io::Result<usize>>, // Ok(n) = accept n bytes
        wrote: Arc<Mutex<Vec<u8>>>,
        shutdowns: Arc<AtomicU32>,
    }
    impl ConnWriter for ScriptedWriter {
        fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
            match self.steps.pop_front() {
                Some(Ok(n)) => {
                    let n = n.min(data.len());
                    self.wrote.lock().unwrap().extend_from_slice(&data[..n]);
                    Ok(n)
                }
                Some(Err(e)) => Err(e),
                None => {
                    self.wrote.lock().unwrap().extend_from_slice(data);
                    Ok(data.len())
                }
            }
        }
        fn set_write_timeout(&self, _: Duration) {}
        fn shutdown_both(&self) {
            self.shutdowns.fetch_add(1, Ordering::Relaxed);
        }
    }

    struct SendProbe {
        server: SocketServer,
        wrote: Arc<Mutex<Vec<u8>>>,
        shutdowns: Arc<AtomicU32>,
    }
    impl SendProbe {
        // A server holding exactly one connection (id 1) whose writer replays
        // `steps`, so send()'s eviction policy can be exercised without a
        // socket that would have to be coaxed into failing.
        fn new(steps: Vec<std::io::Result<usize>>) -> Self {
            let wrote = Arc::new(Mutex::new(Vec::new()));
            let shutdowns = Arc::new(AtomicU32::new(0));
            let w = ScriptedWriter {
                steps: VecDeque::from(steps),
                wrote: wrote.clone(),
                shutdowns: shutdowns.clone(),
            };
            let shared = empty_shared();
            shared.conns.lock().unwrap().insert(1, Conn {
                writer: Arc::new(Mutex::new(Box::new(w) as Box<dyn ConnWriter>)),
                open: true,
            });
            let server = SocketServer {
                shared,
                accept_join: None,
                port: 0,
                path: None,
            };
            SendProbe { server, wrote, shutdowns }
        }
        fn send(&self, osc: &[u8]) -> bool {
            self.server.send(1, osc)
        }
        fn still_registered(&self) -> bool {
            self.server.shared.conns.lock().unwrap().contains_key(&1)
        }
    }

    fn os_err(kind: ErrorKind) -> std::io::Result<usize> {
        Err(std::io::Error::from(kind))
    }

    // A transient kernel refusal (ENOBUFS and friends land in Other/Uncategorized)
    // is not a dead client: nothing of the frame went out, so the stream is still
    // correctly framed and the send can simply be retried.
    #[test]
    fn transient_write_error_keeps_the_connection() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Err(std::io::Error::other("ENOBUFS")),
            Ok(usize::MAX), // then the whole frame goes out
        ]);

        assert!(p.send(&msg), "a retryable write must still report success");
        assert!(p.still_registered(), "a healthy client must survive a transient");
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg),
                   "the frame must land exactly once, intact");
        assert_eq!(p.shutdowns.load(Ordering::Relaxed), 0);
    }

    // A transient part-way through a frame resumes at the right offset — the
    // bytes already accepted must not be resent, or the peer desyncs.
    #[test]
    fn transient_mid_frame_resumes_without_duplicating() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Ok(6), // partial
            Err(std::io::Error::other("ENOBUFS")),
            Ok(usize::MAX), // remainder
        ]);

        assert!(p.send(&msg));
        assert!(p.still_registered());
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg),
                   "resume must continue from the offset, not restart the frame");
    }

    // EINTR mid-write, as on the read side: a signal must not cost a frame
    // or a connection.
    #[test]
    fn interrupted_write_is_retried() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Ok(4),
            os_err(ErrorKind::Interrupted),
            Ok(usize::MAX),
        ]);

        assert!(p.send(&msg));
        assert!(p.still_registered());
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg));
    }

    // WouldBlock/TimedOut from a blocking socket means SO_SNDTIMEO expired —
    // a client that stopped reading for WRITE_TIMEOUT. That one must go, or it
    // wedges the gateway thread on every subsequent send.
    #[test]
    fn stuck_client_is_evicted() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        for kind in [ErrorKind::WouldBlock, ErrorKind::TimedOut] {
            let p = SendProbe::new(vec![os_err(kind)]);
            assert!(!p.send(&msg), "{kind:?}: send must report failure");
            assert!(!p.still_registered(), "{kind:?}: stuck client must be evicted");
            assert_eq!(p.shutdowns.load(Ordering::Relaxed), 1,
                       "{kind:?}: eviction must shut the socket down");
        }
    }

    // A dead peer, and a socket accepting nothing, are both terminal.
    #[test]
    fn broken_connection_is_evicted() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        for steps in [vec![os_err(ErrorKind::BrokenPipe)],
                      vec![os_err(ErrorKind::ConnectionReset)],
                      vec![Ok(0)]] {
            let p = SendProbe::new(steps);
            assert!(!p.send(&msg));
            assert!(!p.still_registered(), "a broken connection must be evicted");
        }
    }

    // A transient that never clears still ends the connection rather than
    // parking the gateway thread on one client indefinitely.
    #[test]
    fn endless_transient_gives_up() {
        let msg = encode("/clockwork/status", &[OscArg::Int(1)]);
        let steps = (0..WRITE_RETRIES + 1)
            .map(|_| Err(std::io::Error::other("ENOBUFS")))
            .collect();
        let p = SendProbe::new(steps);

        assert!(!p.send(&msg));
        assert!(!p.still_registered(), "a transient that never clears is terminal");
    }

    // EINTR is not a dead connection: std never retries an interrupted read
    // itself, so a signal landing on a reader thread must not cost the client
    // its connection.
    #[test]
    fn reader_survives_interrupted_read() {
        let cap = Cap::new();
        let host = Host {
            ctx: &*cap as *const Cap as *mut c_void,
            on_packet,
            on_closed,
        };
        let conn = ScriptedReader(VecDeque::from(vec![
            Ok(frame(&test_secret().auth_packet())),
            Err(std::io::Error::from(ErrorKind::Interrupted)),
            Ok(frame(&encode("/after-eintr", &[]))),
        ]));

        run_reader(conn, 7, empty_shared(), host); // returns at the script's EOF

        let packets = cap.packets.lock().unwrap();
        assert_eq!(packets.len(), 1,
                   "an interrupted read must not drop the connection");
        assert_eq!(decode(&packets[0].1).unwrap().addr, "/after-eintr");
    }

    #[test]
    fn stop_is_prompt() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap);
        let _c = connect(port); // an open connection must not delay stop
        let t0 = Instant::now();
        stop(h);
        // A stop that waited on something that never comes would never
        // return; any bound tells the two apart, and this one only that does.
        assert!(t0.elapsed() < Duration::from_secs(30), "stop should be prompt");
    }

    #[cfg(unix)]
    mod uds {
        use super::*;
        use std::os::unix::fs::PermissionsExt;
        use std::os::unix::net::UnixStream;

        fn tmp(name: &str) -> std::path::PathBuf {
            let dir = std::env::temp_dir()
                .join(format!("ss-stream-test-{}", std::process::id()));
            let _ = std::fs::create_dir_all(&dir);
            dir.join(name)
        }

        // UDS stream: framed round trip works, the socket file is 0600, a
        // stale file is replaced, and stop unlinks the path.
        #[test]
        fn uds_stream_round_trip_and_hygiene() {
            let cap = Cap::new();
            let ctx = &*cap as *const Cap as *mut c_void;
            let path = tmp("stream.sock");
            std::fs::write(&path, b"stale").unwrap();
            let p = path.to_str().unwrap();
            // SAFETY: `p` is a live str.
            let h = unsafe {
                clockwork_osc_uds_stream_start(ctx, on_packet, on_closed, test_secret(),
                                        p.as_ptr(), p.len() as u32)
            };
            assert!(!h.is_null(), "uds stream server should start over a stale file");
            let mode = std::fs::metadata(&path).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600, "socket file must be owner-only");

            let mut c = UnixStream::connect(&path).unwrap();
            c.set_read_timeout(Some(Duration::from_secs(30))).unwrap();
            c.write_all(&frame(&test_secret().auth_packet())).unwrap();
            c.write_all(&frame(&encode("/clockwork/status", &[]))).unwrap();
            assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
            let (conn, bytes) = cap.packets.lock().unwrap()[0].clone();
            assert_eq!(decode(&bytes).unwrap().addr, "/clockwork/status");

            let reply = encode("/clockwork/status", &[OscArg::Int(1)]);
            let sent = send(h, conn, &reply);
            assert_eq!(sent, 1);
            let body = read_frame(&mut c).expect("framed reply");
            assert_eq!(decode(&body).unwrap().addr, "/clockwork/status");

            stop(h);
            assert!(!path.exists(), "stop should unlink the socket path");
        }
    }
}
