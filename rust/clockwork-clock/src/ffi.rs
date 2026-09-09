// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The C ABI of `cpp/clockwork_clock.h`.
//!
//! [`Timeline`] crosses as `ClockworkTimeline` by value (`repr(C)`); the registry is
//! an opaque handle wrapping a mutex, so any thread may call any function on
//! it and the C++ side needs no lock of its own — except the audio thread,
//! which may not wait on a mutex at all. For it the handle keeps a published
//! copy of every slot's snapshot behind a seqlock, refreshed under the
//! registry lock by each mutator (so writers are serialised by the lock they
//! already hold), and `clockwork_midi_timelines_timeline_rt` reads one without
//! taking anything.
//!
//! # Panic freedom across the boundary
//!
//! Unwinding out of an `extern "C"` function is undefined behaviour, so none
//! of these may panic. Every handle and pointer is null-checked here; a byte
//! string that is not UTF-8 is treated as empty (an unclaimable name, the
//! same answer a genuinely empty name gets); a poisoned mutex is entered
//! anyway, since the registry holds no invariant a panic mid-update could
//! break that a reader would then trip over. And since Rust 1.81 a `panic!`
//! reaching an `extern "C"` frame aborts rather than unwinding into C++, so a
//! bug here is a crash with a message and not UB.

use std::os::raw::{c_char, c_void};
use std::sync::atomic::{fence, AtomicU32, Ordering};
use std::sync::Mutex;

use crate::midi::{is_timeline_name, Registry, Transport};
use crate::midi_clock_out;
use crate::timeline::{is_valid_meter, Timeline};

// ── The snapshot ────────────────────────────────────────────────────────────

/// A never-seen timeline, for a name nothing has claimed.
#[no_mangle]
pub extern "C" fn clockwork_timeline_placeholder(id: i32) -> Timeline {
    Timeline::placeholder(id)
}

/// The beat at NTP time `ntp`. 0 for a null timeline.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_beat_at(t: *const Timeline, ntp: f64) -> f64 {
    // SAFETY: the caller passes a valid pointer or null; null is checked.
    match unsafe { t.as_ref() } {
        Some(t) => t.beat_at(ntp),
        None => 0.0,
    }
}

/// The NTP time of `beat`. 0 for a null timeline.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_time_at_beat(t: *const Timeline, beat: f64) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => t.time_at_beat(beat),
        None => 0.0,
    }
}

/// The phase within `quantum` beats at NTP time `ntp`. 0 for a null timeline
/// or no quantum.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_phase_at(t: *const Timeline, ntp: f64, quantum: f64) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => t.phase_at(ntp, quantum),
        None => 0.0,
    }
}

/// Quarter-note beats per bar of the timeline's meter. 0 for a null timeline.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_beats_per_bar(t: *const Timeline) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => t.beats_per_bar(),
        None => 0.0,
    }
}

// ── MIDI clock out ──────────────────────────────────────────────────────────

/// The pulses a follower of `t` owes: from `next` (or
/// `CLOCKWORK_MIDI_CLOCK_OUT_UNSTARTED`) up to `now + horizon`, at most `max`.
/// Returns the first index and writes how many to `*count`; the follower
/// asks from `first + count` next time. See `midi_clock_out::due` for
/// `floor` and the re-sync rule. A null timeline owes nothing: `next`, 0.
///
/// # Safety
/// Pointers are null or valid for the call.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_clock_out_due(
    t: *const Timeline,
    next: i64,
    floor: f64,
    now: f64,
    horizon: f64,
    max: u32,
    count: *mut u32,
) -> i64 {
    // SAFETY: the caller passes valid pointers or null; null is checked.
    let d = match unsafe { t.as_ref() } {
        Some(t) => midi_clock_out::due(t, next, floor, now, horizon, max),
        None => midi_clock_out::Due {
            first: next,
            count: 0,
        },
    };
    // SAFETY: as above.
    if let Some(c) = unsafe { count.as_mut() } {
        *c = d.count;
    }
    d.first
}

/// The NTP time pulse `index` of a follower of `t` is due. 0 for null.
///
/// # Safety
/// Pointers are null or valid for the call.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_clock_out_pulse_ntp(t: *const Timeline, index: i64) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => midi_clock_out::pulse_ntp(t, index),
        None => 0.0,
    }
}

/// The bar at NTP time `ntp`, 0-based from beat 0. 0 for a null timeline.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_bar_at(t: *const Timeline, ntp: f64) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => t.bar_at(ntp),
        None => 0.0,
    }
}

/// Quarter-note beats into the bar at NTP time `ntp`. 0 for a null timeline.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_beat_in_bar_at(t: *const Timeline, ntp: f64) -> f64 {
    // SAFETY: as above.
    match unsafe { t.as_ref() } {
        Some(t) => t.beat_in_bar_at(ntp),
        None => 0.0,
    }
}

/// 1 if `num`/`den` is a meter a bar can be built from.
#[no_mangle]
pub extern "C" fn clockwork_timeline_meter_valid(num: i32, den: i32) -> i32 {
    is_valid_meter(num, den) as i32
}

/// 1 if the (pointer, length) segment names a timeline ("link", "midi",
/// "midi:<port>"): the rule the OSC router uses to tell a timeline segment
/// from a verb, kept beside the resolver that answers for those names.
///
/// # Safety
/// `name` is null or points to `name_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_timeline_is_name(name: *const c_char, name_len: u32) -> i32 {
    // SAFETY: (pointer, length) per the header's contract; null reads as "".
    is_timeline_name(unsafe { text(name, name_len) }) as i32
}

// ── The registry ────────────────────────────────────────────────────────────

// ── The published copies ────────────────────────────────────────────────────

/// A [`Timeline`] as 13 words: the f64s split into (low, high) halves, the
/// i32s as is. 32-bit atomics because every target has them (the ESP32 has no
/// 64-bit ones); the padding tail of the struct is not carried.
const WORDS: usize = 13;

fn encode(t: &Timeline) -> [u32; WORDS] {
    let mut w = [0u32; WORDS];
    for (i, f) in [t.bpm, t.anchor_beat, t.anchor_ntp, t.transition_ntp]
        .iter()
        .enumerate()
    {
        let bits = f.to_bits();
        w[2 * i] = bits as u32;
        w[2 * i + 1] = (bits >> 32) as u32;
    }
    w[8] = t.id as u32;
    w[9] = t.playing as u32;
    w[10] = t.anchored as u32;
    w[11] = t.meter_num as u32;
    w[12] = t.meter_den as u32;
    w
}

fn decode(w: &[u32; WORDS]) -> Timeline {
    let f = |i: usize| f64::from_bits(w[2 * i] as u64 | ((w[2 * i + 1] as u64) << 32));
    Timeline {
        bpm: f(0),
        anchor_beat: f(1),
        anchor_ntp: f(2),
        transition_ntp: f(3),
        id: w[8] as i32,
        playing: w[9] as i32,
        anchored: w[10] as i32,
        meter_num: w[11] as i32,
        meter_den: w[12] as i32,
    }
}

/// One slot's published snapshot: a seqlock over the words. The writer holds
/// the registry mutex, so there is one writer at a time; the reader is the
/// audio thread and never waits — a torn read is retried, and a slot the
/// writer is mid-way through after a bounded number of tries is reported as
/// unreadable rather than spun on.
struct Published {
    seq: AtomicU32,
    words: [AtomicU32; WORDS],
}

/// Retries before an RT read gives up on a slot being written.
const RT_READ_TRIES: usize = 16;

impl Published {
    fn new() -> Self {
        Self {
            seq: AtomicU32::new(0),
            words: std::array::from_fn(|_| AtomicU32::new(0)),
        }
    }

    /// Odd seq while writing, even when done; the fences order the words
    /// against the seq for a reader on another core.
    fn write(&self, t: &Timeline) {
        let s = self.seq.load(Ordering::Relaxed);
        self.seq.store(s.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        for (a, w) in self.words.iter().zip(encode(t)) {
            a.store(w, Ordering::Relaxed);
        }
        self.seq.store(s.wrapping_add(2), Ordering::Release);
    }

    fn read(&self) -> Option<Timeline> {
        for _ in 0..RT_READ_TRIES {
            let s1 = self.seq.load(Ordering::Acquire);
            if s1 & 1 != 0 {
                continue;
            }
            let mut w = [0u32; WORDS];
            for (dst, a) in w.iter_mut().zip(&self.words) {
                *dst = a.load(Ordering::Relaxed);
            }
            fence(Ordering::Acquire);
            if self.seq.load(Ordering::Relaxed) == s1 {
                return Some(decode(&w));
            }
        }
        None
    }
}

/// Opaque registry handle. C sees `ClockworkMidiTimelines*`.
pub struct ClockworkMidiTimelines {
    registry: Mutex<Registry>,
    /// Slot `id` is `published[id - 1]`; an unheld slot holds the id -1
    /// placeholder, so a reader can tell "nothing there" from a timeline.
    published: Vec<Published>,
}

impl ClockworkMidiTimelines {
    fn new(max_slots: usize) -> Self {
        let published: Vec<Published> = (0..max_slots).map(|_| Published::new()).collect();
        let registry = Registry::new(max_slots);
        for p in &published {
            p.write(&Timeline::placeholder(-1));
        }
        Self {
            registry: Mutex::new(registry),
            published,
        }
    }

    /// A read-only call on the registry.
    fn with<R>(h: *const Self, f: impl FnOnce(&mut Registry) -> R, or: R) -> R {
        // SAFETY: the caller passes a handle from clockwork_midi_timelines_new that
        // has not been freed, or null; null is checked.
        match unsafe { h.as_ref() } {
            Some(t) => f(&mut t.registry.lock().unwrap_or_else(|e| e.into_inner())),
            None => or,
        }
    }

    /// A mutating call: the published copies are refreshed before the lock
    /// is released, so the RT reader sees every mutation and writers never
    /// overlap. Every slot is rewritten — a claim can evict another slot and
    /// a stale sweep touches several, and K is small.
    fn with_mut<R>(h: *const Self, f: impl FnOnce(&mut Registry) -> R, or: R) -> R {
        // SAFETY: as above.
        match unsafe { h.as_ref() } {
            Some(t) => {
                let mut r = t.registry.lock().unwrap_or_else(|e| e.into_inner());
                let out = f(&mut r);
                for (i, p) in t.published.iter().enumerate() {
                    let id = i as i32 + 1;
                    p.write(&r.timeline(id).unwrap_or(Timeline::placeholder(-1)));
                }
                out
            }
            None => or,
        }
    }
}

/// One row of the listing, borrowed from the registry for the duration of
/// the callback that receives it: copy what you keep.
#[repr(C)]
pub struct ClockworkTimelineInfo {
    pub id: i32,
    pub name: *const c_char,
    pub name_len: u32,
    pub raw: *const c_char,
    pub raw_len: u32,
    pub bpm: f64,
    pub clocking: i32,
    pub stale: i32,
    pub primary: i32,
}

/// The callback `clockwork_midi_timelines_each` hands rows to.
pub type ClockworkTimelineInfoFn = extern "C" fn(ctx: *mut c_void, info: *const ClockworkTimelineInfo);

/// A (pointer, length) byte string as &str; empty when null or not UTF-8.
///
/// # Safety
/// `p` is null or points to `len` readable bytes.
unsafe fn text<'a>(p: *const c_char, len: u32) -> &'a str {
    if p.is_null() {
        return "";
    }
    // SAFETY: `len` readable bytes at `p`, per the contract.
    let bytes = unsafe { std::slice::from_raw_parts(p.cast::<u8>(), len as usize) };
    std::str::from_utf8(bytes).unwrap_or("")
}

/// A registry of `max_slots` follower timelines. Null if allocation failed.
#[no_mangle]
pub extern "C" fn clockwork_midi_timelines_new(max_slots: u32) -> *mut ClockworkMidiTimelines {
    Box::into_raw(Box::new(ClockworkMidiTimelines::new(max_slots as usize)))
}

/// Free a registry. Null is a no-op.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_free(h: *mut ClockworkMidiTimelines) {
    if !h.is_null() {
        // SAFETY: from clockwork_midi_timelines_new, freed once.
        drop(unsafe { Box::from_raw(h) });
    }
}

/// Find or allocate the slot for a port: 1..K, or -1 if every slot holds a
/// live clock. `*changed` (if non-null) is set to 1 when the listing changed.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_claim(
    h: *mut ClockworkMidiTimelines,
    normalized: *const c_char,
    normalized_len: u32,
    raw: *const c_char,
    raw_len: u32,
    now: f64,
    changed: *mut i32,
) -> i32 {
    // SAFETY: (pointer, length) pairs per the header's contract.
    let (norm, raw) = unsafe { (text(normalized, normalized_len), text(raw, raw_len)) };
    let (id, ch) = ClockworkMidiTimelines::with_mut(h, |r| r.claim(norm, raw, now), (-1, false));
    // SAFETY: a valid out-pointer or null, per the contract.
    if let Some(c) = unsafe { changed.as_mut() } {
        *c = ch as i32;
    }
    id
}

/// Release a slot. 1 if it was held (the listing changed).
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_release(h: *mut ClockworkMidiTimelines, id: i32) -> i32 {
    ClockworkMidiTimelines::with_mut(h, |r| r.free(id), false) as i32
}

/// A `/clockwork/clock/<tl>/` name to an id. Never claims.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_resolve(
    h: *const ClockworkMidiTimelines,
    name: *const c_char,
    name_len: u32,
) -> i32 {
    // SAFETY: as above.
    let name = unsafe { text(name, name_len) };
    ClockworkMidiTimelines::with(h, |r| r.resolve(name), -1)
}

/// The write-path resolver: a "midi:<port>" name claims its slot if unseen.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_resolve_or_claim(
    h: *mut ClockworkMidiTimelines,
    name: *const c_char,
    name_len: u32,
    now: f64,
    changed: *mut i32,
) -> i32 {
    // SAFETY: as above.
    let name = unsafe { text(name, name_len) };
    let (id, ch) = ClockworkMidiTimelines::with_mut(h, |r| r.resolve_or_claim(name, now), (-1, false));
    // SAFETY: a valid out-pointer or null, per the contract.
    if let Some(c) = unsafe { changed.as_mut() } {
        *c = ch as i32;
    }
    id
}

/// One 0xF8 at OS time `ts_us`, received at NTP `now`. 1 if the listing changed.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_pulse(
    h: *mut ClockworkMidiTimelines,
    id: i32,
    ts_us: u64,
    now: f64,
) -> i32 {
    ClockworkMidiTimelines::with_mut(h, |r| r.pulse(id, ts_us, now), false) as i32
}

/// Manual tempo. 1 if the listing changed.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_set_tempo(
    h: *mut ClockworkMidiTimelines,
    id: i32,
    bpm: f64,
    now: f64,
) -> i32 {
    ClockworkMidiTimelines::with_mut(h, |r| r.set_tempo(id, bpm, now), false) as i32
}

/// The meter of slot `id`. 1 if it was set; 0 for a meter
/// `clockwork_timeline_meter_valid` refuses or a slot nothing holds. The listing
/// does not carry the meter, so this never asks for a push.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_set_meter(
    h: *mut ClockworkMidiTimelines,
    id: i32,
    num: i32,
    den: i32,
) -> i32 {
    ClockworkMidiTimelines::with_mut(h, |r| r.set_meter(id, num, den), false) as i32
}

/// A transport message: kind 0 Start, 1 Continue, 2 Stop, 3 Song Position (at
/// `beat`). An unknown kind is ignored. 1 if the listing changed.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_transport(
    h: *mut ClockworkMidiTimelines,
    id: i32,
    kind: i32,
    beat: f64,
    now: f64,
) -> i32 {
    let Some(kind) = Transport::from_i32(kind) else {
        return 0;
    };
    ClockworkMidiTimelines::with_mut(h, |r| r.transport(id, kind, beat, now), false) as i32
}

/// The staleness sweep. 1 if the listing changed.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_tick_stale(h: *mut ClockworkMidiTimelines, now: f64) -> i32 {
    ClockworkMidiTimelines::with_mut(h, |r| r.tick_stale(now), false) as i32
}

/// The primary slot (bare "midi"), or -1.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_primary(h: *const ClockworkMidiTimelines) -> i32 {
    ClockworkMidiTimelines::with(h, |r| r.primary(), -1)
}

/// The snapshot of slot `id` into `*out`. 1 if the slot is held; 0 (and
/// `*out` untouched) otherwise.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_timeline(
    h: *const ClockworkMidiTimelines,
    id: i32,
    out: *mut Timeline,
) -> i32 {
    // SAFETY: the caller passes a valid out-pointer or null; null is checked.
    let Some(out) = (unsafe { out.as_mut() }) else {
        return 0;
    };
    match ClockworkMidiTimelines::with(h, |r| r.timeline(id), None) {
        Some(t) => {
            *out = t;
            1
        }
        None => 0,
    }
}

/// The published snapshot of slot `id` into `*out`, without taking the
/// registry lock: the audio thread's reader. 1 when read cleanly — a slot
/// nothing holds reads as the id -1 placeholder, so the caller can tell
/// "nothing there" and pass that on; 0 (and `*out` untouched) for an id out
/// of range or a slot a writer was mid-way through after RT_READ_TRIES
/// tries, when the caller keeps what it had.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_timeline_rt(
    h: *const ClockworkMidiTimelines,
    id: i32,
    out: *mut Timeline,
) -> i32 {
    // SAFETY: the caller passes a valid handle and out-pointer or null; both
    // are checked.
    let (Some(h), Some(out)) = (unsafe { h.as_ref() }, unsafe { out.as_mut() }) else {
        return 0;
    };
    if id < 1 {
        return 0;
    }
    match h.published.get(id as usize - 1).and_then(Published::read) {
        Some(t) => {
            *out = t;
            1
        }
        None => 0,
    }
}

/// Hand every held slot's listing row to `cb`, in slot order, under the
/// registry's lock: the callback must not call back into the registry.
///
/// # Safety
/// Pointers are null or valid for the call; a registry handle comes from
/// `clockwork_midi_timelines_new` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_timelines_each(
    h: *const ClockworkMidiTimelines,
    ctx: *mut c_void,
    cb: Option<ClockworkTimelineInfoFn>,
) {
    let Some(cb) = cb else { return };
    ClockworkMidiTimelines::with(
        h,
        |r| {
            for i in r.info() {
                let row = ClockworkTimelineInfo {
                    id: i.id,
                    name: i.name.as_ptr().cast::<c_char>(),
                    name_len: i.name.len() as u32,
                    raw: i.raw.as_ptr().cast::<c_char>(),
                    raw_len: i.raw.len() as u32,
                    bpm: i.bpm,
                    clocking: i.clocking as i32,
                    stale: i.stale as i32,
                    primary: i.primary as i32,
                };
                cb(ctx, &row);
            }
        },
        (),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_handle_round_trips_a_claim_and_a_snapshot() {
        // SAFETY: every pointer below is live for the call.
        unsafe {
            let h = clockwork_midi_timelines_new(2);
            let mut changed = 0;
            let name = b"midi:iac";
            let id = clockwork_midi_timelines_resolve_or_claim(
                h,
                name.as_ptr().cast::<c_char>(),
                name.len() as u32,
                100.0,
                &mut changed,
            );
            assert_eq!((id, changed), (1, 1));
            assert_eq!(clockwork_midi_timelines_set_tempo(h, 1, 120.0, 100.0), 1);
            assert_eq!(clockwork_midi_timelines_set_meter(h, 1, 7, 8), 1);
            assert_eq!(clockwork_midi_timelines_set_meter(h, 1, 7, 5), 0);
            let mut t = Timeline::placeholder(0);
            assert_eq!(clockwork_midi_timelines_timeline(h, 1, &mut t), 1);
            assert_eq!(t.id, 1);
            assert_eq!((t.meter_num, t.meter_den), (7, 8));
            assert!((clockwork_timeline_beat_at(&t, 101.0) - 2.0).abs() < 1e-9);
            assert_eq!(clockwork_timeline_beats_per_bar(&t), 3.5);
            // 120 BPM from beat 0 at 100: beat 20 at 110 is bar 5 (17.5..21).
            assert_eq!(clockwork_timeline_bar_at(&t, 110.0), 5.0);
            assert!((clockwork_timeline_beat_in_bar_at(&t, 110.0) - 2.5).abs() < 1e-9);
            assert_eq!(clockwork_timeline_meter_valid(3, 4), 1);
            assert_eq!(clockwork_timeline_meter_valid(3, 6), 0);
            let seg = b"midi:iac/tempo/get";
            assert_eq!(clockwork_timeline_is_name(seg.as_ptr().cast::<c_char>(), 8), 1);
            assert_eq!(clockwork_timeline_is_name(seg.as_ptr().cast::<c_char>(), 5), 0); // "midi:"
            assert_eq!(clockwork_timeline_is_name(seg.as_ptr().cast::<c_char>(), 4), 1); // "midi"
            assert_eq!(clockwork_timeline_is_name(std::ptr::null(), 0), 0);
            assert_eq!(clockwork_midi_timelines_timeline(h, 2, &mut t), 0);
            assert_eq!(clockwork_midi_timelines_primary(h), 1);

            extern "C" fn count(ctx: *mut c_void, info: *const ClockworkTimelineInfo) {
                // SAFETY: `ctx` is the `&mut u32` passed below, and `info` is
                // live for the callback.
                let (n, info) = unsafe { (&mut *ctx.cast::<u32>(), &*info) };
                assert_eq!(info.name_len, 8);
                *n += 1;
            }
            let mut n = 0u32;
            clockwork_midi_timelines_each(h, (&mut n as *mut u32).cast::<c_void>(), Some(count));
            assert_eq!(n, 1);
            clockwork_midi_timelines_free(h);
        }
    }

    #[test]
    fn the_rt_reader_sees_every_mutation_without_the_lock() {
        // SAFETY: every pointer below is live for the call.
        unsafe {
            let h = clockwork_midi_timelines_new(2);
            let mut t = Timeline::placeholder(0);
            // Nothing held: the placeholder, read cleanly. Out of range: nothing.
            assert_eq!(clockwork_midi_timelines_timeline_rt(h, 1, &mut t), 1);
            assert_eq!(t, Timeline::placeholder(-1));
            t = Timeline::placeholder(0);
            assert_eq!(clockwork_midi_timelines_timeline_rt(h, 0, &mut t), 0);
            assert_eq!(clockwork_midi_timelines_timeline_rt(h, 3, &mut t), 0);
            assert_eq!(t.id, 0);

            let name = b"midi:iac";
            let id = clockwork_midi_timelines_resolve_or_claim(
                h,
                name.as_ptr().cast::<c_char>(),
                name.len() as u32,
                100.0,
                std::ptr::null_mut(),
            );
            assert_eq!(id, 1);
            clockwork_midi_timelines_set_tempo(h, 1, 120.0, 100.0);
            clockwork_midi_timelines_set_meter(h, 1, 7, 8);
            clockwork_midi_timelines_transport(h, 1, 0, 0.0, 100.5);
            assert_eq!(clockwork_midi_timelines_timeline_rt(h, 1, &mut t), 1);
            // The same snapshot the locked reader gives.
            let mut locked = Timeline::placeholder(0);
            assert_eq!(clockwork_midi_timelines_timeline(h, 1, &mut locked), 1);
            assert_eq!(t, locked);
            assert_eq!(t.id, 1);
            assert!((t.bpm - 120.0).abs() < 1e-9);
            assert_eq!((t.meter_num, t.meter_den), (7, 8));
            assert_eq!((t.playing, t.anchored), (1, 1));
            assert_eq!(t.transition_ntp, 100.5);

            // Releasing the slot is a mutation too: the placeholder is back.
            assert_eq!(clockwork_midi_timelines_release(h, 1), 1);
            assert_eq!(clockwork_midi_timelines_timeline_rt(h, 1, &mut t), 1);
            assert_eq!(t.id, -1);
            clockwork_midi_timelines_free(h);
        }
    }

    #[test]
    fn the_words_carry_a_timeline_exactly() {
        let t = Timeline {
            bpm: 133.7,
            anchor_beat: -2.5,
            anchor_ntp: 3_900_000_000.125,
            transition_ntp: 0.0,
            id: 3,
            playing: 1,
            anchored: 0,
            meter_num: 13,
            meter_den: 16,
        };
        assert_eq!(decode(&encode(&t)), t);
        let p = Published::new();
        p.write(&t);
        assert_eq!(p.read(), Some(t));
        // A writer mid-way (odd seq) is not read.
        p.seq.store(p.seq.load(Ordering::Relaxed) + 1, Ordering::Relaxed);
        assert_eq!(p.read(), None);
    }

    #[test]
    fn clock_out_answers_across_the_boundary() {
        let t = Timeline {
            bpm: 120.0,
            anchor_beat: 0.0,
            anchor_ntp: 1000.0,
            ..Timeline::placeholder(0)
        };
        let mut count = 0u32;
        // SAFETY: every pointer is live for the call.
        let first = unsafe {
            clockwork_midi_clock_out_due(
                &t,
                midi_clock_out::UNSTARTED,
                0.0,
                1000.0,
                0.1,
                64,
                &mut count,
            )
        };
        assert_eq!((first, count), (0, 5));
        // SAFETY: `t` is live for the call.
        let pulse_1 = unsafe { clockwork_midi_clock_out_pulse_ntp(&t, 1) };
        assert!((pulse_1 - 1000.0 - 1.0 / 48.0).abs() < 1e-9);
    }

    #[test]
    fn nulls_are_answered_not_dereferenced() {
        // SAFETY: null is the documented no-op / "none" input.
        unsafe {
            assert_eq!(clockwork_timeline_beat_at(std::ptr::null(), 1.0), 0.0);
            assert_eq!(
                clockwork_midi_timelines_resolve(std::ptr::null(), std::ptr::null(), 0),
                -1
            );
            assert_eq!(clockwork_midi_timelines_pulse(std::ptr::null_mut(), 1, 0, 0.0), 0);
            let mut t = Timeline::placeholder(0);
            assert_eq!(clockwork_midi_timelines_timeline_rt(std::ptr::null(), 1, &mut t), 0);
            let mut count = 7u32;
            assert_eq!(
                clockwork_midi_clock_out_due(std::ptr::null(), 42, 0.0, 0.0, 0.1, 8, &mut count),
                42
            );
            assert_eq!(count, 0);
            assert_eq!(clockwork_midi_clock_out_pulse_ntp(std::ptr::null(), 1), 0.0);
            clockwork_midi_timelines_free(std::ptr::null_mut());
        }
    }
}
