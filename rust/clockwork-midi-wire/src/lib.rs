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

/// The longest message this crate carries.
pub const MAX_MIDI_BYTES: usize = 3;

const PREFIX_IN: &[u8] = b"/clockwork/midi/in/";
const PREFIX_OUT: &[u8] = b"/clockwork/midi/out/";
const PORTS: &[u8] = b"/clockwork/midi/ports";
const PORTS_REPLY: &[u8] = b"/clockwork/midi/ports.reply";

/// A writer of OSC into a slice: `None` from any step once it has no room.
struct Writer<'a> {
    out: &'a mut [u8],
    at: usize,
}

impl Writer<'_> {
    fn bytes(&mut self, b: &[u8]) -> Option<()> {
        let end = self.at.checked_add(b.len())?;
        self.out.get_mut(self.at..end)?.copy_from_slice(b);
        self.at = end;
        Some(())
    }
    /// A string and its NULs: at least one, to a multiple of four.
    fn string(&mut self, parts: &[&[u8]]) -> Option<()> {
        let start = self.at;
        for p in parts {
            self.bytes(p)?;
        }
        let pad = 4 - ((self.at - start) & 3);
        self.bytes(&[0u8; 4][..pad])
    }
    fn int(&mut self, v: i32) -> Option<()> {
        self.bytes(&v.to_be_bytes())
    }
    fn timetag(&mut self, v: u64) -> Option<()> {
        self.bytes(&v.to_be_bytes())
    }
}

/// A short channel message as its inbound OSC form, written to `out`: the
/// bytes written, or `None` for bytes that are not one whole channel message
/// (a system message, a data byte first, one cut short), a port name with a
/// NUL in it, or a buffer too small. `when` is the moment the bytes arrived,
/// an OSC timetag, sent as a trailing `t`; 0 leaves it off.
///
/// A note-on of velocity 0 is a note-off, as `clockwork-midi` reads it.
pub fn encode_in(port: &str, midi: &[u8], when: u64, out: &mut [u8]) -> Option<usize> {
    if port.as_bytes().contains(&0) {
        return None;
    }
    let status = *midi.first()?;
    if !(0x80..0xF0).contains(&status) {
        return None;
    }
    let channel = i32::from(status & 0x0f) + 1;
    let d = |i: usize| midi.get(i).map(|b| i32::from(b & 0x7f));
    let (kind, values, n): (&[u8], [i32; 2], usize) = match status & 0xf0 {
        0x80 => (b"note_off", [d(1)?, d(2)?], 2),
        0x90 if d(2)? == 0 => (b"note_off", [d(1)?, 0], 2),
        0x90 => (b"note_on", [d(1)?, d(2)?], 2),
        0xA0 => (b"poly_pressure", [d(1)?, d(2)?], 2),
        0xB0 => (b"control_change", [d(1)?, d(2)?], 2),
        0xC0 => (b"program_change", [d(1)?, 0], 1),
        0xD0 => (b"channel_pressure", [d(1)?, 0], 1),
        _ => (b"pitch_bend", [(d(2)? << 7) | d(1)?, 0], 1),
    };
    let mut w = Writer { out, at: 0 };
    w.string(&[PREFIX_IN, kind])?;
    let tags: &[u8] = match (n, when != 0) {
        (2, false) => b",siii",
        (2, true) => b",siiit",
        (_, false) => b",sii",
        (_, true) => b",siit",
    };
    w.string(&[tags])?;
    w.string(&[port.as_bytes()])?;
    w.int(channel)?;
    for v in values.iter().take(n) {
        w.int(*v)?;
    }
    if when != 0 {
        w.timetag(when)?;
    }
    Some(w.at)
}

/// The ports a host supplies, as the port list's OSC form (`clockwork-midi`'s
/// `encode_ports`): `<nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`,
/// at `/clockwork/midi/ports.reply` for an answer to one who asked and
/// `/clockwork/midi/ports` for a notice. A host's port is always enabled.
pub fn encode_ports(input: Option<&str>, output: Option<&str>, reply: bool, out: &mut [u8]) -> Option<usize> {
    fn named(p: Option<&str>) -> Option<&str> {
        p.filter(|s| !s.is_empty() && !s.as_bytes().contains(&0))
    }
    let (input, output) = (named(input), named(output));
    let mut w = Writer { out, at: 0 };
    w.string(&[if reply { PORTS_REPLY } else { PORTS }])?;
    let tags: &[u8] = match (input.is_some(), output.is_some()) {
        (false, false) => b",ii",
        (true, false) => b",isii",
        (false, true) => b",iisi",
        (true, true) => b",isiisi",
    };
    w.string(&[tags])?;
    for port in [input, output] {
        w.int(i32::from(port.is_some()))?;
        if let Some(name) = port {
            w.string(&[name.as_bytes()])?;
            w.int(1)?;
        }
    }
    Some(w.at)
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

/// A reader of one OSC message's arguments, in place.
struct Args<'a> {
    tags: &'a [u8],
    data: &'a [u8],
}

enum Arg<'a> {
    Str(&'a str),
    Num(i32),
    Time(u64),
    Other,
}

fn padded(n: usize) -> usize {
    (n + 4) & !3
}

/// A NUL-terminated, padded string at the head of `data`: it, and what follows.
fn take_string(data: &[u8]) -> Option<(&[u8], &[u8])> {
    let len = data.iter().position(|b| *b == 0)?;
    let rest = data.get(padded(len)..)?;
    Some((&data[..len], rest))
}

impl<'a> Args<'a> {
    fn fixed<const N: usize>(&mut self) -> Option<[u8; N]> {
        let (head, rest) = self.data.split_at_checked(N)?;
        self.data = rest;
        head.try_into().ok()
    }
    fn next(&mut self) -> Option<Arg<'a>> {
        let (tag, tags) = self.tags.split_first()?;
        self.tags = tags;
        Some(match tag {
            b's' | b'S' => {
                let (s, rest) = take_string(self.data)?;
                self.data = rest;
                Arg::Str(core::str::from_utf8(s).ok()?)
            }
            b'i' => Arg::Num(i32::from_be_bytes(self.fixed()?)),
            b'h' => Arg::Num(i64::from_be_bytes(self.fixed()?) as i32),
            b'f' => Arg::Num(f32::from_be_bytes(self.fixed()?) as i32),
            b'd' => Arg::Num(f64::from_be_bytes(self.fixed()?) as i32),
            b't' => Arg::Time(u64::from_be_bytes(self.fixed()?)),
            b'b' => {
                let len = u32::from_be_bytes(self.fixed()?) as usize;
                self.data = self.data.get(len.checked_add(3)? & !3..)?;
                Arg::Other
            }
            b'T' | b'F' | b'N' | b'I' => Arg::Other,
            _ => return None,
        })
    }
    fn num(&mut self) -> Option<i32> {
        match self.next()? {
            Arg::Num(v) => Some(v),
            _ => None,
        }
    }
}

/// An outbound send read from its OSC message, or `None` for anything else:
/// an address that is not a send this crate carries (system exclusive, a
/// port's `enable`), or a send whose arguments cannot be read.
pub fn decode_out(osc: &[u8]) -> Option<Out<'_>> {
    let (address, rest) = take_string(osc)?;
    let kind = address.strip_prefix(PREFIX_OUT)?;
    let (tags, data) = take_string(rest)?;
    let mut args = Args { tags: tags.strip_prefix(b",")?, data };
    let port = match args.next()? {
        Arg::Str(s) => s,
        _ => return None,
    };
    let d7 = |v: i32| (v as u8) & 0x7f;
    let mut every_channel = false;
    let mut channel = |args: &mut Args| -> Option<u8> {
        // -1 is every channel, and so is 0: the schema's own sentinel for it.
        let c = args.num()?;
        every_channel = c == -1 || c as u8 == 0;
        Some(if every_channel { 0 } else { (c as u8).clamp(1, 16) - 1 })
    };
    let (bytes, len): ([u8; MAX_MIDI_BYTES], usize) = match kind {
        b"note_on" => ([0x90 | channel(&mut args)?, d7(args.num()?), d7(args.num()?)], 3),
        b"note_off" => ([0x80 | channel(&mut args)?, d7(args.num()?), d7(args.num()?)], 3),
        b"poly_pressure" => ([0xA0 | channel(&mut args)?, d7(args.num()?), d7(args.num()?)], 3),
        b"control_change" => ([0xB0 | channel(&mut args)?, d7(args.num()?), d7(args.num()?)], 3),
        b"program_change" => ([0xC0 | channel(&mut args)?, d7(args.num()?), 0], 2),
        b"channel_pressure" => ([0xD0 | channel(&mut args)?, d7(args.num()?), 0], 2),
        b"pitch_bend" => {
            let c = channel(&mut args)?;
            let v = args.num()?.clamp(0, 16383);
            ([0xE0 | c, (v & 0x7f) as u8, (v >> 7) as u8], 3)
        }
        b"clock" => ([0xF8, 0, 0], 1),
        b"start" => ([0xFA, 0, 0], 1),
        b"continue" => ([0xFB, 0, 0], 1),
        b"stop" => ([0xFC, 0, 0], 1),
        b"raw" => {
            // Bytes as numbers, three at most; a blob, or more, is not carried.
            let mut bytes = [0u8; MAX_MIDI_BYTES];
            let mut len = 0;
            while let Some(b'i' | b'h' | b'f' | b'd') = args.tags.first() {
                *bytes.get_mut(len)? = args.num()? as u8;
                len += 1;
            }
            if len == 0 || matches!(args.tags.first(), Some(b'b')) {
                return None;
            }
            (bytes, len)
        }
        _ => return None,
    };
    // A timetag at the end is the send's own time; no other argument is one.
    let mut when = 0;
    while !args.tags.is_empty() {
        when = match args.next()? {
            Arg::Time(t) => t,
            _ => 0,
        };
    }
    Some(Out { port, bytes, len, every_channel, when })
}

/// Both conversions over the same bytes, for the fuzzer and the corpus test:
/// as a port's message and as a client's send. What each makes of bytes it
/// takes is checked; bytes it does not take are refused without a panic.
pub fn exercise(data: &[u8]) {
    let mut out = [0u8; 160];
    if let Some(n) = encode_in("port", data, 1, &mut out) {
        assert!(n <= out.len() && n % 4 == 0, "an event is a whole OSC message");
        assert!(out.starts_with(PREFIX_IN), "an event is under the inbound prefix");
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
