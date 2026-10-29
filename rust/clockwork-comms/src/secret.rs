// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The per-boot secret every command port asks for.
//!
//! Whoever launches the host makes 32 random bytes and gets them to the host
//! by a path other processes cannot read: never the command line or the
//! environment. Where the host reads them from is the host's decision; this
//! crate takes the bytes ([`clockwork_comms_secret_from_bytes`]) and hands
//! the secret to every command port the host starts, and a port refuses to
//! start without one. A process that launches its own clients makes one
//! instead, with [`clockwork_comms_secret_generate`].
//!
//! A client proves it holds the secret with the first frame on its
//! connection: `/clockwork/auth` with the 32 bytes as a blob
//! ([`clockwork_comms_secret_auth_packet`]). The server compares the whole
//! packet in time that does not depend on where it differs, and delivers
//! nothing before it.

use clockwork_osc::ffi::no_unwind;
use clockwork_osc::{encode, OscArg};

pub const SECRET_BYTES: usize = 32;

/// The address of a client's first frame.
pub const AUTH_ADDRESS: &str = "/clockwork/auth";

/// The secret. Opaque to C; a port copies it when it starts.
#[derive(Clone)]
pub struct ClockworkCommsSecret([u8; SECRET_BYTES]);

impl ClockworkCommsSecret {
    /// The 32 bytes the launcher made. All zeros is refused: it is what a
    /// launcher that never filled its buffer hands over.
    pub fn from_bytes(bytes: [u8; SECRET_BYTES]) -> Option<Self> {
        (bytes != [0u8; SECRET_BYTES]).then_some(Self(bytes))
    }

    /// 32 bytes from the operating system's random source.
    pub fn generate() -> Option<Self> {
        let mut bytes = [0u8; SECRET_BYTES];
        os_random(&mut bytes).ok()?;
        Self::from_bytes(bytes)
    }

    /// The packet a client sends first.
    pub fn auth_packet(&self) -> Vec<u8> {
        encode(AUTH_ADDRESS, &[OscArg::Blob(self.0.to_vec())])
    }

    /// Whether `packet` is exactly this secret's auth packet. Every byte is
    /// looked at, so how long it takes says nothing about where it differs.
    pub(crate) fn admits(&self, packet: &[u8]) -> bool {
        let want = self.auth_packet();
        if packet.len() != want.len() {
            return false;
        }
        let diff = packet.iter().zip(&want).fold(0u8, |d, (a, b)| d | (a ^ b));
        std::hint::black_box(diff) == 0
    }
}

#[cfg(unix)]
fn os_random(out: &mut [u8]) -> std::io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(out)
}

#[cfg(windows)]
fn os_random(out: &mut [u8]) -> std::io::Result<()> {
    use windows_sys::Win32::Security::Cryptography::{
        BCryptGenRandom, BCRYPT_USE_SYSTEM_PREFERRED_RNG,
    };
    // SAFETY: `out` is a live, writable buffer of `out.len()` bytes, and a
    // null algorithm handle is what BCRYPT_USE_SYSTEM_PREFERRED_RNG asks for.
    let status = unsafe {
        BCryptGenRandom(std::ptr::null_mut(), out.as_mut_ptr(), out.len() as u32,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    };
    if status == 0 { Ok(()) } else { Err(std::io::Error::other(format!("BCryptGenRandom: {status:#x}"))) }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// The secret from the 32 bytes the launcher made, however the host got
/// them. Null for all zeros. Free with [`clockwork_comms_secret_free`].
///
/// # Safety
/// `bytes` is null or readable for 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_comms_secret_from_bytes(bytes: *const u8) -> *mut ClockworkCommsSecret {
    if bytes.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: readable for 32 bytes, per the contract.
    let bytes: [u8; SECRET_BYTES] = unsafe { *bytes.cast::<[u8; SECRET_BYTES]>() };
    match ClockworkCommsSecret::from_bytes(bytes) {
        Some(s) => Box::into_raw(Box::new(s)),
        None => std::ptr::null_mut(),
    }
}

/// A new secret from the operating system's random source, for a process
/// that launches its own clients. Null if the source fails. Free with
/// [`clockwork_comms_secret_free`].
#[no_mangle]
pub extern "C" fn clockwork_comms_secret_generate() -> *mut ClockworkCommsSecret {
    no_unwind(std::ptr::null_mut(), || match ClockworkCommsSecret::generate() {
        Some(s) => Box::into_raw(Box::new(s)),
        None => std::ptr::null_mut(),
    })
}

/// Write the packet a client sends first (unframed OSC) into `out`. Returns
/// its length, or 0 for a null secret or an `out` shorter than that.
///
/// # Safety
/// `secret` is null or live; `out` is null or writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_comms_secret_auth_packet(
    secret: *const ClockworkCommsSecret,
    out: *mut u8,
    cap: u32,
) -> u32 {
    if secret.is_null() || out.is_null() {
        return 0;
    }
    // SAFETY: a live secret, per the contract.
    let packet = unsafe { &*secret }.auth_packet();
    if packet.len() > cap as usize {
        return 0;
    }
    // SAFETY: `out` is writable for `cap` bytes and `packet` fits.
    unsafe { std::ptr::copy_nonoverlapping(packet.as_ptr(), out, packet.len()) };
    packet.len() as u32
}

/// # Safety
/// `secret` is null or from a secret function above, freed once.
#[no_mangle]
pub unsafe extern "C" fn clockwork_comms_secret_free(secret: *mut ClockworkCommsSecret) {
    if !secret.is_null() {
        // SAFETY: the box a secret function leaked, freed once, here.
        drop(unsafe { Box::from_raw(secret) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_osc::decode;

    fn sample() -> ClockworkCommsSecret {
        ClockworkCommsSecret::from_bytes(std::array::from_fn(|i| i as u8)).unwrap()
    }

    #[test]
    fn all_zeros_is_no_secret() {
        assert!(ClockworkCommsSecret::from_bytes([0; SECRET_BYTES]).is_none());
        let mut one = [0; SECRET_BYTES];
        one[31] = 1;
        assert!(ClockworkCommsSecret::from_bytes(one).is_some());
        // SAFETY: a null pointer is what the case passes.
        assert!(unsafe { clockwork_comms_secret_from_bytes(std::ptr::null()) }.is_null());
    }

    #[test]
    fn generated_secrets_differ() {
        let (a, b) = (ClockworkCommsSecret::generate().unwrap(), ClockworkCommsSecret::generate().unwrap());
        assert_ne!(a.0, b.0);
    }

    #[test]
    fn the_auth_packet_is_the_address_and_the_secret_as_a_blob() {
        let s = sample();
        let m = decode(&s.auth_packet()).unwrap();
        assert_eq!(m.addr, AUTH_ADDRESS);
        assert_eq!(m.args, vec![OscArg::Blob(s.0.to_vec())]);
        assert!(s.admits(&s.auth_packet()));
    }

    #[test]
    fn a_packet_that_differs_anywhere_is_not_admitted() {
        let s = sample();
        let good = s.auth_packet();
        for i in 0..good.len() {
            let mut bad = good.clone();
            bad[i] ^= 1;
            assert!(!s.admits(&bad), "byte {i} flipped");
        }
        assert!(!s.admits(&good[..good.len() - 1]));
        assert!(!s.admits(&[good.as_slice(), &[0]].concat()));
    }
}
