// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Sleeps a thread until one of its descriptors is ready (bytes to read, a
//! connection to accept, a hang-up), a deadline passes, or another thread
//! calls `wake` to stop it.
//!
//! `wait` polls the descriptors together with a wake. On POSIX the wake is a
//! pipe that `wake` writes a byte to, and the wait is `poll`. On Windows it is
//! a loopback UDP socket that `wake` sends to itself, because `WSAPoll` waits
//! on sockets only. Nothing drains the wake, so it stays set: a wake before
//! the wait still ends it, and one wake ends every waiter.
//!
//! `wait` retries EINTR with the time left to the deadline, so a signal
//! neither ends the caller's thread nor makes it spin.

use std::io;
use std::time::{Duration, Instant};

#[cfg(unix)]
pub type RawSock = std::os::fd::RawFd;
#[cfg(windows)]
pub type RawSock = std::os::windows::io::RawSocket;

/// The raw descriptor `wait` polls: a file descriptor on POSIX, a socket on
/// Windows.
pub trait AsRawSock {
    fn raw_sock(&self) -> RawSock;
}

#[cfg(unix)]
impl<T: std::os::fd::AsRawFd> AsRawSock for T {
    fn raw_sock(&self) -> RawSock {
        self.as_raw_fd()
    }
}
#[cfg(windows)]
impl<T: std::os::windows::io::AsRawSocket> AsRawSock for T {
    fn raw_sock(&self) -> RawSock {
        self.as_raw_socket()
    }
}

/// The most descriptors one wait takes.
pub const MAX_SOCKS: usize = 8;

/// The time left until `deadline`, in whole milliseconds rounded up so a wait
/// never ends just short of it; -1, for ever, with none.
fn timeout_ms(deadline: Option<Instant>) -> i32 {
    deadline.map_or(-1, |d| {
        let left = d.saturating_duration_since(Instant::now());
        let ms = left.as_micros().div_ceil(1000);
        i32::try_from(ms).unwrap_or(i32::MAX)
    })
}

#[cfg(unix)]
pub struct ReadyWait {
    read: std::os::fd::OwnedFd,
    write: std::os::fd::OwnedFd,
}

#[cfg(unix)]
impl ReadyWait {
    pub fn new() -> io::Result<ReadyWait> {
        use std::os::fd::FromRawFd;
        let mut fds = [0i32; 2];
        // SAFETY: `fds` is two writable ints, which pipe fills on success.
        if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: two descriptors pipe just made, owned from here.
        let (read, write) = unsafe {
            (std::os::fd::OwnedFd::from_raw_fd(fds[0]), std::os::fd::OwnedFd::from_raw_fd(fds[1]))
        };
        for fd in fds {
            // SAFETY: a descriptor this owns; F_SETFD and F_SETFL touch only its flags.
            unsafe {
                libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC);
                libc::fcntl(fd, libc::F_SETFL, libc::O_NONBLOCK);
            }
        }
        Ok(ReadyWait { read, write })
    }

    /// Blocks until one of `socks` is ready, `deadline` passes, or
    /// [`wake`](Self::wake) is called. Returns true if one of `socks` is ready.
    pub fn wait(&self, socks: &[RawSock], deadline: Option<Instant>) -> io::Result<bool> {
        use std::os::fd::AsRawFd;
        assert!(socks.len() <= MAX_SOCKS, "a wait takes at most {MAX_SOCKS} descriptors");
        let idle = libc::pollfd { fd: -1, events: 0, revents: 0 };
        let mut fds = [idle; MAX_SOCKS + 1];
        fds[0] = libc::pollfd { fd: self.read.as_raw_fd(), events: libc::POLLIN, revents: 0 };
        for (slot, &fd) in fds[1..].iter_mut().zip(socks) {
            *slot = libc::pollfd { fd, events: libc::POLLIN, revents: 0 };
        }
        let n = socks.len() + 1;
        loop {
            // SAFETY: `n` pollfds in a live array.
            let rc = unsafe { libc::poll(fds.as_mut_ptr(), n as libc::nfds_t, timeout_ms(deadline)) };
            if rc >= 0 {
                return Ok(fds[1..n].iter().any(|f| f.revents != 0));
            }
            let e = io::Error::last_os_error();
            if e.kind() != io::ErrorKind::Interrupted {
                return Err(e);
            }
        }
    }

    /// Ends every wait, current and future.
    pub fn wake(&self) {
        use std::os::fd::AsRawFd;
        let byte = 1u8;
        // A full pipe is already a wake; any other failure leaves nothing to do.
        // SAFETY: one byte from a live buffer to a descriptor this owns.
        let _ = unsafe { libc::write(self.write.as_raw_fd(), std::ptr::from_ref(&byte).cast(), 1) };
    }
}

#[cfg(windows)]
pub struct ReadyWait {
    // Bound to loopback and connected to itself: a send is a wake.
    wake: std::net::UdpSocket,
}

#[cfg(windows)]
impl ReadyWait {
    pub fn new() -> io::Result<ReadyWait> {
        let wake = std::net::UdpSocket::bind(("127.0.0.1", 0))?;
        wake.connect(wake.local_addr()?)?;
        wake.set_nonblocking(true)?;
        Ok(ReadyWait { wake })
    }

    pub fn wait(&self, socks: &[RawSock], deadline: Option<Instant>) -> io::Result<bool> {
        use std::os::windows::io::AsRawSocket;
        use windows_sys::Win32::Networking::WinSock::{
            WSAGetLastError, WSAPoll, INVALID_SOCKET, POLLRDNORM, SOCKET_ERROR, WSAEINTR, WSAPOLLFD,
        };
        assert!(socks.len() <= MAX_SOCKS, "a wait takes at most {MAX_SOCKS} descriptors");
        let idle = WSAPOLLFD { fd: INVALID_SOCKET, events: 0, revents: 0 };
        let mut fds = [idle; MAX_SOCKS + 1];
        fds[0] = WSAPOLLFD { fd: self.wake.as_raw_socket() as usize, events: POLLRDNORM, revents: 0 };
        for (slot, &s) in fds[1..].iter_mut().zip(socks) {
            *slot = WSAPOLLFD { fd: s as usize, events: POLLRDNORM, revents: 0 };
        }
        let n = socks.len() + 1;
        loop {
            // SAFETY: `n` WSAPOLLFDs in a live array.
            let rc = unsafe { WSAPoll(fds.as_mut_ptr(), n as u32, timeout_ms(deadline)) };
            if rc != SOCKET_ERROR {
                return Ok(fds[1..n].iter().any(|f| f.revents != 0));
            }
            // SAFETY: reads this thread's last socket error.
            let code = unsafe { WSAGetLastError() };
            if code != WSAEINTR {
                return Err(io::Error::from_raw_os_error(code));
            }
        }
    }

    pub fn wake(&self) {
        let _ = self.wake.send(&[1]);
    }
}

impl ReadyWait {
    /// Blocks for `d`, or until [`wake`](Self::wake) is called. For a pause
    /// after a failed accept, which a ready socket must not end.
    pub fn pause(&self, d: Duration) -> io::Result<()> {
        self.wait(&[], Some(Instant::now() + d)).map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;

    #[test]
    fn a_wait_returns_when_a_socket_is_ready_and_when_it_is_woken() {
        let wait = ReadyWait::new().unwrap();
        let quiet = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        let sock = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        let sender = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        sender.send_to(&[7], sock.local_addr().unwrap()).unwrap();
        // Ready, the second of two: says so.
        assert!(wait.wait(&[quiet.raw_sock(), sock.raw_sock()], None).unwrap());

        std::thread::scope(|s| {
            let waiter = s.spawn(|| wait.wait(&[quiet.raw_sock()], None).unwrap());
            std::thread::sleep(Duration::from_millis(20));
            wait.wake();
            assert!(!waiter.join().unwrap(), "woken, not ready");
        });
        // Woken stays woken: a wait after the wake returns at once.
        assert!(!wait.wait(&[quiet.raw_sock()], None).unwrap());
    }

    #[test]
    fn a_wait_ends_at_its_deadline_and_not_before() {
        let wait = ReadyWait::new().unwrap();
        let quiet = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        let deadline = Instant::now() + Duration::from_millis(30);
        assert!(!wait.wait(&[quiet.raw_sock()], Some(deadline)).unwrap());
        assert!(Instant::now() >= deadline);

        let t0 = Instant::now();
        wait.pause(Duration::from_millis(30)).unwrap();
        assert!(t0.elapsed() >= Duration::from_millis(30));
    }

    // A signal ends poll with EINTR. The wait must carry on for the time left,
    // not return an error that ends the caller's thread or a result that makes
    // it spin.
    #[cfg(unix)]
    #[test]
    fn a_signal_during_a_wait_neither_ends_it_nor_fails_it() {
        use std::os::unix::thread::JoinHandleExt;
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::Arc;

        extern "C" fn nothing(_: libc::c_int) {}
        // SAFETY: a handler that does nothing, installed without SA_RESTART so
        // the signal interrupts poll, which poll never restarts anyway.
        unsafe {
            let mut sa: libc::sigaction = std::mem::zeroed();
            sa.sa_sigaction = nothing as extern "C" fn(libc::c_int) as libc::sighandler_t;
            libc::sigemptyset(&mut sa.sa_mask);
            libc::sigaction(libc::SIGUSR2, &sa, std::ptr::null_mut());
        }

        let wait = Arc::new(ReadyWait::new().unwrap());
        let quiet = UdpSocket::bind(("127.0.0.1", 0)).unwrap();
        let fd = quiet.raw_sock();
        let done = Arc::new(AtomicBool::new(false));
        let waiter = {
            let (wait, done) = (wait.clone(), done.clone());
            std::thread::spawn(move || {
                let r = wait.wait(&[fd], Some(Instant::now() + Duration::from_secs(30)));
                done.store(true, Ordering::SeqCst);
                r
            })
        };
        for _ in 0..20 {
            std::thread::sleep(Duration::from_millis(5));
            // SAFETY: a thread not yet joined, sent a signal it handles.
            unsafe { libc::pthread_kill(waiter.as_pthread_t(), libc::SIGUSR2) };
        }
        assert!(!done.load(Ordering::SeqCst), "a signal ended the wait");
        wait.wake();
        assert!(!waiter.join().unwrap().unwrap(), "woken, not ready, and no error");
    }
}
