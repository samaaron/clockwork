// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A short MIDI message to and from its `/clockwork/midi/*` OSC form, with no
//! allocation.
//!
//! The device subsystem (`clockwork-midi`) converts with `Vec`s on threads of
//! its own. A host that supplies the MIDI itself (a DAW handing a plugin its
//! events, a board reading a UART) has the bytes on the audio thread, each
//! with its frame, and wants them out on the audio thread too, so the
//! conversion is made there: into a buffer the caller owns, from a message
//! read in place.
//!
//! The forms are `clockwork-midi`'s schema exactly, and `tests/schema.rs`
//! holds the two together byte for byte:
//!
//! * in:  `/clockwork/midi/in/<kind> <port:s> <channel:i> <values:i…> [<when:t>]`
//! * out: `/clockwork/midi/out/<kind> <port:s> <channel:i> <values:i…> [<when:t>]`
//!
//! Channels are 1 to 16; an outbound channel of -1 is every channel. Short
//! messages only: the seven channel messages, and out, the four single-byte
//! transport messages and `raw` of up to three bytes. System exclusive is not
//! carried: it has no bound, and a store on the audio thread has one.

#![cfg_attr(not(test), no_std)]

pub mod ffi;

use clockwork_osc::wire::{self, Arg, Args, Message};
use clockwork_osc::{clockwork_sys, Timetag};

/// The longest message this crate carries.
pub const MAX_MIDI_BYTES: usize = 3;

const PREFIX_IN: &str = clockwork_sys!("midi/in/");
const PREFIX_OUT: &str = clockwork_sys!("midi/out/");

/// A short channel message as its inbound OSC form, written to `out`: the
/// bytes written, or `None` for bytes that are not one whole channel message
/// (a system message, a data byte first, one cut short), a port name with a
/// NUL in it, or a buffer too small. `when` is the moment the bytes arrived,
/// an OSC timetag, sent as a trailing `t`; 0 leaves it off.
///
/// A note-on of velocity 0 is a note-off, as `clockwork-midi` reads it.
pub fn encode_in(port: &str, midi: &[u8], when: u64, out: &mut [u8]) -> Option<usize> {
    let status = *midi.first()?;
    if !(0x80..0xF0).contains(&status) {
        return None;
    }
    let channel = i32::from(status & 0x0f) + 1;
    let d = |i: usize| midi.get(i).map(|b| i32::from(b & 0x7f));
    let (address, values, n): (&str, [i32; 2], usize) = match status & 0xf0 {
        0x80 => (clockwork_sys!("midi/in/note_off"), [d(1)?, d(2)?], 2),
        0x90 if d(2)? == 0 => (clockwork_sys!("midi/in/note_off"), [d(1)?, 0], 2),
        0x90 => (clockwork_sys!("midi/in/note_on"), [d(1)?, d(2)?], 2),
        0xA0 => (clockwork_sys!("midi/in/poly_pressure"), [d(1)?, d(2)?], 2),
        0xB0 => (clockwork_sys!("midi/in/control_change"), [d(1)?, d(2)?], 2),
        0xC0 => (clockwork_sys!("midi/in/program_change"), [d(1)?, 0], 1),
        0xD0 => (clockwork_sys!("midi/in/channel_pressure"), [d(1)?, 0], 1),
        _ => (clockwork_sys!("midi/in/pitch_bend"), [(d(2)? << 7) | d(1)?, 0], 1),
    };
    let written = wire::message(out, address, |w| {
        w.str(port)?.int(channel)?;
        values[..n].iter().try_for_each(|&v| w.int(v).map(drop))?;
        if when != 0 {
            w.timetag(Timetag::from_ntp(when))?;
        }
        Ok(())
    });
    written.ok().map(<[u8]>::len)
}

/// The ports a host supplies, as the port list's OSC form (`clockwork-midi`'s
/// `encode_ports`): `<nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`,
/// at `/clockwork/midi/ports.reply` for an answer to one who asked and
/// `/clockwork/midi/ports` for a notice. A host's port is always enabled.
pub fn encode_ports(input: Option<&str>, output: Option<&str>, reply: bool, out: &mut [u8]) -> Option<usize> {
    fn named(p: Option<&str>) -> Option<&str> {
        p.filter(|s| !s.is_empty() && !s.as_bytes().contains(&0))
    }
    let address = if reply { clockwork_sys!("midi/ports.reply") } else { clockwork_sys!("midi/ports") };
    let written = wire::message(out, address, |w| {
        for port in [named(input), named(output)] {
            w.int(i32::from(port.is_some()))?;
            if let Some(name) = port {
                w.str(name)?.int(1)?;
            }
        }
        Ok(())
    });
    written.ok().map(<[u8]>::len)
}

/// An outbound send, read from its message in place.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Out<'a> {
    /// The port it names; `*` is every port.
    pub port: &'a str,
    /// The message. For `every_channel` its channel nibble is 0, and the same
    /// message goes to each of the 16.
    pub bytes: [u8; MAX_MIDI_BYTES],
    pub len: usize,
    pub every_channel: bool,
    /// A trailing timetag, if the sender put one on; 0 if not.
    pub when: u64,
}

/// The next argument as a number: any of OSC's four, as an int.
fn num(args: &mut Args<'_>) -> Option<i32> {
    args.next()?.i32()
}

/// An outbound send read from its OSC message, or `None` for anything else:
/// bytes that are not a whole OSC message, an address that is not a send this
/// crate carries (system exclusive, a port's `enable`), or a send whose
/// arguments cannot be read.
pub fn decode_out(osc: &[u8]) -> Option<Out<'_>> {
    let message = Message::parse(osc).ok()?;
    let kind = message.address().strip_prefix(PREFIX_OUT)?;
    let mut args = message.args();
    let port = args.next()?.str()?;
    let d7 = |v: i32| (v as u8) & 0x7f;
    let mut every_channel = false;
    let mut channel = |args: &mut Args<'_>| -> Option<u8> {
        // -1 is every channel, and so is 0: the schema's own sentinel for it.
        let c = num(args)?;
        every_channel = c == -1 || c as u8 == 0;
        Some(if every_channel { 0 } else { (c as u8).clamp(1, 16) - 1 })
    };
    let (bytes, len): ([u8; MAX_MIDI_BYTES], usize) = match kind {
        "note_on" => ([0x90 | channel(&mut args)?, d7(num(&mut args)?), d7(num(&mut args)?)], 3),
        "note_off" => ([0x80 | channel(&mut args)?, d7(num(&mut args)?), d7(num(&mut args)?)], 3),
        "poly_pressure" => ([0xA0 | channel(&mut args)?, d7(num(&mut args)?), d7(num(&mut args)?)], 3),
        "control_change" => ([0xB0 | channel(&mut args)?, d7(num(&mut args)?), d7(num(&mut args)?)], 3),
        "program_change" => ([0xC0 | channel(&mut args)?, d7(num(&mut args)?), 0], 2),
        "channel_pressure" => ([0xD0 | channel(&mut args)?, d7(num(&mut args)?), 0], 2),
        "pitch_bend" => {
            let c = channel(&mut args)?;
            let v = num(&mut args)?.clamp(0, 16383);
            ([0xE0 | c, (v & 0x7f) as u8, (v >> 7) as u8], 3)
        }
        "clock" => ([0xF8, 0, 0], 1),
        "start" => ([0xFA, 0, 0], 1),
        "continue" => ([0xFB, 0, 0], 1),
        "stop" => ([0xFC, 0, 0], 1),
        "raw" => {
            // Bytes as numbers, three at most; a blob, or more, is not carried.
            let mut bytes = [0u8; MAX_MIDI_BYTES];
            let mut len = 0;
            while let Some(v) = args.clone().next().and_then(Arg::i32) {
                args.next();
                *bytes.get_mut(len)? = v as u8;
                len += 1;
            }
            if len == 0 || matches!(args.clone().next(), Some(Arg::Blob(_))) {
                return None;
            }
            (bytes, len)
        }
        _ => return None,
    };
    // A timetag at the end is the send's own time; no other argument is one.
    let when = match args.last() {
        Some(Arg::TimeTag(t)) => t.ntp(),
        _ => 0,
    };
    Some(Out { port, bytes, len, every_channel, when })
}

/// Both conversions over the same bytes, for the fuzzer and the corpus test:
/// as a port's message and as a client's send. What each makes of bytes it
/// takes is checked; bytes it does not take are refused without a panic.
pub fn exercise(data: &[u8]) {
    let mut out = [0u8; 160];
    if let Some(n) = encode_in("port", data, 1, &mut out) {
        assert!(n <= out.len() && n % 4 == 0, "an event is a whole OSC message");
        assert!(out.starts_with(PREFIX_IN.as_bytes()), "an event is under the inbound prefix");
    }
    if let Some(send) = decode_out(data) {
        assert!((1..=MAX_MIDI_BYTES).contains(&send.len), "a send is one short message");
        assert!(!send.every_channel || send.bytes[0] & 0x0f == 0, "a send to every channel names none");
    }
}

#[cfg(test)]
mod corpus {
    // The device subsystem's corpus: the same client commands and port bytes
    // are what these conversions read.
    #[test]
    fn every_corpus_file_is_exercised() {
        let midi = concat!(env!("CARGO_MANIFEST_DIR"), "/../clockwork-midi");
        let n = clockwork_corpus::replay(midi, super::exercise);
        assert!(n > 0, "no corpus under rust/clockwork-midi/corpus");
    }
}
