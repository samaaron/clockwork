// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A word a thread blocks on until it changes, with a deadline.
//!
//! This is `src/workers/wait_word.h` in Rust: the same two words in the same
//! order and the same protocol, so either language can wait on or wake a word
//! in shared memory. Every wait in clockwork for something another thread
//! does blocks on the word that changes; nothing sleeps for a fixed time and
//! looks again.
//!
//! It uses the operating system's address wait: futex on Linux and Android,
//! `os_sync_wait_on_address` on Apple (macOS 14.4, iOS 17.4), `WaitOnAddress`
//! on Windows. On a target with none (a lean, single-threaded build, a
//! browser's main thread) there is nothing to wait for, and a wait returns at
//! once.
//!
//! WAKING IS CHEAP WHEN NOBODY WAITS. A waiter counts itself in `waiters`
//! before it looks at the value, and a waker looks at `waiters` after it
//! changes the value, both sequentially consistent, so one of them always
//! sees the other. A wake with no waiter costs two atomic operations and a
//! fence, and no system call, which is what lets the audio thread wake a
//! worker.
//!
//! SHARED. To wake a waiter in another process that maps the word, use
//! [`Scope::Shared`]. Windows has no cross-process address wait, so there a
//! shared word comes with an event handle each process holds; elsewhere the
//! handle is unused.
//!
//! A wait may return early (a signal, a spurious wake), so every caller looks
//! again at what it waits for; [`WaitWord::wait_for`] does that for it.

#[cfg(not(loom))]
use core::sync::atomic::{fence, AtomicBool, AtomicU32, Ordering::{Relaxed, SeqCst}};
#[cfg(loom)]
use loom::sync::atomic::{fence, AtomicBool, AtomicU32, Ordering::{Relaxed, SeqCst}};
use std::time::Instant;

/// Socket and device threads sleep on their descriptors and a stop here,
/// where other threads sleep on a word.
#[cfg(all(not(loom), any(unix, windows)))]
pub mod ready;

/// The word, and the count of threads blocked on it. `ClockworkWaitWord` in
/// `wait_word.h`, byte for byte.
#[repr(C)]
#[derive(Debug, Default)]
pub struct WaitWord {
    pub value: AtomicU32,
    pub waiters: AtomicU32,
    /// Under loom, the kernel's side of the word (`os`, below).
    #[cfg(loom)]
    kernel: os::Kernel,
}

#[cfg(not(loom))]
const _: () = assert!(core::mem::size_of::<WaitWord>() == 8);

/// Who may be waiting on a word.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scope {
    /// Threads of this process.
    Process,
    /// A thread in another process mapping the same memory. On Windows the
    /// word's event (a `HANDLE`) is what wakes it.
    Shared(*mut core::ffi::c_void),
}

impl WaitWord {
    #[cfg(not(loom))]
    pub const fn new() -> Self {
        WaitWord { value: AtomicU32::new(0), waiters: AtomicU32::new(0) }
    }

    #[cfg(loom)]
    pub fn new() -> Self {
        WaitWord::default()
    }

    /// Block until the value is not `old`.
    pub fn wait(&self, old: u32) {
        self.wait_impl(old, None, Scope::Process);
    }

    /// Block until the value is not `old`, or `deadline` passes. Returns true
    /// if the value changed. With a deadline already past it only looks.
    pub fn wait_until(&self, old: u32, deadline: Instant) -> bool {
        self.wait_impl(old, Some(deadline), Scope::Process)
    }

    /// Like [`wait_until`](Self::wait_until), for a word another process may
    /// wake.
    pub fn wait_until_scoped(&self, old: u32, deadline: Option<Instant>, scope: Scope) -> bool {
        self.wait_impl(old, deadline, scope)
    }

    /// Block until `done()` is true or `deadline` passes, checking again each
    /// time the word changes. For a handshake: whoever changes what `done`
    /// reads then bumps the word. Returns true if `done()` came true.
    pub fn wait_for(&self, mut done: impl FnMut() -> bool, deadline: Option<Instant>) -> bool {
        loop {
            let seen = self.value.load(SeqCst);
            if done() {
                return true;
            }
            match deadline {
                Some(d) if Instant::now() >= d => return false,
                _ => {}
            }
            self.wait_impl(seen, deadline, Scope::Process);
        }
    }

    /// Wake every waiter, after changing the value or what the waiters look
    /// at. Safe on any thread, the audio thread included; never blocks.
    pub fn notify(&self) {
        self.notify_scoped(Scope::Process);
    }

    pub fn notify_scoped(&self, scope: Scope) {
        // Between the caller's change and the look at the count (see wait_impl).
        fence(SeqCst);
        if self.waiters.load(SeqCst) != 0 {
            os::wake_all(self, scope);
        }
    }

    /// Add one to the value and wake every waiter. Safe on any thread; never
    /// blocks.
    pub fn bump(&self) {
        self.bump_scoped(Scope::Process);
    }

    pub fn bump_scoped(&self, scope: Scope) {
        self.value.fetch_add(1, SeqCst);
        self.notify_scoped(scope);
    }

    // Count this thread in, look, wait, count it out. The waiter counts
    // itself in before it looks, and the waker changes the value before it
    // reads the count, so either the look sees the new value or the waker
    // sees the waiter and wakes it. The kernel's own compare closes the gap
    // between the look and the sleep.
    //
    // Each side writes one word and then reads the other, which is safe only
    // in a sequentially consistent order. A fence between the write and the
    // read on each side makes that explicit. Loom checks the fence form
    // exactly (it weakens SeqCst accesses), and the fence keeps a Rust waker
    // sound against a C++ waiter on a shared word.
    fn wait_impl(&self, old: u32, deadline: Option<Instant>, scope: Scope) -> bool {
        if self.value.load(SeqCst) != old {
            return true;
        }
        if deadline.is_some_and(|d| Instant::now() >= d) {
            return false;
        }
        self.waiters.fetch_add(1, SeqCst);
        fence(SeqCst);
        while self.value.load(SeqCst) == old {
            let left = match deadline {
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        break;
                    }
                    Some(d - now)
                }
                None => None,
            };
            if !os::wait(self, old, left, scope) {
                break;   // no address wait here: nothing to block on
            }
        }
        self.waiters.fetch_sub(1, SeqCst);
        self.value.load(SeqCst) != old
    }
}

/// Wakes one sleeper from any number of wakers, and only when the sleeper
/// asked: however many wakers ring while it sleeps, the first wakes it and
/// each of the rest costs a fence and a load.
///
/// The sleeper asks and then looks at what it waits for; a waker changes that
/// and then checks for an ask, with a fence between on each side, so one of
/// them always sees the other.
#[derive(Debug, Default)]
pub struct Doorbell {
    word: WaitWord,
    asked: AtomicBool,
}

impl Doorbell {
    #[cfg(not(loom))]
    pub const fn new() -> Self {
        Doorbell { word: WaitWord::new(), asked: AtomicBool::new(false) }
    }

    #[cfg(loom)]
    pub fn new() -> Self {
        Doorbell::default()
    }

    /// The sleeper: unless `ready()`, sleep until rung or `deadline`. It
    /// checks `ready` after asking, so it sees anything a waker changed and
    /// rang for before then.
    pub fn sleep(&self, mut ready: impl FnMut() -> bool, deadline: Option<Instant>) {
        let seen = self.word.value.load(SeqCst);
        self.asked.store(true, SeqCst);
        fence(SeqCst);
        if !ready() {
            self.word.wait_until_scoped(seen, deadline, Scope::Process);
        }
        self.asked.store(false, SeqCst);
    }

    /// A waker rings after changing what the sleeper's `ready` looks at. Safe
    /// on any thread, the audio thread included; never blocks.
    #[inline]
    pub fn ring(&self) {
        fence(SeqCst);
        if self.asked.load(Relaxed) && self.asked.swap(false, SeqCst) {
            self.word.bump();
        }
    }
}

#[cfg(all(not(loom), any(target_os = "linux", target_os = "android")))]
mod os {
    use super::{Scope, WaitWord};
    use std::time::Duration;

    pub fn wait(w: &WaitWord, old: u32, left: Option<Duration>, scope: Scope) -> bool {
        let word = &w.value;
        let op = if matches!(scope, Scope::Shared(_)) { libc::FUTEX_WAIT } else { libc::FUTEX_WAIT | libc::FUTEX_PRIVATE_FLAG };
        let ts = left.map(|d| libc::timespec {
            tv_sec: d.as_secs().min(libc::time_t::MAX as u64) as libc::time_t,
            tv_nsec: d.subsec_nanos() as libc::c_long,
        });
        let tsp = ts.as_ref().map_or(core::ptr::null(), core::ptr::from_ref);
        // SAFETY: `word` is a live, 4-aligned u32; the kernel compares it with
        // `old` and sleeps on it, touching nothing else.
        unsafe { libc::syscall(libc::SYS_futex, word.as_ptr(), op, old, tsp, core::ptr::null::<u32>(), 0) };
        true
    }

    pub fn wake_all(w: &WaitWord, scope: Scope) {
        let word = &w.value;
        let op = if matches!(scope, Scope::Shared(_)) { libc::FUTEX_WAKE } else { libc::FUTEX_WAKE | libc::FUTEX_PRIVATE_FLAG };
        // SAFETY: as in `wait`; a wake writes nothing.
        unsafe { libc::syscall(libc::SYS_futex, word.as_ptr(), op, i32::MAX, core::ptr::null::<libc::timespec>(), core::ptr::null::<u32>(), 0) };
    }
}

#[cfg(all(not(loom), target_vendor = "apple"))]
mod os {
    use super::{Scope, WaitWord};
    use std::time::Duration;

    // os/os_sync_wait_on_address.h and os/clock.h (macOS 14.4, iOS 17.4), in
    // libSystem.
    const SHARED: u32 = 1;   // OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_SYNC_WAKE_BY_ADDRESS_SHARED
    const OS_CLOCK_MACH_ABSOLUTE_TIME: u32 = 32;
    extern "C" {
        fn os_sync_wait_on_address(addr: *mut libc::c_void, value: u64, size: usize, flags: u32) -> i32;
        fn os_sync_wait_on_address_with_timeout(
            addr: *mut libc::c_void, value: u64, size: usize, flags: u32, clockid: u32, timeout_ns: u64,
        ) -> i32;
        fn os_sync_wake_by_address_all(addr: *mut libc::c_void, size: usize, flags: u32) -> i32;
    }

    fn flags(scope: Scope) -> u32 {
        if matches!(scope, Scope::Shared(_)) { SHARED } else { 0 }
    }

    pub fn wait(w: &WaitWord, old: u32, left: Option<Duration>, scope: Scope) -> bool {
        let word = &w.value;
        let addr = word.as_ptr().cast::<libc::c_void>();
        if let Some(d) = left {
            let ns = u64::try_from(d.as_nanos()).unwrap_or(u64::MAX).max(1);
            // SAFETY: `addr` is a live, 4-aligned u32 of size 4; the kernel
            // compares it with `old` and sleeps on it.
            unsafe {
                os_sync_wait_on_address_with_timeout(addr, u64::from(old), 4, flags(scope), OS_CLOCK_MACH_ABSOLUTE_TIME, ns)
            };
        } else {
            // SAFETY: as above.
            unsafe { os_sync_wait_on_address(addr, u64::from(old), 4, flags(scope)) };
        }
        true
    }

    pub fn wake_all(w: &WaitWord, scope: Scope) {
        let word = &w.value;
        // SAFETY: as in `wait`; a wake writes nothing.
        unsafe { os_sync_wake_by_address_all(word.as_ptr().cast::<libc::c_void>(), 4, flags(scope)) };
    }
}

#[cfg(all(not(loom), windows))]
mod os {
    use super::{Scope, WaitWord};
    use std::time::Duration;
    use windows_sys::Win32::System::Threading::{
        SetEvent, WaitForSingleObject, WaitOnAddress, WakeByAddressAll, INFINITE,
    };

    pub fn wait(w: &WaitWord, old: u32, left: Option<Duration>, scope: Scope) -> bool {
        let word = &w.value;
        // Milliseconds, rounded up, so a wait never ends before its deadline.
        let ms = left.map_or(INFINITE, |d| {
            u32::try_from(d.as_nanos().div_ceil(1_000_000)).unwrap_or(INFINITE - 1)
        });
        match scope {
            Scope::Shared(event) if !event.is_null() => {
                // SAFETY: the event is a live handle the caller holds.
                unsafe { WaitForSingleObject(event, ms) };
            }
            _ => {
                let expected = old;
                // SAFETY: `word` is a live, 4-aligned u32 and `expected` a u32
                // of the same size; the kernel compares and sleeps.
                unsafe { WaitOnAddress(word.as_ptr().cast(), (&raw const expected).cast(), 4, ms) };
            }
        }
        true
    }

    pub fn wake_all(w: &WaitWord, scope: Scope) {
        let word = &w.value;
        match scope {
            Scope::Shared(event) if !event.is_null() => {
                // SAFETY: the event is a live handle the caller holds.
                unsafe { SetEvent(event) };
            }
            _ => {
                // SAFETY: a wake on a live word writes nothing.
                unsafe { WakeByAddressAll(word.as_ptr().cast()) };
            }
        }
    }
}

// Under loom: a model of the kernel's compare-and-sleep that loom can
// explore. To a waker the compare and the sleep are one step, as a futex's
// are; the atomics around them are the real code. Deadlines are not
// modelled: a model waits without one.
#[cfg(loom)]
mod os {
    use super::{Scope, WaitWord};
    use loom::sync::atomic::Ordering::SeqCst;
    use loom::sync::{Condvar, Mutex};
    use std::time::Duration;

    #[derive(Debug, Default)]
    pub struct Kernel {
        lock: Mutex<()>,
        woken: Condvar,
    }

    pub fn wait(w: &WaitWord, old: u32, _: Option<Duration>, _: Scope) -> bool {
        let guard = w.kernel.lock.lock().unwrap();
        if w.value.load(SeqCst) == old {
            drop(w.kernel.woken.wait(guard).unwrap());
        }
        true
    }

    pub fn wake_all(w: &WaitWord, _: Scope) {
        let _guard = w.kernel.lock.lock().unwrap();
        w.kernel.woken.notify_all();
    }
}

#[cfg(all(not(loom), not(any(target_os = "linux", target_os = "android", target_vendor = "apple", windows))))]
mod os {
    use super::{Scope, WaitWord};
    use std::time::Duration;

    pub fn wait(_: &WaitWord, _: u32, _: Option<Duration>, _: Scope) -> bool {
        false
    }
    pub fn wake_all(_: &WaitWord, _: Scope) {}
}

#[cfg(all(test, not(loom)))]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;
    use std::sync::Arc;
    use std::time::Duration;

    #[test]
    fn a_wait_returns_when_another_thread_bumps_the_word_and_not_before() {
        let w = Arc::new(WaitWord::new());
        let returned = Arc::new(AtomicBool::new(false));
        let waiter = {
            let (w, returned) = (w.clone(), returned.clone());
            std::thread::spawn(move || {
                w.wait(0);
                returned.store(true, SeqCst);
            })
        };
        while w.waiters.load(SeqCst) == 0 {
            std::thread::yield_now();
        }
        assert!(!returned.load(SeqCst));
        w.bump();
        waiter.join().unwrap();
        assert!(returned.load(SeqCst));
        assert_eq!(w.value.load(SeqCst), 1);
        assert_eq!(w.waiters.load(SeqCst), 0);
    }

    #[test]
    fn a_timed_wait_returns_at_its_deadline_when_nothing_changes() {
        let w = WaitWord::new();
        let t0 = Instant::now();
        assert!(!w.wait_until(0, t0 + Duration::from_millis(50)));
        let took = t0.elapsed();
        assert!(took >= Duration::from_millis(50));
        assert!(took < Duration::from_secs(10));   // only a wait that ignored its deadline gets here
        assert_eq!(w.waiters.load(SeqCst), 0);
        // With a deadline already past it only looks.
        assert!(!w.wait_until(0, Instant::now() - Duration::from_millis(1)));
    }

    #[test]
    fn a_wait_on_a_word_that_already_differs_returns_at_once() {
        let w = WaitWord::new();
        w.bump();
        assert!(w.wait_until(0, Instant::now() + Duration::from_secs(10)));
        w.wait(0);   // would block for ever if it waited
    }

    #[test]
    fn no_wake_is_lost_however_the_bumps_and_the_waits_interleave() {
        const BUMPS: u32 = 200_000;
        let w = Arc::new(WaitWord::new());
        let produced = Arc::new(AtomicU32::new(0));
        let consumer = {
            let (w, produced) = (w.clone(), produced.clone());
            std::thread::spawn(move || {
                let mut seen = w.value.load(SeqCst);
                while produced.load(SeqCst) < BUMPS {
                    w.wait_until(seen, Instant::now() + Duration::from_secs(30));
                    seen = w.value.load(SeqCst);
                }
            })
        };
        for _ in 0..BUMPS {
            produced.fetch_add(1, SeqCst);
            w.bump();
        }
        let t0 = Instant::now();
        consumer.join().unwrap();
        assert!(t0.elapsed() < Duration::from_secs(5), "the consumer slept past the last bump");
        assert_eq!(w.waiters.load(SeqCst), 0);
    }

    #[test]
    fn many_waiters_are_all_woken_by_one_bump() {
        let w = Arc::new(WaitWord::new());
        let woke = Arc::new(AtomicU32::new(0));
        let threads: Vec<_> = (0..3)
            .map(|_| {
                let (w, woke) = (w.clone(), woke.clone());
                std::thread::spawn(move || {
                    w.wait(0);
                    woke.fetch_add(1, SeqCst);
                })
            })
            .collect();
        while w.waiters.load(SeqCst) < 3 {
            std::thread::yield_now();
        }
        w.bump();
        for t in threads {
            t.join().unwrap();
        }
        assert_eq!(woke.load(SeqCst), 3);
    }

    #[test]
    fn a_condition_over_other_state_is_waited_for_through_the_word() {
        let w = Arc::new(WaitWord::new());
        let in_flight = Arc::new(AtomicU32::new(3));
        let finisher = {
            let (w, in_flight) = (w.clone(), in_flight.clone());
            std::thread::spawn(move || {
                for _ in 0..3 {
                    in_flight.fetch_sub(1, SeqCst);
                    w.bump();
                }
            })
        };
        assert!(w.wait_for(|| in_flight.load(SeqCst) == 0, Some(Instant::now() + Duration::from_secs(10))));
        finisher.join().unwrap();
        // And a condition that never comes true gives up at the deadline.
        assert!(!w.wait_for(|| false, Some(Instant::now() + Duration::from_millis(20))));
    }

    #[test]
    fn a_doorbell_rung_with_nobody_asking_wakes_nobody() {
        let d = Doorbell::new();
        d.ring();
        assert_eq!(d.word.value.load(SeqCst), 0);
        // And a sleeper whose condition is ready does not sleep at all.
        d.sleep(|| true, None);
        assert!(!d.asked.load(SeqCst));
    }

    #[test]
    fn a_doorbell_wakes_its_sleeper_once_however_many_ring() {
        let d = Arc::new(Doorbell::new());
        let ready = Arc::new(AtomicBool::new(false));
        let sleeper = {
            let (d, ready) = (d.clone(), ready.clone());
            std::thread::spawn(move || d.sleep(|| ready.load(SeqCst), None))
        };
        while d.word.waiters.load(SeqCst) == 0 {
            std::thread::yield_now();
        }
        ready.store(true, SeqCst);
        for _ in 0..100 {
            d.ring();
        }
        sleeper.join().unwrap();
        assert_eq!(d.word.value.load(SeqCst), 1);
    }

    #[test]
    fn a_doorbell_sleep_ends_at_its_deadline() {
        let d = Doorbell::new();
        let t0 = Instant::now();
        d.sleep(|| false, Some(t0 + Duration::from_millis(30)));
        assert!(t0.elapsed() >= Duration::from_millis(30));
        assert!(!d.asked.load(SeqCst));
    }
}

// Explores every interleaving of the real atomics around the modelled kernel
// wait. A lost wake leaves a thread blocked for ever, which loom reports as a
// deadlock. Run with: RUSTFLAGS="--cfg loom" cargo test --release --lib
// -p clockwork-wait.
#[cfg(all(test, loom))]
mod loom_tests {
    use super::*;
    use loom::sync::atomic::AtomicBool;
    use loom::sync::Arc;
    use loom::thread;

    #[test]
    fn a_bump_is_never_lost_to_a_wait_however_they_interleave() {
        loom::model(|| {
            let w = Arc::new(WaitWord::new());
            let bumper = {
                let w = w.clone();
                thread::spawn(move || w.bump())
            };
            w.wait(0);
            bumper.join().unwrap();
            assert_eq!(w.waiters.load(SeqCst), 0);
        });
    }

    #[test]
    fn a_condition_changed_before_its_bump_is_seen_by_its_waiter() {
        loom::model(|| {
            let w = Arc::new(WaitWord::new());
            let done = Arc::new(AtomicBool::new(false));
            let finisher = {
                let (w, done) = (w.clone(), done.clone());
                thread::spawn(move || {
                    done.store(true, SeqCst);
                    w.bump();
                })
            };
            assert!(w.wait_for(|| done.load(SeqCst), None));
            finisher.join().unwrap();
        });
    }

    #[test]
    fn one_bump_wakes_two_waiters() {
        loom::model(|| {
            let w = Arc::new(WaitWord::new());
            let other = {
                let w = w.clone();
                thread::spawn(move || w.wait(0))
            };
            let bumper = {
                let w = w.clone();
                thread::spawn(move || w.bump())
            };
            w.wait(0);
            other.join().unwrap();
            bumper.join().unwrap();
        });
    }
}

#[cfg(all(test, loom))]
mod loom_doorbell_tests {
    use super::*;
    use loom::sync::Arc;
    use loom::thread;

    #[test]
    fn a_doorbell_rung_after_a_change_always_wakes_the_sleeper_waiting_for_it() {
        loom::model(|| {
            let d = Arc::new(Doorbell::new());
            let ready = Arc::new(AtomicBool::new(false));
            let waker = {
                let (d, ready) = (d.clone(), ready.clone());
                thread::spawn(move || {
                    ready.store(true, SeqCst);
                    d.ring();
                })
            };
            // As a drain sleeps: look, sleep, look again.
            while !ready.load(SeqCst) {
                d.sleep(|| ready.load(SeqCst), None);
            }
            waker.join().unwrap();
        });
    }
}

/// Counts how often one of this process's threads woke, found by its name,
/// for tests that pin a thread as quiet. Linux only (the thread's voluntary
/// context switches, which /proc keeps per thread); elsewhere there is no
/// such count, and a test that needs one skips.
#[cfg(feature = "testing")]
pub mod testing {
    use std::time::{Duration, Instant};

    /// Whether wakes can be counted here.
    pub fn countable() -> bool {
        cfg!(target_os = "linux")
    }

    /// The named thread's wakes so far.
    pub fn wakes(name: &str) -> Option<u64> {
        #[cfg(target_os = "linux")]
        {
            for task in std::fs::read_dir("/proc/self/task").ok()?.flatten() {
                let comm = std::fs::read_to_string(task.path().join("comm")).unwrap_or_default();
                if comm.trim_end() != name {
                    continue;
                }
                let status = std::fs::read_to_string(task.path().join("status")).ok()?;
                return status
                    .lines()
                    .find_map(|l| l.strip_prefix("voluntary_ctxt_switches:"))
                    .and_then(|v| v.trim().parse().ok());
            }
            None
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = name;
            None
        }
    }

    /// The named thread's wakes over `window`, counted from when the thread
    /// exists (waiting up to ten seconds for it).
    pub fn over(name: &str, window: Duration) -> Option<u64> {
        let until = Instant::now() + Duration::from_secs(10);
        let before = loop {
            if let Some(n) = wakes(name) {
                break n;
            }
            if Instant::now() >= until {
                return None;
            }
            std::thread::sleep(Duration::from_millis(10));
        };
        std::thread::sleep(window);
        Some(wakes(name)? - before)
    }
}
