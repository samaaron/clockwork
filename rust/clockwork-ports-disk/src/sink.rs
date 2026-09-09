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
//! The final drain matters: on close the worker keeps consuming until the ring
//! is empty before it patches the header, so the last partial buffer is in the
//! file rather than lost to the stop flag.

use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use portable_atomic::AtomicU64;
use std::sync::Arc;
use std::thread::JoinHandle;

use clockwork_ports::registry as ports;

use crate::wav::WavWriter;

const CHUNK_FRAMES: usize = 4096;
const IDLE: std::time::Duration = std::time::Duration::from_millis(2);

pub struct DiskSink {
    stop: Arc<AtomicBool>,
    written: Arc<AtomicU64>,
    worker: Option<JoinHandle<()>>,
}

impl DiskSink {
    pub fn open(path: &Path, port: u32, sample_rate: u32, channels: u16)
        -> std::io::Result<DiskSink>
    {
        let port_channels = ports::channels(port);
        if port_channels == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "port is not open".to_string(),
            ));
        }
        // The file is written with the channel count the caller asked for; the
        // port's count is what actually arrives. Mismatches are handled the
        // same way everywhere else here does.
        let writer = WavWriter::create(path, sample_rate, channels)?;

        let stop = Arc::new(AtomicBool::new(false));
        let written = Arc::new(AtomicU64::new(0));
        let worker = {
            let stop = Arc::clone(&stop);
            let written = Arc::clone(&written);
            std::thread::Builder::new()
                .name("clockwork-disk-sink".into())
                .spawn(move || {
                    run(writer, port, port_channels as usize, channels as usize,
                        stop, written)
                })?
        };
        Ok(DiskSink { stop, written, worker: Some(worker) })
    }

    pub fn frames_written(&self) -> u64 { self.written.load(Ordering::Relaxed) }
}

impl Drop for DiskSink {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(w) = self.worker.take() { let _ = w.join(); }
    }
}

fn run(
    mut writer: WavWriter, port: u32, port_channels: usize, file_channels: usize,
    stop: Arc<AtomicBool>, written: Arc<AtomicU64>,
) {
    let mut from_port = vec![0.0f32; CHUNK_FRAMES * port_channels];
    let mut to_file = vec![0.0f32; CHUNK_FRAMES * file_channels];

    loop {
        let avail = ports::readable(port) as usize;
        if avail == 0 {
            // Stop only once the ring is empty, so the tail of the recording
            // reaches the file. `avail` was read BEFORE this load, so a write
            // that landed in between is still waiting; seeing the stop flag
            // means the writer is done, so one more look settles it. Breaking
            // on the stale zero truncates the file by whatever that write
            // carried.
            if stop.load(Ordering::Acquire) {
                if ports::readable(port) == 0 { break; }
                continue;
            }
            std::thread::sleep(IDLE);
            continue;
        }
        let want = core::cmp::min(avail, CHUNK_FRAMES);
        let got = ports::consume(port, &mut from_port[..want * port_channels]) as usize;
        if got == 0 { continue; }

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
            break;
        }
        written.store(writer.frames(), Ordering::Relaxed);
    }
    let _ = writer.finish();
    written.store(writer.frames(), Ordering::Relaxed);
}
