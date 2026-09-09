// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! midir-backed device IO: enumerate, open/close, send, and per-port input
//! callbacks. The same midir API serves native (CoreMIDI/ALSA/WinMM) and, later,
//! web (Web MIDI), so this module is platform-agnostic; threading and the FFI
//! shell live in the platform-specific [`crate::ffi`] layer.
//!
//! Ports are addressed by their normalized handle (see [`crate::normalize`]).
//! Devices are opened explicitly via `enable_*`.

use std::collections::HashSet;
use std::sync::Arc;

use midir::{MidiInput, MidiOutput};

use crate::normalize::{normalize_ports, PortInfo};

// MIDI port storage is backed differently per platform. On Linux a single ALSA
// sequencer client hosts every input port and a second hosts every output port
// (see `linux_ports` / midir's `shared` module): opening one client per port
// exhausts ALSA's per-system client/queue limits. Elsewhere
// (CoreMIDI/WinMM) there is no such cap, so we keep midir's one-connection-per-
// port model unchanged.
#[cfg(target_os = "linux")]
use linux_ports::{Inputs, Outputs};
#[cfg(not(target_os = "linux"))]
use per_port::{Inputs, Outputs};

/// Invoked for each inbound message on midir's input thread:
/// `(normalized_port, raw_os_name, timestamp_us, raw_bytes)`. The raw OS name
/// rides along so the engine can label a clock timeline with the friendly name.
pub type InputCallback = Arc<dyn Fn(&str, &str, u64, &[u8]) + Send + Sync + 'static>;

/// Owns all open MIDI connections plus the last-enumerated port lists.
pub struct MidiIo {
    client_name: String,
    inputs: Inputs,
    outputs: Outputs,
    in_ports: Vec<PortInfo>,
    out_ports: Vec<PortInfo>,
    // "Open everything" intent (set by enable_all). When set, refresh() re-opens
    // any newly-appeared ports so a re-plugged device resumes I/O on its own.
    want_all_in: bool,
    want_all_out: bool,
}

/// A port list as `(normalized_name, is_open)` pairs.
pub type PortList = Vec<(String, bool)>;

impl MidiIo {
    pub fn new(client_name: impl Into<String>, on_input: InputCallback) -> Self {
        let client_name = client_name.into();
        let mut io = Self {
            inputs: Inputs::new(&client_name, on_input),
            outputs: Outputs::new(&client_name),
            client_name,
            in_ports: Vec::new(),
            out_ports: Vec::new(),
            want_all_in: false,
            want_all_out: false,
        };
        io.refresh();
        io
    }

    /// Re-enumerate available ports (call on hotplug / refresh). Connections to
    /// vanished ports are dropped (closing the dead handle so a re-plug of the
    /// same name reconnects instead of being treated as already-open), and if an
    /// "open all" intent is set, newly-appeared ports are reopened.
    pub fn refresh(&mut self) {
        // Skip our own ports so "open everything" can't subscribe our inputs to
        // our own outputs (self-wired feedback loops + duplicate ports, #3543).
        if let Ok(mi) = MidiInput::new(&self.client_name) {
            self.in_ports = normalize_ports(&port_names_in(&mi));
            self.in_ports.retain(|p| !is_own_port(&p.raw));
        }
        if let Ok(mo) = MidiOutput::new(&self.client_name) {
            self.out_ports = normalize_ports(&port_names_out(&mo));
            self.out_ports.retain(|p| !is_own_port(&p.raw));
        }
        {
            let live_in: HashSet<&str> =
                self.in_ports.iter().map(|p| p.normalized.as_str()).collect();
            self.inputs.retain_live(&live_in);
            let live_out: HashSet<&str> =
                self.out_ports.iter().map(|p| p.normalized.as_str()).collect();
            self.outputs.retain_live(&live_out);
        }
        if self.want_all_in {
            self.enable_all(true, true);
        }
        if self.want_all_out {
            self.enable_all(false, true);
        }
    }

    /// (inputs, outputs) as `(normalized_name, is_open)` pairs.
    pub fn port_lists(&self) -> (PortList, PortList) {
        let ins = self
            .in_ports
            .iter()
            .map(|p| (p.normalized.clone(), self.inputs.is_open(&p.normalized)))
            .collect();
        let outs = self
            .out_ports
            .iter()
            .map(|p| (p.normalized.clone(), self.outputs.is_open(&p.normalized)))
            .collect();
        (ins, outs)
    }

    /// Open (or close) an input port by normalized name. Returns true on success
    /// (or if already in the requested state).
    pub fn enable_input(&mut self, norm: &str, enable: bool) -> bool {
        if !enable {
            self.inputs.disable(norm);
            return true;
        }
        self.inputs.enable(&self.client_name, norm)
    }

    /// Open (or close) an output port by normalized name.
    pub fn enable_output(&mut self, norm: &str, enable: bool) -> bool {
        if !enable {
            self.outputs.disable(norm);
            return true;
        }
        self.outputs.enable(&self.client_name, norm)
    }

    /// Enable (or disable) every available input/output port, and remember the
    /// intent so refresh() can reopen ports that appear later (e.g. on re-plug).
    pub fn enable_all(&mut self, input: bool, enable: bool) {
        if input {
            self.want_all_in = enable;
        } else {
            self.want_all_out = enable;
        }
        let names: Vec<String> = if input {
            self.in_ports.iter().map(|p| p.normalized.clone()).collect()
        } else {
            self.out_ports.iter().map(|p| p.normalized.clone()).collect()
        };
        for n in names {
            if input {
                self.enable_input(&n, enable);
            } else {
                self.enable_output(&n, enable);
            }
        }
    }

    /// Send raw bytes to an open output (`"*"` = all open outputs).
    pub fn send(&mut self, port: &str, bytes: &[u8]) {
        self.outputs.send(port, bytes);
    }

    /// Send raw bytes to an open output `delay` from now, letting the PLATFORM
    /// hold the message — the kernel on Linux, the MIDI server on macOS.
    ///
    /// This is what a MIDI clock wants. Sending on time from a thread of ours
    /// means waking that thread on time, and the jitter in that wakeup lands
    /// directly on the wire. False if the message could not be queued; check
    /// [`Self::schedules`] first.
    pub fn send_at(&mut self, port: &str, bytes: &[u8], delay: std::time::Duration) -> bool {
        self.outputs.send_at(port, bytes, delay)
    }

    /// Whether this platform's outputs can hold a message until it is due.
    /// False on WinMM, which has no timestamped short-message send at all.
    pub fn schedules(&self) -> bool {
        self.outputs.schedules()
    }

    /// Names of currently open output ports (used to fan out generated clock).
    pub fn open_outputs(&self) -> Vec<String> {
        self.outputs.open_keys()
    }
}

// ── The process's MIDI IO, shared with whoever else must not open a client ──
//
// Opening MIDI output is not a private act. On Linux every output port lives
// on ONE shared ALSA sequencer client (see `linux_ports`) —
// a second subsystem that opened its own would spend a client and a queue per
// port and eventually exhaust both, which is the exact bug that module exists
// to prevent. Elsewhere it would merely mean the same device appearing twice
// in the OS registry under our own name.
//
// So there is one `MidiIo` per process, created by `clockwork_midi_create`, and this
// is how another subsystem finds it. `clockwork-sinks` uses it: an event sink
// onto a MIDI port sends through the same connections `/clockwork/midi/out/*`
// does, and enabling the port means the same thing to both.
//
// A `Weak`, deliberately. The subsystem's lifetime is the C++ side's — it is
// destroyed by `clockwork_midi_destroy` — and a strong reference here would keep
// midir connections open past it. A holder that finds `None` has its answer:
// there is no MIDI subsystem in this build or at this moment, and a sink onto
// a MIDI port must refuse rather than pretend.
static SHARED: std::sync::Mutex<Option<std::sync::Weak<std::sync::Mutex<MidiIo>>>> =
    std::sync::Mutex::new(None);

/// Publish this process's MIDI IO. Called once, by `clockwork_midi_create`.
pub fn publish_shared(io: &Arc<std::sync::Mutex<MidiIo>>) {
    if let Ok(mut g) = SHARED.lock() {
        *g = Some(Arc::downgrade(io));
    }
}

/// This process's MIDI IO, or `None` if no subsystem is alive.
pub fn shared() -> Option<Arc<std::sync::Mutex<MidiIo>>> {
    SHARED.lock().ok().and_then(|g| g.as_ref().and_then(|w| w.upgrade()))
}

fn port_names_in(mi: &MidiInput) -> Vec<String> {
    mi.ports()
        .iter()
        .map(|p| mi.port_name(p).unwrap_or_default())
        .collect()
}

fn port_names_out(mo: &MidiOutput) -> Vec<String> {
    mo.ports()
        .iter()
        .map(|p| mo.port_name(p).unwrap_or_default())
        .collect()
}

/// The names we give our own MIDI ports on every backend (the add_port / connect
/// sites below). The enumeration filter skips ports bearing these names so "open
/// everything" can't wire us to ourselves — keyed on the name we
/// assign, not the client name, so it works across ALSA/CoreMIDI/WinMM and also
/// skips a second clockwork instance's internal ports.
const OWN_IN_PORT: &str = "clockwork-midi-in";
const OWN_OUT_PORT: &str = "clockwork-midi-out";
use crate::watcher::HOTSWAP_CLIENT_NAME;

/// True when an enumerated raw port name is one of our own ports.
fn is_own_port(raw: &str) -> bool {
    raw.contains(OWN_IN_PORT) || raw.contains(OWN_OUT_PORT)
        // The hot-swap watcher's ALSA announce port is a distinct client, so
        // the I/O port names above don't cover it (its NO_EXPORT capability
        // doesn't help either — midir's filter only checks for WRITE|SUBS).
        || raw.contains(HOTSWAP_CLIENT_NAME)
}

/// Linux/ALSA storage: all inputs share one sequencer client, all outputs share
/// another (midir's `shared` module). This keeps the count of ALSA seq clients
/// and queues constant no matter how many ports are open. Each `enable` still
/// enumerates with a throwaway client to
/// resolve the device address for a normalized handle, then subscribes the port
/// onto the persistent shared client.
#[cfg(target_os = "linux")]
mod linux_ports {
    use std::collections::HashSet;

    use midir::shared::{SharedInput, SharedOutput};
    use midir::Ignore;

    use super::{normalize_ports, port_names_in, port_names_out, InputCallback, OWN_IN_PORT, OWN_OUT_PORT};
    use midir::{MidiInput, MidiOutput};

    pub struct Inputs {
        // None if the shared ALSA client could not be opened; all enables then
        // no-op to false, mirroring the per-port path's graceful degradation.
        shared: Option<SharedInput>,
        open: HashSet<String>,
    }

    impl Inputs {
        pub fn new(client_name: &str, on_input: InputCallback) -> Self {
            let shared = SharedInput::new(client_name, Ignore::None, on_input).ok();
            Inputs {
                shared,
                open: HashSet::new(),
            }
        }

        pub fn is_open(&self, norm: &str) -> bool {
            self.open.contains(norm)
        }

        pub fn disable(&mut self, norm: &str) {
            if let Some(shared) = &self.shared {
                shared.remove_port(norm);
            }
            self.open.remove(norm);
        }

        pub fn retain_live(&mut self, live: &HashSet<&str>) {
            let gone: Vec<String> = self
                .open
                .iter()
                .filter(|n| !live.contains(n.as_str()))
                .cloned()
                .collect();
            for n in gone {
                self.disable(&n);
            }
        }

        pub fn enable(&mut self, client_name: &str, norm: &str) -> bool {
            if self.open.contains(norm) {
                return true;
            }
            let shared = match &self.shared {
                Some(s) => s,
                None => return false,
            };
            let midi_in = match MidiInput::new(client_name) {
                Ok(m) => m,
                Err(_) => return false,
            };
            let ports = midi_in.ports();
            let infos = normalize_ports(&port_names_in(&midi_in));
            let idx = match infos.iter().position(|pi| pi.normalized == norm) {
                Some(i) => i,
                None => return false,
            };
            if shared.add_port(&ports[idx].id(), OWN_IN_PORT, norm, &infos[idx].raw) {
                self.open.insert(norm.to_string());
                true
            } else {
                false
            }
        }
    }

    pub struct Outputs {
        shared: Option<SharedOutput>,
    }

    impl Outputs {
        pub fn new(client_name: &str) -> Self {
            Outputs {
                shared: SharedOutput::new(client_name).ok(),
            }
        }

        pub fn is_open(&self, norm: &str) -> bool {
            self.shared.as_ref().is_some_and(|s| s.contains(norm))
        }

        pub fn disable(&mut self, norm: &str) {
            if let Some(shared) = &mut self.shared {
                shared.remove_port(norm);
            }
        }

        pub fn retain_live(&mut self, live: &HashSet<&str>) {
            let shared = match &mut self.shared {
                Some(s) => s,
                None => return,
            };
            for n in shared.labels() {
                if !live.contains(n.as_str()) {
                    shared.remove_port(&n);
                }
            }
        }

        pub fn enable(&mut self, client_name: &str, norm: &str) -> bool {
            let midi_out = match MidiOutput::new(client_name) {
                Ok(m) => m,
                Err(_) => return false,
            };
            let ports = midi_out.ports();
            let infos = normalize_ports(&port_names_out(&midi_out));
            let idx = match infos.iter().position(|pi| pi.normalized == norm) {
                Some(i) => i,
                None => return false,
            };
            let shared = match &mut self.shared {
                Some(s) => s,
                None => return false,
            };
            shared.add_port(&ports[idx].id(), OWN_OUT_PORT, norm)
        }

        pub fn send(&mut self, port: &str, bytes: &[u8]) {
            let shared = match &mut self.shared {
                Some(s) => s,
                None => return,
            };
            if port == "*" {
                for label in shared.labels() {
                    shared.send(&label, bytes);
                }
            } else {
                shared.send(port, bytes);
            }
        }

        pub fn send_at(&mut self, port: &str, bytes: &[u8], delay: std::time::Duration) -> bool {
            let shared = match &mut self.shared {
                Some(s) => s,
                None => return false,
            };
            if port == "*" {
                let mut any = false;
                for label in shared.labels() {
                    any |= shared.send_at(&label, bytes, delay);
                }
                any
            } else {
                shared.send_at(port, bytes, delay)
            }
        }

        pub fn schedules(&self) -> bool {
            self.shared.as_ref().map(|s| s.schedules()).unwrap_or(false)
        }

        pub fn open_keys(&self) -> Vec<String> {
            self.shared.as_ref().map(|s| s.labels()).unwrap_or_default()
        }
    }
}

/// Default storage (CoreMIDI/WinMM): midir's one-connection-per-port model,
/// unchanged. These platforms have no per-client kernel cap, so there is nothing
/// to share.
#[cfg(not(target_os = "linux"))]
mod per_port {
    use std::collections::{HashMap, HashSet};

    use midir::{Ignore, MidiInput, MidiInputConnection, MidiOutput, MidiOutputConnection};

    use super::{normalize_ports, port_names_in, port_names_out, InputCallback, OWN_IN_PORT, OWN_OUT_PORT};

    pub struct Inputs {
        conns: HashMap<String, MidiInputConnection<()>>,
        on_input: InputCallback,
    }

    impl Inputs {
        pub fn new(_client_name: &str, on_input: InputCallback) -> Self {
            Inputs {
                conns: HashMap::new(),
                on_input,
            }
        }

        pub fn is_open(&self, norm: &str) -> bool {
            self.conns.contains_key(norm)
        }

        pub fn disable(&mut self, norm: &str) {
            self.conns.remove(norm); // dropping the connection closes the port
        }

        pub fn retain_live(&mut self, live: &HashSet<&str>) {
            self.conns.retain(|k, _| live.contains(k.as_str()));
        }

        pub fn enable(&mut self, client_name: &str, norm: &str) -> bool {
            if self.conns.contains_key(norm) {
                return true;
            }
            let mut midi_in = match MidiInput::new(client_name) {
                Ok(m) => m,
                Err(_) => return false,
            };
            midi_in.ignore(Ignore::None); // we want sysex, timing and active-sensing
            let ports = midi_in.ports();
            let infos = normalize_ports(&port_names_in(&midi_in));
            let idx = match infos.iter().position(|pi| pi.normalized == norm) {
                Some(i) => i,
                None => return false,
            };
            let cb = self.on_input.clone();
            let name = norm.to_string();
            let raw = infos[idx].raw.clone();
            match midi_in.connect(
                &ports[idx],
                OWN_IN_PORT,
                move |ts, bytes, _| cb(&name, &raw, ts, bytes),
                (),
            ) {
                Ok(conn) => {
                    self.conns.insert(norm.to_string(), conn);
                    true
                }
                Err(_) => false,
            }
        }
    }

    pub struct Outputs {
        conns: HashMap<String, MidiOutputConnection>,
    }

    impl Outputs {
        pub fn new(_client_name: &str) -> Self {
            Outputs {
                conns: HashMap::new(),
            }
        }

        pub fn is_open(&self, norm: &str) -> bool {
            self.conns.contains_key(norm)
        }

        pub fn disable(&mut self, norm: &str) {
            self.conns.remove(norm); // dropping the connection closes the port
        }

        pub fn retain_live(&mut self, live: &HashSet<&str>) {
            self.conns.retain(|k, _| live.contains(k.as_str()));
        }

        pub fn enable(&mut self, client_name: &str, norm: &str) -> bool {
            if self.conns.contains_key(norm) {
                return true;
            }
            let midi_out = match MidiOutput::new(client_name) {
                Ok(m) => m,
                Err(_) => return false,
            };
            let ports = midi_out.ports();
            let infos = normalize_ports(&port_names_out(&midi_out));
            let idx = match infos.iter().position(|pi| pi.normalized == norm) {
                Some(i) => i,
                None => return false,
            };
            match midi_out.connect(&ports[idx], OWN_OUT_PORT) {
                Ok(conn) => {
                    self.conns.insert(norm.to_string(), conn);
                    true
                }
                Err(_) => false,
            }
        }

        pub fn send(&mut self, port: &str, bytes: &[u8]) {
            if port == "*" {
                for conn in self.conns.values_mut() {
                    let _ = conn.send(bytes);
                }
            } else if let Some(conn) = self.conns.get_mut(port) {
                let _ = conn.send(bytes);
            }
        }

        pub fn send_at(&mut self, port: &str, bytes: &[u8], delay: std::time::Duration) -> bool {
            if port == "*" {
                let mut any = false;
                for conn in self.conns.values_mut() {
                    any |= conn.send_at(bytes, delay).is_ok();
                }
                any
            } else if let Some(conn) = self.conns.get_mut(port) {
                conn.send_at(bytes, delay).is_ok()
            } else {
                false
            }
        }

        /// Asked of the open connections rather than of the platform, because
        /// the capability is the connection's. With none open there is nothing
        /// to schedule to, so false — not an optimistic guess.
        pub fn schedules(&self) -> bool {
            !self.conns.is_empty() && self.conns.values().all(|c| c.schedules())
        }

        pub fn open_keys(&self) -> Vec<String> {
            self.conns.keys().cloned().collect()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::is_own_port;
    use crate::normalize::normalize_ports;

    // Regression guard: enumeration must drop clockwork's own
    // ports (else "open everything" subscribes our inputs to our own outputs,
    // creating ~N× MIDI feedback and a bank of duplicate self-wired ports).
    #[test]
    fn own_ports_excluded_external_kept() {
        let raws = vec![
            "clockwork:clockwork-midi-out 130:50".to_string(), // own -> drop
            "clockwork:clockwork-midi-in 129:3".to_string(),   // own -> drop
            "Midi Through:Midi Through Port-0 14:0".to_string(),  // ext -> keep
            "nanoKONTROL2:nanoKONTROL2 MIDI 1 24:0".to_string(),  // ext -> keep
        ];
        let kept: Vec<_> = normalize_ports(&raws)
            .into_iter()
            .filter(|p| !is_own_port(&p.raw))
            .collect();
        assert_eq!(kept.len(), 2, "kept: {:?}", kept.iter().map(|p| &p.raw).collect::<Vec<_>>());
        assert!(kept.iter().all(|p| !p.raw.contains("clockwork-midi-")));
    }

    // Keyed on the port name we assign, not the client name — so a real device on
    // a client coincidentally named "clockwork" is still kept, and our own ports
    // are caught on any backend (ALSA/CoreMIDI/WinMM name formats differ).
    #[test]
    fn matches_assigned_port_name_not_client_name() {
        assert!(is_own_port("clockwork:clockwork-midi-in 1:0"));
        assert!(is_own_port("clockwork:clockwork-midi-out 1:0"));
        assert!(is_own_port("clockwork-midi-out")); // bare CoreMIDI-style name
        assert!(!is_own_port("clockwork:Launchpad MIDI 1 1:0")); // client-name collision, real device kept
        assert!(!is_own_port("nanoKONTROL2:nanoKONTROL2 MIDI 1 1:0"));
    }
}
