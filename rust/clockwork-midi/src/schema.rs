// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The `/clockwork/midi/*` OSC schema — the single API surface, identical on native and
//! web. Inbound hardware MIDI is encoded to `/clockwork/midi/in/*`; client/VM requests
//! arrive as `/clockwork/midi/out/*`, `/clockwork/midi/clock/*` and device-management verbs and are
//! decoded to an [`OutCommand`].
//!
//! Channels are 1-based (1..=16); `channel == -1` on the wire means "all 16",
//! and `port == "*"` means "all enabled ports". Pitch bend is 14-bit (0..=16383).

use clockwork_osc::wire::{self, Arg, Args, Message, Unwritable};
use clockwork_osc::{clockwork_sys, Refusal, Timetag};
use crate::message::MidiMessage;

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

fn port(mut args: Args<'_>) -> Option<String> {
    args.next()?.str().map(|s| s.to_string())
}
fn i_at(mut args: Args<'_>, i: usize) -> Option<i32> {
    args.nth(i)?.i32()
}

/// Channel arg for a channel-voice message: `-1` ([`ALL_CHANNELS`]) maps to the
/// `0` sentinel (fanned out to all 16 by the send path); 1..=16 pass through.
fn ch_at(args: Args<'_>, i: usize) -> Option<u8> {
    Some(match i_at(args, i)? {
        ALL_CHANNELS => 0,
        c => c as u8,
    })
}

/// A trailing OSC timetag, if the client sent one. Absent means now.
///
/// Read from the end rather than by index: the verbs have different arities
/// and a timetag is unambiguous — no other argument in this schema is a `t`.
fn trailing_when(args: Args<'_>) -> u64 {
    match args.last() {
        Some(Arg::TimeTag(t)) => t.ntp(),
        _ => 0,
    }
}

/// Decode an outbound `/clockwork/midi/*` OSC message into an [`OutCommand`],
/// or say why not: an address that is not one of the verbs is
/// [`Refusal::Unknown`], and a verb whose arguments cannot be read is
/// [`Refusal::Malformed`].
pub fn decode_out(data: &[u8]) -> Result<OutCommand, Refusal> {
    let m = Message::parse(data).map_err(|_| Refusal::Malformed)?;
    let when = trailing_when(m.args());
    Ok(match decode_out_verb(&m)? {
        OutCommand::Send { port, msg, .. } => OutCommand::Send { port, msg, when },
        OutCommand::SendRaw { port, bytes, .. } => OutCommand::SendRaw { port, bytes, when },
        other => other,
    })
}

// The match names the verbs, each with the reader for its arguments: an
// address it does not name is unknown, and a reader that cannot read what it
// was sent makes the verb malformed.
fn decode_out_verb(m: &Message<'_>) -> Result<OutCommand, Refusal> {
    let read: fn(Args<'_>) -> Option<OutCommand> = match m.address() {
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
            let port = port(a)?;
            let mut data = a.skip(1);
            let bytes = match data.clone().next() {
                Some(Arg::Blob(b)) => b.to_vec(),
                _ => data.by_ref().filter_map(|x| x.i32()).map(|v| (v & 0xff) as u8).collect(),
            };
            Some(OutCommand::SendRaw { port, bytes, when: 0 })
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
    read(m.args()).ok_or(Refusal::Malformed)
}

// Each message below is written by a `write_*` into a buffer the caller owns,
// and has an `encode_*` that makes it in a `Vec` of exactly the room it takes.
// A port name with a NUL in it cannot be written ([`Unwritable::Nul`]); a
// handle from `clockwork_osc::normalize` never has one.

/// Where an inbound message is sent. None for a clock pulse (`0xF8`), which
/// goes to the tempo estimator and is never an event.
fn in_address(msg: &MidiMessage) -> Option<&'static str> {
    Some(match msg {
        MidiMessage::NoteOn { .. } => clockwork_sys!("midi/in/note_on"),
        MidiMessage::NoteOff { .. } => clockwork_sys!("midi/in/note_off"),
        MidiMessage::ControlChange { .. } => clockwork_sys!("midi/in/control_change"),
        MidiMessage::ProgramChange { .. } => clockwork_sys!("midi/in/program_change"),
        MidiMessage::ChannelPressure { .. } => clockwork_sys!("midi/in/channel_pressure"),
        MidiMessage::PolyPressure { .. } => clockwork_sys!("midi/in/poly_pressure"),
        MidiMessage::PitchBend { .. } => clockwork_sys!("midi/in/pitch_bend"),
        MidiMessage::SysEx(_) => clockwork_sys!("midi/in/sysex"),
        MidiMessage::SongPosition(_) => clockwork_sys!("midi/in/song_position"),
        MidiMessage::TimeCodeQuarterFrame(_) => clockwork_sys!("midi/in/time_code"),
        MidiMessage::SongSelect(_) => clockwork_sys!("midi/in/song_select"),
        MidiMessage::TuneRequest => clockwork_sys!("midi/in/tune_request"),
        MidiMessage::Start => clockwork_sys!("midi/in/start"),
        MidiMessage::Continue => clockwork_sys!("midi/in/continue"),
        MidiMessage::Stop => clockwork_sys!("midi/in/stop"),
        MidiMessage::ActiveSensing => clockwork_sys!("midi/in/active_sensing"),
        MidiMessage::Reset => clockwork_sys!("midi/in/reset"),
        MidiMessage::Clock => return None,
    })
}

/// An inbound hardware message as its `/clockwork/midi/in/*` event, for the
/// engine's ingress and for subscribers: the port, then the message's numbers
/// ([`MidiMessage::data_args`]) or a sysex's bytes as a blob, then `when` as a
/// trailing `t` argument. None for a clock pulse.
///
/// `when` is the moment the bytes ARRIVED, an OSC timetag; 0 leaves it off.
/// The arrival time is the one thing a host knows that the message does not
/// carry, and a guest placing the event on its own timeline wants it: a note
/// that crossed a jittery hop from the main thread is still the note that was
/// played when it was played. A reader scanning by index is unaffected by a
/// trailing argument.
pub fn write_in<'b>(buf: &'b mut [u8], port: &str, msg: &MidiMessage, when: u64) -> Option<Result<&'b [u8], Unwritable>> {
    in_address(msg).map(|address| write_event(buf, address, port, msg, when))
}

fn write_event<'b>(buf: &'b mut [u8], address: &str, port: &str, msg: &MidiMessage, when: u64) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, address, |w| {
        w.str(port)?;
        if let MidiMessage::SysEx(bytes) = msg {
            w.blob(bytes)?;
        } else {
            let mut numbers = [0i32; 3];
            let n = msg.data_args(&mut numbers);
            numbers[..n].iter().try_for_each(|&v| w.int(v).map(drop))?;
        }
        if when != 0 {
            w.timetag(Timetag::from_ntp(when))?;
        }
        Ok(())
    })
}

/// [`write_in`] with no arrival time on it.
pub fn encode_in(port: &str, msg: &MidiMessage) -> Option<Result<Vec<u8>, Unwritable>> {
    encode_in_at(port, msg, 0)
}

/// The room [`write_in`] takes at most, whatever `when` is: what to reserve
/// for the event before it is written. None for a clock pulse.
pub fn in_room(port: &str, msg: &MidiMessage) -> Option<usize> {
    let address = in_address(msg)?;
    let data = match msg {
        MidiMessage::SysEx(bytes) => wire::blob_room(bytes.len()),
        _ => 3 * 4,
    };
    Some(wire::room(address, 5, wire::text_room(port) + data + 8))
}

/// [`write_in`], as a `Vec`.
pub fn encode_in_at(port: &str, msg: &MidiMessage, when: u64) -> Option<Result<Vec<u8>, Unwritable>> {
    let (address, room) = (in_address(msg)?, in_room(port, msg)?);
    Some(wire::to_vec(room, |buf| write_event(buf, address, port, msg, when)))
}

/// `/clockwork/midi/in/clock_bpm <port:s> <bpm:f>` — the distilled tempo from the estimator.
pub fn write_clock_bpm<'b>(buf: &'b mut [u8], port: &str, bpm: f64) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, CLOCK_BPM, |w| w.str(port)?.float(bpm as f32).map(drop))
}
const CLOCK_BPM: &str = clockwork_sys!("midi/in/clock_bpm");

/// [`write_clock_bpm`], as a `Vec`.
pub fn encode_clock_bpm(port: &str, bpm: f64) -> Result<Vec<u8>, Unwritable> {
    wire::to_vec(wire::room(CLOCK_BPM, 2, wire::text_room(port) + 4), |buf| write_clock_bpm(buf, port, bpm))
}

/// `<address> <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`: the
/// ports there are, to `address`.
pub fn write_ports<'b>(buf: &'b mut [u8], address: &str, ins: &[(String, bool)], outs: &[(String, bool)]) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, address, |w| {
        for rows in [ins, outs] {
            w.int(rows.len() as i32)?;
            rows.iter().try_for_each(|(name, enabled)| w.str(name)?.int(*enabled as i32).map(drop))?;
        }
        Ok(())
    })
}

fn encode_ports_to(address: &str, ins: &[(String, bool)], outs: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    let rows = ins.len() + outs.len();
    let names: usize = ins.iter().chain(outs).map(|(name, _)| wire::text_room(name) + 4).sum();
    wire::to_vec(wire::room(address, 2 + rows * 2, 8 + names), |buf| write_ports(buf, address, ins, outs))
}

/// `/clockwork/midi/ports.reply <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`
/// — the RPC reply to `/clockwork/midi/ports/list`, sent to the caller.
pub fn encode_ports_reply(ins: &[(String, bool)], outs: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    encode_ports_to(clockwork_sys!("midi/ports.reply"), ins, outs)
}

/// `/clockwork/midi/ports …` — same payload, broadcast to subscribers on a hotplug change.
pub fn encode_ports(ins: &[(String, bool)], outs: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    encode_ports_to(clockwork_sys!("midi/ports"), ins, outs)
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_osc::wire::Writer;

    /// A verb as a client would send it.
    fn verb(address: &str, fill: impl FnOnce(&mut Writer<'_>) -> Result<(), Unwritable>) -> Vec<u8> {
        wire::to_vec(256, |buf| wire::message(buf, address, fill)).unwrap()
    }
    fn bare(address: &str) -> Vec<u8> {
        verb(address, |_| Ok(()))
    }
    fn args(bytes: &[u8]) -> Vec<Arg<'_>> {
        Message::parse(bytes).unwrap().args().collect()
    }
    fn address(bytes: &[u8]) -> &str {
        Message::parse(bytes).unwrap().address()
    }
    fn event(port: &str, msg: &MidiMessage) -> Vec<u8> {
        encode_in(port, msg).unwrap().unwrap()
    }

    #[test]
    fn an_inbound_event_carries_its_arrival_as_a_trailing_timetag() {
        let msg = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
        let plain = event("keys", &msg);
        let at = encode_in_at("keys", &msg, 0xdead_beef_0000_0001).unwrap().unwrap();
        assert_eq!(address(&plain), address(&at));
        let (p, a) = (args(&plain), args(&at));
        assert_eq!(a.len(), p.len() + 1);
        assert_eq!(&a[..p.len()], &p[..]);
        assert_eq!(a.last(), Some(&Arg::TimeTag(Timetag::from_ntp(0xdead_beef_0000_0001))));
        // 0 is "unknown": nothing is appended, and the two are the same bytes.
        assert_eq!(encode_in_at("keys", &msg, 0).unwrap().unwrap(), plain);
    }

    #[test]
    fn decodes_note_on_out() {
        let bytes = verb(clockwork_sys!("midi/out/note_on"), |w| w.str("kbd")?.int(1)?.int(60)?.int(100).map(drop));
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
    fn a_number_sent_as_a_float_is_read_as_the_whole_number_in_it() {
        // A client whose language has one kind of number sends floats.
        let bytes = verb(clockwork_sys!("midi/out/note_on"), |w| w.str("kbd")?.float(1.0)?.double(60.9)?.long(100).map(drop));
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
        let tick = verb(clockwork_sys!("midi/clock/tick"), |w| w.str("synth").map(drop));
        assert_eq!(decode_out(&tick), Ok(OutCommand::ClockTick { port: "synth".into() }));

        let sync = verb(clockwork_sys!("midi/clock/sync"), |w| w.str("in")?.int(1).map(drop));
        assert_eq!(decode_out(&sync), Ok(OutCommand::ClockSync { port: "in".into(), enabled: true }));

        let en = verb(clockwork_sys!("midi/in/enable"), |w| w.str("*")?.int(0).map(drop));
        assert_eq!(decode_out(&en), Ok(OutCommand::Enable { port: "*".into(), input: true, enabled: false }));

        assert_eq!(decode_out(&bare(clockwork_sys!("midi/refresh"))), Ok(OutCommand::Refresh));
    }

    #[test]
    fn decodes_single_byte_realtime_out() {
        for (addr, msg) in [
            (clockwork_sys!("midi/out/clock"), MidiMessage::Clock),
            (clockwork_sys!("midi/out/start"), MidiMessage::Start),
            (clockwork_sys!("midi/out/continue"), MidiMessage::Continue),
            (clockwork_sys!("midi/out/stop"), MidiMessage::Stop),
        ] {
            let bytes = verb(addr, |w| w.str("p").map(drop));
            assert_eq!(
                decode_out(&bytes),
                Ok(OutCommand::Send { port: "p".into(), msg, when: 0 })
            );
        }
    }

    #[test]
    fn decodes_raw_from_ints() {
        let bytes = verb(clockwork_sys!("midi/out/raw"), |w| w.str("p")?.int(0x90)?.int(60)?.int(64).map(drop));
        assert_eq!(
            decode_out(&bytes),
            Ok(OutCommand::SendRaw { port: "p".into(), bytes: vec![0x90, 60, 64], when: 0 })
        );
    }

    #[test]
    fn decodes_raw_from_a_blob() {
        for addr in [clockwork_sys!("midi/out/raw"), clockwork_sys!("midi/out/sysex")] {
            let bytes = verb(addr, |w| w.str("p")?.blob(&[0xF0, 1, 2, 0xF7]).map(drop));
            assert_eq!(
                decode_out(&bytes),
                Ok(OutCommand::SendRaw { port: "p".into(), bytes: vec![0xF0, 1, 2, 0xF7], when: 0 })
            );
        }
    }

    #[test]
    fn raw_and_sysex_with_missing_args_are_malformed() {
        // Arity must be checked before the bytes are collected: a bare
        // address with no args reaches the engine from arbitrary OSC clients
        // and must be rejected, not panic (a panic would unwind across the
        // C ABI and abort the host process).
        for addr in [clockwork_sys!("midi/out/raw"), clockwork_sys!("midi/out/sysex")] {
            assert_eq!(decode_out(&bare(addr)), Err(Refusal::Malformed));
        }
        // Port but no data bytes is well-formed: an empty send.
        assert_eq!(
            decode_out(&verb(clockwork_sys!("midi/out/raw"), |w| w.str("p").map(drop))),
            Ok(OutCommand::SendRaw { port: "p".into(), bytes: vec![], when: 0 })
        );
    }

    #[test]
    fn an_unknown_verb_is_told_from_a_malformed_one() {
        // The engine answers each with its own reason, so a client learns
        // whether it misspelled the verb or sent it the wrong arguments.
        assert_eq!(decode_out(&bare(clockwork_sys!("midi/bogus"))), Err(Refusal::Unknown));
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/refresh"))), Err(Refusal::Unknown));
        assert_eq!(decode_out(&bare(clockwork_sys!("midi/in/enable"))), Err(Refusal::Malformed));
        assert_eq!(
            decode_out(&verb(clockwork_sys!("midi/out/note_on"), |w| w.str("kbd")?.int(1).map(drop))),
            Err(Refusal::Malformed)
        );
        // Bytes that are not OSC at all name no verb anyone can answer.
        assert_eq!(decode_out(&[]), Err(Refusal::Malformed));
    }

    #[test]
    fn encodes_inbound_events() {
        let bytes = event("kbd", &MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 });
        assert_eq!(address(&bytes), clockwork_sys!("midi/in/note_on"));
        assert_eq!(args(&bytes), [Arg::Str("kbd"), Arg::Int(2), Arg::Int(64), Arg::Int(99)]);
        let sysex = event("kbd", &MidiMessage::SysEx(vec![0xF0, 1, 2, 3, 0xF7]));
        assert_eq!(address(&sysex), clockwork_sys!("midi/in/sysex"));
        assert_eq!(args(&sysex), [Arg::Str("kbd"), Arg::Blob(&[0xF0, 1, 2, 3, 0xF7])]);
        // clock pulses are not encoded as events
        assert!(encode_in("kbd", &MidiMessage::Clock).is_none());
        assert!(write_in(&mut [0; 64], "kbd", &MidiMessage::Clock, 0).is_none());
    }

    /// One of every message there is.
    fn every_message() -> Vec<MidiMessage> {
        vec![
            MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 },
            MidiMessage::NoteOff { channel: 1, note: 60, velocity: 0 },
            MidiMessage::ControlChange { channel: 10, controller: 74, value: 64 },
            MidiMessage::ProgramChange { channel: 8, program: 40 },
            MidiMessage::ChannelPressure { channel: 2, pressure: 99 },
            MidiMessage::PolyPressure { channel: 3, note: 50, pressure: 7 },
            MidiMessage::PitchBend { channel: 1, value: 8192 },
            MidiMessage::SysEx(vec![]),
            MidiMessage::SysEx(vec![0xF0, 0x7E, 0x7F, 6, 1, 0xF7]),
            MidiMessage::SongPosition(12345),
            MidiMessage::TimeCodeQuarterFrame(0x35),
            MidiMessage::SongSelect(7),
            MidiMessage::TuneRequest,
            MidiMessage::Start,
            MidiMessage::Continue,
            MidiMessage::Stop,
            MidiMessage::ActiveSensing,
            MidiMessage::Reset,
        ]
    }

    #[test]
    fn kind_and_data_args_agree_with_encoded_osc() {
        // The structured web boundary (MidiMessage::kind / data_args) must produce
        // exactly the address suffix + integer args that encode_in emits, so the
        // OSC and structured paths can never diverge.
        for m in every_message() {
            let bytes = event("kbd", &m);
            assert_eq!(address(&bytes), format!(clockwork_sys!("midi/in/{}"), m.kind()), "addr for {m:?}");
            let read = args(&bytes);
            assert_eq!(read[0], Arg::Str("kbd"), "port for {m:?}");
            // Int args after the port must match data_args, in order.
            let osc_ints: Vec<i32> = read[1..].iter().filter_map(|a| a.i32()).collect();
            let mut buf = [0i32; 3];
            let n = m.data_args(&mut buf);
            assert_eq!(osc_ints, buf[..n].to_vec(), "data_args for {m:?}");
        }
    }

    #[test]
    fn ports_reply_roundtrips() {
        let (ins, outs) = ([("a".to_string(), true), ("b".to_string(), false)], [("c".to_string(), true)]);
        let bytes = encode_ports_reply(&ins, &outs).unwrap();
        assert_eq!(address(&bytes), clockwork_sys!("midi/ports.reply"));
        let listed = [Arg::Int(2), Arg::Str("a"), Arg::Int(1), Arg::Str("b"), Arg::Int(0), Arg::Int(1), Arg::Str("c"), Arg::Int(1)];
        assert_eq!(args(&bytes), listed);
        let pushed = encode_ports(&ins, &outs).unwrap();
        assert_eq!(address(&pushed), clockwork_sys!("midi/ports"));
        assert_eq!(args(&pushed), listed);
    }

    #[test]
    fn the_tempo_is_a_port_and_a_float() {
        let bytes = encode_clock_bpm("in", 120.5).unwrap();
        assert_eq!(address(&bytes), clockwork_sys!("midi/in/clock_bpm"));
        assert_eq!(args(&bytes), [Arg::Str("in"), Arg::Float(120.5)]);
    }

    // The room each `encode_*` asks for is enough for its message and no more
    // than the message at its longest: one byte less than what was written is
    // refused, and with an arrival time and three numbers nothing is spare.
    #[test]
    fn every_message_is_made_in_the_room_it_takes() {
        for port in ["", "a", "kbd", "a_long_port_name_2"] {
            for m in every_message() {
                for when in [0, 77] {
                    let made = encode_in_at(port, &m, when).unwrap().unwrap();
                    assert!(write_in(&mut vec![0; made.len() - 1], port, &m, when).unwrap().is_err(), "{m:?}");
                }
            }
            let full = MidiMessage::NoteOn { channel: 1, note: 2, velocity: 3 };
            let made = encode_in_at(port, &full, 77).unwrap().unwrap();
            assert_eq!(made.len(), wire::room(in_address(&full).unwrap(), 5, wire::text_room(port) + 12 + 8));
            let bpm = encode_clock_bpm(port, 120.0).unwrap();
            assert!(write_clock_bpm(&mut vec![0; bpm.len() - 1], port, 120.0).is_err());
            let rows: Vec<(String, bool)> = (0..5).map(|i| (format!("{port}{i}"), i % 2 == 0)).collect();
            for (i, o) in [(0, 0), (1, 0), (0, 1), (2, 3), (5, 5)] {
                let ports = encode_ports(&rows[..i], &rows[..o]).unwrap();
                assert!(write_ports(&mut vec![0; ports.len() - 1], clockwork_sys!("midi/ports"), &rows[..i], &rows[..o]).is_err());
            }
        }
    }

    #[test]
    fn a_name_that_cannot_be_written_is_refused() {
        let note = MidiMessage::NoteOn { channel: 1, note: 2, velocity: 3 };
        assert_eq!(encode_in("k\0bd", &note), Some(Err(Unwritable::Nul)));
        assert_eq!(encode_ports(&[("k\0bd".into(), true)], &[]), Err(Unwritable::Nul));
        assert_eq!(encode_clock_bpm("k\0bd", 120.0), Err(Unwritable::Nul));
    }

    fn note_on_bytes(extra: impl FnOnce(&mut Writer<'_>) -> Result<(), Unwritable>) -> Vec<u8> {
        verb(clockwork_sys!("midi/out/note_on"), |w| {
            w.str("kbd")?.int(1)?.int(60)?.int(100)?;
            extra(w)
        })
    }

    #[test]
    fn no_timetag_means_now() {
        // Four arguments and no time: an absent time is 0, which is the OSC
        // sentinel for immediately.
        match decode_out(&note_on_bytes(|_| Ok(()))) {
            Ok(OutCommand::Send { when, .. }) => assert_eq!(when, 0),
            other => panic!("expected a Send, got {other:?}"),
        }
    }

    #[test]
    fn a_trailing_timetag_is_the_time_to_send_it() {
        let t = 0xDEAD_BEEF_0000_0001u64;
        match decode_out(&note_on_bytes(|w| w.timetag(Timetag::from_ntp(t)).map(drop))) {
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
        match decode_out(&note_on_bytes(|w| w.int(42).map(drop))) {
            Ok(OutCommand::Send { when, .. }) => assert_eq!(when, 0),
            other => panic!("expected a Send, got {other:?}"),
        }
    }

    #[test]
    fn raw_and_sysex_carry_a_time_too() {
        let t = 0x1234_5678_9ABC_DEF0u64;
        let bytes = verb(clockwork_sys!("midi/out/raw"), |w| w.str("kbd")?.blob(&[0x90, 60, 100])?.timetag(Timetag::from_ntp(t)).map(drop));
        match decode_out(&bytes) {
            Ok(OutCommand::SendRaw { when, bytes, .. }) => {
                assert_eq!(when, t);
                assert_eq!(bytes, [0x90, 60, 100]);
            }
            other => panic!("expected a SendRaw, got {other:?}"),
        }
        // From ints, the time on the end is not one of the bytes.
        let ints = verb(clockwork_sys!("midi/out/raw"), |w| w.str("kbd")?.int(0x90)?.int(60)?.timetag(Timetag::from_ntp(t)).map(drop));
        assert_eq!(decode_out(&ints), Ok(OutCommand::SendRaw { port: "kbd".into(), bytes: vec![0x90, 60], when: t }));
    }
}
