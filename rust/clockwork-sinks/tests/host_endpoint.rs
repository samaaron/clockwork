// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The host as a MIDI sink's endpoint (endpoint::HostEndpoint), through the
//! C ABI a guest reaches it by.
//!
//! On a target with no MIDI port and no thread — the worklet — the host's
//! emitter is the only way out. While one is installed, a MIDI sink opens
//! onto it and is driven DIRECT: a send is the delivery, on the calling
//! thread, with `when` unchanged, and counts as scheduled because the host
//! holds the message to its time itself. Nothing is queued, so a flush finds
//! nothing. Uninstalled, opening goes back to whatever this build has.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::sync::Mutex;

use clockwork_sinks::ffi::{
    clockwork_sink_close, clockwork_sink_flush, clockwork_sink_open, clockwork_sink_send,
    clockwork_sink_set_host_emit, clockwork_sink_stats,
};
use clockwork_sinks::{Stats, KIND_MIDI, KIND_OSC};

/// One emitted event: kind, target, bytes, when.
type Taken = (u32, String, Vec<u8>, i64);
static TAKEN: Mutex<Vec<Taken>> = Mutex::new(Vec::new());
static REFUSE: Mutex<bool> = Mutex::new(false);

unsafe extern "C" fn emit(kind: u32, target: *const c_char, bytes: *const u8, len: u32, when: i64) -> i32 {
    if *REFUSE.lock().unwrap() { return 0; }
    // SAFETY: the sink's own NUL-terminated target, and a slice it holds.
    let (t, b) = unsafe {
        (CStr::from_ptr(target).to_string_lossy().into_owned(),
         std::slice::from_raw_parts(bytes, len as usize).to_vec())
    };
    TAKEN.lock().unwrap().push((kind, t, b, when));
    1
}

fn stats(sink: u32) -> Stats {
    let mut s = Stats::default();
    // SAFETY: a writable Stats.
    assert_eq!(unsafe { clockwork_sink_stats(sink, &mut s) }, 1);
    s
}

fn send(sink: u32, bytes: &[u8], when: i64) -> i32 {
    // SAFETY: a slice's pointer and length.
    unsafe { clockwork_sink_send(sink, bytes.as_ptr(), bytes.len() as u32, when) }
}

// One process, one emitter slot: the cases share it under a lock so they
// cannot interleave installs.
static SERIAL: Mutex<()> = Mutex::new(());

#[test]
fn a_midi_sink_opens_onto_the_host_and_a_send_is_the_delivery() {
    let _g = SERIAL.lock().unwrap();
    TAKEN.lock().unwrap().clear();
    clockwork_sink_set_host_emit(Some(emit));

    let target = CString::new("Fake Synth").unwrap();
    // SAFETY: a NUL-terminated target.
    let sink = unsafe { clockwork_sink_open(KIND_MIDI as i32, target.as_ptr(), 0) };
    assert_ne!(sink, 0, "no host sink opened");

    let when = -0x1234_5678_9abc_def0_i64;   // a real timetag: negative as i64
    assert_eq!(send(sink, &[0x90, 60, 100], when), 1);
    assert_eq!(send(sink, &[0x80, 60, 0], 1), 1);

    // Delivered on this thread, at once, with kind, target and time intact.
    let taken = TAKEN.lock().unwrap().clone();
    assert_eq!(taken, vec![
        (KIND_MIDI, "Fake Synth".to_string(), vec![0x90, 60, 100], when),
        (KIND_MIDI, "Fake Synth".to_string(), vec![0x80, 60, 0], 1),
    ]);
    let s = stats(sink);
    assert_eq!((s.sent, s.scheduled, s.dropped, s.late, s.cancelled), (2, 2, 0, 0, 0));

    // Nothing is held, so nothing can be cancelled: the host has them.
    assert_eq!(clockwork_sink_flush(sink), 0);

    // An empty message is refused and not a drop, as everywhere.
    assert_eq!(send(sink, &[], 1), 0);
    assert_eq!(stats(sink).dropped, 0);

    clockwork_sink_close(sink);
    clockwork_sink_set_host_emit(None);
}

#[test]
fn a_host_that_refuses_is_a_drop() {
    let _g = SERIAL.lock().unwrap();
    TAKEN.lock().unwrap().clear();
    clockwork_sink_set_host_emit(Some(emit));
    let target = CString::new("Fake Synth").unwrap();
    // SAFETY: a NUL-terminated target.
    let sink = unsafe { clockwork_sink_open(KIND_MIDI as i32, target.as_ptr(), 0) };
    assert_ne!(sink, 0);

    *REFUSE.lock().unwrap() = true;
    assert_eq!(send(sink, &[0xf8], 1), 0);
    *REFUSE.lock().unwrap() = false;
    let s = stats(sink);
    assert_eq!((s.sent, s.dropped), (0, 1));
    assert!(TAKEN.lock().unwrap().is_empty());

    clockwork_sink_close(sink);
    clockwork_sink_set_host_emit(None);
}

#[test]
fn the_host_takes_midi_only_and_only_while_installed() {
    let _g = SERIAL.lock().unwrap();
    clockwork_sink_set_host_emit(Some(emit));
    // OSC is not the host's: it opens the way it always did (a destination
    // that resolves) or refuses.
    let osc = CString::new("127.0.0.1:9").unwrap();
    // SAFETY: NUL-terminated targets.
    let sink = unsafe { clockwork_sink_open(KIND_OSC as i32, osc.as_ptr(), 0) };
    if sink != 0 {
        TAKEN.lock().unwrap().clear();
        send(sink, &[1, 2, 3], 1);
        assert!(TAKEN.lock().unwrap().is_empty(), "an OSC send reached the host emitter");
        clockwork_sink_close(sink);
    }

    // Uninstalled: a MIDI port that does not exist is refused at open, as
    // this build refused it before there was a host.
    clockwork_sink_set_host_emit(None);
    let target = CString::new("No Such Port, Surely").unwrap();
    // SAFETY: a NUL-terminated target.
    let none = unsafe { clockwork_sink_open(KIND_MIDI as i32, target.as_ptr(), 0) };
    assert_eq!(none, 0);
}
