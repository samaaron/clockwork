// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The message queue: bounded, allocation-free, multi-producer, single-consumer.
//!
//! # Why multi-producer, when the ports ring is not
//!
//! Because `src/clockwork_event_sink.h` draws two arrows into the same sink:
//!
//! ```text
//!      client ──ingress──▶ [ scheduler ]──▶ ┐
//!                          (optional)       ├──▶ SINK ──▶ MIDI port / OSC
//!      DSP ─────────────────────────────────┘
//! ```
//!
//! The DSP's arrow starts on the audio thread and the client's starts on
//! whatever thread clockwork fires timed events from. A single-producer ring
//! would work only until somebody wired both, and the failure would be silent
//! corruption rather than a refusal — so the queue is multi-producer from the
//! start. It costs one compare-exchange on a claim that is uncontended in the
//! common case.
//!
//! # The shape
//!
//! Dmitry Vyukov's bounded queue: a power-of-two array of cells, each with its
//! own sequence counter, and one shared `tail` producers claim from.
//!
//! * A producer reads `tail`, looks at `cell.seq`. `seq == pos` means the cell
//!   is free for this lap; it claims the position with a CAS on `tail`, writes
//!   the payload, and publishes `seq = pos + 1` with a Release store.
//!   `seq < pos` means the consumer has not yet freed that cell — the queue is
//!   FULL, and the producer returns immediately rather than waiting. That
//!   refusal is the header's "a full sink DROPS and counts rather than
//!   blocking", and it is a single load and compare.
//! * The consumer reads `head`, waits for `seq == pos + 1`, reads the payload,
//!   and publishes `seq = pos + capacity` — the cell is now free for the next
//!   lap. There is one consumer (the drain), so `head` needs no CAS.
//!
//! Nothing here allocates, waits, or takes a lock, on either side.
//!
//! # The payload is inline, and there is a ceiling
//!
//! A cell carries its bytes in place. That is what makes `push` free of
//! allocation: the memory was taken at open, when the header says allocation
//! is allowed, and a send is a `memcpy` into a cell it already owns. The cost
//! is that every cell of a queue is the same width whether it carries a
//! three-byte note-on or a sysex dump, and that a message longer than a cell
//! cannot be carried by that queue at all — it is refused, because the
//! alternatives are allocating on the audio thread or truncating a message,
//! and a truncated MIDI message is a different message. A sink answers the
//! width problem with several queues of different cell widths, chosen per
//! kind by [`crate::profile`]; this queue knows only its own.
//!
//! # The stamp a message keeps
//!
//! `push` records the arrival stamp the caller gives it — the sink hands out
//! one counter across all of its queues. It is what makes the drain's sort
//! STABLE: two messages due at the same instant leave in the order they were
//! sent, rather than in whatever order a heap happens to pop them, even when
//! they sat in different queues. Without it, a note-off and the note-on it
//! ends could swap.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicUsize, Ordering};

/// The cell width a queue gets when nobody chose one: enough for any OSC
/// message clockwork itself sends. The profiles in [`crate::profile`] choose
/// per kind, and add wider classes beside it.
pub const DEFAULT_CELL_BYTES: usize = 1024;

/// The shallowest BASE class anyone gets. A sink with room for fewer messages
/// than a block can produce is a sink that drops in normal operation. (The
/// queue itself needs two cells at least — see `profile::MIN_CLASS_DEPTH`.)
pub const MIN_CAPACITY: u32 = 8;
/// The deepest. 8192 × 1 KiB is 8 MiB for one sink, which is already more
/// buffering than any timing argument can justify.
pub const MAX_CAPACITY: u32 = 8192;

/// Round a requested capacity up to a power of two, clamped into range.
pub fn round_capacity(n: u32) -> u32 {
    let n = n.clamp(MIN_CAPACITY, MAX_CAPACITY);
    if n.is_power_of_two() { n } else { n.next_power_of_two() }
}

/// One counter, alone on its cache line. Producers hammer `tail` and the drain
/// owns `head`; sharing a line between them is the classic way to make a
/// lock-free queue slower than a mutex.
#[repr(align(64))]
struct Padded(AtomicUsize);

struct Cell {
    seq: AtomicUsize,
    when: UnsafeCell<i64>,
    len: UnsafeCell<u32>,
    stamp: UnsafeCell<u64>,
}

pub struct MsgQueue {
    cells: Box<[Cell]>,
    /// Every cell's payload, `cell_bytes` apiece, one allocation. A byte is
    /// an `UnsafeCell` because producers write disjoint cells while the
    /// consumer reads another; the sequence counters are what keep them
    /// apart, exactly as for the headers above.
    payload: Box<[UnsafeCell<u8>]>,
    cell_bytes: usize,
    mask: usize,
    tail: Padded,
    head: Padded,
}

// SAFETY: a cell is written only by the producer that won its position and
// read only by the consumer the sequence store released it to; the sequence
// counters are what establish that, so the `UnsafeCell`s are never shared.
unsafe impl Send for MsgQueue {}
// SAFETY: as above.
unsafe impl Sync for MsgQueue {}

impl MsgQueue {
    /// `capacity` must already be a power of two ([`round_capacity`]);
    /// `cell_bytes` is the widest message this queue carries. Allocates the
    /// whole queue once; nothing after this does.
    pub fn new(capacity: u32, cell_bytes: usize) -> MsgQueue {
        let cap = capacity as usize;
        assert!(cap.is_power_of_two(), "queue capacity must be a power of two");
        assert!(cell_bytes > 0, "a cell must hold at least one byte");
        let mut cells = Vec::with_capacity(cap);
        for i in 0..cap {
            cells.push(Cell {
                seq: AtomicUsize::new(i),
                when: UnsafeCell::new(0),
                len: UnsafeCell::new(0),
                stamp: UnsafeCell::new(0),
            });
        }
        let mut payload = Vec::with_capacity(cap * cell_bytes);
        payload.resize_with(cap * cell_bytes, || UnsafeCell::new(0u8));
        MsgQueue {
            cells: cells.into_boxed_slice(),
            payload: payload.into_boxed_slice(),
            cell_bytes,
            mask: cap - 1,
            tail: Padded(AtomicUsize::new(0)),
            head: Padded(AtomicUsize::new(0)),
        }
    }

    pub fn capacity(&self) -> usize { self.mask + 1 }

    /// The widest message this queue carries.
    pub fn cell_bytes(&self) -> usize { self.cell_bytes }

    /// Payload bytes this queue reserved at construction.
    pub fn bytes_reserved(&self) -> u64 { (self.capacity() * self.cell_bytes) as u64 }

    #[inline]
    fn slot(&self, index: usize) -> *mut u8 {
        // A pointer with the whole allocation's provenance, offset into it.
        // Not `payload[0].get()`: a reference to element 0 covers one byte,
        // and a pointer derived from it may not be used past that byte —
        // Miri reports the write to cell 1 as undefined behaviour, exactly
        // as it did for the frame ring before the same fix.
        // SAFETY: every caller masks `index` below the capacity, and the
        // allocation is `capacity * cell_bytes` bytes; `raw_get` makes no
        // reference along the way.
        unsafe { UnsafeCell::raw_get(self.payload.as_ptr().add(index * self.cell_bytes)) }
    }

    /// Messages waiting to be drained. Diagnostics only: it is a snapshot of
    /// two counters that move independently.
    pub fn len(&self) -> usize {
        self.tail.0.load(Ordering::Relaxed)
            .wrapping_sub(self.head.0.load(Ordering::Relaxed))
    }

    pub fn is_empty(&self) -> bool { self.len() == 0 }

    /// Put one message in, stamped `stamp` for the drain's tiebreak. Returns
    /// false if the queue is full or the message is wider than a cell — either
    /// way it was not taken, and the caller decides what that means.
    ///
    /// Callable from the audio thread: no allocation, no lock, no wait. The
    /// CAS loop only ever retries because another producer moved `tail`, which
    /// means progress was made by somebody.
    pub fn push(&self, bytes: &[u8], when: i64, stamp: u64) -> bool {
        if bytes.len() > self.cell_bytes { return false; }

        let mut pos = self.tail.0.load(Ordering::Relaxed);
        let cell = loop {
            let cell = &self.cells[pos & self.mask];
            let seq = cell.seq.load(Ordering::Acquire);
            let diff = seq as isize - pos as isize;
            if diff == 0 {
                match self.tail.0.compare_exchange_weak(
                    pos, pos.wrapping_add(1), Ordering::Relaxed, Ordering::Relaxed,
                ) {
                    Ok(_) => break cell,
                    Err(actual) => { pos = actual; }
                }
            } else if diff < 0 {
                // The consumer has not freed this cell: every position of this
                // lap is still in flight. Full.
                return false;
            } else {
                // Another producer claimed it between our two loads.
                pos = self.tail.0.load(Ordering::Relaxed);
            }
        };

        // SAFETY: this position is ours alone until the Release store below:
        // the CAS on `tail` won it and the consumer has not been handed it.
        // `bytes.len() <= cell_bytes` was checked at the top.
        unsafe {
            *cell.when.get() = when;
            *cell.len.get() = bytes.len() as u32;
            *cell.stamp.get() = stamp;
            core::ptr::copy_nonoverlapping(
                bytes.as_ptr(), self.slot(pos & self.mask), bytes.len());
        }
        cell.seq.store(pos.wrapping_add(1), Ordering::Release);
        true
    }

    /// Take the oldest message, handing it to `f` as `(when, stamp, bytes)`.
    /// `None` if the queue is empty. SINGLE CONSUMER — the drain, and only the
    /// drain.
    ///
    /// The callback shape is deliberate: the bytes stay in the cell, so a
    /// caller that only wants to look copies nothing, and one that wants to
    /// keep the message copies exactly its length rather than a whole cell.
    pub fn pop_with<R>(&self, f: impl FnOnce(i64, u64, &[u8]) -> R) -> Option<R> {
        let pos = self.head.0.load(Ordering::Relaxed);
        let cell = &self.cells[pos & self.mask];
        let seq = cell.seq.load(Ordering::Acquire);
        if seq as isize - pos.wrapping_add(1) as isize != 0 {
            // Either empty, or a producer has claimed the position and not yet
            // published its payload. Both mean "nothing to take yet"; a
            // consumer that skipped ahead would deliver out of arrival order.
            return None;
        }
        self.head.0.store(pos.wrapping_add(1), Ordering::Relaxed);

        // SAFETY: the sequence check above says the producer published this
        // cell and no other consumer exists, so the reads see a complete
        // message of `len <= cell_bytes` bytes.
        let out = unsafe {
            let len = *cell.len.get() as usize;
            let bytes = core::slice::from_raw_parts(self.slot(pos & self.mask), len);
            f(*cell.when.get(), *cell.stamp.get(), bytes)
        };
        // Free the cell for the next lap. Release, so a producer that acquires
        // this sequence sees the read as finished.
        cell.seq.store(pos.wrapping_add(self.mask + 1), Ordering::Release);
        Some(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_full_queue_refuses_rather_than_overwriting() {
        let q = MsgQueue::new(8, DEFAULT_CELL_BYTES);
        for i in 0..8u8 { assert!(q.push(&[i], 1, i as u64), "{i} should fit"); }
        assert!(!q.push(&[99], 1, 99), "the ninth must be refused, not swallowed");
        // And the eight that were taken are all still there, in order — an
        // overwriting queue would have lost the first.
        for i in 0..8u8 {
            let got = q.pop_with(|_, _, b| b[0]).expect("eight were accepted");
            assert_eq!(got, i);
        }
        assert!(q.pop_with(|_, _, _| ()).is_none());
    }

    #[test]
    fn a_message_too_long_for_a_cell_is_refused_whole() {
        let q = MsgQueue::new(8, DEFAULT_CELL_BYTES);
        let big = vec![0u8; DEFAULT_CELL_BYTES + 1];
        assert!(!q.push(&big, 1, 0));
        // Never truncated: a short MIDI message is a different message, and a
        // reader cannot tell a truncated one from a genuine one.
        assert!(q.is_empty());
        assert!(q.push(&big[..DEFAULT_CELL_BYTES], 1, 0), "exactly full must fit");
    }

    #[test]
    fn a_queue_has_the_cell_width_it_was_built_with() {
        // Sixteen-byte cells for MIDI, sixty-four-kilobyte ones for a sysex
        // dump: the width is the queue's, not the crate's.
        let small = MsgQueue::new(8, 16);
        assert_eq!(small.cell_bytes(), 16);
        assert_eq!(small.bytes_reserved(), 8 * 16);
        assert!(small.push(&[0u8; 16], 1, 0), "exactly a cell fits");
        assert!(!small.push(&[0u8; 17], 1, 1), "one byte over does not");
        let got = small.pop_with(|_, _, b| b.len()).expect("one was taken");
        assert_eq!(got, 16);

        // Two is the shallowest a queue can be: with one cell the sequence
        // that means "taken" reads the same as the next position, and a
        // second push would claim the cell out from under the first.
        let wide = MsgQueue::new(2, 65536);
        assert_eq!(wide.capacity(), 2);
        assert!(wide.push(&vec![7u8; 65536], 1, 0));
        assert!(wide.push(&vec![8u8; 65536], 1, 1));
        assert!(!wide.push(&[1], 1, 2), "two cells, both taken");
        let got = wide.pop_with(|_, _, b| (b.len(), b[65535])).expect("taken");
        assert_eq!(got, (65536, 7));
    }

    #[test]
    fn what_goes_in_comes_out_intact_and_in_arrival_order() {
        let q = MsgQueue::new(16, DEFAULT_CELL_BYTES);
        for i in 0..16u64 {
            let body = vec![i as u8; (i as usize) + 1];
            assert!(q.push(&body, -(i as i64), 1000 + i));
        }
        for i in 0..16u64 {
            let (when, stamp, len, first) = q
                .pop_with(|w, s, b| (w, s, b.len(), b[0]))
                .expect("all sixteen were accepted");
            assert_eq!(when, -(i as i64), "the timetag rides with the message");
            assert_eq!(stamp, 1000 + i, "the stamp rides with it too");
            assert_eq!(len, i as usize + 1);
            assert_eq!(first, i as u8);
        }
    }

    #[test]
    fn producers_on_many_threads_lose_nothing_and_tear_nothing() {
        use std::sync::Arc;
        const PRODUCERS: usize = 4;
        // Miri interprets every spin of the yield loops below; the native run
        // keeps the volume.
        let each: usize = if cfg!(miri) { 40 } else { 2000 };
        let q = Arc::new(MsgQueue::new(64, DEFAULT_CELL_BYTES));
        let mut hs = Vec::new();
        for p in 0..PRODUCERS {
            let q = Arc::clone(&q);
            hs.push(std::thread::spawn(move || {
                let mut sent = 0usize;
                for i in 0..each {
                    // A body whose every byte repeats the producer id, so a
                    // torn write shows up as a byte that disagrees with the
                    // rest rather than as a silent mixture.
                    let body = vec![p as u8; 1 + (i % 64)];
                    while !q.push(&body, 1, i as u64) { std::thread::yield_now(); }
                    sent += 1;
                }
                sent
            }));
        }
        let mut seen = [0usize; PRODUCERS];
        let mut total = 0usize;
        let want = PRODUCERS * each;
        while total < want {
            match q.pop_with(|_, _, b| {
                let id = b[0] as usize;
                assert!(b.iter().all(|&x| x as usize == id), "a message tore");
                id
            }) {
                Some(id) => { seen[id] += 1; total += 1; }
                None => std::thread::yield_now(),
            }
        }
        for h in hs { assert_eq!(h.join().unwrap(), each); }
        assert_eq!(seen, [each; PRODUCERS]);
    }
}
