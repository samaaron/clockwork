// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The Rust side of `clockwork_log`.
//!
//! Native Rust subsystems (the OSC ingress, the stream/UDS/pipe transports,
//! the sinks drain) log through [`log!`] and nothing else. Where an engine is
//! present it installs a sink at boot ([`clockwork_rust_log_install`], called
//! from `ClockworkEngine::init`) and every line goes onto the engine's debug
//! ring, the same channel as the C++ side's `clockwork_log` — a host prints
//! it, a client shows it, the test fixture captures it. With no engine (a
//! crate's own tests, the transport probe, an embedder that never installs
//! one) a line goes to stderr, which is the only place it could go.
//!
//! A Rust embedder that runs without the C++ engine can install its own sink
//! with [`install`].

use std::ffi::c_char;
use std::sync::OnceLock;

/// A sink takes one line: UTF-8 bytes and their length, no trailing newline,
/// not NUL-terminated.
pub type Sink = extern "C" fn(*const c_char, u32);

static SINK: OnceLock<Sink> = OnceLock::new();

/// Emit one already-formatted line. Prefer [`log!`].
pub fn emit(line: &str) {
    match SINK.get() {
        Some(sink) => sink(line.as_ptr().cast::<c_char>(), line.len() as u32),
        None => eprintln!("{line}"),   // stdio-ok: no engine, no ring
    }
}

/// Route every later line to `sink`. The first installer wins; a second
/// call is a no-op, so an engine booting twice in one process keeps its
/// first sink (which forwards to whichever engine's ring is up).
pub fn install(sink: Sink) {
    let _ = SINK.set(sink);
}

/// C ABI form of [`install`], for the engine.
///
/// # Safety
/// `sink` must stay callable for the rest of the process.
#[no_mangle]
pub extern "C" fn clockwork_rust_log_install(sink: Sink) {
    install(sink);
}

/// `println!`-shaped: formats, then [`emit`]s the line.
#[macro_export]
macro_rules! log {
    ($($arg:tt)*) => {
        $crate::emit(&::std::format!($($arg)*))
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static SEEN: Mutex<Vec<String>> = Mutex::new(Vec::new());

    extern "C" fn capture(p: *const c_char, n: u32) {
        // SAFETY: `emit` passes the pointer and length of a live `&str`,
        // which outlives this call.
        let bytes = unsafe { std::slice::from_raw_parts(p.cast::<u8>(), n as usize) };
        SEEN.lock().unwrap().push(String::from_utf8_lossy(bytes).into_owned());
    }

    #[test]
    fn an_installed_sink_receives_every_line_and_the_first_install_sticks() {
        install(capture);
        log!("hello {}", 42);
        extern "C" fn other(_: *const c_char, _: u32) { panic!("second install must not win") }
        install(other);
        log!("again");
        let seen = SEEN.lock().unwrap();
        assert_eq!(seen.as_slice(), ["hello 42", "again"]);
    }
}
