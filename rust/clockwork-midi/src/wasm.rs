// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! wasm-bindgen surface for the web MIDI boundary: the same shared Rust core
//! (parse/encode/schema/estimator) exposed to the main-thread JS `MidiManager`,
//! which owns the Web MIDI I/O. Native I/O lives in `device`/`ffi` instead.
#![cfg(target_arch = "wasm32")]

use wasm_bindgen::prelude::*;

use crate::message::MidiMessage;
use crate::schema::{decode_out, encode_in, encode_in_at, encode_ports, encode_ports_reply, OutCommand};
use crate::ClockEstimator;

fn port_rows(names: Vec<String>, enabled: &[u8]) -> Vec<(String, bool)> {
    names
        .into_iter()
        .enumerate()
        .map(|(i, n)| (n, enabled.get(i).copied().unwrap_or(0) != 0))
        .collect()
}

/// `/clockwork/midi/ports.reply <nIn> [name enabled]* <nOut> [name enabled]*` —
/// the reply to `/clockwork/midi/ports/list`, in the native subsystem's exact
/// wire form, so a client cannot tell which host answered. `ins_enabled` /
/// `outs_enabled` are 0/1 per name.
#[wasm_bindgen]
pub fn midi_ports_reply_osc(
    ins: Vec<String>,
    ins_enabled: &[u8],
    outs: Vec<String>,
    outs_enabled: &[u8],
) -> Vec<u8> {
    encode_ports_reply(&port_rows(ins, ins_enabled), &port_rows(outs, outs_enabled))
}

/// `/clockwork/midi/ports …` — the same payload as a push to subscribers on
/// a hotplug or enable change.
#[wasm_bindgen]
pub fn midi_ports_osc(
    ins: Vec<String>,
    ins_enabled: &[u8],
    outs: Vec<String>,
    outs_enabled: &[u8],
) -> Vec<u8> {
    encode_ports(&port_rows(ins, ins_enabled), &port_rows(outs, outs_enabled))
}

/// Raw inbound bytes from a device → a `/clockwork/midi/in/*` OSC packet for the engine /
/// app. Returns `None` for clock pulses (handled by the estimator instead).
#[wasm_bindgen]
pub fn midi_in_osc(port: &str, bytes: &[u8]) -> Option<Vec<u8>> {
    encode_in(port, &MidiMessage::parse(bytes)?)
}

/// [`midi_in_osc`] with the arrival time as a trailing `t` argument: an OSC
/// timetag (a BigInt in JS), 0 for unknown.
#[wasm_bindgen]
pub fn midi_in_osc_at(port: &str, bytes: &[u8], when: u64) -> Option<Vec<u8>> {
    encode_in_at(port, &MidiMessage::parse(bytes)?, when)
}

/// Raw inbound bytes → a flat structured event `[kind, port, ...ints]` as a JS
/// array, e.g. `["note_on", "kbd", 1, 60, 100]`. This is the main-thread fast
/// path: a web consumer that just wants the fields skips the OSC encode here and
/// the matching decode in JS that [`midi_in_osc`] would force. Returns `None`
/// for clock pulses (the estimator handles those) and for unparseable bytes.
#[wasm_bindgen]
pub fn midi_in_fields(port: &str, bytes: &[u8]) -> Option<Vec<JsValue>> {
    let msg = MidiMessage::parse(bytes)?;
    if msg.is_clock_pulse() {
        return None;
    }
    let mut nums = [0i32; 3];
    let n = msg.data_args(&mut nums);
    let mut out = Vec::with_capacity(2 + n);
    out.push(JsValue::from_str(msg.kind()));
    out.push(JsValue::from_str(port));
    for &v in &nums[..n] {
        out.push(JsValue::from_f64(v as f64));
    }
    Some(out)
}

/// A `/clockwork/midi/out/*` OSC packet → `[port_len:u8][port][raw midi bytes]` for the
/// JS layer to hand to `MIDIOutput.send`. `None` for non-send verbs.
#[wasm_bindgen]
/// Pack a /clockwork/midi/out/* message for the main thread.
///
///     [when: 8 bytes little-endian][portLen: 1][port][bytes]
///
/// `when` is the OSC timetag from the verb; 0 means now. It leads rather than
/// trails so the reader can take it before it knows how long the port name is.
///
/// The web is where a timestamp is worth the most: MIDIOutput.send(bytes,
/// timestamp) is scheduled by the browser, and handing it a future time is
/// TIGHTER than racing to deliver on time across the worklet/main boundary.
/// Until this carried `when`, every web send was immediate whatever the verb
/// asked for.
pub fn midi_out_decode(osc: &[u8]) -> Option<Vec<u8>> {
    let (port, bytes, when) = match decode_out(osc)? {
        OutCommand::Send { port, msg, when } => {
            // channel 0 = "all channels" (wire channel -1): concatenate the 16
            // channel-voice messages — MIDIOutput.send accepts a multi-message
            // buffer, so one send() covers all channels.
            if msg.channel() == Some(0) {
                let mut b = Vec::new();
                for ch in 1..=16 {
                    b.extend_from_slice(&msg.with_channel(ch).encode());
                }
                (port, b, when)
            } else {
                (port, msg.encode(), when)
            }
        }
        OutCommand::SendRaw { port, bytes, when } => (port, bytes, when),
        _ => return None,
    };
    let mut out = Vec::with_capacity(8 + 1 + port.len() + bytes.len());
    out.extend_from_slice(&when.to_le_bytes());
    out.push(port.len() as u8);
    out.extend_from_slice(port.as_bytes());
    out.extend_from_slice(&bytes);
    Some(out)
}

/// Normalise a raw device name to its OSC-safe handle — identical to the native
/// path, so a port is addressed the same way on web and native.
#[wasm_bindgen]
pub fn normalize_name(raw: &str) -> String {
    crate::normalize::safe_osc_name(raw)
}

/// Median-filtered tempo estimator for an incoming MIDI clock (the same one the
/// native side uses), fed clock-pulse arrival timestamps in microseconds.
#[wasm_bindgen]
pub struct WasmClockEstimator {
    inner: ClockEstimator,
}

impl Default for WasmClockEstimator {
    fn default() -> Self {
        Self::new()
    }
}

#[wasm_bindgen]
impl WasmClockEstimator {
    #[wasm_bindgen(constructor)]
    pub fn new() -> WasmClockEstimator {
        WasmClockEstimator { inner: ClockEstimator::new() }
    }

    /// Feed a pulse timestamp (µs); returns the BPM estimate once available.
    pub fn update(&mut self, ts_us: f64) -> Option<f64> {
        self.inner.update(ts_us as u64)
    }

    pub fn reset(&mut self) {
        self.inner.reset();
    }
}
