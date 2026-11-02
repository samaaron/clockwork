// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Native MIDI device hot-swap detection.
//!
//! midir is enumeration-only on every backend — it can tell you *what* devices
//! exist when asked, but never *that* the set just changed. This module supplies
//! the missing edge: a platform-native device-arrival/removal notification that
//! pokes a re-enumerate. It is deliberately ignorant of the rest of the MIDI
//! subsystem — it just calls the [`OnChange`] closure the caller supplied, which
//! (in [`crate::ffi`]) re-enumerates via midir, diffs, and broadcasts
//! `/clockwork/midi/ports`. No JUCE, no audio thread, no polling.
//!
//! Each backend feeds raw OS events into a shared [`Coalescer`], which collapses
//! the burst a single hot-plug produces into one `OnChange` call after a short
//! quiet period, then fires a second "settle" call a beat later to beat the
//! race where the OS announces a device before its driver finishes registering
//! the port (so the first enumeration would miss it).
//!
//! * **Windows** — two `Windows.Devices.Enumeration.DeviceWatcher`s (MIDI in/out
//!   selectors). Pairs with the `winrt` feature; needs a process MTA apartment.
//! * **macOS** — a CoreMIDI client notification callback (`SetupChanged` etc.).
//! * **Linux** — a subscription to the ALSA sequencer `System:announce` port.

use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// Called when the OS reports the MIDI device set may have changed. Runs on the
/// watcher's own thread; the implementation must be thread-safe.
pub type OnChange = Arc<dyn Fn() + Send + Sync + 'static>;

/// Client name the watcher registers with the OS MIDI service. On Linux this
/// names a real ALSA sequencer client whose announce port is visible to every
/// enumerator, so the registry filters it out of port lists by this name (see
/// `device::is_own_port`).
pub(crate) const HOTSWAP_CLIENT_NAME: &str = "clockwork-midi-hotswap";

/// Quiet period that ends a burst of device events before the first refresh.
const DEBOUNCE: Duration = Duration::from_millis(200);
/// Delay before the follow-up "settle" refresh that catches a port which wasn't
/// enumerable yet when the OS first announced its device.
const SETTLE: Duration = Duration::from_millis(450);

/// Owns the worker thread that turns OS event pokes into [`OnChange`] calls.
/// Backends obtain a [`Sender`] via [`Coalescer::poker`] and send `()` per event.
struct Coalescer {
    tx: Sender<()>,
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Coalescer {
    fn new(on_change: OnChange) -> Self {
        let (tx, rx) = mpsc::channel::<()>();
        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let handle = thread::Builder::new()
            .name("cw-midi-settle".into())
            .spawn(move || coalesce_loop(rx, stop_thread, on_change))
            .expect("spawn cw-midi-settle thread");
        Coalescer { tx, stop, handle: Some(handle) }
    }

    /// A sender a backend can clone into its event handlers to poke a refresh.
    fn poker(&self) -> Sender<()> {
        self.tx.clone()
    }
}

impl Drop for Coalescer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        // Unblock a thread parked in recv(); it re-checks `stop` and exits.
        let _ = self.tx.send(());
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

fn coalesce_loop(rx: Receiver<()>, stop: Arc<AtomicBool>, on_change: OnChange) {
    let mut burst_waiting = false;
    loop {
        // Block until the first event of a burst (or the stop poke), unless
        // an event already arrived during the last settle wait.
        if !burst_waiting && (rx.recv().is_err() || stop.load(Ordering::SeqCst)) {
            return;
        }
        burst_waiting = false;
        // Drain the rest of the burst: keep extending until it goes quiet.
        loop {
            match rx.recv_timeout(DEBOUNCE) {
                Ok(()) => {
                    if stop.load(Ordering::SeqCst) {
                        return;
                    }
                }
                Err(RecvTimeoutError::Timeout) => break,
                Err(RecvTimeoutError::Disconnected) => return,
            }
        }
        if stop.load(Ordering::SeqCst) {
            return;
        }
        on_change();
        // Refresh again SETTLE later, for a port that became enumerable after
        // the first refresh. Downstream diffs the device lists, so otherwise
        // this is a no-op. The stop ends this wait at once. An event during the
        // wait starts a new burst, coalesced after this refresh.
        let settle_at = Instant::now() + SETTLE;
        loop {
            match rx.recv_timeout(settle_at.saturating_duration_since(Instant::now())) {
                Ok(()) if stop.load(Ordering::SeqCst) => return,
                Ok(()) => burst_waiting = true,
                Err(RecvTimeoutError::Timeout) => break,
                Err(RecvTimeoutError::Disconnected) => return,
            }
        }
        on_change();
    }
}

/// A live MIDI hot-swap watcher. Drop to stop it.
pub struct Watcher {
    _imp: imp::PlatformWatcher,
}

impl Watcher {
    /// Start watching for MIDI device changes, calling `on_change` on each.
    pub fn new(on_change: OnChange) -> Self {
        Watcher {
            _imp: imp::PlatformWatcher::new(on_change),
        }
    }
}

// ── Windows: WinRT DeviceWatcher ──────────────────────────────────────────────
#[cfg(windows)]
mod imp {
    use super::{Coalescer, OnChange};
    use std::sync::mpsc::Sender;
    use std::sync::Once;

    use windows::core::HSTRING;
    use windows::Devices::Enumeration::{
        DeviceInformation, DeviceInformationUpdate, DeviceWatcher,
    };
    use windows::Devices::Midi::{MidiInPort, MidiOutPort};
    use windows::Foundation::TypedEventHandler;

    /// midir's WinRT backend (and our enumeration) activate WinRT objects and
    /// block on their async results, which requires an apartment. Establish a
    /// process-wide implicit MTA once so every MIDI thread — including this
    /// watcher's — is served without each having to initialize COM itself.
    fn ensure_mta() {
        static MTA: Once = Once::new();
        MTA.call_once(|| {
            // Never decremented: the cookie is simply dropped, leaving the
            // implicit MTA alive for the process lifetime so all MIDI threads
            // (this watcher, enumeration, the OSC handler) can activate WinRT.
            // SAFETY: a COM call with no arguments; the cookie it hands back
            // is deliberately dropped so the MTA stays up for the process.
            let _ = unsafe { windows::Win32::System::Com::CoIncrementMTAUsage() };
        });
    }

    /// One MIDI device-class watcher (input or output) plus its handler tokens.
    struct ClassWatcher {
        watcher: DeviceWatcher,
        added: i64,
        removed: i64,
        updated: i64,
    }

    impl ClassWatcher {
        fn new(selector: &HSTRING, poke: Sender<()>) -> windows::core::Result<Self> {
            let watcher = DeviceInformation::CreateWatcherAqsFilter(selector)?;

            let p = poke.clone();
            let added = watcher.Added(&TypedEventHandler::<DeviceWatcher, DeviceInformation>::new(
                move |_, _| {
                    let _ = p.send(());
                    Ok(())
                },
            ))?;

            let p = poke.clone();
            let removed = watcher.Removed(&TypedEventHandler::<
                DeviceWatcher,
                DeviceInformationUpdate,
            >::new(move |_, _| {
                let _ = p.send(());
                Ok(())
            }))?;

            let p = poke;
            let updated = watcher.Updated(&TypedEventHandler::<
                DeviceWatcher,
                DeviceInformationUpdate,
            >::new(move |_, _| {
                let _ = p.send(());
                Ok(())
            }))?;

            watcher.Start()?;
            Ok(ClassWatcher { watcher, added, removed, updated })
        }
    }

    impl Drop for ClassWatcher {
        fn drop(&mut self) {
            let _ = self.watcher.Stop();
            let _ = self.watcher.RemoveAdded(self.added);
            let _ = self.watcher.RemoveRemoved(self.removed);
            let _ = self.watcher.RemoveUpdated(self.updated);
        }
    }

    pub struct PlatformWatcher {
        // Drop order: class watchers first (stop OS callbacks, releasing their
        // sender clones), then the coalescer joins its now-idle thread.
        _ins: Option<ClassWatcher>,
        _outs: Option<ClassWatcher>,
        _coalescer: Coalescer,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            ensure_mta();
            let coalescer = Coalescer::new(on_change);
            let ins = MidiInPort::GetDeviceSelector()
                .and_then(|sel| ClassWatcher::new(&sel, coalescer.poker()))
                .ok();
            let outs = MidiOutPort::GetDeviceSelector()
                .and_then(|sel| ClassWatcher::new(&sel, coalescer.poker()))
                .ok();
            PlatformWatcher { _ins: ins, _outs: outs, _coalescer: coalescer }
        }
    }
}

// ── macOS: CoreMIDI client notifications ──────────────────────────────────────
#[cfg(target_os = "macos")]
mod imp {
    use super::{Coalescer, OnChange};
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};
    use std::thread::{self, JoinHandle};

    use core_foundation::base::{kCFAllocatorDefault, TCFType};
    use core_foundation::runloop::{
        kCFRunLoopDefaultMode, CFRunLoop, CFRunLoopGetCurrent, CFRunLoopSource, CFRunLoopSourceContext,
        CFRunLoopSourceCreate, CFRunLoopSourceSignal, CFRunLoopStop, CFRunLoopWakeUp,
    };
    use coremidi::{Client, Notification};

    /// The watcher thread's run loop, and a run-loop source whose perform
    /// callback stops it. A signalled source stays pending until the run loop
    /// services it, so a stop signalled before the loop starts still stops it.
    /// A bare CFRunLoopStop sent then would be lost.
    struct Stopper {
        run_loop: CFRunLoop,
        source: CFRunLoopSource,
    }
    // SAFETY: CFRunLoopSourceSignal and CFRunLoopWakeUp may be called from
    // any thread, and they are the only calls the dropping thread makes on these.
    unsafe impl Send for Stopper {}

    pub struct PlatformWatcher {
        stop: Arc<AtomicBool>,
        stopper: Arc<Mutex<Option<Stopper>>>,
        handle: Option<JoinHandle<()>>,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            let coalescer = Coalescer::new(on_change);
            let stop = Arc::new(AtomicBool::new(false));
            let stopper = Arc::new(Mutex::new(None));
            let (t_stop, t_stopper) = (stop.clone(), stopper.clone());
            // CoreMIDI delivers MIDIClientCreateWithBlock notifications only on the
            // run loop that was current when the client was created, and only while
            // that loop is running. So the client is both created *and* pumped on
            // this dedicated thread — delivery no longer depends on the host
            // happening to init the engine on a thread with a live CFRunLoop (the
            // standalone server's main thread does; an embedder's may not). This
            // matches the self-contained Linux/Windows backends.
            let handle = thread::Builder::new()
                .name("cw-midi-watch".into())
                .spawn(move || notify_loop(t_stop, t_stopper, coalescer))
                .ok();
            PlatformWatcher { stop, stopper, handle }
        }
    }

    impl Drop for PlatformWatcher {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            // The thread checks `stop` and publishes its Stopper under this
            // lock. So either the thread sees `stop` and never runs its loop,
            // or the signal below reaches the loop.
            if let Some(s) = self.stopper.lock().unwrap().as_ref() {
                // SAFETY: a live source and run loop, both retained by `s`.
                unsafe {
                    CFRunLoopSourceSignal(s.source.as_concrete_TypeRef());
                    CFRunLoopWakeUp(s.run_loop.as_concrete_TypeRef());
                }
            }
            if let Some(h) = self.handle.take() {
                let _ = h.join();
            }
        }
    }

    // The stop source's perform callback: stops the run loop it fired on.
    extern "C" fn stop_this_run_loop(_: *const c_void) {
        // SAFETY: the current thread's run loop, which is the one running.
        unsafe { CFRunLoopStop(CFRunLoopGetCurrent()) };
    }

    fn notify_loop(stop: Arc<AtomicBool>, stopper: Arc<Mutex<Option<Stopper>>>, coalescer: Coalescer) {
        let poke = coalescer.poker();
        // Bind the notification block to *this* thread's run loop. Any setup
        // change (object added/removed, property changed) coalesces into a single
        // re-enumerate downstream.
        let _client = match Client::new_with_notifications(
            crate::watcher::HOTSWAP_CLIENT_NAME,
            move |_n: &Notification| {
                let _ = poke.send(());
            },
        ) {
            Ok(c) => c,
            // No notifications available — the manual `/clockwork/midi/refresh` boundary still
            // works, so degrade quietly rather than tear the subsystem down.
            Err(_) => return,
        };

        let mut context = CFRunLoopSourceContext {
            version: 0,
            info: std::ptr::null_mut(),
            retain: None,
            release: None,
            copyDescription: None,
            equal: None,
            hash: None,
            schedule: None,
            cancel: None,
            perform: stop_this_run_loop,
        };
        // SAFETY: a version-0 context with no info and a plain perform
        // function, copied by the call; the result is owned under the create
        // rule.
        let source = unsafe {
            CFRunLoopSource::wrap_under_create_rule(CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &mut context))
        };
        let run_loop = CFRunLoop::get_current();
        // SAFETY: a CoreFoundation constant, immutable for the process.
        run_loop.add_source(&source, unsafe { kCFRunLoopDefaultMode });
        {
            let mut published = stopper.lock().unwrap();
            if stop.load(Ordering::SeqCst) {
                return;
            }
            *published = Some(Stopper { run_loop, source });
        }

        // Run until stopped. The loop services device notifications as they
        // arrive, and nothing else wakes the thread.
        while !stop.load(Ordering::SeqCst) {
            CFRunLoop::run_current();
        }
        // `_client` drops here, removing the CoreMIDI notification source; then
        // `coalescer` drops, joining its worker thread.
    }
}

// ── Linux: ALSA sequencer System:announce ─────────────────────────────────────
#[cfg(target_os = "linux")]
mod imp {
    use super::{Coalescer, OnChange};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread::{self, JoinHandle};

    use alsa::poll::Descriptors;
    use alsa::seq::{Addr, EventType, PortCap, PortSubscribe, PortType, Seq};
    use clockwork_wait::ready::{ReadyWait, MAX_SOCKS};

    /// The well-known ALSA sequencer System:announce source (client 0, port 1):
    /// it emits PortStart/PortExit/ClientStart/ClientExit as devices come and go.
    const SYSTEM_CLIENT: i32 = 0;
    const SYSTEM_ANNOUNCE_PORT: i32 = 1;

    pub struct PlatformWatcher {
        stop: Arc<AtomicBool>,
        // The thread blocks on this and the sequencer's descriptors; Drop
        // wakes it.
        wait: Option<Arc<ReadyWait>>,
        handle: Option<JoinHandle<()>>,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            let coalescer = Coalescer::new(on_change);
            let stop = Arc::new(AtomicBool::new(false));
            let Ok(wait) = ReadyWait::new().map(Arc::new) else {
                return PlatformWatcher { stop, wait: None, handle: None };
            };
            let (t_stop, t_wait) = (stop.clone(), wait.clone());
            // The loop owns the Seq and the coalescer so both live exactly as
            // long as the thread.
            let handle = thread::Builder::new()
                .name("cw-midi-watch".into())
                .spawn(move || announce_loop(t_stop, t_wait, coalescer))
                .ok();
            PlatformWatcher { stop, wait: Some(wait), handle }
        }
    }

    impl Drop for PlatformWatcher {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            if let Some(w) = &self.wait {
                w.wake();
            }
            if let Some(h) = self.handle.take() {
                let _ = h.join();
            }
        }
    }

    fn announce_loop(stop: Arc<AtomicBool>, wait: Arc<ReadyWait>, coalescer: Coalescer) {
        // No sequencer (a CI runner, a container): nothing to watch, and
        // asking ALSA prints "open /dev/snd/seq failed" on stderr for every
        // engine booted.
        if !crate::device::sequencer_present() {
            return;
        }
        let seq = match Seq::open(None, Some(alsa::Direction::Capture), true) {
            Ok(s) => s,
            Err(_) => return,
        };
        let _ = seq.set_client_name(
            &std::ffi::CString::new(crate::watcher::HOTSWAP_CLIENT_NAME).unwrap());

        // A local port to receive announcements on.
        let port = {
            use alsa::seq::PortInfo;
            let mut info = PortInfo::empty().unwrap();
            // NO_EXPORT: this port only receives kernel announcements —
            // third parties may not route through it, and well-behaved
            // enumerators hide it. (Our own listings filter it by client
            // name; midir's capability check doesn't exclude NO_EXPORT.)
            info.set_capability(PortCap::WRITE | PortCap::SUBS_WRITE | PortCap::NO_EXPORT);
            info.set_type(PortType::MIDI_GENERIC | PortType::APPLICATION);
            info.set_name(&std::ffi::CString::new("hotswap").unwrap());
            if seq.create_port(&info).is_err() {
                return;
            }
            info.get_port()
        };

        // Subscribe our port to System:announce.
        let sub = PortSubscribe::empty().unwrap();
        sub.set_sender(Addr { client: SYSTEM_CLIENT, port: SYSTEM_ANNOUNCE_PORT });
        sub.set_dest(Addr { client: seq.client_id().unwrap_or(0), port });
        if seq.subscribe_port(&sub).is_err() {
            return;
        }

        // The thread blocks on the sequencer's descriptors, which become
        // readable when an announcement arrives, and on the stop.
        let Ok(fds) = (&seq, Some(alsa::Direction::Capture)).get() else { return };
        let fds: Vec<i32> = fds.iter().map(|f| f.fd).collect();
        if fds.len() > MAX_SOCKS {
            return;
        }

        let poke = coalescer.poker();
        let mut input = seq.input();
        loop {
            if stop.load(Ordering::SeqCst) {
                return;
            }
            match input.event_input_pending(true) {
                Ok(0) => {
                    // Block until an announcement arrives or the watcher stops.
                    if wait.wait(&fds, None).is_err() {
                        return;
                    }
                    continue;
                }
                Err(_) => return,
                Ok(_) => {}
            }
            match input.event_input() {
                Ok(ev) => match ev.get_type() {
                    EventType::PortStart
                    | EventType::PortExit
                    | EventType::ClientStart
                    | EventType::ClientExit => {
                        let _ = poke.send(());
                    }
                    _ => {}
                },
                Err(_) => return,
            }
        }
    }
}

// ── Other targets: no native hot-swap (manual /clockwork/midi/refresh still works) ───────
#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
mod imp {
    use super::{Coalescer, OnChange};

    pub struct PlatformWatcher {
        _coalescer: Coalescer,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            PlatformWatcher { _coalescer: Coalescer::new(on_change) }
        }
    }
}

#[cfg(all(test, not(target_arch = "wasm32")))]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;

    fn counting() -> (Coalescer, Arc<AtomicU32>) {
        let calls = Arc::new(AtomicU32::new(0));
        let on_change: OnChange = {
            let calls = calls.clone();
            Arc::new(move || {
                calls.fetch_add(1, Ordering::SeqCst);
            })
        };
        (Coalescer::new(on_change), calls)
    }

    fn until(calls: &AtomicU32, n: u32) -> bool {
        let deadline = Instant::now() + Duration::from_secs(10);
        while calls.load(Ordering::SeqCst) < n && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(1));
        }
        calls.load(Ordering::SeqCst) >= n
    }

    #[test]
    fn a_stop_during_the_settle_wait_ends_it_at_once() {
        let (c, calls) = counting();
        c.poker().send(()).unwrap();
        // The burst has gone quiet and the first refresh has run, so the
        // settle wait has begun.
        assert!(until(&calls, 1));
        let t0 = Instant::now();
        drop(c);
        assert!(t0.elapsed() < SETTLE / 2, "drop waited out the settle");
        assert_eq!(calls.load(Ordering::SeqCst), 1, "a refresh ran after the stop");
    }

    #[test]
    fn an_event_during_the_settle_wait_starts_a_new_burst_after_it() {
        let (c, calls) = counting();
        c.poker().send(()).unwrap();
        assert!(until(&calls, 1));
        c.poker().send(()).unwrap();   // during the settle
        // The settle refresh, then the new burst's refresh and its settle
        // refresh.
        assert!(until(&calls, 4));
    }
}
