// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The `/clockwork/midi/*` OSC schema — the single API surface, identical on native and
//! web. Inbound hardware MIDI is encoded to `/clockwork/midi/in/*`; client/VM requests
//! arrive as `/clockwork/midi/out/*`, `/clockwork/midi/clock/*` and device-management verbs and are
//! decoded to an [`OutCommand`].
//!
//! Channels are 1-based (1..=16); `channel == -1` on the wire means "all 16",
//! and `port == "*"` means "all enabled ports". Pitch bend is 14-bit (0..=16383).

use clockwork_osc::{clockwork_sys, Refusal};
use crate::message::MidiMessage;
use crate::osc::{self, OscArg};

/// A decoded client/VM request.
#[derive(Clone, Debug, PartialEq)]
pub enum OutCommand {
    /// Send a parsed channel-voice message to `port` (`"*"` = all) at `when`.
    ///
    /// `when` is an OSC timetag; 0 and 1 both mean now. A client that wants a
    /// note at a moment appends a `t` argument rather than wrapping the whole
    /// message in /clockwork/schedule — the timestamp belongs on the event, not
    /// in an envelope around it.
    Send { port: String, msg: MidiMessage, when: u64 },
    /// Send arbitrary/raw or sysex bytes to `port` at `when`.
    SendRaw { port: String, bytes: Vec<u8>, when: u64 },
    /// One immediate clock pulse. Continuous clock + transport + beat-bursts are
    /// generated engine-side (MidiClockOut); generated pulses also arrive here.
    ClockTick { port: String },
    /// Enable/disable using incoming MIDI clock on `port` as the ClockworkClock tempo source.
    ClockSync { port: String, enabled: bool },
    /// Open/close a specific port (`input` selects the in vs out list).
    Enable { port: String, input: bool, enabled: bool },
    PortsList,
    Refresh,
    Subscribe,
    Unsubscribe,
}

/// `channel == -1` expands to all channels for outbound messages.
pub const ALL_CHANNELS: i32 = -1;

fn port(args: &[OscArg]) -> Option<String> {
    args.first()?.as_str().map(|s| s.to_string())
}
fn i_at(args: &[OscArg], i: usize) -> Option<i32> {
    args.get(i)?.as_i32()
}

/// Channel arg for a channel-voice message: `-1` ([`ALL_CHANNELS`]) maps to the
/// `0` sentinel (fanned out to all 16 by the send path); 1..=16 pass through.
fn ch_at(args: &[OscArg], i: usize) -> Option<u8> {
    Some(match i_at(args, i)? {
        ALL_CHANNELS => 0,
        c => c as u8,
    })
}

/// A trailing OSC timetag, if the client sent one. Absent means now.
///
/// Scanned from the end rather than by index: the verbs have different arities
/// and a timetag is unambiguous — no other argument in this schema is a `t`.
fn trailing_when(args: &[OscArg]) -> u64 {
    match args.last() {
        Some(OscArg::TimeTag(t)) => *t,
        _ => 0,
    }
}

/// Decode an outbound `/clockwork/midi/*` OSC message into an [`OutCommand`],
/// or say why not: an address that is not one of the verbs is
/// [`Refusal::Unknown`], and a verb whose arguments cannot be read is
/// [`Refusal::Malformed`].
pub fn decode_out(data: &[u8]) -> Result<OutCommand, Refusal> {
    let cmd = decode_out_verb(data)?;
    let when = osc::decode(data).map(|m| trailing_when(&m.args)).unwrap_or(0);
    Ok(match cmd {
        OutCommand::Send { port, msg, .. } => OutCommand::Send { port, msg, when },
        OutCommand::SendRaw { port, bytes, .. } => OutCommand::SendRaw { port, bytes, when },
        other => other,
    })
}

// The match names the verbs, each with the reader for its arguments: an
// address it does not name is unknown, and a reader that cannot read what it
// was sent makes the verb malformed.
fn decode_out_verb(data: &[u8]) -> Result<OutCommand, Refusal> {
    let m = osc::decode(data).ok_or(Refusal::Malformed)?;
    let read: fn(&[OscArg]) -> Option<OutCommand> = match m.addr.as_str() {
        clockwork_sys!("midi/out/note_on") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::NoteOn {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                velocity: i_at(a, 3)? as u8,
            },
        }),
        clockwork_sys!("midi/out/note_off") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::NoteOff {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                velocity: i_at(a, 3)? as u8,
            },
        }),
        clockwork_sys!("midi/out/control_change") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::ControlChange {
                channel: ch_at(a, 1)?,
                controller: i_at(a, 2)? as u8,
                value: i_at(a, 3)? as u8,
            },
        }),
        clockwork_sys!("midi/out/program_change") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::ProgramChange {
                channel: ch_at(a, 1)?,
                program: i_at(a, 2)? as u8,
            },
        }),
        clockwork_sys!("midi/out/channel_pressure") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::ChannelPressure {
                channel: ch_at(a, 1)?,
                pressure: i_at(a, 2)? as u8,
            },
        }),
        clockwork_sys!("midi/out/poly_pressure") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::PolyPressure {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                pressure: i_at(a, 3)? as u8,
            },
        }),
        clockwork_sys!("midi/out/pitch_bend") => |a| Some(OutCommand::Send {
            port: port(a)?,
            when: 0,
            msg: MidiMessage::PitchBend {
                channel: ch_at(a, 1)?,
                value: (i_at(a, 2)?.clamp(0, 16383)) as u16,
            },
        }),
        // Single-byte system real-time sends (clock tick, start, stop,
        // continue): port only.
        clockwork_sys!("midi/out/clock") => |a| Some(OutCommand::Send { port: port(a)?, msg: MidiMessage::Clock, when: 0 }),
        clockwork_sys!("midi/out/start") => |a| Some(OutCommand::Send { port: port(a)?, msg: MidiMessage::Start, when: 0 }),
        clockwork_sys!("midi/out/continue") => |a| Some(OutCommand::Send { port: port(a)?, msg: MidiMessage::Continue, when: 0 }),
        clockwork_sys!("midi/out/stop") => |a| Some(OutCommand::Send { port: port(a)?, msg: MidiMessage::Stop, when: 0 }),

        // raw/sysex: port followed by either int bytes or a single blob.
        clockwork_sys!("midi/out/raw") | clockwork_sys!("midi/out/sysex") => |a| {
            let bytes = if let Some(OscArg::Blob(b)) = a.get(1) {
                b.clone()
            } else {
                a.get(1..)
                    .unwrap_or(&[])
                    .iter()
                    .filter_map(|x| x.as_i32())
                    .map(|v| (v & 0xff) as u8)
                    .collect()
            };
            Some(OutCommand::SendRaw { port: port(a)?, bytes, when: 0 })
        },

        // Continuous clock (start/stop/continue) + beat-bursts are handled
        // engine-side by MidiClockOut; the subsystem only sends single pulses.
        clockwork_sys!("midi/clock/tick") => |a| Some(OutCommand::ClockTick { port: port(a)? }),
        clockwork_sys!("midi/clock/sync") => |a| Some(OutCommand::ClockSync {
            port: port(a)?,
            enabled: i_at(a, 1)? != 0,
        }),

        clockwork_sys!("midi/in/enable") => |a| Some(OutCommand::Enable {
            port: port(a)?,
            input: true,
            enabled: i_at(a, 1)? != 0,
        }),
        clockwork_sys!("midi/out/enable") => |a| Some(OutCommand::Enable {
            port: port(a)?,
            input: false,
            enabled: i_at(a, 1)? != 0,
        }),

        clockwork_sys!("midi/ports/list") | clockwork_sys!("midi/ports/get") => |_| Some(OutCommand::PortsList),
        clockwork_sys!("midi/refresh") => |_| Some(OutCommand::Refresh),
        clockwork_sys!("midi/notify/subscribe") => |_| Some(OutCommand::Subscribe),
        clockwork_sys!("midi/notify/unsubscribe") => |_| Some(OutCommand::Unsubscribe),
        _ => return Err(Refusal::Unknown),
    };
    read(&m.args).ok_or(Refusal::Malformed)
}

/// Encode an inbound hardware message as a `/clockwork/midi/in/*` OSC packet for injection
/// into the engine ingress / broadcast to subscribers. Returns `None` for clock
/// pulses (`0xF8`), which are consumed by the BPM estimator instead.
pub fn encode_in(port: &str, msg: &MidiMessage) -> Option<Vec<u8>> {
    encode_in_at(port, msg, 0)
}

/// [`encode_in`] with the moment the bytes ARRIVED as a trailing `t`
/// argument — an OSC timetag; 0 leaves it off. The arrival time is the one
/// thing a host knows that the message does not carry, and a guest placing
/// the event on its own timeline wants it: a note that crossed a jittery
/// hop from the main thread is still the note that was played when it was
/// played. A reader scanning by index is unaffected by a trailing argument.
pub fn encode_in_at(port: &str, msg: &MidiMessage, when: u64) -> Option<Vec<u8>> {
    let s = |v: &str| OscArg::Str(v.to_string());
    let i = |v: i32| OscArg::Int(v);
    let (addr, args): (&str, Vec<OscArg>) = match msg {
        MidiMessage::NoteOn { channel, note, velocity } => (
            clockwork_sys!("midi/in/note_on"),
            vec![s(port), i(*channel as i32), i(*note as i32), i(*velocity as i32)],
        ),
        MidiMessage::NoteOff { channel, note, velocity } => (
            clockwork_sys!("midi/in/note_off"),
            vec![s(port), i(*channel as i32), i(*note as i32), i(*velocity as i32)],
        ),
        MidiMessage::ControlChange { channel, controller, value } => (
            clockwork_sys!("midi/in/control_change"),
            vec![s(port), i(*channel as i32), i(*controller as i32), i(*value as i32)],
        ),
        MidiMessage::ProgramChange { channel, program } => (
            clockwork_sys!("midi/in/program_change"),
            vec![s(port), i(*channel as i32), i(*program as i32)],
        ),
        MidiMessage::ChannelPressure { channel, pressure } => (
            clockwork_sys!("midi/in/channel_pressure"),
            vec![s(port), i(*channel as i32), i(*pressure as i32)],
        ),
        MidiMessage::PolyPressure { channel, note, pressure } => (
            clockwork_sys!("midi/in/poly_pressure"),
            vec![s(port), i(*channel as i32), i(*note as i32), i(*pressure as i32)],
        ),
        MidiMessage::PitchBend { channel, value } => (
            clockwork_sys!("midi/in/pitch_bend"),
            vec![s(port), i(*channel as i32), i(*value as i32)],
        ),
        MidiMessage::SysEx(bytes) => (
            clockwork_sys!("midi/in/sysex"),
            vec![s(port), OscArg::Blob(bytes.clone())],
        ),
        MidiMessage::SongPosition(pos) => (
            clockwork_sys!("midi/in/song_position"),
            vec![s(port), i(*pos as i32)],
        ),
        MidiMessage::TimeCodeQuarterFrame(d) => (
            clockwork_sys!("midi/in/time_code"),
            vec![s(port), i(*d as i32)],
        ),
        MidiMessage::SongSelect(n) => (clockwork_sys!("midi/in/song_select"), vec![s(port), i(*n as i32)]),
        MidiMessage::TuneRequest => (clockwork_sys!("midi/in/tune_request"), vec![s(port)]),
        MidiMessage::Start => (clockwork_sys!("midi/in/start"), vec![s(port)]),
        MidiMessage::Continue => (clockwork_sys!("midi/in/continue"), vec![s(port)]),
        MidiMessage::Stop => (clockwork_sys!("midi/in/stop"), vec![s(port)]),
        MidiMessage::ActiveSensing => (clockwork_sys!("midi/in/active_sensing"), vec![s(port)]),
        MidiMessage::Reset => (clockwork_sys!("midi/in/reset"), vec![s(port)]),
        MidiMessage::Clock => return None, // estimator side-channel
    };
    let mut args = args;
    if when != 0 { args.push(OscArg::TimeTag(when)); }
    Some(osc::encode(addr, &args))
}

/// `/clockwork/midi/in/clock_bpm <port:s> <bpm:f>` — the distilled tempo from the estimator.
pub fn encode_clock_bpm(port: &str, bpm: f64) -> Vec<u8> {
    osc::encode(
        clockwork_sys!("midi/in/clock_bpm"),
        &[OscArg::Str(port.to_string()), OscArg::Float(bpm as f32)],
    )
}

fn ports_args(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<OscArg> {
    let mut args = Vec::with_capacity(2 + (ins.len() + outs.len()) * 2);
    args.push(OscArg::Int(ins.len() as i32));
    for (name, enabled) in ins {
        args.push(OscArg::Str(name.clone()));
        args.push(OscArg::Int(*enabled as i32));
    }
    args.push(OscArg::Int(outs.len() as i32));
    for (name, enabled) in outs {
        args.push(OscArg::Str(name.clone()));
        args.push(OscArg::Int(*enabled as i32));
    }
    args
}

/// `/clockwork/midi/ports.reply <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`
/// — the RPC reply to `/clockwork/midi/ports/list`, sent to the caller.
pub fn encode_ports_reply(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<u8> {
    osc::encode(clockwork_sys!("midi/ports.reply"), &ports_args(ins, outs))
}

/// `/clockwork/midi/ports …` — same payload, broadcast to subscribers on a hotplug change.
pub fn encode_ports(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<u8> {
    osc::encode(clockwork_sys!("midi/ports"), &ports_args(ins, outs))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_inbound_event_carries_its_arrival_as_a_trailing_timetag() {
        let msg = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
        let plain = encode_in("keys", &msg).unwrap();
        let at = encode_in_at("keys", &msg, 0xdead_beef_0000_0001).unwrap();
        let p = osc::decode(&plain).unwrap();
        let a = osc::decode(&at).unwrap();
        assert_eq!(p.addr, a.addr);
        assert_eq!(a.args.len(), p.args.len() + 1);
        assert_eq!(&a.args[..p.args.len()], &p.args[..]);
        assert_eq!(a.args.last(), Some(&OscArg::TimeTag(0xdead_beef_0000_0001)));
        // 0 is "unknown": nothing is appended, and the two are the same bytes.
        assert_eq!(encode_in_at("keys", &msg, 0).unwrap(), plain);
    }
    use crate::osc;

    #[test]
    fn decodes_note_on_out() {
        let bytes = osc::encode(
            clockwork_sys!("midi/out/note_on"),
            &[
                OscArg::Str("kbd".into()),
                OscArg::Int(1),
                OscArg::Int(60),
                OscArg::Int(100),
            ],
        );
        assert_eq!(
            decode_out(&bytes),
            Ok(OutCommand::Send {
                port: "kbd".into(),
                msg: MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 },
                when: 0,
            })
        );
    }

    #[test]
    fn decodes_clock_and_management_verbs() {
        let tick = osc::encode(clockwork_sys!("midi/clock/tick"), &[OscArg::Str("synth".into())]);
        assert_eq!(
            decode_out(&tick),
            Ok(OutCommand::ClockTick { port: "synth".into() })
        );

        let sync = osc::encode(
            clockwork_sys!("midi/clock/sync"),
            &[OscArg::Str("in".into()), OscArg::Int(1)],
        );
        assert_eq!(
            decode_out(&sync),
            Ok(OutCommand::ClockSync { port: "in".into(), enabled: true })
        );

        let en = osc::encode(
            clockwork_sys!("midi/in/enable"),
            &[OscArg::Str("*".into()), OscArg::Int(0)],
        );
        assert_eq!(
            decode_out(&en),
            Ok(OutCommand::Enable { port: "*".into(), input: true, enabled: false })
        );

        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("midi/refresh"), &[])),
            Ok(OutCommand::Refresh)
        );
    }

    #[test]
    fn decodes_single_byte_realtime_out() {
        for (addr, msg) in [
            (clockwork_sys!("midi/out/clock"), MidiMessage::Clock),
            (clockwork_sys!("midi/out/start"), MidiMessage::Start),
            (clockwork_sys!("midi/out/continue"), MidiMessage::Continue),
            (clockwork_sys!("midi/out/stop"), MidiMessage::Stop),
        ] {
            let bytes = osc::encode(addr, &[OscArg::Str("p".into())]);
            assert_eq!(
                decode_out(&bytes),
                Ok(OutCommand::Send { port: "p".into(), msg, when: 0 })
            );
        }
    }

    #[test]
    fn decodes_raw_from_ints() {
        let bytes = osc::encode(
            clockwork_sys!("midi/out/raw"),
            &[OscArg::Str("p".into()), OscArg::Int(0x90), OscArg::Int(60), OscArg::Int(64)],
        );
        assert_eq!(
            decode_out(&bytes),
            Ok(OutCommand::SendRaw { port: "p".into(), bytes: vec![0x90, 60, 64] , when: 0 })
        );
    }

    #[test]
    fn raw_and_sysex_with_missing_args_are_malformed() {
        // Arity must be checked before the byte-collection slice: a bare
        // address with no args reaches the engine from arbitrary OSC clients
        // and must be rejected, not panic (a panic would unwind across the
        // C ABI and abort the host process).
        for addr in [clockwork_sys!("midi/out/raw"), clockwork_sys!("midi/out/sysex")] {
            assert_eq!(decode_out(&osc::encode(addr, &[])), Err(Refusal::Malformed));
        }
        // Port but no data bytes is well-formed: an empty send.
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("midi/out/raw"), &[OscArg::Str("p".into())])),
            Ok(OutCommand::SendRaw { port: "p".into(), bytes: vec![] , when: 0 })
        );
    }

    #[test]
    fn an_unknown_verb_is_told_from_a_malformed_one() {
        // The engine answers each with its own reason, so a client learns
        // whether it misspelled the verb or sent it the wrong arguments.
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("midi/bogus"), &[])),
            Err(Refusal::Unknown)
        );
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("gamepad/refresh"), &[])),
            Err(Refusal::Unknown)
        );
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("midi/in/enable"), &[])),
            Err(Refusal::Malformed)
        );
        assert_eq!(
            decode_out(&osc::encode(
                clockwork_sys!("midi/out/note_on"),
                &[OscArg::Str("kbd".into()), OscArg::Int(1)],
            )),
            Err(Refusal::Malformed)
        );
        // Bytes that are not OSC at all name no verb anyone can answer.
        assert_eq!(decode_out(&[]), Err(Refusal::Malformed));
    }

    #[test]
    fn encodes_inbound_events() {
        let bytes = encode_in("kbd", &MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 })
            .unwrap();
        let m = osc::decode(&bytes).unwrap();
        assert_eq!(m.addr, clockwork_sys!("midi/in/note_on"));
        assert_eq!(m.args[0], OscArg::Str("kbd".into()));
        assert_eq!(m.args[1], OscArg::Int(2));
        // clock pulses are not encoded as events
        assert!(encode_in("kbd", &MidiMessage::Clock).is_none());
    }

    #[test]
    fn kind_and_data_args_agree_with_encoded_osc() {
        // The structured web boundary (MidiMessage::kind / data_args) must produce
        // exactly the address suffix + integer args that encode_in emits, so the
        // OSC and structured paths can never diverge.
        let cases = [
            MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 },
            MidiMessage::NoteOff { channel: 1, note: 60, velocity: 0 },
            MidiMessage::ControlChange { channel: 10, controller: 74, value: 64 },
            MidiMessage::ProgramChange { channel: 8, program: 40 },
            MidiMessage::ChannelPressure { channel: 2, pressure: 99 },
            MidiMessage::PolyPressure { channel: 3, note: 50, pressure: 7 },
            MidiMessage::PitchBend { channel: 1, value: 8192 },
            MidiMessage::SongPosition(12345),
            MidiMessage::SongSelect(7),
            MidiMessage::Start,
            MidiMessage::Stop,
        ];
        for m in cases {
            let bytes = encode_in("kbd", &m).unwrap();
            let decoded = osc::decode(&bytes).unwrap();
            assert_eq!(decoded.addr, format!(clockwork_sys!("midi/in/{}"), m.kind()), "addr for {m:?}");
            assert_eq!(decoded.args[0], OscArg::Str("kbd".into()), "port for {m:?}");
            // Int args after the port must match data_args, in order.
            let osc_ints: Vec<i32> =
                decoded.args[1..].iter().filter_map(|a| a.as_i32()).collect();
            let mut buf = [0i32; 3];
            let n = m.data_args(&mut buf);
            assert_eq!(osc_ints, buf[..n].to_vec(), "data_args for {m:?}");
        }
    }

    #[test]
    fn ports_reply_roundtrips() {
        let bytes = encode_ports_reply(
            &[("a".into(), true), ("b".into(), false)],
            &[("c".into(), true)],
        );
        let m = osc::decode(&bytes).unwrap();
        assert_eq!(m.addr, clockwork_sys!("midi/ports.reply"));
        assert_eq!(m.args[0], OscArg::Int(2));
        assert_eq!(m.args[1], OscArg::Str("a".into()));
        assert_eq!(m.args[5], OscArg::Int(1)); // nOut
    }
}

#[cfg(test)]
mod when_tests {
    use super::*;

    fn note_on_bytes(extra: Option<OscArg>) -> Vec<u8> {
        let mut args = vec![
            OscArg::Str("kbd".into()),
            OscArg::Int(1), OscArg::Int(60), OscArg::Int(100),
        ];
        if let Some(a) = extra { args.push(a); }
        osc::encode(clockwork_sys!("midi/out/note_on"), &args)
    }

    #[test]
    fn no_timetag_means_now() {
        // Every existing client sends four arguments and keeps working: an
        // absent time is 0, which is the OSC sentinel for immediately.
        match decode_out(&note_on_bytes(None)) {
            Ok(OutCommand::Send { when, .. }) => assert_eq!(when, 0),
            other => panic!("expected a Send, got {other:?}"),
        }
    }

    #[test]
    fn a_trailing_timetag_is_the_time_to_send_it() {
        let t = 0xDEAD_BEEF_0000_0001u64;
        match decode_out(&note_on_bytes(Some(OscArg::TimeTag(t)))) {
            Ok(OutCommand::Send { when, port, .. }) => {
                assert_eq!(when, t);
                assert_eq!(port, "kbd", "the rest of the message is unchanged");
            }
            other => panic!("expected a Send, got {other:?}"),
        }
    }

    #[test]
    fn a_trailing_argument_that_is_not_a_timetag_is_not_a_time() {
        // Only a `t` counts. An int on the end is somebody else's argument, and
        // reading it as a time would send a note in the year 1900 or 5000.
        match decode_out(&note_on_bytes(Some(OscArg::Int(42)))) {
            Ok(OutCommand::Send { when, .. }) => assert_eq!(when, 0),
            other => panic!("expected a Send, got {other:?}"),
        }
    }

    #[test]
    fn raw_and_sysex_carry_a_time_too() {
        let t = 0x1234_5678_9ABC_DEF0u64;
        let bytes = osc::encode(clockwork_sys!("midi/out/raw"), &[
            OscArg::Str("kbd".into()),
            OscArg::Blob(vec![0x90, 60, 100]),
            OscArg::TimeTag(t),
        ]);
        match decode_out(&bytes) {
            Ok(OutCommand::SendRaw { when, .. }) => assert_eq!(when, t),
            other => panic!("expected a SendRaw, got {other:?}"),
        }
    }
}
