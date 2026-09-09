// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Where a message actually goes, and the one question the substrate asks it.
//!
//! # `schedules()` is the whole platform story
//!
//! `src/clockwork_event_sink.h` says timing belongs at the edge: Web MIDI takes a
//! timestamp, CoreMIDI packets carry one, an ALSA sequencer event schedules on
//! a queue, and WinMM's `midiOutShortMsg` is immediate so somebody has to
//! hold. The header's point is that A DSP NEVER BRANCHES ON PLATFORM — it says
//! when it means and the endpoint does the best it can.
//!
//! That reduces to a single question, asked once per drain:
//!
//! * `schedules() == true` — hand the message over the moment it is seen, with
//!   its time, and count it `scheduled`. The platform will deliver it, and
//!   more accurately than we could: a worklet-to-main-thread hop is jittery
//!   and a browser's MIDI scheduler is not.
//! * `schedules() == false` — HOLD it, and deliver when it comes due. Sending
//!   early is the one thing that is definitely wrong.
//!
//! Nothing else in this crate knows which platform it is on, and no caller
//! knows at all.
//!
//! # The native `send_at`, and what it cost
//!
//! Landed 2026-08-30. `MidiOutputConnection::send_at(bytes, delay)` is uniform
//! across midir's backends, and [`MidiEndpoint`] asks the device — not the
//! target OS — whether it can hold a message, because it depends on whether a
//! queue was actually allocated.
//!
//! A DELAY rather than an absolute timestamp, because a delay is the one form
//! every backend honours natively: ALSA schedules relative to its queue,
//! CoreMIDI adds to the current host time, Web MIDI adds to `performance.now()`.
//! An absolute time would need a different clock-domain conversion per platform
//! and would push that choice onto every caller.
//!
//! Per backend:
//!   * **ALSA** — output had no queue at all (`set_direct()` bypasses it), so
//!     `send_at` allocates and starts one and schedules RELATIVE, which is what
//!     avoids mapping our clock onto the queue's. The shared client gets one
//!     queue for the whole client, queues being
//!     client-scoped. Measured on an idle box: sd 0.33ms, offset +1.0ms.
//!   * **CoreMIDI** — already built a timestamped packet and stamped it "now";
//!     the change is a future host time. WRITTEN BUT NOT YET RUN ON macOS.
//!   * **WinMM** — no timestamped short-message send exists, so it refuses and
//!     the sink goes on holding the message itself. This is why the substrate
//!     must still be able to hold: one backend never gaining it is the normal
//!     case, not the exception.
//!   * **Web MIDI** — not in our build at all. `midir` is a
//!     `cfg(not(target_arch = "wasm32"))` dependency and the browser's MIDI API
//!     lives on the main thread, so js/lib/midi_manager.js calls
//!     `MIDIOutput.send(bytes, timestamp)` there directly.
//!
//! `src/clockwork_event_sink.h` did not change, and no caller or DSP did either —
//! which is what it meant for the contract to be "shaped for it".
//!
use std::sync::Mutex;

/// What became of one message.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Delivery {
    /// Handed to the platform, which will put it on the wire now.
    Sent,
    /// Handed to the platform WITH ITS TIME, to be delivered later.
    Scheduled,
    /// The endpoint could not take it — the device went away, the address does
    /// not resolve. It did not go out, so it counts as a drop.
    Failed,
}

/// Somewhere messages go. The substrate holds one per sink and knows nothing
/// else about it.
pub trait Endpoint: Send + Sync {
    /// Deliver one message. `when` is the timetag it was meant for, passed
    /// through unchanged so an endpoint that can honour it may.
    fn deliver(&self, bytes: &[u8], when: i64) -> Delivery;

    /// Can this endpoint honour a future `when` itself? See the module note:
    /// this single bit is the whole of the platform branch.
    fn schedules(&self) -> bool { false }

    /// A last word before the sink closes, for an endpoint with state to let
    /// go of. Most have none.
    fn close(&self) {}
}

/// An endpoint that keeps what it was given.
///
/// Not a toy: it is how the ordering, lateness and drop rules are tested
/// without a device, and it is the only endpoint that can be asserted against
/// exactly. A MIDI port tells you nothing about what it received and a UDP
/// socket tells you nothing about what arrived.
#[derive(Default)]
pub struct CaptureEndpoint {
    /// `(when, bytes)`, in delivery order.
    got: Mutex<Vec<(i64, Vec<u8>)>>,
    /// Set to make every delivery fail, for testing the endpoint-refused path.
    fail: std::sync::atomic::AtomicBool,
    /// Set to answer `schedules()` true, for testing the platform-schedules
    /// path on a platform that does not have one.
    schedules: std::sync::atomic::AtomicBool,
}

impl CaptureEndpoint {
    pub fn new() -> CaptureEndpoint { CaptureEndpoint::default() }

    /// Everything delivered so far, in the order it was delivered.
    pub fn taken(&self) -> Vec<(i64, Vec<u8>)> {
        self.got.lock().unwrap().clone()
    }

    pub fn count(&self) -> usize { self.got.lock().unwrap().len() }

    pub fn set_failing(&self, yes: bool) {
        self.fail.store(yes, std::sync::atomic::Ordering::Relaxed);
    }

    pub fn set_schedules(&self, yes: bool) {
        self.schedules.store(yes, std::sync::atomic::Ordering::Relaxed);
    }
}

impl Endpoint for CaptureEndpoint {
    fn deliver(&self, bytes: &[u8], when: i64) -> Delivery {
        if self.fail.load(std::sync::atomic::Ordering::Relaxed) {
            return Delivery::Failed;
        }
        self.got.lock().unwrap().push((when, bytes.to_vec()));
        if self.schedules() { Delivery::Scheduled } else { Delivery::Sent }
    }
    fn schedules(&self) -> bool {
        self.schedules.load(std::sync::atomic::Ordering::Relaxed)
    }
}

// ── The host ────────────────────────────────────────────────────────────────

/// What the host's emitter is handed: the sink's kind and target, the bytes,
/// and `when` unchanged. Non-zero back means the host took it — and, since a
/// host that takes a message this way holds it to its time itself, took it
/// as SCHEDULED.
pub type HostEmit = unsafe extern "C" fn(
    kind: u32, target: *const std::os::raw::c_char,
    bytes: *const u8, len: u32, when: i64,
) -> i32;

static HOST_EMIT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// Install (or, with `None`, remove) the host's emitter. While one is
/// installed, every MIDI sink opened through the C ABI goes to it — see
/// [`crate::registry::open`].
pub fn set_host_emit(f: Option<HostEmit>) {
    HOST_EMIT.store(f.map_or(0, |f| f as usize), std::sync::atomic::Ordering::Release);
}

pub fn host_emit() -> Option<HostEmit> {
    let p = HOST_EMIT.load(std::sync::atomic::Ordering::Acquire);
    if p == 0 { return None; }
    // SAFETY: the only writer stores a `HostEmit` (or 0), so a non-zero value
    // is one.
    Some(unsafe { std::mem::transmute::<usize, HostEmit>(p) })
}

/// A sink whose endpoint is THE HOST: each message goes back to it, with its
/// time, for the host to put on a wire the substrate cannot reach.
///
/// The web is the case this exists for. The worklet has no MIDI port and no
/// thread — Web MIDI lives on the main thread and nowhere else — so the
/// engine hands each of a guest's sends out over the egress, wrapped with its
/// `when`, and the client's front hands it to `MIDIOutput.send(bytes,
/// timestamp)`. That is the "Web MIDI" row of the header's table, reached by
/// a guest the same way every other row is: it said when it meant.
///
/// `schedules()` is true — the browser holds the message to its time — and
/// the sink is driven DIRECT (see [`crate::registry::Drive`]): the emitter
/// is a ring write, safe on the audio thread, and there is no other thread
/// here to pump. A message handed over this way is past a flush's reach the
/// moment it is sent, which is the same trade the look-ahead window makes,
/// without the window.
pub struct HostEndpoint {
    kind: u32,
    target: std::ffi::CString,
}

impl HostEndpoint {
    /// `None` when no host emitter is installed, or the target cannot be a C
    /// string.
    pub fn for_target(kind: u32, target: &str) -> Option<HostEndpoint> {
        host_emit()?;
        Some(HostEndpoint { kind, target: std::ffi::CString::new(target).ok()? })
    }
}

impl Endpoint for HostEndpoint {
    fn deliver(&self, bytes: &[u8], when: i64) -> Delivery {
        let Some(emit) = host_emit() else { return Delivery::Failed };
        // SAFETY: a NUL-terminated target and a slice's pointer and length,
        // which is the emitter's contract.
        let took = unsafe {
            emit(self.kind, self.target.as_ptr(), bytes.as_ptr(), bytes.len() as u32, when)
        };
        if took != 0 { Delivery::Scheduled } else { Delivery::Failed }
    }
    fn schedules(&self) -> bool { true }
}

// ── MIDI ────────────────────────────────────────────────────────────────────

/// A MIDI output port, reached through the process's one MIDI subsystem.
///
/// It does NOT open a connection of its own. On Linux every output port lives
/// on a single shared ALSA sequencer client; a second client
/// per sink would spend an ALSA client and queue each time. And a port the
/// user enabled through `/clockwork/midi/enable` and a port a sink writes must
/// be the same port, or the same device is open twice under our own name.
#[cfg(all(feature = "midi", not(target_arch = "wasm32")))]
pub struct MidiEndpoint {
    /// The normalised port handle, or `"*"` for every open output.
    port: String,
    io: std::sync::Arc<Mutex<clockwork_midi::device::MidiIo>>,
}

#[cfg(all(feature = "midi", not(target_arch = "wasm32")))]
impl MidiEndpoint {
    /// Open a sink onto `port`, enabling the output if it is not already.
    ///
    /// Returns `None` when there is no MIDI subsystem in this process, or the
    /// named port does not exist. Refusing is the point: `MidiIo::send` to an
    /// unopened port is a silent no-op, and a sink that opened onto nothing
    /// and then dropped every message into it would be indistinguishable from
    /// one that worked — which is the failure mode clockwork has been bitten
    /// by before (see the note on silent control-name drops).
    ///
    /// `"*"` means every currently open output and enables nothing, matching
    /// what `/clockwork/midi/out/*` already means by it.
    pub fn open(port: &str) -> Option<MidiEndpoint> {
        let io = clockwork_midi::device::shared()?;
        if port != "*" {
            let mut guard = io.lock().ok()?;
            if !guard.enable_output(port, true) {
                return None;
            }
        }
        Some(MidiEndpoint { port: port.to_string(), io })
    }
}

#[cfg(all(feature = "midi", not(target_arch = "wasm32")))]
impl Endpoint for MidiEndpoint {
    fn deliver(&self, bytes: &[u8], when: i64) -> Delivery {
        // Off the audio thread — this is the drain — so a lock is allowed
        // here and nowhere else in the path.
        match self.io.lock() {
            Ok(mut io) => {
                // Where the platform can hold the message, hand it the delay
                // and let the kernel or the MIDI server do the waiting. Our own
                // waiting is a thread wakeup, and its jitter lands on the wire.
                if io.schedules() {
                    if let Some(delay) = crate::time::until(when as u64, crate::time::now()) {
                        if io.send_at(&self.port, bytes, delay) {
                            return Delivery::Scheduled;
                        }
                        // Queueing failed (no queue, or the port went away).
                        // Fall through and send now: the message is already
                        // due-ish and late beats lost.
                    }
                }
                io.send(&self.port, bytes);
                Delivery::Sent
            }
            Err(_) => Delivery::Failed,
        }
    }

    /// True where the backend got a queue: ALSA and CoreMIDI schedule, WinMM
    /// cannot and the sink goes on holding the message itself.
    ///
    /// Asked of the device rather than assumed from the target OS, because it
    /// depends on whether a queue was actually allocated — a sequencer that
    /// refused one leaves an otherwise-capable platform unable to schedule.
    fn schedules(&self) -> bool {
        self.io.lock().map(|io| io.schedules()).unwrap_or(false)
    }

    /// The port is deliberately NOT disabled. It may have been open before
    /// this sink existed — a user enabled it, or another sink is writing it —
    /// and closing a sink is not a statement about the device.
    fn close(&self) {}
}

// ── OSC ─────────────────────────────────────────────────────────────────────

/// An OSC destination: a resolved `host:port`, and a socket per family.
///
/// UDP has no scheduled send, so this holds like a native MIDI port does. An
/// OSC BUNDLE carries a timetag the receiver is meant to honour, and it would
/// be possible to wrap a late-bound message in one — but the sink is handed
/// bytes and sending different bytes than it was given is not its decision to
/// make.
#[cfg(all(feature = "osc", not(target_arch = "wasm32")))]
pub struct OscEndpoint {
    host: String,
    port: u16,
    sender: clockwork_osc_net::out::OscSender,
}

#[cfg(all(feature = "osc", not(target_arch = "wasm32")))]
impl OscEndpoint {
    /// Open onto `"host:port"`. Returns `None` if the target is not that
    /// shape, does not resolve, or no outbound socket would bind.
    ///
    /// IPv6 literals take brackets — `"[::1]:4560"` — because a bare `::1:4560`
    /// cannot be split on the last colon without guessing.
    pub fn open(target: &str) -> Option<OscEndpoint> {
        let (host, port) = split_host_port(target)?;
        if !clockwork_osc_net::out::OscSender::resolves(&host, port) {
            return None;
        }
        let sender = clockwork_osc_net::out::OscSender::new()?;
        Some(OscEndpoint { host, port, sender })
    }
}

#[cfg(all(feature = "osc", not(target_arch = "wasm32")))]
impl Endpoint for OscEndpoint {
    fn deliver(&self, bytes: &[u8], _when: i64) -> Delivery {
        if self.sender.send(&self.host, self.port, bytes) {
            Delivery::Sent
        } else {
            Delivery::Failed
        }
    }
}

/// Split `"host:port"`, with `"[v6]:port"` for an IPv6 literal. Public because
/// it is the whole of what `target` means for an OSC sink, and a test of the
/// parse should not need a socket.
pub fn split_host_port(target: &str) -> Option<(String, u16)> {
    let (host, port) = if let Some(rest) = target.strip_prefix('[') {
        let (h, r) = rest.split_once(']')?;
        (h.to_string(), r.strip_prefix(':')?)
    } else {
        let (h, p) = target.rsplit_once(':')?;
        (h.to_string(), p)
    };
    let port: u16 = port.parse().ok()?;
    if host.is_empty() || port == 0 { return None; }
    Some((host, port))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_target_is_a_host_and_a_port_or_it_is_nothing() {
        assert_eq!(split_host_port("127.0.0.1:4560"),
                   Some(("127.0.0.1".to_string(), 4560)));
        assert_eq!(split_host_port("example.local:9"),
                   Some(("example.local".to_string(), 9)));
        assert_eq!(split_host_port("[::1]:4560"), Some(("::1".to_string(), 4560)));
        // A bare v6 literal splits on the LAST colon, which is right as long
        // as a port is present — and a target with no port is refused anyway.
        assert_eq!(split_host_port("::1:4560"), Some(("::1".to_string(), 4560)));
        assert_eq!(split_host_port("fe80::1:4560"), Some(("fe80::1".to_string(), 4560)));
        assert_eq!(split_host_port("nocolon"), None);
        assert_eq!(split_host_port(":4560"), None);
        assert_eq!(split_host_port("host:0"), None, "port 0 names nothing");
        assert_eq!(split_host_port("host:99999"), None);
        assert_eq!(split_host_port(""), None);
    }
}
