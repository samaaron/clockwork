// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The conversions here against `clockwork-midi`'s schema, which the device
//! subsystem and the web's page convert with: the same bytes in, the same
//! bytes out, for every short message there is.

use clockwork_midi::schema::{decode_out, encode_in_at, encode_ports, encode_ports_reply, OutCommand};
use clockwork_midi::{MidiMessage, OscArg};
use clockwork_midi_wire as wire;

/// Every channel message: each status, with data bytes at their ends and
/// between.
fn channel_messages() -> Vec<Vec<u8>> {
    let mut all = Vec::new();
    for status in 0x80..0xF0u8 {
        let two = !matches!(status & 0xf0, 0xC0 | 0xD0);
        for a in [0u8, 1, 60, 64, 127] {
            if two {
                for b in [0u8, 1, 64, 100, 127] {
                    all.push(vec![status, a, b]);
                }
            } else {
                all.push(vec![status, a]);
            }
        }
    }
    all
}

#[test]
fn a_message_in_is_the_schemas_bytes() {
    let mut out = [0u8; 128];
    for midi in channel_messages() {
        for when in [0u64, 0xE8F3_1234_8000_0000] {
            let msg = MidiMessage::parse(&midi).expect("a channel message");
            let want = encode_in_at("daw", &msg, when).expect("the schema encodes it");
            let n = wire::encode_in("daw", &midi, when, &mut out).unwrap_or_else(|| panic!("{midi:02x?} not encoded"));
            assert_eq!(&out[..n], &want[..], "{midi:02x?} at {when:#x}");
        }
    }
}

#[test]
fn a_port_name_of_any_length_is_padded_as_the_schema_pads_it() {
    let mut out = [0u8; 128];
    for port in ["a", "ab", "abc", "abcd", "abcde", "iac_driver_bus_1"] {
        let msg = MidiMessage::parse(&[0x93, 60, 100]).unwrap();
        let want = encode_in_at(port, &msg, 0).unwrap();
        let n = wire::encode_in(port, &[0x93, 60, 100], 0, &mut out).unwrap();
        assert_eq!(&out[..n], &want[..], "{port}");
    }
}

#[test]
fn what_is_not_one_whole_channel_message_is_not_encoded() {
    let mut out = [0u8; 128];
    for midi in [&[][..], &[60, 100], &[0x90], &[0x90, 60], &[0xC0], &[0xF0, 1, 2, 0xF7], &[0xF8], &[0xFA]] {
        assert_eq!(wire::encode_in("daw", midi, 0, &mut out), None, "{midi:02x?}");
    }
    assert_eq!(wire::encode_in("d\0w", &[0x90, 60, 100], 0, &mut out), None);
}

#[test]
fn a_buffer_too_small_is_refused_and_one_exactly_big_enough_is_filled() {
    let mut big = [0u8; 128];
    let n = wire::encode_in("daw", &[0x90, 60, 100], 7, &mut big).unwrap();
    for cap in 0..n {
        let mut small = vec![0u8; cap];
        assert_eq!(wire::encode_in("daw", &[0x90, 60, 100], 7, &mut small), None, "{cap} bytes");
    }
    let mut exact = vec![0u8; n];
    assert_eq!(wire::encode_in("daw", &[0x90, 60, 100], 7, &mut exact), Some(n));
    assert_eq!(&exact[..], &big[..n]);
}

fn osc(addr: &str, args: &[OscArg]) -> Vec<u8> {
    clockwork_midi::osc::encode(addr, args)
}
fn s(v: &str) -> OscArg {
    OscArg::Str(v.to_string())
}

/// The schema's reading of a send, as the bytes that go to the port and
/// whether they go to every channel.
fn schema_bytes(data: &[u8]) -> Option<(String, Vec<u8>, bool, u64)> {
    match decode_out(data).ok()? {
        OutCommand::Send { port, msg, when } => {
            let every = msg.channel() == Some(0);
            let mut bytes = msg.encode();
            if every {
                bytes[0] &= 0xf0; // encode() clamps the sentinel to channel 1
            }
            Some((port, bytes, every, when))
        }
        OutCommand::SendRaw { port, bytes, when } => Some((port, bytes, false, when)),
        _ => None,
    }
}

#[test]
fn a_send_out_is_the_schemas_bytes() {
    let two = ["note_on", "note_off", "poly_pressure", "control_change"];
    let one = ["program_change", "channel_pressure", "pitch_bend"];
    let mut sends = Vec::new();
    for channel in [-1, 0, 1, 2, 10, 16, 17, 200] {
        for kind in two {
            for (a, b) in [(0, 0), (60, 100), (127, 127), (128, 255), (-1, 300)] {
                sends.push(osc(&format!("/clockwork/midi/out/{kind}"), &[s("daw"), OscArg::Int(channel), OscArg::Int(a), OscArg::Int(b)]));
            }
        }
        for kind in one {
            for a in [0, 1, 64, 127, 8192, 16383, 20000, -5] {
                sends.push(osc(&format!("/clockwork/midi/out/{kind}"), &[s("daw"), OscArg::Int(channel), OscArg::Int(a)]));
            }
        }
    }
    for kind in ["clock", "start", "continue", "stop"] {
        sends.push(osc(&format!("/clockwork/midi/out/{kind}"), &[s("*")]));
    }
    sends.push(osc("/clockwork/midi/out/raw", &[s("daw"), OscArg::Int(0x90), OscArg::Int(60), OscArg::Int(100)]));
    sends.push(osc("/clockwork/midi/out/raw", &[s("daw"), OscArg::Int(0xF8)]));
    // Numbers as a sender may write them, and a time at the end.
    sends.push(osc("/clockwork/midi/out/note_on", &[s("daw"), OscArg::Float(1.0), OscArg::Float(60.0), OscArg::Float(100.0)]));
    sends.push(osc("/clockwork/midi/out/note_on", &[s("daw"), OscArg::Int(1), OscArg::Int(60), OscArg::Int(100), OscArg::TimeTag(0xE8F3_1234_8000_0000)]));
    sends.push(osc("/clockwork/midi/out/stop", &[s("daw"), OscArg::TimeTag(99)]));

    for data in &sends {
        let (port, bytes, every, when) = schema_bytes(data).expect("the schema reads it");
        let got = wire::decode_out(data).unwrap_or_else(|| panic!("not read: {:?}", clockwork_midi::osc::decode(data)));
        assert_eq!(got.port, port);
        assert_eq!(&got.bytes[..got.len], &bytes[..], "{:?}", clockwork_midi::osc::decode(data));
        assert_eq!(got.every_channel, every, "{:?}", clockwork_midi::osc::decode(data));
        assert_eq!(got.when, when);
    }
}

#[test]
fn what_is_not_a_short_send_is_not_read() {
    let not_sends = [
        osc("/clockwork/midi/out/enable", &[s("daw"), OscArg::Int(1)]),
        osc("/clockwork/midi/out/sysex", &[s("daw"), OscArg::Blob(vec![0xF0, 1, 0xF7])]),
        osc("/clockwork/midi/out/raw", &[s("daw"), OscArg::Blob(vec![0x90, 60, 100])]),
        osc("/clockwork/midi/out/raw", &[s("daw"), OscArg::Int(0xF0), OscArg::Int(1), OscArg::Int(2), OscArg::Int(0xF7)]),
        osc("/clockwork/midi/out/raw", &[s("daw")]),
        osc("/clockwork/midi/out/note_on", &[s("daw"), OscArg::Int(1), OscArg::Int(60)]),
        osc("/clockwork/midi/out/note_on", &[OscArg::Int(1), OscArg::Int(60), OscArg::Int(100)]),
        osc("/clockwork/midi/out/nothing", &[s("daw")]),
        osc("/clockwork/midi/in/note_on", &[s("daw"), OscArg::Int(1), OscArg::Int(60), OscArg::Int(100)]),
        osc("/clockwork/midi/ports/list", &[]),
    ];
    for data in &not_sends {
        assert_eq!(wire::decode_out(data), None, "{:?}", clockwork_midi::osc::decode(data));
    }
}

#[test]
fn bytes_that_are_no_message_are_not_read_and_nothing_is_read_past_their_end() {
    let whole = osc("/clockwork/midi/out/note_on", &[s("daw"), OscArg::Int(1), OscArg::Int(60), OscArg::Int(100)]);
    for cut in 0..whole.len() {
        assert_eq!(wire::decode_out(&whole[..cut]), None, "cut at {cut}");
    }
    assert_eq!(wire::decode_out(b"/clockwork/midi/out/note_on"), None);
    let mut no_tags = whole.clone();
    no_tags[28] = b'x'; // the type tags' comma
    assert_eq!(wire::decode_out(&no_tags), None);
    let blob = osc("/clockwork/midi/out/stop", &[s("daw"), OscArg::Blob(vec![1, 2, 3])]);
    let mut long_blob = blob.clone();
    let at = blob.len() - 8;
    long_blob[at..at + 4].copy_from_slice(&u32::MAX.to_be_bytes());
    assert_eq!(wire::decode_out(&long_blob), None);
}

#[test]
fn a_hosts_ports_are_the_schemas_port_list() {
    let on = |n: &str| vec![(n.to_string(), true)];
    let mut out = [0u8; 128];
    let cases: [(Option<&str>, Option<&str>); 4] = [(Some("daw"), Some("daw")), (Some("in"), None), (None, Some("out")), (None, None)];
    for (input, output) in cases {
        let ins = input.map(on).unwrap_or_default();
        let outs = output.map(on).unwrap_or_default();
        let n = wire::encode_ports(input, output, true, &mut out).unwrap();
        assert_eq!(&out[..n], &encode_ports_reply(&ins, &outs)[..], "{input:?} {output:?}");
        let n = wire::encode_ports(input, output, false, &mut out).unwrap();
        assert_eq!(&out[..n], &encode_ports(&ins, &outs)[..], "{input:?} {output:?}");
    }
    assert_eq!(wire::encode_ports(Some("daw"), Some("daw"), true, &mut [0u8; 20]), None);
}

/// Every file of the device subsystem's corpus, through both: where both read
/// a send, it is the same send; where both read a port's message, the same
/// event.
#[test]
fn the_corpus_reads_the_same_as_the_schema() {
    let midi = concat!(env!("CARGO_MANIFEST_DIR"), "/../clockwork-midi");
    let mut sends = 0;
    let n = clockwork_corpus::replay_with(midi, |data| {
        if let (Some(got), Some((port, bytes, every, when))) = (wire::decode_out(data), schema_bytes(data)) {
            if bytes.len() <= wire::MAX_MIDI_BYTES {
                assert_eq!((got.port, &got.bytes[..got.len], got.every_channel, got.when), (port.as_str(), &bytes[..], every, when));
                sends += 1;
            }
        }
        let mut out = [0u8; 160];
        if let (Some(n), Some(msg)) = (wire::encode_in("p", data, 0, &mut out), MidiMessage::parse(data)) {
            assert_eq!(Some(&out[..n]), encode_in_at("p", &msg, 0).as_deref());
        }
    });
    assert!(n > 0, "no corpus under rust/clockwork-midi/corpus");
    assert!(sends > 0, "the corpus held no send both read");
}
