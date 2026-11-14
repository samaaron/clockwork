// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The crate against real engines: each test attaches one of its own,
//! headless in this process, with the dummy DSP (dsp/dummy), which answers
//! `/dummy/ping` with `/dummy/pong` and renders a pulse train.
//!
//! Cargo runs a binary's tests on several threads, so several engines run
//! at once. Each test renders its own — nothing ticks in the background
//! unless a test stands up a [`Renderer`] thread to.

// The Rust host: the engine (clockwork-sys) and the subsystems it calls back
// into (clockwork-native, `host`). A crate nothing names is not linked.
extern crate clockwork_native;
extern crate clockwork_sys;

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use clockwork_abi::arena::{geom, ClockworkArenaRegion};
use clockwork_abi::client::CLOCKWORK_CLIENT_ABI_VERSION;
use clockwork_client::{
    Client, Embed, EmbedConfig, Error, Metric, NativeStat, Profile, RegionId, Ring, AudioTapRead,
    AUDIO_TAP_MAX_CHANNELS,
};
use clockwork_native::clockwork_taps;

/// Our origin token: non-zero, so a reply is told from a message nobody
/// asked for.
const ORIGIN: u32 = 0x5245_5A5A;
const RATE: f64 = 48_000.0;

fn config() -> EmbedConfig {
    EmbedConfig::new().sample_rate(RATE).input_channels(2).output_channels(2).max_render_frames(4096)
}

/// An engine of the test's own, attached headless.
fn engine() -> Embed {
    Embed::attach(&config()).expect("attach a headless engine")
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
/// engine has said everything it says at start, so a test that counts
/// replies counts only its own.
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
    let e = &mut engine();
    assert_eq!(e.sample_rate(), RATE);
    assert!(e.block_size() > 0);
    let info = e.info().unwrap();
    assert_eq!(info.sample_rate, RATE);
    assert_eq!(info.block_frames, e.block_size());
    assert_eq!(info.output_channels, 2);
    assert_eq!(info.abi_version, CLOCKWORK_CLIENT_ABI_VERSION);
    assert_eq!(Client::abi_version(), CLOCKWORK_CLIENT_ABI_VERSION);
    assert!(info.features.taps(), "the default build carries tap slots: {:?}", info.features);
    // No device behind an attached handle, and the read-back says so.
    assert_eq!(e.device().err(), Some(Error::Arg));
}

#[test]
fn engines_side_by_side_answer_each_their_own_and_a_config_it_cannot_serve_is_refused() {
    let first = &mut engine();
    settle(first);
    let mut beside = engine();
    settle(&mut beside);
    beside.send(&ping(), ORIGIN).unwrap();
    render_blocks(&mut beside, 1);
    render_blocks(first, 1);
    assert!(beside.poll().any(|m| m.address() == "/dummy/pong"), "the second answers its own ping");
    assert!(!first.poll().any(|m| m.address() == "/dummy/pong"), "the first heard nothing of it");
    // Closing one leaves the other.
    drop(beside);
    first.send(&ping(), ORIGIN).unwrap();
    render_blocks(first, 1);
    assert!(first.poll().any(|m| m.address() == "/dummy/pong"), "the first still answers");
    assert_eq!(Embed::attach(&EmbedConfig::new().output_channels(2)).err(), Some(Error::Arg));
    assert_eq!(Embed::attach(&EmbedConfig::new().sample_rate(RATE)).err(), Some(Error::Arg));
}

// An engine laid out from a plugin's profile beside one laid out from the
// target's (docs/ENGINES.md): its memory is its own size, and it
// answers as any engine does.
#[test]
fn an_engine_from_a_plugin_profile_is_its_own_size_and_answers_the_same() {
    let plugin = Profile::plugin();
    assert_eq!(plugin.refused(), None);
    let mut small = Embed::attach(&config().profile(plugin)).expect("a plugin's engine");
    let big = engine();
    let ingress = |e: &Embed| e.region(RegionId::Ingress).unwrap().len();
    assert_eq!(ingress(&small), plugin.in_ring_bytes as usize);
    assert_eq!(ingress(&big), Profile::target().in_ring_bytes as usize);
    assert!(ingress(&small) < ingress(&big));
    settle(&mut small);
    small.send(&ping(), ORIGIN).unwrap();
    render_blocks(&mut small, 1);
    assert!(small.poll().any(|m| m.address() == "/dummy/pong"));

    // One no engine can be laid out from says why, and is refused.
    let mut tiny = Profile::plugin();
    tiny.in_ring_bytes = 1000;
    assert!(tiny.refused().is_some_and(|why| why.contains("4 KB")), "{:?}", tiny.refused());
    assert_eq!(Embed::attach(&config().profile(tiny)).err(), Some(Error::Arg));
}

#[test]
fn a_reconfigure_keeps_the_engine_and_runs_it_at_the_new_rate() {
    let e = &mut engine();
    settle(e);
    for rate in [44_100.0, RATE] {
        e.reconfigure(&config().sample_rate(rate)).unwrap();
        assert_eq!(e.sample_rate(), rate);
        settle(e);
        e.send(&ping(), ORIGIN).unwrap();
        render_blocks(e, 1);
        assert!(e.poll().any(|m| m.address() == "/dummy/pong"), "answered at {rate} Hz");
    }
    assert_eq!(e.reconfigure(&EmbedConfig::new().output_channels(2)).err(), Some(Error::Arg));
}

#[test]
fn a_ping_is_answered_on_the_next_block_with_the_origin_echoed() {
    let e = &mut engine();
    settle(e);
    e.send(&ping(), ORIGIN).unwrap();
    render_blocks(e, 1);
    let pong = e
        .poll()
        .find(|m| m.address() == "/dummy/pong")
        .map(|m| (m.origin, m.sequence))
        .expect("a /dummy/pong after one block");
    assert_eq!(pong.0, ORIGIN);
    // Nothing more: the reply was taken.
    assert!(e.poll().all(|m| m.address() != "/dummy/pong"));
}

#[test]
fn send_refuses_nothing_and_too_much_and_a_full_ring_is_a_retry() {
    let e = &mut engine();
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
}

#[test]
fn send_with_builds_the_message_in_the_ring_and_never_wedges_it() {
    let e = &mut engine();
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
    let e = &mut engine();
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
fn a_wait_is_over_when_a_block_says_something_and_not_before() {
    let e = &mut engine();
    settle(e);
    // Caught up with whatever was said before.
    while e.wait(Some(Duration::ZERO)).unwrap() {}
    assert!(!e.wait(Some(Duration::from_millis(20))).unwrap(), "nothing rendered, nothing said");
    render_blocks(e, 1);
    assert!(!e.wait(Some(Duration::ZERO)).unwrap(), "a block that said nothing");
    e.send(&ping(), ORIGIN).unwrap();
    render_blocks(e, 1);
    assert!(e.wait(Some(Duration::ZERO)).unwrap(), "the block that answered");
    assert!(e.poll().any(|m| m.address() == "/dummy/pong"));
}

#[test]
fn on_egress_tells_another_thread_each_time_and_stops_when_told() {
    let e = &mut engine();
    settle(e);
    // The engine has said something, so the listener is told once at the
    // start, for it.
    e.send(&ping(), ORIGIN).unwrap();
    render_blocks(e, 1);
    e.drain();
    let (tx, rx) = std::sync::mpsc::channel();
    e.on_egress(move || {
        let _ = tx.send(());
    })
    .unwrap();
    rx.recv_timeout(Duration::from_secs(5)).expect("told at the start");
    // Then for the block that answers a ping: the pong is there to take.
    e.send(&ping(), ORIGIN).unwrap();
    render_blocks(e, 1);
    rx.recv_timeout(Duration::from_secs(5)).expect("told when the block answered");
    assert!(e.poll().any(|m| m.address() == "/dummy/pong"));
    // Stopped, its thread has ended: what told the channel is gone with it.
    e.stop_listening();
    while rx.try_recv().is_ok() {}
    assert_eq!(rx.try_recv(), Err(std::sync::mpsc::TryRecvError::Disconnected));
}

#[test]
fn a_renderer_on_another_thread_ticks_while_the_client_waits_for_its_reply() {
    let e = &mut engine();
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
        // Bounded only against a pong that never comes: the renderer thread
        // takes the time it takes on a loaded machine.
        let got = client.poll_until(Duration::from_secs(30), |m| {
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
    let e = &mut engine();
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
        let seen_out: Vec<(String, u32)> =
            egress.poll().map(|m| (m.address().to_owned(), m.origin)).collect();
        assert!(seen_out.contains(&("/dummy/pong".to_owned(), ORIGIN)), "{seen_out:?}");
        assert_eq!(ingress.missed() + egress.missed(), 0);
    }
    // Watching took nothing: the consumer still gets the reply.
    assert!(client.poll().any(|m| m.is_reply_to(ORIGIN) && m.address() == "/dummy/pong"));
}

#[test]
fn regions_are_named_and_absent_is_an_answer() {
    let e = &engine();
    let metrics = e.region(RegionId::Metrics).unwrap();
    assert!(metrics.len() >= 4 && !metrics.is_writable());
    assert_eq!(metrics.read_u32(metrics.len() / 4), None);
    assert!(metrics.read_u32(0).is_some());
    assert!(e.region(RegionId::Ingress).unwrap().is_writable());
    assert!(!e.region(RegionId::Egress).unwrap().is_writable());
    assert!(!e.region(RegionId::Window).unwrap().is_empty());
    assert!(!e.region(RegionId::Taps).unwrap().is_empty());
    assert!(!e.region(RegionId::Scope).unwrap().is_empty());
    let stats = e.region(RegionId::NativeStats).unwrap();
    assert!(stats.len() >= 4 * clockwork_client::metrics_schema::NATIVE_STATS.len());
    assert!(!stats.is_writable());
    // The bulk lanes are the handle's own (docs/MEMORY.md): the assets area, which a client writes a sample into,
    // and the returns area after it.
    let assets = e.region(RegionId::Assets).unwrap();
    assert!(!assets.is_empty() && assets.is_writable());
    assert!(!e.region(RegionId::Returns).unwrap().is_empty());
}

#[test]
fn metrics_are_a_block_that_counts_the_blocks_rendered() {
    let e = &mut engine();
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
    let e = &engine();
    let s = e.native_stats().unwrap();
    assert!(s.len() >= clockwork_client::metrics_schema::NATIVE_STATS.len());
    assert_eq!(s.overruns(), 0);
    assert_eq!(s.get(NativeStat::CbOverruns), Some(0));
    assert!(s.cpu_avg_percent() >= 0.0 && s.cpu_peak_percent() >= 0.0);
    assert_eq!(s.iter().count(), NativeStat::ALL.len());
}

#[test]
fn the_clock_is_one_snapshot_and_beats_follow_its_tempo() {
    let e = &engine();
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
fn the_sample_clock_pairs_a_frame_with_the_ntp_it_is_heard_at() {
    let e = &mut engine();
    // Published by a block.
    assert!(e.sample_clock().is_err());
    render_blocks(e, 1);
    let c = e.sample_clock().unwrap();
    assert!(c.sample_rate() > 0, "{c:?}");
    assert_eq!(c.ntp_at_frame(c.engine_frames()), c.dac_ntp());
    let a_second_on = c.ntp_at_frame(c.engine_frames() + u64::from(c.sample_rate()));
    assert!((a_second_on - c.dac_ntp() - 1.0).abs() < 1e-6, "{a_second_on} vs {}", c.dac_ntp());
}

#[test]
fn a_tap_slot_reads_silence_until_a_writer_claims_it_and_then_what_was_written() {
    let e = &engine();
    // The buffer-sizing constant is the engine's own geometry, or the read
    // above it is wrong.
    // The engine's core, found from memory it publishes, as a client in this
    // process finds it.
    let metrics = e.region(RegionId::Metrics).unwrap();
    // SAFETY: the engine is attached, so its arena is mapped; a lookup.
    let core = unsafe { clockwork_abi::lanes::clockwork_core_of_memory(metrics.as_ptr().cast()) };
    assert!(!core.is_null(), "the engine's metrics are in its core's arena");
    // SAFETY: the engine is attached, so the arena header is mapped and
    // published; read only.
    let header = unsafe { &*clockwork_abi::lanes::clockwork_arena_header(core) };
    let tap_entry = header.find(ClockworkArenaRegion::CLOCKWORK_TAPS).expect("the arena has a tap region");
    assert_eq!(tap_entry.geom[geom::TAP_CHANNELS] as usize, AUDIO_TAP_MAX_CHANNELS);

    assert_eq!(e.audio_tap(1_000_000).err(), Some(Error::Absent));
    let tap = e.audio_tap(0).unwrap();
    assert_eq!(tap.slot(), 0);
    assert!(!tap.is_valid());
    let mut out = vec![7.0f32; 256 * AUDIO_TAP_MAX_CHANNELS];
    let got = tap.read_newest(256, &mut out);
    assert_eq!(got.frames, 0);
    assert!(out.iter().all(|&s| s == 0.0), "an unclaimed slot yields silence, not the buffer's past");
    // Too small a buffer reads nothing rather than past its end.
    assert_eq!(tap.read_newest(256, &mut out[..1]), AudioTapRead::default());

    // Now a writer claims the slot and writes a ramp, as a guest does. The
    // claim counts as a live spell.
    let went_live = tap.activations();
    // SAFETY: the engine is attached, so its arena is mapped.
    let arena = unsafe { clockwork_abi::lanes::clockwork_lanes_base(core) }.cast::<u8>();
    // SAFETY: the engine's arena, mapped while it is attached.
    assert!(unsafe { clockwork_taps::stream_activate(arena, 0, 2) });
    assert_eq!(tap.activations(), went_live.wrapping_add(1));
    let n = 1024usize;
    let mut ramp = vec![0.0f32; n * 2];
    for f in 0..n {
        ramp[f * 2] = f as f32;
        ramp[f * 2 + 1] = -(f as f32);
    }
    // SAFETY: as above.
    unsafe { clockwork_taps::stream_write(arena, 0, &ramp, n, 0) };
    assert!(tap.is_valid());
    let got = tap.read_newest(256, &mut out);
    assert_eq!(got, AudioTapRead { frames: 256, channels: 2 });
    for f in 0..256 {
        assert_eq!(out[f * 2], (768 + f) as f32, "frame {f} left");
        assert_eq!(out[f * 2 + 1], -((768 + f) as f32), "frame {f} right");
    }
    // The audible edge never runs past what was written, and a window that
    // reaches before the ring's start is silence at the front.
    assert!(tap.audible_end() <= n as u64);
    let mut wide = vec![7.0f32; 2048 * AUDIO_TAP_MAX_CHANNELS];
    let got = tap.read_newest(2048, &mut wide);
    assert_eq!(got.frames, n);
    assert!(wide[..(2048 - n) * 2].iter().all(|&s| s == 0.0));
    assert_eq!(wide[(2048 - n) * 2], 0.0);
    assert_eq!(wide[(2048 - n + 1) * 2], 1.0);
    let got = tap.read_audible(64, &mut out);
    assert!(got.frames <= 64);

    // SAFETY: as above.
    unsafe { clockwork_taps::stream_release(arena, 0) };
    assert!(!tap.is_valid());
    // Released, it still says it went live: a reader that polled only after
    // the release can tell.
    assert_eq!(tap.activations(), went_live.wrapping_add(1));
    let got = tap.read_newest(256, &mut out);
    assert_eq!(got.frames, 0);
    assert!(out.iter().all(|&s| s == 0.0));
}

#[test]
fn an_endpoint_is_named_by_port_and_an_engine_that_is_not_there_is_not_found() {
    let ep = Client::default_endpoint(57_110);
    assert!(ep.contains("57110"), "{ep}");
    assert_ne!(Client::default_endpoint(57_111), ep);
    let secret = [7u8; 32];
    assert_eq!(Client::open_shm(&ep, &secret).err(), Some(Error::NotFound));
    assert_eq!(Client::open_shm("bad\0endpoint", &secret).err(), Some(Error::Arg));
}

/// A table of anything, read from the engine's memory with no message sent: these
/// are a machine's facts, to show that nothing in the table is about music.
#[test]
fn a_table_of_anything_is_read_directly_from_the_engines_memory() {
    use clockwork_client::params::*;
    use clockwork_osc::OscArg;

    const PUMP: u32 = 10;
    const TEMPERATURE: u32 = 11;
    const SPEED: u32 = 12;
    const MODE: u32 = 13;
    let custom = CLOCKWORK_PARAM_UNIT_CUSTOM;
    let mut d = Define::new();
    d.add(ClockworkParamEntry::new_group(PUMP, "Pump"), &[Say::Doc("The coolant pump")]);
    d.add(ClockworkParamEntry::new(TEMPERATURE, "Temp", CLOCKWORK_PARAM_CONTINUOUS, -40.0, 125.0, 20.0)
              .unit(custom).flags(CLOCKWORK_PARAM_READ_ONLY).group(PUMP),
          &[Say::FullName("Coolant temperature"), Say::Unit("°C")]);
    d.add(ClockworkParamEntry::new(SPEED, "Speed", CLOCKWORK_PARAM_CONTINUOUS, 100.0, 10000.0, 1000.0)
              .unit(custom).taper(CLOCKWORK_PARAM_TAPER_LOG).group(PUMP),
          &[Say::Unit("rpm"), Say::Doc("How fast it turns")]);
    d.add(ClockworkParamEntry::new(MODE, "Mode", CLOCKWORK_PARAM_CHOICE, 0.0, 2.0, 0.0).group(PUMP),
          &[Say::Value("Idle"), Say::Value("Run"), Say::Value("Fault")]);

    let e = &mut engine();
    settle(e);
    e.send(&clockwork_osc::encode("/clockwork/param/define", &[
        OscArg::Blob(d.entry_bytes().to_vec()),
        OscArg::Blob(d.text().to_vec()),
    ]), ORIGIN).unwrap();
    render_blocks(e, 1);

    let moved_before;
    {
        let mut table = e.client().params().expect("a table");
        assert!(table.refresh());
        assert_eq!(table.params().len(), 4);
        let temperature = table.find(TEMPERATURE).unwrap();
        assert_eq!(temperature.full_name(), "Coolant temperature");
        assert_eq!(temperature.text_of(table.value(temperature.slot())), "20 °C");
        let speed = table.find(SPEED).unwrap();
        assert_eq!(speed.doc(), Some("How fast it turns"));
        assert!((speed.position_of(1000.0) - 0.5).abs() < 1e-5, "a decade of a two-decade sweep");
        assert_eq!(table.find(MODE).unwrap().text_of(table.value(table.find(MODE).unwrap().slot())), "Idle");
        assert_eq!(table.find(PUMP).unwrap().doc(), Some("The coolant pump"));
        moved_before = e.client().metrics().get(Metric::ParamMoves).unwrap();
    }

    // A set moves the value in memory: a reader sees it with no message, and the
    // facts it holds still stand.
    e.send(&clockwork_osc::encode("/clockwork/param/set", &[
        OscArg::Int(MODE as i32), OscArg::Float(1.0),
    ]), ORIGIN).unwrap();
    render_blocks(e, 1);
    let client = e.client();
    let mut table = client.params().unwrap();
    assert!(table.refresh());
    let generation = table.generation();
    let mode = table.find(MODE).unwrap().clone();
    assert_eq!(mode.text_of(table.value(mode.slot())), "Run");
    assert!(!table.refresh());
    assert_eq!(table.generation(), generation);
    let metrics = client.metrics();
    assert_eq!(metrics.get(Metric::ParamMoves), Some(moved_before + 1));
    assert_eq!(metrics.get(Metric::ParamsLive), Some(4));
    assert_eq!(metrics.get(Metric::ParamsGeneration), Some(generation));
}
