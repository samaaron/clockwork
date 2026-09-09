// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The `/clockwork/gamepad/*` OSC schema — the single API surface, identical on native
//! and web. Inbound controller state changes are encoded to `/clockwork/gamepad/in/*`;
//! client/VM requests arrive as `/clockwork/gamepad/out/*` and device-management verbs
//! and are decoded to an [`OutCommand`].
//!
//! Pads are addressed by their normalised handle (see `clockwork_osc::normalize`);
//! `pad == "*"` means "all connected pads". Rumble magnitudes are 0..=1.

use clockwork_osc::clockwork_sys;
use crate::osc::{self, OscArg};

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

fn pad(args: &[OscArg]) -> Option<String> {
    args.first()?.as_str().map(|s| s.to_string())
}
fn f_at(args: &[OscArg], i: usize) -> Option<f32> {
    args.get(i)?.as_f64().map(|v| v as f32)
}
fn i_at(args: &[OscArg], i: usize) -> Option<i32> {
    args.get(i)?.as_i32()
}

/// Decode an outbound `/clockwork/gamepad/*` OSC message into an [`OutCommand`].
/// Returns `None` for non-`/clockwork/gamepad/*` addresses or malformed args.
pub fn decode_out(data: &[u8]) -> Option<OutCommand> {
    let m = osc::decode(data)?;
    let a = &m.args;

    match m.addr.as_str() {
        clockwork_sys!("gamepad/out/rumble") => Some(OutCommand::Rumble {
            pad: pad(a)?,
            strong: f_at(a, 1)?.clamp(0.0, 1.0),
            weak: f_at(a, 2)?.clamp(0.0, 1.0),
            duration_ms: i_at(a, 3)?,
        }),
        clockwork_sys!("gamepad/out/rumble_stop") => Some(OutCommand::RumbleStop { pad: pad(a)? }),

        clockwork_sys!("gamepad/enable") => Some(OutCommand::Enable {
            pad: pad(a)?,
            enabled: i_at(a, 1)? != 0,
        }),

        clockwork_sys!("gamepad/devices/list") | clockwork_sys!("gamepad/devices/get") => Some(OutCommand::DevicesList),
        clockwork_sys!("gamepad/refresh") => Some(OutCommand::Refresh),
        clockwork_sys!("gamepad/notify/subscribe") => Some(OutCommand::Subscribe),
        clockwork_sys!("gamepad/notify/unsubscribe") => Some(OutCommand::Unsubscribe),
        _ => None,
    }
}

/// `/clockwork/gamepad/in/button <pad:s> <button:s> <pressed:i> <value:f>` — value is
/// 0..=1 (analog triggers sweep; digital buttons jump 0/1).
pub fn encode_button(pad: &str, button: &str, pressed: bool, value: f32) -> Vec<u8> {
    encode_button_at(pad, button, pressed, value, 0)
}

/// [`encode_button`] with the moment it was seen as a trailing `t` argument
/// (an OSC timetag; 0 leaves it off) — see clockwork-midi's `encode_in_at`.
pub fn encode_button_at(pad: &str, button: &str, pressed: bool, value: f32, when: u64) -> Vec<u8> {
    let mut args = vec![
        OscArg::Str(pad.to_string()),
        OscArg::Str(button.to_string()),
        OscArg::Int(pressed as i32),
        OscArg::Float(value),
    ];
    if when != 0 { args.push(OscArg::TimeTag(when)); }
    osc::encode(clockwork_sys!("gamepad/in/button"), &args)
}

/// `/clockwork/gamepad/in/axis <pad:s> <axis:s> <value:f>` — value is -1..=1, up/right
/// positive.
pub fn encode_axis(pad: &str, axis: &str, value: f32) -> Vec<u8> {
    encode_axis_at(pad, axis, value, 0)
}

/// [`encode_axis`] with the moment it was seen as a trailing `t` argument.
pub fn encode_axis_at(pad: &str, axis: &str, value: f32, when: u64) -> Vec<u8> {
    let mut args = vec![
        OscArg::Str(pad.to_string()),
        OscArg::Str(axis.to_string()),
        OscArg::Float(value),
    ];
    if when != 0 { args.push(OscArg::TimeTag(when)); }
    osc::encode(clockwork_sys!("gamepad/in/axis"), &args)
}

fn devices_args(rows: &[(String, bool)]) -> Vec<OscArg> {
    let mut args = Vec::with_capacity(1 + rows.len() * 2);
    args.push(OscArg::Int(rows.len() as i32));
    for (name, enabled) in rows {
        args.push(OscArg::Str(name.clone()));
        args.push(OscArg::Int(*enabled as i32));
    }
    args
}

/// `/clockwork/gamepad/devices.reply <n:i> [name:s enabled:i]*` — the RPC reply to
/// `/clockwork/gamepad/devices/list`, sent to the caller.
pub fn encode_devices_reply(rows: &[(String, bool)]) -> Vec<u8> {
    osc::encode(clockwork_sys!("gamepad/devices.reply"), &devices_args(rows))
}

/// `/clockwork/gamepad/devices …` — same payload, broadcast to subscribers on a
/// connect/disconnect/enable change.
pub fn encode_devices(rows: &[(String, bool)]) -> Vec<u8> {
    osc::encode(clockwork_sys!("gamepad/devices"), &devices_args(rows))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_event_carries_the_moment_it_was_seen_as_a_trailing_timetag() {
        let plain = osc::decode(&encode_button("pad", "a", true, 1.0)).unwrap();
        let at = osc::decode(&encode_button_at("pad", "a", true, 1.0, 77)).unwrap();
        assert_eq!(at.args.len(), plain.args.len() + 1);
        assert_eq!(&at.args[..plain.args.len()], &plain.args[..]);
        assert_eq!(at.args.last(), Some(&OscArg::TimeTag(77)));
        let axis = osc::decode(&encode_axis_at("pad", "left_x", -0.5, 78)).unwrap();
        assert_eq!(axis.args.last(), Some(&OscArg::TimeTag(78)));
        assert_eq!(encode_axis_at("pad", "left_x", -0.5, 0), encode_axis("pad", "left_x", -0.5));
    }

    #[test]
    fn decodes_rumble() {
        let bytes = osc::encode(
            clockwork_sys!("gamepad/out/rumble"),
            &[
                OscArg::Str("xbox_controller".into()),
                OscArg::Float(0.8),
                OscArg::Float(0.3),
                OscArg::Int(250),
            ],
        );
        assert_eq!(
            decode_out(&bytes),
            Some(OutCommand::Rumble {
                pad: "xbox_controller".into(),
                strong: 0.8,
                weak: 0.3,
                duration_ms: 250,
            })
        );
    }

    #[test]
    fn rumble_magnitudes_accept_ints_and_clamp() {
        let bytes = osc::encode(
            clockwork_sys!("gamepad/out/rumble"),
            &[
                OscArg::Str("*".into()),
                OscArg::Int(1),
                OscArg::Float(7.5),
                OscArg::Int(0),
            ],
        );
        assert_eq!(
            decode_out(&bytes),
            Some(OutCommand::Rumble { pad: "*".into(), strong: 1.0, weak: 1.0, duration_ms: 0 })
        );
    }

    #[test]
    fn decodes_management_verbs() {
        let en = osc::encode(
            clockwork_sys!("gamepad/enable"),
            &[OscArg::Str("*".into()), OscArg::Int(0)],
        );
        assert_eq!(
            decode_out(&en),
            Some(OutCommand::Enable { pad: "*".into(), enabled: false })
        );
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("gamepad/devices/list"), &[])),
            Some(OutCommand::DevicesList)
        );
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("gamepad/refresh"), &[])),
            Some(OutCommand::Refresh)
        );
        assert_eq!(
            decode_out(&osc::encode(clockwork_sys!("gamepad/notify/subscribe"), &[])),
            Some(OutCommand::Subscribe)
        );
        let stop = osc::encode(clockwork_sys!("gamepad/out/rumble_stop"), &[OscArg::Str("p".into())]);
        assert_eq!(decode_out(&stop), Some(OutCommand::RumbleStop { pad: "p".into() }));
    }

    #[test]
    fn malformed_and_foreign_decode_to_none() {
        // Arity is checked before use: a bare address reaches the engine from
        // arbitrary OSC clients and must be rejected, not panic (a panic would
        // unwind across the C ABI and abort the host process).
        assert_eq!(decode_out(&osc::encode(clockwork_sys!("gamepad/out/rumble"), &[])), None);
        assert_eq!(decode_out(&osc::encode(clockwork_sys!("gamepad/enable"), &[OscArg::Str("p".into())])), None);
        assert_eq!(decode_out(&osc::encode(clockwork_sys!("midi/refresh"), &[])), None);
        assert_eq!(decode_out(&[]), None);
    }

    #[test]
    fn encodes_inbound_events() {
        let b = encode_button("pad", "south", true, 1.0);
        let m = osc::decode(&b).unwrap();
        assert_eq!(m.addr, clockwork_sys!("gamepad/in/button"));
        assert_eq!(m.args[0], OscArg::Str("pad".into()));
        assert_eq!(m.args[1], OscArg::Str("south".into()));
        assert_eq!(m.args[2], OscArg::Int(1));
        assert_eq!(m.args[3], OscArg::Float(1.0));

        let a = encode_axis("pad", "left_x", -0.5);
        let m = osc::decode(&a).unwrap();
        assert_eq!(m.addr, clockwork_sys!("gamepad/in/axis"));
        assert_eq!(m.args[2], OscArg::Float(-0.5));
    }

    #[test]
    fn devices_reply_roundtrips() {
        let bytes = encode_devices_reply(&[("a".into(), true), ("b".into(), false)]);
        let m = osc::decode(&bytes).unwrap();
        assert_eq!(m.addr, clockwork_sys!("gamepad/devices.reply"));
        assert_eq!(m.args[0], OscArg::Int(2));
        assert_eq!(m.args[1], OscArg::Str("a".into()));
        assert_eq!(m.args[2], OscArg::Int(1));
        assert_eq!(m.args[4], OscArg::Int(0));
    }
}
