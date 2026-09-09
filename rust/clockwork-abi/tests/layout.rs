// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The declarations against the headers. A C probe (tests/probe.c) is
//! compiled against src/*.h by the system C compiler and prints every
//! struct's size and every field's offset; each line is compared with the
//! Rust side. A header that moves without this crate moving with it fails
//! here, on the host, before any guest reads a field through the wrong
//! pointer on the audio thread.
//!
//! Host only: it needs a C compiler on PATH (`cc`, or `$CC`). Skipped with a
//! message, not passed, when there is none.

use std::mem::{offset_of, size_of};
use std::path::PathBuf;
use std::process::Command;

use clockwork_abi::client::*;
use clockwork_abi::dsp_api::*;
use clockwork_abi::sink::*;
use clockwork_abi::lanes;
use clockwork_abi::arena::*;
use clockwork_abi::embed::*;

macro_rules! expect {
    ($m:ident, $T:ty { $($f:ident),* $(,)? }) => {{
        $m.push((format!("size {}", stringify!($T)), size_of::<$T>()));
        $( $m.push((format!("field {} {}", stringify!($T), stringify!($f)), offset_of!($T, $f))); )*
    }};
}

#[test]
fn every_struct_is_the_header_s_size_and_every_field_is_at_the_header_s_offset() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let src = root.join("../../src");
    let probe = root.join("tests/probe.c");
    let out = std::env::temp_dir().join(format!("clockwork-abi-probe-{}", std::process::id()));
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".into());
    let compiled = Command::new(&cc)
        .arg("-std=c11").arg("-I").arg(&src).arg("-o").arg(&out).arg(&probe)
        .status();
    let Ok(status) = compiled else {
        eprintln!("no C compiler ({cc}) on PATH — the layout probe did not run");
        return;
    };
    assert!(status.success(), "the probe did not compile against the headers");
    let output = Command::new(&out).output().expect("run the probe");
    let _ = std::fs::remove_file(&out);
    let text = String::from_utf8(output.stdout).unwrap();

    let mut mine: Vec<(String, usize)> = Vec::new();
    expect!(mine, DspConfig {
        sample_rate, block_size, max_input_channels, max_output_channels, clock, channel_map,
        shm_window, shm_window_bytes, persistent, persistent_bytes, arena, arena_bytes,
        arena_bulk, arena_bulk_bytes, inbox, inbox_bytes, outbox, outbox_bytes,
        guest_config, guest_config_bytes, deterministic_seed, fp_env,
    });
    expect!(mine, DspHost { ctx, emit_osc, log, open_sink, send_sink, free_bytes, asset_release });
    expect!(mine, DspInfo {
        name, version, holds_schedule, arena_bytes_wanted, arena_bulk_bytes_wanted, wants_events,
    });
    expect!(mine, ClockworkAsset {
        struct_bytes, id, kind, origin, bytes, byte_count, channels, frames, sample_rate,
    });
    expect!(mine, ClockworkClientInfo {
        struct_bytes, abi_version, engine_version, features, sample_rate, block_frames,
        input_channels, output_channels,
    });
    expect!(mine, ClockworkClientMessage { bytes, length, origin, route, sequence });
    expect!(mine, ClockworkRegion { base, bytes, writable });
    expect!(mine, ClockworkClientClock {
        struct_bytes, bpm, beat_origin_ntp, is_playing_at_ntp, is_playing, flags, meter_num, meter_den,
    });
    expect!(mine, ClockworkScopeReader { struct_bytes, slot, _internal, _ring_frames });
    expect!(mine, ClockworkSinkStats { sent, dropped, late, scheduled, cancelled });
    expect!(mine, ClockworkStatus {});
    expect!(mine, ClockworkRegionId {});
    expect!(mine, ClockworkSinkKind {});
    expect!(mine, ClockworkSink {});
    expect!(mine, ClockworkEmbedConfig {
        struct_bytes, sample_rate, block_size, input_channels, output_channels, max_render_frames,
        app_name, device, instance_id, flags, driver,
    });
    expect!(mine, ClockworkEmbedDevice {
        struct_bytes, driver, device, sample_rate, buffer_frames, block_frames,
        input_channels, output_channels, output_latency_frames, input_latency_frames,
    });
    expect!(mine, ClockworkArenaEntry { id, offset, bytes, owner, geom });
    expect!(mine, ClockworkArenaHeader {
        magic, version, header_bytes, instance_id, arena_bytes, block_bytes, guest_offset,
        guest_bytes, entry_count, entry_bytes, state, reserved, entries,
    });
    let consts: [(&str, i64); 23] = [
        ("CLOCKWORK_E_NOMEM", ClockworkStatus::E_NOMEM.0 as i64),
        ("CLOCKWORK_REGION_EGRESS", ClockworkRegionId::EGRESS.0 as i64),
        ("CLOCKWORK_REGION_NATIVE_STATS", ClockworkRegionId::NATIVE_STATS.0 as i64),
        ("kClockworkSinkOsc", ClockworkSinkKind::OSC.0 as i64),
        ("CLOCKWORK_FEATURE_SCHEDULER", CLOCKWORK_FEATURE_SCHEDULER as i64),
        ("CLOCKWORK_ROUTE_NOTIFY", CLOCKWORK_ROUTE_NOTIFY as i64),
        ("CLOCKWORK_ASSET_AUDIO_F32", CLOCKWORK_ASSET_AUDIO_F32 as i64),
        ("DSP_FP_ENV_FLUSH_TO_ZERO", DSP_FP_ENV_FLUSH_TO_ZERO as i64),
        ("CLOCKWORK_CLIENT_ABI_VERSION", CLOCKWORK_CLIENT_ABI_VERSION as i64),
        ("CLOCKWORK_ATTACH_REFUSED", lanes::CLOCKWORK_ATTACH_REFUSED as i64),
        ("CLOCKWORK_ATTACH_BOOTED", lanes::CLOCKWORK_ATTACH_BOOTED as i64),
        ("CLOCKWORK_ATTACH_JOINED", lanes::CLOCKWORK_ATTACH_JOINED as i64),
        ("CLOCKWORK_RESERVED_LANES_MAX", lanes::CLOCKWORK_RESERVED_LANES_MAX as i64),
        ("CLOCKWORK_ARENA_MAGIC", CLOCKWORK_ARENA_MAGIC as i64),
        ("CLOCKWORK_ARENA_VERSION", CLOCKWORK_ARENA_VERSION as i64),
        ("CLOCKWORK_ARENA_HEADER_BYTES", CLOCKWORK_ARENA_HEADER_BYTES as i64),
        ("CLOCKWORK_ARENA_MAX_ENTRIES", CLOCKWORK_ARENA_MAX_ENTRIES as i64),
        ("CLOCKWORK_ARENA_SCOPE", ClockworkArenaRegion::SCOPE.0 as i64),
        ("CLOCKWORK_OWNER_HOST", ClockworkArenaOwner::HOST.0 as i64),
        ("CLOCKWORK_GEOM_SLOTS_STACK_BYTES", geom::SLOTS_STACK_BYTES as i64),
        ("CLOCKWORK_ARENA_TRACK_TAPS", ClockworkArenaRegion::TRACK_TAPS.0 as i64),
        ("CLOCKWORK_GEOM_TRACK_FIRST_INDEX", geom::TRACK_FIRST_INDEX as i64),
        ("CLOCKWORK_TAP_IN", CLOCKWORK_TAP_IN as i64),
    ];

    // Every line the probe printed must match, and every expectation must
    // have been printed: a field missing from either side is a failure.
    let mut seen = 0;
    for line in text.lines() {
        let parts: Vec<&str> = line.split(' ').collect();
        match parts.as_slice() {
            ["const", name, value] => {
                let (_, mine) = consts.iter().find(|(n, _)| n == name)
                    .unwrap_or_else(|| panic!("the probe printed a constant this crate does not declare: {name}"));
                assert_eq!(*mine, value.parse::<i64>().unwrap(), "{name}");
                seen += 1;
            }
            _ => {
                let key = parts[..parts.len() - 1].join(" ");
                let value: usize = parts[parts.len() - 1].parse().unwrap();
                let (_, mine) = mine.iter().find(|(k, _)| *k == key)
                    .unwrap_or_else(|| panic!("the header has something this crate does not declare: {key}"));
                assert_eq!(*mine, value, "{key}: Rust says {mine}, the header says {value}");
                seen += 1;
            }
        }
    }
    assert_eq!(seen, mine.len() + consts.len(),
        "this crate declares something the probe did not print — add it to tests/probe.c");
}
