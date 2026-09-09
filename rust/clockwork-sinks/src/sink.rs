// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! What sits in a slot: a queue, four counters, and the drain that puts
//! messages back into time order.
//!
//! # How order is kept without the audio thread paying for it
//!
//! `src/clockwork_event_sink.h` asks for two things that pull against each other:
//! `clockwork_sink_send` must never allocate, lock or block, and messages must be
//! delivered in time order. A sorted structure on the send path would be a
//! lock or a heap operation on the audio thread, and a DSP is entitled to
//! neither.
//!
//! So the send path does no ordering at all. It appends to a queue in ARRIVAL
//! order — one compare-exchange and a `memcpy` — and the ordering happens on
//! the drain, off the audio thread, where a binary heap costs nobody anything:
//!
//! 1. Take everything every queue has, into a heap keyed on `(when, arrival)`.
//! 2. Deliver from the top while the top is due.
//!
//! Arrival is the tiebreak, so two messages due at the same instant leave in
//! the order they were sent — a note-off cannot overtake the note-on it ends.
//! There are several queues (one per size class, see [`crate::profile`]), so
//! the arrival stamp is the SINK's counter, not a queue position: two
//! messages that took cells of different widths still leave in send order.
//!
//! # What that guarantees, exactly
//!
//! **Messages present in the sink at the same drain leave in `when` order.**
//! That is the whole guarantee, and it is worth stating what it excludes: a
//! message that arrives AFTER an earlier-timed message has already gone out
//! cannot un-send it. Nothing can, short of holding every message for a
//! lookahead window and adding that window to every note's latency. Such a
//! message is due already, so it goes out at once and is counted `late` —
//! which is exactly what `late` is for, and why the header calls it "the one
//! worth watching".
//!
//! # `late`, defined so it can be zero
//!
//! The header says `late` is "messages the endpoint could not honour the time
//! of ... the message had already come due by the time it was drained". Read
//! at the moment of DELIVERY that would count everything, because a held
//! message is released a hair after its due time by construction. So it is
//! counted at the moment the drain FIRST SEES a message: if it was already
//! past due then, the sink never had the chance to honour it, and that is a
//! real fault with a real cause — the producer was late, or the drain was
//! starved. A message the sink held and released on time is not late, and a
//! working sink therefore reports zero.
//!
//! # The four counters, and how they add up
//!
//! * `sent` — handed to the endpoint, whatever the endpoint then did.
//! * `scheduled` — the subset of `sent` given to a platform that will deliver
//!   it later itself (Web MIDI's `send_at`). Never non-zero where no platform
//!   scheduler exists.
//! * `late` — the subset of `sent` that was already past due when first seen.
//! * `dropped` — never handed over: the queue was full, the message was longer
//!   than a cell, or the endpoint refused it.
//!
//! So `sent + dropped` is every message the sink accepted plus every one the
//! endpoint refused, and `scheduled` and `late` are disjoint views into `sent`
//! rather than additions to it.

use std::cmp::{Ordering as CmpOrdering, Reverse};
use std::collections::BinaryHeap;
use std::ffi::CString;
use std::sync::atomic::Ordering;
use portable_atomic::AtomicU64;
use std::sync::{Arc, Mutex};

use crate::endpoint::{Delivery, Endpoint};
use crate::profile::Layout;
use crate::queue::MsgQueue;
use crate::time::{is_due, is_past, is_within_lookahead};

/// Kinds, matching `ClockworkSinkKind` in `src/clockwork_event_sink.h`.
pub const KIND_MIDI: u32 = 1;
pub const KIND_OSC: u32 = 2;

/// `ClockworkSinkStats`, field for field. `#[repr(C)]` so the FFI writes it through
/// rather than transcribing it — a transcription is a place for the two to
/// drift.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Stats {
    pub sent: u64,
    pub dropped: u64,
    pub late: u64,
    pub scheduled: u64,
    /// Messages a flush cancelled before they went out. Distinct from
    /// `dropped`: a drop is the sink failing to carry a message, a
    /// cancellation is a caller deciding it should not go.
    pub cancelled: u64,
}

/// One message waiting for its moment, on the drain thread's own heap.
struct Queued {
    /// The timetag as sent, passed to the endpoint unchanged.
    when: i64,
    /// `(when as u64, arrival)`. Unsigned because a real timetag is negative
    /// as an `i64` — see `crate::time`.
    key: (u64, u64),
    /// Already past due when the drain first saw it.
    late: bool,
    bytes: Vec<u8>,
}

impl PartialEq for Queued {
    fn eq(&self, other: &Self) -> bool { self.key == other.key }
}
impl Eq for Queued {}
impl PartialOrd for Queued {
    fn partial_cmp(&self, other: &Self) -> Option<CmpOrdering> { Some(self.cmp(other)) }
}
impl Ord for Queued {
    fn cmp(&self, other: &Self) -> CmpOrdering { self.key.cmp(&other.key) }
}

pub struct Sink {
    pub kind: u32,
    /// Who calls [`Sink::pump`] — the crate's drain thread, or the caller.
    /// See [`crate::registry::Drive`].
    pub drive: crate::registry::Drive,
    /// The target as given at open. Kept alive because `clockwork_sink_target` hands
    /// out the pointer, which is therefore valid exactly as long as the sink.
    pub target: CString,
    /// The queues producers write, narrowest cell first: a message takes the
    /// first one it fits that has room. Each multi-producer, bounded,
    /// allocation-free.
    classes: Vec<MsgQueue>,
    /// The arrival stamp, shared by every class so the drain's tiebreak holds
    /// across them. One relaxed increment per send.
    arrival: AtomicU64,
    endpoint: Arc<dyn Endpoint>,
    /// Held messages, ordered. Touched ONLY by the drain — never by a
    /// producer, which is why a mutex here does not break the header's rule
    /// about the send path.
    pending: Mutex<BinaryHeap<Reverse<Queued>>>,
    sent: AtomicU64,
    dropped: AtomicU64,
    late: AtomicU64,
    scheduled: AtomicU64,
    cancelled: AtomicU64,
}

impl Sink {
    pub fn new(kind: u32, target: CString, layout: &Layout,
               endpoint: Arc<dyn Endpoint>, drive: crate::registry::Drive) -> Sink {
        let classes = layout.classes().iter()
            .map(|c| MsgQueue::new(c.cells, c.cell_bytes as usize))
            .collect();
        Sink {
            kind,
            drive,
            target,
            classes,
            arrival: AtomicU64::new(0),
            endpoint,
            pending: Mutex::new(BinaryHeap::new()),
            sent: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
            late: AtomicU64::new(0),
            scheduled: AtomicU64::new(0),
            cancelled: AtomicU64::new(0),
        }
    }

    /// Offer one message. Returns whether it was accepted.
    ///
    /// AUDIO THREAD CALLABLE: no allocation, no lock, no wait. A full queue
    /// refuses in a load and a compare, counts, and returns — the header's
    /// "an output that stalls the audio thread is worse than an output that
    /// misses a note and says so".
    ///
    /// A message takes the narrowest cell it fits; if that class is full it
    /// tries the next wider one rather than being refused while room exists.
    /// Wider than the widest, or every class it fits full, and it is refused
    /// whole and counted.
    #[inline]
    pub fn send(&self, bytes: &[u8], when: i64) -> bool {
        // Nothing to carry is refused and not a drop: the same answer the C
        // ABI gives, so the two surfaces agree.
        if bytes.is_empty() { return false; }
        // A DIRECT sink's send is its delivery: the endpoint is one that is
        // safe here and holds the message to its time itself, and there is
        // no thread behind this sink to queue for. The counts are the drain's
        // counts, kept here instead.
        if self.drive == crate::registry::Drive::Direct {
            return match self.endpoint.deliver(bytes, when) {
                Delivery::Sent => { self.sent.fetch_add(1, Ordering::Relaxed); true }
                Delivery::Scheduled => {
                    self.sent.fetch_add(1, Ordering::Relaxed);
                    self.scheduled.fetch_add(1, Ordering::Relaxed);
                    true
                }
                Delivery::Failed => { self.dropped.fetch_add(1, Ordering::Relaxed); false }
            };
        }
        let stamp = self.arrival.fetch_add(1, Ordering::Relaxed);
        for q in &self.classes {
            if bytes.len() <= q.cell_bytes() && q.push(bytes, when, stamp) {
                return true;
            }
        }
        self.dropped.fetch_add(1, Ordering::Relaxed);
        false
    }

    /// The widest message this sink can carry.
    pub fn max_message_bytes(&self) -> u32 {
        self.classes.last().map(|q| q.cell_bytes() as u32).unwrap_or(0)
    }

    /// Payload bytes reserved at open, every class together.
    pub fn bytes_reserved(&self) -> u64 {
        self.classes.iter().map(|q| q.bytes_reserved()).sum()
    }

    pub fn stats(&self) -> Stats {
        Stats {
            sent: self.sent.load(Ordering::Relaxed),
            dropped: self.dropped.load(Ordering::Relaxed),
            late: self.late.load(Ordering::Relaxed),
            scheduled: self.scheduled.load(Ordering::Relaxed),
            cancelled: self.cancelled.load(Ordering::Relaxed),
        }
    }

    /// Messages accepted but not yet delivered — in the queue or held for
    /// their time. Diagnostics, and what a test waits on.
    pub fn in_flight(&self) -> usize {
        let held = self.pending.lock().map(|p| p.len()).unwrap_or(0);
        self.classes.iter().map(|q| q.len()).sum::<usize>() + held
    }

    /// Drain: take everything queued, deliver what is due, hold the rest.
    ///
    /// Returns the timetag of the earliest message still being held, so a
    /// caller can sleep until then rather than poll. `None` means nothing is
    /// waiting on a clock.
    ///
    /// NOT the audio thread's. It allocates (one `Vec` per held message), it
    /// takes the pending lock, and the endpoint underneath it may do IO.
    pub fn pump(&self, now: u64) -> Option<u64> {
        let Ok(mut pending) = self.pending.lock() else { return None };

        // 1. Everything the producers left, from every class, into time order.
        for q in &self.classes {
            while q.pop_with(|when, stamp, bytes| {
                let w = when as u64;
                pending.push(Reverse(Queued {
                    when,
                    key: (w, stamp),
                    late: is_past(w, now),
                    bytes: bytes.to_vec(),
                }));
            }).is_some() {}
        }

        // 2. Everything due, in that order — plus, for an endpoint that
        //    schedules for itself, everything within the look-ahead window.
        //
        //    NOT the lot at once, which is what this did first. Handing a
        //    message to the kernel or the MIDI server is irreversible: it can
        //    no longer be cancelled, and a run-stop that cannot cancel a note
        //    an hour out is not a run-stop. Holding everything instead trades
        //    the platform's timer for our thread's wakeup.
        //
        //    The window buys both. Beyond it the message stays here and a
        //    flush can still reach it; inside it the platform does the fine
        //    timing. Only the last LOOKAHEAD is uncancellable.
        let schedules = self.endpoint.schedules();
        loop {
            match pending.peek() {
                Some(Reverse(top))
                    if is_due(top.key.0, now)
                        || (schedules && is_within_lookahead(top.key.0, now)) => {}
                Some(Reverse(top)) => return Some(top.key.0),
                None => return None,
            }
            let Reverse(msg) = pending.pop().expect("peeked");
            match self.endpoint.deliver(&msg.bytes, msg.when) {
                Delivery::Sent => {
                    self.sent.fetch_add(1, Ordering::Relaxed);
                    if msg.late { self.late.fetch_add(1, Ordering::Relaxed); }
                }
                Delivery::Scheduled => {
                    self.sent.fetch_add(1, Ordering::Relaxed);
                    self.scheduled.fetch_add(1, Ordering::Relaxed);
                }
                // It did not go out. That is a drop, by the same argument a
                // full queue is: the message is gone and the number says so.
                Delivery::Failed => { self.dropped.fetch_add(1, Ordering::Relaxed); }
            }
        }
    }
}

impl Sink {
    /// Cancel everything this sink is still holding. Returns how many went.
    ///
    /// ONLY what is still here. A message already handed to the platform —
    /// anything inside the look-ahead window — is gone and cannot be recalled;
    /// that is the cost of letting the kernel do the fine timing, and it is
    /// bounded by the window rather than unbounded.
    ///
    /// This cancels, and does nothing else. It emits no all-notes-off and
    /// decides nothing about what a caller meant: a note already sounding goes
    /// on sounding. Whoever defines what "stop" means composes this with
    /// whatever else that takes — clockwork does not know which channels or
    /// which ports are involved, and inventing MIDI traffic nobody asked for
    /// is not its job.
    pub fn flush(&self) -> u32 {
        let mut pending = match self.pending.lock() {
            Ok(p) => p,
            Err(poisoned) => poisoned.into_inner(),
        };
        // The producers' queue FIRST. A message sent but not yet pumped is in
        // the ring, not the heap, and clearing only the heap would let it
        // survive a flush and arrive afterwards — a cancelled note that plays
        // anyway, which is the exact failure a run-stop exists to prevent.
        let mut in_flight = 0u32;
        for q in &self.classes {
            while q.pop_with(|_when, _stamp, _bytes| { in_flight += 1; }).is_some() {}
        }

        let n = pending.len() as u32 + in_flight;
        pending.clear();
        if n > 0 {
            self.cancelled.fetch_add(n as u64, Ordering::Relaxed);
        }
        n
    }
}

impl Drop for Sink {
    fn drop(&mut self) {
        // Anything still held is discarded rather than flushed. Flushing would
        // send messages before their time, which is the one thing this whole
        // file exists to avoid — and closing a sink is a deliberate act, not a
        // surprise. The cost is real and worth naming: a note-off scheduled
        // ahead of a close does not go out, so a DSP that closes a sink with
        // notes in flight should send its own all-notes-off first.
        self.endpoint.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::endpoint::CaptureEndpoint;
    use crate::time;

    fn capture_sink(capacity: u32) -> (Sink, Arc<CaptureEndpoint>) {
        let ep = Arc::new(CaptureEndpoint::new());
        let sink = Sink::new(KIND_OSC, CString::new("capture").unwrap(),
                             &Layout::single(crate::queue::DEFAULT_CELL_BYTES as u32, capacity),
                             ep.clone() as Arc<dyn Endpoint>,
                             crate::registry::Drive::Manual);
        (sink, ep)
    }

    fn classed_sink(layout: &Layout) -> (Sink, Arc<CaptureEndpoint>) {
        let ep = Arc::new(CaptureEndpoint::new());
        let sink = Sink::new(KIND_OSC, CString::new("capture").unwrap(), layout,
                             ep.clone() as Arc<dyn Endpoint>,
                             crate::registry::Drive::Manual);
        (sink, ep)
    }

    #[test]
    fn a_message_of_every_width_takes_a_cell_its_own_size_and_arrives_whole() {
        let layout = crate::profile::default_for(KIND_OSC);
        let (sink, ep) = classed_sink(&layout);
        // One message per class, sized to fill its cell exactly, tagged by
        // its first byte so the capture identifies it.
        let widths = [1024usize, 8192, 16384, 32768, 65536];
        for (i, w) in widths.iter().enumerate() {
            let mut body = vec![i as u8; *w];
            body[*w - 1] = 0xEE;
            assert!(sink.send(&body, 1), "{w} bytes must be carried");
        }
        assert_eq!(sink.max_message_bytes(), 65536);
        sink.pump(time::now());
        let taken = ep.taken();
        assert_eq!(taken.len(), 5);
        for (i, (_, b)) in taken.iter().enumerate() {
            assert_eq!(b.len(), widths[i], "delivered whole, not truncated");
            assert_eq!(b[0], i as u8);
            assert_eq!(*b.last().unwrap(), 0xEE, "the last byte made it too");
        }
        assert_eq!(sink.stats().dropped, 0);
    }

    #[test]
    fn an_empty_message_is_refused_and_not_counted_as_a_drop() {
        let (sink, ep) = capture_sink(8);
        assert!(!sink.send(&[], 1));
        sink.pump(time::now());
        assert_eq!(ep.count(), 0);
        assert_eq!(sink.stats(), Stats::default(), "nothing was lost, so nothing is counted lost");
    }

    #[test]
    fn wider_than_the_widest_class_is_refused_whole_and_counted() {
        let layout = crate::profile::default_for(KIND_OSC);
        let (sink, ep) = classed_sink(&layout);
        assert!(!sink.send(&vec![0u8; 65537], 1));
        assert_eq!(sink.stats().dropped, 1);
        sink.pump(time::now());
        assert_eq!(ep.count(), 0, "nothing partial went out");
    }

    #[test]
    fn a_full_class_spills_into_the_next_wider_one_rather_than_dropping() {
        // Two 1-byte cells and two 64-byte ones: the third small message has
        // no small cell left, and room still exists, so it rides a wide cell.
        let layout = Layout::normalise(vec![
            crate::profile::Class { cell_bytes: 1, cells: 2 },
            crate::profile::Class { cell_bytes: 64, cells: 2 },
        ]).unwrap();
        let (sink, ep) = classed_sink(&layout);
        for i in 0..4u8 { assert!(sink.send(&[i], 1), "{i}: four cells in all"); }
        assert!(!sink.send(&[9], 1), "the fifth has nowhere to go");
        assert_eq!(sink.stats().dropped, 1);
        sink.pump(time::now());
        let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
        assert_eq!(got, vec![0, 1, 2, 3], "all four, in send order");
    }

    #[test]
    fn ties_across_classes_still_leave_in_the_order_they_were_sent() {
        // Same instant, alternating widths, so consecutive messages sit in
        // different queues. Without a sink-wide stamp the heap could not tell
        // which came first.
        let layout = crate::profile::default_for(KIND_OSC);
        let (sink, ep) = classed_sink(&layout);
        let when = (time::now() + 1000) as i64;
        for i in 0..8u8 {
            let width = if i % 2 == 0 { 8 } else { 4000 };   // base cell vs 8 KB class
            let mut body = vec![0u8; width];
            body[0] = i;
            assert!(sink.send(&body, when));
        }
        sink.pump(time::now() + 1_000_000);
        let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
        assert_eq!(got, (0..8u8).collect::<Vec<_>>());
    }

    #[test]
    fn a_sink_reserves_what_its_layout_says() {
        let (sink, _) = classed_sink(&crate::profile::default_for(KIND_OSC));
        assert_eq!(sink.bytes_reserved(), 256 * 1024);
        let (midi, _) = classed_sink(&crate::profile::default_for(crate::sink::KIND_MIDI));
        assert_eq!(midi.bytes_reserved(), 256 * 16 + 2 * 65536);
    }

    #[test]
    fn messages_leave_in_time_order_however_they_arrived() {
        let (sink, ep) = capture_sink(64);
        let base = time::now();
        // Sent backwards, plus one "immediately" in the middle, which must
        // come out first of all: 1 is the smallest timetag there is.
        let order = [5i64, 3, 4, 1, 2];
        for (i, step) in order.iter().enumerate() {
            let when = if *step == 1 { 1 } else { (base + (*step as u64) * 1000) as i64 };
            assert!(sink.send(&[i as u8], when));
        }
        // A `now` far past every one of them, so all are due at one pump.
        sink.pump(base + 1_000_000);
        let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
        // index 3 carried when=1; then steps 2,3,4,5 → indices 4,1,2,0.
        assert_eq!(got, vec![3, 4, 1, 2, 0]);
    }

    #[test]
    fn two_messages_at_the_same_instant_keep_the_order_they_were_sent() {
        // The tiebreak. Without the arrival stamp a heap may pop equal keys in
        // any order, and a note-off would be free to overtake its note-on.
        let (sink, ep) = capture_sink(64);
        let base = time::now();
        let when = (base + 1000) as i64;
        for i in 0..16u8 { assert!(sink.send(&[i], when)); }
        sink.pump(base + 1_000_000);
        let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
        assert_eq!(got, (0..16u8).collect::<Vec<_>>());
    }

    #[test]
    fn a_message_not_yet_due_is_held_and_not_sent_early() {
        let (sink, ep) = capture_sink(64);
        let base = time::now();
        let due = base + (1u64 << 32);      // one second out
        assert!(sink.send(b"later", due as i64));
        let next = sink.pump(base);
        assert_eq!(ep.count(), 0, "sending early is the one thing that is wrong");
        assert_eq!(next, Some(due), "the drain must say when to come back");
        assert_eq!(sink.in_flight(), 1);

        assert_eq!(sink.pump(due), None, "nothing held once it is delivered");
        assert_eq!(ep.count(), 1);
        assert_eq!(sink.stats(), Stats { sent: 1, dropped: 0, late: 0, scheduled: 0, cancelled: 0 },
                   "delivered at its time is not late");
    }

    #[test]
    fn a_message_already_past_due_when_it_is_first_seen_counts_late() {
        let (sink, ep) = capture_sink(64);
        let base = time::now();
        assert!(sink.send(b"overdue", (base - 5000) as i64));
        assert!(sink.send(b"now", 1), "an immediate message can never be late");
        sink.pump(base);
        assert_eq!(ep.count(), 2);
        assert_eq!(sink.stats(), Stats { sent: 2, dropped: 0, late: 1, scheduled: 0, cancelled: 0 });
    }

    #[test]
    fn an_endpoint_that_schedules_is_handed_the_future_and_counts_it() {
        // Within the look-ahead, an endpoint that schedules takes the message
        // WITH ITS TIME and the sink lets go of it.
        //
        // This once asserted the same for a message a whole second out, when
        // the handoff was all-or-nothing. It is bounded now: past the window a
        // message stays here, which is what keeps it cancellable. The clause
        // that changed is the distance, not the behaviour inside the window.
        let (sink, ep) = capture_sink(64);
        ep.set_schedules(true);
        let base = time::now();
        let due = base + time::LOOKAHEAD_UNITS / 2;
        assert!(sink.send(b"web", due as i64));
        assert_eq!(sink.pump(base), None, "inside the window, the platform takes it");
        assert_eq!(ep.count(), 1);
        assert_eq!(ep.taken()[0].0, due as i64, "the time goes with the message");
        assert_eq!(sink.stats(), Stats { sent: 1, dropped: 0, late: 0, scheduled: 1, cancelled: 0 });

        // And beyond it, the sink keeps hold.
        let distant = base + time::LOOKAHEAD_UNITS * 100;
        assert!(sink.send(b"later", distant as i64));
        assert_eq!(sink.pump(base), Some(distant),
                   "past the look-ahead it stays here, and stays cancellable");
        assert_eq!(ep.count(), 1, "not handed over");
    }

    #[test]
    fn a_full_sink_drops_and_counts_and_the_earlier_messages_survive() {
        let (sink, ep) = capture_sink(8);
        for i in 0..8u8 { assert!(sink.send(&[i], 1), "{i}"); }
        for i in 0..100u8 {
            assert!(!sink.send(&[i], 1), "a full sink must refuse, not grow");
        }
        assert_eq!(sink.stats().dropped, 100);
        sink.pump(time::now());
        let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
        assert_eq!(got, (0..8u8).collect::<Vec<_>>(),
                   "what was accepted is delivered; the overflow is what went");
        assert_eq!(sink.stats().sent, 8);
    }

    #[test]
    fn a_refused_delivery_is_a_drop_because_it_did_not_go_out() {
        let (sink, ep) = capture_sink(8);
        ep.set_failing(true);
        assert!(sink.send(b"x", 1));
        sink.pump(time::now());
        assert_eq!(sink.stats(), Stats { sent: 0, dropped: 1, late: 0, scheduled: 0, cancelled: 0 });
    }
}

#[cfg(test)]
mod flush_tests {
    use super::*;
    use crate::endpoint::CaptureEndpoint;
    use crate::time;
    use std::ffi::CString;

    fn capture_sink(capacity: u32) -> (Sink, Arc<CaptureEndpoint>) {
        let ep = Arc::new(CaptureEndpoint::new());
        let sink = Sink::new(KIND_OSC, CString::new("capture").unwrap(),
                             &Layout::single(crate::queue::DEFAULT_CELL_BYTES as u32, capacity),
                             ep.clone() as Arc<dyn Endpoint>,
                             crate::registry::Drive::Manual);
        (sink, ep)
    }

    /// Far enough out that no look-ahead reaches it.
    fn far(now: u64) -> i64 { (now + time::LOOKAHEAD_UNITS * 100) as i64 }

    #[test]
    fn a_flush_cancels_what_is_held_and_counts_it() {
        let (sink, ep) = capture_sink(16);
        let now = time::now();
        for i in 0..4 {
            assert!(sink.send(b"note", far(now) + i));
        }
        sink.pump(now);                       // nothing is due; all four are held
        assert_eq!(ep.taken().len(), 0);

        assert_eq!(sink.flush(), 4);
        assert_eq!(sink.stats().cancelled, 4);

        // And they are really gone: time passing does not resurrect them.
        sink.pump(u64::MAX);
        assert_eq!(ep.taken().len(), 0, "a cancelled message must not arrive later");
        assert_eq!(sink.stats().sent, 0);
    }

    #[test]
    fn a_flush_is_not_a_drop() {
        // Two different facts about a message that did not arrive: the sink
        // failed to carry it, or a caller decided it should not go. Counting
        // a cancellation as a drop would make a working run-stop look like a
        // malfunctioning sink.
        let (sink, _ep) = capture_sink(16);
        let now = time::now();
        assert!(sink.send(b"x", far(now)));
        sink.flush();
        let s = sink.stats();
        assert_eq!(s.cancelled, 1);
        assert_eq!(s.dropped, 0);
    }

    #[test]
    fn flushing_an_empty_sink_is_a_no_op() {
        let (sink, _ep) = capture_sink(16);
        assert_eq!(sink.flush(), 0);
        assert_eq!(sink.stats().cancelled, 0);
    }

    #[test]
    fn an_endpoint_that_schedules_is_handed_only_the_look_ahead() {
        // The whole point of the window. Before this, `schedules` handed over
        // EVERYTHING at once, which made a message an hour out as
        // uncancellable as one a millisecond out.
        let (sink, ep) = capture_sink(64);
        ep.set_schedules(true);
        let now = time::now();

        let near = (now + time::LOOKAHEAD_UNITS / 2) as i64;   // inside
        let distant = far(now);                                // outside
        assert!(sink.send(b"near", near));
        assert!(sink.send(b"far", distant));

        sink.pump(now);

        let taken = ep.taken();
        assert_eq!(taken.len(), 1, "only the message inside the window goes over");
        assert_eq!(taken[0].1, b"near");

        // The distant one is still ours, and still cancellable.
        assert_eq!(sink.flush(), 1);
        assert_eq!(sink.stats().cancelled, 1);
    }

    #[test]
    fn what_the_platform_already_has_cannot_be_cancelled() {
        // The honest limit, pinned so nobody expects otherwise. Inside the
        // window the message is with the kernel and a flush cannot reach it.
        let (sink, ep) = capture_sink(16);
        ep.set_schedules(true);
        let now = time::now();
        assert!(sink.send(b"gone", (now + time::LOOKAHEAD_UNITS / 2) as i64));

        sink.pump(now);
        assert_eq!(ep.taken().len(), 1, "handed over");

        assert_eq!(sink.flush(), 0, "nothing left here to cancel");
        assert_eq!(sink.stats().cancelled, 0);
        assert_eq!(sink.stats().scheduled, 1);
    }

    #[test]
    fn an_immediate_message_is_always_within_the_window() {
        // when <= 1 means now; there is nothing to hold it for.
        let now = time::now();
        assert!(time::is_within_lookahead(0, now));
        assert!(time::is_within_lookahead(time::IMMEDIATE, now));
        assert!(!time::is_within_lookahead(now + time::LOOKAHEAD_UNITS * 10, now));
    }
}
