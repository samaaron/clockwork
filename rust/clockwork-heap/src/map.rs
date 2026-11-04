// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Which core's heap owns an address (docs/ENGINES.md).
//!
//! A free can come from any thread and has only the address to go on. This
//! is how it finds the heap that owns the block: a radix map from each
//! 64 KiB chunk of the address space to the heap whose memory it is, as
//! jemalloc's rtree maps an address to its extent, in miniature. A lookup is
//! three dependent loads, wait-free, on any thread, the audio thread's
//! included.
//!
//! A core's memory is published when it is laid out and cleared when the
//! core closes, before the memory goes back: on a control thread, never on
//! the audio thread, because publishing may make the map's nodes. A range
//! names every chunk it touches, so the owner a lookup finds may not hold
//! the address itself, at a range's ragged edge: the owner checks its exact
//! span. Two owners never share a chunk; a process that holds several cores
//! takes each core's memory on chunks. Nodes are
//! made as they are needed, straight from the system allocator, so the map
//! never lands in a core's pool, and are never freed, so a lookup never meets
//! freed memory and there is no cap on cores. Nothing outlives its core: a
//! free after the core has gone is a bug, as a free after `dsp_free` is.
//!
//! The owner is opaque here: the core's heap says what it is.

use core::ffi::c_void;
use core::ptr::{self, NonNull};

#[cfg(not(loom))]
use core::sync::atomic::{AtomicPtr, Ordering};
#[cfg(loom)]
use loom::sync::atomic::{AtomicPtr, Ordering};

/// The map's grain: a published range names every chunk of this many bytes
/// it touches.
pub const CHUNK_BYTES: usize = 1 << CHUNK_SHIFT;
const CHUNK_SHIFT: u32 = 16;
// src/clockwork_heap.h names the same grain to C (CLOCKWORK_HEAP_CHUNK_BYTES).
const _: () = assert!(CHUNK_BYTES == 65536);

// How a chunk's number splits across the three levels. On 64-bit targets the
// map covers the low 2^48 bytes, all that any of them hands a process; on
// 32-bit ones, all of it. Under loom a few bits each, so a model stays small.
#[cfg(all(not(loom), target_pointer_width = "64"))]
const BITS: [u32; 3] = [10, 11, 11];
#[cfg(all(not(loom), not(target_pointer_width = "64")))]
const BITS: [u32; 3] = [4, 4, 8];
#[cfg(loom)]
const BITS: [u32; 3] = [1, 1, 2];

const ROOT: usize = 1 << BITS[0];
const MID: usize = 1 << BITS[1];
const LEAF: usize = 1 << BITS[2];
/// The first address past what the map covers.
const COVERED: u64 = 1 << (CHUNK_SHIFT + BITS[0] + BITS[1] + BITS[2]);

/// Why a range was not published. Nothing of it is mapped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refused {
    /// Empty, or reaching past what the map covers.
    Range,
    /// The system could not make a node of the map.
    NoMemory,
    /// Part of the range is another owner's.
    Taken,
}

/// The map. The process has one, [`MAP`]; a test makes its own.
pub struct Map {
    mids: [AtomicPtr<Mid>; ROOT],
}

struct Mid {
    leaves: [AtomicPtr<Leaf>; MID],
}

struct Leaf {
    owners: [AtomicPtr<c_void>; LEAF],
}

/// The process's map, shared by every core by design (docs/ENGINES.md).
#[cfg(not(loom))]
pub static MAP: Map = Map::new();

impl Map {
    #[cfg(not(loom))]
    pub const fn new() -> Map {
        Map { mids: [const { AtomicPtr::new(ptr::null_mut()) }; ROOT] }
    }

    #[cfg(loom)]
    pub fn new() -> Map {
        Map { mids: core::array::from_fn(|_| AtomicPtr::new(ptr::null_mut())) }
    }

    /// The owner of `addr`'s chunk, or null: the owner whose span holds
    /// `addr`, if anyone's does. Any thread, wait-free.
    pub fn owner_of(&self, addr: usize) -> *mut c_void {
        let Some((r, m, l)) = split(addr) else { return ptr::null_mut() };
        let mid = self.mids[r].load(Ordering::Acquire);
        if mid.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: a node published here is whole (made before its release)
        // and is freed only when the map goes.
        let leaf = unsafe { &*mid }.leaves[m].load(Ordering::Acquire);
        if leaf.is_null() {
            return ptr::null_mut();
        }
        // SAFETY: as above.
        unsafe { &*leaf }.owners[l].load(Ordering::Acquire)
    }

    /// Map every chunk `[base, base + bytes)` touches to `owner`. A control
    /// thread, never the audio thread: it may make the map's nodes. Refused,
    /// it maps nothing of the range.
    pub fn publish(&self, base: usize, bytes: usize, owner: NonNull<c_void>) -> Result<(), Refused> {
        let chunks = chunks(base, bytes).ok_or(Refused::Range)?;
        for (i, chunk) in chunks.clone().enumerate() {
            if let Err(refused) = self.claim(chunk, owner) {
                chunks.take(i).for_each(|c| self.release(c, owner));
                return Err(refused);
            }
        }
        Ok(())
    }

    /// Forget `[base, base + bytes)` where it names `owner`; what names
    /// another owner, or nothing, stays as it is.
    pub fn clear(&self, base: usize, bytes: usize, owner: NonNull<c_void>) {
        if let Some(chunks) = chunks(base, bytes) {
            chunks.for_each(|c| self.release(c, owner));
        }
    }

    /// Where `[base, base + bytes)` names `from`, name `to` instead, each
    /// chunk in one step: no reader sees the range unowned between.
    pub fn retag(&self, base: usize, bytes: usize, from: NonNull<c_void>, to: NonNull<c_void>) {
        let Some(chunks) = chunks(base, bytes) else { return };
        for chunk in chunks {
            let Some((r, m, l)) = split(chunk) else { continue };
            let mid = self.mids[r].load(Ordering::Acquire);
            if mid.is_null() {
                continue;
            }
            // SAFETY: as in `owner_of`.
            let leaf = unsafe { &*mid }.leaves[m].load(Ordering::Acquire);
            if leaf.is_null() {
                continue;
            }
            // SAFETY: as in `owner_of`. A chunk another owner holds is left.
            let _ = unsafe { &*leaf }.owners[l].compare_exchange(from.as_ptr(), to.as_ptr(), Ordering::AcqRel, Ordering::Relaxed);
        }
    }

    /// Name `owner` for the chunk at `chunk`, which must be free: the map's
    /// nodes made on the way. A control thread, never the audio thread.
    pub fn claim(&self, chunk: usize, owner: NonNull<c_void>) -> Result<(), Refused> {
        self.entry(chunk)?
            .compare_exchange(ptr::null_mut(), owner.as_ptr(), Ordering::AcqRel, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| Refused::Taken)
    }

    /// Forget the chunk at `chunk` if it names `owner`.
    pub fn release(&self, chunk: usize, owner: NonNull<c_void>) {
        let Some((r, m, l)) = split(chunk) else { return };
        let mid = self.mids[r].load(Ordering::Acquire);
        if mid.is_null() {
            return;
        }
        // SAFETY: as in `owner_of`.
        let leaf = unsafe { &*mid }.leaves[m].load(Ordering::Acquire);
        if leaf.is_null() {
            return;
        }
        // SAFETY: as in `owner_of`. A chunk another owner holds is left.
        let _ = unsafe { &*leaf }.owners[l].compare_exchange(owner.as_ptr(), ptr::null_mut(), Ordering::AcqRel, Ordering::Relaxed);
    }

    /// The entry for `addr`'s chunk, making the nodes on the way.
    fn entry(&self, addr: usize) -> Result<&AtomicPtr<c_void>, Refused> {
        let (r, m, l) = split(addr).ok_or(Refused::Range)?;
        let mid = node(&self.mids[r])?;
        let leaf = node(&mid.leaves[m])?;
        Ok(&leaf.owners[l])
    }
}

impl Default for Map {
    fn default() -> Map {
        Map::new()
    }
}

impl Drop for Map {
    // Only a test's map goes; the process's lives as long as the process.
    fn drop(&mut self) {
        for at in &self.mids {
            let mid = at.load(Ordering::Acquire);
            if mid.is_null() {
                continue;
            }
            // SAFETY: made by `node`, and nothing else holds the map.
            for leaf in &unsafe { &*mid }.leaves {
                let leaf = leaf.load(Ordering::Acquire);
                if !leaf.is_null() {
                    // SAFETY: made by `node`, freed once.
                    unsafe { Leaf::unmake(leaf) };
                }
            }
            // SAFETY: made by `node`, freed once.
            unsafe { Mid::unmake(mid) };
        }
    }
}

/// `addr`'s chunk as (root, mid, leaf) indices, or `None` past the map.
fn split(addr: usize) -> Option<(usize, usize, usize)> {
    if addr as u64 >= COVERED {
        return None;
    }
    let chunk = addr >> CHUNK_SHIFT;
    Some((chunk >> (BITS[1] + BITS[2]), (chunk >> BITS[2]) & (MID - 1), chunk & (LEAF - 1)))
}

/// The address of every chunk `[base, base + bytes)` touches, when it is
/// not empty and lies within the map. Counted rather than ended: on a 32-bit
/// target the end of the last chunk is past every address.
pub fn chunks(base: usize, bytes: usize) -> Option<impl Iterator<Item = usize> + Clone> {
    let end = base as u64 + bytes as u64;
    if bytes == 0 || end > COVERED {
        return None;
    }
    let first = base - base % CHUNK_BYTES;
    let count = (end - first as u64).div_ceil(CHUNK_BYTES as u64) as usize;
    Some((0..count).map(move |i| first + i * CHUNK_BYTES))
}

/// Whether `[base, base + bytes)` touches the chunk at `chunk`.
pub fn touches(base: usize, bytes: usize, chunk: usize) -> bool {
    bytes != 0 && base < chunk.saturating_add(CHUNK_BYTES) && base + bytes > chunk
}

/// The node `at` points to, made and published if there is none yet; a
/// thread that loses the race to publish gives its own back and takes the
/// winner's.
fn node<T: Node>(at: &AtomicPtr<T>) -> Result<&T, Refused> {
    let mut p = at.load(Ordering::Acquire);
    if p.is_null() {
        let made = T::make().ok_or(Refused::NoMemory)?;
        p = match at.compare_exchange(ptr::null_mut(), made, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => made,
            Err(theirs) => {
                // SAFETY: ours, never published.
                unsafe { T::unmake(made) };
                theirs
            }
        };
    }
    // SAFETY: published whole, and freed only when the map goes.
    Ok(unsafe { &*p })
}

/// A node of the map: every entry starts null.
trait Node: Sized {
    fn make() -> Option<*mut Self>;
    /// # Safety
    /// `p` came from `make`, and is given back once.
    unsafe fn unmake(p: *mut Self);
}

// Outside loom a node is zeroed memory from the system allocator, never the
// global allocator, which may be serving a core's pool: an atomic pointer of
// all zero bits is null.
#[cfg(not(loom))]
macro_rules! node_from_the_system {
    ($t:ty) => {
        impl Node for $t {
            fn make() -> Option<*mut Self> {
                use std::alloc::{GlobalAlloc, Layout, System};
                // SAFETY: a non-zero size; all zero bits is every entry null.
                let p = unsafe { System.alloc_zeroed(Layout::new::<Self>()) };
                (!p.is_null()).then_some(p.cast())
            }
            unsafe fn unmake(p: *mut Self) {
                use std::alloc::{GlobalAlloc, Layout, System};
                // SAFETY: from `make`, with this layout.
                unsafe { System.dealloc(p.cast(), Layout::new::<Self>()) };
            }
        }
    };
}
#[cfg(not(loom))]
node_from_the_system!(Mid);
#[cfg(not(loom))]
node_from_the_system!(Leaf);

// Under loom an atomic is more than its bits, so a node is built.
#[cfg(loom)]
impl Node for Mid {
    fn make() -> Option<*mut Self> {
        Some(Box::into_raw(Box::new(Mid { leaves: core::array::from_fn(|_| AtomicPtr::new(ptr::null_mut())) })))
    }
    unsafe fn unmake(p: *mut Self) {
        // SAFETY: from `make`.
        drop(unsafe { Box::from_raw(p) });
    }
}
#[cfg(loom)]
impl Node for Leaf {
    fn make() -> Option<*mut Self> {
        Some(Box::into_raw(Box::new(Leaf { owners: core::array::from_fn(|_| AtomicPtr::new(ptr::null_mut())) })))
    }
    unsafe fn unmake(p: *mut Self) {
        // SAFETY: from `make`.
        drop(unsafe { Box::from_raw(p) });
    }
}

#[cfg(all(test, not(loom)))]
mod tests {
    use super::*;

    // An owner the map only names, never reads.
    fn owner(n: usize) -> NonNull<c_void> {
        NonNull::new(ptr::without_provenance_mut(n * 64)).unwrap()
    }

    const LEAF_BYTES: usize = LEAF * CHUNK_BYTES;
    const MID_BYTES: usize = MID * LEAF_BYTES;

    #[test]
    fn an_address_in_a_published_range_finds_its_owner_and_one_outside_does_not() {
        let map = Map::new();
        let base = 5 * CHUNK_BYTES;
        map.publish(base, 3 * CHUNK_BYTES, owner(1)).unwrap();
        assert_eq!(map.owner_of(base), owner(1).as_ptr());
        assert_eq!(map.owner_of(base + 3 * CHUNK_BYTES - 1), owner(1).as_ptr());
        assert!(map.owner_of(base - 1).is_null());
        assert!(map.owner_of(base + 3 * CHUNK_BYTES).is_null());
        assert!(map.owner_of(0).is_null());
        assert!(map.owner_of(usize::MAX).is_null());
    }

    #[test]
    fn a_range_across_leaves_and_mids_is_found_whole() {
        let map = Map::new();
        let base = MID_BYTES - 2 * CHUNK_BYTES;
        let bytes = 4 * CHUNK_BYTES + LEAF_BYTES;
        map.publish(base, bytes, owner(2)).unwrap();
        for addr in (base..base + bytes).step_by(CHUNK_BYTES / 4) {
            assert_eq!(map.owner_of(addr), owner(2).as_ptr(), "at {addr:#x}");
        }
        assert!(map.owner_of(base + bytes).is_null());
    }

    #[test]
    fn clearing_forgets_only_what_names_the_owner() {
        let map = Map::new();
        map.publish(0, 2 * CHUNK_BYTES, owner(1)).unwrap();
        map.publish(2 * CHUNK_BYTES, 2 * CHUNK_BYTES, owner(2)).unwrap();
        map.clear(0, 4 * CHUNK_BYTES, owner(1));
        assert!(map.owner_of(0).is_null());
        assert!(map.owner_of(CHUNK_BYTES).is_null());
        assert_eq!(map.owner_of(2 * CHUNK_BYTES), owner(2).as_ptr());
        assert_eq!(map.owner_of(3 * CHUNK_BYTES), owner(2).as_ptr());
        // Cleared, a range is the next owner's to take.
        map.publish(0, 2 * CHUNK_BYTES, owner(3)).unwrap();
        assert_eq!(map.owner_of(0), owner(3).as_ptr());
    }

    #[test]
    fn a_range_names_every_chunk_it_touches() {
        let map = Map::new();
        let base = 3 * CHUNK_BYTES + 100;
        map.publish(base, CHUNK_BYTES, owner(1)).unwrap();
        assert_eq!(map.owner_of(3 * CHUNK_BYTES), owner(1).as_ptr(), "the ragged start's chunk");
        assert_eq!(map.owner_of(4 * CHUNK_BYTES + 99), owner(1).as_ptr(), "the ragged end's chunk");
        assert!(map.owner_of(5 * CHUNK_BYTES).is_null());
        assert!(map.owner_of(3 * CHUNK_BYTES - 1).is_null());
        // A neighbour touching one of those chunks is refused, and maps none
        // of itself.
        assert_eq!(map.publish(4 * CHUNK_BYTES + 200, CHUNK_BYTES, owner(2)), Err(Refused::Taken));
        assert!(map.owner_of(5 * CHUNK_BYTES).is_null());
        map.clear(base, CHUNK_BYTES, owner(1));
        assert!(map.owner_of(3 * CHUNK_BYTES).is_null());
        assert!(map.owner_of(4 * CHUNK_BYTES).is_null());
    }

    #[test]
    fn a_range_that_is_empty_or_reaches_past_the_map_is_refused() {
        let map = Map::new();
        assert_eq!(map.publish(0, 0, owner(1)), Err(Refused::Range));
        // The last chunk the map covers is a chunk like any other; one past
        // it is not.
        let last = (COVERED - CHUNK_BYTES as u64) as usize;
        assert_eq!(map.publish(last, 2 * CHUNK_BYTES, owner(1)), Err(Refused::Range));
        map.publish(last, CHUNK_BYTES, owner(1)).unwrap();
        assert_eq!(map.owner_of(last + CHUNK_BYTES - 1), owner(1).as_ptr());
        assert!(map.owner_of(0).is_null());
    }

    #[test]
    fn a_range_partly_anothers_is_refused_and_maps_none_of_itself() {
        let map = Map::new();
        map.publish(2 * CHUNK_BYTES, 2 * CHUNK_BYTES, owner(1)).unwrap();
        assert_eq!(map.publish(0, 6 * CHUNK_BYTES, owner(2)), Err(Refused::Taken));
        for chunk in [0, 1, 4, 5] {
            assert!(map.owner_of(chunk * CHUNK_BYTES).is_null(), "chunk {chunk}");
        }
        assert_eq!(map.owner_of(2 * CHUNK_BYTES), owner(1).as_ptr());
        assert_eq!(map.owner_of(3 * CHUNK_BYTES), owner(1).as_ptr());
    }

    #[test]
    fn cores_published_from_many_threads_at_once_are_each_found() {
        let map = Map::new();
        std::thread::scope(|s| {
            for n in 0..8 {
                let map = &map;
                // Neighbours in one leaf, so the threads race to make it.
                s.spawn(move || map.publish(n * 2 * CHUNK_BYTES, 2 * CHUNK_BYTES, owner(n + 1)).unwrap());
            }
        });
        for n in 0..8 {
            assert_eq!(map.owner_of(n * 2 * CHUNK_BYTES + 1), owner(n + 1).as_ptr());
        }
    }
}

// Every interleaving of publishing and clearing against a lookup, on loom's
// atomics. Run with: RUSTFLAGS="--cfg loom" cargo test --release --lib -p
// clockwork-heap map.
#[cfg(all(test, loom))]
mod loom_tests {
    use super::*;
    use loom::cell::UnsafeCell;
    use loom::sync::Arc;
    use loom::thread;

    // What a core's heap has made before its memory is published.
    struct Heap {
        made: UnsafeCell<u32>,
    }

    fn named(heap: &Heap) -> NonNull<c_void> {
        NonNull::from(heap).cast()
    }

    // The nodes are there already, a neighbour's, so only the owner's own
    // publication orders what its heap made before the lookup.
    #[test]
    fn a_lookup_during_a_publish_finds_nothing_or_a_heap_already_made() {
        loom::model(|| {
            let map = Arc::new(Map::new());
            let neighbour = Heap { made: UnsafeCell::new(0) };
            map.publish(0, CHUNK_BYTES, named(&neighbour)).unwrap();
            let heap = Arc::new(Heap { made: UnsafeCell::new(0) });
            let publisher = {
                let (map, heap) = (map.clone(), heap.clone());
                thread::spawn(move || {
                    // SAFETY: nothing reads it until the heap is published.
                    heap.made.with_mut(|m| unsafe { *m = 7 });
                    map.publish(CHUNK_BYTES, CHUNK_BYTES, named(&heap)).unwrap();
                })
            };
            let found = map.owner_of(CHUNK_BYTES + 8);
            if !found.is_null() {
                assert_eq!(found, named(&heap).as_ptr());
                // SAFETY: published, so written; loom checks the read is
                // ordered after the write.
                assert_eq!(heap.made.with(|m| unsafe { *m }), 7);
            }
            publisher.join().unwrap();
        });
    }

    #[test]
    fn two_cores_making_the_same_nodes_are_both_found() {
        loom::model(|| {
            let map = Arc::new(Map::new());
            let (a, b) = (Arc::new(Heap { made: UnsafeCell::new(0) }), Arc::new(Heap { made: UnsafeCell::new(0) }));
            let other = {
                let (map, b) = (map.clone(), b.clone());
                thread::spawn(move || map.publish(CHUNK_BYTES, CHUNK_BYTES, named(&b)).unwrap())
            };
            map.publish(0, CHUNK_BYTES, named(&a)).unwrap();
            other.join().unwrap();
            assert_eq!(map.owner_of(0), named(&a).as_ptr());
            assert_eq!(map.owner_of(CHUNK_BYTES), named(&b).as_ptr());
        });
    }

    #[test]
    fn two_cores_publishing_one_chunk_one_has_it_and_the_other_maps_nothing() {
        loom::model(|| {
            let map = Arc::new(Map::new());
            let (a, b) = (Arc::new(Heap { made: UnsafeCell::new(0) }), Arc::new(Heap { made: UnsafeCell::new(0) }));
            let other = {
                let (map, b) = (map.clone(), b.clone());
                thread::spawn(move || map.publish(CHUNK_BYTES, 2 * CHUNK_BYTES, named(&b)).is_ok())
            };
            let a_won = map.publish(0, 2 * CHUNK_BYTES, named(&a)).is_ok();
            let b_won = other.join().unwrap();
            assert!(a_won != b_won, "exactly one has the shared chunk");
            let winner = if a_won { named(&a) } else { named(&b) };
            assert_eq!(map.owner_of(CHUNK_BYTES), winner.as_ptr());
            let (mine, theirs) = if a_won { (0, 2) } else { (2, 0) };
            assert_eq!(map.owner_of(mine * CHUNK_BYTES), winner.as_ptr());
            assert!(map.owner_of(theirs * CHUNK_BYTES).is_null(), "the loser left a chunk mapped");
        });
    }

    #[test]
    fn a_lookup_during_a_clear_finds_the_owner_or_nothing() {
        loom::model(|| {
            let map = Arc::new(Map::new());
            let heap = Arc::new(Heap { made: UnsafeCell::new(0) });
            map.publish(0, 2 * CHUNK_BYTES, named(&heap)).unwrap();
            let closer = {
                let (map, heap) = (map.clone(), heap.clone());
                thread::spawn(move || map.clear(0, 2 * CHUNK_BYTES, named(&heap)))
            };
            let found = map.owner_of(CHUNK_BYTES);
            assert!(found.is_null() || found == named(&heap).as_ptr());
            closer.join().unwrap();
            assert!(map.owner_of(CHUNK_BYTES).is_null());
        });
    }
}
