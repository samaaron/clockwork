// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Why a call was refused: `ClockworkStatus`, minus `CLOCKWORK_OK`.

use core::ffi::CStr;
use core::fmt;

use clockwork_abi::client::{clockwork_client_status_text, ClockworkStatus};

/// A refusal. Every call that can fail says why, to the thread that called
/// it — there is no errno and no last-error on the handle.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Error {
    /// A null pointer, a zero length, a bad enum, a struct too small.
    Arg,
    /// The engine's layout is not one this build reads.
    Version,
    /// No engine at that address or endpoint.
    NotFound,
    /// Found, but not ours to open — or the process already has an engine.
    Perm,
    /// The engine has no such region or slot. An answer, not a fault: a
    /// client greys the control rather than drawing zeros.
    Absent,
    /// The ingress ring had no room. A moment, not a verdict: it drains
    /// every block, so the caller retries ([`Error::is_retry`]).
    Full,
    /// The message cannot fit in the ring at any moment; bulk belongs in
    /// the inbox instead.
    TooBig,
    /// The engine went away under us.
    Closed,
    /// Out of memory.
    NoMem,
    /// A status this build of the crate does not name. Receiving one is not
    /// undefined behaviour, which is why the ABI's status is a newtype and
    /// this variant exists.
    Unknown(i32),
}

/// The crate's `Result`.
pub type Result<T> = core::result::Result<T, Error>;

impl Error {
    /// The error a status is, or `None` for `CLOCKWORK_OK`.
    pub fn from_status(s: ClockworkStatus) -> Option<Error> {
        Some(match s {
            ClockworkStatus::OK => return None,
            ClockworkStatus::E_ARG => Error::Arg,
            ClockworkStatus::E_VERSION => Error::Version,
            ClockworkStatus::E_NOT_FOUND => Error::NotFound,
            ClockworkStatus::E_PERM => Error::Perm,
            ClockworkStatus::E_ABSENT => Error::Absent,
            ClockworkStatus::E_FULL => Error::Full,
            ClockworkStatus::E_TOO_BIG => Error::TooBig,
            ClockworkStatus::E_CLOSED => Error::Closed,
            ClockworkStatus::E_NOMEM => Error::NoMem,
            other => Error::Unknown(other.0),
        })
    }

    /// The status this error is, as the C ABI spells it.
    pub fn status(self) -> ClockworkStatus {
        match self {
            Error::Arg => ClockworkStatus::E_ARG,
            Error::Version => ClockworkStatus::E_VERSION,
            Error::NotFound => ClockworkStatus::E_NOT_FOUND,
            Error::Perm => ClockworkStatus::E_PERM,
            Error::Absent => ClockworkStatus::E_ABSENT,
            Error::Full => ClockworkStatus::E_FULL,
            Error::TooBig => ClockworkStatus::E_TOO_BIG,
            Error::Closed => ClockworkStatus::E_CLOSED,
            Error::NoMem => ClockworkStatus::E_NOMEM,
            Error::Unknown(v) => ClockworkStatus(v),
        }
    }

    /// True for the one refusal that is about timing rather than the
    /// request: a full ring, which a caller sends into again next block.
    pub fn is_retry(self) -> bool {
        matches!(self, Error::Full)
    }

    /// The library's own phrase for this status.
    pub fn text(self) -> &'static str {
        // SAFETY: the phrase is static storage, never null, never allocated
        // (clockwork_client.h), and every status value has one — an unknown
        // value gets "unknown status" rather than a null.
        let p = unsafe { clockwork_client_status_text(self.status()) };
        // SAFETY: NUL-terminated static storage, as above.
        unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("unknown status")
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.text())
    }
}

impl std::error::Error for Error {}

/// `Ok(())` for `CLOCKWORK_OK`, the error otherwise.
pub(crate) fn check(s: ClockworkStatus) -> Result<()> {
    match Error::from_status(s) {
        None => Ok(()),
        Some(e) => Err(e),
    }
}

/// The error a null handle came with: the status the open wrote, or — if
/// it wrote OK beside a null, which the headers say it never does —
/// `Closed`, the nearest true thing.
pub(crate) fn refused(s: ClockworkStatus) -> Error {
    Error::from_status(s).unwrap_or(Error::Closed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_status_round_trips_and_ok_is_not_an_error() {
        assert_eq!(Error::from_status(ClockworkStatus::OK), None);
        for e in [
            Error::Arg,
            Error::Version,
            Error::NotFound,
            Error::Perm,
            Error::Absent,
            Error::Full,
            Error::TooBig,
            Error::Closed,
            Error::NoMem,
            Error::Unknown(99),
        ] {
            assert_eq!(Error::from_status(e.status()), Some(e));
        }
        assert!(check(ClockworkStatus::OK).is_ok());
        assert_eq!(check(ClockworkStatus::E_FULL), Err(Error::Full));
        assert!(Error::Full.is_retry());
        assert!(!Error::TooBig.is_retry());
    }

    #[test]
    fn the_phrase_is_the_library_s_own() {
        // The words are the C library's, not a copy kept here: the test
        // pins two so a renamed status is noticed, and the unknown one so
        // an out-of-range value is a phrase rather than a crash.
        assert_eq!(Error::Full.to_string(), "ingress ring full; retry");
        assert_eq!(Error::Absent.to_string(), "this engine has no such region");
        assert_eq!(Error::Unknown(99).to_string(), "unknown status");
        assert_eq!(refused(ClockworkStatus::E_PERM), Error::Perm);
        assert_eq!(refused(ClockworkStatus::OK), Error::Closed);
    }
}
