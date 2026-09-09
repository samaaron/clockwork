// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The handle, and everything reached through it.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;
use std::ffi::CString;
use std::time::{Duration, Instant};

use clockwork_abi::client::{
    clockwork_client_abi_version, clockwork_client_beat_at, clockwork_client_clock,
    clockwork_client_close, clockwork_client_default_endpoint, clockwork_client_info,
    clockwork_client_open_memory, clockwork_client_open_shm, clockwork_client_open_shm_handle,
    clockwork_client_poll, clockwork_client_region, clockwork_client_send,
    clockwork_client_send_abort, clockwork_client_send_begin, clockwork_client_send_commit,
    clockwork_client_tap_close, clockwork_client_tap_missed, clockwork_client_tap_open,
    clockwork_client_tap_poll, ClockworkClient, ClockworkClientClock, ClockworkClientInfo,
    ClockworkClientMessage, ClockworkClientTap, ClockworkRegion, ClockworkRegionId,
    ClockworkStatus, CLOCKWORK_FEATURE_AUDIO_TAPS, CLOCKWORK_FEATURE_BULK,
    CLOCKWORK_FEATURE_SCHEDULER, CLOCKWORK_FEATURE_SCOPE, CLOCKWORK_FEATURE_WINDOW,
    CLOCKWORK_ROUTE_NOTIFY, CLOCKWORK_ROUTE_REPLY,
};

use crate::error::{check, refused, Error, Result};
use crate::metrics::{Metrics, NativeStats};
use crate::scope::Scope;

/// How many messages one [`Client::poll`] takes at most, unless
/// [`Client::set_poll_batch`] says otherwise.
pub const POLL_BATCH: usize = 64;

/// An unfilled inbox slot.
const EMPTY_SLOT: ClockworkClientMessage = ClockworkClientMessage {
    bytes: core::ptr::null(),
    length: 0,
    origin: 0,
    route: 0,
    sequence: 0,
};

/// A handle over an engine's memory.
///
/// One handle is one thread's at a time: it is [`Send`] and not [`Sync`].
/// Two threads that both want to talk to the engine open two handles.
/// Draining is single-consumer — every handle onto one engine shares the
/// egress ring's read cursor — so exactly one handle polls.
pub struct Client {
    raw: NonNull<ClockworkClient>,
    /// Whether close is ours to call. An [`crate::Embed`]'s client is the
    /// handle's, closed with it.
    owned: bool,
    inbox: Vec<ClockworkClientMessage>,
}

// SAFETY: nothing in a handle is pinned to the thread that opened it — the
// C library keeps no thread-local state — so moving one is sound. Sharing
// one is not (clockwork_client.h, THREADS), which is why there is no Sync.
unsafe impl Send for Client {}

impl Client {
    /// The ABI version the linked library was built as. A binding checks it
    /// against `clockwork_abi::client::CLOCKWORK_CLIENT_ABI_VERSION` and
    /// refuses rather than misread a struct.
    pub fn abi_version() -> u32 {
        // SAFETY: no arguments, no state.
        unsafe { clockwork_client_abi_version() }
    }

    /// The endpoint an engine on `port` serves at unless told otherwise:
    /// under `$TMPDIR` on macOS and Linux, a named pipe on Windows.
    pub fn default_endpoint(port: u32) -> String {
        let mut buf = vec![0u8; 256];
        loop {
            // SAFETY: `buf` is live for the call and `cap` is its length;
            // the library writes NUL-terminated within it or nothing.
            let need = unsafe {
                clockwork_client_default_endpoint(port, buf.as_mut_ptr().cast(), buf.len() as u32)
            } as usize;
            if need < buf.len() {
                buf.truncate(need);
                return String::from_utf8_lossy(&buf).into_owned();
            }
            buf.resize(need + 1, 0);
        }
    }

    /// Attach to an engine in another process on this machine, through the
    /// endpoint it serves its segment from ([`Client::default_endpoint`], or
    /// what it was started with as `--shm-endpoint`). `NotFound` for no
    /// engine there, an engine started with `-u 0`, or a layout this build
    /// does not recognise.
    pub fn open_shm(endpoint: &str) -> Result<Client> {
        let endpoint = CString::new(endpoint).map_err(|_| Error::Arg)?;
        let mut st = ClockworkStatus::OK;
        // SAFETY: a NUL-terminated string that outlives the call, and a live
        // out-pointer.
        let raw = unsafe { clockwork_client_open_shm(endpoint.as_ptr(), &mut st) };
        Self::opened(raw, st, true)
    }

    /// Attach through a segment handle already received — an fd from a Unix
    /// socket, a HANDLE duplicated into this process. Owned by the client
    /// from here, whether or not the open succeeds.
    pub fn open_shm_handle(native_handle: isize) -> Result<Client> {
        let mut st = ClockworkStatus::OK;
        // SAFETY: a live out-pointer; the handle's validity is the caller's
        // claim and the library's to check.
        let raw = unsafe { clockwork_client_open_shm_handle(native_handle, &mut st) };
        Self::opened(raw, st, true)
    }

    /// A handle over an engine's arena in this address space. Nothing is
    /// mapped and nothing is owned.
    ///
    /// # Safety
    /// `base..base + bytes` is the engine's arena, readable — and writable
    /// where the engine says a client writes — for as long as the handle
    /// lives. An [`crate::Embed`] does this for you.
    pub unsafe fn open_memory(base: *mut c_void, bytes: u32) -> Result<Client> {
        let mut st = ClockworkStatus::OK;
        // SAFETY: the caller's contract, above, plus a live out-pointer.
        let raw = unsafe { clockwork_client_open_memory(base, bytes, &mut st) };
        Self::opened(raw, st, true)
    }

    pub(crate) fn opened(raw: *mut ClockworkClient, st: ClockworkStatus, owned: bool) -> Result<Client> {
        match NonNull::new(raw) {
            Some(raw) => Ok(Client { raw, owned, inbox: vec![EMPTY_SLOT; POLL_BATCH] }),
            None => Err(refused(st)),
        }
    }

    /// The raw handle, for a call this crate does not wrap. Owned by this
    /// wrapper: do not close it.
    pub fn as_ptr(&self) -> *mut ClockworkClient {
        self.raw.as_ptr()
    }

    /// What is on the other end.
    pub fn info(&self) -> Result<Info> {
        let mut raw = ClockworkClientInfo {
            struct_bytes: core::mem::size_of::<ClockworkClientInfo>() as u32,
            ..ClockworkClientInfo::default()
        };
        // SAFETY: a live handle and a struct that says its own size.
        check(unsafe { clockwork_client_info(self.raw.as_ptr(), &mut raw) })?;
        Ok(Info {
            abi_version: raw.abi_version,
            engine_version: raw.engine_version,
            features: Features(raw.features),
            sample_rate: raw.sample_rate,
            block_frames: raw.block_frames,
            input_channels: raw.input_channels,
            output_channels: raw.output_channels,
        })
    }

    /// One OSC message or bundle, copied into the ingress ring; the engine
    /// reads it on its next block. `origin` is echoed on any reply so a
    /// client with several conversations in flight can tell them apart;
    /// zero means "not distinguishing" and sends replies to the
    /// notification audience instead. [`Error::Full`] is a moment — retry;
    /// [`Error::TooBig`] is a verdict.
    pub fn send(&self, osc: &[u8], origin: u32) -> Result<()> {
        if osc.is_empty() {
            return Err(Error::Arg);
        }
        let bytes = u32::try_from(osc.len()).map_err(|_| Error::TooBig)?;
        // SAFETY: a live handle and `bytes` bytes of a live slice.
        check(unsafe { clockwork_client_send(self.raw.as_ptr(), osc.as_ptr(), bytes, origin) })
    }

    /// Build the message in the ring, where it is going to live: `fill`
    /// gets `max_bytes` of the ring and returns how many it used, which is
    /// what is published. THE RING'S WRITE LOCK IS HELD WHILE `fill` RUNS —
    /// no other producer can write — so it must fill bytes and nothing else.
    /// A `fill` that panics, or returns 0 or more than `max_bytes`, has its
    /// reservation given back and nothing a reader can see is changed.
    pub fn send_with<F>(&self, max_bytes: u32, origin: u32, fill: F) -> Result<()>
    where
        F: FnOnce(&mut [u8]) -> usize,
    {
        let mut st = ClockworkStatus::OK;
        // SAFETY: a live handle and a live out-pointer.
        let p = unsafe { clockwork_client_send_begin(self.raw.as_ptr(), max_bytes, &mut st) };
        let Some(p) = NonNull::new(p) else {
            return Err(refused(st));
        };
        let mut reservation = Reservation { client: self, open: true };
        // SAFETY: begin handed out `max_bytes` of ring for the caller to fill,
        // held until commit or abort; nothing else can touch it while the
        // reservation is open, and `reservation` aborts on every path that
        // does not commit.
        let buf = unsafe { core::slice::from_raw_parts_mut(p.as_ptr(), max_bytes as usize) };
        let used = fill(buf);
        if used == 0 || used > max_bytes as usize {
            return Err(Error::Arg); // the reservation drops, and aborts
        }
        reservation.open = false;
        // SAFETY: the reservation is open and `used` is within its bound;
        // commit closes it on every status.
        check(unsafe { clockwork_client_send_commit(self.raw.as_ptr(), used as u32, origin) })
    }

    /// Take what is waiting on the engine's egress: up to the poll batch,
    /// as messages whose bytes are the ring's and live until the next poll
    /// on this handle. A `sequence` gap means the ring lapped this reader.
    pub fn poll(&mut self) -> Messages<'_> {
        // SAFETY: a live handle, and `out` has `inbox.len()` slots, which is
        // the most the call is allowed to write.
        let n = unsafe {
            clockwork_client_poll(self.raw.as_ptr(), self.inbox.as_mut_ptr(), self.inbox.len() as u32)
        } as usize;
        Messages::over(&self.inbox[..n.min(self.inbox.len())])
    }

    /// How many messages one poll takes at most.
    pub fn poll_batch(&self) -> usize {
        self.inbox.len()
    }

    /// Set how many messages one poll takes at most (at least one).
    pub fn set_poll_batch(&mut self, messages: usize) {
        self.inbox.resize(messages.max(1), EMPTY_SLOT);
    }

    /// Take everything waiting and discard it. Returns how many were taken.
    /// A host that is not reading replies still calls this now and then, so
    /// nothing piles up to lap the ring or to be mistaken for a fresh reply
    /// on the next handshake.
    pub fn drain(&mut self) -> usize {
        let mut taken = 0;
        loop {
            let n = self.poll().len();
            taken += n;
            if n < self.inbox.len() {
                return taken;
            }
        }
    }

    /// Poll until `pick` answers, or `timeout` passes. The engine acts on a
    /// message on its next block, so the wait is by time: a poll, a
    /// millisecond's sleep, again. Messages `pick` declines are taken and
    /// gone, as with any poll.
    pub fn poll_until<T, F>(&mut self, timeout: Duration, mut pick: F) -> Option<T>
    where
        F: FnMut(&Message<'_>) -> Option<T>,
    {
        let deadline = Instant::now() + timeout;
        loop {
            for m in self.poll() {
                if let Some(t) = pick(&m) {
                    return Some(t);
                }
            }
            if Instant::now() >= deadline {
                return None;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    /// Watch a ring without taking from it: for a logger, a traffic
    /// inspector, and the only way to see the ingress ring at all.
    pub fn tap(&self, ring: Ring) -> Result<Tap<'_>> {
        let mut st = ClockworkStatus::OK;
        // SAFETY: a live handle, a ring id the header names, a live out-pointer.
        let raw = unsafe { clockwork_client_tap_open(self.raw.as_ptr(), ring.region_id(), &mut st) };
        match NonNull::new(raw) {
            Some(raw) => Ok(Tap { raw, ring, inbox: vec![EMPTY_SLOT; POLL_BATCH], _client: PhantomData }),
            None => Err(refused(st)),
        }
    }

    /// A span of memory the engine publishes, by name. [`Error::Absent`] is
    /// an answer: an engine with no guest window, or a handle that cannot
    /// reach the bulk lanes, says so rather than handing back a pointer
    /// into whatever is next along.
    pub fn region(&self, id: RegionId) -> Result<Region<'_>> {
        let mut raw = ClockworkRegion { base: core::ptr::null_mut(), bytes: 0, writable: 0 };
        // SAFETY: a live handle and a live out-struct.
        check(unsafe { clockwork_client_region(self.raw.as_ptr(), id.raw(), &mut raw) })?;
        match NonNull::new(raw.base.cast::<u8>()) {
            Some(base) => Ok(Region { base, bytes: raw.bytes, writable: raw.writable != 0, _client: PhantomData }),
            None => Err(Error::Absent),
        }
    }

    /// One coherent reading of the engine's counters.
    pub fn metrics(&self) -> Metrics {
        Metrics::read(self)
    }

    /// One coherent reading of the device layer's account of its callback.
    pub fn native_stats(&self) -> Result<NativeStats> {
        NativeStats::read(self)
    }

    /// One coherent snapshot of the clock: tempo and beat origin read
    /// together, because a client assembling them from separate calls can
    /// catch a tempo change between two of them.
    pub fn clock(&self) -> Result<Clock> {
        let mut raw = ClockworkClientClock {
            struct_bytes: core::mem::size_of::<ClockworkClientClock>() as u32,
            ..ClockworkClientClock::default()
        };
        // SAFETY: a live handle and a struct that says its own size.
        check(unsafe { clockwork_client_clock(self.raw.as_ptr(), &mut raw) })?;
        Ok(Clock { raw })
    }

    /// A reader on scope slot `slot`. [`Error::Absent`] when the engine has
    /// no such slot.
    pub fn scope(&self, slot: u32) -> Result<Scope<'_>> {
        Scope::open(self, slot)
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        if self.owned {
            // SAFETY: ours to close, closed once: this is the only place,
            // and `owned` is false for a handle the engine closes itself.
            unsafe { clockwork_client_close(self.raw.as_ptr()) }
        }
    }
}

/// An open `send_begin` that has not been committed. Aborts on drop.
struct Reservation<'a> {
    client: &'a Client,
    open: bool,
}

impl Drop for Reservation<'_> {
    fn drop(&mut self) {
        if self.open {
            // SAFETY: the handle is live for 'a; abort is accepted with or
            // without a reservation open.
            unsafe { clockwork_client_send_abort(self.client.raw.as_ptr()) }
        }
    }
}

// ── What is on the other end ───────────────────────────────────────────────

/// What an engine has: `CLOCKWORK_FEATURE_*` bits. A client asks rather
/// than assumes — an engine built for an embedded target may have no
/// scope, no taps and no bulk lanes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct Features(pub u32);

impl Features {
    /// The raw bits.
    pub const fn bits(self) -> u32 {
        self.0
    }
    /// Scope slots.
    pub const fn scope(self) -> bool {
        self.0 & CLOCKWORK_FEATURE_SCOPE != 0
    }
    /// The audio taps: what left for the device and what arrived from it.
    pub const fn audio_taps(self) -> bool {
        self.0 & CLOCKWORK_FEATURE_AUDIO_TAPS != 0
    }
    /// Inbox and outbox.
    pub const fn bulk(self) -> bool {
        self.0 & CLOCKWORK_FEATURE_BULK != 0
    }
    /// The guest publishes a window.
    pub const fn window(self) -> bool {
        self.0 & CLOCKWORK_FEATURE_WINDOW != 0
    }
    /// Timed messages are held.
    pub const fn scheduler(self) -> bool {
        self.0 & CLOCKWORK_FEATURE_SCHEDULER != 0
    }
}

/// What [`Client::info`] found.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Info {
    /// The library's ABI version.
    pub abi_version: u32,
    /// The engine's own build version.
    pub engine_version: u32,
    pub features: Features,
    pub sample_rate: f64,
    pub block_frames: u32,
    pub input_channels: u32,
    pub output_channels: u32,
}

// ── Receiving ──────────────────────────────────────────────────────────────

/// Which ring a message rode: `CLOCKWORK_ROUTE_*`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Route {
    /// An answer to the message's `origin`.
    Reply,
    /// To whoever subscribed to notifications.
    Notify,
    /// A route word this build does not name.
    Other(u32),
}

impl Route {
    /// From the ABI's word.
    pub const fn from_raw(word: u32) -> Route {
        match word {
            CLOCKWORK_ROUTE_REPLY => Route::Reply,
            CLOCKWORK_ROUTE_NOTIFY => Route::Notify,
            other => Route::Other(other),
        }
    }

    /// The ABI's word.
    pub const fn raw(self) -> u32 {
        match self {
            Route::Reply => CLOCKWORK_ROUTE_REPLY,
            Route::Notify => CLOCKWORK_ROUTE_NOTIFY,
            Route::Other(w) => w,
        }
    }
}

/// One message off a ring. `bytes` are the ring's and live until the next
/// poll on the handle or tap that produced them — which the lifetime says;
/// copy them to keep them.
#[derive(Clone, Copy, Debug)]
pub struct Message<'a> {
    /// The OSC packet.
    pub bytes: &'a [u8],
    /// The token the request carried, or 0.
    pub origin: u32,
    pub route: Route,
    /// Monotonic per ring; a gap means the ring lapped.
    pub sequence: u32,
}

impl<'a> Message<'a> {
    fn from_slot(m: &'a ClockworkClientMessage) -> Message<'a> {
        let bytes: &'a [u8] = if m.bytes.is_null() || m.length == 0 {
            &[]
        } else {
            // SAFETY: the ring's bytes, which the library keeps valid until
            // the next poll on this handle or tap; that needs `&mut`, so it
            // cannot happen while 'a lives.
            unsafe { core::slice::from_raw_parts(m.bytes, m.length as usize) }
        };
        Message { bytes, origin: m.origin, route: Route::from_raw(m.route), sequence: m.sequence }
    }

    /// The OSC address, for an OSC message: the bytes up to the first NUL.
    /// Empty for a bundle (`#bundle` is its own marker) or for bytes that
    /// are not UTF-8.
    pub fn address(&self) -> &'a str {
        let end = self.bytes.iter().position(|&b| b == 0).unwrap_or(self.bytes.len());
        match core::str::from_utf8(&self.bytes[..end]) {
            Ok(s) if s.starts_with('/') => s,
            _ => "",
        }
    }

    /// True for a reply carrying `origin`.
    pub fn is_reply_to(&self, origin: u32) -> bool {
        self.route == Route::Reply && self.origin == origin
    }
}

/// The messages one poll took. An exact-size iterator; `len()` is how many.
pub struct Messages<'a> {
    slots: core::slice::Iter<'a, ClockworkClientMessage>,
}

impl<'a> Messages<'a> {
    fn over(slots: &'a [ClockworkClientMessage]) -> Messages<'a> {
        Messages { slots: slots.iter() }
    }
}

impl<'a> Iterator for Messages<'a> {
    type Item = Message<'a>;

    fn next(&mut self) -> Option<Message<'a>> {
        self.slots.next().map(Message::from_slot)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.slots.size_hint()
    }
}

impl ExactSizeIterator for Messages<'_> {}

// ── Watching a ring ────────────────────────────────────────────────────────

/// A ring a [`Tap`] can watch.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Ring {
    /// What clients write and the engine reads.
    Ingress,
    /// What the engine writes and clients read.
    Egress,
}

impl Ring {
    fn region_id(self) -> u32 {
        match self {
            Ring::Ingress => ClockworkRegionId::INGRESS.0 as u32,
            Ring::Egress => ClockworkRegionId::EGRESS.0 as u32,
        }
    }
}

/// A reader that watches a ring without taking from it. It carries its own
/// cursor, starting at the ring's head when opened, so several taps and the
/// real consumer all see the same traffic. It can be lapped — a writer
/// knows nothing about observers — and then resynchronises to the newest
/// data and counts what went past ([`Tap::missed`]).
pub struct Tap<'c> {
    raw: NonNull<ClockworkClientTap>,
    ring: Ring,
    inbox: Vec<ClockworkClientMessage>,
    _client: PhantomData<&'c Client>,
}

impl Tap<'_> {
    /// Which ring this watches.
    pub fn ring(&self) -> Ring {
        self.ring
    }

    /// What was written since the last poll, as [`Client::poll`] delivers
    /// it. Bytes live until the next poll on this tap.
    pub fn poll(&mut self) -> Messages<'_> {
        // SAFETY: a live tap, and `out` has `inbox.len()` slots, which is the
        // most the call is allowed to write.
        let n = unsafe {
            clockwork_client_tap_poll(self.raw.as_ptr(), self.inbox.as_mut_ptr(), self.inbox.len() as u32)
        } as usize;
        Messages::over(&self.inbox[..n.min(self.inbox.len())])
    }

    /// Frames written past this tap before it could read them, since it
    /// opened.
    pub fn missed(&self) -> u64 {
        // SAFETY: a live tap.
        unsafe { clockwork_client_tap_missed(self.raw.as_ptr()) }
    }
}

impl Drop for Tap<'_> {
    fn drop(&mut self) {
        // SAFETY: ours to close, closed once, before the handle it borrows.
        unsafe { clockwork_client_tap_close(self.raw.as_ptr()) }
    }
}

// ── Regions ────────────────────────────────────────────────────────────────

/// A region, by name: `ClockworkRegionId`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RegionId {
    /// Engine counters; see [`Client::metrics`].
    Metrics,
    /// What the guest publishes. Opaque here: its shape is the guest's, and
    /// the first `u32` is a version stamp the guest bumps.
    Window,
    /// Bulk in: the client writes, the guest reads.
    Inbox,
    /// Bulk out: the guest writes, the client reads.
    Outbox,
    Scope,
    AudioTaps,
    /// The ring clients write and the engine reads.
    Ingress,
    /// The ring the engine writes and clients read.
    Egress,
    /// The device layer's account of its callback; see [`Client::native_stats`].
    NativeStats,
}

impl RegionId {
    /// The ABI's id.
    pub const fn raw(self) -> ClockworkRegionId {
        match self {
            RegionId::Metrics => ClockworkRegionId::METRICS,
            RegionId::Window => ClockworkRegionId::WINDOW,
            RegionId::Inbox => ClockworkRegionId::INBOX,
            RegionId::Outbox => ClockworkRegionId::OUTBOX,
            RegionId::Scope => ClockworkRegionId::SCOPE,
            RegionId::AudioTaps => ClockworkRegionId::AUDIO_TAPS,
            RegionId::Ingress => ClockworkRegionId::INGRESS,
            RegionId::Egress => ClockworkRegionId::EGRESS,
            RegionId::NativeStats => ClockworkRegionId::NATIVE_STATS,
        }
    }
}

/// A span of the engine's memory, borrowed from the handle. A live writer
/// is on the other side of most of it, so reads are volatile and by word;
/// a caller that wants a struct out of it reads the words and assembles
/// them, knowing what it is talking to.
pub struct Region<'c> {
    base: NonNull<u8>,
    bytes: u32,
    writable: bool,
    _client: PhantomData<&'c Client>,
}

impl Region<'_> {
    /// Bytes in the region.
    pub fn len(&self) -> usize {
        self.bytes as usize
    }

    /// True for a region of no bytes.
    pub fn is_empty(&self) -> bool {
        self.bytes == 0
    }

    /// True only for the inbox: everything else is the engine's or the
    /// guest's to write.
    pub fn is_writable(&self) -> bool {
        self.writable
    }

    /// The region's base, for a caller that reads a shape this crate does
    /// not know. Valid while the handle is.
    pub fn as_ptr(&self) -> *mut u8 {
        self.base.as_ptr()
    }

    /// The `u32` at `word`, if the region has that many.
    pub fn read_u32(&self, word: usize) -> Option<u32> {
        let at = word.checked_mul(4)?;
        if at.checked_add(4)? > self.len() {
            return None;
        }
        // SAFETY: inside the region, which the engine keeps mapped for the
        // handle's life; volatile because the engine writes it concurrently.
        // Regions are laid out at 8-byte alignment (clockwork_arena.h), so
        // a word offset is aligned.
        Some(unsafe { self.base.as_ptr().add(at).cast::<u32>().read_volatile() })
    }

    /// The first `out.len()` words, or as many as the region has. Returns
    /// how many were read.
    pub fn read_u32s(&self, out: &mut [u32]) -> usize {
        let n = out.len().min(self.len() / 4);
        for (i, w) in out.iter_mut().take(n).enumerate() {
            *w = self.read_u32(i).unwrap_or(0);
        }
        n
    }
}

// ── The clock ──────────────────────────────────────────────────────────────

/// One coherent snapshot of the engine's clock.
#[derive(Clone, Copy, Debug)]
pub struct Clock {
    raw: ClockworkClientClock,
}

impl Clock {
    pub fn bpm(&self) -> f64 {
        self.raw.bpm
    }
    /// The NTP instant beat 0 falls on.
    pub fn beat_origin_ntp(&self) -> f64 {
        self.raw.beat_origin_ntp
    }
    pub fn is_playing(&self) -> bool {
        self.raw.is_playing != 0
    }
    /// The NTP instant the transport last started or stopped.
    pub fn is_playing_at_ntp(&self) -> f64 {
        self.raw.is_playing_at_ntp
    }
    pub fn flags(&self) -> u32 {
        self.raw.flags
    }
    /// The meter, as (numerator, denominator).
    pub fn meter(&self) -> (i32, i32) {
        (self.raw.meter_num, self.raw.meter_den)
    }
    /// The ABI's struct.
    pub fn raw(&self) -> &ClockworkClientClock {
        &self.raw
    }

    /// The beat at an NTP instant, under this snapshot. The library's own
    /// arithmetic, so every binding agrees to the bit.
    pub fn beat_at(&self, ntp_seconds: f64) -> f64 {
        // SAFETY: a live struct of our own.
        unsafe { clockwork_client_beat_at(&self.raw, ntp_seconds) }
    }

    /// The beat now, under this snapshot.
    pub fn beat_now(&self) -> f64 {
        self.beat_at(crate::time::ntp_now())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_message_knows_its_address_and_route() {
        let osc = b"/dummy/pong\0,\0\0\0";
        let slot = ClockworkClientMessage { bytes: osc.as_ptr(), length: osc.len() as u32, origin: 7, route: 0, sequence: 3 };
        let m = Message::from_slot(&slot);
        assert_eq!(m.address(), "/dummy/pong");
        assert_eq!(m.bytes, osc);
        assert!(m.is_reply_to(7));
        assert!(!m.is_reply_to(8));
        assert_eq!(m.route, Route::Reply);

        let bundle = b"#bundle\0";
        let slot = ClockworkClientMessage { bytes: bundle.as_ptr(), length: 8, origin: 0, route: 2, sequence: 4 };
        let m = Message::from_slot(&slot);
        assert_eq!(m.address(), "");
        assert_eq!(m.route, Route::Notify);
        assert!(!m.is_reply_to(0));

        let empty = Message::from_slot(&EMPTY_SLOT);
        assert!(empty.bytes.is_empty());
        assert_eq!(Route::from_raw(9), Route::Other(9));
        assert_eq!(Route::Other(9).raw(), 9);
        assert_eq!(Route::Reply.raw(), CLOCKWORK_ROUTE_REPLY);
    }

    #[test]
    fn features_are_bits() {
        let f = Features(CLOCKWORK_FEATURE_SCOPE | CLOCKWORK_FEATURE_WINDOW);
        assert!(f.scope() && f.window());
        assert!(!f.audio_taps() && !f.bulk() && !f.scheduler());
        assert_eq!(Features::default().bits(), 0);
    }

    #[test]
    fn region_ids_are_the_header_s() {
        assert_eq!(RegionId::NativeStats.raw(), ClockworkRegionId::NATIVE_STATS);
        assert_eq!(Ring::Ingress.region_id(), ClockworkRegionId::INGRESS.0 as u32);
        assert_eq!(Ring::Egress.region_id(), ClockworkRegionId::EGRESS.0 as u32);
    }
}
