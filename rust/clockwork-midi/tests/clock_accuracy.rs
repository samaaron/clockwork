// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! End-to-end accuracy of the MIDI subsystem over *real* system MIDI (CoreMIDI/
//! ALSA virtual ports), driving the C ABI exactly as the engine does.
//!
//!   * incoming clock — a precise virtual clock master, BPM estimate checked
//!   * note round-trip — /clockwork/midi/out → device and device → /clockwork/midi/in
//!
//! (The midi_clock_beat burst lives engine-side in C++ MidiClockOut, covered by
//! the native MidiClockOut tests.)
//!
//! Ignored by default (needs virtual MIDI + real time). Run on a dev box with:
//!     cargo test --test clock_accuracy -- --ignored --nocapture
// Virtual MIDI ports are a CoreMIDI and ALSA facility: Windows has none, so
// there is nothing for this test to loop through there.
#![cfg(all(not(target_arch = "wasm32"), unix))]

use clockwork_osc::clockwork_sys;
use std::ffi::c_void;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use midir::os::unix::{VirtualInput, VirtualOutput};
use midir::{MidiInput, MidiOutput};

use clockwork_midi::ffi::{clockwork_midi_create, clockwork_midi_destroy, clockwork_midi_handle_osc, ClockworkMidi};
use clockwork_midi::osc::{encode, OscArg};

/// Host context shared with the C ABI callbacks: captures the per-0xF8 (port, ts)
/// stream and the OSC packets the subsystem emits.
struct Ctx {
    clocks: Mutex<Vec<(String, u64)>>,   // (port, ts_us) per 0xF8
    events: Mutex<Vec<Vec<u8>>>,
}

/// # Safety
/// `port` is readable for `len` bytes.
unsafe fn port_str(port: *const u8, len: u32) -> String {
    // SAFETY: per the contract.
    String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(port, len as usize) }).into_owned()
}

extern "C" fn emit_cb(ctx: *mut c_void, _kind: i32, osc: *const u8, len: u32) {
    // SAFETY: `ctx` is the leaked `Ctx` from `new_ctx`; `osc`/`len` is the
    // callback's contract.
    let (c, bytes) = unsafe { (&*ctx.cast::<Ctx>(), std::slice::from_raw_parts(osc, len as usize)) };
    c.events.lock().unwrap().push(bytes.to_vec());
}
extern "C" fn clock_cb(ctx: *mut c_void, norm: *const u8, norm_len: u32,
                       _raw: *const u8, _raw_len: u32, ts_us: u64) {
    // SAFETY: as `emit_cb`; the name is the callback's (pointer, length).
    let (c, p) = unsafe { (&*ctx.cast::<Ctx>(), port_str(norm, norm_len)) };
    c.clocks.lock().unwrap().push((p, ts_us));
}
extern "C" fn transport_cb(_ctx: *mut c_void, _norm: *const u8, _norm_len: u32,
                           _raw: *const u8, _raw_len: u32, _kind: i32, _beat: f64) {}

fn new_ctx() -> *mut Ctx {
    Box::into_raw(Box::new(Ctx {
        clocks: Mutex::new(Vec::new()),
        events: Mutex::new(Vec::new()),
    }))
}

fn create(ctx: *mut Ctx) -> *mut ClockworkMidi {
    // SAFETY: a null app name is allowed by the contract; the callbacks and
    // ctx outlive the subsystem, which the test closes before returning.
    unsafe { clockwork_midi_create(ctx.cast::<c_void>(), emit_cb, clock_cb, transport_cb, std::ptr::null()) }
}

/// Resolve a virtual port's bare name to the handle the subsystem enumerates.
///
/// That handle is the NORMALISED, dedup'd form of the full OS port name, not
/// the name the virtual port was created with: on ALSA it carries the owning
/// client too, on CoreMIDI it does not. These cases hardcoded the bare name, so
/// they could only ever pass on macOS — and being `#[ignore]`d, nothing noticed
/// until the box they could not run on got a MIDI device. tests/
/// virtual_loopback.rs always did it this way; this file did not.
///
/// Must be called AFTER the virtual port exists and after `create()`, which is
/// what publishes the shared `MidiIo` this reads.
fn resolve(substr: &str, input: bool) -> String {
    let io = clockwork_midi::device::shared()
        .expect("clockwork_midi_create publishes the shared MidiIo");
    let mut guard = io.lock().unwrap();
    guard.refresh();
    let (ins, outs) = guard.port_lists();
    let list = if input { ins } else { outs };
    list.iter()
        .find(|(n, _)| n.contains(substr))
        .map(|(n, _)| n.clone())
        .unwrap_or_else(|| {
            panic!("virtual {} port {substr} not enumerated",
                   if input { "input" } else { "output" })
        })
}

fn send(h: *mut ClockworkMidi, addr: &str, args: &[OscArg]) {
    let bytes = encode(addr, args);
    // SAFETY: `h` is live and `bytes` is a live Vec.
    unsafe { clockwork_midi_handle_osc(h, bytes.as_ptr(), bytes.len() as u32) };
}

fn mean_std(xs: &[f64]) -> (f64, f64) {
    let mean = xs.iter().sum::<f64>() / xs.len() as f64;
    let var = xs.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / xs.len() as f64;
    (mean, var.sqrt())
}

#[test]
#[ignore = "needs virtual MIDI + real time; run with --ignored --nocapture"]
fn incoming_clock_delivery() {
    // The native path forwards every 0xF8 to ClockworkClock (which computes tempo/beat
    // engine-side). Verify the real midir path delivers every pulse on the right
    // port with low inter-arrival jitter.
    let bpm = 120.0;
    let interval = Duration::from_secs_f64(60.0 / bpm / 24.0);
    let vout = MidiOutput::new("ss-test").unwrap();
    let mut conn = vout.create_virtual("ss-clk-in").unwrap();

    let ctx = new_ctx();
    let h = create(ctx);
    let port = resolve("ss-clk-in", true);
    send(h, clockwork_sys!("midi/in/enable"), &[OscArg::Str(port.clone()), OscArg::Int(1)]);

    let mut next = Instant::now();
    for _ in 0..240 {
        next += interval;
        while Instant::now() < next {}
        conn.send(&[0xF8]).unwrap();
    }
    std::thread::sleep(Duration::from_millis(50));
    // SAFETY: from `create`, destroyed once.
    unsafe { clockwork_midi_destroy(h) };

    // SAFETY: `ctx` is the leaked box from `new_ctx`, alive for the process.
    let clocks = unsafe { &*ctx }.clocks.lock().unwrap().clone();
    assert!(clocks.iter().all(|(p, _)| p == &port), "pulse tagged wrong port");
    let n = clocks.len();
    println!("clock IN: sent 240, received {n}");
    assert!((n as i64 - 240).abs() <= 3, "lost/extra pulses: {n}");

    let recv: Vec<u64> = clocks.iter().map(|&(_, t)| t).collect();
    let ideal = interval.as_micros() as f64;
    let devs: Vec<f64> = recv.windows(2).map(|w| ((w[1] - w[0]) as f64 - ideal).abs()).collect();
    let (mean_j, _) = mean_std(&devs);
    println!("delivery inter-arrival jitter: mean={mean_j:.1}us over {n} pulses");
    assert!(mean_j < 500.0, "delivery jitter {mean_j}us too high");
}

#[test]
#[ignore = "needs virtual MIDI + real time; run with --ignored --nocapture"]
fn two_ports_route_independently() {
    // Two virtual clock masters at different tempos: every pulse must be tagged
    // with its originating port — the multi-timeline routing contract.
    let cases = [("ss-clk-a", 120.0_f64), ("ss-clk-b", 150.0_f64)];

    let mut conns = Vec::new();
    let mut resolved = Vec::new();
    let ctx = new_ctx();
    let h = create(ctx);
    for (name, _) in cases {
        let vout = MidiOutput::new("ss-test").unwrap();
        conns.push(vout.create_virtual(name).unwrap());
        let port = resolve(name, true);
        send(h, clockwork_sys!("midi/in/enable"), &[OscArg::Str(port.clone()), OscArg::Int(1)]);
        resolved.push(port);
    }

    // Interleave precise pulses to both ports for ~1.5 s.
    let intervals: Vec<Duration> =
        cases.iter().map(|(_, bpm)| Duration::from_secs_f64(60.0 / bpm / 24.0)).collect();
    let mut next: Vec<Instant> = vec![Instant::now(); cases.len()];
    let deadline = Instant::now() + Duration::from_millis(1500);
    while Instant::now() < deadline {
        for i in 0..cases.len() {
            if Instant::now() >= next[i] {
                conns[i].send(&[0xF8]).unwrap();
                next[i] += intervals[i];
            }
        }
    }
    std::thread::sleep(Duration::from_millis(50));
    // SAFETY: from `create`, destroyed once.
    unsafe { clockwork_midi_destroy(h) };

    // SAFETY: `ctx` is the leaked box from `new_ctx`, alive for the process.
    let clocks = unsafe { &*(ctx) }.clocks.lock().unwrap().clone();
    for (i, (name, _)) in cases.iter().enumerate() {
        let count = clocks.iter().filter(|(p, _)| p == &resolved[i]).count();
        println!("{name}: {count} pulses routed");
        assert!(count > 50, "{name} got too few pulses: {count}"); // ~72 at 120 BPM over 1.5s
    }
}

#[test]
#[ignore = "needs virtual MIDI + real time; run with --ignored --nocapture"]
fn ramp_tracking_and_delivery_jitter() {
    // Precise accelerando 120 -> 180 BPM over 3s into the real subsystem; measure
    // (a) no pulses lost and (b) per-pulse delivery jitter (received inter-arrival
    // vs the true sent cadence). With coremidi_send_timestamped the received
    // timestamps are the precise send instants, so jitter reflects the real path
    // ClockworkClock anchors on. (Tempo/beat accuracy under a ramp is tested
    // engine-side in test_clockwork_clock.cpp.)
    let vout = MidiOutput::new("ss-test").unwrap();
    let mut conn = vout.create_virtual("ss-ramp-in").unwrap();

    let ctx = new_ctx();
    let h = create(ctx);
    let port = resolve("ss-ramp-in", true);
    send(h, clockwork_sys!("midi/in/enable"), &[OscArg::Str(port), OscArg::Int(1)]);

    let (start_bpm, end_bpm, ramp_s) = (120.0_f64, 180.0_f64, 3.0_f64);
    let t0 = Instant::now();
    let mut next = Instant::now();
    let mut sent: Vec<Instant> = Vec::new();
    loop {
        let el = t0.elapsed().as_secs_f64();
        if el >= ramp_s { break; }
        let true_bpm = start_bpm + (end_bpm - start_bpm) * (el / ramp_s);
        next += Duration::from_secs_f64(60.0 / true_bpm / 24.0);
        while Instant::now() < next {}
        conn.send(&[0xF8]).unwrap();
        sent.push(Instant::now());
    }
    std::thread::sleep(Duration::from_millis(50));
    // SAFETY: from `create`, destroyed once.
    unsafe { clockwork_midi_destroy(h) };

    // SAFETY: `ctx` is the leaked box from `new_ctx`, alive for the process.
    let clocks = unsafe { &*ctx }.clocks.lock().unwrap().clone();
    let (n_sent, n_recv) = (sent.len(), clocks.len());
    println!("ramp: sent {n_sent} pulses, received {n_recv}");
    assert!((n_recv as i64 - n_sent as i64).abs() <= 3, "lost/extra pulses: {n_sent} vs {n_recv}");

    // Delivery jitter: received inter-arrival (midir ts) vs true sent cadence.
    let recv: Vec<u64> = clocks.iter().map(|&(_, t)| t).collect();
    let send_us: Vec<f64> = sent.iter().map(|t| t.duration_since(t0).as_secs_f64() * 1e6).collect();
    let m = recv.len().min(send_us.len());
    let (mut max_j, mut sum_j) = (0.0_f64, 0.0_f64);
    for i in 1..m {
        let j = ((recv[i] - recv[i - 1]) as f64 - (send_us[i] - send_us[i - 1])).abs();
        max_j = max_j.max(j);
        sum_j += j;
    }
    let mean_j = sum_j / (m - 1).max(1) as f64;
    println!("delivery inter-arrival jitter: mean={mean_j:.1}us max={max_j:.1}us over {m} pulses");
    assert!(mean_j < 500.0, "mean delivery jitter {mean_j}us too high");
}

#[test]
#[ignore = "needs virtual MIDI; run with --ignored --nocapture"]
fn note_round_trip_through_ffi() {
    // device → engine: a virtual source the engine reads as input.
    let vout = MidiOutput::new("ss-test").unwrap();
    let mut src = vout.create_virtual("ss-rt-in").unwrap();
    // engine → device: a virtual destination the engine writes as output.
    let got: std::sync::Arc<Mutex<Vec<Vec<u8>>>> = std::sync::Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let vin = MidiInput::new("ss-test").unwrap();
    let _dst = vin
        .create_virtual("ss-rt-out", move |_t, b, _| sink.lock().unwrap().push(b.to_vec()), ())
        .unwrap();

    let ctx = new_ctx();
    let h = create(ctx);
    let in_port = resolve("ss-rt-in", true);
    let out_port = resolve("ss-rt-out", false);
    send(h, clockwork_sys!("midi/in/enable"), &[OscArg::Str(in_port), OscArg::Int(1)]);
    send(h, clockwork_sys!("midi/out/enable"), &[OscArg::Str(out_port.clone()), OscArg::Int(1)]);

    // IN: device sends note-on → engine emits /clockwork/midi/in/note_on.
    src.send(&[0x90, 60, 100]).unwrap();
    // OUT: engine sends note-on → device receives the bytes.
    send(
        h,
        clockwork_sys!("midi/out/note_on"),
        &[OscArg::Str(out_port), OscArg::Int(1), OscArg::Int(64), OscArg::Int(99)],
    );
    std::thread::sleep(Duration::from_millis(200));
    // SAFETY: from `create`, destroyed once.
    unsafe { clockwork_midi_destroy(h) };

    // SAFETY: `ctx` is the leaked box from `new_ctx`, alive for the process.
    let events = unsafe { &*(ctx) }.events.lock().unwrap().clone();
    assert!(
        events.iter().any(|e| {
            clockwork_midi::osc::decode(e).map(|m| m.addr) == Some(clockwork_sys!("midi/in/note_on").into())
        }),
        "expected /clockwork/midi/in/note_on, got {} events",
        events.len()
    );
    let got = got.lock().unwrap();
    assert!(
        got.iter().any(|b| b == &[0x90, 64, 99]),
        "expected note-on at device, got {got:?}"
    );
}
