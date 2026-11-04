// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The stream server's accept and reader threads block until a connection
//! arrives, a client sends, or the server stops, and wake for nothing else.
//!
//! This file is its own test binary with one case, because wakes are counted
//! by thread name and a second server in the same process would share it.
//! Wake counts are read on Linux only; the stop check runs everywhere.

use std::ffi::c_void;
use std::io::Write;
use std::net::TcpStream;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use clockwork_comms::secret::ClockworkCommsSecret;
use clockwork_comms::stream::{
    clockwork_osc_stream_port, clockwork_osc_stream_stop, clockwork_osc_tcp_start,
};
use clockwork_osc::encode;
use clockwork_wait::testing;

static PACKETS: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());

extern "C" fn on_packet(_: *mut c_void, _: u32, osc: *const u8, len: u32) {
    // SAFETY: `osc` is readable for `len` bytes for the callback's duration.
    let bytes = unsafe { std::slice::from_raw_parts(osc, len as usize) };
    PACKETS.lock().unwrap().push(bytes.to_vec());
}
extern "C" fn on_closed(_: *mut c_void, _: u32) {}

fn frame(pkt: &[u8]) -> Vec<u8> {
    let mut out = (pkt.len() as u32).to_be_bytes().to_vec();
    out.extend_from_slice(pkt);
    out
}

#[test]
fn the_accept_thread_and_a_quiet_reader_sleep_and_stopping_ends_both_at_once() {
    let secret = ClockworkCommsSecret::from_bytes(std::array::from_fn(|i| i as u8 + 7)).unwrap();
    let bind = "127.0.0.1";
    // SAFETY: the secret and the address are live for the call; the callbacks
    // are plain functions that outlive the server.
    let h = unsafe {
        clockwork_osc_tcp_start(std::ptr::null_mut(), std::ptr::null_mut(), on_packet, on_closed, &secret, 0,
                                bind.as_ptr(), bind.len() as u32)
    };
    assert!(!h.is_null());
    // SAFETY: a live server.
    let port = unsafe { clockwork_osc_stream_port(h) } as u16;

    if testing::countable() {
        // Polling for a connection every 20 ms would wake 25 times in 500 ms.
        assert!(testing::over("cw-osc-accept", Duration::from_millis(500)).unwrap() <= 1);
    }

    let mut c = TcpStream::connect(("127.0.0.1", port)).unwrap();
    c.write_all(&frame(&secret.auth_packet())).unwrap();
    c.write_all(&frame(&encode("/hello", &[]))).unwrap();
    let until = Instant::now() + Duration::from_secs(30);
    while PACKETS.lock().unwrap().is_empty() && Instant::now() < until {
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(PACKETS.lock().unwrap().len(), 1);

    if testing::countable() {
        // A 100 ms read timeout would wake 5 times in 500 ms.
        assert!(testing::over("cw-osc-stream", Duration::from_millis(500)).unwrap() <= 1);
    }

    // The client is authenticated and quiet, so its reader is blocked with no
    // timeout. Stopping the server must end it at once.
    let t0 = Instant::now();
    // SAFETY: a live server, stopped once.
    unsafe { clockwork_osc_stream_stop(h) };
    assert!(t0.elapsed() < Duration::from_secs(1), "stopping the server waited for a reader");
}
