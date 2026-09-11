// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Cross-process SHM peer client — the peer side of the SHM command plane
//! (`src/shm_peer_plane.h` / `ShmTransport`). It maps a running server's shared
//! segment, attaches to the plane, writes OSC into the command ring, and reads
//! replies from the reply ring, using the byte-identical Message wire format
//! (`src/ring/ring.h`). The transport harness uses it to drive the
//! `--shm-commands` transport end-to-end on every OS — the SHM analogue of the
//! socket clients in `transport_probe`.
//!
//! This is a Rust-side test client, not part of the C ABI. It mirrors
//! three C++ contracts, and self-checks them at attach so a layout drift fails
//! loudly rather than corrupting the ring:
//!
//! * segment header offsets — `shm_segment.hpp` (all `uint32_t` fields);
//! * the 64-byte plane header — `shm_peer_plane.h`;
//! * the ring writer/reader protocol — `RingBufferWriter.h` + `ring_drain.h`.
//!
//! Both processes are the same machine (native endianness), so integers are
//! read/written with `to_ne_bytes`, matching the C++ `memcpy`s.

use std::sync::atomic::{fence, AtomicI32, AtomicU32, Ordering};

const SEG_MAGIC: u32 = 0x5C09_E00C; // shm_segment_header::MAGIC (E00C: the arena describes itself)
const MESSAGE_MAGIC: u32 = 0xDEAD_BEEF; // ring/ring.h
const PADDING_MAGIC: u32 = 0xBADD_CAFE;
const MSG_HDR: usize = 16; // sizeof(Message)

// shm_segment_header field byte offsets (all u32; see shm_segment.hpp): magic,
// blob_offset, blob_size, then peer_offset (SEGMENT-relative) and the plane
// geometry. The regions inside the blob are not here: they are in the arena's
// own header (clockwork_arena.h), which this peer does not read.
const HDR_MAGIC: usize = 0;
const HDR_PEER_OFFSET: usize = 3 * 4;
const HDR_PEER_CMD_BYTES: usize = 5 * 4;
const HDR_PEER_REP_BYTES: usize = 6 * 4;

// ShmPeerPlaneHeader field byte offsets (64-byte header; shm_peer_plane.h).
const P_OWNER_PID: usize = 0;
const P_GENERATION: usize = 4;
const P_CMD_HEAD: usize = 8;
const P_CMD_TAIL: usize = 12;
const P_CMD_SEQ: usize = 16;
const P_CMD_LOCK: usize = 20;
const P_REP_HEAD: usize = 24;
const P_REP_TAIL: usize = 28;
const P_REP_DROPPED: usize = 36;
const P_CMD_RING_SIZE: usize = 40;
const P_REP_RING_SIZE: usize = 44;
const PLANE_HDR: usize = 64;

// ── platform segment mapping ─────────────────────────────────────────────────

struct Segment {
    base: *mut u8,
    len: usize,
    #[cfg(unix)]
    fd: i32,
    #[cfg(windows)]
    mapping: windows_sys::Win32::Foundation::HANDLE,
}

impl Drop for Segment {
    fn drop(&mut self) {
        // SAFETY: `base`/`len` and the descriptor are exactly what
        // `open_segment` mapped and opened, released once, here.
        unsafe {
            #[cfg(unix)]
            {
                libc::munmap(self.base.cast::<libc::c_void>(), self.len);
                libc::close(self.fd);
            }
            #[cfg(windows)]
            {
                use windows_sys::Win32::Foundation::CloseHandle;
                use windows_sys::Win32::System::Memory::{
                    MEMORY_MAPPED_VIEW_ADDRESS, UnmapViewOfFile,
                };
                UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.base.cast() });
                CloseHandle(self.mapping);
            }
        }
    }
}

// ── attaching ────────────────────────────────────────────────────────────────
//
// The segment is anonymous (shm_segment.hpp): the engine serves a duplicate of
// its handle from an attach endpoint (shm_attach.hpp) — a Unix socket that
// passes the descriptor as SCM_RIGHTS, or a Windows named pipe over which the
// engine duplicates the handle into this process. Both ends check the peer is
// the same user; a planted endpoint hands over nothing.

const ATTACH_MAGIC: u32 = 0x4357_4154; // "CWAT", shm_attach::MAGIC
const ATTACH_VERSION: u32 = 1;
const HELLO_LEN: usize = 24; // magic u32, version u32, size u64, handle u64

fn parse_hello(buf: &[u8]) -> Result<(u64, u64), String> {
    if buf.len() != HELLO_LEN {
        return Err(format!("short hand-off: {} bytes", buf.len()));
    }
    let u32_at = |o: usize| u32::from_ne_bytes(buf[o..o + 4].try_into().unwrap());
    let u64_at = |o: usize| u64::from_ne_bytes(buf[o..o + 8].try_into().unwrap());
    if u32_at(0) != ATTACH_MAGIC || u32_at(4) != ATTACH_VERSION {
        return Err("malformed hand-off (bad magic or version)".into());
    }
    Ok((u64_at(8), u64_at(16)))
}

/// The endpoint an engine on `port` serves at unless told otherwise; mirrors
/// shm_attach::default_endpoint.
pub fn default_endpoint(port: u32) -> String {
    #[cfg(windows)]
    {
        format!(r"\\.\pipe\clockwork-shm-{port}")
    }
    #[cfg(unix)]
    {
        let dir = ["XDG_RUNTIME_DIR", "TMPDIR"]
            .iter()
            .filter_map(|v| std::env::var(v).ok())
            .map(|d| d.trim_end_matches('/').to_string())
            .find(|d| !d.is_empty());
        match dir {
            Some(d) => format!("{d}/clockwork-shm-{port}.sock"),
            // SAFETY: getuid cannot fail and touches nothing.
            None => format!("/tmp/clockwork-shm-{}-{port}.sock", unsafe { libc::getuid() }),
        }
    }
}

#[cfg(unix)]
fn open_segment(endpoint: &str) -> Result<Segment, String> {
    use std::os::unix::io::AsRawFd;
    let stream = std::os::unix::net::UnixStream::connect(endpoint)
        .map_err(|e| format!("connect {endpoint}: {e}"))?;
    stream
        .set_read_timeout(Some(std::time::Duration::from_secs(2)))
        .map_err(|e| format!("timeout: {e}"))?;
    let sock = stream.as_raw_fd();

    // The server must be ours. A path is guessable; a uid is not forgeable.
    // SAFETY: plain getsockopt / getpeereid on a connected socket with live
    // out-pointers of the size each call documents.
    let peer_uid: libc::uid_t = unsafe {
        #[cfg(target_os = "linux")]
        {
            let mut cr: libc::ucred = std::mem::zeroed();
            let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
            if libc::getsockopt(
                sock,
                libc::SOL_SOCKET,
                libc::SO_PEERCRED,
                (&mut cr as *mut libc::ucred).cast(),
                &mut len,
            ) != 0
            {
                return Err("peer credentials unavailable".into());
            }
            cr.uid
        }
        #[cfg(not(target_os = "linux"))]
        {
            let mut uid: libc::uid_t = 0;
            let mut gid: libc::gid_t = 0;
            if libc::getpeereid(sock, &mut uid, &mut gid) != 0 {
                return Err("peer credentials unavailable".into());
            }
            uid
        }
    };
    // SAFETY: geteuid cannot fail and touches nothing.
    if peer_uid != unsafe { libc::geteuid() } {
        return Err(format!("the engine at {endpoint} is not running as this user"));
    }

    // One recvmsg: the hello in the data, the descriptor in the ancillary.
    let mut hello = [0u8; HELLO_LEN];
    let mut ctl = [0u8; 64]; // >= CMSG_SPACE(size_of::<c_int>()) on every target
    let fd: libc::c_int;
    let got: isize;
    // SAFETY: every pointer handed to recvmsg names a live local buffer of the
    // length given beside it; the cmsg walk uses the kernel-filled lengths and
    // only reads a c_int from a SCM_RIGHTS entry that declares at least one.
    unsafe {
        let mut iov = libc::iovec { iov_base: hello.as_mut_ptr().cast(), iov_len: HELLO_LEN };
        let mut msg: libc::msghdr = std::mem::zeroed();
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctl.as_mut_ptr().cast();
        msg.msg_controllen = ctl.len() as _;
        got = libc::recvmsg(sock, &mut msg, 0);
        let mut received: libc::c_int = -1;
        let mut c = libc::CMSG_FIRSTHDR(&msg);
        while !c.is_null() {
            if (*c).cmsg_level == libc::SOL_SOCKET
                && (*c).cmsg_type == libc::SCM_RIGHTS
                && (*c).cmsg_len as usize >= libc::CMSG_LEN(std::mem::size_of::<libc::c_int>() as u32) as usize
            {
                std::ptr::copy_nonoverlapping(
                    libc::CMSG_DATA(c),
                    (&mut received as *mut libc::c_int).cast::<u8>(),
                    std::mem::size_of::<libc::c_int>(),
                );
            }
            c = libc::CMSG_NXTHDR(&msg, c);
        }
        if got != HELLO_LEN as isize || (msg.msg_flags & libc::MSG_CTRUNC) != 0 {
            if received >= 0 {
                libc::close(received);
            }
            return Err(format!("malformed hand-off from {endpoint}"));
        }
        fd = received;
    }
    drop(stream);
    if fd < 0 {
        return Err(format!("no descriptor in the hand-off from {endpoint}"));
    }
    if let Err(e) = parse_hello(&hello) {
        // SAFETY: a descriptor we own and have not mapped.
        unsafe { libc::close(fd) };
        return Err(e);
    }

    // SAFETY: a descriptor we own, a zeroed `stat` for `fstat` to fill, and
    // an `mmap` of the size the file reports; every failure path closes it.
    unsafe {
        libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC);
        let mut st: libc::stat = std::mem::zeroed();
        if libc::fstat(fd, &mut st) != 0 {
            libc::close(fd);
            return Err("fstat failed".into());
        }
        let len = st.st_size as usize;
        let base = libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        );
        if base == libc::MAP_FAILED {
            libc::close(fd);
            return Err("mmap failed".into());
        }
        Ok(Segment { base: base.cast::<u8>(), len, fd })
    }
}

#[cfg(windows)]
fn open_segment(endpoint: &str) -> Result<Segment, String> {
    use windows_sys::Win32::Foundation::{CloseHandle, ERROR_PIPE_BUSY, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{CreateFileW, ReadFile, OPEN_EXISTING};
    use windows_sys::Win32::System::Memory::{
        MapViewOfFile, VirtualQuery, FILE_MAP_ALL_ACCESS, MEMORY_BASIC_INFORMATION,
    };
    use windows_sys::Win32::System::Pipes::WaitNamedPipeW;
    let path = if endpoint.starts_with(r"\\.\pipe\") {
        endpoint.to_string()
    } else {
        format!(r"\\.\pipe\{endpoint}")
    };
    let name: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: a NUL-terminated UTF-16 name, a read into a local buffer of the
    // length given, a whole-mapping view, and a zeroed info block for
    // `VirtualQuery` to fill; every failure path closes what it opened.
    unsafe {
        let mut pipe = INVALID_HANDLE_VALUE;
        for _ in 0..3 {
            pipe = CreateFileW(name.as_ptr(), 0x8000_0000 /* GENERIC_READ */, 0, std::ptr::null(), OPEN_EXISTING, 0, std::ptr::null_mut());
            if pipe != INVALID_HANDLE_VALUE {
                break;
            }
            if std::io::Error::last_os_error().raw_os_error() != Some(ERROR_PIPE_BUSY as i32) {
                break;
            }
            WaitNamedPipeW(name.as_ptr(), 1000);
        }
        if pipe == INVALID_HANDLE_VALUE {
            return Err(format!("open {path}: {}", std::io::Error::last_os_error()));
        }
        let mut hello = [0u8; HELLO_LEN];
        let mut got: u32 = 0;
        let ok = ReadFile(pipe, hello.as_mut_ptr(), HELLO_LEN as u32, &mut got, std::ptr::null_mut());
        CloseHandle(pipe);
        if ok == 0 || got as usize != HELLO_LEN {
            return Err(format!("malformed hand-off from {path}"));
        }
        let (_size, handle) = parse_hello(&hello)?;
        if handle == 0 {
            return Err("the engine could not duplicate its segment into this process".into());
        }
        let mapping = handle as usize as windows_sys::Win32::Foundation::HANDLE;
        let view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0); // 0 = whole mapping
        if view.Value.is_null() {
            CloseHandle(mapping);
            return Err("MapViewOfFile failed".into());
        }
        // Region size — the mapping has no size query, so measure the view like
        // shm_segment.hpp's shm_map_handle does.
        let mut info: MEMORY_BASIC_INFORMATION = std::mem::zeroed();
        VirtualQuery(view.Value, &mut info, std::mem::size_of::<MEMORY_BASIC_INFORMATION>());
        Ok(Segment { base: view.Value.cast::<u8>(), len: info.RegionSize, mapping })
    }
}

// Every offset below is inside the mapping: `open` checks the header, plane
// and both rings fit in `len` before any of these is reached, and the ring
// code bounds each access by that geometry.
impl Segment {
    #[inline]
    fn read_u32(&self, off: usize) -> u32 {
        // SAFETY: inside the mapping (see above); unaligned read, so no
        // alignment claim is made.
        unsafe { std::ptr::read_unaligned(self.base.add(off).cast::<u32>()) }
    }
    #[inline]
    fn atomic_i32(&self, off: usize) -> &AtomicI32 {
        // SAFETY: inside the mapping, and 4-aligned: `open` refuses a plane
        // that is not 8-aligned and every cursor offset is a multiple of 4.
        unsafe { &*self.base.add(off).cast::<AtomicI32>() }
    }
    #[inline]
    fn atomic_u32(&self, off: usize) -> &AtomicU32 {
        // SAFETY: as `atomic_i32`.
        unsafe { &*self.base.add(off).cast::<AtomicU32>() }
    }
    /// # Safety
    /// `off..off + src.len()` must be inside the command ring, under the
    /// writer lock.
    #[inline]
    unsafe fn write(&self, off: usize, src: &[u8]) {
        // SAFETY: per the contract; `src` is a live slice.
        unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), self.base.add(off), src.len()) }
    }
    /// # Safety
    /// As [`Segment::write`].
    #[inline]
    unsafe fn zero(&self, off: usize, len: usize) {
        // SAFETY: per the contract.
        unsafe { std::ptr::write_bytes(self.base.add(off), 0, len) }
    }
    /// A copy of `len` bytes at `off`. A COPY, not a slice: the other process
    /// writes this memory, and a `&[u8]` over it would promise the bytes hold
    /// still for as long as the reference lives — a promise nothing keeps.
    /// The copy is one `memcpy` per frame, which a test client can afford.
    #[inline]
    fn read_bytes(&self, off: usize, len: usize) -> Vec<u8> {
        let mut out = vec![0u8; len];
        // SAFETY: `off + len` is inside the mapping (every caller bounds it by
        // the ring geometry the header published), and a raw copy makes no
        // aliasing claim about memory another process may be writing.
        unsafe { std::ptr::copy_nonoverlapping(self.base.add(off), out.as_mut_ptr(), len) };
        out
    }
}

// ── the peer ─────────────────────────────────────────────────────────────────

pub struct ShmPeer {
    seg: Segment,
    plane: usize,    // byte offset of the plane header in the segment
    cmd_ring: usize, // byte offset of the command ring base
    cmd_size: i64,
    rep_ring: usize,
    rep_size: i64,
}

impl ShmPeer {
    /// Attach through the engine's endpoint (`default_endpoint(port)` unless it
    /// was started with --shm-endpoint) and locate the peer plane, validating
    /// the magic and cross-checking the geometry the header and plane both carry.
    pub fn open(endpoint: &str) -> Result<ShmPeer, String> {
        let seg = open_segment(endpoint)?;
        if seg.len < 128 {
            return Err("segment smaller than a header".into());
        }
        let magic = seg.read_u32(HDR_MAGIC);
        if magic != SEG_MAGIC {
            return Err(format!("bad segment magic {magic:#010x} (want {SEG_MAGIC:#010x})"));
        }
        fence(Ordering::Acquire); // pairs with the creator's release before MAGIC

        let plane = seg.read_u32(HDR_PEER_OFFSET) as usize;
        let cmd_size = seg.read_u32(HDR_PEER_CMD_BYTES);
        let rep_size = seg.read_u32(HDR_PEER_REP_BYTES);
        // The plane is alignas(8) and its cursors are accessed as atomics, so a
        // misaligned peer_offset would build misaligned atomic refs (UB). The
        // engine always 8-aligns it; reject anything else rather than trust a
        // valid-magic-but-corrupt segment.
        if plane % 8 != 0 {
            return Err(format!("peer plane offset {plane} is not 8-aligned"));
        }
        let end = plane
            .checked_add(PLANE_HDR + cmd_size as usize + rep_size as usize)
            .ok_or("plane geometry overflow")?;
        if end > seg.len {
            return Err("peer plane runs past the segment".into());
        }
        // The plane header carries the same geometry; a mismatch means we located
        // it wrong (header layout drift) — fail rather than write a bad ring.
        let p_cmd = seg.read_u32(plane + P_CMD_RING_SIZE);
        let p_rep = seg.read_u32(plane + P_REP_RING_SIZE);
        if p_cmd != cmd_size || p_rep != rep_size {
            return Err(format!(
                "plane geometry mismatch: header {cmd_size}/{rep_size} vs plane {p_cmd}/{p_rep}"
            ));
        }
        Ok(ShmPeer {
            cmd_ring: plane + PLANE_HDR,
            rep_ring: plane + PLANE_HDR + cmd_size as usize,
            cmd_size: cmd_size as i64,
            rep_size: rep_size as i64,
            plane,
            seg,
        })
    }

    /// Claim the plane (shm_peer_attach): stamp pid, reset the writer lock, skip
    /// any stale replies, bump the generation.
    pub fn attach(&self, pid: u32) {
        self.seg.atomic_u32(self.plane + P_OWNER_PID).store(pid, Ordering::Relaxed);
        self.seg.atomic_i32(self.plane + P_CMD_LOCK).store(0, Ordering::Relaxed);
        let head = self.seg.atomic_i32(self.plane + P_REP_HEAD).load(Ordering::Acquire);
        self.seg.atomic_i32(self.plane + P_REP_TAIL).store(head, Ordering::Release);
        self.seg.atomic_u32(self.plane + P_GENERATION).fetch_add(1, Ordering::AcqRel);
    }

    pub fn replies_dropped(&self) -> u32 {
        self.seg.atomic_u32(self.plane + P_REP_DROPPED).load(Ordering::Relaxed)
    }

    /// Write one OSC packet into the command ring (RingBufferWriter protocol).
    /// Returns false when the ring is full — backpressure, no blocking.
    pub fn write_cmd(&self, data: &[u8]) -> bool {
        let sz = self.cmd_size;
        let total = (MSG_HDR + data.len()) as i64;
        let aligned = (total + 3) & !3;

        let lock = self.seg.atomic_i32(self.plane + P_CMD_LOCK);
        let head = self.seg.atomic_i32(self.plane + P_CMD_HEAD);
        let tail = self.seg.atomic_i32(self.plane + P_CMD_TAIL);
        let seq = self.seg.atomic_i32(self.plane + P_CMD_SEQ);

        while lock
            .compare_exchange_weak(0, 1, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            std::hint::spin_loop();
        }

        let h = head.load(Ordering::Relaxed) as i64;
        let t = tail.load(Ordering::Acquire) as i64;
        let used = (h - t + sz) % sz;
        let avail = sz - used - 1;
        if aligned > avail {
            lock.store(0, Ordering::Release);
            return false;
        }

        let mut uh = h;
        let space_to_end = sz - uh;
        if aligned > space_to_end {
            let space_at_front = if t > 0 { t - 1 } else { 0 };
            if aligned > space_at_front {
                lock.store(0, Ordering::Release);
                return false;
            }
            // SAFETY: `uh..sz` is the ring's tail, the lock is held, and
            // the space check above says the frame fits at the front.
            unsafe {
                self.seg.write(self.cmd_ring + uh as usize, &PADDING_MAGIC.to_ne_bytes());
                self.seg.zero(self.cmd_ring + uh as usize + 4, (space_to_end - 4) as usize);
            }
            uh = 0;
        }

        let s = seq.fetch_add(1, Ordering::Relaxed) as u32;
        let mut hdr = [0u8; MSG_HDR];
        hdr[0..4].copy_from_slice(&MESSAGE_MAGIC.to_ne_bytes());
        hdr[4..8].copy_from_slice(&(total as u32).to_ne_bytes());
        hdr[8..12].copy_from_slice(&s.to_ne_bytes());
        // sourceId 0: the host drain re-stamps SHM_PEER_ORIGIN_TOKEN.
        // SAFETY: `aligned` bytes at `uh` were shown free above, under the
        // lock, and `aligned <= sz`.
        unsafe {
            let base = self.cmd_ring + uh as usize;
            self.seg.write(base, &hdr);
            self.seg.write(base + MSG_HDR, data);
            if aligned > total {
                self.seg.zero(base + total as usize, (aligned - total) as usize);
            }
        }
        head.store(((uh + aligned) % sz) as i32, Ordering::Release);
        lock.store(0, Ordering::Release);
        true
    }

    /// Drain every complete reply frame currently in the reply ring, handing each
    /// payload to `f` (ring_drain protocol; the peer owns rep_tail).
    pub fn drain_replies(&self, mut f: impl FnMut(&[u8])) {
        let sz = self.rep_size;
        let head_a = self.seg.atomic_i32(self.plane + P_REP_HEAD);
        let tail_a = self.seg.atomic_i32(self.plane + P_REP_TAIL);
        loop {
            let head = head_a.load(Ordering::Acquire) as i64;
            let tail = tail_a.load(Ordering::Relaxed) as i64;
            if head == tail {
                break;
            }
            if head < 0 || head >= sz {
                break; // bad head — producer state we can't repair
            }
            if tail < 0 || tail >= sz {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let ut = tail;
            let avail = (head - ut + sz) % sz;
            let space_to_end = sz - ut;
            if space_to_end < 4 || avail < 4 {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let magic = self.seg.read_u32(self.rep_ring + ut as usize);
            if magic == PADDING_MAGIC {
                if ut == 0 {
                    tail_a.store(head as i32, Ordering::Release);
                    break;
                }
                tail_a.store(0, Ordering::Release);
                continue;
            }
            if magic != MESSAGE_MAGIC || space_to_end < MSG_HDR as i64 {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let total = self.seg.read_u32(self.rep_ring + ut as usize + 4) as i64;
            let footprint = (total + 3) & !3;
            if total < MSG_HDR as i64
                || footprint > sz
                || footprint > space_to_end
                || footprint > avail
            {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let payload_size = (total - MSG_HDR as i64) as usize;
            if payload_size > 0 {
                f(&self.seg.read_bytes(self.rep_ring + ut as usize + MSG_HDR, payload_size));
            }
            tail_a.store(((ut + footprint) % sz) as i32, Ordering::Release);
        }
    }
}
