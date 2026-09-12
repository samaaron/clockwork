// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Unix-domain-socket datagram OSC ingress — the kernel-ACL'd sibling of the
//! UDP control port (`ffi::clockwork_osc_ingress_start_with_src`).
//!
//! The server binds a filesystem socket path (created `0600`; put it in a
//! `0700` directory for the full owner-only guarantee) and delivers each
//! datagram verbatim to the host TOGETHER WITH the sender's socket path, so a
//! transport can intern an origin token and address a reply back. A client
//! that wants replies must bind its own path (macOS has no autobind); an
//! unbound sender is delivered with an empty peer path and is unaddressable.
//!
//! Unix-only: on Windows the start function returns null; the named-pipe
//! server (pipe.rs) is the platform's owner-ACL'd analogue.

#[cfg(unix)]
mod imp {
    use std::ffi::c_void;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixDatagram;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread::JoinHandle;
    use std::time::Duration;

    use clockwork_osc::ffi::no_unwind;

    /// emit-with-source callback: (ctx, peer_path, peer_len, osc, len). Both
    /// byte strings are valid only for the duration of the call; `peer_path`
    /// is empty when the sender did not bind a path (unaddressable).
    pub type EmitPathFn =
        extern "C" fn(*mut c_void, *const u8, u32, *const u8, u32);

    #[derive(Clone, Copy)]
    struct HostPath {
        ctx: *mut c_void,
        emit: EmitPathFn,
    }
    // SAFETY: `ctx` is an opaque token this crate never dereferences, only
    // hands to `emit`, and the C++ side's contract is that it outlives the
    // server and the callback is thread-safe; a fn pointer is Send + Sync.
    unsafe impl Send for HostPath {}
    // SAFETY: as above.
    unsafe impl Sync for HostPath {}

    impl HostPath {
        fn emit(&self, peer: &[u8], osc: &[u8]) {
            (self.emit)(self.ctx, peer.as_ptr(), peer.len() as u32,
                        osc.as_ptr(), osc.len() as u32);
        }
    }

    /// A running UDS datagram server: one recv thread; sends go out through a
    /// clone of the bound socket, so replies originate from the server path.
    /// Dropping it stops the thread, closes the socket, and unlinks the path.
    pub struct ClockworkOscUds {
        stop: Arc<AtomicBool>,
        join: Option<JoinHandle<()>>,
        sender: UnixDatagram,
        path: PathBuf,
    }

    impl Drop for ClockworkOscUds {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::Relaxed);
            if let Some(j) = self.join.take() {
                let _ = j.join();
            }
            let _ = std::fs::remove_file(&self.path);
        }
    }

    fn run_recv(socket: UnixDatagram, stop: Arc<AtomicBool>, host: HostPath) {
        // Timed read so the loop wakes to check `stop`; ~0% idle CPU (the same
        // idiom as the UDP recv loops in ffi.rs).
        let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
        let mut buf = vec![0u8; 65536];
        while !stop.load(Ordering::Relaxed) {
            match socket.recv_from(&mut buf) {
                Ok((n, src)) => {
                    let peer: &[u8] = src
                        .as_pathname()
                        .and_then(Path::to_str)
                        .map(str::as_bytes)
                        .unwrap_or(&[]);
                    // A client that bound with the full sizeof(sockaddr_un)
                    // has its path reported NUL-padded by the kernel; trim so
                    // the peer stays addressable for replies.
                    let end = peer.iter().position(|&b| b == 0).unwrap_or(peer.len());
                    no_unwind((), || host.emit(&peer[..end], &buf[..n]));
                }
                Err(e) if clockwork_osc_net::recv_retryable(&e) => {}
                Err(_) => break, // socket closed / fatal
            }
        }
    }

    pub fn start(host_ctx: *mut c_void, emit: EmitPathFn, path: &str) -> Option<Box<ClockworkOscUds>> {
        let pb = PathBuf::from(path);
        // The server owns its socket path: replace a stale file from a previous
        // run (bind fails on an existing path).
        let _ = std::fs::remove_file(&pb);
        let socket = match UnixDatagram::bind(&pb) {
            Ok(s) => s,
            Err(e) => {
                clockwork_log::log!("[osc] UDS dgram bind {path} failed: {e}");
                return None;
            }
        };
        // Owner-only. There is a bind→chmod window; the caller closes it by
        // placing the socket in a 0700 directory (a per-session directory).
        if let Err(e) = std::fs::set_permissions(&pb, std::fs::Permissions::from_mode(0o600)) {
            clockwork_log::log!("[osc] UDS dgram chmod {path} failed: {e}");
            return None;
        }
        let sender = match socket.try_clone() {
            Ok(s) => s,
            Err(e) => {
                clockwork_log::log!("[osc] UDS dgram clone failed: {e}");
                return None;
            }
        };
        let stop = Arc::new(AtomicBool::new(false));
        let host = HostPath { ctx: host_ctx, emit };
        let t_stop = stop.clone();
        let join = std::thread::Builder::new()
            .name("ss-osc-uds".into())
            .spawn(move || run_recv(socket, t_stop, host))
            .ok()?;
        Some(Box::new(ClockworkOscUds { stop, join: Some(join), sender, path: pb }))
    }

    pub fn send(server: &ClockworkOscUds, path: &[u8], data: &[u8]) -> bool {
        let Ok(path) = std::str::from_utf8(path) else { return false };
        if path.is_empty() {
            return false;
        }
        server.sender.send_to(data, path).is_ok()
    }
}

#[cfg(unix)]
pub use imp::{EmitPathFn, ClockworkOscUds};

// Windows: no AF_UNIX in Rust std — the start stub returns null and the C++
// side reports the flag as unsupported (pipe.rs is the Windows analogue).
#[cfg(not(unix))]
pub struct ClockworkOscUds;
#[cfg(not(unix))]
pub type EmitPathFn =
    extern "C" fn(*mut std::ffi::c_void, *const u8, u32, *const u8, u32);

// ── C ABI ────────────────────────────────────────────────────────────────────

use std::ffi::c_void;
#[cfg(unix)]
use std::slice;

/// Start a UDS datagram OSC server bound to `path` (byte string, ptr+len, not
/// NUL-terminated; created 0600, replacing a stale socket file). Each datagram
/// is delivered verbatim to `emit` with the sender's socket path. Returns an
/// owning pointer (null on failure, and always null on Windows); free with
/// [`clockwork_osc_uds_stop`]. `ctx`/`emit` must outlive it.
///
/// # Safety
/// `path` is null or readable for `path_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_uds_dgram_start(
    ctx: *mut c_void,
    emit: EmitPathFn,
    path: *const u8,
    path_len: u32,
) -> *mut ClockworkOscUds {
    #[cfg(unix)]
    {
        clockwork_osc::ffi::no_unwind(std::ptr::null_mut(), || {
            if path.is_null() {
                return std::ptr::null_mut();
            }
            // SAFETY: readable for `path_len` bytes, per the contract.
            let bytes = unsafe { slice::from_raw_parts(path, path_len as usize) };
            let Ok(path) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            if path.is_empty() {
                return std::ptr::null_mut();
            }
            match imp::start(ctx, emit, path) {
                Some(srv) => Box::into_raw(srv),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(unix))]
    {
        let _ = (ctx, emit, path, path_len);
        std::ptr::null_mut()
    }
}

/// Send one OSC packet to the peer bound at `path` (byte string, ptr+len),
/// from the server's own socket (so the client sees the server path as the
/// sender). Returns 1 on send, 0 on failure/empty path. Off the audio thread.
///
/// # Safety
/// `handle` is null or a live handle; `path` and `data` are null or readable
/// for `path_len` and `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_uds_dgram_send(
    handle: *mut ClockworkOscUds,
    path: *const u8,
    path_len: u32,
    data: *const u8,
    len: u32,
) -> i32 {
    #[cfg(unix)]
    {
        if handle.is_null() || path.is_null() || data.is_null() {
            return 0;
        }
        // SAFETY: a live handle and two readable byte strings, per the
        // contract; nulls were refused above.
        let (me, path, data) = unsafe {
            (
                &*handle,
                slice::from_raw_parts(path, path_len as usize),
                slice::from_raw_parts(data, len as usize),
            )
        };
        clockwork_osc::ffi::no_unwind(0, || imp::send(me, path, data) as i32)
    }
    #[cfg(not(unix))]
    {
        let _ = (handle, path, path_len, data, len);
        0
    }
}

/// Stop the recv thread, close the socket, unlink the path, free the server.
///
/// # Safety
/// `handle` is null or from [`clockwork_osc_uds_dgram_start`], not yet
/// stopped, and nothing uses it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_osc_uds_stop(handle: *mut ClockworkOscUds) {
    if handle.is_null() {
        return;
    }
    // SAFETY: the box the start function leaked, freed once, here.
    #[cfg(unix)]
    clockwork_osc::ffi::no_unwind((), || drop(unsafe { Box::from_raw(handle) }));
    #[cfg(not(unix))]
    let _ = handle;
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixDatagram;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};
    use clockwork_osc::{decode, encode, OscArg};

    // Capture (peer_path, bytes) pairs via the C callback.
    struct Cap(Mutex<Vec<(String, Vec<u8>)>>);
    extern "C" fn collect(ctx: *mut c_void, peer: *const u8, peer_len: u32,
                          osc: *const u8, len: u32) {
        // SAFETY: `ctx` is the boxed `Cap` each test keeps alive until after
        // the stop; the two byte strings are the callback's contract.
        let (c, peer, bytes) = unsafe {
            (
                &*ctx.cast::<Cap>(),
                slice::from_raw_parts(peer, peer_len as usize),
                slice::from_raw_parts(osc, len as usize),
            )
        };
        let peer = String::from_utf8_lossy(peer).to_string();
        c.0.lock().unwrap().push((peer, bytes.to_vec()));
    }
    // The C ABI, with the test's contract stated once: `h` is a live handle
    // from `start_server`, stopped exactly once at the end.
    fn send(h: *mut ClockworkOscUds, peer: &str, data: &[u8]) -> i32 {
        // SAFETY: see above; both slices are live.
        unsafe {
            clockwork_osc_uds_dgram_send(h, peer.as_ptr(), peer.len() as u32,
                                  data.as_ptr(), data.len() as u32)
        }
    }
    fn stop(h: *mut ClockworkOscUds) {
        // SAFETY: see above.
        unsafe { clockwork_osc_uds_stop(h) };
    }

    fn wait_until(mut f: impl FnMut() -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        false
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("ss-uds-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        dir.join(name)
    }

    fn start_server(cap: &Cap, path: &str) -> *mut ClockworkOscUds {
        let ctx = cap as *const Cap as *mut c_void;
        // SAFETY: `path` is a live str.
        unsafe {
            clockwork_osc_uds_dgram_start(ctx, collect, path.as_ptr(), path.len() as u32)
        }
    }

    // A bound client's datagram arrives verbatim, with the client's path, and
    // a reply sent to that path round-trips back to the client.
    #[test]
    fn round_trip_with_reply() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let server_path = tmp("rt-server.sock");
        let client_path = tmp("rt-client.sock");
        let _ = std::fs::remove_file(&client_path);
        let h = start_server(&cap, server_path.to_str().unwrap());
        assert!(!h.is_null());

        let client = UnixDatagram::bind(&client_path).unwrap();
        client.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let msg = encode("/s_new", &[OscArg::Str("sine".into()), OscArg::Int(1000)]);
        client.send_to(&msg, &server_path).unwrap();

        let ok = wait_until(|| {
            cap.0.lock().unwrap().iter().any(|(peer, bytes)| {
                peer == client_path.to_str().unwrap()
                    && decode(bytes).map(|m| m.addr == "/s_new").unwrap_or(false)
            })
        });
        assert!(ok, "datagram should arrive with the client's socket path");

        // Reply to the captured peer path; the client receives it.
        let reply = encode("/done", &[OscArg::Str("/s_new".into())]);
        let peer = client_path.to_str().unwrap();
        let sent = send(h, peer, &reply);
        assert_eq!(sent, 1);
        let mut buf = [0u8; 1024];
        let (n, _) = client.recv_from(&mut buf).expect("reply delivery");
        assert_eq!(decode(&buf[..n]).unwrap().addr, "/done");

        stop(h);
        let _ = std::fs::remove_file(&client_path);
    }

    // An unbound sender is delivered with an empty peer path (unaddressable),
    // and replying to an empty path fails cleanly.
    #[test]
    fn unbound_sender_is_unaddressable() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let server_path = tmp("unbound-server.sock");
        let h = start_server(&cap, server_path.to_str().unwrap());
        assert!(!h.is_null());

        let client = UnixDatagram::unbound().unwrap();
        let msg = encode("/clockwork/status", &[]);
        let ok = wait_until(|| {
            let _ = client.send_to(&msg, &server_path);
            cap.0.lock().unwrap().iter().any(|(peer, _)| peer.is_empty())
        });
        assert!(ok, "unbound sender should be delivered with an empty peer path");

        let sent = send(h, "", &msg);
        assert_eq!(sent, 0, "empty peer path must not be sendable");

        stop(h);
    }

    // The socket file is created owner-only (0600) and a stale file from a
    // dead server is replaced on the next start.
    #[test]
    fn socket_file_owner_only_and_stale_replaced() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let path = tmp("perm-server.sock");
        std::fs::write(&path, b"stale").unwrap(); // simulate a leftover file

        let h = start_server(&cap, path.to_str().unwrap());
        assert!(!h.is_null(), "a stale file at the path must be replaced");
        let mode = std::fs::metadata(&path).unwrap().permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "socket file must be owner-only");

        stop(h);
        assert!(!path.exists(), "stop should unlink the socket path");
    }

    // Stop unlinks the path and joins the recv thread promptly.
    #[test]
    fn stop_is_prompt() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let path = tmp("stop-server.sock");
        let h = start_server(&cap, path.to_str().unwrap());
        assert!(!h.is_null());
        let t0 = Instant::now();
        stop(h);
        assert!(t0.elapsed() < Duration::from_secs(1), "stop should be prompt");
        assert!(!path.exists());
    }
}
