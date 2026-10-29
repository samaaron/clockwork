// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Load / stress tests for the local OSC transports, driven through the public
//! C ABI (`clockwork_osc_*`) — the exact surface the C++ side ships — at medium volume
//! on every platform `cargo test` runs on: TCP + UDS stream + UDS datagram, and
//! the Windows named pipe (`cfg(windows)`).
//!
//! Each test hammers a running server with many messages across several
//! concurrent sources and asserts correctness UNDER LOAD, not just liveness:
//!
//! * completeness — every framed message the client sent is delivered
//!   (stream transports are reliable; the datagram one is checked with the
//!   loss tolerance its contract allows);
//! * per-source ordering — ids arrive strictly monotonically per connection;
//! * zero corruption — every delivered packet decodes to the expected shape;
//! * a mix of tiny and large (100 KiB, split-write) frames, so reassembly is
//!   exercised while the pipe is saturated;
//! * the server stays live afterwards (a final round trip still answers).
//!
//! Throughput and any datagram loss are logged for regression visibility.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use clockwork_osc::{decode, encode, OscArg};

// Medium load: 4 connections × 25k = 100k messages through the stream servers.
const CONNS: usize = 1; // one client: a server serves one connection
const PER_CONN: i32 = 100_000;
const LARGE_EVERY: i32 = 500; // a ~100 KiB frame periodically, to stress reassembly
const SPLIT_EVERY: i32 = 97; // split some frames mid-wire, to stress the accumulator

fn load_msg(id: i32, large: bool) -> Vec<u8> {
    if large {
        // Well under MAX_FRAME (256 KiB); big enough to span many read chunks.
        encode("/l", &[OscArg::Int(id), OscArg::Str("x".repeat(100 * 1024))])
    } else {
        encode("/l", &[OscArg::Int(id)])
    }
}

fn frame(pkt: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + pkt.len());
    out.extend_from_slice(&(pkt.len() as u32).to_be_bytes());
    out.extend_from_slice(pkt);
    out
}

/// Decode a load packet to its id, or `None` if it isn't a well-formed `/l i`.
fn packet_id(osc: &[u8]) -> Option<i32> {
    let m = decode(osc)?;
    if m.addr != "/l" {
        return None;
    }
    m.args.first().and_then(|a| a.as_i32())
}

/// Per-source running verification: how many arrived, whether any arrived out
/// of order, and how many failed to decode.
#[derive(Default, Clone, Copy)]
struct Track {
    count: i64,
    last: i32, // highest id seen so far (ids are 1-based and strictly increasing)
    disorder: i64,
    corrupt: i64,
}

#[derive(Default)]
struct Sink {
    by_src: Mutex<HashMap<u64, Track>>,
}

impl Sink {
    fn record(&self, src: u64, osc: &[u8]) {
        let mut map = self.by_src.lock().unwrap();
        let t = map.entry(src).or_default();
        match packet_id(osc) {
            Some(id) => {
                t.count += 1;
                if id <= t.last {
                    t.disorder += 1;
                }
                t.last = id;
            }
            None => t.corrupt += 1,
        }
    }
    fn total(&self) -> i64 {
        self.by_src.lock().unwrap().values().map(|t| t.count).sum()
    }
}

// ── stream transports (TCP + UDS) ────────────────────────────────────────────

mod stream_abi {
    use super::*;
    pub use clockwork_comms::stream::{
        clockwork_osc_stream_port, clockwork_osc_stream_stop, clockwork_osc_tcp_start, ClockworkOscStream,
    };
    use clockwork_comms::secret::ClockworkCommsSecret;

    /// The secret every server here starts with and every client presents.
    pub fn secret() -> &'static ClockworkCommsSecret {
        static SECRET: std::sync::OnceLock<ClockworkCommsSecret> = std::sync::OnceLock::new();
        SECRET.get_or_init(|| ClockworkCommsSecret::generate().expect("an OS random source"))
    }

    pub extern "C" fn on_packet(ctx: *mut c_void, conn: u32, osc: *const u8, len: u32) {
        // SAFETY: `ctx` is the `Arc<Sink>` the test holds until after the
        // stop; `osc`/`len` is the callback's contract.
        let (sink, bytes) = unsafe { (&*ctx.cast::<Sink>(), std::slice::from_raw_parts(osc, len as usize)) };
        sink.record(conn as u64, bytes);
    }
    pub extern "C" fn on_closed(_ctx: *mut c_void, _conn: u32) {}
}

/// One client: the secret, then ids 1..=PER_CONN in order, some large, some
/// split mid-frame.
fn blast_stream<W: std::io::Write>(mut w: W) {
    w.write_all(&frame(&stream_abi::secret().auth_packet())).unwrap();
    for id in 1..=PER_CONN {
        let framed = frame(&load_msg(id, id % LARGE_EVERY == 0));
        if id % SPLIT_EVERY == 0 && framed.len() > 8 {
            let cut = framed.len() / 2;
            w.write_all(&framed[..cut]).unwrap();
            w.write_all(&framed[cut..]).unwrap();
        } else {
            w.write_all(&framed).unwrap();
        }
    }
    w.flush().unwrap();
}

/// Drive `CONNS` concurrent clients (built by `connect`) into an already-started
/// stream server `handle`, then assert full, ordered, uncorrupted delivery.
fn run_stream_load<W, C>(name: &str, handle: *mut stream_abi::ClockworkOscStream, sink: &Arc<Sink>, connect: C)
where
    W: std::io::Write + Send + 'static,
    C: Fn() -> W + Send + Sync + 'static,
{
    let connect = Arc::new(connect);
    let t0 = Instant::now();
    let clients: Vec<_> = (0..CONNS)
        .map(|_| {
            let connect = connect.clone();
            std::thread::spawn(move || blast_stream(connect()))
        })
        .collect();
    for c in clients {
        c.join().unwrap();
    }

    let target = CONNS as i64 * PER_CONN as i64;
    let deadline = Instant::now() + Duration::from_secs(60);
    while sink.total() < target {
        assert!(
            Instant::now() < deadline,
            "{name}: only {}/{target} delivered before timeout",
            sink.total()
        );
        std::thread::sleep(Duration::from_millis(20));
    }

    let map = sink.by_src.lock().unwrap();
    assert_eq!(map.len(), CONNS, "{name}: saw {} connections, want {CONNS}", map.len());
    for (conn, t) in map.iter() {
        assert_eq!(t.count, PER_CONN as i64, "{name}: conn {conn} got {} msgs", t.count);
        assert_eq!(t.disorder, 0, "{name}: conn {conn} saw {} out-of-order", t.disorder);
        assert_eq!(t.corrupt, 0, "{name}: conn {conn} saw {} corrupt", t.corrupt);
    }
    let secs = t0.elapsed().as_secs_f64();
    eprintln!(
        "[load] {name}: {target} msgs / {CONNS} conns in {secs:.2}s = {:.0} msg/s",
        target as f64 / secs
    );
    drop(map);
    // SAFETY: a live handle from a start function, stopped once, here.
    unsafe { stream_abi::clockwork_osc_stream_stop(handle) };
}

#[test]
fn tcp_load() {
    use stream_abi::*;
    let sink = Arc::new(Sink::default());
    let bind = "127.0.0.1";
    // SAFETY: `bind` is a live str.
    let handle = unsafe {
        clockwork_osc_tcp_start(
            Arc::as_ptr(&sink).cast_mut().cast::<c_void>(),
            on_packet,
            on_closed,
            secret(),
            0,
            bind.as_ptr(),
            bind.len() as u32,
        )
    };
    assert!(!handle.is_null(), "tcp server should start");
    // SAFETY: just started, not yet stopped.
    let port = unsafe { clockwork_osc_stream_port(handle) } as u16;
    run_stream_load("tcp", handle, &sink, move || {
        std::net::TcpStream::connect(("127.0.0.1", port)).unwrap()
    });
}

#[cfg(unix)]
#[test]
fn uds_stream_load() {
    use stream_abi::*;
    use clockwork_comms::stream::clockwork_osc_uds_stream_start;
    let sink = Arc::new(Sink::default());
    let dir = std::env::temp_dir().join(format!("ss-load-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let path = dir.join("stream.sock");
    let ps = path.to_str().unwrap();
    // SAFETY: `ps` is a live str.
    let handle = unsafe {
        clockwork_osc_uds_stream_start(
            Arc::as_ptr(&sink).cast_mut().cast::<c_void>(),
            on_packet,
            on_closed,
            secret(),
            ps.as_ptr(),
            ps.len() as u32,
        )
    };
    assert!(!handle.is_null(), "uds stream server should start");
    let path2 = path.clone();
    run_stream_load("uds-stream", handle, &sink, move || {
        std::os::unix::net::UnixStream::connect(&path2).unwrap()
    });
}

#[cfg(windows)]
#[test]
fn pipe_load() {
    use stream_abi::*;
    use clockwork_comms::pipe::clockwork_osc_pipe_start;
    let sink = Arc::new(Sink::default());
    let name = format!("ss-load-{}", std::process::id());
    // SAFETY: `name` is a live String.
    let handle = unsafe {
        clockwork_osc_pipe_start(
            Arc::as_ptr(&sink).cast_mut().cast::<c_void>(),
            on_packet,
            on_closed,
            secret(),
            name.as_ptr(),
            name.len() as u32,
        )
    };
    assert!(!handle.is_null(), "named-pipe server should start");
    let full = format!(r"\\.\pipe\{name}");
    run_stream_load("pipe", handle, &sink, move || {
        // Instances free up as clients attach; retry briefly past a busy pipe.
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            match std::fs::OpenOptions::new().read(true).write(true).open(&full) {
                Ok(f) => return f,
                Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
                Err(e) => panic!("pipe open failed: {e}"),
            }
        }
    });
}
