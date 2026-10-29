// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! transport_probe — OSC client for the CI transport harness.
//!
//! Two modes over every platform/protocol:
//!
//!   transport_probe <proto> <target>              smoke: one /status, want a reply
//!   transport_probe load <proto> <target> <count> load: send <count> /clockwork/sync ids,
//!                                                 require every /clockwork/synced reply
//!
//!   proto ∈ tcp (host:port) | uds (path, unix) | pipe (name, windows) | shm (endpoint or port)
//!
//! Exit 0 = success; nonzero = transport error, timeout, or a failed load
//! invariant. Stream transports (tcp/uds/pipe) speak the shared 4-byte
//! big-endian length-prefixed framing, and first present the host's secret,
//! which the probe reads from its stdin as 64 hex digits and a newline, the
//! form the harness gives both processes.

use std::io::{Read, Write};
use std::sync::atomic::{AtomicI32, AtomicI64, Ordering};
use std::time::{Duration, Instant};

use clockwork_osc::{decode, encode, OscArg};

const TIMEOUT: Duration = Duration::from_secs(3);
// Load runs blast many messages through the real engine; give it wall-clock room.
const LOAD_TIMEOUT: Duration = Duration::from_secs(30);
// At most this many /clockwork/sync ids unanswered at once in a stream load.
// The engine takes what its command ring holds and drops the rest, counting
// them (engineMessagesDropped): a client that sends more than it can take is
// in error, and this probe measures transports, not the engine's limits. All
// 5000 at once filled the ring on a loaded runner and lost commands.
const STREAM_WINDOW: i64 = 256;

fn frame(pkt: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + pkt.len());
    out.extend_from_slice(&(pkt.len() as u32).to_be_bytes());
    out.extend_from_slice(pkt);
    out
}

fn read_frame(r: &mut impl Read) -> std::io::Result<Vec<u8>> {
    let mut hdr = [0u8; 4];
    r.read_exact(&mut hdr)?;
    let len = u32::from_be_bytes(hdr) as usize;
    let mut body = vec![0u8; len];
    r.read_exact(&mut body)?;
    Ok(body)
}

fn report(reply: &[u8]) {
    match decode(reply) {
        Some(m) => println!("OK {} ({} bytes)", m.addr, reply.len()),
        None => println!("OK <undecodable> ({} bytes)", reply.len()),
    }
}

fn run(proto: &str, target: &str, secret: &clockwork_comms::secret::ClockworkCommsSecret, probe: &[u8]) -> Result<Vec<u8>, String> {
    let auth = frame(&secret.auth_packet());
    let auth = auth.as_slice();
    match proto {
        "tcp" => {
            let mut s = std::net::TcpStream::connect(target).map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            s.set_write_timeout(Some(TIMEOUT)).ok();
            s.write_all(auth).map_err(|e| format!("send: {e}"))?;
            s.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            read_frame(&mut s).map_err(|e| format!("recv: {e}"))
        }
        #[cfg(unix)]
        "uds" => {
            let mut s = std::os::unix::net::UnixStream::connect(target)
                .map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            s.set_write_timeout(Some(TIMEOUT)).ok();
            s.write_all(auth).map_err(|e| format!("send: {e}"))?;
            s.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            read_frame(&mut s).map_err(|e| format!("recv: {e}"))
        }
        #[cfg(windows)]
        "pipe" => {
            // A named-pipe client is just a file open on \\.\pipe\<name>.
            let full = if target.starts_with(r"\\.\pipe\") {
                target.to_string()
            } else {
                format!(r"\\.\pipe\{target}")
            };
            let mut f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&full)
                .map_err(|e| format!("open {full}: {e}"))?;
            f.write_all(auth).map_err(|e| format!("send: {e}"))?;
            f.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            // A pipe File has no read timeout; the wall-clock cap in main()
            // bounds this read so a silent server can't hang the probe.
            read_frame(&mut f).map_err(|e| format!("recv: {e}"))
        }
        "shm" => {
            // SHM peer plane: `target` is the engine's attach endpoint, or its
            // port (then the default endpoint for it). Attach, write the probe
            // into the command ring, and read the first reply back off the
            // reply ring.
            let endpoint = match target.parse::<u32>() {
                Ok(port) => clockwork_comms::shm::default_endpoint(port),
                Err(_) => target.to_string(),
            };
            let peer = clockwork_comms::shm::ShmPeer::open(&endpoint, secret)?;
            peer.attach(std::process::id());
            if !peer.write_cmd(probe) {
                return Err("command ring full".into());
            }
            let deadline = Instant::now() + TIMEOUT;
            loop {
                let mut reply = None;
                peer.drain_replies(|p| {
                    if reply.is_none() {
                        reply = Some(p.to_vec());
                    }
                });
                if let Some(r) = reply {
                    return Ok(r);
                }
                if Instant::now() >= deadline {
                    return Err("no reply".into());
                }
                std::thread::sleep(Duration::from_millis(5));
            }
        }
        other => Err(format!("unsupported protocol on this platform: {other}")),
    }
}

// ── load mode ────────────────────────────────────────────────────────────────

// How far a stream load got: ids written, replies read and the last of them.
// What a load that stalls says it was doing, wherever it stalled — in a write
// the server stopped taking, or in a read for replies that never came.
static SENT: AtomicI64 = AtomicI64::new(0);
static GOT: AtomicI64 = AtomicI64::new(0);
static LAST_ID: AtomicI32 = AtomicI32::new(0);

fn progress() -> String {
    format!(
        "{} sent, {} replies, last reply id {}",
        SENT.load(Ordering::Relaxed),
        GOT.load(Ordering::Relaxed),
        LAST_ID.load(Ordering::Relaxed)
    )
}

fn sync_msg(id: i32) -> Vec<u8> {
    encode("/clockwork/sync", &[OscArg::Int(id)])
}

/// The id of a `/clockwork/synced` reply, if that's what the packet is.
fn synced_id(pkt: &[u8]) -> Option<i32> {
    let m = decode(pkt)?;
    if m.addr != "/clockwork/synced" {
        return None;
    }
    m.args.first().and_then(|a| a.as_i32())
}

/// A bidirectional stream we can duplicate so a reader thread drains replies
/// while the main thread keeps writing (else the server's send buffer fills and
/// it drops us).
trait CloneStream: Read + Write + Send + Sized + 'static {
    fn clone_stream(&self) -> std::io::Result<Self>;
}
impl CloneStream for std::net::TcpStream {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}
#[cfg(unix)]
impl CloneStream for std::os::unix::net::UnixStream {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}
#[cfg(windows)]
impl CloneStream for std::fs::File {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}

/// Send `count` /clockwork/sync ids down a reliable stream, at most STREAM_WINDOW
/// unanswered at once, while a reader thread collects the /clockwork/synced
/// replies; every id must come back exactly once, in order.
fn stream_load<S: CloneStream>(mut writer: S, count: i32) -> Result<String, String> {
    let reader_conn = writer.clone_stream().map_err(|e| format!("clone: {e}"))?;
    let t0 = Instant::now();
    let reader = std::thread::spawn(move || -> Result<i64, String> {
        let mut r = reader_conn;
        let (mut got, mut last) = (0i64, 0i32);
        while got < count as i64 {
            let body = read_frame(&mut r).map_err(|e| format!("recv: {e} ({})", progress()))?;
            if let Some(id) = synced_id(&body) {
                if id <= last {
                    return Err(format!("out-of-order reply: {id} after {last}"));
                }
                last = id;
                got += 1;
                GOT.store(got, Ordering::Relaxed);
                LAST_ID.store(id, Ordering::Relaxed);
            }
        }
        Ok(got)
    });
    // Capture a write failure rather than early-returning, so the reader thread
    // is always joined (a broken connection makes its read fail promptly too).
    let mut write_err = None;
    for id in 1..=count {
        // Wait for room in the window, or for the reader to have given up.
        while id as i64 - GOT.load(Ordering::Relaxed) > STREAM_WINDOW && !reader.is_finished() {
            std::thread::sleep(Duration::from_micros(100));
        }
        if let Err(e) = writer.write_all(&frame(&sync_msg(id))) {
            write_err = Some(format!("send {id}: {e}"));
            break;
        }
        SENT.store(id as i64, Ordering::Relaxed);
    }
    writer.flush().ok();
    let reader_res = reader.join().map_err(|_| "reader panicked".to_string())?;
    if let Some(e) = write_err {
        return Err(e);
    }
    let got = reader_res?;
    if got != count as i64 {
        return Err(format!("only {got}/{count} replies ({})", progress()));
    }
    let secs = t0.elapsed().as_secs_f64();
    Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
}

/// Windowed single-threaded load for Windows named pipes. stream_load's
/// cloned-handle reader thread can't be used here: a synchronous pipe handle
/// serializes I/O on its file object, so once the writer blocks inside
/// WriteFile (server-inbound buffer full) the reader can no longer issue
/// ReadFile, and with the server's reply write blocked against a full
/// client-inbound buffer the four parties deadlock. Bounding the ids in
/// flight keeps both 64KB pipe buffers nearly empty, so no write can block.
#[cfg(windows)]
fn pipe_load(mut f: std::fs::File, count: i32) -> Result<String, String> {
    // ~24-byte frames each way: 512 in flight ≈ 12KB per direction, a 5x
    // margin under the 64KB pipe buffers that must never fill.
    const WINDOW: i32 = 512;
    let t0 = Instant::now();
    let (mut sent, mut got, mut last) = (0i32, 0i64, 0i32);
    while got < count as i64 {
        while sent < count && sent - (got as i32) < WINDOW {
            sent += 1;
            f.write_all(&frame(&sync_msg(sent)))
                .map_err(|e| format!("send {sent}: {e}"))?;
        }
        let body = read_frame(&mut f).map_err(|e| format!("recv after {got}: {e}"))?;
        if let Some(id) = synced_id(&body) {
            if id <= last {
                return Err(format!("out-of-order reply: {id} after {last}"));
            }
            last = id;
            got += 1;
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
}

fn load(proto: &str, target: &str, secret: &clockwork_comms::secret::ClockworkCommsSecret, count: i32) -> Result<String, String> {
    let auth = frame(&secret.auth_packet());
    let auth = auth.as_slice();
    match proto {
        "tcp" => {
            let mut s = std::net::TcpStream::connect(target).map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(LOAD_TIMEOUT)).ok();
            s.write_all(auth).map_err(|e| format!("send: {e}"))?;
            stream_load(s, count)
        }
        #[cfg(unix)]
        "uds" => {
            let mut s = std::os::unix::net::UnixStream::connect(target)
                .map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(LOAD_TIMEOUT)).ok();
            s.write_all(auth).map_err(|e| format!("send: {e}"))?;
            stream_load(s, count)
        }
        #[cfg(windows)]
        "pipe" => {
            let full = if target.starts_with(r"\\.\pipe\") {
                target.to_string()
            } else {
                format!(r"\\.\pipe\{target}")
            };
            let mut f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&full)
                .map_err(|e| format!("open {full}: {e}"))?;
            f.write_all(auth).map_err(|e| format!("send: {e}"))?;
            pipe_load(f, count)
        }
        "shm" => {
            // Peer-plane load: keep the command ring full (it backpressures
            // losslessly) while continuously draining the reply ring, exactly as
            // a real peer would — so nothing is dropped and /clockwork/synced ids arrive in
            // order. `target` is the engine's attach endpoint, or its port.
            let endpoint = match target.parse::<u32>() {
                Ok(port) => clockwork_comms::shm::default_endpoint(port),
                Err(_) => target.to_string(),
            };
            let peer = clockwork_comms::shm::ShmPeer::open(&endpoint, secret)?;
            peer.attach(std::process::id());
            let t0 = Instant::now();
            let (mut sent, mut got, mut last, mut disorder) = (0i32, 0i64, 0i32, false);
            let deadline = Instant::now() + LOAD_TIMEOUT;
            while got < count as i64 {
                while sent < count && peer.write_cmd(&sync_msg(sent + 1)) {
                    sent += 1;
                }
                peer.drain_replies(|p| {
                    if let Some(id) = synced_id(p) {
                        if id <= last {
                            disorder = true;
                        }
                        last = id;
                        got += 1;
                    }
                });
                if disorder {
                    return Err(format!("out-of-order reply near {last}"));
                }
                if Instant::now() >= deadline {
                    return Err(format!("only {got}/{count} replies"));
                }
            }
            let dropped = peer.replies_dropped();
            if dropped != 0 {
                return Err(format!("{dropped} replies dropped (peer fell behind)"));
            }
            let secs = t0.elapsed().as_secs_f64();
            Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
        }
        other => Err(format!("unsupported protocol on this platform: {other}")),
    }
}

/// The host's secret, from stdin as 64 hex digits and a newline. Exits
/// without one: every protocol asks for it.
fn secret_from_stdin(proto: &str) -> clockwork_comms::secret::ClockworkCommsSecret {
    let mut line = String::new();
    let _ = std::io::stdin().read_line(&mut line);
    let hex = line.trim_end();
    let bytes: Option<Vec<u8>> = (hex.len() == 64)
        .then(|| (0..64).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).ok()).collect())
        .flatten();
    let secret = bytes
        .and_then(|b| <[u8; 32]>::try_from(b).ok())
        .and_then(clockwork_comms::secret::ClockworkCommsSecret::from_bytes);
    match secret {
        Some(secret) => secret,
        None => {
            eprintln!("{proto}: the host's secret is read from stdin: 64 hex digits and a newline");
            std::process::exit(2);
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    // Load mode: `transport_probe load <proto> <target> <count>`.
    if args.len() == 5 && args[1] == "load" {
        let (proto, target) = (args[2].clone(), args[3].clone());
        let count: i32 = args[4].parse().unwrap_or(0);
        if count <= 0 {
            eprintln!("load: <count> must be a positive integer");
            std::process::exit(2);
        }
        let secret = secret_from_stdin(&proto);
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(load(&proto, &target, &secret, count));
        });
        match rx.recv_timeout(LOAD_TIMEOUT + Duration::from_secs(2)) {
            Ok(Ok(summary)) => println!("LOAD OK {} {}: {summary}", args[2], args[3]),
            Ok(Err(e)) => {
                eprintln!("LOAD FAIL {} {}: {e}", args[2], args[3]);
                std::process::exit(1);
            }
            Err(_) => {
                eprintln!("LOAD FAIL {} {}: timed out ({})", args[2], args[3], progress());
                std::process::exit(1);
            }
        }
        return;
    }

    if args.len() != 3 {
        eprintln!("usage: transport_probe <tcp|uds|pipe|shm> <target>");
        eprintln!("       transport_probe load <proto> <target> <count>");
        std::process::exit(2);
    }
    let (proto, target) = (args[1].clone(), args[2].clone());

    // Hard wall-clock cap on the whole probe, enforced here rather than relying
    // on per-transport socket timeouts: a Windows named-pipe File read has no
    // read timeout, so a server that accepts the pipe but never replies would
    // otherwise hang forever (and hang clockwork, which runs us with no outer
    // timeout of its own). Run the probe on a worker and give up if it stalls.
    let secret = secret_from_stdin(&proto);
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let probe = encode("/clockwork/status", &[]);
        let _ = tx.send(run(&proto, &target, &secret, &probe));
    });
    match rx.recv_timeout(TIMEOUT + Duration::from_secs(1)) {
        Ok(Ok(reply)) => report(&reply),
        Ok(Err(e)) => {
            eprintln!("FAIL {} {}: {e}", args[1], args[2]);
            std::process::exit(1);
        }
        Err(_) => {
            eprintln!("FAIL {} {}: timed out", args[1], args[2]);
            std::process::exit(1);
        }
    }
}
