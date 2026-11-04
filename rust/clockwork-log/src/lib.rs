// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The Rust side of `clockwork_log`.
//!
//! Native Rust subsystems (the OSC ingress, the stream/UDS/pipe transports,
//! the sinks drain) log through [`log!`] and nothing else, and every line
//! names its core: an engine's log carries its own lines and no other
//! engine's (docs/ENGINES.md). Where an engine is present a sink is
//! installed ([`clockwork_rust_log_install`], called from `clockwork_init`)
//! and a line goes onto its core's debug ring, the same channel as the C++
//! side's `clockwork_log` — a host prints it, a client shows it, the test
//! fixture captures it. With no sink (a crate's own tests, the transport
//! probe, an embedder that never installs one) a line goes to stderr, which
//! is the only place it could go. A line with no core — null, said by a
//! service of the process rather than of one engine — is the sink's to
//! place: the engine's puts it on the console.
//!
//! A Rust embedder that runs without the C++ engine can install its own sink
//! with [`install`].

use clockwork_fence::no_unwind;
use std::ffi::c_char;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

pub use clockwork_abi::lanes::ClockworkCore;

/// A sink takes one line of `core`'s log: UTF-8 bytes and their length, no
/// trailing newline, not NUL-terminated. `core` may be null.
pub type Sink = extern "C" fn(*mut ClockworkCore, *const c_char, u32);

static SINK: OnceLock<Sink> = OnceLock::new();

/// Emit one already-formatted line of `core`'s log. Prefer [`log!`].
pub fn emit(core: *mut ClockworkCore, line: &str) {
    match SINK.get() {
        Some(sink) => sink(core, line.as_ptr().cast::<c_char>(), line.len() as u32),
        None => eprintln!("{line}"),   // stdio-ok: no engine, no ring
    }
}

/// Route every later line to `sink`. The first installer wins; a second
/// call is a no-op. The engine's sink is stateless — it puts each line on
/// the core the line names — so every core in the process shares it.
pub fn install(sink: Sink) {
    let _ = SINK.set(sink);
}

/// C ABI form of [`install`], for the engine.
///
/// # Safety
/// `sink` must stay callable for the rest of the process.
#[no_mangle]
pub extern "C" fn clockwork_rust_log_install(sink: Sink) {
    no_unwind((), || {
        install(sink);
    })
}

/// `println!`-shaped, after the core: `log!(core, "...", args)` formats, then
/// [`emit`]s the line as `core`'s.
#[macro_export]
macro_rules! log {
    ($core:expr, $($arg:tt)*) => {
        $crate::emit($core, &::std::format!($($arg)*))
    };
}

/// The core a service's lines name, kept where the service's threads reach
/// it: a core handle is never freed (clockwork_core.h), and this is only
/// ever handed to the sink, never dereferenced here, so it travels between
/// threads freely.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CoreRef(*mut ClockworkCore);

impl CoreRef {
    #[must_use]
    pub const fn new(core: *mut ClockworkCore) -> Self {
        Self(core)
    }

    /// The core, for [`log!`].
    #[must_use]
    pub const fn ptr(self) -> *mut ClockworkCore {
        self.0
    }
}

// SAFETY: see the type's doc — never dereferenced, and what it points to is
// never freed.
unsafe impl Send for CoreRef {}
// SAFETY: as above.
unsafe impl Sync for CoreRef {}

/// How often one kind of line is said: trouble that repeats (junk on a port,
/// a sender over a limit) must not become a flood of lines; how many times it
/// happened is a metric's job. Owned by whatever owns the trouble. The Rust
/// side's form of `clockwork::LogLimit` (src/log_limit.h). Any thread.
pub struct LogLimit {
    first: u32,
    seen: AtomicU32,
}

impl LogLimit {
    /// Said the first `n` times, then not again.
    #[must_use]
    pub const fn first(n: u32) -> Self {
        Self { first: n, seen: AtomicU32::new(0) }
    }

    /// Whether this time is one to say. Counts it.
    pub fn say(&self) -> bool {
        self.seen.fetch_add(1, Ordering::Relaxed) < self.first
    }
}

/// A sink that keeps every line with the core it named, for a crate's tests:
/// [`capture::install`] first thing in the test process (the first installer
/// wins, so it then sees every line any test logs) and ask
/// [`capture::logged`] what has been seen.
#[doc(hidden)]
pub mod capture {
    use super::{no_unwind, ClockworkCore};
    use std::ffi::c_char;
    use std::sync::Mutex;

    /// A line and the core it was said as.
    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Line {
        pub core: usize,
        pub text: String,
    }

    static SEEN: Mutex<Vec<Line>> = Mutex::new(Vec::new());

    extern "C" fn keep(core: *mut ClockworkCore, p: *const c_char, n: u32) {
        no_unwind((), || {
            // SAFETY: `emit` passes the pointer and length of a live `&str`,
            // which outlives this call.
            let bytes = unsafe { std::slice::from_raw_parts(p.cast::<u8>(), n as usize) };
            SEEN.lock().unwrap().push(Line {
                core: core as usize,
                text: String::from_utf8_lossy(bytes).into_owned(),
            });
        })
    }

    pub fn install() {
        super::install(keep);
    }

    /// Has a line containing `text` been logged since the capture was installed?
    pub fn logged(text: &str) -> bool {
        SEEN.lock().unwrap().iter().any(|l| l.text.contains(text))
    }

    /// Every line so far, in order, as text.
    pub fn lines() -> Vec<String> {
        SEEN.lock().unwrap().iter().map(|l| l.text.clone()).collect()
    }

    /// Every line so far with the core it named.
    pub fn lines_with_cores() -> Vec<Line> {
        SEEN.lock().unwrap().clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_installed_sink_receives_every_line_with_its_core_and_the_first_install_sticks() {
        capture::install();
        let a = 0x10 as *mut ClockworkCore;
        let b = CoreRef::new(0x20 as *mut ClockworkCore);
        log!(a, "hello {}", 42);
        extern "C" fn other(_: *mut ClockworkCore, _: *const c_char, _: u32) { panic!("second install must not win") }
        install(other);
        log!(b.ptr(), "again");
        log!(std::ptr::null_mut(), "nobody's");   // no-core: a line of no engine's, kept as such
        let seen = capture::lines_with_cores();
        assert_eq!(seen.iter().map(|l| l.text.as_str()).collect::<Vec<_>>(), ["hello 42", "again", "nobody's"]);
        assert_eq!(seen.iter().map(|l| l.core).collect::<Vec<_>>(), [0x10, 0x20, 0]);
    }

    #[test]
    fn a_limit_says_the_first_few_times_and_then_not_again() {
        let limit = LogLimit::first(3);
        let said: Vec<usize> = (1..=10).filter(|_| limit.say()).collect();
        assert_eq!(said, [1, 2, 3]);
    }

    #[test]
    fn threads_asking_at_once_are_told_yes_the_first_few_times_between_them() {
        let limit = LogLimit::first(5);
        let yes = AtomicU32::new(0);
        std::thread::scope(|s| {
            for _ in 0..4 {
                s.spawn(|| {
                    for _ in 0..1000 {
                        if limit.say() {
                            yes.fetch_add(1, Ordering::Relaxed);
                        }
                    }
                });
            }
        });
        assert_eq!(yes.load(Ordering::Relaxed), 5);
    }
}
