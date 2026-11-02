// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The cue port's receive threads block until a datagram arrives or the
//! server stops, and wake for nothing else.
//!
//! This file is its own test binary with one case, because wakes are counted
//! by thread name and a second cue server in the same process would share it.
//! Wake counts are read on Linux only; the cue and destroy checks run
//! everywhere.

use std::ffi::c_void;
use std::net::UdpSocket;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use clockwork_osc::{decode, encode};
use clockwork_osc_net::ffi::{
    clockwork_osc_configure, clockwork_osc_create, clockwork_osc_destroy,
};
use clockwork_wait::testing;

static CUES: Mutex<Vec<String>> = Mutex::new(Vec::new());

extern "C" fn on_emit(_: *mut c_void, _: i32, osc: *const u8, len: u32) {
    // SAFETY: `osc` is readable for `len` bytes for the callback's duration.
    let bytes = unsafe { std::slice::from_raw_parts(osc, len as usize) };
    if let Some(m) = decode(bytes) {
        CUES.lock().unwrap().push(m.addr);
    }
}
extern "C" fn on_dropped(_: *mut c_void, _: u32) {}

#[test]
fn the_cue_threads_sleep_until_a_datagram_and_destroy_ends_them_at_once() {
    let osc = clockwork_osc_create(std::ptr::null_mut(), on_emit, on_dropped);
    assert!(!osc.is_null());
    let port = UdpSocket::bind(("127.0.0.1", 0)).unwrap().local_addr().unwrap().port();
    // SAFETY: a live handle.
    unsafe { clockwork_osc_configure(osc, i32::from(port), 1, 1) };

    if testing::countable() {
        // A 100 ms receive timeout would wake 5 times in 500 ms.
        assert!(testing::over("cw-osc-cue4", Duration::from_millis(500)).unwrap() <= 1);
    }

    let sender = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
    sender.send_to(&encode("/hello", &[]), ("127.0.0.1", port)).unwrap();
    let until = Instant::now() + Duration::from_secs(30);
    while CUES.lock().unwrap().is_empty() && Instant::now() < until {
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(CUES.lock().unwrap().as_slice(), ["/external-osc-cue"]);

    let t0 = Instant::now();
    // SAFETY: a live handle, destroyed once.
    unsafe { clockwork_osc_destroy(osc) };
    assert!(t0.elapsed() < Duration::from_secs(1), "destroy waited for a receive");
}
