// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Native C ABI for the MIDI subsystem — the boundary the C++ engine links against.
//!
//! The engine creates one instance, supplying three host callbacks, and feeds it
//! decoded-from-the-wire `/clockwork/midi/*` OSC via [`clockwork_midi_handle_osc`]. The subsystem
//! owns its midir device IO (and midir's own input thread); it never touches the
//! audio thread. Clock-OUT generation lives engine-side in MidiClockOut.
//!
//! Data flows back to the engine through the callbacks:
//! * `emit` — a `/clockwork/midi/in/*` (or `/clockwork/midi/ports*`) OSC packet
//!   to inject into the RT IN ring / broadcast to subscribers.
//! * `clock` — one 0xF8 pulse → ClockworkClock (pulse-count beat + estimate).
//! * `transport` — Start/Continue/Stop/SongPosition → ClockworkClock transport.
//!
//! Callbacks may fire on the midir input thread, so the engine's implementations
//! must be thread-safe.

use std::collections::HashSet;
use std::ffi::c_void;
use std::slice;
use std::sync::{Arc, Mutex};

use crate::device::{InputCallback, MidiIo};
use crate::message::MidiMessage;
use crate::schema::{decode_out, encode_in, encode_ports, encode_ports_reply, OutCommand};
use crate::sync::{transport_event, TransportEvent};

// Emit-callback shape (here delivering `/clockwork/midi/in/*` + `/clockwork/midi/ports*` packets),
// kind codes and panic fence: shared across the subsystem C ABIs — see
// clockwork_osc::ffi.
pub use clockwork_osc::ffi::{no_unwind, EmitFn, EMIT_BROADCAST, EMIT_REPLY};
/// One MIDI clock pulse (0xF8) for an input port → the engine, which anchors the
/// timeline beat on the pulse count and estimates tempo engine-side (not here).
/// `norm` is the normalised handle the engine keys the timeline on; `raw` is the
/// friendly OS device name for display; `ts_us` is the pulse's OS timestamp (µs).
/// Strings are not NUL-terminated; all args valid only during the call.
pub type ClockFn = extern "C" fn(
    ctx: *mut c_void,
    norm: *const u8, norm_len: u32,
    raw: *const u8, raw_len: u32,
    ts_us: u64,
);
/// Transport intent for one input port: kind 0=Start 1=Continue 2=Stop
/// 3=Position; `beat` is the target beat for Start/Position, `-1` otherwise.
/// `norm`/`raw` as in [`ClockFn`].
pub type TransportFn = extern "C" fn(
    ctx: *mut c_void,
    norm: *const u8, norm_len: u32,
    raw: *const u8, raw_len: u32,
    kind: i32, beat: f64,
);

/// Transport `kind` codes shared with the C++ side.
pub const TRANSPORT_START: i32 = 0;
pub const TRANSPORT_CONTINUE: i32 = 1;
pub const TRANSPORT_STOP: i32 = 2;
pub const TRANSPORT_POSITION: i32 = 3;

/// The host callbacks + opaque context, bundled and made Send/Sync so the helper
/// threads can hold a copy. Safety: the C++ engine guarantees `ctx` outlives the
/// instance and the callbacks are thread-safe.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
    clock: ClockFn,
    transport: TransportFn,
}
// SAFETY: `ctx` is an opaque token this crate never dereferences, only hands
// back to the callbacks, and the C++ engine's contract is that it outlives
// the instance and the callbacks are thread-safe; fn pointers are Send + Sync.
unsafe impl Send for Host {}
// SAFETY: as above.
unsafe impl Sync for Host {}

impl Host {
    fn emit(&self, kind: i32, osc: &[u8]) {
        (self.emit)(self.ctx, kind, osc.as_ptr(), osc.len() as u32);
    }
    fn clock(&self, norm: &str, raw: &str, ts_us: u64) {
        (self.clock)(
            self.ctx,
            norm.as_ptr(), norm.len() as u32,
            raw.as_ptr(), raw.len() as u32,
            ts_us,
        );
    }
    fn transport(&self, norm: &str, raw: &str, kind: i32, beat: f64) {
        (self.transport)(
            self.ctx,
            norm.as_ptr(), norm.len() as u32,
            raw.as_ptr(), raw.len() as u32,
            kind, beat,
        );
    }
}

/// Shared with the midir input thread. `muted` holds ports the host has
/// explicitly opted out of clock-following via `/clockwork/midi/clock/sync … 0`; every
/// other enabled input that sends 0xF8 drives its own engine-side timeline.
#[derive(Default)]
struct InputState {
    muted: HashSet<String>,
}

/// The opaque handle the C++ side owns.
pub struct ClockworkMidi {
    // Declared first so it drops first: stopping the watcher's thread before the
    // rest tears down guarantees no refresh runs against half-dropped state.
    _watcher: crate::watcher::Watcher,
    host: Host,
    io: Arc<Mutex<MidiIo>>,
    input: Arc<Mutex<InputState>>,
}

/// Send now, or hand the platform the delay if the message is for later.
///
/// `when` on the verb replaces wrapping the whole message in
/// /clockwork/schedule: the timestamp belongs on the event. Where the platform
/// can hold it — an ALSA queue, a CoreMIDI packet timestamp — it does, and the
/// kernel's timer beats a thread of ours waking up to send "now".
///
/// A time already past sends immediately rather than being dropped: late is
/// better than silent, and the sink's `late` counter is where that is recorded.
fn send_at_or_now(io: &mut crate::device::MidiIo, port: &str, bytes: &[u8], when: u64) {
    match clockwork_osc::osc::delay_until(when) {
        Some(delay) if io.schedules() => { io.send_at(port, bytes, delay); }
        _ => io.send(port, bytes),
    }
}

impl ClockworkMidi {
    fn handle(&self, cmd: OutCommand) {
        match cmd {
            OutCommand::Send { port, msg, when } => {
                let mut io = self.io.lock().unwrap();
                // channel 0 is the "all channels" sentinel (wire channel -1):
                // fan a channel-voice message out to all 16 channels.
                if msg.channel() == Some(0) {
                    for ch in 1..=16 {
                        send_at_or_now(&mut io, &port, &msg.with_channel(ch).encode(), when);
                    }
                } else {
                    send_at_or_now(&mut io, &port, &msg.encode(), when);
                }
            }
            OutCommand::SendRaw { port, bytes, when } => {
                send_at_or_now(&mut self.io.lock().unwrap(), &port, &bytes, when)
            }

            // One immediate clock pulse, for a caller that wants a single 0xF8.
            // Continuous-clock generation + transport + beat-bursts are owned by
            // the engine's ClockworkClock-timed MidiClockOut (C++), which since
            // 2026-08-30 hands each tick straight to a sink carrying its own
            // time — those pulses do NOT come back through here.
            OutCommand::ClockTick { port } => self.io.lock().unwrap().send(&port, &[0xF8]),
            // Per-port clock-follow toggle. Every enabled input is tracked by
            // default; this opts a port out (mute) or back in. Muting stops the
            // pulse feed so its engine-side timeline goes stale and is reclaimed.
            OutCommand::ClockSync { port, enabled } => {
                let mut is = self.input.lock().unwrap();
                if enabled {
                    is.muted.remove(&port);
                } else {
                    is.muted.insert(port);
                }
            }

            OutCommand::Enable { port, input, enabled } => {
                {
                    let mut io = self.io.lock().unwrap();
                    if port == "*" {
                        io.enable_all(input, enabled);
                    } else if input {
                        io.enable_input(&port, enabled);
                    } else {
                        io.enable_output(&port, enabled);
                    }
                }
                self.push_ports();
            }
            OutCommand::Refresh => {
                self.io.lock().unwrap().refresh();
                self.push_ports();
            }
            OutCommand::PortsList => self.reply_ports(),

            // Subscription is an egress-audience concern owned by the C++ boundary.
            OutCommand::Subscribe | OutCommand::Unsubscribe => {}
        }
    }

    fn reply_ports(&self) {
        let (ins, outs) = self.io.lock().unwrap().port_lists();
        self.host.emit(EMIT_REPLY, &encode_ports_reply(&ins, &outs));
    }

    fn push_ports(&self) {
        let (ins, outs) = self.io.lock().unwrap().port_lists();
        self.host.emit(EMIT_BROADCAST, &encode_ports(&ins, &outs));
    }
}

/// Handle one inbound message (runs on midir's input thread). `norm` is the
/// normalised port handle (estimator key + /clockwork/midi/in address + timeline key);
/// `raw` is the friendly OS name passed through for timeline labelling.
fn handle_input(host: &Host, input: &Arc<Mutex<InputState>>,
                norm: &str, raw: &str, ts_us: u64, bytes: &[u8]) {
    let msg = match MidiMessage::parse(bytes) {
        Some(m) => m,
        None => return,
    };

    // Clock pulses feed each port's engine-side timeline (beat + tempo are
    // computed in ClockworkClock), so we just forward every 0xF8 with its OS
    // timestamp unless the port is muted. They never enter the /clockwork/midi/in ring.
    if msg.is_clock_pulse() {
        if input.lock().unwrap().muted.contains(norm) {
            return; // opted out via /clockwork/midi/clock/sync — don't drive a timeline
        }
        host.clock(norm, raw, ts_us);
        return;
    }

    // Transport/position drives the originating port's timeline (unless muted).
    if let Some(ev) = transport_event(&msg) {
        let muted = input.lock().unwrap().muted.contains(norm);
        if !muted {
            let (kind, beat) = transport_code(ev);
            host.transport(norm, raw, kind, beat);
        }
    }

    // Everything else surfaces as a /clockwork/midi/in/* event for the engine/clients.
    if let Some(osc) = encode_in(norm, &msg) {
        host.emit(EMIT_BROADCAST, &osc);
    }
}

fn transport_code(ev: TransportEvent) -> (i32, f64) {
    match ev {
        TransportEvent::Start => (TRANSPORT_START, 0.0),
        TransportEvent::Continue => (TRANSPORT_CONTINUE, -1.0),
        TransportEvent::Stop => (TRANSPORT_STOP, -1.0),
        TransportEvent::Position { .. } => (TRANSPORT_POSITION, ev.beat().unwrap_or(-1.0)),
    }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Create the MIDI subsystem. Returns an owning pointer (null on failure); free
/// with [`clockwork_midi_destroy`]. The callbacks and `ctx` must remain valid until then.
///
/// # Safety
/// `app_name` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_create(
    ctx: *mut c_void,
    emit: EmitFn,
    clock: ClockFn,
    transport: TransportFn,
    app_name: *const std::ffi::c_char,
) -> *mut ClockworkMidi {
    // The OS MIDI registry client name other apps see; NULL/"" falls back.
    let client_name = if app_name.is_null() {
        String::new()
    } else {
        // SAFETY: NUL-terminated per the contract above; null was checked.
        unsafe { std::ffi::CStr::from_ptr(app_name) }
            .to_string_lossy()
            .into_owned()
    };
    let client_name = if client_name.is_empty() {
        "clockwork".to_string()
    } else {
        client_name
    };
    no_unwind(std::ptr::null_mut(), || {
        let host = Host {
            ctx,
            emit,
            clock,
            transport,
        };

        let input = Arc::new(Mutex::new(InputState::default()));

        // The input callback runs inside the OS MIDI stack's callback frame
        // (CoreMIDI read proc / WinRT handler), so it must never unwind.
        let in_state = input.clone();
        let on_input: InputCallback = Arc::new(move |norm: &str, raw: &str, ts: u64, bytes: &[u8]| {
            no_unwind((), || handle_input(&host, &in_state, norm, raw, ts, bytes));
        });
        let io = Arc::new(Mutex::new(MidiIo::new(client_name, on_input)));
        // Published so another subsystem can send through the same
        // connections rather than opening a second client — see
        // device::publish_shared. Weakly held there, so this stays the
        // only owner and clockwork_midi_destroy still closes every port.
        crate::device::publish_shared(&io);

        // Native hot-swap: the OS device-change notification (WinRT DeviceWatcher /
        // CoreMIDI notify / ALSA announce) re-enumerates and diff-broadcasts
        // `/clockwork/midi/ports` directly — no JUCE, no audio-thread, no polling.
        let watch_host = host;
        let watch_io = io.clone();
        let on_change: crate::watcher::OnChange =
            Arc::new(move || no_unwind((), || refresh_and_broadcast(&watch_host, &watch_io)));
        let watcher = crate::watcher::Watcher::new(on_change);

        Box::into_raw(Box::new(ClockworkMidi {
            _watcher: watcher,
            host,
            io,
            input,
        }))
    })
}

/// Destroy the subsystem: closes all ports.
///
/// # Safety
/// `handle` is null or from [`clockwork_midi_create`], not yet destroyed,
/// and nothing uses it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_destroy(handle: *mut ClockworkMidi) {
    if handle.is_null() {
        return;
    }
    // Box dropped here → `io` drops → all midir connections close.
    // SAFETY: the box `clockwork_midi_create` leaked, freed once.
    no_unwind((), || drop(unsafe { Box::from_raw(handle) }));
}

/// One wire message from [`clockwork_midi_encode_out`]: the port it is for (`"*"` =
/// every open output), its bytes, and the time the verb itself named with a
/// trailing timetag (0 when it named none). Strings are not NUL-terminated;
/// all pointers are valid only during the call.
pub type EncodeOutFn = extern "C" fn(
    ctx: *mut c_void,
    port: *const u8, port_len: u32,
    bytes: *const u8, len: u32,
    when: u64,
);

/// Decode one `/clockwork/midi/out/*` send verb into the bytes it puts on the
/// wire, without sending them: `cb` is called once per message — a
/// channel-voice verb on channel 0 fans out to sixteen. Returns 1 when the
/// packet was a send verb (whether or not it produced anything), 0 when it was
/// anything else, which the caller should hand to [`clockwork_midi_handle_osc`].
///
/// This exists so the engine can put a note on the sink for its port instead
/// of sending it here: the sink keeps the note's TIME (a scheduled note is
/// released at its moment, not at the block it fired in) and the accounting
/// (`sent`/`scheduled`/`late`), and the encoding stays in one place.
///
/// # Safety
/// `data` is null or readable for `len` bytes; `cb` is called only during
/// this call, with `ctx`.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_encode_out(
    data: *const u8,
    len: u32,
    ctx: *mut c_void,
    cb: EncodeOutFn,
) -> i32 {
    if data.is_null() {
        return 0;
    }
    // SAFETY: readable for `len` bytes, per the contract; null was refused.
    let bytes = unsafe { slice::from_raw_parts(data, len as usize) };
    no_unwind(0, || {
        let emit = |port: &str, wire: &[u8], when: u64| {
            cb(ctx, port.as_ptr(), port.len() as u32, wire.as_ptr(), wire.len() as u32, when);
        };
        match decode_out(bytes) {
            Some(OutCommand::Send { port, msg, when }) => {
                if msg.channel() == Some(0) {
                    for ch in 1..=16 {
                        emit(&port, &msg.with_channel(ch).encode(), when);
                    }
                } else {
                    emit(&port, &msg.encode(), when);
                }
                1
            }
            Some(OutCommand::SendRaw { port, bytes, when }) => {
                emit(&port, &bytes, when);
                1
            }
            _ => 0,
        }
    })
}

/// Feed one decoded `/clockwork/midi/*` OSC packet (the C++ boundary forwards these off the
/// audio thread). Unknown/foreign addresses are ignored.
///
/// # Safety
/// `handle` is null or a live handle; `data` is null or readable for `len`
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_handle_osc(handle: *mut ClockworkMidi, data: *const u8, len: u32) {
    if handle.is_null() || data.is_null() {
        return;
    }
    // SAFETY: a live handle and a readable byte string, per the contract;
    // nulls were refused above.
    let (me, bytes) = unsafe { (&*handle, slice::from_raw_parts(data, len as usize)) };
    no_unwind((), || {
        if let Some(cmd) = decode_out(bytes) {
            me.handle(cmd);
        }
    });
}

/// Emit a fresh `/clockwork/midi/ports.reply` to the caller — used by the C++ boundary to send
/// a device snapshot to a newly-subscribed client.
///
/// # Safety
/// `handle` is null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_emit_ports(handle: *mut ClockworkMidi) {
    if handle.is_null() {
        return;
    }
    // SAFETY: a live handle, per the contract.
    let me = unsafe { &*handle };
    no_unwind((), || me.reply_ports());
}

/// Re-enumerate devices and, only if the port list actually changed, broadcast
/// the updated `/clockwork/midi/ports` to subscribers. Shared by the C ABI [`clockwork_midi_refresh`]
/// and the native hot-swap [`crate::watcher::Watcher`]; the change check keeps a
/// device-event storm from spamming `/clockwork/midi/ports`.
fn refresh_and_broadcast(host: &Host, io: &Mutex<MidiIo>) {
    let updated = {
        let mut io = io.lock().unwrap();
        let before = io.port_lists();
        io.refresh();
        let after = io.port_lists();
        if after != before { Some(after) } else { None }
    };
    if let Some((ins, outs)) = updated {
        host.emit(EMIT_BROADCAST, &encode_ports(&ins, &outs));
    }
}

/// Re-enumerate devices and, only if the port list actually changed, broadcast
/// the updated `/clockwork/midi/ports` to subscribers. Retained for the manual
/// `/clockwork/midi/refresh` boundary; native hot-swap is driven by the watcher (see
/// [`crate::watcher`]).
///
/// # Safety
/// `handle` is null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_refresh(handle: *mut ClockworkMidi) {
    if handle.is_null() {
        return;
    }
    // SAFETY: a live handle, per the contract.
    let me = unsafe { &*handle };
    no_unwind((), || refresh_and_broadcast(&me.host, &me.io));
}
