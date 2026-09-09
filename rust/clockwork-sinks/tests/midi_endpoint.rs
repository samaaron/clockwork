// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The MIDI endpoint, and an honest account of how far it is tested here.
//!
//! # What is tested unconditionally
//!
//! The BOUNDARY: that a MIDI sink refuses to open when there is nothing to
//! open onto. That is not a consolation prize — it is the single most
//! important thing about this endpoint. `MidiIo::send` to a port that is not
//! open is a silent no-op, so a sink that opened optimistically would accept
//! every message, deliver none, and report `sent` for all of them. Clockwork
//! has been bitten by exactly that shape before (unknown `/s_new` control
//! names vanishing without an error, which hid a whole routing chain being
//! bypassed). A refusal at open is what makes the failure visible.
//!
//! # What is NOT tested here, and why
//!
//! Bytes actually reaching a MIDI port. It needs a virtual/loopback port,
//! which needs the ALSA sequencer (`/dev/snd/seq`) or CoreMIDI, and the box
//! this was written on has neither — `snd_seq_hw_open` answers "Permission
//! denied", which is also why `clockwork-midi`'s own
//! `tests/virtual_loopback.rs` is `#[ignore]`d. So the case below is written
//! and marked the same way rather than skipped or faked, and on a machine with
//! a sequencer it runs:
//!
//! ```text
//! cargo test -p clockwork-sinks --test midi_endpoint -- --ignored
//! ```
//!
//! Everything the sink does BEFORE the endpoint — the ordering, the holding,
//! the lateness, the drop counting — is covered against the capture endpoint
//! in `tests/sinks.rs`, and none of it differs by kind.

// Virtual MIDI ports are a CoreMIDI and ALSA facility: Windows has none, so
// there is nothing for this test to send through there.
#![cfg(all(feature = "midi", not(target_arch = "wasm32"), unix))]

use std::ffi::CString;

use clockwork_sinks::ffi::*;
use clockwork_sinks::sink::KIND_MIDI;

#[test]
fn a_midi_sink_refuses_when_there_is_no_midi_subsystem() {
    // Nothing in this process has called clockwork_midi_create, so
    // device::shared() is None and there is no port to enable.
    assert!(clockwork_midi::device::shared().is_none(),
            "this case is about the absence; something else opened a subsystem");
    let any = CString::new("whatever").unwrap();
    // SAFETY: a NUL-terminated CString.
    assert_eq!(unsafe { clockwork_sink_open(KIND_MIDI as i32, any.as_ptr(), 64) }, 0);
    // Including the wildcard: "every open output" of nothing is still nothing.
    let star = CString::new("*").unwrap();
    // SAFETY: a NUL-terminated CString.
    assert_eq!(unsafe { clockwork_sink_open(KIND_MIDI as i32, star.as_ptr(), 64) }, 0);
}

// ── The real path, when the machine has a sequencer ─────────────────────────

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports (/dev/snd/seq); run with --ignored"]
fn a_midi_sink_delivers_to_a_virtual_port() {
    use std::os::raw::c_void;
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use midir::os::unix::VirtualInput;
    use midir::{Ignore, MidiInput};

    // A virtual DESTINATION: other clients — including our own MIDI subsystem
    // — see it as an output port they may write to.
    let got: Arc<Mutex<Vec<Vec<u8>>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let mut vin = MidiInput::new("ss-sink-test").expect("a MIDI client");
    vin.ignore(Ignore::None);
    let _vconn = vin
        .create_virtual("ss-sink-loop", move |_ts, bytes: &[u8], _| {
            sink.lock().unwrap().push(bytes.to_vec());
        }, ())
        .expect("a virtual destination");

    // The subsystem, which is what publishes the shared MidiIo the endpoint
    // finds. Its callbacks are never exercised by an output-only test.
    extern "C" fn emit(_c: *mut c_void, _k: i32, _d: *const u8, _l: u32) {}
    extern "C" fn clock(_c: *mut c_void, _n: *const u8, _nl: u32,
                        _r: *const u8, _rl: u32, _t: u64) {}
    extern "C" fn transport(_c: *mut c_void, _n: *const u8, _nl: u32,
                            _r: *const u8, _rl: u32, _k: i32, _b: f64) {}
    let app = CString::new("clockwork-sink-test").unwrap();
    // SAFETY: `app` is a NUL-terminated string that outlives the call.
    let midi = unsafe { clockwork_midi::ffi::clockwork_midi_create(
        core::ptr::null_mut(), emit, clock, transport, app.as_ptr()) };
    assert!(!midi.is_null(), "the MIDI subsystem must start");

    // The handle is the normalised, dedup'd form of the OS port name, so it is
    // discovered from the enumeration rather than guessed.
    let io = clockwork_midi::device::shared().expect("clockwork_midi_create publishes it");
    let norm = {
        let guard = io.lock().unwrap();
        let (_ins, outs) = guard.port_lists();
        outs.iter().find(|(n, _)| n.contains("ss-sink-loop")).map(|(n, _)| n.clone())
            .expect("the virtual destination was enumerated as an output")
    };

    let target = CString::new(norm).unwrap();
    // SAFETY: a NUL-terminated CString.
    let h = unsafe { clockwork_sink_open(KIND_MIDI as i32, target.as_ptr(), 64) };
    assert_ne!(h, 0, "opening a sink onto a real port must enable it");

    // Sent backwards in time and one "immediately": what arrives must be in
    // time order, at a real port, through the drain thread — nothing here
    // pumps anything.
    let base = clockwork_sinks::time::now();
    let ms = |n: u64| base + ((1u64 << 32) / 1000) * n;
    let sends: [([u8; 3], i64); 4] = [
        ([0x90, 64, 100], ms(60) as i64),
        ([0x90, 62, 100], ms(40) as i64),
        ([0x80, 60, 0],   ms(20) as i64),
        ([0x90, 60, 100], 1),
    ];
    for (body, when) in sends {
        // SAFETY: `body` is 3 live bytes.
        assert_ne!(unsafe { clockwork_sink_send(h, body.as_ptr(), 3, when) }, 0);
    }

    let deadline = Instant::now() + Duration::from_secs(3);
    while got.lock().unwrap().len() < 4 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    let arrived = got.lock().unwrap().clone();
    assert_eq!(arrived.len(), 4, "only {} of 4 arrived: {arrived:?}", arrived.len());
    assert_eq!(arrived,
               vec![vec![0x90, 60, 100], vec![0x80, 60, 0],
                    vec![0x90, 62, 100], vec![0x90, 64, 100]],
               "a MIDI port gets the messages in time order, not send order");

    let mut stats = clockwork_sinks::sink::Stats::default();
    // SAFETY: a live out-pointer.
    unsafe { clockwork_sink_stats(h, &mut stats) };
    assert_eq!(stats.sent, 4);
    assert_eq!(stats.dropped, 0);
    assert_eq!(stats.late, 0, "held and released on time is not late");
    // The three future messages are handed to the platform WITH their time;
    // the fourth ("immediately", when == 1) has no future to hand over and goes
    // out now. `scheduled` is a view into `sent`, not an addition to it.
    //
    // This case only runs where ALSA or CoreMIDI virtual ports exist, and both
    // schedule. On WinMM — no timestamped short-message send — the sink holds
    // the messages itself and this would be 0.
    assert_eq!(stats.scheduled, 3,
               "the future-dated messages should go to the platform's own queue");

    clockwork_sink_close(h);
    // SAFETY: from `clockwork_midi_create` above, destroyed once.
    unsafe { clockwork_midi::ffi::clockwork_midi_destroy(midi) };
}
