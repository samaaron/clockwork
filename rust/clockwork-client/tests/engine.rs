// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The crate against a real engine: attached headless in this process,
//! with the dummy DSP (dsp/dummy), which answers `/dummy/ping` with
//! `/dummy/pong` and renders a pulse train.
//!
//! One engine per process, and cargo runs a binary's tests on several
//! threads, so every test here takes the one engine under a lock and
//! renders it itself — nothing ticks in the background unless a test
//! stands up a [`Renderer`] thread to.

// The Rust host: the engine (clockwork-sys) and the subsystems it calls back
// into (clockwork-native, `host`). A crate nothing names is not linked.
extern crate clockwork_native;
extern crate clockwork_sys;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use clockwork_abi::arena::{geom, ClockworkArenaRegion};
use clockwork_abi::client::CLOCKWORK_CLIENT_ABI_VERSION;
use clockwork_client::{
    Client, Embed, EmbedConfig, Error, Metric, NativeStat, RegionId, Ring, Route, ScopeRead,
    SCOPE_MAX_CHANNELS,
};
use clockwork_native::clockwork_scope;

/// Our origin token: non-zero, so replies come back to us rather than to
/// the notification audience.
const ORIGIN: u32 = 0x5245_5A5A;
const RATE: f64 = 48_000.0;

static ENGINE: Mutex<Option<Embed>> = Mutex::new(None);

fn config() -> EmbedConfig {
    EmbedConfig::new().sample_rate(RATE).input_channels(2).output_channels(2).max_render_frames(4096)
}

/// The engine, attached on first use. A test that panicked poisons the
/// lock; the engine underneath is still fine.
fn engine() -> MutexGuard<'static, Option<Embed>> {
    let mut g = ENGINE.lock().unwrap_or_else(|p| p.into_inner());
    if g.is_none() {
        *g = Some(Embed::attach(&config()).expect("attach a headless engine"));
    }
    g
}

fn ping() -> Vec<u8> {
    clockwork_osc::encode("/dummy/ping", &[])
}

/// Render `blocks` whole engine blocks, which is what makes the engine act
/// on what was sent.
fn render_blocks(e: &mut Embed, blocks: usize) {
    let n = e.block_size() as usize * blocks;
    let (mut l, mut r) = (vec![0.0f32; n], vec![0.0f32; n]);
    assert_eq!(e.render(&mut [l.as_mut_slice(), r.as_mut_slice()], &[], n), n);
}

/// Render and drain until nothing more comes out for a few blocks: the
/// engine has answered everything an earlier test left in its rings, so a
/// test that counts replies counts only its own.
fn settle(e: &mut Embed) {
    let mut quiet = 0;
    for _ in 0..10_000 {
        render_blocks(e, 1);
        if e.drain() == 0 {
            quiet += 1;
            if quiet >= 4 {
                return;
            }
        } else {
            quiet = 0;
        }
    }
    panic!("the engine never went quiet");
}

/// The engine's own count of messages taken off the ingress ring.
fn processed(e: &Embed) -> u32 {
    e.metrics().get(Metric::EngineMessagesProcessed).unwrap()
}

#[test]
fn attach_reports_the_geometry_it_runs_at_and_has_no_device() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    assert_eq!(e.sample_rate(), RATE);
    assert!(e.block_size() > 0);
    let info = e.info().unwrap();
    assert_eq!(info.sample_rate, RATE);
    assert_eq!(info.block_frames, e.block_size());
    assert_eq!(info.output_channels, 2);
    assert_eq!(info.abi_version, CLOCKWORK_CLIENT_ABI_VERSION);
    assert_eq!(Client::abi_version(), CLOCKWORK_CLIENT_ABI_VERSION);
    assert!(info.features.scope(), "the default build carries scope slots: {:?}", info.features);
    // No device behind an attached handle, and the read-back says so.
    assert_eq!(e.device().err(), Some(Error::Arg));
}

#[test]
fn one_engine_per_process_and_a_config_it_cannot_serve_is_refused() {
    let _g = engine();
    assert_eq!(Embed::attach(&config()).err(), Some(Error::Perm));
    // Refused for the argument before the process is looked at.
    assert_eq!(Embed::attach(&EmbedConfig::new().output_channels(2)).err(), Some(Error::Arg));
    assert_eq!(Embed::attach(&EmbedConfig::new().sample_rate(RATE)).err(), Some(Error::Arg));
}

#[test]
fn a_ping_is_answered_on_the_next_block_with_the_origin_echoed() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    e.send(&ping(), ORIGIN).unwrap();
    render_blocks(e, 1);
    let pong = e
        .poll()
        .find(|m| m.address() == "/dummy/pong")
        .map(|m| (m.origin, m.route, m.sequence))
        .expect("a /dummy/pong after one block");
    assert_eq!(pong.0, ORIGIN);
    assert_eq!(pong.1, Route::Reply);
    // Nothing more: the reply was taken.
    assert!(e.poll().all(|m| m.address() != "/dummy/pong"));
}

#[test]
fn send_refuses_nothing_and_too_much_and_a_full_ring_is_a_retry() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    assert_eq!(e.send(&[], ORIGIN).err(), Some(Error::Arg));
    let huge = vec![0u8; 1 << 20];
    assert_eq!(e.send(&huge, ORIGIN).err(), Some(Error::TooBig));
    assert!(Error::Full.is_retry());
    // Fill the ring without rendering: pings until it says so.
    let p = ping();
    let before = processed(e);
    let mut sent = 0u32;
    let full = loop {
        match e.send(&p, ORIGIN) {
            Ok(()) => sent += 1,
            Err(Error::Full) => break true,
            Err(other) => panic!("{other}"),
        }
        if sent > 1_000_000 {
            break false;
        }
    };
    assert!(full, "the ring never filled");
    // Rendering makes room — the engine takes what it can each block — so
    // a retry goes through, and every message that was queued is processed,
    // by the engine's own count. (Their replies may lap the egress ring;
    // that is reported as a sequence gap, not hidden, and not counted here.)
    let mut sent_after = false;
    for _ in 0..10_000 {
        render_blocks(e, 1);
        e.drain();
        if e.send(&p, ORIGIN).is_ok() {
            sent_after = true;
            break;
        }
    }
    assert!(sent_after, "a full ring never made room");
    let mut done = 0;
    for _ in 0..1_000_000 {
        done = processed(e).wrapping_sub(before);
        if done > sent {
            break;
        }
        render_blocks(e, 1);
        e.drain();
    }
    assert!(done > sent, "{done} of {} processed", sent + 1);
    settle(e);
}

#[test]
fn send_with_builds_the_message_in_the_ring_and_never_wedges_it() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    let p = ping();
    e.send_with(64, ORIGIN, |buf| {
        buf[..p.len()].copy_from_slice(&p);
        p.len()
    })
    .unwrap();
    // A fill that lies about its length is refused and the reservation
    // given back...
    assert_eq!(e.send_with(64, ORIGIN, |_| 65).err(), Some(Error::Arg));
    assert_eq!(e.send_with(64, ORIGIN, |_| 0).err(), Some(Error::Arg));
    // ...and so is one that panics, so the ring is not stopped for the life
    // of the process. (The panic is the point; its report is not.)
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        e.send_with(64, ORIGIN, |_| panic!("mid-fill"))
    }));
    std::panic::set_hook(hook);
    assert!(panicked.is_err());
    // The ring still takes a message after all three.
    e.send(&p, ORIGIN).unwrap();
    render_blocks(e, 1);
    assert_eq!(e.poll().filter(|m| m.address() == "/dummy/pong").count(), 2);
}

#[test]
fn drain_takes_everything_and_sequences_are_monotonic() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    let p = ping();
    for _ in 0..10 {
        e.send(&p, ORIGIN).unwrap();
    }
    render_blocks(e, 1);
    e.set_poll_batch(4);
    let mut seqs = Vec::new();
    let mut pongs = 0;
    loop {
        let batch: Vec<_> = e.poll().map(|m| (m.sequence, m.address() == "/dummy/pong")).collect();
        if batch.is_empty() {
            break;
        }
        assert!(batch.len() <= 4);
        pongs += batch.iter().filter(|b| b.1).count();
        seqs.extend(batch.iter().map(|b| b.0));
    }
    assert_eq!(pongs, 10);
    assert!(seqs.windows(2).all(|w| w[1] > w[0]), "{seqs:?}");
    e.set_poll_batch(clockwork_client::POLL_BATCH);
    for _ in 0..10 {
        e.send(&p, ORIGIN).unwrap();
    }
    render_blocks(e, 1);
    assert!(e.drain() >= 10);
    assert_eq!(e.poll().len(), 0);
}

#[test]
fn a_renderer_on_another_thread_ticks_while_the_client_waits_for_its_reply() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    let block = e.block_size() as usize;
    let (mut renderer, client) = e.split();
    let stop = AtomicBool::new(false);
    std::thread::scope(|s| {
        let stop = &stop;
        s.spawn(move || {
            let (mut l, mut r) = (vec![0.0f32; block], vec![0.0f32; block]);
            while !stop.load(Ordering::Relaxed) {
                renderer.render(&mut [l.as_mut_slice(), r.as_mut_slice()], &[], block);
                std::thread::sleep(Duration::from_millis(1));
            }
        });
        client.send(&ping(), ORIGIN).unwrap();
        let got = client.poll_until(Duration::from_secs(5), |m| {
            (m.address() == "/dummy/pong").then_some(m.origin)
        });
        stop.store(true, Ordering::Relaxed);
        assert_eq!(got, Some(ORIGIN));
        // And a wait for something that never comes ends when told.
        let never = client.poll_until(Duration::from_millis(30), |m| {
            (m.address() == "/dummy/never").then_some(())
        });
        assert_eq!(never, None);
    });
}

#[test]
fn a_tap_watches_both_rings_without_taking() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    settle(e);
    let (mut renderer, client) = e.split();
    let block = client.info().unwrap().block_frames as usize;
    let (mut l, mut r) = (vec![0.0f32; block], vec![0.0f32; block]);
    {
        let mut ingress = client.tap(Ring::Ingress).unwrap();
        let mut egress = client.tap(Ring::Egress).unwrap();
        assert_eq!((ingress.ring(), egress.ring()), (Ring::Ingress, Ring::Egress));
        client.send(&ping(), ORIGIN).unwrap();
        // The ingress ring is the engine's to consume, so it is watched
        // between the send and the block that eats it — which is when a
        // logger sees it too.
        let seen_in: Vec<(String, u32)> = ingress.poll().map(|m| (m.address().to_owned(), m.origin)).collect();
        assert_eq!(seen_in, [("/dummy/ping".to_owned(), ORIGIN)]);
        renderer.render(&mut [l.as_mut_slice(), r.as_mut_slice()], &[], block);
        let seen_out: Vec<(String, u32, Route)> =
            egress.poll().map(|m| (m.address().to_owned(), m.origin, m.route)).collect();
        assert!(seen_out.contains(&("/dummy/pong".to_owned(), ORIGIN, Route::Reply)), "{seen_out:?}");
        assert_eq!(ingress.missed() + egress.missed(), 0);
    }
    // Watching took nothing: the consumer still gets the reply.
    assert!(client.poll().any(|m| m.is_reply_to(ORIGIN) && m.address() == "/dummy/pong"));
}

#[test]
fn regions_are_named_and_absent_is_an_answer() {
    let g = engine();
    let e = g.as_ref().unwrap();
    let metrics = e.region(RegionId::Metrics).unwrap();
    assert!(metrics.len() >= 4 && !metrics.is_writable());
    assert_eq!(metrics.read_u32(metrics.len() / 4), None);
    assert!(metrics.read_u32(0).is_some());
    assert!(e.region(RegionId::Ingress).unwrap().is_writable());
    assert!(!e.region(RegionId::Egress).unwrap().is_writable());
    assert!(!e.region(RegionId::Window).unwrap().is_empty());
    assert!(!e.region(RegionId::Scope).unwrap().is_empty());
    assert!(!e.region(RegionId::AudioTaps).unwrap().is_empty());
    let stats = e.region(RegionId::NativeStats).unwrap();
    assert!(stats.len() >= 4 * clockwork_client::metrics_schema::NATIVE_STATS.len());
    assert!(!stats.is_writable());
    // An in-process handle over bare memory cannot see the bulk lanes.
    assert_eq!(e.region(RegionId::Inbox).err(), Some(Error::Absent));
    assert_eq!(e.region(RegionId::Outbox).err(), Some(Error::Absent));
}

#[test]
fn metrics_are_a_block_that_counts_the_blocks_rendered() {
    let mut g = engine();
    let e = g.as_mut().unwrap();
    let before = e.metrics();
    render_blocks(e, 4);
    let after = e.metrics();
    assert_eq!(after.len() * 4, e.region(RegionId::Metrics).unwrap().len());
    let ticks = |m: &clockwork_client::Metrics| m.get(Metric::EngineProcessCount).unwrap();
    assert!(ticks(&after) >= ticks(&before) + 4, "{before:?} -> {after:?}");
    assert_eq!(after.get(Metric::AudioSampleRate), Some(RATE as u32));
    assert_eq!(after.get(Metric::AudioBlockSize), Some(e.block_size()));
    assert_eq!(after.get(Metric::AudioOutputChannels), Some(2));
    assert!(after.iter().any(|(m, _)| m == Metric::EngineProcessCount));
    assert_eq!(after.as_slice()[0], ticks(&after));
    assert_eq!(after.word(after.len()), None);
}

#[test]
fn native_stats_read_as_a_block_and_a_headless_engine_never_overruns() {
    let g = engine();
    let e = g.as_ref().unwrap();
    let s = e.native_stats().unwrap();
    assert!(s.len() >= clockwork_client::metrics_schema::NATIVE_STATS.len());
    assert_eq!(s.overruns(), 0);
    assert_eq!(s.get(NativeStat::CbOverruns), Some(0));
    assert!(s.cpu_avg_percent() >= 0.0 && s.cpu_peak_percent() >= 0.0);
    assert_eq!(s.iter().count(), NativeStat::ALL.len());
}

#[test]
fn the_clock_is_one_snapshot_and_beats_follow_its_tempo() {
    let g = engine();
    let e = g.as_ref().unwrap();
    let c = e.clock().unwrap();
    assert!(c.bpm() > 0.0, "{c:?}");
    let origin = c.beat_origin_ntp();
    let one_beat = 60.0 / c.bpm();
    let a = c.beat_at(origin);
    let b = c.beat_at(origin + one_beat);
    assert!((b - a - 1.0).abs() < 1e-9, "{a} -> {b}");
    assert!(c.beat_now().is_finite());
    let (num, den) = c.meter();
    assert!(num > 0 && den > 0, "{num}/{den}");
    assert_eq!(c.raw().bpm, c.bpm());
}

#[test]
fn a_scope_slot_reads_silence_until_a_writer_claims_it_and_then_what_was_written() {
    let g = engine();
    let e = g.as_ref().unwrap();
    // The buffer-sizing constant is the engine's own geometry, or the read
    // above it is wrong.
    // SAFETY: the engine is attached, so the arena header is mapped and
    // published; read only.
    let header = unsafe { &*clockwork_abi::lanes::clockwork_arena_header() };
    let scope_entry = header.find(ClockworkArenaRegion::SCOPE).expect("the arena has a scope region");
    assert_eq!(scope_entry.geom[geom::SCOPE_CHANNELS] as usize, SCOPE_MAX_CHANNELS);

    assert_eq!(e.scope(1_000_000).err(), Some(Error::Absent));
    let scope = e.scope(0).unwrap();
    assert_eq!(scope.slot(), 0);
    assert!(!scope.is_valid());
    let mut out = vec![7.0f32; 256 * SCOPE_MAX_CHANNELS];
    let got = scope.read_newest(256, &mut out);
    assert_eq!(got.frames, 0);
    assert!(out.iter().all(|&s| s == 0.0), "an unclaimed slot yields silence, not the buffer's past");
    // Too small a buffer reads nothing rather than past its end.
    assert_eq!(scope.read_newest(256, &mut out[..1]), ScopeRead::default());

    // Now a writer claims the slot and writes a ramp, as a guest does.
    assert!(clockwork_scope::stream_activate(0, 2));
    let n = 1024usize;
    let mut ramp = vec![0.0f32; n * 2];
    for f in 0..n {
        ramp[f * 2] = f as f32;
        ramp[f * 2 + 1] = -(f as f32);
    }
    clockwork_scope::stream_write(0, &ramp, n, 0);
    assert!(scope.is_valid());
    let got = scope.read_newest(256, &mut out);
    assert_eq!(got, ScopeRead { frames: 256, channels: 2 });
    for f in 0..256 {
        assert_eq!(out[f * 2], (768 + f) as f32, "frame {f} left");
        assert_eq!(out[f * 2 + 1], -((768 + f) as f32), "frame {f} right");
    }
    // The audible edge never runs past what was written, and a window that
    // reaches before the ring's start is silence at the front.
    assert!(scope.audible_end() <= n as u64);
    let mut wide = vec![7.0f32; 2048 * SCOPE_MAX_CHANNELS];
    let got = scope.read_newest(2048, &mut wide);
    assert_eq!(got.frames, n);
    assert!(wide[..(2048 - n) * 2].iter().all(|&s| s == 0.0));
    assert_eq!(wide[(2048 - n) * 2], 0.0);
    assert_eq!(wide[(2048 - n + 1) * 2], 1.0);
    let got = scope.read_audible(64, &mut out);
    assert!(got.frames <= 64);

    clockwork_scope::stream_release(0);
    assert!(!scope.is_valid());
    let got = scope.read_newest(256, &mut out);
    assert_eq!(got.frames, 0);
    assert!(out.iter().all(|&s| s == 0.0));
}

#[test]
fn an_endpoint_is_named_by_port_and_an_engine_that_is_not_there_is_not_found() {
    let _g = engine();
    let ep = Client::default_endpoint(57_110);
    assert!(ep.contains("57110"), "{ep}");
    assert_ne!(Client::default_endpoint(57_111), ep);
    assert_eq!(Client::open_shm(&ep).err(), Some(Error::NotFound));
    assert_eq!(Client::open_shm("bad\0endpoint").err(), Some(Error::Arg));
}
