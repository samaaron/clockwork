// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The test the ring exists to survive: a real producer thread against a real
//! consumer loop, for long enough that the wrap, the empty case and the full
//! case all happen thousands of times.
//!
//! Both cases run through the C ABI rather than against `FrameRing` directly,
//! because the slot table's refcount is on that path too and a race there
//! would be a use-after-free on the audio thread.
//!
//! What is asserted, and why each matters:
//!
//! * **No torn frames.** Every frame carries its own index in every channel.
//!   A frame assembled from two different pushes would show channels
//!   disagreeing, which no amount of "it sounded fine" would ever reveal.
//! * **No loss and no duplication.** The consumer's frame indices must be
//!   exactly consecutive in the lossless case, so a single dropped or repeated
//!   frame fails the run.
//! * **Counters reconcile exactly.** Frames in == frames out + dropped, and
//!   the underrun count equals the silence actually emitted. A counter that
//!   is merely plausible is worse than none: it is what gets believed.

use std::ffi::CString;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use clockwork_ports::ffi::*;

const SOURCE: i32 = 1;
const SINK: i32 = 2;

/// Frame `f`, channel `c`. Integers below 2^24 are exact in f32, so the
/// assertions are equalities.
fn sample(f: u64, c: usize) -> f32 { (f * 4 + c as u64) as f32 }

fn open(name: &str, dir: i32, channels: u32, cap: u32) -> u32 {
    let n = CString::new(name).unwrap();
    // SAFETY: `n` is a NUL-terminated CString.
    let p = unsafe { clockwork_port_open(n.as_ptr(), dir, channels, cap) };
    assert_ne!(p, 0, "port table full");
    p
}

#[test]
fn a_source_hammered_for_a_sustained_run_loses_nothing() {
    const CH: usize = 3;
    const TOTAL: u64 = 2_000_000;
    // The producer's block must NOT divide the ring's capacity, or the write
    // cursor stays aligned to it and a push NEVER straddles the wrap — the
    // ring's hardest case would then go unexercised for the whole run. 100
    // against 1024 misaligns immediately and keeps misaligning. (Found by
    // mutation: deleting the wrap-segment copy left this test green.)
    const BLOCK: usize = 100;

    let port = open("concurrency-source", SOURCE, CH as u32, 1024);

    // The endpoint: fills the ring as fast as it is drained, keeping the
    // remainder when it will not fit — which is what clockwork_ports.h tells a
    // producer to do, and the only way "nothing is lost" can be true.
    let producer = std::thread::spawn(move || {
        let mut buf = vec![0.0f32; BLOCK * CH];
        let mut next: u64 = 0;
        while next < TOTAL {
            let n = std::cmp::min(BLOCK as u64, TOTAL - next) as usize;
            for f in 0..n {
                for c in 0..CH { buf[f * CH + c] = sample(next + f as u64, c); }
            }
            let mut done = 0usize;
            while done < n {
                // SAFETY: `buf[done * CH..]` holds at least `(n - done) * CH` floats.
                let took = unsafe {
                    clockwork_port_produce(port, buf[done * CH..].as_ptr(), (n - done) as u32)
                } as usize;
                if took == 0 { std::thread::yield_now(); }
                done += took;
            }
            next += n as u64;
        }
        next
    });

    // The audio thread: a fixed cadence, taking whatever is there.
    let mut chans: Vec<Vec<f32>> = (0..CH).map(|_| vec![f32::NAN; BLOCK]).collect();
    let mut next_out: u64 = 0;
    let mut requested: u64 = 0;
    let mut blocks: u64 = 0;
    while next_out < TOTAL {
        let ptrs: Vec<*mut f32> = chans.iter_mut().map(|v| v.as_mut_ptr()).collect();
        // SAFETY: `CH` channel pointers, each to `BLOCK` writable frames.
        let got = unsafe {
            clockwork_port_read(port, ptrs.as_ptr(), CH as u32, BLOCK as u32)
        } as u64;
        requested += BLOCK as u64;
        blocks += 1;
        for (c, chan) in chans.iter().enumerate() {
            for (f, &v) in chan.iter().enumerate().take(got as usize) {
                assert_eq!(v, sample(next_out + f as u64, c),
                           "torn, lost or duplicated frame at {}", next_out + f as u64);
            }
            // The zero-filled tail is silence, every time.
            for &v in &chan[got as usize..] {
                assert_eq!(v, 0.0, "underrun tail must be silence");
            }
        }
        next_out += got;
        assert!(blocks < 10_000_000, "consumer made no progress");
    }

    let produced = producer.join().unwrap();
    assert_eq!(produced, TOTAL);
    assert_eq!(next_out, TOTAL, "every frame produced came out exactly once");

    // Frames in == frames out. Nothing was dropped: a source counts only what
    // the audio thread asked for and did not get, and that is exactly the
    // silence it emitted.
    let under = clockwork_port_underruns(port);
    assert_eq!(under, requested - next_out,
               "underrun count must equal the silence actually emitted");
    assert_eq!(clockwork_port_overruns(port), 0, "a source never overruns");
    clockwork_port_close(port);
}

#[test]
fn a_sink_that_overruns_accounts_for_every_frame_it_dropped() {
    const CH: usize = 2;
    const TOTAL: u64 = 1_000_000;
    // Not a divisor of the 256-frame ring, for the same reason.
    const BLOCK: usize = 100;

    // Deliberately shallow, and drained by a consumer that dawdles, so the
    // ring really does fill.
    let port = open("concurrency-sink", SINK, CH as u32, 256);
    let stop = Arc::new(AtomicBool::new(false));

    let consumed = {
        let stop = Arc::clone(&stop);
        std::thread::spawn(move || {
            let mut buf = vec![0.0f32; 96 * CH];
            let mut count: u64 = 0;
            let mut last: Option<u64> = None;
            loop {
                // SAFETY: `buf` holds 96 frames of `CH` channels.
                let got = unsafe { clockwork_port_consume(port, buf.as_mut_ptr(), 96) } as usize;
                for f in 0..got {
                    // Channel 0 names the frame; every other channel must
                    // agree, or the frame was assembled from two writes.
                    let idx = (buf[f * CH] as u64) / 4;
                    for c in 0..CH {
                        assert_eq!(buf[f * CH + c], sample(idx, c), "torn frame");
                    }
                    if let Some(prev) = last {
                        assert!(idx > prev, "a sink must not reorder or repeat");
                    }
                    last = Some(idx);
                }
                count += got as u64;
                if got == 0 {
                    if stop.load(Ordering::Acquire) { break; }
                    std::thread::yield_now();
                }
            }
            count
        })
    };

    // The audio thread: offers a block, keeps nothing back, and never retries.
    let mut chans: Vec<Vec<f32>> = (0..CH).map(|_| vec![0.0f32; BLOCK]).collect();
    let mut offered: u64 = 0;
    let mut accepted: u64 = 0;
    let mut next: u64 = 0;
    while next < TOTAL {
        for (c, chan) in chans.iter_mut().enumerate() {
            for (f, v) in chan.iter_mut().enumerate() { *v = sample(next + f as u64, c); }
        }
        let ptrs: Vec<*const f32> = chans.iter().map(|v| v.as_ptr()).collect();
        // SAFETY: `CH` channel pointers, each to `BLOCK` live frames.
        let took = unsafe {
            clockwork_port_write(port, ptrs.as_ptr(), CH as u32, BLOCK as u32)
        } as u64;
        offered += BLOCK as u64;
        accepted += took;
        // A partial write leaves a hole, which is the drop policy working:
        // the audio thread moves on and the endpoint learns from the counter.
        next += BLOCK as u64;
    }

    // Let the consumer catch up, then tell it to stop.
    while clockwork_port_readable(port) > 0 { std::thread::yield_now(); }
    stop.store(true, Ordering::Release);
    let out = consumed.join().unwrap();

    let dropped = clockwork_port_overruns(port);
    assert_eq!(offered, TOTAL);
    assert_eq!(accepted + dropped, offered,
               "every frame is either accepted or counted as dropped");
    assert_eq!(out, accepted, "everything accepted came out exactly once");
    assert!(dropped > 0, "the sink never actually filled — the test proved nothing");
    assert_eq!(clockwork_port_underruns(port), 0, "a sink never underruns");
    clockwork_port_close(port);
}

#[test]
fn a_port_closed_under_a_live_reader_keeps_reads_whole() {
    // The one race the slot table exists to survive, and the only one the
    // cases above cannot reach: a control thread closing a port at the exact
    // moment the audio thread is INSIDE clockwork_port_read on it.
    //
    // Closing is what frees the ring, so getting this wrong is a use-after-
    // free on the audio thread — which does not announce itself, because the
    // allocator hands the very next port the block it just freed. What DOES
    // announce itself is a read that is not uniform: each generation of port
    // is filled entirely with its own tag and read whole, so every read must
    // come back all-silence or all-one-tag. A ring freed and re-issued under a
    // reader gives it the boundary between two of them.
    //
    // The block is deliberately large (2048 frames) so the reader spends a
    // long time inside one read and the close has somewhere to land.
    //
    // HONEST LIMIT: this is a smoke test, not a proof. Mutation-checked by
    // dropping `refs(w) != 0` from the reclaim condition in registry.rs — so a
    // closed slot IS freed under a live reader — and it stayed green, because
    // the freed port and the one opened a microsecond later come out of the
    // same allocator block and read alike. The rule itself is pinned
    // deterministically in registry.rs's own tests
    // (`a_closed_slot_stays_reserved_while_a_reader_is_inside_it`), which does
    // catch that mutation. What this case is worth is the churn: four thousand
    // opens, fills and closes against a reader that never stops.
    const ROUNDS: u32 = 4_000;
    const BLOCK: usize = 2048;

    let handle = Arc::new(std::sync::atomic::AtomicU32::new(0));
    let stop = Arc::new(AtomicBool::new(false));
    let bad = Arc::new(std::sync::atomic::AtomicU32::new(0));

    let reader = {
        let handle = Arc::clone(&handle);
        let stop = Arc::clone(&stop);
        let bad = Arc::clone(&bad);
        std::thread::spawn(move || {
            let mut buf = vec![0.0f32; BLOCK];
            while !stop.load(Ordering::Acquire) {
                let h = handle.load(Ordering::Acquire);
                let ptrs: [*mut f32; 1] = [buf.as_mut_ptr()];
                // SAFETY: one channel pointer to `BLOCK` writable frames; a closed handle is refused by the registry, which is the point.
                unsafe { clockwork_port_read(h, ptrs.as_ptr(), 1, BLOCK as u32) };
                let first = buf[0];
                let plausible = first == 0.0
                    || (first >= 1.0 && first <= ROUNDS as f32 && first.fract() == 0.0);
                if !plausible || buf.iter().any(|&v| v != first) {
                    bad.fetch_add(1, Ordering::Relaxed);
                }
            }
        })
    };

    for tag in 1..=ROUNDS {
        let p = open("churn", SOURCE, 1, BLOCK as u32);
        let fill = vec![tag as f32; BLOCK];
        assert_eq!(
            // SAFETY: `fill` holds `BLOCK` frames of 1 channel.
            unsafe { clockwork_port_produce(p, fill.as_ptr(), BLOCK as u32) },
            BLOCK as u32
        );
        handle.store(p, Ordering::Release);
        // Close it while the reader is very likely to be inside it.
        clockwork_port_close(p);
        assert_eq!(bad.load(Ordering::Relaxed), 0,
                   "a read spanned two ports at round {}", tag);
    }

    stop.store(true, Ordering::Release);
    reader.join().unwrap();
    assert_eq!(bad.load(Ordering::Relaxed), 0);
}
