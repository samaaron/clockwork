// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! In-process MIDI loopback over CoreMIDI/ALSA virtual ports, exercising the
//! real `device::MidiIo` byte path in both directions.
//!
//! Ignored by default: virtual ports need CoreMIDI/ALSA (not Windows) and the
//! delivery is asynchronous, so this is timing-dependent and unsuitable for CI.
//! Run on a dev machine with:
//!     cargo test --test virtual_loopback -- --ignored
// Virtual MIDI ports are a CoreMIDI and ALSA facility: Windows has none, so
// there is nothing for this test to loop through there.
#![cfg(all(not(target_arch = "wasm32"), unix))]

use std::sync::{Arc, Mutex};
use std::time::Duration;

use midir::os::unix::{VirtualInput, VirtualOutput};
use midir::{MidiInput, MidiOutput};

use clockwork_midi::device::MidiIo;
use clockwork_midi::message::MidiMessage;

/// What the loopback captured: `(port, bytes)` per message.
type Captured = Arc<Mutex<Vec<(String, Vec<u8>)>>>;

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports; run with --ignored on a dev box"]
fn input_loopback_via_virtual_port() {
    // A virtual *source* the subsystem will enumerate as an input port.
    let vout = MidiOutput::new("ss-test-src").unwrap();
    let mut vconn = vout.create_virtual("ss-loop-in").unwrap();

    let got: Captured = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let mut io = MidiIo::new(
        "clockwork-test",
        Arc::new(move |port: &str, _raw: &str, _ts, bytes: &[u8]| {
            sink.lock().unwrap().push((port.to_string(), bytes.to_vec()));
        }),
    );
    io.refresh();
    // The handle is the normalised, dedup'd form of the full OS port name, not
    // the bare virtual-port name — discover it from the enumeration.
    let (ins, _) = io.port_lists();
    let norm = ins
        .iter()
        .find(|(n, _)| n.contains("ss-loop-in"))
        .map(|(n, _)| n.clone())
        .expect("virtual input port enumerated");
    assert!(io.enable_input(&norm, true), "open virtual input port");

    let expected = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
    vconn.send(&expected.encode()).unwrap();
    std::thread::sleep(Duration::from_millis(200));

    let got = got.lock().unwrap();
    assert!(
        got.iter()
            .any(|(p, b)| *p == norm && MidiMessage::parse(b) == Some(expected.clone())),
        "expected note-on loopback, got {got:?}"
    );
}

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports; run with --ignored on a dev box"]
fn output_loopback_via_virtual_port() {
    // A virtual *destination* the subsystem will enumerate as an output port.
    let got: Arc<Mutex<Vec<Vec<u8>>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let vin = MidiInput::new("ss-test-dst").unwrap();
    let _vconn = vin
        .create_virtual(
            "ss-loop-out",
            move |_ts, bytes, _| sink.lock().unwrap().push(bytes.to_vec()),
            (),
        )
        .unwrap();

    let mut io = MidiIo::new("clockwork-test", Arc::new(|_, _, _, _: &[u8]| {}));
    io.refresh();
    let (_, outs) = io.port_lists();
    let norm = outs
        .iter()
        .find(|(n, _)| n.contains("ss-loop-out"))
        .map(|(n, _)| n.clone())
        .expect("virtual output port enumerated");
    assert!(io.enable_output(&norm, true), "open virtual output port");

    let expected = MidiMessage::ControlChange { channel: 1, controller: 7, value: 42 };
    io.send(&norm, &expected.encode());
    std::thread::sleep(Duration::from_millis(200));

    let got = got.lock().unwrap();
    assert!(
        got.iter().any(|b| MidiMessage::parse(b) == Some(expected.clone())),
        "expected control-change loopback, got {got:?}"
    );
}
