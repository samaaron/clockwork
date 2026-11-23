// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The `/clockwork/gamepad/*` OSC schema — the single API surface, identical on native
//! and web. Inbound controller state changes are encoded to `/clockwork/gamepad/in/*`;
//! client/VM requests arrive as `/clockwork/gamepad/out/*` and device-management verbs
//! and are decoded to an [`OutCommand`].
//!
//! Pads are addressed by their normalised handle (see `clockwork_osc::normalize`);
//! `pad == "*"` means "all connected pads". Rumble magnitudes are 0..=1.

use clockwork_osc::{clockwork_sys, Refusal, Timetag};
use clockwork_osc::wire::{self, Args, Message, Unwritable, Writer};

/// A decoded client/VM request.
#[derive(Clone, Debug, PartialEq)]
pub enum OutCommand {
    /// Start (or retrigger) rumble on `pad` (`"*"` = all). `strong`/`weak` are
    /// the two motor magnitudes (0..=1); `duration_ms <= 0` means "until stop".
    Rumble { pad: String, strong: f32, weak: f32, duration_ms: i32 },
    /// Stop any active rumble on `pad` (`"*"` = all).
    RumbleStop { pad: String },
    /// Mute/unmute a pad's `/clockwork/gamepad/in/*` events (`"*"` = all, and sets the
    /// default for pads that connect later). Pads are enabled by default.
    Enable { pad: String, enabled: bool },
    DevicesList,
    Refresh,
    Subscribe,
    Unsubscribe,
}

fn pad(mut args: Args<'_>) -> Option<String> {
    args.next()?.str().map(|s| s.to_string())
}
fn f_at(mut args: Args<'_>, i: usize) -> Option<f32> {
    args.nth(i)?.f64().map(|v| v as f32)
}
fn i_at(mut args: Args<'_>, i: usize) -> Option<i32> {
    args.nth(i)?.i32()
}

/// Decode an outbound `/clockwork/gamepad/*` OSC message into an [`OutCommand`],
/// or say why not: an address that is not one of the verbs is
/// [`Refusal::Unknown`], and a verb whose arguments cannot be read is
/// [`Refusal::Malformed`].
///
/// The match names the verbs, each with the reader for its arguments.
pub fn decode_out(data: &[u8]) -> Result<OutCommand, Refusal> {
    let m = Message::parse(data).map_err(|_| Refusal::Malformed)?;
    let read: fn(Args<'_>) -> Option<OutCommand> = match m.address() {
        clockwork_sys!("gamepad/out/rumble") => |a| Some(OutCommand::Rumble {
            pad: pad(a)?,
            strong: f_at(a, 1)?.clamp(0.0, 1.0),
            weak: f_at(a, 2)?.clamp(0.0, 1.0),
            duration_ms: i_at(a, 3)?,
        }),
        clockwork_sys!("gamepad/out/rumble_stop") => |a| Some(OutCommand::RumbleStop { pad: pad(a)? }),

        clockwork_sys!("gamepad/enable") => |a| Some(OutCommand::Enable {
            pad: pad(a)?,
            enabled: i_at(a, 1)? != 0,
        }),

        clockwork_sys!("gamepad/devices/list") | clockwork_sys!("gamepad/devices/get") => |_| Some(OutCommand::DevicesList),
        clockwork_sys!("gamepad/refresh") => |_| Some(OutCommand::Refresh),
        clockwork_sys!("gamepad/notify/subscribe") => |_| Some(OutCommand::Subscribe),
        clockwork_sys!("gamepad/notify/unsubscribe") => |_| Some(OutCommand::Unsubscribe),
        _ => return Err(Refusal::Unknown),
    };
    read(m.args()).ok_or(Refusal::Malformed)
}

// Each message below is written by a `write_*` into a buffer the caller owns,
// and has an `encode_*` that makes it in a `Vec` of exactly the room it takes.
// A name with a NUL in it cannot be written ([`Unwritable::Nul`]); a handle
// from `clockwork_osc::normalize` never has one.

/// The moment an event was seen as its trailing `t` argument; 0 leaves it off.
fn at(w: &mut Writer<'_>, when: u64) -> Result<(), Unwritable> {
    if when != 0 {
        w.timetag(Timetag::from_ntp(when))?;
    }
    Ok(())
}

/// `/clockwork/gamepad/in/button <pad:s> <button:s> <pressed:i> <value:f> [when:t]` —
/// value is 0..=1 (analog triggers sweep; digital buttons jump 0/1); `when` is
/// the moment it was seen, an OSC timetag (see clockwork-midi's `encode_in_at`).
pub fn write_button<'b>(buf: &'b mut [u8], pad: &str, button: &str, pressed: bool, value: f32, when: u64) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, BUTTON, |w| {
        w.str(pad)?.str(button)?.int(pressed as i32)?.float(value)?;
        at(w, when)
    })
}
const BUTTON: &str = clockwork_sys!("gamepad/in/button");

/// [`write_button`] with no moment on it.
pub fn encode_button(pad: &str, button: &str, pressed: bool, value: f32) -> Result<Vec<u8>, Unwritable> {
    encode_button_at(pad, button, pressed, value, 0)
}

/// [`write_button`], as a `Vec`.
pub fn encode_button_at(pad: &str, button: &str, pressed: bool, value: f32, when: u64) -> Result<Vec<u8>, Unwritable> {
    wire::to_vec(button_room(pad, button), |buf| write_button(buf, pad, button, pressed, value, when))
}

/// The room [`write_button`] takes at most, whatever the moment on it.
pub fn button_room(pad: &str, button: &str) -> usize {
    wire::room(BUTTON, 5, wire::text_room(pad) + wire::text_room(button) + 4 + 4 + 8)
}

/// `/clockwork/gamepad/in/axis <pad:s> <axis:s> <value:f> [when:t]` — value is
/// -1..=1, up/right positive.
pub fn write_axis<'b>(buf: &'b mut [u8], pad: &str, axis: &str, value: f32, when: u64) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, AXIS, |w| {
        w.str(pad)?.str(axis)?.float(value)?;
        at(w, when)
    })
}
const AXIS: &str = clockwork_sys!("gamepad/in/axis");

/// [`write_axis`] with no moment on it.
pub fn encode_axis(pad: &str, axis: &str, value: f32) -> Result<Vec<u8>, Unwritable> {
    encode_axis_at(pad, axis, value, 0)
}

/// [`write_axis`], as a `Vec`.
pub fn encode_axis_at(pad: &str, axis: &str, value: f32, when: u64) -> Result<Vec<u8>, Unwritable> {
    wire::to_vec(axis_room(pad, axis), |buf| write_axis(buf, pad, axis, value, when))
}

/// The room [`write_axis`] takes at most, whatever the moment on it.
pub fn axis_room(pad: &str, axis: &str) -> usize {
    wire::room(AXIS, 4, wire::text_room(pad) + wire::text_room(axis) + 4 + 8)
}

/// `<address> <n:i> [name:s enabled:i]*`: the pads there are, to `address`.
pub fn write_devices<'b>(buf: &'b mut [u8], address: &str, rows: &[(String, bool)]) -> Result<&'b [u8], Unwritable> {
    wire::message(buf, address, |w| {
        w.int(rows.len() as i32)?;
        rows.iter().try_for_each(|(name, enabled)| w.str(name)?.int(*enabled as i32).map(drop))
    })
}

fn encode_devices_to(address: &str, rows: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    let names: usize = rows.iter().map(|(name, _)| wire::text_room(name) + 4).sum();
    wire::to_vec(wire::room(address, 1 + rows.len() * 2, 4 + names), |buf| write_devices(buf, address, rows))
}

/// `/clockwork/gamepad/devices.reply <n:i> [name:s enabled:i]*` — the RPC reply to
/// `/clockwork/gamepad/devices/list`, sent to the caller.
pub fn encode_devices_reply(rows: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    encode_devices_to(clockwork_sys!("gamepad/devices.reply"), rows)
}

/// `/clockwork/gamepad/devices …` — same payload, broadcast to subscribers on a
/// connect/disconnect/enable change.
pub fn encode_devices(rows: &[(String, bool)]) -> Result<Vec<u8>, Unwritable> {
    encode_devices_to(clockwork_sys!("gamepad/devices"), rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_osc::wire::Arg;

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

    #[test]
    fn an_event_carries_the_moment_it_was_seen_as_a_trailing_timetag() {
        let plain = encode_button("pad", "a", true, 1.0).unwrap();
        let at = encode_button_at("pad", "a", true, 1.0, 77).unwrap();
        let (plain, at) = (args(&plain), args(&at));
        assert_eq!(at.len(), plain.len() + 1);
        assert_eq!(&at[..plain.len()], &plain[..]);
        assert_eq!(at.last(), Some(&Arg::TimeTag(Timetag::from_ntp(77))));
        let axis = encode_axis_at("pad", "left_x", -0.5, 78).unwrap();
        assert_eq!(args(&axis).last(), Some(&Arg::TimeTag(Timetag::from_ntp(78))));
        assert_eq!(encode_axis_at("pad", "left_x", -0.5, 0), encode_axis("pad", "left_x", -0.5));
    }

    #[test]
    fn decodes_rumble() {
        let bytes = verb(clockwork_sys!("gamepad/out/rumble"), |w| {
            w.str("xbox_controller")?.float(0.8)?.float(0.3)?.int(250).map(drop)
        });
        assert_eq!(
            decode_out(&bytes),
            Ok(OutCommand::Rumble {
                pad: "xbox_controller".into(),
                strong: 0.8,
                weak: 0.3,
                duration_ms: 250,
            })
        );
    }

    #[test]
    fn rumble_magnitudes_accept_ints_and_clamp() {
        let bytes = verb(clockwork_sys!("gamepad/out/rumble"), |w| w.str("*")?.int(1)?.float(7.5)?.int(0).map(drop));
        assert_eq!(
            decode_out(&bytes),
            Ok(OutCommand::Rumble { pad: "*".into(), strong: 1.0, weak: 1.0, duration_ms: 0 })
        );
    }

    #[test]
    fn a_count_or_a_flag_sent_as_a_float_is_read_as_the_whole_number_in_it() {
        let rumble = verb(clockwork_sys!("gamepad/out/rumble"), |w| w.str("p")?.float(0.5)?.float(0.5)?.float(250.9).map(drop));
        assert_eq!(decode_out(&rumble), Ok(OutCommand::Rumble { pad: "p".into(), strong: 0.5, weak: 0.5, duration_ms: 250 }));
        let enable = verb(clockwork_sys!("gamepad/enable"), |w| w.str("p")?.double(1.0).map(drop));
        assert_eq!(decode_out(&enable), Ok(OutCommand::Enable { pad: "p".into(), enabled: true }));
    }

    #[test]
    fn decodes_management_verbs() {
        let en = verb(clockwork_sys!("gamepad/enable"), |w| w.str("*")?.int(0).map(drop));
        assert_eq!(
            decode_out(&en),
            Ok(OutCommand::Enable { pad: "*".into(), enabled: false })
        );
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/devices/list"))), Ok(OutCommand::DevicesList));
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/refresh"))), Ok(OutCommand::Refresh));
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/notify/subscribe"))), Ok(OutCommand::Subscribe));
        let stop = verb(clockwork_sys!("gamepad/out/rumble_stop"), |w| w.str("p").map(drop));
        assert_eq!(decode_out(&stop), Ok(OutCommand::RumbleStop { pad: "p".into() }));
    }

    #[test]
    fn malformed_and_unknown_verbs_are_told_apart() {
        // Arity is checked before use: a bare address reaches the engine from
        // arbitrary OSC clients and must be rejected, not panic (a panic would
        // unwind across the C ABI and abort the host process). The engine
        // answers each refusal with its own reason, so a client learns whether
        // it misspelled the verb or sent it the wrong arguments.
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/out/rumble"))), Err(Refusal::Malformed));
        assert_eq!(decode_out(&verb(clockwork_sys!("gamepad/enable"), |w| w.str("p").map(drop))), Err(Refusal::Malformed));
        assert_eq!(decode_out(&bare(clockwork_sys!("gamepad/bogus"))), Err(Refusal::Unknown));
        assert_eq!(decode_out(&bare(clockwork_sys!("midi/refresh"))), Err(Refusal::Unknown));
        assert_eq!(decode_out(&[]), Err(Refusal::Malformed));
    }

    #[test]
    fn encodes_inbound_events() {
        let b = encode_button("pad", "south", true, 1.0).unwrap();
        assert_eq!(Message::parse(&b).unwrap().address(), clockwork_sys!("gamepad/in/button"));
        assert_eq!(args(&b), [Arg::Str("pad"), Arg::Str("south"), Arg::Int(1), Arg::Float(1.0)]);

        let a = encode_axis("pad", "left_x", -0.5).unwrap();
        assert_eq!(Message::parse(&a).unwrap().address(), clockwork_sys!("gamepad/in/axis"));
        assert_eq!(args(&a), [Arg::Str("pad"), Arg::Str("left_x"), Arg::Float(-0.5)]);
    }

    #[test]
    fn devices_reply_roundtrips() {
        let bytes = encode_devices_reply(&[("a".into(), true), ("b".into(), false)]).unwrap();
        assert_eq!(Message::parse(&bytes).unwrap().address(), clockwork_sys!("gamepad/devices.reply"));
        assert_eq!(args(&bytes), [Arg::Int(2), Arg::Str("a"), Arg::Int(1), Arg::Str("b"), Arg::Int(0)]);
        let pushed = encode_devices(&[("a".into(), true), ("b".into(), false)]).unwrap();
        assert_eq!(Message::parse(&pushed).unwrap().address(), clockwork_sys!("gamepad/devices"));
        assert_eq!(args(&pushed), args(&bytes));
    }

    // The room each `encode_*` asks for is exactly what its message takes: a
    // byte less and it is refused, so none asks for too little and none for
    // more than it uses.
    #[test]
    fn every_message_is_made_in_exactly_the_room_it_takes() {
        for name in ["", "a", "pad", "an_xbox_controller_2"] {
            for when in [0, 77] {
                let button = encode_button_at(name, "south", true, 1.0, when).unwrap();
                assert!(write_button(&mut vec![0; button.len() - 1], name, "south", true, 1.0, when).is_err());
                let axis = encode_axis_at(name, "left_x", 0.5, when).unwrap();
                assert!(write_axis(&mut vec![0; axis.len() - 1], name, "left_x", 0.5, when).is_err());
                // An event with no moment is made in room for one: eight bytes go unused.
                let spare = if when == 0 { 8 } else { 0 };
                assert_eq!(wire::room(BUTTON, 5, wire::text_room(name) + wire::text_room("south") + 16), button.len() + spare);
                assert_eq!(wire::room(AXIS, 4, wire::text_room(name) + wire::text_room("left_x") + 12), axis.len() + spare);
            }
            let rows: Vec<(String, bool)> = (0..5).map(|i| (format!("{name}{i}"), i % 2 == 0)).collect();
            for n in 0..=rows.len() {
                let devices = encode_devices(&rows[..n]).unwrap();
                assert!(write_devices(&mut vec![0; devices.len() - 1], clockwork_sys!("gamepad/devices"), &rows[..n]).is_err());
            }
        }
    }

    #[test]
    fn a_name_that_cannot_be_written_is_refused() {
        assert_eq!(encode_button("p\0ad", "a", true, 1.0), Err(Unwritable::Nul));
        assert_eq!(encode_devices(&[("p\0ad".into(), true)]), Err(Unwritable::Nul));
    }
}
