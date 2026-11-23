// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! OSC 1.0, read and written in place with no allocation: every argument type,
//! nested arrays, bundles, and clockwork's uuid (`u`, sixteen bytes). The one
//! codec of the Rust tree, fit for the audio thread, where a guest's `dsp_osc`
//! and `dsp_process` run.
//!
//! READING borrows the packet. [`Message::parse`] and [`Packet::parse`] check
//! the whole of what they are given once; after that the address, the
//! arguments and a bundle's elements are read straight from the caller's bytes
//! and cannot fail. A string or a blob is a slice of the packet, so what is
//! read lives only as long as the bytes do: copy what is to be kept.
//!
//! WRITING fills a buffer the caller owns. [`Writer`] makes one message,
//! [`BundleWriter`] a bundle of them. Nothing grows: what does not fit is
//! refused with [`Unwritable::NoRoom`], and the message or bundle is left as it
//! was before the call, still whole and still writable.
//!
//! What is written is what reads back: an address without its `/`, or text
//! with a NUL in it, is refused ([`Unwritable`]) and not written wrong.
//! [`crate::exercise`] holds reading and writing to each other under the
//! fuzzer.

use crate::Timetag;

/// How deep arrays may nest in a message, and bundles in a packet. Bytes from
/// outside can nest as deep as they like; real OSC does not come close.
pub const MAX_DEPTH: u32 = 64;

const BUNDLE: &[u8] = b"#bundle\0";

/// A length rounded up to OSC's four-byte boundary.
const fn padded(len: usize) -> usize {
    (len + 3) & !3
}

// ── Reading ────────────────────────────────────────────────────────────────

/// Why bytes are not an OSC message or packet.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Malformed {
    /// A string (the address, the type tags, a string argument) that has no
    /// NUL, is not padded to four bytes, or is not UTF-8.
    Text,
    /// An address that does not begin with `/`.
    Address,
    /// Type tags that do not begin with `,`.
    Tags,
    /// A type tag this codec does not know.
    UnknownTag(u8),
    /// An argument whose bytes run past the end of the message.
    Truncated,
    /// A `]` with no `[` before it, or a `[` never closed.
    Array,
    /// Arrays or bundles nested deeper than [`MAX_DEPTH`].
    TooDeep,
    /// A bundle without its timetag, or an element whose size is not all there.
    Bundle,
}

/// A NUL-terminated string padded to four bytes, and what follows it.
fn text(data: &[u8]) -> Result<(&str, &[u8]), Malformed> {
    let end = data.iter().position(|&b| b == 0).ok_or(Malformed::Text)?;
    let s = core::str::from_utf8(&data[..end]).map_err(|_| Malformed::Text)?;
    let rest = data.get(padded(end + 1)..).ok_or(Malformed::Text)?;
    Ok((s, rest))
}

/// The next `N` bytes, and what follows them.
fn fixed<const N: usize>(data: &[u8]) -> Result<([u8; N], &[u8]), Malformed> {
    let (head, rest) = data.split_first_chunk::<N>().ok_or(Malformed::Truncated)?;
    Ok((*head, rest))
}

/// One argument that is not an array, by its tag, and what follows it.
fn scalar(tag: u8, data: &[u8]) -> Result<(Arg<'_>, &[u8]), Malformed> {
    Ok(match tag {
        b'i' => {
            let (b, rest) = fixed(data)?;
            (Arg::Int(i32::from_be_bytes(b)), rest)
        }
        b'f' => {
            let (b, rest) = fixed(data)?;
            (Arg::Float(f32::from_be_bytes(b)), rest)
        }
        b'h' => {
            let (b, rest) = fixed(data)?;
            (Arg::Long(i64::from_be_bytes(b)), rest)
        }
        b'd' => {
            let (b, rest) = fixed(data)?;
            (Arg::Double(f64::from_be_bytes(b)), rest)
        }
        b't' => {
            let (b, rest) = fixed(data)?;
            (Arg::TimeTag(Timetag::from_ntp(u64::from_be_bytes(b))), rest)
        }
        b'c' => {
            let (b, rest) = fixed(data)?;
            (Arg::Char(u32::from_be_bytes(b)), rest)
        }
        b'r' => {
            let (b, rest) = fixed(data)?;
            (Arg::Color(u32::from_be_bytes(b)), rest)
        }
        b'm' => {
            let (b, rest) = fixed(data)?;
            (Arg::Midi(u32::from_be_bytes(b)), rest)
        }
        b'T' => (Arg::Bool(true), data),
        b'F' => (Arg::Bool(false), data),
        b'N' => (Arg::Nil, data),
        b'I' => (Arg::Inf, data),
        b's' => {
            let (s, rest) = text(data)?;
            (Arg::Str(s), rest)
        }
        b'S' => {
            let (s, rest) = text(data)?;
            (Arg::Symbol(s), rest)
        }
        b'b' => {
            let (size, rest) = fixed(data)?;
            let size = usize::try_from(i32::from_be_bytes(size)).map_err(|_| Malformed::Truncated)?;
            let blob = rest.get(..size).ok_or(Malformed::Truncated)?;
            let rest = rest.get(padded(size)..).ok_or(Malformed::Truncated)?;
            (Arg::Blob(blob), rest)
        }
        b'u' => {
            let (hi, rest) = fixed(data)?;
            let (lo, rest) = fixed(rest)?;
            (Arg::Uuid(u64::from_be_bytes(hi), u64::from_be_bytes(lo)), rest)
        }
        other => return Err(Malformed::UnknownTag(other)),
    })
}

/// Every argument the tags name is there in `payload`, and the arrays close.
fn check(tags: &[u8], mut payload: &[u8]) -> Result<(), Malformed> {
    let mut depth = 0u32;
    for &tag in tags {
        match tag {
            b'[' if depth >= MAX_DEPTH => return Err(Malformed::TooDeep),
            b'[' => depth += 1,
            b']' if depth == 0 => return Err(Malformed::Array),
            b']' => depth -= 1,
            _ => payload = scalar(tag, payload)?.1,
        }
    }
    if depth == 0 { Ok(()) } else { Err(Malformed::Array) }
}

/// One argument of a message.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Arg<'a> {
    /// `i`
    Int(i32),
    /// `f`
    Float(f32),
    /// `h`
    Long(i64),
    /// `d`
    Double(f64),
    /// `t`
    TimeTag(Timetag),
    /// `c`
    Char(u32),
    /// `r`: RGBA, packed big-endian.
    Color(u32),
    /// `m`: a port id and three MIDI bytes, packed.
    Midi(u32),
    /// `T` and `F`, which carry no bytes.
    Bool(bool),
    /// `N`, which carries no bytes.
    Nil,
    /// `I`, which carries no bytes.
    Inf,
    /// `s`
    Str(&'a str),
    /// `S`
    Symbol(&'a str),
    /// `b`
    Blob(&'a [u8]),
    /// `u`: clockwork's own, a 128-bit uuid as its high and low halves.
    Uuid(u64, u64),
    /// `[` to its `]`: the arguments between them.
    Array(Args<'a>),
}

impl<'a> Arg<'a> {
    /// Any of the four numbers, as a double. A long past 2^53 loses its low bits.
    pub fn f64(self) -> Option<f64> {
        match self {
            Arg::Int(v) => Some(f64::from(v)),
            Arg::Long(v) => Some(v as f64),
            Arg::Float(v) => Some(f64::from(v)),
            Arg::Double(v) => Some(v),
            _ => None,
        }
    }

    /// An int or a long. Not a float: one that happens to be whole is still a float.
    pub fn i64(self) -> Option<i64> {
        match self {
            Arg::Int(v) => Some(i64::from(v)),
            Arg::Long(v) => Some(v),
            _ => None,
        }
    }

    /// Any of the four numbers, as an int: a float loses its fraction, a long
    /// its high bits. For a verb that takes a count or a flag from clients
    /// whose language has one kind of number.
    pub fn i32(self) -> Option<i32> {
        match self {
            Arg::Int(v) => Some(v),
            Arg::Long(v) => Some(v as i32),
            Arg::Float(v) => Some(v as i32),
            Arg::Double(v) => Some(v as i32),
            _ => None,
        }
    }

    /// A string or a symbol.
    pub fn str(self) -> Option<&'a str> {
        match self {
            Arg::Str(s) | Arg::Symbol(s) => Some(s),
            _ => None,
        }
    }

    pub fn blob(self) -> Option<&'a [u8]> {
        match self {
            Arg::Blob(b) => Some(b),
            _ => None,
        }
    }

    pub fn bool(self) -> Option<bool> {
        match self {
            Arg::Bool(b) => Some(b),
            _ => None,
        }
    }

    pub fn timetag(self) -> Option<Timetag> {
        match self {
            Arg::TimeTag(t) => Some(t),
            _ => None,
        }
    }
}

/// The arguments of a message, or of an array in one, in order. Made only from
/// a message that was checked, so reading one cannot fail.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Args<'a> {
    tags: &'a [u8],
    payload: &'a [u8],
}

impl<'a> Args<'a> {
    /// The array that opens at the front of `tags` (after its `[`): what is in
    /// it, and what follows its `]`.
    fn array(tags: &'a [u8], payload: &'a [u8]) -> Option<(Args<'a>, Args<'a>)> {
        let (mut depth, mut after) = (1u32, payload);
        for (at, &tag) in tags.iter().enumerate() {
            match tag {
                b'[' => depth += 1,
                b']' => {
                    depth -= 1;
                    if depth == 0 {
                        let inside = &payload[..payload.len() - after.len()];
                        return Some((Args { tags: &tags[..at], payload: inside }, Args { tags: &tags[at + 1..], payload: after }));
                    }
                }
                _ => after = scalar(tag, after).ok()?.1,
            }
        }
        None
    }
}

impl<'a> Iterator for Args<'a> {
    type Item = Arg<'a>;

    fn next(&mut self) -> Option<Arg<'a>> {
        let (&tag, tags) = self.tags.split_first()?;
        if tag == b'[' {
            let (inside, after) = Args::array(tags, self.payload)?;
            *self = after;
            return Some(Arg::Array(inside));
        }
        let (arg, payload) = scalar(tag, self.payload).ok()?;
        self.tags = tags;
        self.payload = payload;
        Some(arg)
    }
}

/// One OSC message, read where it lies.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Message<'a> {
    address: &'a str,
    tags: &'a str,
    payload: &'a [u8],
}

impl<'a> Message<'a> {
    /// The message in `bytes`, checked whole: its address, its type tags, and
    /// every argument they name. Bytes after the last argument are not read.
    pub fn parse(bytes: &'a [u8]) -> Result<Message<'a>, Malformed> {
        let message = Message::split(bytes)?;
        check(message.tags.as_bytes(), message.payload)?;
        Ok(message)
    }

    /// The address and the tags, with the arguments not yet checked.
    fn split(bytes: &'a [u8]) -> Result<Message<'a>, Malformed> {
        let (address, rest) = text(bytes)?;
        if !address.starts_with('/') {
            return Err(Malformed::Address);
        }
        let (tags, payload) = text(rest)?;
        let tags = tags.strip_prefix(',').ok_or(Malformed::Tags)?;
        Ok(Message { address, tags, payload })
    }

    pub fn address(&self) -> &'a str {
        self.address
    }

    /// The type tags, one character an argument, without their comma: `"ifs"`
    /// for an int, a float and a string. An array is its `[`, its arguments'
    /// tags and its `]`.
    pub fn tags(&self) -> &'a str {
        self.tags
    }

    pub fn args(&self) -> Args<'a> {
        Args { tags: self.tags.as_bytes(), payload: self.payload }
    }
}

/// A bundle: a time, and the packets due at it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Bundle<'a> {
    timetag: Timetag,
    elements: &'a [u8],
}

impl<'a> Bundle<'a> {
    pub fn timetag(&self) -> Timetag {
        self.timetag
    }

    pub fn elements(&self) -> Elements<'a> {
        Elements { rest: self.elements }
    }

    /// The bundle in `bytes`, if they open as one, with its elements not yet checked.
    fn split(bytes: &'a [u8]) -> Option<Result<Bundle<'a>, Malformed>> {
        let after = bytes.strip_prefix(BUNDLE)?;
        Some(match fixed(after) {
            Ok((timetag, elements)) => Ok(Bundle { timetag: Timetag::from_ntp(u64::from_be_bytes(timetag)), elements }),
            Err(_) => Err(Malformed::Bundle),
        })
    }

    /// The element at the front of `rest`, by its size, and what follows it.
    fn element(rest: &'a [u8]) -> Result<(&'a [u8], &'a [u8]), Malformed> {
        let (size, rest) = fixed(rest).map_err(|_| Malformed::Bundle)?;
        let size = usize::try_from(i32::from_be_bytes(size)).map_err(|_| Malformed::Bundle)?;
        rest.split_at_checked(size).ok_or(Malformed::Bundle)
    }
}

/// The packets in a bundle, in order. Made only from a bundle that was
/// checked, so reading one cannot fail.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Elements<'a> {
    rest: &'a [u8],
}

impl<'a> Iterator for Elements<'a> {
    type Item = Packet<'a>;

    fn next(&mut self) -> Option<Packet<'a>> {
        if self.rest.is_empty() {
            return None;
        }
        let (element, rest) = Bundle::element(self.rest).ok()?;
        self.rest = rest;
        match Bundle::split(element) {
            Some(bundle) => bundle.ok().map(Packet::Bundle),
            None => Message::split(element).ok().map(Packet::Message),
        }
    }
}

/// What arrives as one: a message, or a bundle of packets.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Packet<'a> {
    Message(Message<'a>),
    Bundle(Bundle<'a>),
}

impl<'a> Packet<'a> {
    /// The packet in `bytes`, checked whole: a bundle's every element, and
    /// theirs, as [`Message::parse`] checks a message.
    pub fn parse(bytes: &'a [u8]) -> Result<Packet<'a>, Malformed> {
        Packet::checked(bytes, 0)
    }

    fn checked(bytes: &'a [u8], depth: u32) -> Result<Packet<'a>, Malformed> {
        let Some(bundle) = Bundle::split(bytes) else {
            return Message::parse(bytes).map(Packet::Message);
        };
        if depth >= MAX_DEPTH {
            return Err(Malformed::TooDeep);
        }
        let bundle = bundle?;
        let mut rest = bundle.elements;
        while !rest.is_empty() {
            let (element, after) = Bundle::element(rest)?;
            Packet::checked(element, depth + 1)?;
            rest = after;
        }
        Ok(Packet::Bundle(bundle))
    }

    /// Every message in the packet, in order, each with the time it is due:
    /// its bundle's timetag, or `when` for a message that came on its own.
    pub fn each_message(&self, when: Timetag, each: &mut impl FnMut(Message<'a>, Timetag)) {
        match self {
            Packet::Message(message) => each(*message, when),
            Packet::Bundle(bundle) => {
                for element in bundle.elements() {
                    element.each_message(bundle.timetag(), each);
                }
            }
        }
    }

    /// The packet written into `buf` as this codec writes one: the same
    /// address, arguments and times, with nothing after a message's last
    /// argument. Never longer than the bytes it was read from.
    pub fn write<'b>(&self, buf: &'b mut [u8]) -> Result<&'b [u8], Unwritable> {
        match self {
            Packet::Message(message) => {
                let mut w = Writer::new(buf, message.address())?;
                message.args().try_for_each(|arg| w.arg(arg).map(drop))?;
                Ok(w.finish())
            }
            Packet::Bundle(bundle) => {
                let mut w = BundleWriter::new(buf, bundle.timetag())?;
                bundle.elements().try_for_each(|element| w.packet(&element).map(drop))?;
                Ok(w.finish())
            }
        }
    }
}

// ── Writing ────────────────────────────────────────────────────────────────

/// Why something was not written. Whatever was refused left no trace: the
/// message or bundle is as it was before the call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unwritable {
    /// The buffer has no room for it.
    NoRoom,
    /// An address that does not begin with `/`, or has a NUL in it.
    Address,
    /// A string with a NUL in it, which would read back cut short.
    Nul,
    /// Arrays or bundles nested deeper than [`MAX_DEPTH`], which would not read back.
    TooDeep,
    /// A blob or a bundle's element too long for the 32 bits its size is carried in.
    TooLong,
}

/// One message, written into a buffer the caller owns.
///
/// ```
/// use clockwork_osc::wire::{Arg, Message, Writer};
/// let mut buf = [0u8; 32];
/// let mut w = Writer::new(&mut buf, "/tau5/event").unwrap();
/// w.double(2.001).unwrap();
/// let bytes = w.finish();
/// let message = Message::parse(bytes).unwrap();
/// assert_eq!(message.address(), "/tau5/event");
/// assert_eq!(message.args().next(), Some(Arg::Double(2.001)));
/// ```
#[derive(Debug)]
pub struct Writer<'b> {
    buf: &'b mut [u8],
    /// Where the type tags begin: the address's padded length.
    tags_at: usize,
    /// How many type tags are written, their comma among them.
    tags: usize,
    /// The message's length so far.
    end: usize,
    /// How many arrays are open.
    depth: u32,
}

impl<'b> Writer<'b> {
    /// A message to `address` with no arguments yet.
    pub fn new(buf: &'b mut [u8], address: &str) -> Result<Writer<'b>, Unwritable> {
        if !address.starts_with('/') || address.contains('\0') {
            return Err(Unwritable::Address);
        }
        let tags_at = padded(address.len() + 1);
        let end = tags_at + 4;
        let message = buf.get_mut(..end).ok_or(Unwritable::NoRoom)?;
        message[..address.len()].copy_from_slice(address.as_bytes());
        message[address.len()..tags_at].fill(0);
        message[tags_at..].copy_from_slice(b",\0\0\0");
        Ok(Writer { buf, tags_at, tags: 1, end, depth: 0 })
    }

    /// The message's length so far.
    pub fn len(&self) -> usize {
        self.end
    }

    /// False: a message always has its address and its type tags.
    pub fn is_empty(&self) -> bool {
        false
    }

    /// The finished message.
    pub fn finish(self) -> &'b [u8] {
        let buf: &'b [u8] = self.buf;
        &buf[..self.end]
    }

    /// Where the arguments begin when `tags` type tags are written: after the
    /// tags, their NUL and the padding to four bytes.
    fn payload_at(&self, tags: usize) -> usize {
        self.tags_at + padded(tags + 1)
    }

    /// One more type tag and `bytes` of argument after what is there: where
    /// those bytes go. The arguments move up four bytes when the tags need the
    /// room. Checked before anything is touched.
    fn push(&mut self, tag: u8, bytes: usize) -> Result<usize, Unwritable> {
        let (from, to) = (self.payload_at(self.tags), self.payload_at(self.tags + 1));
        let at = self.end + (to - from);
        let end = at.checked_add(bytes).filter(|&end| end <= self.buf.len()).ok_or(Unwritable::NoRoom)?;
        if to > from {
            self.buf.copy_within(from..self.end, to);
            self.buf[from..to].fill(0);
        }
        self.buf[self.tags_at + self.tags] = tag;
        self.tags += 1;
        self.end = end;
        Ok(at)
    }

    /// Back to when there were `tags` type tags and the message was `end` long.
    fn rewind(&mut self, tags: usize, end: usize) {
        let (from, to) = (self.payload_at(self.tags), self.payload_at(tags));
        self.buf.copy_within(from..from + (end - to), to);
        self.buf[self.tags_at + tags..to].fill(0);
        self.tags = tags;
        self.end = end;
    }

    fn put<const N: usize>(&mut self, tag: u8, bytes: [u8; N]) -> Result<&mut Self, Unwritable> {
        let at = self.push(tag, N)?;
        self.buf[at..at + N].copy_from_slice(&bytes);
        Ok(self)
    }

    fn put_text(&mut self, tag: u8, s: &str) -> Result<&mut Self, Unwritable> {
        if s.contains('\0') {
            return Err(Unwritable::Nul);
        }
        let at = self.push(tag, padded(s.len() + 1))?;
        self.buf[at..at + s.len()].copy_from_slice(s.as_bytes());
        self.buf[at + s.len()..self.end].fill(0);
        Ok(self)
    }

    pub fn int(&mut self, v: i32) -> Result<&mut Self, Unwritable> {
        self.put(b'i', v.to_be_bytes())
    }

    pub fn float(&mut self, v: f32) -> Result<&mut Self, Unwritable> {
        self.put(b'f', v.to_be_bytes())
    }

    pub fn long(&mut self, v: i64) -> Result<&mut Self, Unwritable> {
        self.put(b'h', v.to_be_bytes())
    }

    pub fn double(&mut self, v: f64) -> Result<&mut Self, Unwritable> {
        self.put(b'd', v.to_be_bytes())
    }

    pub fn timetag(&mut self, v: Timetag) -> Result<&mut Self, Unwritable> {
        self.put(b't', v.ntp().to_be_bytes())
    }

    pub fn char(&mut self, v: u32) -> Result<&mut Self, Unwritable> {
        self.put(b'c', v.to_be_bytes())
    }

    pub fn color(&mut self, v: u32) -> Result<&mut Self, Unwritable> {
        self.put(b'r', v.to_be_bytes())
    }

    pub fn midi(&mut self, v: u32) -> Result<&mut Self, Unwritable> {
        self.put(b'm', v.to_be_bytes())
    }

    pub fn bool(&mut self, v: bool) -> Result<&mut Self, Unwritable> {
        self.put(if v { b'T' } else { b'F' }, [])
    }

    pub fn nil(&mut self) -> Result<&mut Self, Unwritable> {
        self.put(b'N', [])
    }

    pub fn inf(&mut self) -> Result<&mut Self, Unwritable> {
        self.put(b'I', [])
    }

    pub fn str(&mut self, v: &str) -> Result<&mut Self, Unwritable> {
        self.put_text(b's', v)
    }

    pub fn symbol(&mut self, v: &str) -> Result<&mut Self, Unwritable> {
        self.put_text(b'S', v)
    }

    pub fn blob(&mut self, v: &[u8]) -> Result<&mut Self, Unwritable> {
        let size = i32::try_from(v.len()).map_err(|_| Unwritable::TooLong)?;
        let at = self.push(b'b', 4 + padded(v.len()))?;
        self.buf[at..at + 4].copy_from_slice(&size.to_be_bytes());
        self.buf[at + 4..at + 4 + v.len()].copy_from_slice(v);
        self.buf[at + 4 + v.len()..self.end].fill(0);
        Ok(self)
    }

    pub fn uuid(&mut self, hi: u64, lo: u64) -> Result<&mut Self, Unwritable> {
        let at = self.push(b'u', 16)?;
        self.buf[at..at + 8].copy_from_slice(&hi.to_be_bytes());
        self.buf[at + 8..at + 16].copy_from_slice(&lo.to_be_bytes());
        Ok(self)
    }

    /// An array: `fill` writes what is in it. If `fill` fails, or the array
    /// does not fit, none of it is written.
    pub fn array(&mut self, fill: impl FnOnce(&mut Writer<'b>) -> Result<(), Unwritable>) -> Result<&mut Self, Unwritable> {
        if self.depth >= MAX_DEPTH {
            return Err(Unwritable::TooDeep);
        }
        let (tags, end) = (self.tags, self.end);
        self.depth += 1;
        let written = self.bracketed(fill);
        self.depth -= 1;
        if let Err(why) = written {
            self.rewind(tags, end);
            return Err(why);
        }
        Ok(self)
    }

    fn bracketed(&mut self, fill: impl FnOnce(&mut Writer<'b>) -> Result<(), Unwritable>) -> Result<(), Unwritable> {
        self.push(b'[', 0)?;
        fill(self)?;
        self.push(b']', 0)?;
        Ok(())
    }

    /// An argument as it was read: what [`Message::args`] gives, written back.
    pub fn arg(&mut self, arg: Arg<'_>) -> Result<&mut Self, Unwritable> {
        match arg {
            Arg::Int(v) => self.int(v),
            Arg::Float(v) => self.float(v),
            Arg::Long(v) => self.long(v),
            Arg::Double(v) => self.double(v),
            Arg::TimeTag(v) => self.timetag(v),
            Arg::Char(v) => self.char(v),
            Arg::Color(v) => self.color(v),
            Arg::Midi(v) => self.midi(v),
            Arg::Bool(v) => self.bool(v),
            Arg::Nil => self.nil(),
            Arg::Inf => self.inf(),
            Arg::Str(v) => self.str(v),
            Arg::Symbol(v) => self.symbol(v),
            Arg::Blob(v) => self.blob(v),
            Arg::Uuid(hi, lo) => self.uuid(hi, lo),
            Arg::Array(mut items) => self.array(|w| items.try_for_each(|item| w.arg(item).map(drop))),
        }
    }
}

/// One message written into `buf`: `fill` writes its arguments.
///
/// ```
/// use clockwork_osc::wire;
/// let mut buf = [0u8; 32];
/// let bytes = wire::message(&mut buf, "/tau5/spawn", |w| w.double(4.0).map(drop)).unwrap();
/// assert_eq!(bytes.len(), 24);
/// ```
pub fn message<'b>(
    buf: &'b mut [u8],
    address: &str,
    fill: impl FnOnce(&mut Writer<'b>) -> Result<(), Unwritable>,
) -> Result<&'b [u8], Unwritable> {
    let mut w = Writer::new(buf, address)?;
    fill(&mut w)?;
    Ok(w.finish())
}

/// How long a message is: its address, `args` type tags and `payload` bytes
/// of arguments. With [`text_room`] and [`blob_room`], the buffer a message
/// wants, for a caller that has to make one.
pub const fn room(address: &str, args: usize, payload: usize) -> usize {
    padded(address.len() + 1) + padded(args + 2) + payload
}

/// How long a string or a symbol is as an argument.
pub const fn text_room(s: &str) -> usize {
    padded(s.len() + 1)
}

/// How long a blob of `len` bytes is as an argument.
pub const fn blob_room(len: usize) -> usize {
    4 + padded(len)
}

/// What `write` makes in a buffer of `room` bytes, as a `Vec`: for a boundary
/// that hands bytes over by value, as wasm-bindgen's does. Nothing here grows
/// either: `room` is all `write` gets.
pub fn to_vec(
    room: usize,
    write: impl for<'b> FnOnce(&'b mut [u8]) -> Result<&'b [u8], Unwritable>,
) -> Result<alloc::vec::Vec<u8>, Unwritable> {
    let mut buf = alloc::vec![0u8; room];
    let len = write(&mut buf)?.len();
    buf.truncate(len);
    Ok(buf)
}

/// A bundle, written into a buffer the caller owns, an element at a time.
///
/// ```
/// use clockwork_osc::wire::{BundleWriter, Packet};
/// use clockwork_osc::Timetag;
/// let mut buf = [0u8; 64];
/// let mut bundle = BundleWriter::new(&mut buf, Timetag::IMMEDIATELY).unwrap();
/// bundle.message("/a", |w| w.int(1).map(drop)).unwrap();
/// bundle.message("/b", |w| w.str("two").map(drop)).unwrap();
/// let bytes = bundle.finish();
/// assert!(matches!(Packet::parse(bytes), Ok(Packet::Bundle(_))));
/// ```
#[derive(Debug)]
pub struct BundleWriter<'b> {
    buf: &'b mut [u8],
    /// The bundle's length so far.
    end: usize,
    /// How many bundles this one is inside.
    depth: u32,
}

impl<'b> BundleWriter<'b> {
    /// A bundle due at `timetag` with no elements yet.
    pub fn new(buf: &'b mut [u8], timetag: Timetag) -> Result<BundleWriter<'b>, Unwritable> {
        BundleWriter::nested(buf, timetag, 0)
    }

    fn nested(buf: &'b mut [u8], timetag: Timetag, depth: u32) -> Result<BundleWriter<'b>, Unwritable> {
        let end = BUNDLE.len() + 8;
        let head = buf.get_mut(..end).ok_or(Unwritable::NoRoom)?;
        head[..BUNDLE.len()].copy_from_slice(BUNDLE);
        head[BUNDLE.len()..].copy_from_slice(&timetag.ntp().to_be_bytes());
        Ok(BundleWriter { buf, end, depth })
    }

    /// The bundle's length so far.
    pub fn len(&self) -> usize {
        self.end
    }

    /// False: a bundle always has its header.
    pub fn is_empty(&self) -> bool {
        false
    }

    /// The finished bundle.
    pub fn finish(self) -> &'b [u8] {
        let buf: &'b [u8] = self.buf;
        &buf[..self.end]
    }

    /// The room after what is written, less the four bytes of an element's size.
    fn room(&mut self) -> Result<&mut [u8], Unwritable> {
        self.buf.get_mut(self.end + 4..).ok_or(Unwritable::NoRoom)
    }

    /// The `bytes` just written into [`BundleWriter::room`] become an element.
    fn close(&mut self, bytes: usize) -> Result<&mut Self, Unwritable> {
        let size = i32::try_from(bytes).map_err(|_| Unwritable::TooLong)?;
        self.buf[self.end..self.end + 4].copy_from_slice(&size.to_be_bytes());
        self.end += 4 + bytes;
        Ok(self)
    }

    /// A message as the next element: `fill` writes its arguments. If `fill`
    /// fails, or the message does not fit, none of it is written.
    pub fn message(
        &mut self,
        address: &str,
        fill: impl FnOnce(&mut Writer<'_>) -> Result<(), Unwritable>,
    ) -> Result<&mut Self, Unwritable> {
        let mut message = Writer::new(self.room()?, address)?;
        fill(&mut message)?;
        let bytes = message.len();
        self.close(bytes)
    }

    /// A bundle as the next element: `fill` writes its elements. If `fill`
    /// fails, or the bundle does not fit, none of it is written.
    pub fn bundle(
        &mut self,
        timetag: Timetag,
        fill: impl FnOnce(&mut BundleWriter<'_>) -> Result<(), Unwritable>,
    ) -> Result<&mut Self, Unwritable> {
        if self.depth + 1 >= MAX_DEPTH {
            return Err(Unwritable::TooDeep);
        }
        let depth = self.depth + 1;
        let mut bundle = BundleWriter::nested(self.room()?, timetag, depth)?;
        fill(&mut bundle)?;
        let bytes = bundle.len();
        self.close(bytes)
    }

    /// A packet as it was read, as the next element.
    pub fn packet(&mut self, packet: &Packet<'_>) -> Result<&mut Self, Unwritable> {
        match packet {
            Packet::Message(message) => {
                self.message(message.address(), |w| message.args().try_for_each(|arg| w.arg(arg).map(drop)))
            }
            Packet::Bundle(bundle) => {
                self.bundle(bundle.timetag(), |w| bundle.elements().try_for_each(|element| w.packet(&element).map(drop)))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::{format, string::{String, ToString}, vec, vec::Vec};

    // ── values that own what they hold, for generating messages and comparing
    //    what was read with what was written. Tests only: the codec has none. ──

    #[derive(Clone, Debug, PartialEq)]
    enum Value {
        Int(i32),
        Float(f32),
        Long(i64),
        Double(f64),
        TimeTag(u64),
        Char(u32),
        Color(u32),
        Midi(u32),
        Bool(bool),
        Nil,
        Inf,
        Str(String),
        Symbol(String),
        Blob(Vec<u8>),
        Uuid(u64, u64),
        Array(Vec<Value>),
    }

    #[derive(Clone, Debug, PartialEq)]
    struct Msg {
        addr: String,
        args: Vec<Value>,
    }

    #[derive(Clone, Debug, PartialEq)]
    enum Pkt {
        Message(Msg),
        Bundle(u64, Vec<Pkt>),
    }

    fn owned(arg: Arg<'_>) -> Value {
        match arg {
            Arg::Int(v) => Value::Int(v),
            Arg::Float(v) => Value::Float(v),
            Arg::Long(v) => Value::Long(v),
            Arg::Double(v) => Value::Double(v),
            Arg::TimeTag(v) => Value::TimeTag(v.ntp()),
            Arg::Char(v) => Value::Char(v),
            Arg::Color(v) => Value::Color(v),
            Arg::Midi(v) => Value::Midi(v),
            Arg::Bool(v) => Value::Bool(v),
            Arg::Nil => Value::Nil,
            Arg::Inf => Value::Inf,
            Arg::Str(v) => Value::Str(v.to_string()),
            Arg::Symbol(v) => Value::Symbol(v.to_string()),
            Arg::Blob(v) => Value::Blob(v.to_vec()),
            Arg::Uuid(hi, lo) => Value::Uuid(hi, lo),
            Arg::Array(items) => Value::Array(items.map(owned).collect()),
        }
    }

    fn owned_message(m: &Message<'_>) -> Msg {
        Msg { addr: m.address().to_string(), args: m.args().map(owned).collect() }
    }

    fn owned_packet(p: &Packet<'_>) -> Pkt {
        match p {
            Packet::Message(m) => Pkt::Message(owned_message(m)),
            Packet::Bundle(b) => Pkt::Bundle(b.timetag().ntp(), b.elements().map(|e| owned_packet(&e)).collect()),
        }
    }

    fn write_arg(w: &mut Writer<'_>, arg: &Value) -> Result<(), Unwritable> {
        match arg {
            Value::Int(v) => w.int(*v).map(drop),
            Value::Float(v) => w.float(*v).map(drop),
            Value::Long(v) => w.long(*v).map(drop),
            Value::Double(v) => w.double(*v).map(drop),
            Value::TimeTag(v) => w.timetag(Timetag::from_ntp(*v)).map(drop),
            Value::Char(v) => w.char(*v).map(drop),
            Value::Color(v) => w.color(*v).map(drop),
            Value::Midi(v) => w.midi(*v).map(drop),
            Value::Bool(v) => w.bool(*v).map(drop),
            Value::Nil => w.nil().map(drop),
            Value::Inf => w.inf().map(drop),
            Value::Str(v) => w.str(v).map(drop),
            Value::Symbol(v) => w.symbol(v).map(drop),
            Value::Blob(v) => w.blob(v).map(drop),
            Value::Uuid(hi, lo) => w.uuid(*hi, *lo).map(drop),
            Value::Array(items) => w.array(|w| items.iter().try_for_each(|a| write_arg(w, a))).map(drop),
        }
    }

    fn write_message<'b>(buf: &'b mut [u8], m: &Msg) -> Result<&'b [u8], Unwritable> {
        message(buf, &m.addr, |w| m.args.iter().try_for_each(|a| write_arg(w, a)))
    }

    fn write_element(b: &mut BundleWriter<'_>, p: &Pkt) -> Result<(), Unwritable> {
        match p {
            Pkt::Message(m) => b.message(&m.addr, |w| m.args.iter().try_for_each(|a| write_arg(w, a))).map(drop),
            Pkt::Bundle(timetag, elements) => {
                b.bundle(Timetag::from_ntp(*timetag), |b| elements.iter().try_for_each(|e| write_element(b, e))).map(drop)
            }
        }
    }

    fn write_packet<'b>(buf: &'b mut [u8], p: &Pkt) -> Result<&'b [u8], Unwritable> {
        match p {
            Pkt::Message(m) => write_message(buf, m),
            Pkt::Bundle(timetag, elements) => {
                let mut b = BundleWriter::new(buf, Timetag::from_ntp(*timetag))?;
                elements.iter().try_for_each(|e| write_element(&mut b, e))?;
                Ok(b.finish())
            }
        }
    }

    /// The message or packet, written with room to spare.
    fn written(m: &Msg) -> Vec<u8> {
        write_message(&mut vec![0xAAu8; 1 << 16], m).unwrap().to_vec()
    }
    fn written_packet(p: &Pkt) -> Vec<u8> {
        write_packet(&mut vec![0xAAu8; 1 << 16], p).unwrap().to_vec()
    }

    // ── seeded generator (deterministic; no dev-deps) ──────────────────────────

    struct Lcg(u64);
    impl Lcg {
        fn next(&mut self) -> u64 {
            self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            self.0
        }
        fn below(&mut self, n: u32) -> u32 {
            ((self.next() >> 33) as u32) % n
        }
    }

    // A value of each type with no NaN in it, so what is read back can be
    // compared with `==`; the floats' every bit has its own case below.
    fn gen_arg(rng: &mut Lcg, depth: u32) -> Value {
        let pick = rng.below(if depth < 3 { 16 } else { 15 });
        match pick {
            0 => Value::Int(rng.next() as i32),
            1 => Value::Float((rng.below(20000) as f32) / 100.0 - 100.0),
            2 => Value::Long(rng.next() as i64),
            3 => Value::Double((rng.below(20000) as f64) / 100.0 - 100.0),
            4 => Value::TimeTag(rng.next()),
            5 => Value::Char(rng.below(0x110000)),
            6 => Value::Color(rng.next() as u32),
            7 => Value::Midi(rng.next() as u32),
            8 => Value::Bool(rng.below(2) == 1),
            9 => Value::Nil,
            10 => Value::Inf,
            11 => Value::Str(gen_text(rng)),
            12 => Value::Symbol(gen_text(rng)),
            13 => Value::Blob((0..rng.below(10)).map(|_| rng.next() as u8).collect()),
            14 => Value::Uuid(rng.next(), rng.next()),
            _ => {
                let n = rng.below(4);
                Value::Array((0..n).map(|_| gen_arg(rng, depth + 1)).collect())
            }
        }
    }

    // Up to seven characters, now and then one that is more than a byte of UTF-8.
    fn gen_text(rng: &mut Lcg) -> String {
        let n = rng.below(8);
        (0..n).map(|_| if rng.below(8) == 0 { 'é' } else { (b'a' + (rng.below(26) as u8)) as char }).collect()
    }

    fn gen_message(rng: &mut Lcg) -> Msg {
        let args = (0..rng.below(6)).map(|_| gen_arg(rng, 0)).collect();
        Msg { addr: format!("/{}", gen_text(rng)), args }
    }

    fn gen_packet(rng: &mut Lcg, depth: u32) -> Pkt {
        if depth < 3 && rng.below(3) == 0 {
            let n = rng.below(4);
            Pkt::Bundle(rng.next(), (0..n).map(|_| gen_packet(rng, depth + 1)).collect())
        } else {
            Pkt::Message(gen_message(rng))
        }
    }

    /// Miri interprets every instruction, so the loops below — which run in
    /// well under a second natively — take hours under it. They are scaled
    /// down there rather than skipped: what Miri is being asked for is the
    /// code path, and that is covered by the first few hundred iterations.
    /// The full counts still run on every ordinary `cargo test`.
    const fn scaled(full: u64, under_miri: u64) -> u64 {
        if cfg!(miri) { under_miri } else { full }
    }

    // ── the wire, byte for byte: worked out by hand, so a change to the format
    //    is caught as that and not only as the codec disagreeing with itself ──

    #[test]
    fn an_int_is_four_bytes_big_endian_after_the_padded_address_and_tags() {
        let mut buf = [0xAAu8; 16];
        assert_eq!(
            message(&mut buf, "/a", |w| w.int(1).map(drop)).unwrap(),
            [
                b'/', b'a', 0, 0, // address, NUL, pad to 4
                b',', b'i', 0, 0, // type tags, pad to 4
                0, 0, 0, 1, // int32 big-endian
            ]
        );
    }

    #[test]
    fn true_false_nil_and_inf_are_their_tags_and_no_bytes() {
        let mut buf = [0xAAu8; 16];
        assert_eq!(
            message(&mut buf, "/x", |w| w.bool(true)?.bool(false)?.nil()?.inf().map(drop)).unwrap(),
            [
                b'/', b'x', 0, 0, //
                b',', b'T', b'F', b'N', b'I', 0, 0, 0, // ",TFNI" + NUL, pad to 8
            ]
        );
    }

    #[test]
    fn a_string_is_padded_and_a_blob_is_its_size_then_its_padded_bytes() {
        let mut buf = [0xAAu8; 24];
        assert_eq!(
            message(&mut buf, "/s", |w| w.str("hi")?.blob(&[1, 2, 3]).map(drop)).unwrap(),
            [
                b'/', b's', 0, 0, //
                b',', b's', b'b', 0, //
                b'h', b'i', 0, 0, // "hi" + NUL + pad
                0, 0, 0, 3, // blob length
                1, 2, 3, 0, // blob bytes + pad to 4
            ]
        );
    }

    #[test]
    fn a_uuid_is_sixteen_big_endian_bytes_tagged_u() {
        let mut buf = [0xAAu8; 24];
        let bytes = message(&mut buf, "/x", |w| w.uuid(1, 2).map(drop)).unwrap();
        // "/x\0\0" + ",u\0\0" + 16 bytes
        assert_eq!(bytes.len(), 4 + 4 + 16);
        assert_eq!(&bytes[4..6], b",u");
        assert_eq!(&bytes[8..16], &1u64.to_be_bytes());
        assert_eq!(&bytes[16..24], &2u64.to_be_bytes());
        assert_eq!(Message::parse(bytes).unwrap().args().next(), Some(Arg::Uuid(1, 2)));
        assert_eq!(Message::parse(&bytes[..20]).err(), Some(Malformed::Truncated), "a uuid cut short is refused, not misread");
    }

    // ── the wire, from the OSC 1.0 specification's own examples ──────────────────

    const SPEC_OSCILLATOR: &[u8] = &[
        0x2f, 0x6f, 0x73, 0x63, 0x69, 0x6c, 0x6c, 0x61, 0x74, 0x6f, 0x72, 0x2f, 0x34, 0x2f, 0x66, 0x72, 0x65, 0x71, 0x75,
        0x65, 0x6e, 0x63, 0x79, 0x00, 0x2c, 0x66, 0x00, 0x00, 0x43, 0xdc, 0x00, 0x00,
    ];
    const SPEC_FOO: &[u8] = &[
        0x2f, 0x66, 0x6f, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x69, 0x69, 0x73, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x03,
        0xe8, 0xff, 0xff, 0xff, 0xff, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x3f, 0x9d, 0xf3, 0xb6, 0x40, 0xb5,
        0xb2, 0x2d,
    ];

    #[test]
    fn the_specifications_examples_are_read() {
        let m = Message::parse(SPEC_OSCILLATOR).unwrap();
        assert_eq!(m.address(), "/oscillator/4/frequency");
        assert_eq!(m.tags(), "f");
        assert_eq!(m.args().collect::<Vec<_>>(), [Arg::Float(440.0)]);

        let m = Message::parse(SPEC_FOO).unwrap();
        assert_eq!(m.address(), "/foo");
        assert_eq!(m.tags(), "iisff");
        assert_eq!(
            m.args().collect::<Vec<_>>(),
            [Arg::Int(1000), Arg::Int(-1), Arg::Str("hello"), Arg::Float(1.234), Arg::Float(5.678)]
        );
    }

    #[test]
    fn the_specifications_examples_are_written() {
        let mut buf = [0xAAu8; 64];
        let mut w = Writer::new(&mut buf, "/oscillator/4/frequency").unwrap();
        w.float(440.0).unwrap();
        assert_eq!(w.finish(), SPEC_OSCILLATOR);

        let mut buf = [0xAAu8; 64];
        let mut w = Writer::new(&mut buf, "/foo").unwrap();
        w.int(1000).unwrap().int(-1).unwrap().str("hello").unwrap().float(1.234).unwrap().float(5.678).unwrap();
        assert_eq!(w.finish(), SPEC_FOO);
    }

    // ── every type ───────────────────────────────────────────────────────────────

    #[test]
    fn every_type_is_written_and_read_back_as_it_was() {
        let mut buf = [0xAAu8; 256];
        let mut w = Writer::new(&mut buf, "/all").unwrap();
        w.int(-7).unwrap().float(1.5).unwrap().long(1 << 40).unwrap().double(-2.5).unwrap();
        w.timetag(Timetag::from_ntp(0xDEAD_BEEF_0000_0001)).unwrap().char(0x41).unwrap().color(0x1122_3344).unwrap();
        w.midi(0x9040_0000).unwrap().bool(true).unwrap().bool(false).unwrap().nil().unwrap().inf().unwrap();
        w.str("hi").unwrap().symbol("sym").unwrap().blob(&[1, 2, 3, 4, 5]).unwrap().uuid(0x0011_2233_4455_6677, 0x8899_AABB_CCDD_EEFF).unwrap();
        w.array(|w| {
            w.int(1)?;
            w.array(|w| w.str("deep").map(drop))?;
            w.array(|_| Ok(()))?;
            Ok(())
        })
        .unwrap();
        w.int(9).unwrap();
        let bytes = w.finish();
        assert_eq!(bytes.len() % 4, 0);

        let m = Message::parse(bytes).unwrap();
        assert_eq!(m.tags(), "ifhdtcrmTFNIsSbu[i[s][]]i");
        let mut args = m.args();
        assert_eq!(args.next(), Some(Arg::Int(-7)));
        assert_eq!(args.next(), Some(Arg::Float(1.5)));
        assert_eq!(args.next(), Some(Arg::Long(1 << 40)));
        assert_eq!(args.next(), Some(Arg::Double(-2.5)));
        assert_eq!(args.next().and_then(|a| a.timetag()).map(Timetag::ntp), Some(0xDEAD_BEEF_0000_0001));
        assert_eq!(args.next(), Some(Arg::Char(0x41)));
        assert_eq!(args.next(), Some(Arg::Color(0x1122_3344)));
        assert_eq!(args.next(), Some(Arg::Midi(0x9040_0000)));
        assert_eq!(args.next(), Some(Arg::Bool(true)));
        assert_eq!(args.next(), Some(Arg::Bool(false)));
        assert_eq!(args.next(), Some(Arg::Nil));
        assert_eq!(args.next(), Some(Arg::Inf));
        assert_eq!(args.next(), Some(Arg::Str("hi")));
        assert_eq!(args.next(), Some(Arg::Symbol("sym")));
        assert_eq!(args.next(), Some(Arg::Blob(&[1, 2, 3, 4, 5])));
        assert_eq!(args.next(), Some(Arg::Uuid(0x0011_2233_4455_6677, 0x8899_AABB_CCDD_EEFF)));
        let Some(Arg::Array(mut outer)) = args.next() else { panic!("an array") };
        assert_eq!(outer.next(), Some(Arg::Int(1)));
        let Some(Arg::Array(mut inner)) = outer.next() else { panic!("an array in the array") };
        assert_eq!(inner.next(), Some(Arg::Str("deep")));
        assert_eq!(inner.next(), None);
        let Some(Arg::Array(mut empty)) = outer.next() else { panic!("an empty array") };
        assert_eq!(empty.next(), None);
        assert_eq!(outer.next(), None);
        assert_eq!(args.next(), Some(Arg::Int(9)));
        assert_eq!(args.next(), None);
        assert_eq!(args.next(), None, "and stays ended");
    }

    #[test]
    fn a_float_keeps_every_bit_a_not_a_number_included() {
        let (single, double) = (f32::from_bits(0x7FC0_1234), f64::from_bits(0x7FF8_0000_DEAD_BEEF));
        let mut buf = [0u8; 32];
        let mut w = Writer::new(&mut buf, "/n").unwrap();
        w.float(single).unwrap().double(double).unwrap();
        let m = Message::parse(w.finish()).unwrap();
        let mut args = m.args();
        let (Some(Arg::Float(a)), Some(Arg::Double(b))) = (args.next(), args.next()) else { panic!("two floats") };
        assert_eq!((a.to_bits(), b.to_bits()), (single.to_bits(), double.to_bits()));
    }

    #[test]
    fn a_message_with_no_arguments_has_an_empty_tag_string() {
        let mut buf = [0xAAu8; 16];
        let bytes = Writer::new(&mut buf, "/ping").unwrap().finish();
        assert_eq!(bytes, b"/ping\0\0\0,\0\0\0");
        let m = Message::parse(bytes).unwrap();
        assert_eq!((m.address(), m.tags(), m.args().next()), ("/ping", "", None));
    }

    #[test]
    fn an_argument_answers_as_the_number_text_or_bytes_it_is() {
        assert_eq!(Arg::Int(3).f64(), Some(3.0));
        assert_eq!(Arg::Long(-4).f64(), Some(-4.0));
        assert_eq!(Arg::Float(0.5).f64(), Some(0.5));
        assert_eq!(Arg::Double(2.001).f64(), Some(2.001));
        assert_eq!(Arg::Str("3").f64(), None);
        assert_eq!(Arg::Bool(true).f64(), None);
        assert_eq!(Arg::Int(3).i64(), Some(3));
        assert_eq!(Arg::Long(1 << 40).i64(), Some(1 << 40));
        assert_eq!(Arg::Float(3.0).i64(), None, "a float is not silently made an integer");
        assert_eq!(Arg::Str("a").str(), Some("a"));
        assert_eq!(Arg::Symbol("b").str(), Some("b"));
        assert_eq!(Arg::Int(1).str(), None);
        assert_eq!(Arg::Blob(&[1, 2]).blob(), Some(&[1u8, 2][..]));
        assert_eq!(Arg::Str("a").blob(), None);
        assert_eq!(Arg::Bool(true).bool(), Some(true));
        assert_eq!(Arg::Int(1).bool(), None);
        assert_eq!(Arg::Int(1).timetag(), None);
    }

    // ── what is refused, and why ─────────────────────────────────────────────────

    #[test]
    fn any_number_answers_as_an_int_and_nothing_else_does() {
        assert_eq!(Arg::Int(-3).i32(), Some(-3));
        assert_eq!(Arg::Long(7).i32(), Some(7));
        assert_eq!(Arg::Long(0x1_0000_0002).i32(), Some(2));
        assert_eq!(Arg::Float(7.9).i32(), Some(7));
        assert_eq!(Arg::Double(-7.9).i32(), Some(-7));
        assert_eq!(Arg::Str("7").i32(), None);
        assert_eq!(Arg::Bool(true).i32(), None);
    }

    #[test]
    fn a_message_is_as_long_as_its_room_says() {
        let blob = [9u8; 5];
        for name in ["", "a", "abc", "abcd", "abcdefg"] {
            for extra in 0..6usize {
                let need = room("/an/address", 2 + extra, text_room(name) + blob_room(blob.len()) + 4 * extra);
                let fill = |w: &mut Writer<'_>| {
                    w.str(name)?.blob(&blob)?;
                    (0..extra).try_for_each(|i| w.int(i as i32).map(drop))
                };
                let whole = to_vec(need, |b| message(b, "/an/address", fill)).unwrap();
                assert_eq!(whole.len(), need, "{name:?} and {extra} ints");
                assert_eq!(to_vec(need - 1, |b| message(b, "/an/address", fill)), Err(Unwritable::NoRoom));
            }
        }
    }

    #[test]
    fn a_malformed_message_is_refused_with_its_reason() {
        let cases: &[(&[u8], Malformed)] = &[
            (b"", Malformed::Text),
            (b"/a", Malformed::Text),                                     // no NUL
            (b"/a\0", Malformed::Text),                                   // NUL, but not padded to four
            (b"/a\0\0", Malformed::Text),                                 // no tag string at all
            (b"a\0\0\0,\0\0\0", Malformed::Address),
            (b"\0\0\0\0,\0\0\0", Malformed::Address),
            (b"/a\0\0i\0\0\0\0\0\0\x01", Malformed::Tags),               // tags without their comma
            (b"/\xFF\0\0,\0\0\0", Malformed::Text),                       // not UTF-8
            (b"/a\0\0,q\0\0", Malformed::UnknownTag(b'q')),
            (b"/a\0\0,\xC3\xA9\0", Malformed::UnknownTag(0xC3)),
            (b"/a\0\0,i\0\0\0\0\0", Malformed::Truncated),
            (b"/a\0\0,d\0\0\0\0\0\0", Malformed::Truncated),
            (&[b'/', b'a', 0, 0, b',', b'u', 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], Malformed::Truncated),
            (b"/a\0\0,s\0\0abc", Malformed::Text),                        // a string with no end
            (b"/a\0\0,b\0\0\0\0\0\x05abcd", Malformed::Truncated),       // a blob longer than what follows
            (b"/a\0\0,b\0\0\0\0\0\x05abcde", Malformed::Truncated),      // all there, but not its padding
            (b"/a\0\0,b\0\0\xFF\xFF\xFF\xFF", Malformed::Truncated),     // a negative length
            (b"/a\0\0,]\0\0", Malformed::Array),
            (b"/a\0\0,[\0\0", Malformed::Array),
            (b"/a\0\0,[]]\0\0\0\0", Malformed::Array),
        ];
        for (bytes, why) in cases {
            assert_eq!(Message::parse(bytes).err(), Some(*why), "{bytes:?}");
        }
    }

    #[test]
    fn a_malformed_bundle_is_refused_with_its_reason() {
        let cases: &[(&[u8], Malformed)] = &[
            (b"#bundle\0", Malformed::Bundle),                                              // no timetag
            (b"#bundle\0\0\0\0\0\0\0\0", Malformed::Bundle),                                // seven bytes of one
            (b"#bundle\0\0\0\0\0\0\0\0\x01\0\0", Malformed::Bundle),                        // half an element's size
            (b"#bundle\0\0\0\0\0\0\0\0\x01\0\0\0\x08/a\0\0", Malformed::Bundle),            // an element cut short
            (b"#bundle\0\0\0\0\0\0\0\0\x01\xFF\xFF\xFF\xFF", Malformed::Bundle),            // a negative size
            (b"#bundle\0\0\0\0\0\0\0\0\x01\0\0\0\0", Malformed::Text),                      // an empty element
            (b"#bundle\0\0\0\0\0\0\0\0\x01\0\0\0\x08/a\0\0,q\0\0", Malformed::UnknownTag(b'q')),
            (b"#bundle", Malformed::Text),                                                  // no NUL, so not a bundle: a bad message
        ];
        for (bytes, why) in cases {
            assert_eq!(Packet::parse(bytes).err(), Some(*why), "{bytes:?}");
        }
    }

    #[test]
    fn bytes_after_the_last_argument_are_left_unread_and_not_written_back() {
        let bytes = b"/a\0\0,i\0\0\0\0\0\x07junk";
        let m = Message::parse(bytes).unwrap();
        assert_eq!(m.args().collect::<Vec<_>>(), [Arg::Int(7)]);
        assert_eq!(Packet::Message(m).write(&mut [0u8; 16]).unwrap(), &bytes[..12]);
    }

    // ── nesting, to the cap and not past it ──────────────────────────────────────

    fn nested_arrays(depth: u32) -> Msg {
        let mut a = Value::Int(1);
        for _ in 0..depth {
            a = Value::Array(vec![a]);
        }
        Msg { addr: "/x".into(), args: vec![a] }
    }

    fn nested_bundles(depth: u32) -> Pkt {
        let mut p = Pkt::Message(Msg { addr: "/x".into(), args: vec![] });
        for _ in 0..depth {
            p = Pkt::Bundle(5, vec![p]);
        }
        p
    }

    // The same, as bytes put together by hand: what a sender with no cap would send.
    fn nested_array_bytes(depth: usize) -> Vec<u8> {
        let mut bytes = b"/x\0\0,".to_vec();
        bytes.extend(core::iter::repeat_n(b'[', depth));
        bytes.push(b'i');
        bytes.extend(core::iter::repeat_n(b']', depth));
        bytes.resize(padded(bytes.len() + 1), 0);
        bytes.extend_from_slice(&1i32.to_be_bytes());
        bytes
    }

    fn nested_bundle_bytes(depth: usize) -> Vec<u8> {
        let mut bytes = b"/x\0\0,\0\0\0".to_vec();
        for _ in 0..depth {
            let mut outer = BUNDLE.to_vec();
            outer.extend_from_slice(&5u64.to_be_bytes());
            outer.extend_from_slice(&(bytes.len() as i32).to_be_bytes());
            outer.extend_from_slice(&bytes);
            bytes = outer;
        }
        bytes
    }

    #[test]
    fn arrays_nest_to_the_cap_and_one_deeper_is_refused_read_or_written() {
        let mut buf = [0u8; 512];
        let at_cap = nested_arrays(MAX_DEPTH);
        let bytes = nested_array_bytes(MAX_DEPTH as usize);
        assert_eq!(owned_message(&Message::parse(&bytes).unwrap()), at_cap);
        assert_eq!(write_message(&mut buf, &at_cap), Ok(&bytes[..]));

        let past = nested_arrays(MAX_DEPTH + 1);
        assert_eq!(Message::parse(&nested_array_bytes(MAX_DEPTH as usize + 1)).err(), Some(Malformed::TooDeep));
        assert_eq!(write_message(&mut buf, &past), Err(Unwritable::TooDeep));
    }

    #[test]
    fn bundles_nest_to_the_cap_and_one_deeper_is_refused_read_or_written() {
        let mut buf = [0u8; 2048];
        let at_cap = nested_bundles(MAX_DEPTH);
        let bytes = nested_bundle_bytes(MAX_DEPTH as usize);
        assert_eq!(owned_packet(&Packet::parse(&bytes).unwrap()), at_cap);
        assert_eq!(write_packet(&mut buf, &at_cap), Ok(&bytes[..]));

        let past = nested_bundles(MAX_DEPTH + 1);
        assert_eq!(Packet::parse(&nested_bundle_bytes(MAX_DEPTH as usize + 1)).err(), Some(Malformed::TooDeep));
        assert_eq!(write_packet(&mut buf, &past), Err(Unwritable::TooDeep));
    }

    // ── bundles ──────────────────────────────────────────────────────────────────

    #[test]
    fn a_bundle_is_written_and_read_element_by_element() {
        let mut buf = [0xAAu8; 128];
        let mut b = BundleWriter::new(&mut buf, Timetag::from_ntp(1)).unwrap();
        b.message("/a", |w| w.int(1).map(drop)).unwrap();
        b.bundle(Timetag::from_ntp(0x10), |b| b.message("/b", |w| w.str("x").map(drop)).map(drop)).unwrap();
        b.message("/c", |_| Ok(())).unwrap();
        let bytes = b.finish();

        let mut expect = b"#bundle\0".to_vec();
        expect.extend_from_slice(&1u64.to_be_bytes());
        expect.extend_from_slice(&12i32.to_be_bytes());
        expect.extend_from_slice(b"/a\0\0,i\0\0\0\0\0\x01");
        expect.extend_from_slice(&32i32.to_be_bytes());
        expect.extend_from_slice(b"#bundle\0");
        expect.extend_from_slice(&0x10u64.to_be_bytes());
        expect.extend_from_slice(&12i32.to_be_bytes());
        expect.extend_from_slice(b"/b\0\0,s\0\0x\0\0\0");
        expect.extend_from_slice(&8i32.to_be_bytes());
        expect.extend_from_slice(b"/c\0\0,\0\0\0");
        assert_eq!(bytes, &expect[..]);

        let Packet::Bundle(top) = Packet::parse(bytes).unwrap() else { panic!("a bundle") };
        assert_eq!(top.timetag().ntp(), 1);
        let mut elements = top.elements();
        let Some(Packet::Message(a)) = elements.next() else { panic!("a message first") };
        assert_eq!((a.address(), a.args().next()), ("/a", Some(Arg::Int(1))));
        let Some(Packet::Bundle(inner)) = elements.next() else { panic!("a bundle second") };
        assert_eq!(inner.timetag().ntp(), 0x10);
        let Some(Packet::Message(b)) = inner.elements().next() else { panic!("a message in it") };
        assert_eq!((b.address(), b.args().next()), ("/b", Some(Arg::Str("x"))));
        let Some(Packet::Message(c)) = elements.next() else { panic!("a message third") };
        assert_eq!(c.address(), "/c");
        assert!(elements.next().is_none());
    }

    #[test]
    fn each_message_of_a_packet_comes_in_order_with_the_time_of_the_bundle_it_is_in() {
        let mut buf = [0u8; 256];
        let mut b = BundleWriter::new(&mut buf, Timetag::from_ntp(100)).unwrap();
        b.message("/first", |_| Ok(())).unwrap();
        b.bundle(Timetag::from_ntp(200), |b| {
            b.message("/second", |_| Ok(()))?;
            b.bundle(Timetag::from_ntp(300), |b| b.message("/third", |_| Ok(())).map(drop))?;
            Ok(())
        })
        .unwrap();
        b.message("/fourth", |_| Ok(())).unwrap();
        let packet = Packet::parse(b.finish()).unwrap();

        let mut seen = Vec::new();
        packet.each_message(Timetag::from_ntp(7), &mut |m, when| seen.push((m.address(), when.ntp())));
        assert_eq!(seen, [("/first", 100), ("/second", 200), ("/third", 300), ("/fourth", 100)]);

        // a message on its own is due when the caller says its packet is
        let mut buf = [0u8; 16];
        let alone = Packet::parse(Writer::new(&mut buf, "/alone").unwrap().finish()).unwrap();
        let mut seen = Vec::new();
        alone.each_message(Timetag::from_ntp(7), &mut |m, when| seen.push((m.address(), when.ntp())));
        assert_eq!(seen, [("/alone", 7)]);
    }

    // ── the writer, when there is no room or the thing cannot be written ─────────

    #[test]
    fn an_argument_that_does_not_fit_is_refused_and_what_was_written_stands() {
        for seed in 0..scaled(300, 10) {
            let mut rng = Lcg(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0x0F17);
            let m = gen_message(&mut rng);
            let whole = written(&m);
            let bare = written(&Msg { addr: m.addr.clone(), args: vec![] });
            for room in 0..=whole.len() {
                let mut buf = vec![0xAAu8; room];
                let Ok(mut w) = Writer::new(&mut buf, &m.addr) else {
                    assert!(room < bare.len(), "seed {seed}: the address and no arguments fit in {room}");
                    continue;
                };
                let mut written = 0;
                for arg in &m.args {
                    match write_arg(&mut w, arg) {
                        Ok(()) => written += 1,
                        Err(why) => {
                            assert_eq!(why, Unwritable::NoRoom, "seed {seed}, {room} bytes");
                            break;
                        }
                    }
                }
                assert_eq!(written == m.args.len(), room == whole.len(), "seed {seed}: all of it fits in exactly its own length");
                assert_eq!(w.finish(), &self::written(&Msg { addr: m.addr.clone(), args: m.args[..written].to_vec() })[..], "seed {seed}, {room} bytes: the arguments that fitted, as a whole message");
            }
        }
    }

    #[test]
    fn a_bundles_element_that_does_not_fit_is_refused_and_what_was_written_stands() {
        for seed in 0..scaled(300, 10) {
            let mut rng = Lcg(seed.wrapping_mul(0x1_0000_01B3) ^ 0xB0D1);
            let elements: Vec<Pkt> = (0..1 + rng.below(4)).map(|_| gen_packet(&mut rng, 1)).collect();
            let whole = written_packet(&Pkt::Bundle(9, elements.clone()));
            for room in 16..=whole.len() {
                let mut buf = vec![0xAAu8; room];
                let mut b = BundleWriter::new(&mut buf, Timetag::from_ntp(9)).unwrap();
                let mut written = 0;
                for e in &elements {
                    match write_element(&mut b, e) {
                        Ok(()) => written += 1,
                        Err(why) => {
                            assert_eq!(why, Unwritable::NoRoom, "seed {seed}, {room} bytes");
                            break;
                        }
                    }
                }
                assert_eq!(written == elements.len(), room == whole.len(), "seed {seed}");
                let kept = Pkt::Bundle(9, elements[..written].to_vec());
                assert_eq!(b.finish(), &written_packet(&kept)[..], "seed {seed}, {room} bytes");
            }
            assert_eq!(BundleWriter::new(&mut [0u8; 15], Timetag::from_ntp(9)).err(), Some(Unwritable::NoRoom));
        }
    }

    #[test]
    fn what_would_not_read_back_as_it_was_is_not_written() {
        let mut buf = [0u8; 64];
        assert_eq!(Writer::new(&mut buf, "no/slash").err(), Some(Unwritable::Address));
        assert_eq!(Writer::new(&mut buf, "").err(), Some(Unwritable::Address));
        assert_eq!(Writer::new(&mut buf, "/a\0b").err(), Some(Unwritable::Address));
        let mut w = Writer::new(&mut buf, "/a").unwrap();
        w.int(1).unwrap();
        assert_eq!(w.str("x\0y").err(), Some(Unwritable::Nul));
        assert_eq!(w.symbol("\0").err(), Some(Unwritable::Nul));
        // an array whose filling fails leaves no trace of itself
        assert_eq!(
            w.array(|w| {
                w.int(2)?.int(3)?.int(4)?.int(5)?;
                w.str("x\0y").map(drop)
            })
            .err(),
            Some(Unwritable::Nul)
        );
        w.int(6).unwrap();
        assert_eq!(w.finish(), b"/a\0\0,ii\0\0\0\0\x01\0\0\0\x06");

        let mut b = BundleWriter::new(&mut buf, Timetag::from_ntp(1)).unwrap();
        assert_eq!(b.message("nope", |_| Ok(())).err(), Some(Unwritable::Address));
        assert_eq!(b.message("/a", |w| w.str("\0").map(drop)).err(), Some(Unwritable::Nul));
        assert_eq!(b.finish(), b"#bundle\0\0\0\0\0\0\0\0\x01");
    }

    // ── everything there is to write, read back; and anything at all, read ───────

    #[test]
    fn every_generated_message_reads_back_as_what_was_written_and_rewrites_to_the_same_bytes() {
        let mut buf = vec![0u8; 4096];
        for seed in 0..scaled(5000, 100) {
            let mut rng = Lcg(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0xABCD);
            let m = gen_message(&mut rng);
            let bytes = written(&m);
            assert_eq!(bytes.len() % 4, 0, "seed {seed}");
            let read = Message::parse(&bytes).unwrap_or_else(|why| panic!("seed {seed}: {why:?}"));
            assert_eq!(owned_message(&read), m, "seed {seed}");
            assert_eq!(Packet::Message(read).write(&mut buf), Ok(&bytes[..]), "seed {seed}");
            // In exactly its own length, with whatever was in the buffer before.
            assert_eq!(write_message(&mut vec![0x55u8; bytes.len()], &m), Ok(&bytes[..]), "seed {seed}");
        }
    }

    #[test]
    fn every_generated_packet_reads_back_as_what_was_written_and_rewrites_to_the_same_bytes() {
        let mut buf = vec![0u8; 16384];
        for seed in 0..scaled(5000, 100) {
            let mut rng = Lcg(seed.wrapping_mul(0x1_0000_01B3) ^ 0x1234);
            let p = gen_packet(&mut rng, 0);
            let bytes = written_packet(&p);
            let read = Packet::parse(&bytes).unwrap_or_else(|why| panic!("seed {seed}: {why:?}"));
            assert_eq!(owned_packet(&read), p, "seed {seed}");
            assert_eq!(read.write(&mut buf), Ok(&bytes[..]), "seed {seed}");
            assert_eq!(write_packet(&mut vec![0x55u8; bytes.len()], &p), Ok(&bytes[..]), "seed {seed}");
        }
    }

    // What reads at all reads the same once written back: `crate::exercise`,
    // which is also what the fuzzer runs.
    #[test]
    fn arbitrary_bytes_are_refused_or_read_and_never_more() {
        let mut rng = Lcg(0xF022);
        let mut read = 0u32;
        for _ in 0..scaled(200_000, 500) {
            let len = rng.below(80) as usize;
            let mut bytes: Vec<u8> = (0..len).map(|_| rng.next() as u8).collect();
            match rng.below(4) {
                0 if bytes.len() >= 8 => bytes[..8].copy_from_slice(b"#bundle\0"),
                1 if bytes.len() >= 8 => bytes[..8].copy_from_slice(b"/a\0\0,ifs"),
                _ => {}
            }
            read += u32::from(Packet::parse(&bytes).is_ok());
            crate::exercise(&bytes);
        }
        assert!(cfg!(miri) || read > 100, "some of them read, or this looked at nothing: {read}");
    }

    #[test]
    fn every_truncation_and_every_damaged_packet_is_refused_or_read_and_never_more() {
        for seed in 0..scaled(2000, 20) {
            let mut rng = Lcg(seed ^ 0x5151);
            let bytes = written_packet(&gen_packet(&mut rng, 0));
            for cut in 0..=bytes.len() {
                crate::exercise(&bytes[..cut]);
            }
            // one byte changed, forty times over: a tag, a length, a terminator, a size
            for _ in 0..40 {
                let mut damaged = bytes.clone();
                let at = rng.below(damaged.len() as u32) as usize;
                damaged[at] = match rng.below(4) {
                    0 => 0,
                    1 => 0xFF,
                    2 => b"ifhdtcrmTFNIsSbu[],/#"[rng.below(21) as usize],
                    _ => rng.next() as u8,
                };
                crate::exercise(&damaged);
            }
        }
    }
}
