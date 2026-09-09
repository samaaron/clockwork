// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Streaming a WAV file into a source port.
//!
//! A worker thread, a `WavReader`, and `clockwork_port_produce`. That is the whole
//! endpoint — which is the point of the substrate: nothing here knows about
//! rings, slots, underruns or the audio thread, and nothing in the substrate
//! knows about files.
//!
//! # On a slow disk
//!
//! Nothing waits for it. The worker asks the port how much room there is,
//! reads that much (capped), and offers it; if the read takes 40 ms the ring
//! simply drains in the meantime, the audio thread reads silence, and the
//! underrun counter says by how much. The disk never reaches the audio thread,
//! and the ring depth chosen at `clockwork_port_open` is exactly how much slowness
//! the stream can absorb before it is audible.
//!
//! The worker polls rather than waiting on a condition variable, because being
//! signalled would mean the audio thread signalling it — a syscall on the
//! audio thread to save a millisecond of latency on a path whose latency is
//! already a ring's depth. It sleeps only when the ring is full or the file is
//! done.
//!
//! # Channel and rate mismatch
//!
//! Channels are the substrate's rule applied one layer up: a file with fewer
//! channels than the port leaves the extra port channels silent; one with more
//! has the extras discarded. Rate is NOT converted — a 44.1 kHz file into a
//! 48 kHz session plays fast, and that is `docs/PORTS.md` open question 2,
//! not something this endpoint should decide on its own.

use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use portable_atomic::AtomicU64;
use std::sync::Arc;
use std::thread::JoinHandle;

use clockwork_ports::registry as ports;

use crate::wav::WavReader;

/// Frames per disk read. Big enough that the syscall is not the cost, small
/// enough that one read cannot monopolise a shallow ring.
const CHUNK_FRAMES: usize = 4096;
/// How long to wait when there is nothing useful to do.
const IDLE: std::time::Duration = std::time::Duration::from_millis(2);

pub struct DiskSource {
    stop: Arc<AtomicBool>,
    eof: Arc<AtomicBool>,
    produced: Arc<AtomicU64>,
    total_frames: u64,
    sample_rate: u32,
    file_channels: u16,
    worker: Option<JoinHandle<()>>,
}

impl DiskSource {
    pub fn open(path: &Path, port: u32) -> std::io::Result<DiskSource> {
        let port_channels = ports::channels(port);
        if port_channels == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "port is not open".to_string(),
            ));
        }
        // Opened here, on the calling (control) thread, so a missing or
        // malformed file is an error the caller gets back rather than a
        // silence it has to diagnose later.
        let reader = WavReader::open(path)?;
        let total_frames = reader.total_frames;
        let sample_rate = reader.sample_rate;
        let file_channels = reader.channels;

        let stop = Arc::new(AtomicBool::new(false));
        let eof = Arc::new(AtomicBool::new(false));
        let produced = Arc::new(AtomicU64::new(0));

        let worker = {
            let stop = Arc::clone(&stop);
            let eof = Arc::clone(&eof);
            let produced = Arc::clone(&produced);
            std::thread::Builder::new()
                .name("clockwork-disk-source".into())
                .spawn(move || run(reader, port, port_channels as usize, stop, eof, produced))?
        };

        Ok(DiskSource {
            stop, eof, produced, total_frames, sample_rate, file_channels,
            worker: Some(worker),
        })
    }

    pub fn total_frames(&self) -> u64 { self.total_frames }
    pub fn sample_rate(&self) -> u32 { self.sample_rate }
    pub fn file_channels(&self) -> u16 { self.file_channels }
    pub fn frames_produced(&self) -> u64 { self.produced.load(Ordering::Relaxed) }
    /// True once every frame in the file has been handed to the port. The port
    /// then underruns to silence, which is the correct end of a stream.
    pub fn at_eof(&self) -> bool { self.eof.load(Ordering::Acquire) }
}

impl Drop for DiskSource {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(w) = self.worker.take() { let _ = w.join(); }
    }
}

fn run(
    mut reader: WavReader, port: u32, port_channels: usize,
    stop: Arc<AtomicBool>, eof: Arc<AtomicBool>, produced: Arc<AtomicU64>,
) {
    let file_channels = reader.channels as usize;
    let mut from_file = vec![0.0f32; CHUNK_FRAMES * file_channels];
    let mut to_port = vec![0.0f32; CHUNK_FRAMES * port_channels];

    while !stop.load(Ordering::Acquire) {
        if eof.load(Ordering::Acquire) { std::thread::sleep(IDLE); continue; }

        let room = ports::writable(port) as usize;
        if room == 0 { std::thread::sleep(IDLE); continue; }
        let want = core::cmp::min(room, CHUNK_FRAMES);

        let got = match reader.read_frames(&mut from_file, want) {
            Ok(n) => n,
            // A read error ends the stream the same way EOF does: silence,
            // counted as underruns by the port. Nothing here can usefully
            // recover, and stalling the worker would look identical.
            Err(_) => { eof.store(true, Ordering::Release); continue; }
        };
        if got == 0 { eof.store(true, Ordering::Release); continue; }

        // File layout to port layout. Same rule as the substrate's, one layer
        // up: missing channels are silence, extra ones are discarded.
        if file_channels == port_channels {
            to_port[..got * port_channels].copy_from_slice(&from_file[..got * file_channels]);
        } else {
            for f in 0..got {
                for c in 0..port_channels {
                    to_port[f * port_channels + c] = if c < file_channels {
                        from_file[f * file_channels + c]
                    } else {
                        0.0
                    };
                }
            }
        }

        // Keep the remainder and try again, exactly as clockwork_ports.h says: a
        // full ring means the audio thread has not got here yet, and dropping
        // would lose part of the file for no reason.
        let mut done = 0usize;
        while done < got {
            if stop.load(Ordering::Acquire) { return; }
            let took = ports::produce(port, &to_port[done * port_channels..got * port_channels]) as usize;
            if took == 0 { std::thread::sleep(IDLE); continue; }
            done += took;
            produced.fetch_add(took as u64, Ordering::Relaxed);
        }
    }
}
