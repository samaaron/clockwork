// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The panic fence (docs/ENGINES.md). Past an `extern "C"` boundary a
//! panic aborts the process, which in a plugin is the host's, so every
//! `extern "C"` function in Clockwork's Rust is one call to [`no_unwind`]
//! with what the function answers when it fails: a null, a zero, a status,
//! nothing. `scripts/check-ffi.sh` holds every one to it.

#![cfg_attr(not(feature = "std"), no_std)]

/// Run `f`; a panic in it answers `fallback` instead of unwinding. The
/// operation is dropped and the caller is told it failed; state a panic
/// left half-written is the caller's to distrust (a poisoned lock then
/// disables what it guards rather than taking the process with it).
#[cfg(feature = "std")]
pub fn no_unwind<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
}

/// Without std there is no unwinding: a panic aborts (the embedded
/// profile's `panic = "abort"`), so the fence is the call itself.
#[cfg(not(feature = "std"))]
pub fn no_unwind<T>(_fallback: T, f: impl FnOnce() -> T) -> T {
    f()
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::no_unwind;

    #[test]
    fn a_panic_answers_the_fallback_and_a_return_its_value() {
        assert_eq!(no_unwind(-1, || 7), 7);
        let quiet = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let caught = no_unwind(-1, || -> i32 { panic!("inside the fence") });
        std::panic::set_hook(quiet);
        assert_eq!(caught, -1);
    }
}
