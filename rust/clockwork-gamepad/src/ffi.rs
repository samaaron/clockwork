// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Native C ABI for the gamepad subsystem — the boundary the C++ engine links
//! against (via the `clockwork-native` umbrella staticlib).
//!
//! Each core's engine creates a registration, supplying one host callback, and
//! feeds it decoded-from-the-wire `/clockwork/gamepad/*` OSC via
//! [`clockwork_gamepad_handle_osc`].
//! Translated `/clockwork/gamepad/in/*` events and `/clockwork/gamepad/devices` pushes flow back
//! through the `emit` callback, which may fire on the poll thread — the
//! engine's implementation must be thread-safe (it is: the egress ring).
//!
//! The device IO (see [`crate::backend`]) is **process-global**: one poll
//! thread and one device context, created on first use and kept for the life
//! of the process. gilrs is built as a per-process singleton — its Linux
//! backend spawns a hotplug thread with no shutdown path, so tearing down and
//! recreating the context leaks a thread + fds each time. Instances are
//! host registrations, one per core (docs/ENGINES.md): create adds the
//! core's callback, destroy removes it (with none left the poll thread parks,
//! keeping the device registry warm for the next). What the pads say reaches
//! every core registered; which pads are enabled is the process's, as the
//! pads are. Destroy synchronises on the registrations' lock, so no callback
//! fires after it returns.
//!
//! Replies (`/clockwork/gamepad/devices.reply`) are emitted synchronously inside
//! [`clockwork_gamepad_handle_osc`] / [`clockwork_gamepad_emit_devices`] on the caller's
//! thread, while the engine's origin token still identifies the caller.

use std::ffi::{c_char, c_void};
use std::ptr;
use std::slice;
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use crate::backend::GamepadIo;
use crate::io::{Out, Registry};
use crate::schema::{
    decode_out, encode_devices, encode_devices_reply, OutCommand,
};
// Emit-callback shape (here delivering `/clockwork/gamepad/in/*` + `/clockwork/gamepad/devices*`
// packets), kind codes and panic fence: shared across the subsystem C ABIs —
// see clockwork_osc::ffi.
pub use clockwork_osc::ffi::{no_unwind, EmitFn, EMIT_BROADCAST, EMIT_REPLY};
use clockwork_osc::Refusal;

/// The host callback + opaque context, bundled and made Send/Sync so the poll
/// thread can hold a copy.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
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
}

/// Every core's registration. None: parked, events discarded.
type Cores = Mutex<Vec<Arc<Host>>>;

/// The process-global subsystem: poll thread, device registry, rumble channel
/// and the cores registered.
struct Global {
    registry: Arc<Mutex<Registry>>,
    cores: Arc<Cores>,
    tx: Sender<OutCommand>,
}

static GLOBAL: OnceLock<Global> = OnceLock::new();

fn global() -> &'static Global {
    GLOBAL.get_or_init(|| {
        let registry = Arc::new(Mutex::new(Registry::default()));
        let cores: Arc<Cores> = Arc::new(Mutex::new(Vec::new()));
        let (tx, rx) = channel();

        let thread_registry = registry.clone();
        let thread_cores = cores.clone();
        // The poll thread runs for the life of the process (matching the
        // device backends' own lifetime expectations). Errors are best-effort:
        // if the backend can't start, the registry simply stays empty.
        let _ = std::thread::Builder::new().name("clockwork-gamepad".into()).spawn(move || {
            // The loop body must never unwind into the OS thread shim.
            no_unwind((), || poll_loop(thread_cores, thread_registry, rx));
        });

        Global { registry, cores, tx }
    })
}

/// Every core's, under the registrations' lock: a core that leaves waits for
/// it.
fn emit_all(cores: &Cores, kind: i32, osc: &[u8]) {
    for host in cores.lock().unwrap().iter() {
        host.emit(kind, osc);
    }
}

/// One thing the pads said, to every core.
fn deliver(cores: &Cores, registry: &Mutex<Registry>, out: &Out) {
    match out.encode() {
        Some(event) => emit_all(cores, EMIT_BROADCAST, &event),
        None => {
            let rows = registry.lock().unwrap().snapshot();
            emit_all(cores, EMIT_BROADCAST, &encode_devices(&rows));
        }
    }
}

/// The opaque handle the C++ side owns: a core's registration against the
/// global subsystem carrying the engine's callback (also used directly for
/// the synchronous replies).
pub struct ClockworkGamepad {
    host: Arc<Host>,
}

impl ClockworkGamepad {
    fn handle(&self, cmd: OutCommand) {
        match cmd {
            // Rumble needs the backend's device context: hop to the poll thread.
            cmd @ (OutCommand::Rumble { .. } | OutCommand::RumbleStop { .. }) => {
                let _ = global().tx.send(cmd);
            }

            // Enable toggles the shared registry directly (the poll thread
            // reads it per event), so the devices push reflects it immediately.
            OutCommand::Enable { pad, enabled } => {
                let changed = global().registry.lock().unwrap().set_enabled(&pad, enabled);
                if changed {
                    self.push_devices();
                }
            }
            OutCommand::DevicesList => self.reply_devices(),
            // The backends track hotplug themselves; refresh just re-broadcasts
            // the current snapshot for clients that want to resync.
            OutCommand::Refresh => self.push_devices(),

            // Subscription is an egress-audience concern owned by the C++ boundary.
            OutCommand::Subscribe | OutCommand::Unsubscribe => {}
        }
    }

    fn reply_devices(&self) {
        let rows = global().registry.lock().unwrap().snapshot();
        self.host.emit(EMIT_REPLY, &encode_devices_reply(&rows));
    }

    // The pads are the process's, so every core is told.
    fn push_devices(&self) {
        let rows = global().registry.lock().unwrap().snapshot();
        emit_all(&global().cores, EMIT_BROADCAST, &encode_devices(&rows));
    }
}

/// The poll-thread body: drain backend events → emit to every registered
/// core, run rumble commands, expire rumble deadlines. The ~4 ms poll pace
/// keeps worst-case added input latency well under an audio buffer; with no
/// core registered the thread parks at a lazy tick, keeping the registry
/// warm. Emission holds the registrations' lock, so
/// `clockwork_gamepad_destroy`'s removal strictly orders against any
/// in-flight callback.
fn poll_loop(
    cores: Arc<Cores>,
    registry: Arc<Mutex<Registry>>,
    rx: std::sync::mpsc::Receiver<OutCommand>,
) {
    let mut io = match GamepadIo::new(registry.clone()) {
        Ok(io) => io,
        Err(_) => return, // no backend (rare): devices list stays empty
    };
    loop {
        let parked = cores.lock().unwrap().is_empty();
        let pace = if parked { Duration::from_millis(250) } else { Duration::from_millis(4) };
        for out in io.poll(pace) {
            deliver(&cores, &registry, &out);
        }
        for cmd in rx.try_iter() {
            match cmd {
                OutCommand::Rumble { pad, strong, weak, duration_ms } => {
                    io.rumble(&pad, strong, weak, duration_ms);
                }
                OutCommand::RumbleStop { pad } => io.rumble_stop(&pad),
                _ => {} // only rumble verbs are forwarded here
            }
        }
        io.expire_rumble();
    }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Register a core with the gamepad subsystem: adds `emit` to the cores told
/// what the pads say (starting the process-global device IO on first use).
/// Returns an owning pointer (null on failure); free with
/// [`clockwork_gamepad_destroy`]. `ctx` and the callback must remain valid
/// until then.
#[no_mangle]
pub extern "C" fn clockwork_gamepad_create(ctx: *mut c_void, emit: EmitFn) -> *mut ClockworkGamepad {
    no_unwind(std::ptr::null_mut(), || {
        let host = Arc::new(Host { ctx, emit });
        global().cores.lock().unwrap().push(host.clone());
        Box::into_raw(Box::new(ClockworkGamepad { host }))
    })
}

/// Remove the core's registration; the last core's also stops all rumble. The
/// poll thread and device context persist (parked) for the life of the
/// process; once this returns no further callbacks reach the core (removal
/// synchronises on the emission lock).
///
/// # Safety
/// `handle` is null or from `clockwork_gamepad_create`, not yet destroyed,
/// and nothing uses it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_gamepad_destroy(handle: *mut ClockworkGamepad) {
    no_unwind((), || {
        if handle.is_null() {
            return;
        }
        // SAFETY: the box `clockwork_gamepad_create` leaked, freed once.
        let me = unsafe { Box::from_raw(handle) };
        let g = global();
        let mut cores = g.cores.lock().unwrap();
        cores.retain(|h| !Arc::ptr_eq(h, &me.host));
        let last = cores.is_empty();
        drop(cores);
        if last {
            let _ = g.tx.send(OutCommand::RumbleStop { pad: "*".into() });
        }
    })
}

/// Feed one decoded `/clockwork/gamepad/*` OSC packet (the C++ boundary forwards these off
/// the audio thread). Returns null when the packet was a verb and was taken,
/// else why it was refused, a static NUL-terminated string the caller answers
/// with: "unknown clockwork verb" or "malformed" ([`Refusal`]).
///
/// # Safety
/// `handle` is null or a live handle; `data` is null or readable for `len`
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_gamepad_handle_osc(handle: *mut ClockworkGamepad, data: *const u8, len: u32) -> *const c_char {
    no_unwind(ptr::null(), || {
        if handle.is_null() || data.is_null() {
            return Refusal::Malformed.reason().as_ptr();
        }
        // SAFETY: a live handle and a readable byte string, per the contract;
        // nulls were refused above.
        let (me, bytes) = unsafe { (&*handle, slice::from_raw_parts(data, len as usize)) };
        match decode_out(bytes) {
            Ok(cmd) => {
                me.handle(cmd);
                ptr::null()
            }
            Err(refusal) => refusal.reason().as_ptr(),
        }
    })
}

/// Emit a fresh `/clockwork/gamepad/devices.reply` to the caller — used by the C++ boundary
/// to send a device snapshot to a newly-subscribed client.
///
/// # Safety
/// `handle` is null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_gamepad_emit_devices(handle: *mut ClockworkGamepad) {
    no_unwind((), || {
        if handle.is_null() {
            return;
        }
        // SAFETY: a live handle, per the contract.
        let me = unsafe { &*handle };
        me.reply_devices()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_osc::clockwork_sys;
    use clockwork_osc::osc;

    // What each core's registration was told, by core.
    static HEARD: Mutex<Vec<(usize, Vec<u8>)>> = Mutex::new(Vec::new());

    extern "C" fn heard_by(ctx: *mut c_void, _: i32, osc: *const u8, len: u32) {
        // SAFETY: the bytes Host::emit hands over, for this call.
        let bytes = unsafe { slice::from_raw_parts(osc, len as usize) };
        HEARD.lock().unwrap().push((ctx as usize, bytes.to_vec()));
    }

    fn heard(core: usize, addr: &str) -> usize {
        HEARD.lock().unwrap().iter().filter(|(c, b)| *c == core && osc::decode(b).is_some_and(|m| m.addr == addr)).count()
    }

    // Two cores' counts, read while no delivery is under way: the poll thread
    // tells every core at once, and may (a pad plugged in) at any time.
    fn counts(one: usize, two: usize, addr: &str) -> (usize, usize) {
        let _quiet = global().cores.lock().unwrap();
        (heard(one, addr), heard(two, addr))
    }

    fn refresh(from: *mut ClockworkGamepad) {
        let verb = osc::encode(clockwork_sys!("gamepad/refresh"), &[]);
        // SAFETY: a live handle and a readable packet.
        assert!(unsafe { clockwork_gamepad_handle_osc(from, verb.as_ptr(), verb.len() as u32) }.is_null());
    }

    // The process's gamepads are every core's (docs/ENGINES.md, suspect
    // 9): what they say reaches every core registered, and a core leaving
    // leaves the others registered.
    #[test]
    fn every_core_hears_the_process_gamepads_and_one_leaving_leaves_the_others() {
        // The tally is the module's, and the tests run at once: each counts its
        // own cores only, and none clears it.
        let (a, b) = (clockwork_gamepad_create(ptr::without_provenance_mut(1), heard_by), clockwork_gamepad_create(ptr::without_provenance_mut(2), heard_by));
        assert!(!a.is_null() && !b.is_null());
        let devices = clockwork_sys!("gamepad/devices");

        refresh(a);
        let (one, two) = counts(1, 2, devices);
        assert!(one >= 1 && one == two, "the devices are told to every core: {one}, {two}");

        // SAFETY: destroyed once, and not used after.
        unsafe { clockwork_gamepad_destroy(b) };
        let (one, gone) = counts(1, 2, devices);
        refresh(a);
        assert_eq!(counts(1, 2, devices).1, gone, "the core that left hears nothing more");
        assert!(counts(1, 2, devices).0 > one, "the core still registered still hears");
        // SAFETY: as above.
        unsafe { clockwork_gamepad_destroy(a) };
    }

    // What the poll thread hears from a pad, through the same delivery.
    #[test]
    fn a_pad_is_heard_by_every_core_registered() {
        let cores: Cores = Mutex::new(Vec::new());
        let registry = Mutex::new(Registry::default());
        let (a, b) = (Arc::new(Host { ctx: ptr::without_provenance_mut(11), emit: heard_by }), Arc::new(Host { ctx: ptr::without_provenance_mut(12), emit: heard_by }));
        cores.lock().unwrap().extend([a.clone(), b]);
        let press = Out::from_pad_event("pad", crate::PadEvent::Button { idx: 0, pressed: true, value: 1.0 }, 0);
        let button = clockwork_sys!("gamepad/in/button");
        deliver(&cores, &registry, &press);
        assert_eq!((heard(11, button), heard(12, button)), (1, 1));
        cores.lock().unwrap().retain(|h| Arc::ptr_eq(h, &a));
        deliver(&cores, &registry, &press);
        assert_eq!((heard(11, button), heard(12, button)), (2, 1));
    }
}
