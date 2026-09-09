// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The `midi:<port>` follower-timeline registry.
//!
//! A fixed K-slot table of timelines that follow a MIDI clock arriving on a
//! port. The MIDI subsystem feeds one 0xF8 pulse or one transport message at
//! a time; readers take a [`Timeline`] snapshot. Slot assignment, LRU
//! eviction, primary selection and staleness all live here.
//!
//! The estimator: THE BEAT IS THE PULSE COUNT. Each 0xF8 is exactly 1/24 of a
//! beat of position, pinned to the pulse's timestamp; the smoothed tempo only
//! extrapolates between pulses and is never integrated into the beat, so
//! arrival jitter cannot accumulate into drift. Tempo is the mean of a window
//! of inter-pulse intervals, with an adaptive outlier test that tells a real
//! tempo step (several same-side outliers in a row: reset the window) from a
//! dropout or a bounce (a lone outlier: ignore it).
//!
//! Time: pulses arrive stamped by an OS clock in microseconds; everything the
//! registry stores is NTP seconds. The offset between the two is captured
//! per slot on the first pulse after a reset (`now - ts`), so intervals are
//! exact OS-clock differences and only the anchor is placed on the NTP axis.
//!
//! Every `now` comes from the caller. Nothing here reads a clock.

use crate::timeline::{is_valid_meter, Timeline};

/// Standard MIDI clock: 24 pulses per quarter note.
pub const PPQN: f64 = 24.0;
/// A slot whose port has sent nothing for this long is stale: the clock is
/// gone, the timeline free-runs at its last tempo until it returns.
pub const STALE_AFTER: f64 = 1.5;

const DEFAULT_PERIOD: f64 = 60.0 / (Timeline::FALLBACK_BPM * PPQN);
/// Tempo window: ~2 beats of intervals.
const WINDOW: usize = 48;
/// Step threshold, in window standard deviations.
const OUTLIER_SD: f64 = 3.0;
/// Samples before the window is trusted to judge outliers.
const MIN_FOR_OUTLIER: usize = 10;
/// Same-side outliers in a row that confirm a tempo step.
const STEP_CONFIRM: u32 = 3;
/// Plausibility band, seconds per pulse: outside 10..400 BPM an interval is a
/// delivery artefact (a queue flush, a stall), never a tempo observation.
const MIN_INTERVAL: f64 = 60.0 / (400.0 * PPQN);
const MAX_INTERVAL: f64 = 60.0 / (10.0 * PPQN);
/// A pulse arriving this soon after the last, relative to the period, is part
/// of a bunched flush: real position, meaningless timing.
const BUNCHED_FRACTION: f64 = 0.25;
/// A tempo change smaller than this is not worth a `/clockwork/clock/timelines`
/// push.
const NOTIFY_BPM_DELTA: f64 = 1.0;

/// Whether a `/clockwork/clock/<segment>/…` path segment names a timeline:
/// `link`, bare `midi` (the primary slot) or `midi:<port>` with a non-empty
/// port. The grammar [`Registry::resolve`] answers for, so the OSC router
/// and the resolver cannot disagree about what is a timeline and what is a
/// verb — `tempo`, `rpc`, `midi:` with nothing after it are verbs (or
/// refusals), not timelines.
pub fn is_timeline_name(name: &str) -> bool {
    name == "link" || name == "midi" || name.strip_prefix("midi:").is_some_and(|p| !p.is_empty())
}

/// A MIDI transport message, as the subsystem reports it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Transport {
    /// Reset the pulse counter; the next 0xF8 is beat 0.
    Start = 0,
    /// Run from where the counter is.
    Continue = 1,
    Stop = 2,
    /// Song Position: the next 0xF8 is at `beat`.
    Position = 3,
}

impl Transport {
    pub fn from_i32(kind: i32) -> Option<Self> {
        match kind {
            0 => Some(Self::Start),
            1 => Some(Self::Continue),
            2 => Some(Self::Stop),
            3 => Some(Self::Position),
            _ => None,
        }
    }
}

/// One row of the `/clockwork/clock/timelines` listing.
#[derive(Clone, Debug, PartialEq)]
pub struct Info {
    pub id: i32,
    /// Wire identity: `midi:<handle>`.
    pub name: String,
    /// The OS device name, for display.
    pub raw: String,
    pub bpm: f64,
    /// A clock is arriving (not stale).
    pub clocking: bool,
    pub stale: bool,
    pub primary: bool,
}

/// One follower timeline.
#[derive(Clone, Debug)]
struct Slot {
    active: bool,
    stale: bool,
    /// OSC-safe handle: match key and address segment.
    normalized: String,
    /// The OS device name.
    raw: String,

    /// 0xF8s since the last transport reset.
    pulse_count: i64,
    /// The beat at `pulse_count == 0` (Start: 0, Song Position: the position).
    base_beat: f64,
    /// OS pulse clock -> NTP, captured once per reset.
    ts_to_ntp: f64,
    ts_origin_set: bool,

    /// The tempo window, as a ring plus running sums.
    iv: [f64; WINDOW],
    iv_count: usize,
    iv_head: usize,
    iv_sum: f64,
    iv_sum_sq: f64,
    /// Smoothed seconds per pulse: the window mean.
    period: f64,
    /// NTP time of the latest pulse: the phase anchor.
    last_ts: f64,
    /// 0 = none, 1 = have an anchor, >= 2 = locked.
    pulses: u32,
    outlier_run: u32,
    outlier_sign: i32,

    playing: bool,
    /// A Start or Song Position has defined where beat 0 is.
    anchored: bool,
    transition_ntp: f64,
    /// Set by the meter verb; no MIDI message carries one. 4/4 until then.
    meter_num: i32,
    meter_den: i32,
    /// The last pulse or transport feed.
    last_fed: f64,
    went_stale: f64,
    /// The tempo at the last timelines push.
    last_notified_bpm: f64,
}

impl Default for Slot {
    fn default() -> Self {
        Self {
            active: false,
            stale: false,
            normalized: String::new(),
            raw: String::new(),
            pulse_count: 0,
            base_beat: 0.0,
            ts_to_ntp: 0.0,
            ts_origin_set: false,
            iv: [0.0; WINDOW],
            iv_count: 0,
            iv_head: 0,
            iv_sum: 0.0,
            iv_sum_sq: 0.0,
            period: DEFAULT_PERIOD,
            last_ts: 0.0,
            pulses: 0,
            outlier_run: 0,
            outlier_sign: 0,
            playing: false,
            anchored: false,
            transition_ntp: 0.0,
            meter_num: 4,
            meter_den: 4,
            last_fed: 0.0,
            went_stale: 0.0,
            last_notified_bpm: 0.0,
        }
    }
}

impl Slot {
    fn bpm(&self) -> f64 {
        60.0 / (self.period * PPQN)
    }

    /// The beat at the latest pulse.
    fn cur_beat(&self) -> f64 {
        self.base_beat + self.pulse_count as f64 / PPQN
    }

    fn beat_at(&self, ntp: f64) -> f64 {
        self.cur_beat() + (ntp - self.last_ts) / (PPQN * self.period)
    }

    fn snapshot(&self, id: i32) -> Timeline {
        Timeline {
            bpm: self.bpm(),
            anchor_beat: self.cur_beat(),
            anchor_ntp: self.last_ts,
            transition_ntp: self.transition_ntp,
            id,
            playing: self.playing as i32,
            anchored: self.anchored as i32,
            meter_num: self.meter_num,
            meter_den: self.meter_den,
        }
    }

    /// Forget the pulse history; the next pulse re-anchors at `now`.
    fn reset_pulses(&mut self, now: f64) {
        self.pulse_count = 0;
        self.pulses = 0;
        self.outlier_run = 0;
        self.iv_clear();
        self.ts_origin_set = false;
        self.last_ts = now;
    }

    fn iv_clear(&mut self) {
        self.iv_count = 0;
        self.iv_head = 0;
        self.iv_sum = 0.0;
        self.iv_sum_sq = 0.0;
    }

    fn iv_add(&mut self, iv: f64) {
        if self.iv_count == WINDOW {
            let old = self.iv[self.iv_head];
            self.iv_sum -= old;
            self.iv_sum_sq -= old * old;
        } else {
            self.iv_count += 1;
        }
        self.iv[self.iv_head] = iv;
        self.iv_sum += iv;
        self.iv_sum_sq += iv * iv;
        self.iv_head = (self.iv_head + 1) % WINDOW;
        if self.iv_count > 0 {
            self.period = self.iv_sum / self.iv_count as f64;
        }
    }

    /// One inter-pulse interval into the tempo window.
    fn feed_interval(&mut self, iv: f64) {
        if !(MIN_INTERVAL..=MAX_INTERVAL).contains(&iv) {
            return;
        }
        let n = self.iv_count as f64;
        let mean = if self.iv_count > 0 {
            self.iv_sum / n
        } else {
            iv
        };
        let var = if self.iv_count > 1 {
            (self.iv_sum_sq / n - mean * mean).max(0.0)
        } else {
            0.0
        };
        // A clean clock still jitters ~0.5%: never judge against a tighter SD.
        let sd = var.sqrt().max(0.005 * mean);
        if self.iv_count >= MIN_FOR_OUTLIER && (iv - mean).abs() > OUTLIER_SD * sd {
            // A possible step — or a glitch. Same side STEP_CONFIRM times is a step.
            let sign = if iv > mean { 1 } else { -1 };
            if sign == self.outlier_sign {
                self.outlier_run += 1;
            } else {
                self.outlier_sign = sign;
                self.outlier_run = 1;
            }
            if self.outlier_run >= STEP_CONFIRM {
                self.iv_clear();
                self.iv_add(iv);
                self.outlier_run = 0;
            }
        } else {
            self.iv_add(iv);
            self.outlier_run = 0;
        }
    }

    /// A pulse or transport arrived: no longer stale. Returns whether that
    /// is a change worth telling clients about.
    fn fed(&mut self, now: f64) -> bool {
        self.last_fed = now;
        std::mem::replace(&mut self.stale, false)
    }

    /// Whether the tempo has moved enough since the last push to push again.
    fn tempo_moved(&mut self) -> bool {
        let bpm = self.bpm();
        if (bpm - self.last_notified_bpm).abs() >= NOTIFY_BPM_DELTA {
            self.last_notified_bpm = bpm;
            true
        } else {
            false
        }
    }
}

/// The K-slot registry. Ids are 1..=K; 0 is Link (not here) and -1 is "no
/// such timeline". Every mutator returns whether the listing changed —
/// added, removed, stale, primary, or a tempo worth pushing — so the caller
/// can broadcast `/clockwork/clock/timelines`.
#[derive(Debug)]
pub struct Registry {
    slots: Vec<Slot>,
    /// 1..=K, or -1 when nothing is claimed.
    primary: i32,
}

impl Registry {
    pub fn new(max_slots: usize) -> Self {
        Self {
            slots: vec![Slot::default(); max_slots],
            primary: -1,
        }
    }

    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    /// The bare `midi` timeline: the lowest live slot, else the lowest
    /// claimed one, else -1.
    pub fn primary(&self) -> i32 {
        self.primary
    }

    fn slot(&self, id: i32) -> Option<&Slot> {
        if id < 1 {
            return None;
        }
        self.slots.get(id as usize - 1).filter(|s| s.active)
    }

    fn slot_mut(&mut self, id: i32) -> Option<&mut Slot> {
        if id < 1 {
            return None;
        }
        self.slots.get_mut(id as usize - 1).filter(|s| s.active)
    }

    fn slot_for(&self, normalized: &str) -> i32 {
        self.slots
            .iter()
            .position(|s| s.active && s.normalized == normalized)
            .map_or(0, |i| i as i32 + 1)
    }

    fn recompute_primary(&mut self) {
        let mut first_active = -1;
        for (i, s) in self.slots.iter().enumerate() {
            if !s.active {
                continue;
            }
            let id = i as i32 + 1;
            if first_active < 0 {
                first_active = id;
            }
            if !s.stale {
                self.primary = id;
                return;
            }
        }
        self.primary = first_active;
    }

    /// Find or allocate the slot for a port. Idempotent. Returns
    /// `(id, changed)`: 1..=K, or -1 when every slot holds a live clock.
    /// A stale slot is reclaimed longest-stale first; a port returning to its
    /// own stale slot continues the beat from where the free-run reached and
    /// re-locks on its next pulse — `anchored` is kept, because the grid still
    /// traces to the original Start; a port that wants a new beat 0 sends one.
    pub fn claim(&mut self, normalized: &str, raw: &str, now: f64) -> (i32, bool) {
        if normalized.is_empty() {
            return (-1, false);
        }
        let mut id = self.slot_for(normalized);
        if id == 0 {
            let mut oldest = f64::INFINITY;
            for (i, s) in self.slots.iter().enumerate() {
                if !s.active {
                    id = i as i32 + 1;
                    break;
                }
                if s.stale && s.went_stale < oldest {
                    oldest = s.went_stale;
                    id = i as i32 + 1;
                }
            }
            if id == 0 {
                return (-1, false);
            }
            self.slots[id as usize - 1] = Slot {
                active: true,
                normalized: normalized.to_owned(),
                raw: if raw.is_empty() {
                    normalized.to_owned()
                } else {
                    raw.to_owned()
                },
                last_ts: now,
                last_fed: now,
                ..Slot::default()
            };
            self.recompute_primary();
            return (id, true);
        }
        let s = &mut self.slots[id as usize - 1];
        if !s.stale {
            return (id, false);
        }
        s.base_beat = s.beat_at(now);
        s.reset_pulses(now);
        s.stale = false;
        s.last_fed = now;
        self.recompute_primary();
        (id, true)
    }

    /// Release a slot. Returns whether it was held.
    pub fn free(&mut self, id: i32) -> bool {
        if self.slot(id).is_none() {
            return false;
        }
        self.slots[id as usize - 1] = Slot::default();
        self.recompute_primary();
        true
    }

    /// A `/clockwork/clock/<tl>/` name to an id: "" and "link" are 0; "midi" is
    /// the primary slot; "midi:<port>" is that port's slot. -1 for an unclaimed
    /// port, no primary, or a malformed name. Never claims.
    pub fn resolve(&self, name: &str) -> i32 {
        if name.is_empty() || name == "link" {
            return 0;
        }
        if name == "midi" {
            return self.primary;
        }
        match name.strip_prefix("midi:") {
            Some(port) => match self.slot_for(port) {
                0 => -1,
                id => id,
            },
            None => -1,
        }
    }

    /// The write-path resolver: like [`resolve`](Self::resolve), but a
    /// "midi:<port>" name claims the slot if the port has not clocked yet.
    /// Bare "midi" cannot claim: it names no port.
    pub fn resolve_or_claim(&mut self, name: &str, now: f64) -> (i32, bool) {
        match name.strip_prefix("midi:") {
            Some(port) => self.claim(port, port, now),
            None => (self.resolve(name), false),
        }
    }

    /// One 0xF8 at OS time `ts_us`, received at `now`.
    pub fn pulse(&mut self, id: i32, ts_us: u64, now: f64) -> bool {
        let Some(s) = self.slot_mut(id) else {
            return false;
        };
        let ts = ts_us as f64 * 1e-6;
        if !s.ts_origin_set {
            s.ts_to_ntp = now - ts;
            s.ts_origin_set = true;
        }
        let ts = ts + s.ts_to_ntp;
        if s.pulses == 0 {
            // The first 0xF8 after Start/Continue IS the downbeat (or the
            // Song Position): anchor on it, don't count past it.
            s.last_ts = ts;
            s.pulses = 1;
        } else {
            let iv = ts - s.last_ts;
            let bunched = s.pulses >= 2 && iv < BUNCHED_FRACTION * s.period;
            if !bunched {
                s.feed_interval(iv);
                s.pulses = 2;
            }
            s.last_ts = ts;
            s.pulse_count += 1;
        }
        let unstaled = s.fed(now);
        let moved = s.tempo_moved();
        if unstaled {
            self.recompute_primary();
        }
        unstaled || moved
    }

    /// Manual tempo (the OSC set verb, or a placeholder nobody clocks). The
    /// beat stays continuous; a live clock's pulses take over from here.
    pub fn set_tempo(&mut self, id: i32, bpm: f64, now: f64) -> bool {
        let bpm = if bpm >= 1.0 { bpm } else { 1.0 }; // also rejects NaN
        let Some(s) = self.slot_mut(id) else {
            return false;
        };
        let beat_now = s.beat_at(now);
        let period = 60.0 / (bpm * PPQN);
        s.iv_clear();
        s.iv_add(period); // the window holds the manual tempo; pulses smooth from here
        s.base_beat = beat_now - s.pulse_count as f64 / PPQN;
        s.last_ts = now;
        s.pulses = 2;
        s.outlier_run = 0;
        let unstaled = s.fed(now);
        let moved = s.tempo_moved();
        if unstaled {
            self.recompute_primary();
        }
        unstaled || moved
    }

    /// The meter of slot `id`. The beat grid is untouched — bars are counted
    /// from beat 0 whatever the meter — so nothing re-anchors and no `now`
    /// is needed. False for a meter [`is_valid_meter`] refuses or a slot
    /// nothing holds; the listing does not carry the meter, so this never
    /// asks for a push.
    pub fn set_meter(&mut self, id: i32, num: i32, den: i32) -> bool {
        if !is_valid_meter(num, den) {
            return false;
        }
        let Some(s) = self.slot_mut(id) else {
            return false;
        };
        s.meter_num = num;
        s.meter_den = den;
        true
    }

    /// A transport message at `now`. `beat` is the Song Position (in beats)
    /// and is read only for [`Transport::Position`].
    pub fn transport(&mut self, id: i32, kind: Transport, beat: f64, now: f64) -> bool {
        let Some(s) = self.slot_mut(id) else {
            return false;
        };
        match kind {
            Transport::Start => {
                s.base_beat = 0.0;
                s.reset_pulses(now);
                s.playing = true;
                s.transition_ntp = now;
                s.anchored = true;
            }
            Transport::Position => {
                s.base_beat = beat;
                s.reset_pulses(now);
                s.anchored = true;
            }
            Transport::Continue => {
                s.playing = true;
                s.transition_ntp = now;
            }
            Transport::Stop => {
                s.playing = false;
                s.transition_ntp = now;
            }
        }
        let unstaled = s.fed(now);
        if unstaled {
            self.recompute_primary();
        }
        unstaled
    }

    /// The staleness sweep. Stale slots are not freed: they free-run at their
    /// last tempo, so a client following one holds tempo until the clock
    /// returns, and are reclaimed only when a new port needs a slot.
    pub fn tick_stale(&mut self, now: f64) -> bool {
        let mut changed = false;
        for s in &mut self.slots {
            if s.active && !s.stale && now - s.last_fed > STALE_AFTER {
                s.stale = true;
                s.went_stale = now;
                changed = true;
            }
        }
        if changed {
            self.recompute_primary();
        }
        changed
    }

    /// The snapshot of slot `id`, or None if nothing holds it.
    pub fn timeline(&self, id: i32) -> Option<Timeline> {
        self.slot(id).map(|s| s.snapshot(id))
    }

    /// The listing rows, claimed slots only, in slot order.
    pub fn info(&self) -> Vec<Info> {
        self.slots
            .iter()
            .enumerate()
            .filter(|(_, s)| s.active)
            .map(|(i, s)| Info {
                id: i as i32 + 1,
                name: format!("midi:{}", s.normalized),
                raw: s.raw.clone(),
                bpm: s.bpm(),
                clocking: !s.stale,
                stale: s.stale,
                primary: self.primary == i as i32 + 1,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: f64 = 3_900_000_000.0; // some NTP instant
    const K: usize = 2;

    fn reg() -> Registry {
        Registry::new(K)
    }

    /// Feed `n` pulses at `bpm` starting at OS time `ts_us`; returns the OS time
    /// of the next pulse. `now` tracks the OS clock exactly (offset `NOW`).
    fn pulses(r: &mut Registry, id: i32, bpm: f64, n: usize, ts_us: &mut f64) {
        let period = 60.0e6 / (bpm * PPQN);
        for _ in 0..n {
            r.pulse(id, *ts_us as u64, NOW + *ts_us * 1e-6);
            *ts_us += period;
        }
    }

    // ── slots ───────────────────────────────────────────────────────────

    #[test]
    fn claim_hands_out_slots_and_is_idempotent() {
        let mut r = reg();
        assert_eq!(r.claim("a", "A", NOW), (1, true));
        assert_eq!(r.claim("a", "A", NOW), (1, false));
        assert_eq!(r.claim("b", "B", NOW), (2, true));
        assert_eq!(r.claim("c", "C", NOW), (-1, false)); // full of live clocks
        let info = r.info();
        assert_eq!(info.len(), 2);
        assert_eq!(info[0].name, "midi:a");
        assert_eq!(info[0].raw, "A");
        assert!(info[0].primary);
        assert!(!info[1].primary);
    }

    #[test]
    fn raw_name_falls_back_to_the_handle() {
        let mut r = reg();
        r.claim("iac-1", "", NOW);
        assert_eq!(r.info()[0].raw, "iac-1");
    }

    #[test]
    fn a_full_registry_evicts_the_longest_stale_slot() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.claim("b", "B", NOW);
        // b goes stale first, then a much later.
        assert!(r.tick_stale(NOW + 2.0)); // both fed at NOW → both stale
        assert!(r.pulse(1, 0, NOW + 2.0)); // a returns: fed again, unstale
        assert!(r.tick_stale(NOW + 4.0)); // a stale again, later than b
        let (id, changed) = r.claim("c", "C", NOW + 5.0);
        assert_eq!((id, changed), (2, true)); // b (stale longest) was evicted
        assert_eq!(r.resolve("midi:b"), -1);
        assert_eq!(r.resolve("midi:a"), 1);
    }

    #[test]
    fn free_releases_the_slot_and_moves_primary() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.claim("b", "B", NOW);
        assert!(r.free(1));
        assert!(!r.free(1));
        assert_eq!(r.primary(), 2);
        assert_eq!(r.resolve("midi"), 2);
        assert!(r.timeline(1).is_none());
    }

    // ── names ───────────────────────────────────────────────────────────

    #[test]
    fn resolve_grammar() {
        let mut r = reg();
        assert_eq!(r.resolve(""), 0);
        assert_eq!(r.resolve("link"), 0);
        assert_eq!(r.resolve("midi"), -1); // no primary yet
        assert_eq!(r.resolve("midi:x"), -1); // unclaimed port
        assert_eq!(r.resolve("bogus"), -1);
        r.claim("x", "X", NOW);
        assert_eq!(r.resolve("midi"), 1);
        assert_eq!(r.resolve("midi:x"), 1);
    }

    #[test]
    fn a_timeline_name_is_what_resolve_has_a_grammar_for() {
        for ok in ["link", "midi", "midi:x", "midi:iac-driver-bus-1"] {
            assert!(is_timeline_name(ok), "{ok}");
        }
        for not in ["", "midi:", "tempo", "rpc", "links", "Link", "midi/x", "bogus", "meter"] {
            assert!(!is_timeline_name(not), "{not}");
        }
        // Every name the grammar admits, resolve has an answer for (an id or
        // -1 for a port nobody claimed) — never a malformed-name answer that
        // would differ from a verb's.
        let r = reg();
        assert_eq!(r.resolve("link"), 0);
        assert_eq!(r.resolve("midi:x"), -1);
    }

    #[test]
    fn resolve_or_claim_claims_only_a_named_port() {
        let mut r = reg();
        assert_eq!(r.resolve_or_claim("midi", NOW), (-1, false));
        assert_eq!(r.resolve_or_claim("midi:x", NOW), (1, true));
        assert_eq!(r.resolve_or_claim("midi:x", NOW), (1, false));
        assert_eq!(r.resolve_or_claim("link", NOW), (0, false));
    }

    // ── the estimator ───────────────────────────────────────────────────

    #[test]
    fn a_steady_clock_yields_its_tempo_and_the_beat_is_the_pulse_count() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 1_000_000.0;
        pulses(&mut r, 1, 120.0, 97, &mut ts); // first pulse = beat 0, then 96 more
        let t = r.timeline(1).unwrap();
        assert!((t.bpm - 120.0).abs() < 1e-3, "bpm {}", t.bpm);
        assert_eq!(t.anchor_beat, 4.0); // 96 / 24
        assert_eq!(t.playing, 1);
        assert_eq!(t.anchored, 1);
        // The anchor is the last pulse, placed on the NTP axis.
        let last = ts - 60.0e6 / (120.0 * PPQN);
        assert!((t.anchor_ntp - (NOW + last * 1e-6)).abs() < 2e-6); // µs-truncated stamp
                                                                    // ...and extrapolation runs at the tempo.
        assert!((t.beat_at(t.anchor_ntp + 0.5) - 5.0).abs() < 1e-5);
    }

    #[test]
    fn jitter_moves_the_tempo_estimate_but_never_the_beat() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let period = 60.0e6 / (120.0 * PPQN);
        let mut ts = 0.0;
        for i in 0..49 {
            let jitter = if i % 2 == 0 { 800.0 } else { -800.0 }; // ±0.8 ms
            r.pulse(1, (ts + jitter) as u64, NOW + (ts + jitter) * 1e-6);
            ts += period;
        }
        let t = r.timeline(1).unwrap();
        assert_eq!(t.anchor_beat, 2.0); // exactly 48/24
        assert!((t.bpm - 120.0).abs() < 0.5, "bpm {}", t.bpm);
    }

    #[test]
    fn a_burst_of_bunched_pulses_advances_the_beat_but_not_the_tempo() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 49, &mut ts);
        let before = r.timeline(1).unwrap().bpm;
        // Six ticks flushed 50 µs apart (a stalled driver catching up).
        for _ in 0..6 {
            r.pulse(1, ts as u64, NOW + ts * 1e-6);
            ts += 50.0;
        }
        let t = r.timeline(1).unwrap();
        assert_eq!(t.anchor_beat, 54.0 / 24.0);
        assert_eq!(t.bpm, before);
    }

    #[test]
    fn a_lone_dropout_does_not_change_the_tempo() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 30, &mut ts);
        let before = r.timeline(1).unwrap().bpm;
        ts += 60.0e6 / (120.0 * PPQN) * 3.0; // three periods of silence
        r.pulse(1, ts as u64, NOW + ts * 1e-6);
        assert_eq!(r.timeline(1).unwrap().bpm, before);
    }

    #[test]
    fn a_real_tempo_step_is_followed() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 48, &mut ts);
        pulses(&mut r, 1, 140.0, 8, &mut ts); // > STEP_CONFIRM same-side outliers
        let t = r.timeline(1).unwrap();
        assert!((t.bpm - 140.0).abs() < 1e-2, "bpm {}", t.bpm);
    }

    #[test]
    fn a_pulse_outside_the_plausible_band_never_seeds_the_window() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        r.pulse(1, 0, NOW);
        r.pulse(1, 5_000_000, NOW + 5.0); // 5 s gap: 0.5 BPM, an artefact
        assert!((r.timeline(1).unwrap().bpm - Timeline::FALLBACK_BPM).abs() < 1e-9);
    }

    #[test]
    fn the_first_pulse_after_start_is_the_downbeat() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 30, &mut ts);
        r.transport(1, Transport::Start, 0.0, NOW + ts * 1e-6);
        pulses(&mut r, 1, 120.0, 1, &mut ts);
        assert_eq!(r.timeline(1).unwrap().anchor_beat, 0.0);
        pulses(&mut r, 1, 120.0, 24, &mut ts);
        assert_eq!(r.timeline(1).unwrap().anchor_beat, 1.0);
    }

    // ── transport ───────────────────────────────────────────────────────

    #[test]
    fn transport_kinds() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        let t = r.timeline(1).unwrap();
        assert_eq!((t.playing, t.anchored, t.transition_ntp), (0, 0, 0.0));

        r.transport(1, Transport::Start, 0.0, NOW + 1.0);
        let t = r.timeline(1).unwrap();
        assert_eq!((t.playing, t.anchored), (1, 1));
        assert_eq!(t.transition_ntp, NOW + 1.0);

        r.transport(1, Transport::Stop, 0.0, NOW + 2.0);
        let t = r.timeline(1).unwrap();
        assert_eq!((t.playing, t.anchored), (0, 1));
        assert_eq!(t.transition_ntp, NOW + 2.0);

        r.transport(1, Transport::Position, 16.0, NOW + 3.0);
        let t = r.timeline(1).unwrap();
        assert_eq!(t.anchor_beat, 16.0);
        assert_eq!(t.playing, 0);

        r.transport(1, Transport::Continue, 0.0, NOW + 4.0);
        let t = r.timeline(1).unwrap();
        assert_eq!((t.playing, t.anchored), (1, 1));
        assert_eq!(t.transition_ntp, NOW + 4.0);
        assert_eq!(Transport::from_i32(3), Some(Transport::Position));
        assert_eq!(Transport::from_i32(4), None);
    }

    // ── manual tempo ────────────────────────────────────────────────────

    #[test]
    fn set_tempo_keeps_the_beat_continuous() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        let before = r.timeline(1).unwrap().beat_at(NOW + 10.0);
        assert!(r.set_tempo(1, 90.0, NOW + 10.0)); // changed: 60 → 90
        let t = r.timeline(1).unwrap();
        assert!((t.bpm - 90.0).abs() < 1e-9);
        assert!((t.beat_at(NOW + 10.0) - before).abs() < 1e-9);
        assert!((t.beat_at(NOW + 11.0) - (before + 1.5)).abs() < 1e-9);
        assert!(!r.set_tempo(1, 90.4, NOW + 12.0)); // < 1 BPM: not worth a push
        assert!(!r.set_tempo(9, 90.0, NOW)); // no such slot
    }

    #[test]
    fn set_tempo_clamps_nonsense() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.set_tempo(1, f64::NAN, NOW);
        assert!((r.timeline(1).unwrap().bpm - 1.0).abs() < 1e-9);
        r.set_tempo(1, 0.0, NOW);
        assert!((r.timeline(1).unwrap().bpm - 1.0).abs() < 1e-9);
    }

    // ── meter ───────────────────────────────────────────────────────────

    #[test]
    fn set_meter_changes_the_bar_grouping_and_nothing_else() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let before = r.timeline(1).unwrap();
        assert_eq!((before.meter_num, before.meter_den), (4, 4));

        assert!(r.set_meter(1, 7, 8));
        let t = r.timeline(1).unwrap();
        assert_eq!((t.meter_num, t.meter_den), (7, 8));
        assert_eq!(t.beats_per_bar(), 3.5);
        // The grid did not move: same anchor, same tempo, same transport.
        assert_eq!(t.anchor_beat, before.anchor_beat);
        assert_eq!(t.anchor_ntp, before.anchor_ntp);
        assert_eq!(t.bpm, before.bpm);
        assert_eq!((t.playing, t.anchored), (before.playing, before.anchored));

        // Refused: a nonsense meter, an unheld slot. The meter stands.
        assert!(!r.set_meter(1, 0, 4));
        assert!(!r.set_meter(1, 4, 3));
        assert!(!r.set_meter(2, 3, 4));
        let t = r.timeline(1).unwrap();
        assert_eq!((t.meter_num, t.meter_den), (7, 8));

        // A freed slot forgets it.
        r.free(1);
        r.claim("a", "A", NOW);
        let t = r.timeline(1).unwrap();
        assert_eq!((t.meter_num, t.meter_den), (4, 4));
    }

    // ── staleness ───────────────────────────────────────────────────────

    #[test]
    fn a_silent_clock_goes_stale_and_free_runs_at_its_last_tempo() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 49, &mut ts);
        let last_ntp = NOW + (ts - 60.0e6 / (120.0 * PPQN)) * 1e-6;
        assert!(!r.tick_stale(last_ntp + 1.0));
        assert!(r.tick_stale(last_ntp + 1.6));
        assert!(!r.tick_stale(last_ntp + 1.7)); // already reported
        let i = &r.info()[0];
        assert!(i.stale && !i.clocking);
        let t = r.timeline(1).unwrap();
        assert!((t.bpm - 120.0).abs() < 1e-3);
        assert!((t.beat_at(last_ntp + 10.0) - 22.0).abs() < 1e-3); // 2 + 10 s * 2 beats/s
                                                                   // A pulse brings it back (and reports the change).
        assert!(r.pulse(1, (ts + 10.0e6) as u64, last_ntp + 10.0));
        assert!(!r.info()[0].stale);
    }

    #[test]
    fn primary_prefers_a_live_clock_over_a_stale_one() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.claim("b", "B", NOW);
        assert_eq!(r.primary(), 1);
        r.pulse(2, 0, NOW + 1.4); // keep b fed
        assert!(r.tick_stale(NOW + 1.6)); // a stale, b live
        assert_eq!(r.primary(), 2);
        r.tick_stale(NOW + 5.0); // both stale → lowest active
        assert_eq!(r.primary(), 1);
    }

    #[test]
    fn a_returning_port_continues_the_free_run_beat() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.transport(1, Transport::Start, 0.0, NOW);
        let mut ts = 0.0;
        pulses(&mut r, 1, 120.0, 49, &mut ts);
        r.tick_stale(NOW + 100.0);
        let expected = r.timeline(1).unwrap().beat_at(NOW + 100.0);
        assert_eq!(r.claim("a", "A", NOW + 100.0), (1, true));
        let t = r.timeline(1).unwrap();
        assert!((t.anchor_beat - expected).abs() < 1e-9);
        assert_eq!(t.anchor_ntp, NOW + 100.0);
        assert_eq!(t.anchored, 1);
        assert!(!r.info()[0].stale);
    }

    #[test]
    fn info_rows_carry_the_wire_fields() {
        let mut r = reg();
        r.claim("a", "A", NOW);
        r.set_tempo(1, 100.0, NOW);
        let i = &r.info()[0];
        assert_eq!(i.id, 1);
        assert!((i.bpm - 100.0).abs() < 1e-9);
        assert!(i.clocking && !i.stale && i.primary);
    }
}
