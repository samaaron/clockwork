// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! `src/clockwork_event_sink.h` through the C ABI a caller actually has.
//!
//! The unit tests in `src/sink.rs` pin the drain's own arithmetic. What is
//! pinned HERE is everything a C caller can see: that a handle behaves, that a
//! closed one refuses instead of faulting, that a full sink drops rather than
//! waits, and that an OSC sink put bytes on a real socket.
//!
//! The table is process-global, exactly as it is in clockwork, and cargo
//! runs cases on threads of one process. So every case takes [`ONE_AT_A_TIME`]
//! first: `close_all` means ALL, and a case that swept the table while another
//! was waiting on a held message would close it out from under it. That is not
//! a flaw in the substrate — it is what "process-global" means — but it does
//! mean the tests must not pretend to be independent.

use std::ffi::{CStr, CString};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use clockwork_sinks::ffi::*;
use clockwork_sinks::registry::{self, Drive};
use clockwork_sinks::profile::Layout as SinkLayout;
use clockwork_sinks::queue::DEFAULT_CELL_BYTES;
use clockwork_sinks::sink::{Stats, KIND_MIDI, KIND_OSC};
use clockwork_sinks::CaptureEndpoint;

/// One case at a time. See the module note.
static ONE_AT_A_TIME: Mutex<()> = Mutex::new(());

/// The guard, taken by every case. Poison is ignored on purpose: one case
/// failing should report its own failure, not turn every later case into a
/// panic about a mutex.
fn serialise() -> std::sync::MutexGuard<'static, ()> {
    ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner())
}

/// A sink onto a capture endpoint, drained only when the test says so.
// One class of ordinary cells, `capacity` deep: the shape every sink had
// before profiles, which is what "full" and "wider than a cell" below mean.
fn manual(capacity: u32) -> (u32, Arc<CaptureEndpoint>) {
    let ep = Arc::new(CaptureEndpoint::new());
    let h = registry::open_with_layout(KIND_OSC, "capture:0",
                                       &SinkLayout::single(DEFAULT_CELL_BYTES as u32, capacity),
                                       ep.clone(), Drive::Manual);
    assert_ne!(h, 0, "the capture endpoint must always open");
    (h, ep)
}

fn stats_of(h: u32) -> Stats {
    registry::acquire(h).expect("an open sink has stats").stats()
}

/// Send through the Rust API: what `clockwork_sink_send` does once it has a
/// slice. The C edge — null bytes, a null out-pointer, a C string target —
/// is covered by the ABI cases in this file, which are the ones that keep
/// their `unsafe`.
fn send(h: u32, bytes: &[u8], when: i64) -> bool {
    registry::acquire(h).is_some_and(|s| s.send(bytes, when))
}

// ── Opening, closing, listing, describing ───────────────────────────────────

#[test]
fn a_sink_describes_itself_and_appears_in_the_list_until_it_is_closed() {
    let _g = serialise();
    registry::close_all();

    let mut slots = [0u32; 8];
    // SAFETY: `slots` has 8 writable entries.
    assert_eq!(unsafe { clockwork_sink_list(slots.as_mut_ptr(), 8) }, 0,
               "no sinks, no list");

    let (h, _ep) = manual(64);
    assert_eq!(clockwork_sink_is_open(h), 1);
    assert_eq!(clockwork_sink_kind(h), KIND_OSC as i32);
    // SAFETY: the target is NUL-terminated and lives while the sink is open.
    let target = unsafe { CStr::from_ptr(clockwork_sink_target(h)) };
    assert_eq!(target.to_str().unwrap(), "capture:0");
    assert_eq!(stats_of(h), Stats::default(), "a new sink has done nothing");

    // SAFETY: `slots` has 8 writable entries.
    let n = unsafe { clockwork_sink_list(slots.as_mut_ptr(), 8) };
    assert_eq!(n, 1);
    assert_eq!(slots[0], h);

    clockwork_sink_close(h);
    assert_eq!(clockwork_sink_is_open(h), 0);
    // SAFETY: `slots` has 8 writable entries.
    assert_eq!(unsafe { clockwork_sink_list(slots.as_mut_ptr(), 8) }, 0);
}

#[test]
fn list_reports_how_many_there_are_even_when_it_cannot_write_them_all() {
    let _g = serialise();
    registry::close_all();
    let sinks: Vec<_> = (0..5).map(|_| manual(8)).collect();

    let mut two = [0u32; 2];
    // The count is the truth about the table, not about the buffer — a caller
    // sizing an array needs to be told it was short.
    // SAFETY: `two` has 2 writable entries.
    assert_eq!(unsafe { clockwork_sink_list(two.as_mut_ptr(), 2) }, 5);
    // SAFETY: null with a zero cap is the documented count-only call.
    assert_eq!(unsafe { clockwork_sink_list(core::ptr::null_mut(), 0) }, 5);
    assert_eq!(two[0], sinks[0].0);

    for (h, _) in &sinks { clockwork_sink_close(*h); }
}

#[test]
fn a_kind_this_build_cannot_reach_refuses_rather_than_swallowing() {
    let _g = serialise();
    // Neither of these names anything: kind 7 is not a kind, and a MIDI sink
    // with no MIDI subsystem alive has nowhere to send. Both must answer
    // CLOCKWORK_SINK_NONE — a sink that opened and then dropped everything would be
    // indistinguishable from one that worked.
    let name = CString::new("nothing").unwrap();
    // SAFETY: a NUL-terminated CString.
    assert_eq!(unsafe { clockwork_sink_open(7, name.as_ptr(), 64) }, 0);
    // SAFETY: a NUL-terminated CString.
    assert_eq!(unsafe { clockwork_sink_open(KIND_MIDI as i32, name.as_ptr(), 64) }, 0,
               "no clockwork_midi_create has run in this process, so there is no port");
    // An OSC target that is not host:port is the same refusal.
    let bad = CString::new("not-a-destination").unwrap();
    // SAFETY: a NUL-terminated CString.
    assert_eq!(unsafe { clockwork_sink_open(KIND_OSC as i32, bad.as_ptr(), 64) }, 0);
    // SAFETY: null is the documented refusal.
    assert_eq!(unsafe { clockwork_sink_open(KIND_OSC as i32, core::ptr::null(), 64) }, 0);
}

// ── Sending and delivering ──────────────────────────────────────────────────

#[test]
fn a_message_sent_is_a_message_delivered() {
    let _g = serialise();
    let (h, ep) = manual(64);
    assert!(send(h, &[0x90, 60, 100], 1));
    assert_eq!(ep.count(), 0, "nothing goes out before a drain");
    registry::pump(h, clockwork_sinks::time::now());
    assert_eq!(ep.taken(), vec![(1i64, vec![0x90, 60, 100])]);
    assert_eq!(stats_of(h), Stats { sent: 1, dropped: 0, late: 0, scheduled: 0, cancelled: 0 });
    clockwork_sink_close(h);
}

#[test]
fn an_empty_message_is_refused_and_is_not_a_drop() {
    let _g = serialise();
    let (h, ep) = manual(64);
    // SAFETY: a null pointer with length 0 is the ABI's "no bytes"; nothing is read.
    assert_eq!(unsafe { clockwork_sink_send(h, core::ptr::null(), 0, 1) }, 0);
    assert!(!send(h, &[], 1));
    registry::pump(h, clockwork_sinks::time::now());
    assert_eq!(ep.count(), 0);
    // Nothing was lost, so nothing is counted lost.
    assert_eq!(stats_of(h), Stats::default());
    clockwork_sink_close(h);
}

#[test]
fn messages_leave_in_time_order_however_they_were_sent() {
    let _g = serialise();
    // The claim the header makes and the one a caller most needs: "must be
    // delivered in time order per sink". Sent deliberately backwards, with an
    // "immediately" in the middle, which is the earliest time there is.
    let (h, ep) = manual(64);
    let base = clockwork_sinks::time::now();
    let sends: [(u8, i64); 6] = [
        (0, (base + 5000) as i64),
        (1, (base + 3000) as i64),
        (2, (base + 4000) as i64),
        (3, 1),
        (4, (base + 1000) as i64),
        (5, (base + 2000) as i64),
    ];
    for (tag, when) in sends {
        assert!(send(h, &[tag], when));
    }
    registry::pump(h, base + 1_000_000);
    let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
    assert_eq!(got, vec![3, 4, 5, 1, 2, 0]);
    // And each carried its own time to the endpoint unchanged: the header is
    // explicit that the endpoint, not the substrate, honours `when`.
    let whens: Vec<i64> = ep.taken().iter().map(|(w, _)| *w).collect();
    assert_eq!(whens, vec![1, (base + 1000) as i64, (base + 2000) as i64,
                           (base + 3000) as i64, (base + 4000) as i64,
                           (base + 5000) as i64]);
    clockwork_sink_close(h);
}

#[test]
fn a_message_drained_after_its_time_is_counted_late() {
    let _g = serialise();
    let (h, ep) = manual(64);
    let base = clockwork_sinks::time::now();
    // One already overdue, one due later, one immediate. Only the first was
    // ever un-honourable: the sink never had the chance.
    assert!(send(h, b"overdue", (base - 100_000) as i64));
    assert!(send(h, b"soon", (base + 1000) as i64));
    assert!(send(h, b"now", 1));

    registry::pump(h, base);
    assert_eq!(ep.count(), 2, "the one due later is held, not sent early");
    assert_eq!(stats_of(h), Stats { sent: 2, dropped: 0, late: 1, scheduled: 0, cancelled: 0 });

    registry::pump(h, base + 1000);
    assert_eq!(ep.count(), 3);
    assert_eq!(stats_of(h).late, 1, "delivered at its time is not late");
    clockwork_sink_close(h);
}

// ── The full sink ───────────────────────────────────────────────────────────

#[test]
fn a_full_sink_drops_and_counts_and_does_not_block() {
    let _g = serialise();
    // The header's sharpest promise: "an output that stalls the audio thread
    // is worse than an output that misses a note and says so". Nothing drains
    // this sink, so after the first eight every send is into a full queue.
    const CAPACITY: u32 = 8;
    const OVERFLOW: usize = 100_000;
    let (h, ep) = manual(CAPACITY);

    for i in 0..CAPACITY {
        assert!(send(h, &[i as u8], 1), "{i}");
    }

    let body = [0x90u8, 60, 100];
    let mut worst = Duration::ZERO;
    let start = Instant::now();
    for _ in 0..OVERFLOW {
        let t = Instant::now();
        let accepted = send(h, &body, 1);
        worst = worst.max(t.elapsed());
        assert!(!accepted, "a full sink must refuse");
    }
    let total = start.elapsed();

    assert_eq!(stats_of(h).dropped, OVERFLOW as u64, "every refusal is counted");

    // Timing, stated as what "does not block" means. A refusal is a load, a
    // compare and a counter increment; on the slowest machine this is likely
    // to run on that is nanoseconds, so a millisecond is four orders of
    // margin, and anything that WAITED — a lock, a syscall, a condvar — would
    // be far over it. The total is bounded too, so a stall that only happened
    // occasionally could not hide inside a generous per-call bound.
    assert!(worst < Duration::from_millis(1),
            "one refused send took {worst:?} — that is a wait, not a refusal");
    assert!(total < Duration::from_secs(2),
            "{OVERFLOW} refused sends took {total:?}");

    // And the sink is undamaged: the eight it accepted are all still there.
    registry::pump(h, clockwork_sinks::time::now());
    let got: Vec<u8> = ep.taken().iter().map(|(_, b)| b[0]).collect();
    assert_eq!(got, (0..CAPACITY as u8).collect::<Vec<_>>());
    clockwork_sink_close(h);
}

// ── Handles that are not, or are no longer, sinks ───────────────────────────

#[test]
fn a_closed_or_stale_or_invented_handle_refuses_safely() {
    let _g = serialise();
    let (h, ep) = manual(64);
    assert!(send(h, b"before", 1));
    clockwork_sink_close(h);
    clockwork_sink_close(h);          // twice is safe

    for bad in [h, 0u32, 1u32, 0xdead_beef, 0xffff_ffff] {
        assert_eq!(clockwork_sink_is_open(bad), 0, "{bad:#x}");
        assert_eq!(clockwork_sink_kind(bad), 0, "{bad:#x}");
        assert!(clockwork_sink_target(bad).is_null(), "{bad:#x}");
        assert!(!send(bad, b"x", 1), "{bad:#x}");
        let mut s = Stats { sent: 7, dropped: 7, late: 7, scheduled: 7, cancelled: 0 };
        // SAFETY: a live out-pointer; the handle is meant to be bogus.
        assert_eq!(unsafe { clockwork_sink_stats(bad, &mut s) }, 0, "{bad:#x}");
        assert_eq!(s, Stats { sent: 7, dropped: 7, late: 7, scheduled: 7, cancelled: 0 },
                   "a refused stats call must not touch the caller's struct");
        assert!(registry::pump(bad, clockwork_sinks::time::now()).is_none());
    }
    // A NULL out pointer is a refusal, not a fault.
    // SAFETY: null is the documented refusal.
    assert_eq!(unsafe { clockwork_sink_stats(h, core::ptr::null_mut()) }, 0);
    // Nothing was delivered after the close, either.
    assert_eq!(ep.count(), 0);
}

#[test]
fn a_handle_that_outlives_its_sink_cannot_reach_the_next_occupant() {
    let _g = serialise();
    registry::close_all();
    // The generation's whole job, at the ABI. Without it a DSP holding a
    // handle across a close would send another destination's messages.
    let (first, first_ep) = manual(8);
    let idx = registry::table().slot_index(first);
    clockwork_sink_close(first);

    let mut second = 0;
    let mut second_ep = None;
    for _ in 0..=clockwork_sinks::MAX_SINKS {
        let (h, ep) = manual(8);
        if registry::table().slot_index(h) == idx { second = h; second_ep = Some(ep); break; }
    }
    assert_ne!(second, 0, "the freed slot never came back");
    assert_ne!(second, first, "a reopened slot must not reissue the old handle");

    assert!(send(second, b"mine", 1));
    assert!(!send(first, b"yours", 1),
               "a stale handle reached a live sink");
    registry::pump(second, clockwork_sinks::time::now());
    assert_eq!(second_ep.unwrap().taken(), vec![(1i64, b"mine".to_vec())]);
    assert_eq!(first_ep.count(), 0);
    registry::close_all();
}

// ── An endpoint that is really there ────────────────────────────────────────

#[test]
#[cfg(feature = "osc")]
fn an_osc_sink_puts_bytes_on_a_real_socket_without_being_pumped() {
    let _g = serialise();
    use std::net::UdpSocket;
    // End to end through the ABI and the service thread: nothing in this test
    // drains anything. The header has no drain call, so a sink that needed one
    // would be a sink that never delivered.
    let listener = UdpSocket::bind(("127.0.0.1", 0)).expect("a loopback port");
    listener.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let port = listener.local_addr().unwrap().port();

    let target = CString::new(format!("127.0.0.1:{port}")).unwrap();
    // SAFETY: a NUL-terminated CString.
    let h = unsafe { clockwork_sink_open(KIND_OSC as i32, target.as_ptr(), 64) };
    assert_ne!(h, 0, "a resolvable loopback destination must open");

    let body = b"/hello\0\0,i\0\0\0\0\0\x2a";
    assert!(send(h, body, 1));

    let mut buf = [0u8; 256];
    let n = listener.recv(&mut buf).expect("the drain thread delivered nothing");
    assert_eq!(&buf[..n], body, "the bytes arrived exactly as sent");
    assert_eq!(stats_of(h), Stats { sent: 1, dropped: 0, late: 0, scheduled: 0, cancelled: 0 });
    clockwork_sink_close(h);
}

#[test]
#[cfg(feature = "osc")]
fn an_osc_sink_honours_a_future_time_by_holding_it() {
    let _g = serialise();
    use std::net::UdpSocket;
    // UDP has no scheduled send, so the sink holds. What this proves is that
    // it holds for about the right length of time rather than either sending
    // early or forgetting.
    let listener = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
    listener.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let port = listener.local_addr().unwrap().port();
    let target = CString::new(format!("127.0.0.1:{port}")).unwrap();
    // SAFETY: a NUL-terminated CString.
    let h = unsafe { clockwork_sink_open(KIND_OSC as i32, target.as_ptr(), 64) };
    assert_ne!(h, 0);

    // 100 ms out, in 32.32 fixed point.
    let due = clockwork_sinks::time::now() + ((1u64 << 32) / 10);
    let sent_at = Instant::now();
    assert!(send(h, b"late-bound", due as i64));

    let mut buf = [0u8; 64];
    let n = listener.recv(&mut buf).expect("held forever");
    let waited = sent_at.elapsed();
    assert_eq!(&buf[..n], b"late-bound");
    assert!(waited >= Duration::from_millis(95),
            "delivered after {waited:?} — that is early, which is the one \
             thing a sink must never be");
    assert!(waited < Duration::from_millis(400), "delivered after {waited:?}");
    assert_eq!(stats_of(h).late, 0, "released at its time is not late");
    clockwork_sink_close(h);
}

// ── Opening is not idempotent ───────────────────────────────────────────────
//
// Two opens on ONE target are two sinks, not one. This is pinned because the
// obvious "improvement" — hand back the existing handle — would be a bug
// rather than a tidy-up. A sink belongs to whoever opened it, and clockwork
// closes a DSP's sinks when that DSP is freed (close_dsp_sinks,
// audio_processor.cpp). Sharing one handle between owners would let dsp_free
// close a sink the engine opened for itself: the same MIDI port name reaches
// both MidiClockOut and a DSP, so the casualty would be the engine's own clock
// output, and it would look like MIDI sync simply stopping.
//
// De-duplicating safely means refcounting, not a lookup. A caller that wants
// one sink per target does what OscControl does instead: list the open sinks,
// compare clockwork_sink_target, reuse the match.

#[test]
fn opening_the_same_target_twice_gives_two_independent_sinks() {
    let _g = serialise();
    registry::close_all();

    let (first, _ep_first)   = manual(64);
    let (second, _ep_second) = manual(64);
    assert_ne!(first, second, "two opens on one target must be two handles");

    // Both live, both naming the same destination — so they were not merged.
    assert_ne!(clockwork_sink_is_open(first), 0);
    assert_ne!(clockwork_sink_is_open(second), 0);
    let t1 = registry::acquire(first).unwrap().target.clone();
    let t2 = registry::acquire(second).unwrap().target.clone();
    assert_eq!(t1, t2, "both name the same target");

    // And each occupies its own slot.
    let mut slots = [0u32; 8];
    assert_eq!(registry::table().list(&mut slots), 2, "two sinks, two slots");

    // Independent lifetimes: closing one leaves the other sending. This is the
    // assertion that would fail first if a lookup were added to open.
    clockwork_sink_close(first);
    assert_eq!(clockwork_sink_is_open(first), 0);
    assert_ne!(clockwork_sink_is_open(second), 0,
               "closing one sink must not close another on the same target");
    assert!(send(second, b"still here", 1),
               "the surviving sink still accepts");

    clockwork_sink_close(second);
}
