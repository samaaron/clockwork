// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The boundary `src/scheduler/Scheduler.h` claims — "RT-safe: no allocation,
//! no locks" — proved rather than asserted in prose.
//!
//! Building a store allocates and is a construction-time call. Everything the
//! audio thread touches afterwards — add, pop, release, flush, clear, the
//! clear handshake, and the compaction that the add path may trigger —
//! allocates nothing at all.
//!
//! Clockwork's own `src/rt_alloc.h` cannot see this: its hooks replace the
//! C++ global `operator new`/`delete`, and Rust's allocator goes to `malloc`
//! directly. So the proof lives here, with a counting `#[global_allocator]`,
//! exactly as `clockwork-ports` proves the same rule for the ring.
//!
//! The arm is thread-local and `const`-initialised so it cannot itself
//! allocate on first touch.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

use clockwork_schedule::ffi::*;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static COUNT: Cell<usize> = const { Cell::new(0) };
}

struct Counting;

impl Counting {
    #[inline]
    fn note() {
        // `try_with`: during thread teardown the TLS is gone, and an allocator
        // that panicked there would abort the process.
        let _ = ARMED.try_with(|a| {
            if a.get() {
                let _ = COUNT.try_with(|c| c.set(c.get() + 1));
            }
        });
    }
}

// SAFETY: every call is forwarded to `System` unchanged; only a counter is
// added, so `System`'s guarantees are this allocator's.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        Self::note();
        // SAFETY: the caller's contract is `System`'s.
        unsafe { System.alloc(l) }
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.alloc_zeroed(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.dealloc(p, l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.realloc(p, l, n) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

fn arm() {
    COUNT.with(|c| c.set(0));
    ARMED.with(|a| a.set(true));
}

fn disarm() -> usize {
    ARMED.with(|a| a.set(false));
    COUNT.with(|c| c.get())
}

#[cfg(feature = "store")]
#[test]
fn construction_allocates_and_the_fire_path_does_not() {
    let h = clockwork_sched_new(256, 65536, 4, 4);
    assert!(!h.is_null());

    // A payload big enough that the pool must be recycled many times over, so
    // the loop below is guaranteed to drive compaction and not merely the
    // easy bump-allocate path.
    let payload = [0xABu8; 200];
    let meta: u32 = 7;

    // SAFETY: `h` is a live store; `payload` outlives every add; each meta
    // slot is 4 bytes at 4-byte alignment, so a `u32` fits; `ev` is only
    // read after a pop returned 1.
    unsafe {
        // Pin one event at the far end so the pool never drains to zero: the
        // only way the churn below can keep succeeding is in-place compaction.
        let pin = clockwork_sched_add(h, i64::MAX, 1, payload.as_ptr(), 64);
        assert!(!pin.is_null());

        arm();
        let mut fired = 0usize;
        for i in 0..5_000i64 {
            let m = clockwork_sched_add(h, i, 1, payload.as_ptr(), payload.len() as u32);
            assert!(!m.is_null(), "add refused at {i} — the pool stopped recycling");
            std::ptr::write(m.cast::<u32>(), meta);

            let mut ev = std::mem::MaybeUninit::<ClockworkSchedEvent>::uninit();
            while clockwork_sched_pop_due(h, i, ev.as_mut_ptr()) == 1 {
                let e = ev.assume_init_ref();
                assert_eq!(std::ptr::read(e.meta.cast::<u32>()), meta);
                clockwork_sched_release(h, e.slot);
                fired += 1;
            }
            if i % 97 == 0 {
                clockwork_sched_flush(h, 2); // a tag nothing carries: the O(n) sweep
            }
            if i % 401 == 0 {
                clockwork_sched_request_clear(h);
                assert_eq!(clockwork_sched_drain_pending_clear(h), 1);
                // The clear took the pin too; put it back.
                assert!(!clockwork_sched_add(h, i64::MAX, 1, payload.as_ptr(), 64).is_null());
            }
            let _ = clockwork_sched_next_time(h);
            let _ = clockwork_sched_size(h);
            let _ = clockwork_sched_data_used(h);
        }
        let n = disarm();
        assert_eq!(n, 0, "the fire path allocated {n} times");
        assert!(fired > 4_000, "the loop barely fired ({fired}) — it proved nothing");

        clockwork_sched_free(h);
    }
}

#[cfg(feature = "parse")]
#[test]
fn parsing_a_wire_message_allocates_nothing() {
    // Address, padded; ",hb"; an int64 timetag; a blob. Built before arming.
    let mut m: Vec<u8> = Vec::new();
    m.extend_from_slice(clockwork_schedule::SCHEDULE_ADDR.as_bytes());
    m.push(0);
    while m.len() % 4 != 0 {
        m.push(0);
    }
    m.extend_from_slice(b",hb\0");
    m.extend_from_slice(&42u64.to_be_bytes());
    m.extend_from_slice(&8u32.to_be_bytes());
    m.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);

    let mut bundle = b"#bundle\0".to_vec();
    bundle.extend_from_slice(&1u64.to_be_bytes());

    arm();
    // SAFETY: `m` and `bundle` are live for every call, and `bundle` is at
    // least 16 bytes.
    unsafe {
        for _ in 0..1000 {
            let p = clockwork_sched_parse(m.as_ptr(), m.len() as u32);
            assert_eq!(p.ok, 1);
            assert_eq!(p.when, 42);
            assert_eq!(p.blob_len, 8);
            assert_eq!(clockwork_sched_is_bundle(bundle.as_ptr(), bundle.len() as u32), 1);
            assert_eq!(clockwork_sched_bundle_timetag(bundle.as_ptr()), 1);
            assert_eq!(clockwork_sched_ntp_to_timetag(3600.5), (3600i64 << 32) | 0x8000_0000);
        }
    }
    let n = disarm();
    assert_eq!(n, 0, "parsing allocated {n} times");
}


/*
 * In-place construction allocates nothing.
 *
 * The companion above is named construction_allocates_and_the_fire_path_does
 * _not, and it was telling the truth: `Store::new` allocates. That was fine
 * as a crate-level fact and fatal as a deployment one — clockwork built its
 * scheduler from a file-scope static, so the constructor ran during static
 * initialisation, before the heap was arranged. The allocation failed,
 * clockwork_sched_new returned null, and a null handle refuses every add exactly as
 * a full queue does. The web build shipped a scheduler that accepted nothing
 * and reported it as backpressure.
 *
 * `new_in` removes the possibility rather than the instance: given memory the
 * caller already has, there is nothing to fail.
 */
#[test]
fn in_place_construction_allocates_nothing_and_still_schedules() {
    use clockwork_schedule::Store;

    let need = Store::bytes_needed(64, 4096, 4, 4).expect("valid shape");
    let mut backing = vec![0u8; need];          // allocated BEFORE arming

    arm();
    // SAFETY: `backing` is `need` writable bytes and outlives `s`.
    let mut s = unsafe {
        Store::new_in(backing.as_mut_ptr(), backing.len(), 64, 4096, 4, 4)
    }
    .expect("in-place construction");

    // And the fire path still works, still without allocating.
    let msg = [1u8, 2, 3, 4, 5, 6, 7, 8];
    for i in 0..16 {
        assert!(s.add(1_000 + i as i64, 7, &msg).is_some(), "add {i} refused");
    }
    let mut fired = 0;
    while s.pop_due(2_000).is_some() {
        fired += 1;
    }
    let allocs = disarm();

    assert_eq!(fired, 16, "everything parked must come back");
    assert_eq!(allocs, 0, "in-place construction and firing must not allocate");
}

#[test]
fn bytes_needed_refuses_shapes_new_in_would_refuse() {
    use clockwork_schedule::Store;
    assert!(Store::bytes_needed(0, 4096, 4, 4).is_none(), "no slots");
    assert!(Store::bytes_needed(64, 0, 4, 4).is_none(), "no data pool");
    assert!(Store::bytes_needed(u32::MAX, 4096, 4, 4).is_none(), "past the index width");
    assert!(Store::bytes_needed(64, 4096, 0, 4).is_none(), "no meta");
    assert!(Store::bytes_needed(64, 4096, 3, 4).is_none(), "meta not a multiple of align");
}

#[test]
fn a_short_buffer_is_refused_rather_than_overrun() {
    use clockwork_schedule::Store;
    let need = Store::bytes_needed(64, 4096, 4, 4).unwrap();
    let mut small = vec![0u8; need - 1];
    // SAFETY: `small` is exactly `small.len()` writable bytes; the refusal
    // is the point, and nothing outlives the call.
    let got = unsafe { Store::new_in(small.as_mut_ptr(), small.len(), 64, 4096, 4, 4) };
    assert!(got.is_none(), "a buffer one byte short must be refused");
}
