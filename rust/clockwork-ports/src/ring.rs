// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The frame ring: single producer, single consumer, lock-free, no allocation
//! after construction.
//!
//! # Why this shape
//!
//! Two monotonic counters, never masked in storage, masked only when
//! addressing. `write - read` is the fill and `cap - (write - read)` is the
//! room, and neither needs a spare slot or a wrap flag to tell full from
//! empty — the two states differ by 0 vs `cap`, not by two equal indices. The
//! counters are `usize` and wrap at 2^64 on any host this runs on, which at
//! 48 kHz is longer than the heat death of the recording session.
//!
//! Capacity is a power of two so addressing is a mask rather than a modulo:
//! the audio thread pays one `and` per segment, not a division.
//!
//! Storage is INTERLEAVED because that is what the endpoint side of
//! `clockwork_ports.h` speaks (`clockwork_port_produce` / `clockwork_port_consume` take
//! interleaved buffers) and because one frame is then contiguous — a partial
//! transfer can never split a frame across the wrap, which is the "no torn
//! frames" property the concurrency test asserts. The audio-thread side is
//! planar and pays a strided copy. That asymmetry is deliberate: the strided
//! side is the one that copies a whole block at a fixed cadence, and the
//! contiguous side is the one that must never tear.
//!
//! # The memory ordering
//!
//! Producer: `Acquire` on `read` (so the space it computes is not stale in the
//! direction that matters), then the copy, then `Release` on `write`.
//! Consumer: `Acquire` on `write`, the copy, `Release` on `read`. The two
//! releases are what publish the data; nothing else synchronises, and there is
//! no fence on the fast path beyond those.
//!
//! Each counter sits on its own 64-byte line. Producer and consumer are on
//! different threads by construction, and sharing a line between them is the
//! classic way to make a lock-free ring slower than a mutex.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicUsize, Ordering};

/// One counter, alone on its cache line.
#[repr(align(64))]
struct Padded(AtomicUsize);

/// The two counters, each on its own line. This is ALSO the layout of the
/// header of a ring in caller-owned storage (`FrameRing::in_storage`): the
/// write counter at byte 0, the read counter at byte 64, 128 bytes in all —
/// `CLOCKWORK_PORT_SHARED_HEADER_BYTES` in `src/clockwork_ports.h`, which a C++ process
/// on the other side of a shared-memory segment lays out by hand.
#[repr(C)]
struct Counters {
    write: Padded,
    read: Padded,
}

/// Bytes of header a ring in caller-owned storage needs. Fixed by contract.
pub const SHARED_HEADER_BYTES: usize = 128;

/// The smallest ring anyone gets, whatever they asked for. A ring shallower
/// than a block is a ring that underruns every block.
pub const MIN_CAPACITY_FRAMES: u32 = 16;
/// The deepest ring anyone gets: 2^24 frames is ~6 minutes of stereo at
/// 48 kHz and 128 MiB of memory. Past that the caller wants a file, not a ring.
pub const MAX_CAPACITY_FRAMES: u32 = 1 << 24;
/// Channel ceiling. Bounds the stack scratch nothing here needs and keeps a
/// nonsense argument from asking for a terabyte.
pub const MAX_CHANNELS: u32 = 64;

/// Round up to a power of two, clamped into the allowed range.
pub fn round_capacity(frames: u32) -> u32 {
    let f = frames.clamp(MIN_CAPACITY_FRAMES, MAX_CAPACITY_FRAMES);
    if f.is_power_of_two() { f } else { f.next_power_of_two() }
}

/// Bytes a ring of this shape needs in caller-owned storage: the counter
/// header, then `capacity * channels` interleaved samples.
pub fn storage_bytes(capacity_frames: u32, channels: u32) -> usize {
    SHARED_HEADER_BYTES + capacity_frames as usize * channels as usize * core::mem::size_of::<f32>()
}

/// Where a ring's samples and counters live.
///
/// # Two kinds of storage, one ring
///
/// A ring OWNS its storage in the ordinary case: a boxed slice and a boxed
/// pair of counters, freed with the port. It BORROWS it when the caller hands
/// over memory of its own — a shared-memory segment mapped by two processes,
/// where the producer is in one and the consumer in the other. The ring
/// logic is identical; only where the bytes are differs, and the counters
/// travel with the bytes so that both processes agree on the fill.
///
/// Owned samples sit in `UnsafeCell`s. The producer writes its region while
/// the consumer reads another through the same `&FrameRing`, and memory
/// reached through a shared reference is immutable unless it is in a cell:
/// this is what makes those writes defined rather than a write through a
/// pointer that only ever granted reads (Miri caught the earlier shape).
///
/// Borrowed counters are not trusted (`src/clockwork_ports.h`): the other process
/// may be dead, wedged or hostile, and the arithmetic below is saturating
/// and every address is masked, so a nonsense counter can cost frames — a
/// drop, a block of stale samples — and never a memory fault.
enum Storage {
    Owned {
        buf: Box<[UnsafeCell<f32>]>,
        counters: Box<Counters>,
    },
    Borrowed {
        data: *mut f32,
        counters: *const Counters,
    },
}

pub struct FrameRing {
    /// `cap_frames * channels` samples, interleaved. The producer writes the
    /// region the consumer is not reading, and the counters are what
    /// establish that.
    storage: Storage,
    cap_frames: usize,
    mask: usize,
    channels: usize,
}

// SAFETY: the two counters make the split safe: whoever holds the producer
// role touches only [write, write+space), whoever holds the consumer role
// touches only [read, read+fill), and those never overlap. The samples are
// in cells (owned) or in memory the caller vouched for (borrowed).
unsafe impl Send for FrameRing {}
// SAFETY: as above — every access through `&FrameRing` is to a region the
// counters assign to exactly one role.
unsafe impl Sync for FrameRing {}

impl FrameRing {
    /// `capacity_frames` is rounded up to a power of two by the caller
    /// (`round_capacity`); this asserts rather than silently misbehaving.
    pub fn new(capacity_frames: u32, channels: u32) -> FrameRing {
        let cap = capacity_frames as usize;
        assert!(cap.is_power_of_two(), "ring capacity must be a power of two");
        let ch = channels as usize;
        assert!(ch > 0, "a ring with no channels carries nothing");
        let mut buf = Vec::with_capacity(cap * ch);
        buf.resize_with(cap * ch, || UnsafeCell::new(0.0f32));
        FrameRing {
            storage: Storage::Owned {
                buf: buf.into_boxed_slice(),
                counters: Box::new(Counters {
                    write: Padded(AtomicUsize::new(0)),
                    read: Padded(AtomicUsize::new(0)),
                }),
            },
            cap_frames: cap,
            mask: cap - 1,
            channels: ch,
        }
    }

    /// A ring over memory the caller owns: `storage_bytes(capacity, channels)`
    /// bytes at `base`, 64-byte aligned, holding the counter header and then
    /// the samples. With `reset` the counters are zeroed — the side that
    /// CREATES the memory does that, once, before anyone else can see it; a
    /// side that JOINS memory another process is already using must not, or
    /// it would throw away whatever is in flight.
    ///
    /// # Safety
    /// `base` must be valid, 64-byte aligned, and stay mapped for as long as
    /// the ring exists; nothing else may write the header except through a
    /// `FrameRing` over the same memory on the other side of the transfer.
    pub unsafe fn in_storage(
        base: *mut u8, capacity_frames: u32, channels: u32, reset: bool,
    ) -> FrameRing {
        let cap = capacity_frames as usize;
        assert!(cap.is_power_of_two(), "ring capacity must be a power of two");
        let ch = channels as usize;
        assert!(ch > 0, "a ring with no channels carries nothing");
        assert!(!base.is_null() && (base as usize) % 64 == 0, "ring storage must be 64-byte aligned");
        let counters = base.cast::<Counters>().cast_const();
        if reset {
            // SAFETY: `base` is valid and aligned per the contract above, and
            // `Counters` is the header the caller laid out at it.
            let c = unsafe { &*counters };
            c.write.0.store(0, Ordering::Relaxed);
            c.read.0.store(0, Ordering::Release);
        }
        FrameRing {
            storage: Storage::Borrowed {
                // SAFETY: the samples follow the header inside the same mapping.
                data: unsafe { base.add(SHARED_HEADER_BYTES) }.cast::<f32>(),
                counters,
            },
            cap_frames: cap,
            mask: cap - 1,
            channels: ch,
        }
    }

    #[inline]
    fn counters(&self) -> &Counters {
        match &self.storage {
            Storage::Owned { counters, .. } => counters,
            // SAFETY: the pointer was valid at construction and the caller of
            // `in_storage` promised the mapping outlives the ring.
            Storage::Borrowed { counters, .. } => unsafe { &**counters },
        }
    }

    #[inline]
    fn write_counter(&self) -> &AtomicUsize { &self.counters().write.0 }
    #[inline]
    fn read_counter(&self) -> &AtomicUsize { &self.counters().read.0 }

    /// The fill as the counters state it, clamped to the capacity: a
    /// borrowed counter can say anything, and a fill past the capacity is a
    /// lie this must not act on.
    #[inline]
    fn fill(&self, w: usize, r: usize) -> usize {
        core::cmp::min(w.wrapping_sub(r), self.cap_frames)
    }

    pub fn channels(&self) -> usize { self.channels }
    pub fn capacity_frames(&self) -> usize { self.cap_frames }

    /// Frames waiting. Consumer's view; a producer may only ever see this grow.
    pub fn readable(&self) -> usize {
        let w = self.write_counter().load(Ordering::Acquire);
        let r = self.read_counter().load(Ordering::Acquire);
        self.fill(w, r)
    }

    /// Frames that would fit. Producer's view.
    pub fn writable(&self) -> usize {
        self.cap_frames - self.readable()
    }

    #[inline]
    fn slots(&self) -> *mut f32 {
        match &self.storage {
            // A pointer INTO the cells, which is what makes writing through
            // it defined while the consumer reads other cells.
            Storage::Owned { buf, .. } => UnsafeCell::raw_get(buf.as_ptr()),
            Storage::Borrowed { data, .. } => *data,
        }
    }

    /// The two contiguous segments starting at monotonic frame `pos` covering
    /// `n` frames: (first index, first length, second length). The second
    /// segment always starts at index 0.
    #[inline]
    fn segments(&self, pos: usize, n: usize) -> (usize, usize, usize) {
        let start = pos & self.mask;
        let first = core::cmp::min(n, self.cap_frames - start);
        (start, first, n - first)
    }

    /// The producer's region for `n` frames from `w`, as two exclusive
    /// slices of samples (the second is the wrap, possibly empty).
    ///
    /// # Safety
    /// The caller holds the producer role, and `n` is at most the room the
    /// counters reported: the region is then disjoint from everything the
    /// consumer may be reading, so exclusive access is real.
    // `&mut` out of `&self` is the whole point: the counters, not the borrow
    // checker, are what make the producer's region exclusive.
    #[allow(clippy::mut_from_ref)]
    #[inline]
    unsafe fn write_region(&self, w: usize, n: usize) -> (&mut [f32], &mut [f32]) {
        let ch = self.channels;
        let (start, first, second) = self.segments(w, n);
        let base = self.slots();
        // SAFETY: both ranges lie inside the buffer (masked start, lengths
        // bounded by the capacity) and, per the contract, nobody else touches
        // them until the write counter is published.
        unsafe {
            (core::slice::from_raw_parts_mut(base.add(start * ch), first * ch),
             core::slice::from_raw_parts_mut(base, second * ch))
        }
    }

    /// The consumer's region for `n` frames from `r`, as two shared slices.
    ///
    /// # Safety
    /// The caller holds the consumer role, and `n` is at most the fill the
    /// counters reported: the producer does not write this region until the
    /// read counter is published past it.
    #[inline]
    unsafe fn read_region(&self, r: usize, n: usize) -> (&[f32], &[f32]) {
        let ch = self.channels;
        let (start, first, second) = self.segments(r, n);
        let base = self.slots();
        // SAFETY: as for write_region, with shared access.
        unsafe {
            (core::slice::from_raw_parts(base.add(start * ch), first * ch),
             core::slice::from_raw_parts(base, second * ch))
        }
    }

    // ── Producer side ──────────────────────────────────────────────────────

    /// Copy interleaved frames in: `src.len() / channels` of them. Returns how
    /// many were taken, which is `min(frames, writable())`. Never blocks,
    /// never allocates. Producer role, single thread.
    pub fn push_interleaved(&self, src: &[f32]) -> usize {
        let ch = self.channels;
        let frames = src.len() / ch;
        let w = self.write_counter().load(Ordering::Relaxed);
        let r = self.read_counter().load(Ordering::Acquire);
        let room = self.cap_frames - self.fill(w, r);
        let n = core::cmp::min(frames, room);
        if n == 0 { return 0; }

        // SAFETY: producer role; `n <= room`.
        let (a, b) = unsafe { self.write_region(w, n) };
        a.copy_from_slice(&src[..a.len()]);
        b.copy_from_slice(&src[a.len()..a.len() + b.len()]);
        self.write_counter().store(w.wrapping_add(n), Ordering::Release);
        n
    }

    /// Copy planar frames in, one slice per caller channel (`None` for a
    /// channel the caller has no buffer for). Caller channels beyond the
    /// ring's are discarded; ring channels the caller does not offer, and
    /// samples past the end of a short slice, are written as silence, so a
    /// frame in the ring is always whole. Producer role, single thread.
    pub fn push_planar(&self, src: &[Option<&[f32]>], frames: usize) -> usize {
        let w = self.write_counter().load(Ordering::Relaxed);
        let r = self.read_counter().load(Ordering::Acquire);
        let room = self.cap_frames - self.fill(w, r);
        let n = core::cmp::min(frames, room);
        if n == 0 { return 0; }

        let ch = self.channels;
        // SAFETY: producer role; `n <= room`.
        let (a, b) = unsafe { self.write_region(w, n) };
        let first = a.len() / ch;
        for c in 0..ch {
            let s: &[f32] = src.get(c).copied().flatten().unwrap_or(&[]);
            for i in 0..first {
                a[i * ch + c] = s.get(i).copied().unwrap_or(0.0);
            }
            for i in 0..(n - first) {
                b[i * ch + c] = s.get(first + i).copied().unwrap_or(0.0);
            }
        }
        self.write_counter().store(w.wrapping_add(n), Ordering::Release);
        n
    }

    // ── Consumer side ──────────────────────────────────────────────────────

    /// Copy interleaved frames out, up to `dst.len() / channels` of them.
    /// Returns how many were available. Consumer role, single thread.
    pub fn pop_interleaved(&self, dst: &mut [f32]) -> usize {
        let ch = self.channels;
        let frames = dst.len() / ch;
        let r = self.read_counter().load(Ordering::Relaxed);
        let w = self.write_counter().load(Ordering::Acquire);
        let n = core::cmp::min(frames, self.fill(w, r));
        if n == 0 { return 0; }

        // SAFETY: consumer role; `n <= fill`.
        let (a, b) = unsafe { self.read_region(r, n) };
        dst[..a.len()].copy_from_slice(a);
        dst[a.len()..a.len() + b.len()].copy_from_slice(b);
        self.read_counter().store(r.wrapping_add(n), Ordering::Release);
        n
    }

    /// Copy planar frames out, one slice per caller channel (`None` for a
    /// channel the caller does not want), and ZERO the remainder of every
    /// caller channel out to `frames`. Caller channels the ring does not
    /// have are zeroed whole; ring channels the caller does not take are
    /// discarded.
    ///
    /// Returns how many frames were really there. The zero-fill is the
    /// silence-never-stale policy, done here rather than by the caller so
    /// that there is one place it can be got wrong. Consumer role, single
    /// thread.
    pub fn pop_planar(&self, dst: &mut [Option<&mut [f32]>], frames: usize) -> usize {
        let r = self.read_counter().load(Ordering::Relaxed);
        let w = self.write_counter().load(Ordering::Acquire);
        let n = core::cmp::min(frames, self.fill(w, r));

        let ch = self.channels;
        // SAFETY: consumer role; `n <= fill`. With n == 0 both slices are
        // empty and nothing is read.
        let (a, b) = unsafe { self.read_region(r, n) };
        let first = a.len() / ch;
        for (c, d) in dst.iter_mut().enumerate() {
            let Some(d) = d.as_deref_mut() else { continue };
            let end = frames.min(d.len());
            if c < ch {
                for i in 0..first.min(end) {
                    d[i] = a[i * ch + c];
                }
                for i in first..n.min(end) {
                    d[i] = b[(i - first) * ch + c];
                }
                // The shortfall: silence, not stale.
                for v in &mut d[n.min(end)..end] { *v = 0.0; }
            } else {
                // A channel the port does not have: silence, not stale.
                for v in &mut d[..end] { *v = 0.0; }
            }
        }
        if n > 0 {
            self.read_counter().store(r.wrapping_add(n), Ordering::Release);
        }
        n
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Interleaved frame `f`, channel `c`. Integers below 2^24 are exact in
    /// f32, so every assertion in these tests is an equality, not a tolerance.
    fn sample(f: usize, c: usize) -> f32 { (f * 8 + c) as f32 }

    fn fill(frames: usize, channels: usize) -> Vec<f32> {
        (0..frames * channels).map(|i| sample(i / channels, i % channels)).collect()
    }

    #[test]
    fn capacity_rounds_up_to_a_power_of_two() {
        assert_eq!(round_capacity(1000), 1024);
        assert_eq!(round_capacity(1024), 1024);
        assert_eq!(round_capacity(1025), 2048);
        // Clamped at both ends rather than refused.
        assert_eq!(round_capacity(0), MIN_CAPACITY_FRAMES);
        assert_eq!(round_capacity(1), MIN_CAPACITY_FRAMES);
        assert_eq!(round_capacity(u32::MAX), MAX_CAPACITY_FRAMES);
    }

    #[test]
    fn interleaved_round_trip_is_sample_for_sample() {
        let r = FrameRing::new(64, 3);
        let src = fill(40, 3);
        assert_eq!(r.push_interleaved(&src), 40);
        let mut dst = vec![-1.0f32; 40 * 3];
        assert_eq!(r.pop_interleaved(&mut dst), 40);
        assert_eq!(dst, src);
    }

    #[test]
    fn fill_and_room_are_exact() {
        let r = FrameRing::new(16, 2);
        assert_eq!(r.readable(), 0);
        assert_eq!(r.writable(), 16);
        let src = fill(10, 2);
        assert_eq!(r.push_interleaved(&src), 10);
        assert_eq!(r.readable(), 10);
        assert_eq!(r.writable(), 6);
        // A full ring is `cap` readable and 0 writable — the two states are
        // told apart by the counters' difference, not by a spare slot.
        assert_eq!(r.push_interleaved(&src), 6);
        assert_eq!(r.readable(), 16);
        assert_eq!(r.writable(), 0);
        assert_eq!(r.push_interleaved(&src[..2]), 0);
    }

    #[test]
    fn frames_survive_the_wrap_whole() {
        // Push and pop 7 frames at a time through a 16-frame ring for long
        // enough that every possible split lands on the boundary.
        let r = FrameRing::new(16, 3);
        let mut next_in = 0usize;
        let mut next_out = 0usize;
        for _ in 0..200 {
            let src: Vec<f32> = (0..7 * 3)
                .map(|i| sample(next_in + i / 3, i % 3)).collect();
            let took = r.push_interleaved(&src);
            next_in += took;

            let mut dst = vec![f32::NAN; 7 * 3];
            let got = r.pop_interleaved(&mut dst);
            for f in 0..got {
                for c in 0..3 {
                    assert_eq!(dst[f * 3 + c], sample(next_out + f, c),
                               "torn or reordered frame at {}", next_out + f);
                }
            }
            next_out += got;
        }
        assert!(next_out > 1000, "the test did not actually move anything");
    }

    #[test]
    fn planar_read_zero_fills_the_shortfall() {
        let r = FrameRing::new(16, 2);
        r.push_interleaved(&fill(4, 2));

        let mut a = [9.0f32; 8];
        let mut b = [9.0f32; 8];
        let got = r.pop_planar(&mut [Some(&mut a[..]), Some(&mut b[..])], 8);
        assert_eq!(got, 4);
        for f in 0..4 {
            assert_eq!(a[f], sample(f, 0));
            assert_eq!(b[f], sample(f, 1));
        }
        // Silence, not stale, and not the 9.0 the caller left there.
        for f in 4..8 { assert_eq!(a[f], 0.0); assert_eq!(b[f], 0.0); }
    }

    #[test]
    fn a_caller_with_more_channels_gets_silence_on_the_extras() {
        let r = FrameRing::new(16, 1);
        r.push_interleaved(&fill(4, 1));

        let mut a = [9.0f32; 4];
        let mut b = [9.0f32; 4];
        assert_eq!(r.pop_planar(&mut [Some(&mut a[..]), Some(&mut b[..])], 4), 4);
        for f in 0..4 {
            assert_eq!(a[f], sample(f, 0));
            assert_eq!(b[f], 0.0, "channel the port does not have must be silent");
        }
    }

    #[test]
    fn a_caller_with_fewer_channels_has_its_extras_discarded() {
        let r = FrameRing::new(16, 3);
        let a = [1.0f32; 4];
        let b = [2.0f32; 4];
        let c = [3.0f32; 4];
        // The port has three, the caller offers two: the third is silence.
        assert_eq!(r.push_planar(&[Some(&a[..]), Some(&b[..])], 4), 4);
        let mut out = vec![f32::NAN; 4 * 3];
        assert_eq!(r.pop_interleaved(&mut out), 4);
        for f in 0..4 {
            assert_eq!(out[f * 3], 1.0);
            assert_eq!(out[f * 3 + 1], 2.0);
            assert_eq!(out[f * 3 + 2], 0.0, "unoffered port channel must be silent");
        }

        // And the other way: a caller with more channels than the port loses
        // the extras rather than being refused.
        let r2 = FrameRing::new(16, 2);
        assert_eq!(r2.push_planar(&[Some(&a[..]), Some(&b[..]), Some(&c[..])], 4), 4);
        let mut out2 = vec![f32::NAN; 4 * 2];
        assert_eq!(r2.pop_interleaved(&mut out2), 4);
        for f in 0..4 {
            assert_eq!(out2[f * 2], 1.0);
            assert_eq!(out2[f * 2 + 1], 2.0);
        }
    }

    #[test]
    fn a_missing_or_short_planar_channel_is_silence_never_a_fault() {
        // A `None` channel and a slice shorter than the block: what a C
        // caller's null pointer and short buffer become at the edge. Both are
        // silence in the ring, and neither reads past anything.
        let r = FrameRing::new(16, 2);
        let short = [5.0f32; 2];
        assert_eq!(r.push_planar(&[None, Some(&short[..])], 4), 4);
        let mut out = vec![f32::NAN; 4 * 2];
        assert_eq!(r.pop_interleaved(&mut out), 4);
        for f in 0..4 {
            assert_eq!(out[f * 2], 0.0, "no buffer offered: silence");
            assert_eq!(out[f * 2 + 1], if f < 2 { 5.0 } else { 0.0 }, "past a short slice: silence");
        }

        // And on the way out: a short destination takes what fits, a None
        // channel is skipped, and the rest is untouched.
        r.push_interleaved(&fill(4, 2));
        let mut a = [9.0f32; 2];
        assert_eq!(r.pop_planar(&mut [Some(&mut a[..]), None], 4), 4);
        assert_eq!(a, [sample(0, 0), sample(1, 0)]);
    }

    #[test]
    fn a_read_from_an_empty_ring_is_a_whole_block_of_silence() {
        let r = FrameRing::new(16, 2);
        let mut a = [7.0f32; 8];
        let mut b = [7.0f32; 8];
        assert_eq!(r.pop_planar(&mut [Some(&mut a[..]), Some(&mut b[..])], 8), 0);
        assert!(a.iter().all(|&v| v == 0.0));
        assert!(b.iter().all(|&v| v == 0.0));
    }

    /// The borrowed shape: two rings over ONE block of memory, one holding
    /// the producer role and one the consumer, the way two processes would.
    /// Frames pushed through one come out of the other, sample for sample.
    #[test]
    fn two_rings_over_one_storage_see_the_same_frames() {
        #[repr(align(64))]
        struct Aligned([u8; 128 + 16 * 2 * 4]);
        let mut mem = Aligned([0xAAu8; 128 + 16 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        // SAFETY: `mem` is aligned, sized by storage_bytes, and outlives both rings.
        let (a, b) = unsafe {
            (FrameRing::in_storage(base, 16, 2, true),
             FrameRing::in_storage(base, 16, 2, false))
        };
        // The creator's reset zeroed the counters the joiner reads.
        assert_eq!(b.readable(), 0);
        let src = fill(10, 2);
        assert_eq!(a.push_interleaved(&src), 10);
        assert_eq!(b.readable(), 10);
        let mut dst = vec![-1.0f32; 10 * 2];
        assert_eq!(b.pop_interleaved(&mut dst), 10);
        assert_eq!(dst, src);
        assert_eq!(a.writable(), 16);
        assert_eq!(storage_bytes(16, 2), 128 + 16 * 2 * 4);
    }

    /// A counter the other side has corrupted costs frames, never memory:
    /// a read counter far ahead of the write counter reads as "full", so a
    /// push takes nothing and a pop hands back at most a ring's worth, all
    /// of it from inside the buffer.
    #[test]
    fn a_hostile_counter_is_clamped_not_trusted() {
        #[repr(align(64))]
        struct Aligned([u8; 128 + 16 * 2 * 4]);
        let mut mem = Aligned([0u8; 128 + 16 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        // SAFETY: `mem` is aligned, sized by storage_bytes, and outlives the ring.
        let r = unsafe { FrameRing::in_storage(base, 16, 2, true) };
        // The read counter, overwritten from "outside", far ahead of the
        // write counter: the wrapping difference is nearly usize::MAX.
        // SAFETY: the header at `base` is the Counters the ring laid out.
        unsafe { (*base.cast::<Counters>()).read.0.store(100, Ordering::Relaxed) };
        assert_eq!(r.readable(), 16, "a fill past the capacity is reported as full");
        assert_eq!(r.writable(), 0);
        assert_eq!(r.push_interleaved(&fill(4, 2)), 0);
        let mut dst = vec![f32::NAN; 40 * 2];
        // At most the capacity comes back, and the counter simply advances.
        assert_eq!(r.pop_interleaved(&mut dst), 16);
    }
}
