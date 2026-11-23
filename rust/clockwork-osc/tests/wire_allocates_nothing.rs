// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `clockwork_osc::wire` reads and writes without allocating: counted, with an
//! allocator that counts. One test in this file, so nothing else in the
//! process allocates while it counts.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

use clockwork_osc::wire::{Arg, BundleWriter, Message, Packet, Writer};
use clockwork_osc::Timetag;

struct Counting;
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);

// SAFETY: every call goes to the system allocator unchanged; this only counts.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        // SAFETY: the caller's contract, passed on.
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: the caller's contract, passed on.
        unsafe { System.dealloc(ptr, layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        // SAFETY: the caller's contract, passed on.
        unsafe { System.realloc(ptr, layout, size) }
    }
}

#[global_allocator]
static COUNTING: Counting = Counting;

fn allocations_in(work: impl FnOnce()) -> usize {
    let before = ALLOCATIONS.load(Ordering::Relaxed);
    work();
    ALLOCATIONS.load(Ordering::Relaxed) - before
}

/// Every argument of a message, arrays entered, folded into one number so the reading is not optimised away.
fn read(args: clockwork_osc::wire::Args<'_>) -> u64 {
    args.fold(0u64, |sum, arg| {
        sum.wrapping_add(match arg {
            Arg::Array(inside) => read(inside),
            Arg::Str(s) | Arg::Symbol(s) => s.len() as u64,
            Arg::Blob(b) => b.len() as u64,
            other => other.f64().map_or(1, |v| v.to_bits()),
        })
    })
}

#[test]
fn reading_and_writing_allocate_nothing() {
    let mut buf = [0u8; 512];
    let mut copy = [0u8; 512];
    let mut seen = 0u64;
    let mut messages = 0u32;

    let counted = allocations_in(|| {
        let mut bundle = BundleWriter::new(&mut buf, Timetag::from_ntp(100)).unwrap();
        bundle
            .message("/tau5/event", |w| {
                w.double(2.001)?.int(7)?.str("a string")?.blob(&[1, 2, 3])?.bool(true)?;
                w.array(|w| {
                    w.long(1 << 40)?;
                    w.array(|w| w.symbol("deep").map(drop))?;
                    Ok(())
                })?;
                Ok(())
            })
            .unwrap();
        bundle.bundle(Timetag::from_ntp(200), |b| b.message("/inner", |w| w.float(0.5).map(drop)).map(drop)).unwrap();
        // one that is refused, and one that does not fit, allocate nothing either
        assert!(bundle.message("no slash", |_| Ok(())).is_err());
        assert!(bundle.message("/big", |w| w.blob(&[0u8; 600]).map(drop)).is_err());
        let bytes = bundle.finish();

        let packet = Packet::parse(bytes).unwrap();
        packet.each_message(Timetag::IMMEDIATELY, &mut |message, when| {
            messages += 1;
            seen = seen.wrapping_add(read(message.args())).wrapping_add(when.ntp()).wrapping_add(message.address().len() as u64);
        });
        let rewritten = packet.write(&mut copy).unwrap();
        assert!(rewritten == bytes);
        assert!(Message::parse(b"/bad\0\0\0\0,q\0\0").is_err());

        let mut one = [0u8; 32];
        let mut w = Writer::new(&mut one, "/tau5/spawn").unwrap();
        w.double(2.001).unwrap();
        seen = seen.wrapping_add(read(Message::parse(w.finish()).unwrap().args()));
    });

    assert_eq!(messages, 2, "the work was done");
    assert_ne!(seen, 0);
    assert_eq!(counted, 0, "wire allocated");

    // and the counter does count: the same kind of message, made in a Vec
    let owned = allocations_in(|| {
        let bytes = clockwork_osc::wire::to_vec(32, |buf| clockwork_osc::wire::message(buf, "/tau5/event", |w| w.double(2.001).map(drop)));
        assert_eq!(bytes.map(|b| b.len()), Ok(24));
    });
    assert_eq!(owned, 1, "the counting allocator saw the Vec, and to_vec made nothing else");
}
