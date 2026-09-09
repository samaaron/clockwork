// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The boot door: an engine on the audio device, ticking itself. Its own
//! test binary, because one engine per process and engine.rs attaches.
//!
//! A machine with no audio device (a CI runner) still boots — the device
//! layer opens what it can — so what is asserted about the device is
//! asserted only when one opened.

extern crate clockwork_native;
extern crate clockwork_sys;

use std::time::Duration;

use clockwork_client::{Embed, EmbedConfig, Error};

#[test]
fn boot_ticks_itself_and_reports_the_device_it_opened() {
    let cfg = EmbedConfig::new().app_name("clockwork-client test").no_input(true);
    let mut e = match Embed::boot(&cfg) {
        Ok(e) => e,
        Err(Error::Absent) => {
            eprintln!("this build has no device layer; boot not tested");
            return;
        }
        Err(e) => panic!("boot refused: {e}"),
    };
    // One engine per process.
    assert_eq!(Embed::boot(&cfg).err(), Some(Error::Perm));
    assert_eq!(Embed::attach(&EmbedConfig::new().sample_rate(48_000.0).output_channels(2)).err(), Some(Error::Perm));

    // A booted engine renders itself: a host's render is answered with 0.
    let (mut l, mut r) = (vec![0.0f32; 64], vec![0.0f32; 64]);
    assert_eq!(e.render(&mut [l.as_mut_slice(), r.as_mut_slice()], &[], 64), 0);
    assert!(e.sample_rate() > 0.0);
    assert!(e.block_size() > 0);
    let info = e.info().unwrap();
    assert_eq!(info.sample_rate, e.sample_rate());

    match e.device() {
        Ok(d) if !d.driver.is_empty() => {
            eprintln!("booted on {} : {} at {} Hz, {}-frame callback", d.driver, d.device, d.sample_rate, d.buffer_frames);
            assert!(!d.device.is_empty());
            assert_eq!(d.sample_rate, e.sample_rate());
            assert_eq!(d.block_frames, e.block_size());
            assert_eq!(d.input_channels, 0, "NO_INPUT opened no input");
            assert!(d.output_channels > 0);
            assert!(d.buffer_frames > 0);
            // The device callback ticks the engine, so a ping is answered
            // with nobody rendering.
            e.drain();
            e.send(&clockwork_osc::encode("/dummy/ping", &[]), 0x424f_4f54).unwrap();
            let got = e.poll_until(Duration::from_secs(5), |m| {
                (m.address() == "/dummy/pong").then_some(m.origin)
            });
            assert_eq!(got, Some(0x424f_4f54));
            let stats = e.native_stats().unwrap();
            assert_eq!(stats.overruns(), 0, "{stats:?}");
        }
        other => eprintln!("no device opened here ({other:?}); device read-back not tested"),
    }
}
