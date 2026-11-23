// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The panic fence (docs/ENGINES.md). Past an `extern "C"` boundary a
//! panic aborts the process, which in a plugin is the host's, so every
//! `extern "C"` function in Clockwork's Rust is one call to [`no_unwind`]
//! with what the function answers when it fails: a null, a zero, a status,
//! nothing. `scripts/check-ffi.sh` holds every one to it.

#![cfg_attr(not(feature = "std"), no_std)]

use core::ffi::c_void;

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

/// Write one message into `room` bytes at `at`; the bytes written, or 0 for
/// none. Called with the ring's writer lock held, so it fills bytes and does
/// nothing else: no lock, no allocation, no call out. It may be called more
/// than once for one event (once per ring it enters, and again into a
/// scratch buffer for a log line), or not at all, and writes the same bytes
/// each time.
pub type FillFn = extern "C" fn(fill_ctx: *mut c_void, at: *mut u8, room: u32) -> u32;

/// An event for the engine's in ring, written where it is to lie: the host
/// reserves `room` bytes, has `fill` write the message into them, and commits
/// what it wrote. `fill` and `fill_ctx` are valid for the duration of the
/// call. It may fire on a subsystem-owned thread, so the host's
/// implementation must be thread-safe.
pub type EventFn = extern "C" fn(ctx: *mut c_void, room: u32, fill: FillFn, fill_ctx: *mut c_void);

/// Hand `event` a message of at most `room` bytes that `write` makes: `write`
/// is given the reserved bytes and answers how many it wrote, from their
/// start. A `write` that answers None, more than it was given, or panics,
/// leaves no event.
pub fn event_with<F>(event: EventFn, ctx: *mut c_void, room: usize, write: &F)
where
    F: Fn(&mut [u8]) -> Option<usize>,
{
    extern "C" fn fill<F>(fill_ctx: *mut c_void, at: *mut u8, room: u32) -> u32
    where
        F: Fn(&mut [u8]) -> Option<usize>,
    {
        no_unwind(0, || {
            if fill_ctx.is_null() || at.is_null() {
                return 0;
            }
            // SAFETY: `fill_ctx` is the `&F` event_with passed, alive for the
            // call it is in; `at` is writable for `room` bytes and nothing else
            // reads or writes them until this returns (the host's reservation).
            let (write, buf) = unsafe { (&*fill_ctx.cast::<F>(), core::slice::from_raw_parts_mut(at, room as usize)) };
            match write(buf) {
                Some(wrote) if wrote <= room as usize => wrote as u32,
                _ => 0,
            }
        })
    }
    let Ok(room) = u32::try_from(room) else { return };
    event(ctx, room, fill::<F>, core::ptr::from_ref(write).cast_mut().cast());
}

#[cfg(test)]
#[cfg(feature = "std")]
mod tests {
    use super::*;
    use std::cell::RefCell;

    #[test]
    fn a_panic_answers_the_fallback_and_a_return_its_value() {
        assert_eq!(no_unwind(-1, || 7), 7);
        let quiet = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let caught = no_unwind(-1, || -> i32 { panic!("inside the fence") });
        std::panic::set_hook(quiet);
        assert_eq!(caught, -1);
    }

    // A host with `room` bytes to give, which keeps what each fill answered
    // and the bytes it left.
    struct Ring {
        bytes: Vec<u8>,
        wrote: Vec<u32>,
    }

    extern "C" fn reserve(ctx: *mut c_void, room: u32, fill: FillFn, fill_ctx: *mut c_void) {
        // SAFETY: the RefCell<Ring> the test passed, alive for the call.
        let ring = unsafe { &*ctx.cast::<RefCell<Ring>>() };
        let mut ring = ring.borrow_mut();
        ring.bytes = vec![0xAA; room as usize];
        let n = fill(fill_ctx, ring.bytes.as_mut_ptr(), room);
        ring.wrote.push(n);
    }

    fn ring() -> RefCell<Ring> {
        RefCell::new(Ring { bytes: Vec::new(), wrote: Vec::new() })
    }
    fn ctx(ring: &RefCell<Ring>) -> *mut c_void {
        core::ptr::from_ref(ring).cast_mut().cast()
    }

    #[test]
    fn an_event_is_written_into_the_room_the_host_reserved() {
        let ring = ring();
        event_with(reserve, ctx(&ring), 8, &|buf: &mut [u8]| {
            buf[..3].copy_from_slice(b"abc");
            Some(3)
        });
        let ring = ring.borrow();
        assert_eq!(ring.wrote, [3]);
        assert_eq!(ring.bytes, b"abc\xAA\xAA\xAA\xAA\xAA");
    }

    #[test]
    fn a_write_that_fails_or_panics_or_claims_more_than_the_room_leaves_no_event() {
        let ring = ring();
        event_with(reserve, ctx(&ring), 16, &|_: &mut [u8]| None);
        event_with(reserve, ctx(&ring), 16, &|_: &mut [u8]| Some(17));
        let quiet = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        event_with(reserve, ctx(&ring), 16, &|_: &mut [u8]| panic!("in the fill"));
        std::panic::set_hook(quiet);
        assert_eq!(ring.borrow().wrote, [0, 0, 0]);
    }

    #[test]
    fn a_fill_handed_nothing_to_write_into_writes_nothing() {
        extern "C" fn no_room(_: *mut c_void, _: u32, fill: FillFn, fill_ctx: *mut c_void) {
            assert_eq!(fill(fill_ctx, core::ptr::null_mut(), 16), 0);
        }
        event_with(no_room, core::ptr::null_mut(), 16, &|_: &mut [u8]| Some(4));
    }
}
