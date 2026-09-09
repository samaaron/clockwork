// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Scheduled MIDI output: does the platform hold the message, or do we?
//!
//! `MidiOutputConnection::send_at(bytes, delay)` hands a delay to the platform
//! so the kernel (ALSA) or the MIDI server (CoreMIDI) does the waiting. The
//! alternative — a thread of ours sleeping and then sending — collects its own
//! scheduling jitter on the way, which is exactly what a MIDI clock cannot
//! afford: our sink's drain polls at 500us, so "immediately" costs up to that.
//!
//! Two properties, and the second is the one that matters:
//!   * the message arrives at about the delay asked for; and
//!   * it is NEVER early. A clock tick ahead of its beat is worse than one
//!     behind it, because nothing downstream can correct for it.
//!
//! Ignored by default: needs real ALSA/CoreMIDI virtual ports (/dev/snd/seq on
//! Linux, so a box where the user is in the `audio` group).
//!     cargo test -p clockwork-midi --test scheduled_send -- --ignored --nocapture
// Virtual MIDI ports are a CoreMIDI and ALSA facility: Windows has none, so
// there is nothing for this test to loop through there.
#![cfg(all(not(target_arch = "wasm32"), unix))]

use midir::os::unix::VirtualInput;
use midir::{MidiInput, MidiOutput};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Wall-clock arrival of each message, relative to a shared origin.
type Arrivals = Arc<Mutex<Vec<Duration>>>;

fn virtual_pair(name: &str, origin: Instant) -> (midir::MidiOutputConnection, Arrivals, midir::MidiInputConnection<()>) {
    let arrivals: Arrivals = Arc::new(Mutex::new(Vec::new()));
    let sink = arrivals.clone();

    let vin = MidiInput::new("ss-sched-test").unwrap();
    let dst = vin
        .create_virtual(name, move |_t, _b, _| {
            sink.lock().unwrap().push(origin.elapsed());
        }, ())
        .unwrap();

    // Find the port we just made and connect an output to it.
    let out = MidiOutput::new("ss-sched-test-out").unwrap();
    let port = out
        .ports()
        .into_iter()
        .find(|p| out.port_name(p).map(|n| n.contains(name)).unwrap_or(false))
        .unwrap_or_else(|| panic!("virtual destination {name} not enumerated"));
    let conn = out.connect(&port, "ss-sched-out").unwrap();
    (conn, arrivals, dst)
}

#[test]
#[ignore = "needs real ALSA/CoreMIDI virtual ports; run with --ignored --nocapture"]
fn the_platform_holds_the_message_until_it_is_due() {
    let origin = Instant::now();
    let (mut conn, arrivals, _dst) = virtual_pair("ss-sched-in", origin);

    assert!(conn.schedules(), "this backend reports no scheduled send");

    const DELAY_MS: u64 = 150;
    let sent_at = origin.elapsed();
    conn.send_at(&[0x90, 60, 100], Duration::from_millis(DELAY_MS)).unwrap();

    // Well past due, with room for the callback thread.
    std::thread::sleep(Duration::from_millis(DELAY_MS + 150));

    let got = arrivals.lock().unwrap().clone();
    assert_eq!(got.len(), 1, "expected exactly one message, got {}", got.len());

    let held_for = got[0].saturating_sub(sent_at);
    println!("asked for {DELAY_MS}ms, held for {:.1}ms", held_for.as_secs_f64() * 1000.0);

    // NEVER EARLY. A 5 ms floor rather than the full delay leaves room for the
    // receiving callback's own latency without admitting a genuinely early send.
    assert!(
        held_for >= Duration::from_millis(DELAY_MS - 5),
        "delivered EARLY: held {held_for:?}, asked for {DELAY_MS}ms"
    );
    // And actually held, rather than sent immediately and slow to arrive.
    assert!(
        held_for < Duration::from_millis(DELAY_MS + 100),
        "held far too long: {held_for:?}"
    );
}

#[test]
#[ignore = "needs real ALSA/CoreMIDI virtual ports; run with --ignored --nocapture"]
fn scheduled_sends_arrive_in_order_however_they_were_queued() {
    let origin = Instant::now();
    let (mut conn, arrivals, _dst) = virtual_pair("ss-sched-order", origin);

    // Queued backwards: the platform, not the order of our calls, decides
    // delivery order. Sending these straight out would arrive reversed.
    for (i, ms) in [200u64, 150, 100, 50].into_iter().enumerate() {
        // Note number varies, velocity stays legal: a MIDI data byte is 0..127
        // and 200 is not one, which the ALSA encoder rejects outright.
        conn.send_at(&[0x90, 60 + i as u8, 100], Duration::from_millis(ms)).unwrap();
    }
    std::thread::sleep(Duration::from_millis(400));

    let got = arrivals.lock().unwrap().clone();
    assert_eq!(got.len(), 4, "expected 4 messages, got {}", got.len());
    for w in got.windows(2) {
        assert!(w[0] <= w[1], "arrived out of order: {:?} then {:?}", w[0], w[1]);
    }
    println!(
        "queued 200,150,100,50ms → arrived at {}",
        got.iter().map(|d| format!("{:.0}ms", d.as_secs_f64() * 1000.0))
            .collect::<Vec<_>>().join(", ")
    );
}

#[test]
#[ignore = "needs real ALSA/CoreMIDI virtual ports; run with --ignored --nocapture"]
fn scheduled_delivery_is_tighter_than_our_own_polling() {
    // The number this whole change exists for. 24 pulses at a 20.83 ms spacing
    // is one beat of MIDI clock at 120 BPM.
    let origin = Instant::now();
    let (mut conn, arrivals, _dst) = virtual_pair("ss-sched-jitter", origin);

    const N: u64 = 24;
    const SPACING_MS: f64 = 20.833_333;
    let base = origin.elapsed();
    let start = Instant::now();
    for i in 0..N {
        // A relative schedule is relative to when the KERNEL receives it, so
        // the delay must be computed at the moment of the call. Passing a
        // fixed offset from a loop start instead makes every send inherit the
        // time the previous ones took to enqueue, which shows up as drift
        // growing across the beat rather than as jitter.
        let target = Duration::from_secs_f64(SPACING_MS * i as f64 / 1000.0);
        let delay = target.saturating_sub(start.elapsed());
        conn.send_at(&[0xF8], delay).unwrap();
    }
    std::thread::sleep(Duration::from_millis(((N as f64) * SPACING_MS) as u64 + 200));

    let got = arrivals.lock().unwrap().clone();
    assert_eq!(got.len(), N as usize, "lost pulses: got {}", got.len());

    // Signed error against the ideal grid. Offset and jitter are DIFFERENT
    // faults and must not be averaged together: a constant lateness is latency
    // through the virtual port and the receiving callback, which shifts every
    // pulse equally and is inaudible in a clock. What breaks sync is the
    // SPREAD around that offset, so that is what gets the tight bound.
    let devs: Vec<f64> = got
        .iter()
        .enumerate()
        .map(|(i, t)| {
            let ideal = base.as_secs_f64() * 1000.0 + SPACING_MS * i as f64;
            t.as_secs_f64() * 1000.0 - ideal
        })
        .collect();
    let offset = devs.iter().sum::<f64>() / devs.len() as f64;
    let jitter = devs.iter().cloned().fold(f64::MIN, f64::max)
               - devs.iter().cloned().fold(f64::MAX, f64::min);
    let sd = (devs.iter().map(|d| (d - offset).powi(2)).sum::<f64>()
              / devs.len() as f64).sqrt();
    println!("scheduled clock over {N} pulses: offset {offset:+.3}ms, \
              jitter (spread) {jitter:.3}ms, sd {sd:.3}ms");

    // Measured on this box, idle and under the full MIDI suite:
    //     offset +0.80..+1.23ms   sd 0.32..0.50ms   spread 1.29..2.67ms
    // with one run's spread going past 4ms. The offset is delivery latency
    // through the virtual port and the receiving callback and is near constant;
    // sd is the real timing quality; spread is max-minus-min over only 24
    // samples, so one descheduled callback moves it and it has the loosest
    // bound.
    //
    // The spread bound is one whole pulse interval, not a number picked to make
    // this pass: at 20.83ms spacing, a pulse displaced by more than a full
    // interval is landing in another pulse's slot, which is the point where the
    // clock stops being a clock. A bound tuned to the observed tail would just
    // be re-measuring the machine's scheduler.
    //
    // Any of these catches the regression that matters by a wide margin: if
    // send_at silently sent immediately, all 24 pulses would land at once and
    // the last would be ~479ms early.
    assert!(sd < 3.0, "clock sd {sd:.3}ms is too loose for a MIDI clock");
    assert!(jitter < SPACING_MS, "a pulse is displaced by more than a whole \
            pulse interval: spread {jitter:.3}ms");
    assert!(offset.abs() < 5.0, "unexpected constant offset {offset:+.3}ms");
}
