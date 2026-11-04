// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Draining a sink port to a WAV file.
//!
//! The mirror of the source: a worker thread, `clockwork_port_consume`, and a
//! `WavWriter` whose header is patched with the real length on close.
//!
//! # On a slow disk
//!
//! Again, nothing waits. If the write blocks, the ring fills; the audio
//! thread's `clockwork_port_write` then takes what fits and drops the rest,
//! counting it, and carries on at the same cadence. A recording made over a
//! disk that could not keep up is short by exactly the overrun count, which is
//! a number you can look at — as against the alternative, where the audio
//! thread waits on a filesystem and the whole session glitches.
//!
//! The worker wakes every quarter of the ring's duration at the session rate
//! and writes all that has arrived ([`crate::pace`]), so a recording reaches
//! the file within a quarter ring.
//!
//! The final drain matters: on close the worker keeps consuming until the ring
//! is empty before it patches the header, so the last partial buffer is in the
//! file rather than lost to the stop flag.

use std::path::Path;
use std::sync::atomic::Ordering;
use portable_atomic::AtomicU64;
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Instant;

use clockwork_ports::registry::Ports;

use crate::pace::{self, Stop};
use crate::Feed;
use crate::wav::WavWriter;

const CHUNK_FRAMES: usize = 4096;

pub struct DiskSink {
    stop: Arc<Stop>,
    written: Arc<AtomicU64>,
    worker: Option<JoinHandle<()>>,
}

impl DiskSink {
    /// `sample_rate` is the file's and the session's: the audio thread writes
    /// the port at it, which paces the worker.
    /// `port` is a sink in `ports`, which the worker keeps while it runs.
    pub fn open(path: &Path, ports: Arc<Ports>, port: u32, sample_rate: u32, channels: u16)
        -> std::io::Result<DiskSink>
    {
        let port_channels = ports.channels(port);
        if port_channels == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "port is not open".to_string(),
            ));
        }
        if sample_rate == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "a sample rate of 0".to_string(),
            ));
        }
        // The file is written with the channel count the caller asked for; the
        // port's count is what actually arrives. Mismatches are handled the
        // same way everywhere else here does.
        let writer = WavWriter::create(path, sample_rate, channels)?;

        let stop = Arc::new(Stop::default());
        let written = Arc::new(AtomicU64::new(0));
        let worker = {
            let stop = Arc::clone(&stop);
            let written = Arc::clone(&written);
            std::thread::Builder::new()
                .name("cw-disk-sink".into())
                .spawn(move || {
                    let feed = Feed { ports: &ports, port, channels: port_channels as usize };
                    run(writer, feed, channels as usize, sample_rate, &stop, &written)
                })?
        };
        Ok(DiskSink { stop, written, worker: Some(worker) })
    }

    pub fn frames_written(&self) -> u64 { self.written.load(Ordering::Relaxed) }
}

impl Drop for DiskSink {
    fn drop(&mut self) {
        self.stop.set();
        if let Some(w) = self.worker.take() { let _ = w.join(); }
    }
}

fn run(
    mut writer: WavWriter, feed: Feed<'_>, file_channels: usize,
    sample_rate: u32, stop: &Stop, written: &AtomicU64,
) {
    let Feed { ports, port, channels: port_channels } = feed;
    let mut from_port = vec![0.0f32; CHUNK_FRAMES * port_channels];
    let mut to_file = vec![0.0f32; CHUNK_FRAMES * file_channels];
    let tick = pace::quarter_ring(ports.capacity(port), sample_rate);

    'worker: loop {
        // Read the stop BEFORE the drain. The audio thread's last write came
        // before the close that set it, so a drain that starts after seeing
        // the stop takes the tail of the recording too.
        let stopping = stop.is_set();

        // Write all that has arrived.
        loop {
            let avail = ports.readable(port) as usize;
            if avail == 0 { break; }
            let want = core::cmp::min(avail, CHUNK_FRAMES);
            let got = ports.consume(port, &mut from_port[..want * port_channels]) as usize;
            if got == 0 { break; }

            let data: &[f32] = if file_channels == port_channels {
                &from_port[..got * port_channels]
            } else {
                for f in 0..got {
                    for c in 0..file_channels {
                        to_file[f * file_channels + c] = if c < port_channels {
                            from_port[f * port_channels + c]
                        } else {
                            0.0
                        };
                    }
                }
                &to_file[..got * file_channels]
            };
            if writer.write_frames(data, got).is_err() {
                // The disk is gone. Stop writing; the port keeps counting
                // overruns, which is what a caller will look at.
                break 'worker;
            }
            written.store(writer.frames(), Ordering::Relaxed);
        }

        if stopping || ports.channels(port) == 0 { break; }
        stop.sleep_until(Some(Instant::now() + tick));
    }
    let _ = writer.finish();
    written.store(writer.frames(), Ordering::Relaxed);
}
