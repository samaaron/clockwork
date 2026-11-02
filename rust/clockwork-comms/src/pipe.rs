// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Windows named-pipe stream server — the pipe analogue of the UDS stream
//! server, behind the same [`ClockworkOscStream`] C ABI (conn ids, `on_packet` /
//! `on_closed`, [`clockwork_osc_stream_send`](crate::stream::clockwork_osc_stream_send) /
//! `_stop`), so the C++ StreamOscTransport works unchanged over TCP, UDS, and
//! pipes.
//!
//! Wire format is identical too: byte-mode pipes carrying 4-byte big-endian
//! length-prefixed OSC packets, so a client frames the same way over every
//! stream transport.
//!
//! Security: instances are created with an owner-only DACL (SYSTEM + the
//! current user's SID, built via SDDL) and `PIPE_REJECT_REMOTE_CLIENTS`; the
//! first instance uses `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a squatter holding
//! the name fails the start instead of intercepting clients. The instance
//! count is the connection cap — an over-cap client's connect fails busy.
//!
//! ⚠ Not yet exercised on a real Windows machine: coverage so far is a
//! cross-target type-check plus the CI transport harness (run.ps1).


use std::ffi::c_void;

use crate::stream::ClockworkOscStream;

#[cfg(windows)]
mod imp {
    use std::collections::HashMap;
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
    use std::sync::{Arc, Mutex};
    use std::thread::JoinHandle;
    use std::time::Instant;

    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, LocalFree, ERROR_IO_PENDING, ERROR_PIPE_CONNECTED, HANDLE,
        INVALID_HANDLE_VALUE, WAIT_OBJECT_0,
    };
    use windows_sys::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        SDDL_REVISION_1,
    };
    use windows_sys::Win32::Security::{
        GetTokenInformation, TokenUser, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
    };
    use windows_sys::Win32::Storage::FileSystem::{
        ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX,
    };
    use windows_sys::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_READMODE_BYTE,
        PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventW, GetCurrentProcess, OpenProcessToken, SetEvent, WaitForMultipleObjects, INFINITE,
    };

    use crate::secret::ClockworkCommsSecret;
    use crate::stream::{
        check_auth, drain_frames, frame_packet, refuse, Gate, Host, StreamServerImpl, AUTH_DEADLINE,
    };

    const PIPE_BUF_BYTES: u32 = 65536;
    // How long a send waits for the client to read before it gives up. The
    // same as the stream servers' WRITE_TIMEOUT.
    const WRITE_TIMEOUT_MS: u32 = 2000;

    /// UTF-16, NUL-terminated.
    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    /// An owner-only security descriptor (SYSTEM + the current user), built
    /// from SDDL. Owned pointer, freed with LocalFree.
    struct OwnerSd(*mut c_void);
    // SAFETY: an owned, immutable LocalAlloc block that only `CreateNamedPipeW`
    // reads; any thread may hold or free it.
    unsafe impl Send for OwnerSd {}
    // SAFETY: as above.
    unsafe impl Sync for OwnerSd {}
    impl Drop for OwnerSd {
        fn drop(&mut self) {
            // SAFETY: the descriptor `owner_only_sd` had the system allocate,
            // freed once, here.
            unsafe { LocalFree(self.0) };
        }
    }

    fn owner_only_sd() -> Option<OwnerSd> {
        // SAFETY: plain Win32 calls with live out-pointers; the token buffer
        // is sized by the first `GetTokenInformation` call before the second
        // reads it as a `TOKEN_USER`, and each handle and string the system
        // allocates is released on every path once it is no longer read.
        unsafe {
            // Current user's SID string, from the process token.
            let mut token: HANDLE = std::ptr::null_mut();
            if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
                return None;
            }
            let mut needed: u32 = 0;
            GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut needed);
            let mut buf = vec![0u8; needed as usize];
            let ok = GetTokenInformation(
                token,
                TokenUser,
                buf.as_mut_ptr().cast::<c_void>(),
                needed,
                &mut needed,
            );
            CloseHandle(token);
            if ok == 0 {
                return None;
            }
            let user = &*buf.as_ptr().cast::<TOKEN_USER>();
            let mut sid_w: *mut u16 = std::ptr::null_mut();
            if ConvertSidToStringSidW(user.User.Sid, &mut sid_w) == 0 {
                return None;
            }
            let mut sid = String::new();
            let mut p = sid_w;
            while *p != 0 {
                sid.push(char::from_u32(*p as u32).unwrap_or('?'));
                p = p.add(1);
            }
            LocalFree(sid_w.cast::<c_void>());

            // Protected DACL: full control for SYSTEM and the owner, nobody else.
            let sddl = wide(&format!("D:P(A;;GA;;;SY)(A;;GA;;;{sid})"));
            let mut sd: *mut c_void = std::ptr::null_mut();
            if ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                (&mut sd as *mut *mut c_void).cast(),
                std::ptr::null_mut(),
            ) == 0
            {
                return None;
            }
            Some(OwnerSd(sd))
        }
    }

    /// A manual-reset event, closed on drop.
    struct Event(HANDLE);
    impl Event {
        fn new() -> Option<Event> {
            // SAFETY: default security, manual reset, not signalled, unnamed.
            let h = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
            (!h.is_null()).then_some(Event(h))
        }
    }
    impl Drop for Event {
        fn drop(&mut self) {
            // SAFETY: an event this owns, closed once.
            unsafe { CloseHandle(self.0) };
        }
    }
    // SAFETY: an event handle may be waited on and signalled from any thread.
    unsafe impl Send for Event {}
    // SAFETY: as above.
    unsafe impl Sync for Event {}

    /// Waits for an overlapped operation on `h` that was just started.
    /// `started` is the starting call's return value. Returns the bytes moved
    /// if the operation completes, and None if it fails, `timeout_ms` passes or
    /// `stop` is set. On a timeout or stop it cancels the operation and waits
    /// for the cancel, so `ov` and its buffer are free to reuse.
    ///
    /// # Safety
    /// `h` is open, and `ov` is the OVERLAPPED the operation was started with,
    /// with its hEvent set.
    unsafe fn finish(h: HANDLE, ov: &mut OVERLAPPED, started: i32, stop: HANDLE, timeout_ms: u32) -> Option<u32> {
        // SAFETY: no arguments; thread-local state.
        if started == 0 && unsafe { GetLastError() } != ERROR_IO_PENDING {
            return None;
        }
        let waits = [ov.hEvent, stop];
        let mut n: u32 = 0;
        // SAFETY: two live event handles.
        if unsafe { WaitForMultipleObjects(2, waits.as_ptr(), 0, timeout_ms) } == WAIT_OBJECT_0 {
            // SAFETY: the operation `ov` started on `h`, complete now.
            return (unsafe { GetOverlappedResult(h, ov, &mut n, 0) } != 0).then_some(n);
        }
        // SAFETY: as above; the second call waits for the cancel to land.
        unsafe {
            CancelIoEx(h, ov);
            GetOverlappedResult(h, ov, &mut n, 1);
        }
        None
    }

    fn overlapped(event: &Event) -> OVERLAPPED {
        // SAFETY: OVERLAPPED is plain data, all-zero is its documented start.
        let mut ov: OVERLAPPED = unsafe { std::mem::zeroed() };
        ov.hEvent = event.0;
        ov
    }

    /// One live client connection: the instance handle plus a closed flag,
    /// both guarded by one mutex so a send can never race the owner's
    /// CloseHandle (no write-after-close, no handle-reuse ABA). Only the
    /// instance's thread closes it, after its read has finished.
    struct PipeConn {
        state: Mutex<(HANDLE, bool)>, // (handle, closed)
        open: AtomicBool,             // presented the secret: counts against the cap
        write_done: Event,            // signalled when a send's write completes
        stop: HANDLE,                 // the server's stop event
    }
    // SAFETY: the handle is only ever used under the mutex, which is what
    // makes a send and the owner's close unable to overlap; the events may be
    // used from any thread.
    unsafe impl Send for PipeConn {}
    // SAFETY: as above.
    unsafe impl Sync for PipeConn {}

    impl PipeConn {
        /// Writes one frame to the client. Gives up after WRITE_TIMEOUT_MS, so
        /// a client that stops reading cannot block the sender.
        fn send(&self, framed: &[u8]) -> bool {
            let guard = self.state.lock().unwrap();
            let (handle, closed) = *guard;
            if closed {
                return false;
            }
            let mut ov = overlapped(&self.write_done);
            // SAFETY: an open handle (the lock is held and `closed` is false),
            // a live slice, and an OVERLAPPED that lives until `finish` has
            // waited the write out.
            unsafe {
                let started = WriteFile(handle, framed.as_ptr(), framed.len() as u32,
                                        std::ptr::null_mut(), &mut ov);
                finish(handle, &mut ov, started, self.stop, WRITE_TIMEOUT_MS) == Some(framed.len() as u32)
            }
        }
        /// Disconnects and closes the handle, under the lock sends take. Only
        /// the instance's thread calls this.
        fn close(&self) {
            let mut guard = self.state.lock().unwrap();
            let (handle, closed) = *guard;
            if !closed {
                // SAFETY: an open handle, closed once: the flag set below
                // keeps every later call from touching it.
                unsafe {
                    DisconnectNamedPipe(handle);
                    CloseHandle(handle);
                }
                *guard = (std::ptr::null_mut(), true);
            }
        }
    }

    struct Shared {
        stop: AtomicBool,
        stop_event: Event, // set once on stop; ends every connect, read and write wait
        conns: Mutex<HashMap<u32, Arc<PipeConn>>>,
        next_id: AtomicU32, // conn ids from 1, never reused (0 = in-process)
        secret: ClockworkCommsSecret,
        refused: AtomicU64,
    }

    /// Whether a client that just presented the secret may open: under the
    /// registry lock, so two cannot both become the client.
    fn admit(shared: &Shared, id: u32) -> bool {
        let conns = shared.conns.lock().unwrap();
        if conns.values().any(|c| c.open.load(Ordering::Relaxed)) {
            return false;
        }
        if let Some(c) = conns.get(&id) {
            c.open.store(true, Ordering::Relaxed);
        }
        true
    }

    pub(super) struct PipeServer {
        shared: Arc<Shared>,
        threads: Vec<JoinHandle<()>>,
    }

    impl StreamServerImpl for PipeServer {
        fn send(&self, conn_id: u32, data: &[u8]) -> bool {
            let conn = self.shared.conns.lock().unwrap().get(&conn_id).cloned();
            let Some(conn) = conn else { return false };
            conn.send(&frame_packet(data))
        }
        fn refused(&self) -> u64 {
            self.shared.refused.load(Ordering::Relaxed)
        }
    }

    impl Drop for PipeServer {
        fn drop(&mut self) {
            // The flag stops the loops between waits. The event ends any
            // connect, read or write wait in progress.
            self.shared.stop.store(true, Ordering::Release);
            // SAFETY: a live event.
            unsafe { SetEvent(self.shared.stop_event.0) };
            for j in self.threads.drain(..) {
                let _ = j.join();
            }
        }
    }

    /// Serves one connected client until EOF, a protocol violation or the
    /// stop, and returns its Gate. The connection cap counts only clients that
    /// presented the secret. Each read blocks until the client sends,
    /// disconnects or the server stops, and before the secret also ends at
    /// AUTH_DEADLINE.
    fn serve_client(conn: &PipeConn, id: u32, shared: &Shared, host: &Host, read_done: &Event) -> Gate {
        let mut acc: Vec<u8> = Vec::new();
        let mut tmp = [0u8; 8192];
        let opened = Instant::now();
        let mut gate = Gate::Waiting;
        loop {
            if shared.stop.load(Ordering::Acquire) {
                break;
            }
            let timeout_ms = if gate == Gate::Waiting {
                let left = AUTH_DEADLINE.saturating_sub(opened.elapsed());
                if left.is_zero() {
                    gate = Gate::Refused;
                    break;
                }
                u32::try_from(left.as_millis().max(1)).unwrap_or(INFINITE - 1)
            } else {
                INFINITE
            };
            let handle = {
                let guard = conn.state.lock().unwrap();
                let (handle, closed) = *guard;
                if closed {
                    break;
                }
                handle
            };
            let mut ov = overlapped(read_done);
            // SAFETY: the handle stays open while this thread, its owner,
            // serves it; `tmp` and `ov` live until `finish` has waited the
            // read out.
            let got = unsafe {
                let started = ReadFile(handle, tmp.as_mut_ptr(), tmp.len() as u32, std::ptr::null_mut(), &mut ov);
                finish(handle, &mut ov, started, shared.stop_event.0, timeout_ms)
            };
            let Some(got) = got else {
                // AUTH_DEADLINE passed, the server stopped, or the client
                // disconnected (a broken pipe).
                if gate == Gate::Waiting && opened.elapsed() >= AUTH_DEADLINE {
                    gate = Gate::Refused;
                }
                break;
            };
            if got == 0 {
                continue;
            }
            acc.extend_from_slice(&tmp[..got as usize]);
            if gate == Gate::Waiting {
                gate = check_auth(&mut acc, &shared.secret);
                if gate == Gate::Open && !admit(shared, id) {
                    gate = Gate::OverCap;
                }
                if gate == Gate::Waiting {
                    continue;
                }
                if gate != Gate::Open {
                    break;
                }
            }
            if !drain_frames(&mut acc, |osc| host.packet(id, osc)) {
                break; // protocol violation — drop the connection
            }
        }
        gate
    }

    /// One pipe instance: (create if needed →) wait for a client → serve →
    /// disconnect → loop. `pre` carries the first, squat-checked instance
    /// (as usize — a raw HANDLE isn't Send); `instances` is how many the pipe
    /// has in all.
    fn run_instance(
        mut pre: Option<usize>,
        name_w: Vec<u16>,
        sd: Arc<OwnerSd>,
        instances: u32,
        shared: Arc<Shared>,
        host: Host,
    ) {
        // Signalled when this instance's connect or read completes. They
        // never overlap, so one event serves both.
        let Some(io_done) = Event::new() else { return };
        while !shared.stop.load(Ordering::Acquire) {
            let handle = match pre.take() {
                Some(h) => h as HANDLE,
                None => {
                    let sa = SECURITY_ATTRIBUTES {
                        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                        lpSecurityDescriptor: sd.0,
                        bInheritHandle: 0,
                    };
                    // SAFETY: a NUL-terminated wide name and a security
                    // descriptor that outlives the call (`sd` is an `Arc`).
                    let h = unsafe {
                        CreateNamedPipeW(
                            name_w.as_ptr(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                                | PIPE_REJECT_REMOTE_CLIENTS,
                            instances,
                            PIPE_BUF_BYTES,
                            PIPE_BUF_BYTES,
                            0,
                            &sa,
                        )
                    };
                    if h == INVALID_HANDLE_VALUE {
                        return; // can't recreate the instance — retire the slot
                    }
                    h
                }
            };

            // Wait for a client to connect, or for the stop.
            let mut ov = overlapped(&io_done);
            // SAFETY: an instance handle this thread owns; `ov` lives until
            // the connect is complete or waited out.
            let connected = unsafe {
                let started = ConnectNamedPipe(handle, &mut ov);
                (started == 0 && GetLastError() == ERROR_PIPE_CONNECTED)
                    || finish(handle, &mut ov, started, shared.stop_event.0, INFINITE).is_some()
            };
            if !connected || shared.stop.load(Ordering::Acquire) {
                // SAFETY: the handle is this thread's and is not used again.
                unsafe { CloseHandle(handle) };
                continue;
            }

            let id = shared.next_id.fetch_add(1, Ordering::Relaxed);
            let Some(write_done) = Event::new() else {
                // SAFETY: as above.
                unsafe { CloseHandle(handle) };
                continue;
            };
            let conn = Arc::new(PipeConn {
                state: Mutex::new((handle, false)),
                open: AtomicBool::new(false),
                write_done,
                stop: shared.stop_event.0,
            });
            {
                // One instance is the client's and one is for a connection
                // still to present the secret; the OS refuses a third. Nothing
                // displaces a connection.
                shared.conns.lock().unwrap().insert(id, conn.clone());
            }

            let gate = serve_client(&conn, id, &shared, &host, &io_done);

            shared.conns.lock().unwrap().remove(&id);
            conn.close();
            if gate == Gate::Refused {
                refuse(&shared.refused, id);
            } else if gate == Gate::OverCap {
                clockwork_log::log!("[osc] pipe conn {id} closed — over the connection cap");
            } else if gate == Gate::Open && !shared.stop.load(Ordering::Acquire) {
                host.closed(id);
            }
        }
    }

    pub(super) fn start(
        host: Host,
        secret: ClockworkCommsSecret,
        name: &str,
    ) -> Option<super::ClockworkOscStream> {
        let full = if name.starts_with(r"\\.\pipe\") {
            name.to_string()
        } else {
            format!(r"\\.\pipe\{name}")
        };
        let name_w = wide(&full);
        let sd = Arc::new(owner_only_sd()?);
        // Two instances: the client's, and one for a connection still to
        // present the secret, so a silent one never holds the client's.
        let instances = 2;

        // First instance is created here, squat-checked, so a hostile process
        // already holding the name fails the START (and start returns null)
        // rather than sitting between us and our clients.
        let sa = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: sd.0,
            bInheritHandle: 0,
        };
        // SAFETY: a NUL-terminated wide name and a security descriptor that
        // outlives the call.
        let first = unsafe {
            CreateNamedPipeW(
                name_w.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                instances,
                PIPE_BUF_BYTES,
                PIPE_BUF_BYTES,
                0,
                &sa,
            )
        };
        if first == INVALID_HANDLE_VALUE {
            clockwork_log::log!("[osc] named pipe create {full} failed (name in use?)");
            return None;
        }

        let Some(stop_event) = Event::new() else {
            // SAFETY: nobody took `first`, so it is closed here, once.
            unsafe { CloseHandle(first) };
            return None;
        };
        let shared = Arc::new(Shared {
            stop: AtomicBool::new(false),
            stop_event,
            conns: Mutex::new(HashMap::new()),
            next_id: AtomicU32::new(1),
            secret,
            refused: AtomicU64::new(0),
        });
        let mut threads = Vec::new();
        for i in 0..instances {
            let pre = if i == 0 { Some(first as usize) } else { None };
            let (t_name, t_sd, t_shared) = (name_w.clone(), sd.clone(), shared.clone());
            if let Ok(j) = std::thread::Builder::new()
                .name("cw-osc-pipe".into())
                .spawn(move || run_instance(pre, t_name, t_sd, instances, t_shared, host))
            {
                threads.push(j);
            }
        }
        if threads.is_empty() {
            // SAFETY: nobody took `first`, so it is closed here, once.
            unsafe { CloseHandle(first) };
            return None;
        }
        Some(super::ClockworkOscStream(Box::new(PipeServer { shared, threads })))
    }
}

/// Start a named-pipe OSC server (Windows). `name` is a byte string (ptr+len,
/// not NUL-terminated) — either a bare name ("clockwork-cmd") that is
/// prefixed with `\\.\pipe\`, or a full pipe path. Instances carry an
/// owner-only DACL and reject remote clients; `max_conns` is the instance
/// count, so an over-cap connect fails busy. Same handle/send/stop ABI as the
/// socket stream servers, the secret included: without one it does not
/// start. Always null on non-Windows platforms.
///
/// # Safety
/// `secret` is null or live; `name` is null or readable for `name_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_pipe_start(
    ctx: *mut c_void,
    on_packet: crate::stream::StreamPacketFn,
    on_closed: crate::stream::StreamClosedFn,
    secret: *const crate::secret::ClockworkCommsSecret,
    name: *const u8,
    name_len: u32,
) -> *mut ClockworkOscStream {
    #[cfg(windows)]
    {
        clockwork_osc::ffi::no_unwind(std::ptr::null_mut(), || {
            // SAFETY: null or live, per the contract.
            let Some(secret) = (unsafe { crate::stream::take_secret("named pipe", secret) }) else {
                return std::ptr::null_mut();
            };
            if name.is_null() {
                return std::ptr::null_mut();
            }
            // SAFETY: readable for `name_len` bytes, per the contract.
            let bytes = unsafe { std::slice::from_raw_parts(name, name_len as usize) };
            let Ok(name) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            if name.is_empty() {
                return std::ptr::null_mut();
            }
            let host = crate::stream::Host { ctx, on_packet, on_closed };
            match imp::start(host, secret, name) {
                Some(srv) => Box::into_raw(Box::new(srv)),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(windows))]
    {
        let _ = (ctx, on_packet, on_closed, secret, name, name_len);
        std::ptr::null_mut()
    }
}

// Only a Windows runner has a named pipe: the ci.yml `rust (windows-x64)` job
// runs these.
#[cfg(all(test, windows))]
mod tests {
    use super::*;
    use crate::stream::tests::{frame, on_closed, on_packet, refused, stop, test_secret, wait_until, Cap};
    use std::ffi::c_void;
    use std::fs::OpenOptions;
    use std::io::Write;
    use std::time::Duration;
    use clockwork_osc::encode;

    fn start(cap: &Cap, name: &str) -> *mut ClockworkOscStream {
        let ctx = cap as *const Cap as *mut c_void;
        // SAFETY: `name` is a live str; the secret outlives the server.
        let h = unsafe {
            clockwork_osc_pipe_start(ctx, on_packet, on_closed, test_secret(), name.as_ptr(), name.len() as u32)
        };
        assert!(!h.is_null(), "pipe server should start");
        h
    }

    // A client of the pipe: the OS retries a busy instance for a while.
    fn connect(name: &str) -> std::fs::File {
        let path = format!(r"\\.\pipe\{name}");
        let deadline = std::time::Instant::now() + Duration::from_secs(30);
        loop {
            match OpenOptions::new().read(true).write(true).open(&path) {
                Ok(f) => return f,
                Err(e) if std::time::Instant::now() < deadline => {
                    let _ = e;
                    std::thread::sleep(Duration::from_millis(20));
                }
                Err(e) => panic!("could not open {path}: {e}"),
            }
        }
    }

    // A client that presents the secret and sends one packet, which the
    // host receives, then leaves, which the host hears: the server is
    // serving. `served` is how many such clients came before it; a hostile
    // client is never reported closed, since the host never heard of it.
    fn a_real_client_is_served(cap: &Cap, name: &str, served: usize) {
        let before = cap.packets.lock().unwrap().len();
        let mut c = connect(name);
        c.write_all(&frame(&test_secret().auth_packet())).unwrap();
        c.write_all(&frame(&encode("/alive", &[clockwork_osc::OscArg::Int(served as i32)]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().len() > before), "client {served} delivered");
        drop(c);
        assert!(wait_until(|| cap.closed.lock().unwrap().len() > served), "client {served} seen closed");
    }

    // Every file of the comms corpus, written to the pipe by a client as its
    // first bytes, in reads cut at a few places: the server refuses or
    // ignores it and still admits the next client that presents the secret.
    #[test]
    fn the_corpus_through_a_real_pipe_leaves_the_server_serving() {
        let cap = Cap::new();
        let name = format!("clockwork-pipe-corpus-{}", std::process::id());
        let h = start(&cap, &name);
        let mut hostile = 0usize;
        let mut served = 0usize;
        clockwork_corpus::replay_with(env!("CARGO_MANIFEST_DIR"), |data| {
            for cut in [0usize, 1, 3, data.len() / 2] {
                let cut = cut.min(data.len());
                let mut c = connect(&name);
                let _ = c.write_all(&data[..cut]);
                std::thread::sleep(Duration::from_millis(40));
                let _ = c.write_all(&data[cut..]);
                std::thread::sleep(Duration::from_millis(40));
                drop(c);
                hostile += 1;
            }
            a_real_client_is_served(&cap, &name, served);
            served += 1;
        });
        assert!(hostile > 0, "no corpus under rust/clockwork-comms/corpus");
        assert!(refused(h) > 0, "the corpus holds no frame that is the secret");
        stop(h);
    }
}
