// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! One engine's core: made and closed here (docs/ENGINES.md.1).
//!
//! A core is everything behind `lanes.h` for one engine, and the handle every
//! entry point takes. A process holds as many as it makes, each independent
//! of the others; a wasm instance and a device hold one, whose memory is
//! the instance's or the device's own, and refuse a second.
//!
//! A handle outlives its core. Closed, it names no core, so a call that
//! passes it is refused rather than reaching freed memory; and checking a
//! handle is one load, which the audio thread does every block. Handles are
//! never freed: a word and a link each, the price of that check.
//!
//! The core holds its C++ state (`src/core_state.h`) as a pointer the C++
//! side hands over at the core's first `clockwork_init`, with the function
//! that frees it; closing the core frees it. Nothing here links against the
//! C++.
//!
//! The C side is `src/clockwork_core.h`.

use clockwork_fence::no_unwind;
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};
use std::sync::Mutex;

use clockwork_abi::client::ClockworkStatus;
use clockwork_heap::core_heap::CoreHeap;
use clockwork_heap::entry::{self, Entry, Kind};
use clockwork_wait::WaitWord;

/// What frees a core's C++ state.
pub type FreeState = unsafe extern "C" fn(state: *mut c_void);

/// One engine's handle. Opaque to C. Never freed, so what a thread must be
/// able to touch without knowing whether the core is still there lives here:
/// the gate, the count of threads inside without it, the closing flag, and
/// the words those threads wait on (docs/ENGINES.md).
pub struct ClockworkCore {
    /// The core, or null once it is closed.
    core: AtomicPtr<Core>,
    /// The handle made before this one: every handle ever made, for the
    /// walks over the live cores.
    older: *const ClockworkCore,
    /// The core's gate (docs/ENGINES.md): the right to work on
    /// the core's guest and its pools, one thread at a time. 1 while held.
    gate: WaitWord,
    /// Blocks that found the gate held and rendered nothing.
    busy: AtomicU32,
    /// Threads inside the core without the gate (a hold); a close waits for
    /// none, on `drained`.
    inside: AtomicU32,
    /// Set once, by the close that ends the core: no new gate, hold or wait
    /// from then on.
    closing: AtomicBool,
    drained: WaitWord,
    /// What a client thread sleeps on until the engine says something
    /// (clockwork_client_wait): bumped once per block that wrote RT-out and
    /// once per NRT-out write, and once more by a close, which every waiter
    /// then sees.
    said: WaitWord,
}

/// What taking the gate answered.
const GATE_TAKEN: c_int = 0;
const GATE_HELD: c_int = 1;
const GATE_CLOSED: c_int = 2;

impl ClockworkCore {
    /// Take the gate: at once, or waiting out whoever holds it. `GATE_CLOSED`
    /// once the core is closing, unless this is the close itself; a waiter
    /// that sees the close begin gives up the same way.
    fn take_gate(&self, wait: bool, for_close: bool) -> c_int {
        loop {
            if !for_close && self.closing.load(Ordering::Acquire) {
                return GATE_CLOSED;
            }
            if self.gate.value.compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire).is_ok() {
                return GATE_TAKEN;
            }
            if !wait {
                self.busy.fetch_add(1, Ordering::Relaxed);
                return GATE_HELD;
            }
            self.gate.wait(1);
        }
    }

    fn release_gate(&self) {
        self.gate.value.store(0, Ordering::SeqCst);
        self.gate.notify();
    }

    /// Count this thread inside the core, unless it is closing. The count
    /// goes up before the flag is read, and the close raises the flag before
    /// it reads the count, so one of them sees the other.
    fn hold(&self) -> bool {
        self.inside.fetch_add(1, Ordering::SeqCst);
        if self.closing.load(Ordering::SeqCst) {
            self.unhold();
            return false;
        }
        true
    }

    fn unhold(&self) {
        if self.inside.fetch_sub(1, Ordering::SeqCst) == 1 {
            self.drained.bump();
        }
    }
}

/// A thread's hold on a core while it reads what the core owns without the
/// gate, let go as it ends. None once the core is closing.
struct Hold<'a>(&'a ClockworkCore);

impl<'a> Hold<'a> {
    fn on(handle: &'a ClockworkCore) -> Option<Hold<'a>> {
        // Made only once the hold is taken: a Hold made and dropped for a core
        // that refused it would let go of a hold that was never its own.
        handle.hold().then(|| Hold(handle))
    }
}

impl Drop for Hold<'_> {
    fn drop(&mut self) {
        self.0.unhold();
    }
}

/// One engine's parts.
struct Core {
    /// Which core this is, for log lines and diagnostics: a process's cores
    /// are numbered from 1 in the order they were made.
    id: u32,
    /// The C++ state, or null before the core's first `clockwork_init`.
    state: AtomicPtr<c_void>,
    /// What frees `state` when the core closes.
    free_state: Mutex<Option<FreeState>>,
    /// The span the core's arena occupies, `(base, bytes)`: a client opened
    /// over memory in it is a client of this core.
    memory: Mutex<(usize, usize)>,
    /// Which handle owns each of the arena's tap slots.
    taps: clockwork_taps::Taps,
    /// The core's ports: counted, so a disk endpoint on one of them keeps
    /// the table while its worker runs.
    ports: std::sync::Arc<clockwork_ports::registry::Ports>,
    /// The core's event sinks, and what they are made by: the shapes, and
    /// the host's emitter while the host is the far end.
    sinks: std::sync::Arc<clockwork_sinks::registry::Sinks>,
    /// The core's memory: its guest's pool (docs/ENGINES.md).
    heap: CoreHeap,
    /// Whether the core holds one of the process's places for a guest
    /// instance (`GUESTS`).
    guest: AtomicBool,
}

/// Every handle ever made, newest first.
static HANDLES: AtomicPtr<ClockworkCore> = AtomicPtr::new(std::ptr::null_mut());

/// How many cores are live.
static LIVE: AtomicU32 = AtomicU32::new(0);

/// Whether this target holds one core per process: a wasm instance's memory
/// is its one engine's, and so is a device's static arena.
const ONE_CORE_PER_PROCESS: bool = cfg!(any(target_os = "emscripten", target_os = "espidf"));

/// The last id handed out.
static LAST_ID: AtomicU32 = AtomicU32::new(0);

/// Cores holding a guest instance, against how many the guest says one
/// process may hold (`DspInfo::instances`, docs/ENGINES.md).
static GUESTS: AtomicU32 = AtomicU32::new(0);

/// Make a core; `E_PERM` while another is live where a process holds one.
fn new() -> Result<*mut ClockworkCore, ClockworkStatus> {
    if ONE_CORE_PER_PROCESS
        && LIVE.compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire).is_err()
    {
        return Err(ClockworkStatus::E_PERM);
    }
    if !ONE_CORE_PER_PROCESS {
        LIVE.fetch_add(1, Ordering::AcqRel);
    }
    let id = LAST_ID.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    // The handle first: the core's services name it in their lines.
    let handle = Box::leak(Box::new(ClockworkCore {
        core: AtomicPtr::new(std::ptr::null_mut()),
        older: std::ptr::null(),
        gate: WaitWord::new(),
        busy: AtomicU32::new(0),
        inside: AtomicU32::new(0),
        closing: AtomicBool::new(false),
        drained: WaitWord::new(),
        said: WaitWord::new(),
    }));
    let core = Box::into_raw(Box::new(Core {
        id,
        state: AtomicPtr::new(std::ptr::null_mut()),
        free_state: Mutex::new(None),
        memory: Mutex::new((0, 0)),
        taps: clockwork_taps::Taps::default(),
        ports: std::sync::Arc::new(clockwork_ports::registry::Ports::new()),
        sinks: clockwork_sinks::registry::Sinks::new(std::ptr::from_mut(handle).cast()),
        heap: CoreHeap::new(),
        guest: AtomicBool::new(false),
    }));
    handle.core.store(core, Ordering::Release);
    let mut head = HANDLES.load(Ordering::Acquire);
    loop {
        handle.older = head;
        match HANDLES.compare_exchange_weak(head, handle, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return Ok(handle),
            Err(h) => head = h,
        }
    }
}

/// Close `core` (docs/ENGINES.md): refuse new entries, wake whoever
/// waits on the core, wait until no thread is inside it, then free it. Its
/// C++ state is freed — which ends the engine — while the core is still
/// live, so the engine's last words reach its rings; then it is no longer
/// live and its handle names nothing. A core that is not live (null, or
/// already closed) is left alone, and a second close at once returns at
/// once: the first is the close.
fn close(handle: *mut ClockworkCore) {
    // SAFETY: a handle is null or one `new` made, and handles are never freed.
    let Some(h) = (unsafe { handle.as_ref() }) else { return };
    if h.core.load(Ordering::Acquire).is_null() || h.closing.swap(true, Ordering::SeqCst) {
        return;
    }
    // Whoever waits on the core wakes, sees it closing and leaves; whoever
    // waits for the gate gives up the same way.
    h.said.bump();
    h.gate.notify();
    // The gate: no block or control call is in flight once it is ours, and
    // none begins. Then nobody inside without it.
    h.take_gate(true, true);
    h.drained.wait_for(|| h.inside.load(Ordering::SeqCst) == 0, None);
    // From here nothing but this thread reaches the core.
    let Some(c) = live(handle) else { return };
    let free = c.free_state.lock().map(|mut f| f.take()).unwrap_or(None);
    let state = c.state.load(Ordering::Acquire);
    if let (false, Some(free)) = (state.is_null(), free) {
        // The engine tears down through its core, as the gate's holder: a
        // control call it makes inside takes the gate for nothing.
        let outside = entry::enter(Entry {
            core: handle.cast(),
            heap: std::ptr::from_ref(&c.heap).cast(),
            kind: Kind::Control,
            gate: true,
        });
        // SAFETY: `free` is the function the C++ side handed over with
        // `state`, for exactly this; taking it gave it to this close alone,
        // and nothing else is inside the core.
        unsafe { free(state) };
        entry::leave(outside);
    }
    c.state.store(std::ptr::null_mut(), Ordering::Release);
    // A guest instance the state's free left behind, a dead engine's one
    // forgotten on the web, goes with the core.
    give_guest(c);
    let core = h.core.swap(std::ptr::null_mut(), Ordering::AcqRel);
    LIVE.fetch_sub(1, Ordering::AcqRel);
    // SAFETY: made by Box::into_raw in `new`; the swap above unpublished
    // it and the latch above emptied the core, so this is the one drop.
    drop(unsafe { Box::from_raw(core) });
    // The gate stays the handle's; closed, it is never taken again.
    h.release_gate();
}

/// Take a place for `c`'s guest instance among the `allowed` one process may
/// hold. A core holds one place, so its own rebuild is never refused.
fn take_guest(c: &Core, allowed: u32) -> bool {
    if c.guest.load(Ordering::Acquire) {
        return true;
    }
    let mut held = GUESTS.load(Ordering::Acquire);
    loop {
        if held >= allowed {
            return false;
        }
        match GUESTS.compare_exchange_weak(held, held + 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => break,
            Err(now) => held = now,
        }
    }
    c.guest.store(true, Ordering::Release);
    true
}

/// Give back `c`'s place, if it holds one.
fn give_guest(c: &Core) {
    if c.guest.swap(false, Ordering::AcqRel) {
        GUESTS.fetch_sub(1, Ordering::AcqRel);
    }
}

/// `handle`'s core, when it is live. For a caller inside the core — holding
/// its gate, or a `Hold` — which a close waits out before the core goes.
fn live(handle: *const ClockworkCore) -> Option<&'static Core> {
    // SAFETY: a handle is null or one `new` made, and handles are never
    // freed; its core is a Box that only `close` frees, after every thread
    // inside has left (the gate and `inside`, above).
    unsafe { handle.as_ref()?.core.load(Ordering::Acquire).as_ref() }
}

/// `handle` itself, when it names one.
fn handle_of(handle: *const ClockworkCore) -> Option<&'static ClockworkCore> {
    // SAFETY: as `live`: null or one `new` made, never freed.
    unsafe { handle.as_ref() }
}

/// Every handle whose core is live, newest first.
fn live_handles() -> impl Iterator<Item = *mut ClockworkCore> {
    let mut h = HANDLES.load(Ordering::Acquire);
    std::iter::from_fn(move || {
        while !h.is_null() {
            let this = h;
            // SAFETY: published whole by `new`, and never freed.
            let handle = unsafe { &*this };
            h = handle.older.cast_mut();
            if !handle.core.load(Ordering::Acquire).is_null() {
                return Some(this);
            }
        }
        None
    })
}

fn adopt_state(core: *const ClockworkCore, state: *mut c_void, free: Option<FreeState>) -> bool {
    let Some(core) = live(core) else { return false };
    if state.is_null() {
        return false;
    }
    let Ok(mut slot) = core.free_state.lock() else { return false };
    if core.state.compare_exchange(std::ptr::null_mut(), state, Ordering::AcqRel, Ordering::Acquire).is_err() {
        return false;
    }
    *slot = free;
    true
}

fn of_memory(at: usize) -> *mut ClockworkCore {
    live_handles()
        .find(|&h| {
            // Held while its span is read: a core closing meanwhile is not
            // the one, and is not freed under the read.
            let Some(handle) = handle_of(h) else { return false };
            let Some(_hold) = Hold::on(handle) else { return false };
            live(h).is_some_and(|c| {
                let (base, bytes) = c.memory.lock().map(|m| *m).unwrap_or((0, 0));
                bytes != 0 && at >= base && at - base < bytes
            })
        })
        .unwrap_or(std::ptr::null_mut())
}

// ── The C side (src/clockwork_core.h) ───────────────────────────────────────
// A panic must not reach the host: every call is the fence (clockwork-fence).

/// Make a core. NULL with `*status` set (when given) to `E_PERM` while
/// another is live, `E_NOMEM` if the core could not be made.
///
/// # Safety
/// `status` is null or points at a writable `ClockworkStatus`.
#[no_mangle]
pub unsafe extern "C" fn clockwork_core_new(status: *mut ClockworkStatus) -> *mut ClockworkCore {
    no_unwind(std::ptr::null_mut(), || {
        // What the caller is told if what follows panics.
        if !status.is_null() {
            // SAFETY: the caller's contract: null or writable.
            unsafe { *status = ClockworkStatus::E_NOMEM };
        }
        let (core, st) = match new() {
            Ok(core) => (core, ClockworkStatus::OK),
            Err(st) => (std::ptr::null_mut(), st),
        };
        if !status.is_null() {
            // SAFETY: as above.
            unsafe { *status = st };
        }
        core
    })
}

/// Close a core made by `clockwork_core_new`, freeing its C++ state. Null, or
/// a core already closed, is ignored.
#[no_mangle]
pub extern "C" fn clockwork_core_close(core: *mut ClockworkCore) {
    no_unwind((), || {
        close(core)
    })
}

/// Take `core`'s gate (clockwork_core.h): 0 taken; 1 held by another
/// thread, counted as a block that rendered nothing, when `wait` is false;
/// 2 when the core is closing or gone. With `wait` the call sleeps until it
/// takes the gate or the core begins to close. Never from the thread that
/// holds it: that thread's entry says so, and core_state.h's GateHold reads
/// the entry first.
#[no_mangle]
pub extern "C" fn clockwork_core_gate_take(core: *const ClockworkCore, wait: bool) -> c_int {
    no_unwind(GATE_CLOSED, || handle_of(core).map_or(GATE_CLOSED, |h| h.take_gate(wait, false)))
}

/// Give `core`'s gate back, from the thread that took it.
#[no_mangle]
pub extern "C" fn clockwork_core_gate_release(core: *const ClockworkCore) {
    no_unwind((), || {
        if let Some(h) = handle_of(core) {
            h.release_gate();
        }
    })
}

/// Whether `core`'s gate is held, by anyone.
#[no_mangle]
pub extern "C" fn clockwork_core_gate_held(core: *const ClockworkCore) -> bool {
    no_unwind(false, || handle_of(core).is_some_and(|h| h.gate.value.load(Ordering::Acquire) == 1))
}

/// Blocks that found `core`'s gate held and rendered nothing.
#[no_mangle]
pub extern "C" fn clockwork_core_gate_busy(core: *const ClockworkCore) -> u32 {
    no_unwind(0, || handle_of(core).map_or(0, |h| h.busy.load(Ordering::Acquire)))
}

/// Threads waiting for `core`'s gate.
#[no_mangle]
pub extern "C" fn clockwork_core_gate_waiters(core: *const ClockworkCore) -> u32 {
    no_unwind(0, || handle_of(core).map_or(0, |h| h.gate.waiters.load(Ordering::SeqCst)))
}

/// Count this thread inside `core` without its gate, for what it reads of
/// the core until clockwork_core_unhold: a close waits for it. False once
/// the core is closing, when nothing of it may be read.
#[no_mangle]
pub extern "C" fn clockwork_core_hold(core: *const ClockworkCore) -> bool {
    no_unwind(false, || handle_of(core).is_some_and(ClockworkCore::hold))
}

/// Let go of a hold.
#[no_mangle]
pub extern "C" fn clockwork_core_unhold(core: *const ClockworkCore) {
    no_unwind((), || {
        if let Some(h) = handle_of(core) {
            h.unhold();
        }
    })
}

/// Whether `core` has begun to close (or is closed, or null).
#[no_mangle]
pub extern "C" fn clockwork_core_closing(core: *const ClockworkCore) -> bool {
    no_unwind(true, || handle_of(core).is_none_or(|h| h.closing.load(Ordering::Acquire)))
}

/// The engine said something: move `core`'s word and wake every waiter.
/// Any thread, the audio thread included; never blocks.
#[no_mangle]
pub extern "C" fn clockwork_core_said_bump(core: *const ClockworkCore) {
    no_unwind((), || {
        if let Some(h) = handle_of(core) {
            h.said.bump();
        }
    })
}

/// `core`'s said word now.
#[no_mangle]
pub extern "C" fn clockwork_core_said_value(core: *const ClockworkCore) -> u32 {
    no_unwind(0, || handle_of(core).map_or(0, |h| h.said.value.load(Ordering::SeqCst)))
}

/// Threads waiting on `core`'s said word.
#[no_mangle]
pub extern "C" fn clockwork_core_said_waiters(core: *const ClockworkCore) -> u32 {
    no_unwind(0, || handle_of(core).map_or(0, |h| h.said.waiters.load(Ordering::SeqCst)))
}

/// Sleep until `core` has said something since the word read `old`, or
/// `timeout_ms` passes (u32::MAX: no deadline), or the core begins to close:
/// 1, 0 and -1. Held while it sleeps, so the core is not freed under it;
/// -1 at once for a core already closing.
#[no_mangle]
pub extern "C" fn clockwork_core_said_wait(core: *const ClockworkCore, old: u32, timeout_ms: u32) -> c_int {
    no_unwind(-1, || {
        let Some(h) = handle_of(core) else { return -1 };
        let Some(_hold) = Hold::on(h) else { return -1 };
        let deadline = (timeout_ms != u32::MAX)
            .then(|| std::time::Instant::now() + std::time::Duration::from_millis(u64::from(timeout_ms)));
        let moved = h.said.wait_for(
            || h.said.value.load(Ordering::SeqCst) != old || h.closing.load(Ordering::SeqCst),
            deadline,
        );
        if h.closing.load(Ordering::SeqCst) {
            -1
        } else {
            c_int::from(moved)
        }
    })
}

/// Whether `core` is live: made and not yet closed.
#[no_mangle]
pub extern "C" fn clockwork_core_live(core: *const ClockworkCore) -> bool {
    no_unwind(false, || live(core).is_some())
}

/// `core`'s id, numbered from 1 in the order the process made its cores; 0
/// when `core` is not live.
#[no_mangle]
pub extern "C" fn clockwork_core_id(core: *const ClockworkCore) -> u32 {
    no_unwind(0, || live(core).map_or(0, |c| c.id))
}

/// `core`'s tap slots, for the tap calls (clockwork-taps), or null when
/// `core` is not live. Any thread.
#[no_mangle]
pub extern "C" fn clockwork_core_taps(core: *const ClockworkCore) -> *const clockwork_taps::Taps {
    no_unwind(std::ptr::null(), || live(core).map_or(std::ptr::null(), |c| &c.taps as *const _))
}

/// `core`'s ports, for the port calls (clockwork-ports), or null when `core`
/// is not live. Any thread.
#[no_mangle]
pub extern "C" fn clockwork_core_ports(core: *const ClockworkCore) -> *const clockwork_ports::registry::Ports {
    no_unwind(std::ptr::null(), || live(core).map_or(std::ptr::null(), |c| std::sync::Arc::as_ptr(&c.ports)))
}

/// `core`'s event sinks, for the sink calls (clockwork-sinks), or null when
/// `core` is not live. Any thread.
#[no_mangle]
pub extern "C" fn clockwork_core_sinks(core: *const ClockworkCore) -> *const clockwork_sinks::registry::Sinks {
    no_unwind(std::ptr::null(), || live(core).map_or(std::ptr::null(), |c| std::sync::Arc::as_ptr(&c.sinks)))
}

/// Take a place for `core`'s guest instance, before it is built, among the
/// `allowed` one process may hold (`DSP_INSTANCES_ANY`, the most a count
/// holds, is any). False when
/// they are all held by other cores, or `core` is not live. A core already
/// holding one keeps it. The core's control thread.
#[no_mangle]
pub extern "C" fn clockwork_core_guest_take(core: *const ClockworkCore, allowed: u32) -> bool {
    no_unwind(false, || live(core).is_some_and(|c| take_guest(c, allowed)))
}

/// Give back `core`'s place: its instance is freed, or was never built.
/// Closing the core gives it back too. The core's control thread.
#[no_mangle]
pub extern "C" fn clockwork_core_guest_give(core: *const ClockworkCore) {
    no_unwind((), || {
        if let Some(c) = live(core) {
            give_guest(c);
        }
    })
}

/// `core`'s heap, for the heap calls (clockwork-heap), or null when `core`
/// is not live. Any thread.
#[no_mangle]
pub extern "C" fn clockwork_core_heap(core: *const ClockworkCore) -> *const CoreHeap {
    no_unwind(std::ptr::null(), || live(core).map_or(std::ptr::null(), |c| &c.heap as *const _))
}

/// `core`'s C++ state, or null before its first `clockwork_init` or when
/// `core` is not live. Any thread, the audio thread's included: two atomic
/// loads.
#[no_mangle]
pub extern "C" fn clockwork_core_state(core: *const ClockworkCore) -> *mut c_void {
    no_unwind(std::ptr::null_mut(), || {
        live(core).map_or(std::ptr::null_mut(), |c| c.state.load(Ordering::Acquire))
    })
}

/// Hand `core` its C++ state, and what frees it when the core closes. Once:
/// false when `core` is not live, `state` is null, or the core has one.
#[no_mangle]
pub extern "C" fn clockwork_core_adopt_state(
    core: *const ClockworkCore,
    state: *mut c_void,
    free: Option<FreeState>,
) -> bool {
    no_unwind(false, || adopt_state(core, state, free))
}

/// Say where `core`'s arena is, so a client opened over it finds its core
/// (`clockwork_core_of_memory`). `bytes` 0 says it has none.
#[no_mangle]
pub extern "C" fn clockwork_core_set_memory(core: *const ClockworkCore, base: *const c_void, bytes: usize) {
    no_unwind((), || {
        if let Some(c) = live(core) {
            if let Ok(mut m) = c.memory.lock() {
                *m = (base as usize, bytes);
            }
        }
    })
}

/// The live core whose arena holds `at`, or null: how a client opened over
/// memory in this process finds its core. Memory of another process's
/// engine has none here.
#[no_mangle]
pub extern "C" fn clockwork_core_of_memory(at: *const c_void) -> *mut ClockworkCore {
    no_unwind(std::ptr::null_mut(), || of_memory(at as usize))
}

/// `ClockworkEntryKind`: what kind of call a thread is in. A newtype, as the
/// client's enums are: a kind the header does not list must be a value here,
/// not undefined behaviour.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkEntryKind(pub c_int);

/// `ClockworkEntry`: a thread's entry as C sees it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkEntry {
    pub core: *mut ClockworkCore,
    pub kind: ClockworkEntryKind,
    pub gate: bool,
}

impl From<Entry> for ClockworkEntry {
    fn from(e: Entry) -> ClockworkEntry {
        ClockworkEntry { core: e.core.cast(), kind: ClockworkEntryKind(e.kind as c_int), gate: e.gate }
    }
}

impl From<ClockworkEntry> for Entry {
    /// The core's heap comes with it, none for a core that is not live; a
    /// kind that names none is control's: what it allocates is the system's.
    fn from(e: ClockworkEntry) -> Entry {
        let kind = u8::try_from(e.kind.0).ok().and_then(Kind::from_u8).unwrap_or(Kind::Control);
        let heap = live(e.core).map_or(std::ptr::null(), |c| std::ptr::from_ref(&c.heap).cast());
        Entry { core: e.core.cast(), heap, kind, gate: e.gate }
    }
}

/// Whether `e` is inside a core without its gate, and so counts as a hold on
/// it (`inside`): the gate's holder is already waited out by the gate. An
/// entry into the core the thread is already in holds nothing more.
fn is_hold(e: &Entry, outer: &Entry) -> bool {
    !e.core.is_null() && !e.gate && e.core != outer.core
}

/// Make `entry` this thread's, for a call that works inside its core
/// (docs/ENGINES.md). Returns the entry the thread had, to give back to
/// [`clockwork_core_leave`] as the call returns. An entry into a core
/// without its gate holds the core (clockwork_core_hold) until it is left;
/// into a core that is closing it is an entry into no core.
#[no_mangle]
pub extern "C" fn clockwork_core_enter(entry: ClockworkEntry) -> ClockworkEntry {
    no_unwind(entry::current().into(), || {
        let mut e: Entry = entry.into();
        let outer = entry::current();
        if is_hold(&e, &outer) && !handle_of(e.core.cast()).is_some_and(ClockworkCore::hold) {
            e = Entry { kind: e.kind, ..Entry::NONE };
        }
        entry::enter(e).into()
    })
}

/// Leave the entry made last; `previous` is what [`clockwork_core_enter`]
/// returned.
#[no_mangle]
pub extern "C" fn clockwork_core_leave(previous: ClockworkEntry) {
    no_unwind((), || {
        let outer: Entry = previous.into();
        let here = entry::current();
        if is_hold(&here, &outer) {
            if let Some(h) = handle_of(here.core.cast()) {
                h.unhold();
            }
        }
        entry::leave(outer);
    })
}

/// This thread's entry: no core, control, and no gate, when it is inside none.
#[no_mangle]
pub extern "C" fn clockwork_core_entry() -> ClockworkEntry {
    no_unwind(Entry::NONE.into(), || {
        entry::current().into()
    })
}

/// The core this thread is inside, or null.
#[no_mangle]
pub extern "C" fn clockwork_core_current() -> *mut ClockworkCore {
    no_unwind(core::ptr::null_mut(), || {
        entry::current().core.cast()
    })
}

/// The core this wasm memory holds, or null: for a worklet made again over
/// the same memory, which comes back to that core rather than making
/// another. A wasm instance holds one core, so there is no choice to make.
#[cfg(target_os = "emscripten")]
#[no_mangle]
pub extern "C" fn clockwork_core_held() -> *mut ClockworkCore {
    no_unwind(core::ptr::null_mut(), || {
        live_handles().next().unwrap_or(std::ptr::null_mut())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_abi::dsp_api::DSP_INSTANCES_ANY;
    use std::sync::atomic::AtomicUsize;

    // The registry is the process's; the tests take turns at it.
    static TURN: Mutex<()> = Mutex::new(());

    fn make() -> *mut ClockworkCore {
        // SAFETY: a null status is allowed.
        let core = unsafe { clockwork_core_new(std::ptr::null_mut()) };
        assert!(!core.is_null());
        core
    }

    #[test]
    fn a_core_is_live_from_new_until_close() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        assert!(clockwork_core_live(core));
        assert!(clockwork_core_id(core) > 0);
        assert!(!clockwork_core_taps(core).is_null());
        assert!(!clockwork_core_ports(core).is_null());
        assert!(!clockwork_core_sinks(core).is_null());
        clockwork_core_close(core);
        assert!(!clockwork_core_live(core));
        assert_eq!(clockwork_core_id(core), 0);
        assert!(clockwork_core_taps(core).is_null());
        assert!(clockwork_core_ports(core).is_null());
        assert!(clockwork_core_sinks(core).is_null());
    }

    // Many cores at once, each its own: its own id, state, tables and heap,
    // and closing one leaves the others live.
    #[test]
    fn cores_live_at_once_are_each_their_own() {
        let _turn = TURN.lock().unwrap();
        let (a, b, c) = (make(), make(), make());
        assert!(clockwork_core_live(a) && clockwork_core_live(b) && clockwork_core_live(c));
        let ids = [clockwork_core_id(a), clockwork_core_id(b), clockwork_core_id(c)];
        assert!(ids[0] < ids[1] && ids[1] < ids[2]);
        assert_ne!(clockwork_core_heap(a), clockwork_core_heap(b));
        assert_ne!(clockwork_core_ports(a), clockwork_core_ports(b));
        assert_ne!(clockwork_core_sinks(a), clockwork_core_sinks(b));
        assert!(clockwork_core_adopt_state(a, 0x10 as *mut c_void, None));
        assert!(clockwork_core_state(b).is_null(), "a's state reached b");
        clockwork_core_close(b);
        assert!(clockwork_core_live(a) && !clockwork_core_live(b) && clockwork_core_live(c));
        assert_eq!(clockwork_core_state(a), 0x10 as *mut c_void);
        clockwork_core_close(a);
        clockwork_core_close(c);
    }

    // A guest a process holds once (DspInfo::instances = 1): one core holds
    // the place, keeps it through its own rebuild, and another core gets it
    // only once the first gives it back or closes.
    #[test]
    fn a_guest_place_is_one_core_s_until_it_gives_it_back_or_closes() {
        let _turn = TURN.lock().unwrap();
        let (a, b) = (make(), make());
        assert!(clockwork_core_guest_take(a, 1));
        assert!(clockwork_core_guest_take(a, 1), "its own rebuild");
        assert!(!clockwork_core_guest_take(b, 1));
        assert!(!clockwork_core_guest_take(b, 0), "no place to take");
        assert!(clockwork_core_guest_take(b, DSP_INSTANCES_ANY), "a guest any number of can always be built");
        clockwork_core_guest_give(b);
        clockwork_core_guest_give(a);
        clockwork_core_guest_give(a);
        assert!(clockwork_core_guest_take(b, 1));
        clockwork_core_close(b);
        assert!(clockwork_core_guest_take(a, 1), "a core that closed gave its place back");
        clockwork_core_close(a);
        assert!(!clockwork_core_guest_take(a, 1), "a closed core takes nothing");
    }

    #[test]
    fn closing_twice_or_closing_null_does_nothing() {
        let _turn = TURN.lock().unwrap();
        clockwork_core_close(std::ptr::null_mut());
        let core = make();
        let id = clockwork_core_id(core);
        clockwork_core_close(core);
        clockwork_core_close(core);
        let next = make();
        assert!(clockwork_core_id(next) > id);
        clockwork_core_close(next);
    }

    static FREED: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn free_counted(state: *mut c_void) {
        FREED.fetch_add(state as usize, Ordering::SeqCst);
    }

    #[test]
    fn a_core_adopts_its_state_once_and_frees_it_when_it_closes() {
        let _turn = TURN.lock().unwrap();
        FREED.store(0, Ordering::SeqCst);
        let core = make();
        assert!(clockwork_core_state(core).is_null());
        let state = 0x40 as *mut c_void;
        assert!(clockwork_core_adopt_state(core, state, Some(free_counted)));
        assert_eq!(clockwork_core_state(core), state);
        assert!(!clockwork_core_adopt_state(core, 0x80 as *mut c_void, Some(free_counted)));
        assert_eq!(FREED.load(Ordering::SeqCst), 0);
        clockwork_core_close(core);
        assert_eq!(FREED.load(Ordering::SeqCst), 0x40);
        assert!(clockwork_core_state(core).is_null());
        assert!(!clockwork_core_adopt_state(core, state, Some(free_counted)));
    }

    // The latch (docs/ENGINES.md): a thread inside the core without the
    // gate holds it; a close refuses new holds, wakes whoever waits on the
    // core, and frees nothing until the last hold is let go.
    #[test]
    fn a_close_waits_for_every_hold_and_refuses_new_ones() {
        let _turn = TURN.lock().unwrap();
        FREED.store(0, Ordering::SeqCst);
        let core = make();
        assert!(clockwork_core_adopt_state(core, 0x8 as *mut c_void, Some(free_counted)));
        assert!(clockwork_core_hold(core));
        let at = core as usize;
        let closer = std::thread::spawn(move || clockwork_core_close(at as *mut ClockworkCore));
        // The close has begun once a new hold is refused; the core is still
        // there, unfreed, for the hold that is still inside.
        while clockwork_core_hold(core) {
            clockwork_core_unhold(core);
            std::thread::yield_now();
        }
        assert!(clockwork_core_closing(core));
        assert!(clockwork_core_live(core), "freed under a hold");
        assert_eq!(FREED.load(Ordering::SeqCst), 0);
        assert_eq!(clockwork_core_gate_take(core, false), GATE_CLOSED, "a gate while closing");
        clockwork_core_unhold(core);
        closer.join().unwrap();
        assert!(!clockwork_core_live(core));
        assert_eq!(FREED.load(Ordering::SeqCst), 0x8);
        assert_eq!(clockwork_core_said_wait(core, 0, 10), -1, "a wait on a closed core");
    }

    // A search over every core (a client opened over memory looks for its
    // core) meets one that is closing with a thread still inside: it is looked
    // past, and the look takes nothing from that thread's hold.
    #[test]
    fn a_search_past_a_closing_core_leaves_its_holds_alone() {
        let _turn = TURN.lock().unwrap();
        FREED.store(0, Ordering::SeqCst);
        let core = make();
        assert!(clockwork_core_adopt_state(core, 0x10 as *mut c_void, Some(free_counted)));
        assert!(clockwork_core_hold(core));
        let at = core as usize;
        let closer = std::thread::spawn(move || clockwork_core_close(at as *mut ClockworkCore));
        while clockwork_core_hold(core) {
            clockwork_core_unhold(core);
            std::thread::yield_now();
        }
        assert!(clockwork_core_of_memory(0x1000 as *const c_void).is_null(), "no core has that memory");
        assert_eq!(handle_of(core).unwrap().inside.load(Ordering::SeqCst), 1, "the hold still stands");
        assert!(clockwork_core_live(core), "freed under a hold");
        assert_eq!(FREED.load(Ordering::SeqCst), 0);
        clockwork_core_unhold(core);
        closer.join().unwrap();
        assert!(!clockwork_core_live(core));
        assert_eq!(FREED.load(Ordering::SeqCst), 0x10);
    }

    #[test]
    fn a_waiter_on_the_core_is_woken_by_its_close_and_told() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        let seen = clockwork_core_said_value(core);
        let at = core as usize;
        let waiter = std::thread::spawn(move || clockwork_core_said_wait(at as *const ClockworkCore, seen, u32::MAX));
        // Parked, then closed under it.
        let h = handle_of(core).unwrap();
        while h.said.waiters.load(Ordering::SeqCst) == 0 {
            std::thread::yield_now();
        }
        clockwork_core_close(core);
        assert_eq!(waiter.join().unwrap(), -1);
    }

    #[test]
    fn the_gate_is_the_handles_and_a_waiter_for_it_gives_up_at_a_close() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        assert_eq!(clockwork_core_gate_take(core, false), GATE_TAKEN);
        assert!(clockwork_core_gate_held(core));
        assert_eq!(clockwork_core_gate_take(core, false), GATE_HELD);
        assert_eq!(clockwork_core_gate_busy(core), 1);
        let h = handle_of(core).unwrap();
        let at = core as usize;
        let waiter = std::thread::spawn(move || clockwork_core_gate_take(at as *const ClockworkCore, true));
        while h.gate.waiters.load(Ordering::SeqCst) == 0 {
            std::thread::yield_now();
        }
        assert_eq!(clockwork_core_gate_waiters(core), 1);
        // The close takes the gate itself once the block gives it back; the
        // thread that waited for it is told the core closed instead.
        let closer = std::thread::spawn(move || clockwork_core_close(at as *mut ClockworkCore));
        while !clockwork_core_closing(core) {
            std::thread::yield_now();
        }
        clockwork_core_gate_release(core);
        assert_eq!(waiter.join().unwrap(), GATE_CLOSED);
        closer.join().unwrap();
        assert!(!clockwork_core_live(core));
    }

    #[test]
    fn two_closes_at_once_free_once_and_both_return() {
        let _turn = TURN.lock().unwrap();
        FREED.store(0, Ordering::SeqCst);
        let core = make();
        assert!(clockwork_core_adopt_state(core, 0x8 as *mut c_void, Some(free_counted)));
        let at = core as usize;
        let (a, b) = (std::thread::spawn(move || clockwork_core_close(at as *mut ClockworkCore)),
                      std::thread::spawn(move || clockwork_core_close(at as *mut ClockworkCore)));
        a.join().unwrap();
        b.join().unwrap();
        assert_eq!(FREED.load(Ordering::SeqCst), 0x8);
        assert!(!clockwork_core_live(core));
    }

    // An entry into a core without its gate is a hold on it.
    #[test]
    fn an_entry_without_the_gate_holds_the_core_until_it_is_left() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        let outside = clockwork_core_enter(ClockworkEntry { core, kind: ClockworkEntryKind(0), gate: false });
        assert_eq!(handle_of(core).unwrap().inside.load(Ordering::SeqCst), 1);
        let within = clockwork_core_enter(ClockworkEntry { core, kind: ClockworkEntryKind(2), gate: false });
        assert_eq!(handle_of(core).unwrap().inside.load(Ordering::SeqCst), 1, "the same core, held once");
        clockwork_core_leave(within);
        clockwork_core_leave(outside);
        assert_eq!(handle_of(core).unwrap().inside.load(Ordering::SeqCst), 0);
        clockwork_core_close(core);
        let refused = clockwork_core_enter(ClockworkEntry { core, kind: ClockworkEntryKind(0), gate: false });
        assert!(clockwork_core_current().is_null(), "an entry into a closed core is into none");
        clockwork_core_leave(refused);
    }

    static CLOSING: AtomicPtr<ClockworkCore> = AtomicPtr::new(std::ptr::null_mut());
    static LIVE_WHILE_FREED: AtomicUsize = AtomicUsize::new(0);
    static STATE_WHILE_FREED: AtomicUsize = AtomicUsize::new(0);
    unsafe extern "C" fn free_while_live(_state: *mut c_void) {
        let core = CLOSING.load(Ordering::SeqCst);
        LIVE_WHILE_FREED.store(if clockwork_core_live(core) { 1 } else { 2 }, Ordering::SeqCst);
        STATE_WHILE_FREED.store(clockwork_core_state(core) as usize, Ordering::SeqCst);
    }

    // The free tears the engine down through the core, as the C++ does
    // (teardown_memory): the core is live and its state is its own until it
    // returns.
    #[test]
    fn a_closing_core_frees_its_state_while_still_live_so_the_engine_can_say_its_last() {
        let _turn = TURN.lock().unwrap();
        LIVE_WHILE_FREED.store(0, Ordering::SeqCst);
        STATE_WHILE_FREED.store(0, Ordering::SeqCst);
        let core = make();
        CLOSING.store(core, Ordering::SeqCst);
        assert!(clockwork_core_adopt_state(core, 0x10 as *mut c_void, Some(free_while_live)));
        clockwork_core_close(core);
        assert_eq!(LIVE_WHILE_FREED.load(Ordering::SeqCst), 1);
        assert_eq!(STATE_WHILE_FREED.load(Ordering::SeqCst), 0x10, "the state was gone while it was freed");
        assert!(!clockwork_core_live(core));
        assert!(clockwork_core_state(core).is_null());
    }

    #[test]
    fn a_client_finds_its_core_by_the_memory_it_opens() {
        let _turn = TURN.lock().unwrap();
        let other = make();
        let core = make();
        let arena = [0u8; 64];
        let at = arena.as_ptr().cast::<c_void>();
        assert!(clockwork_core_of_memory(at).is_null());
        clockwork_core_set_memory(core, at, arena.len());
        assert_eq!(clockwork_core_of_memory(at), core);
        assert_eq!(clockwork_core_of_memory(arena[63..].as_ptr().cast()), core);
        assert!(clockwork_core_of_memory(arena.as_ptr().wrapping_add(64).cast()).is_null());
        clockwork_core_close(core);
        assert!(clockwork_core_of_memory(at).is_null());
        clockwork_core_close(other);
    }

    #[test]
    fn an_entry_made_from_c_gives_the_callers_back() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        let render = ClockworkEntry { core, kind: ClockworkEntryKind(Kind::Render as c_int), gate: true };
        let door = ClockworkEntry { kind: ClockworkEntryKind(Kind::Door as c_int), ..render };
        let outside = clockwork_core_enter(render);
        assert_eq!(outside, ClockworkEntry { core: std::ptr::null_mut(), kind: ClockworkEntryKind(0), gate: false });
        let block = clockwork_core_enter(door);
        assert_eq!(block, render);
        assert_eq!(clockwork_core_entry(), door);
        clockwork_core_leave(block);
        assert_eq!(clockwork_core_entry(), render);
        clockwork_core_leave(outside);
        assert!(clockwork_core_current().is_null());
        clockwork_core_close(core);
    }

    #[test]
    fn an_entry_carries_its_live_cores_heap_and_none_for_another() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        let outside = clockwork_core_enter(ClockworkEntry { core, kind: ClockworkEntryKind(0), gate: true });
        assert_eq!(entry::current().heap, clockwork_core_heap(core).cast());
        assert!(!entry::current().heap.is_null());
        clockwork_core_leave(outside);
        let stranger = make();
        clockwork_core_close(stranger);
        let outside = clockwork_core_enter(ClockworkEntry { core: stranger, kind: ClockworkEntryKind(0), gate: false });
        assert!(entry::current().heap.is_null());
        clockwork_core_leave(outside);
        clockwork_core_close(core);
    }

    #[test]
    fn a_kind_from_c_that_names_none_is_entered_as_control() {
        let _turn = TURN.lock().unwrap();
        let core = make();
        for unknown in [5, -1, 256 + Kind::Door as c_int] {
            let outside = clockwork_core_enter(ClockworkEntry { core, kind: ClockworkEntryKind(unknown), gate: true });
            assert_eq!(entry::current(), Entry { core: core.cast(), heap: clockwork_core_heap(core).cast(), kind: Kind::Control, gate: true });
            clockwork_core_leave(outside);
        }
        clockwork_core_close(core);
    }
}
