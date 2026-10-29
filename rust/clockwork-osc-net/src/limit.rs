// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The cue port's rate limit.
//!
//! The cue port takes datagrams from anyone who can reach it and needs no
//! secret, so it is the one way in that a stranger can flood. Two token
//! buckets bound what gets through: one for the port as a whole and one per
//! sender address. Each holds one second's worth of cues at its rate and
//! refills continuously; a cue with no token in either bucket is dropped,
//! and the server counts the drop. A rate of 0 means no limit on that side.
//!
//! The per-sender table holds [`SENDERS`] addresses. A new sender takes the
//! slot of the one seen longest ago, so a flood from spoofed addresses
//! cannot grow it, and an evicted sender that comes back starts with a full
//! bucket — the overall bucket is what holds that flood.
//!
//! Time is passed in, so the tests drive it.

use std::net::IpAddr;
use std::time::Instant;

/// How many sender addresses the limiter tracks at once.
pub const SENDERS: usize = 64;

/// Tokens are held in millionths, so a refill of `rate` per second is exact
/// per microsecond.
const SCALE: u64 = 1_000_000;

struct Bucket {
    tokens: u64,
    filled: Instant,
}

impl Bucket {
    fn full(rate: u32, now: Instant) -> Self {
        Bucket { tokens: u64::from(rate).saturating_mul(SCALE), filled: now }
    }

    fn refill(&mut self, rate: u32, now: Instant) {
        let cap = u64::from(rate).saturating_mul(SCALE);
        let elapsed_us = u64::try_from(now.saturating_duration_since(self.filled).as_micros()).unwrap_or(u64::MAX);
        let earned = elapsed_us.saturating_mul(u64::from(rate));
        self.tokens = self.tokens.saturating_add(earned).min(cap);
        self.filled = now;
    }

    fn available(&self) -> u32 {
        u32::try_from(self.tokens / SCALE).unwrap_or(u32::MAX)
    }

    fn take(&mut self, n: u32) {
        self.tokens -= u64::from(n) * SCALE;
    }
}

struct Sender {
    ip: IpAddr,
    bucket: Bucket,
    seen: Instant,
}

pub struct Limiter {
    per_second: u32,
    per_sender: u32,
    overall: Bucket,
    senders: Vec<Sender>,
}

impl Limiter {
    /// `per_second` bounds the port, `per_sender` each address; 0 is no bound.
    pub fn new(per_second: u32, per_sender: u32, now: Instant) -> Self {
        Limiter {
            per_second,
            per_sender,
            overall: Bucket::full(per_second, now),
            senders: Vec::with_capacity(SENDERS),
        }
    }

    /// Change the rates. Every bucket starts full again.
    pub fn set(&mut self, per_second: u32, per_sender: u32, now: Instant) {
        *self = Limiter::new(per_second, per_sender, now);
    }

    pub fn rates(&self) -> (u32, u32) {
        (self.per_second, self.per_sender)
    }

    /// Take up to `want` cues from `from` at `now`: how many it grants.
    pub fn take(&mut self, from: IpAddr, now: Instant, want: u32) -> u32 {
        let mut granted = want;
        if self.per_second > 0 {
            self.overall.refill(self.per_second, now);
            granted = granted.min(self.overall.available());
        }
        let slot = if self.per_sender > 0 {
            let i = self.slot_for(from, now);
            let s = &mut self.senders[i];
            s.seen = now;
            s.bucket.refill(self.per_sender, now);
            granted = granted.min(s.bucket.available());
            Some(i)
        } else {
            None
        };
        if self.per_second > 0 {
            self.overall.take(granted);
        }
        if let Some(i) = slot {
            self.senders[i].bucket.take(granted);
        }
        granted
    }

    // The sender's slot: its own, a free one, or the one seen longest ago.
    fn slot_for(&mut self, ip: IpAddr, now: Instant) -> usize {
        if let Some(i) = self.senders.iter().position(|s| s.ip == ip) {
            return i;
        }
        let fresh = Sender { ip, bucket: Bucket::full(self.per_sender, now), seen: now };
        if self.senders.len() < SENDERS {
            self.senders.push(fresh);
            return self.senders.len() - 1;
        }
        let i = self
            .senders
            .iter()
            .enumerate()
            .min_by_key(|(_, s)| s.seen)
            .map_or(0, |(i, _)| i);
        self.senders[i] = fresh;
        i
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::Ipv4Addr;
    use std::time::Duration;

    fn ip(n: u8) -> IpAddr {
        IpAddr::V4(Ipv4Addr::new(10, 0, 0, n))
    }

    #[test]
    fn allows_its_rate_in_a_burst_and_refuses_the_next() {
        let t0 = Instant::now();
        let mut l = Limiter::new(10, 0, t0);
        for _ in 0..10 {
            assert_eq!(l.take(ip(1), t0, 1), 1);
        }
        assert_eq!(l.take(ip(1), t0, 1), 0);
    }

    #[test]
    fn refills_at_its_rate_and_holds_no_more_than_a_second() {
        let t0 = Instant::now();
        let mut l = Limiter::new(10, 0, t0);
        assert_eq!(l.take(ip(1), t0, 10), 10);
        assert_eq!(l.take(ip(1), t0 + Duration::from_millis(99), 1), 0);
        assert_eq!(l.take(ip(1), t0 + Duration::from_millis(100), 1), 1);
        // A long quiet spell earns one second's worth, not more.
        assert_eq!(l.take(ip(1), t0 + Duration::from_secs(60), 100), 10);
    }

    #[test]
    fn senders_are_bounded_one_by_one() {
        let t0 = Instant::now();
        let mut l = Limiter::new(0, 5, t0);
        assert_eq!(l.take(ip(1), t0, 5), 5);
        assert_eq!(l.take(ip(2), t0, 5), 5);
        assert_eq!(l.take(ip(1), t0, 1), 0);
        assert_eq!(l.take(ip(2), t0, 1), 0);
        assert_eq!(l.take(ip(3), t0, 1), 1);
    }

    #[test]
    fn the_port_bounds_every_sender_together() {
        let t0 = Instant::now();
        let mut l = Limiter::new(8, 5, t0);
        assert_eq!(l.take(ip(1), t0, 5), 5);
        assert_eq!(l.take(ip(2), t0, 5), 3);
        assert_eq!(l.take(ip(3), t0, 1), 0);
    }

    #[test]
    fn a_bundle_gets_what_is_left() {
        let t0 = Instant::now();
        let mut l = Limiter::new(0, 5, t0);
        assert_eq!(l.take(ip(1), t0, 3), 3);
        assert_eq!(l.take(ip(1), t0, 7), 2);
        assert_eq!(l.take(ip(1), t0, 7), 0);
    }

    #[test]
    fn the_sender_table_is_bounded_and_evicts_the_stalest() {
        let t0 = Instant::now();
        let mut l = Limiter::new(0, 1, t0);
        for n in 0..SENDERS {
            let ip = IpAddr::V4(Ipv4Addr::new(10, 1, (n / 256) as u8, (n % 256) as u8));
            assert_eq!(l.take(ip, t0 + Duration::from_micros(n as u64), 1), 1);
        }
        assert_eq!(l.senders.len(), SENDERS);
        // Sender 0 was seen first; the newcomer takes its slot.
        let later = t0 + Duration::from_millis(1);
        assert_eq!(l.take(ip(200), later, 1), 1);
        assert_eq!(l.senders.len(), SENDERS);
        assert!(l.senders.iter().all(|s| s.ip != IpAddr::V4(Ipv4Addr::new(10, 1, 0, 0))));
        // Sender 1 kept its slot and its empty bucket.
        assert_eq!(l.take(IpAddr::V4(Ipv4Addr::new(10, 1, 0, 1)), later, 1), 0);
    }

    #[test]
    fn zero_is_no_limit() {
        let t0 = Instant::now();
        let mut l = Limiter::new(0, 0, t0);
        assert_eq!(l.take(ip(1), t0, 1_000_000), 1_000_000);
        assert_eq!(l.take(ip(1), t0, 1_000_000), 1_000_000);
        assert!(l.senders.is_empty());
    }

    #[test]
    fn set_starts_the_buckets_full_at_the_new_rates() {
        let t0 = Instant::now();
        let mut l = Limiter::new(2, 0, t0);
        assert_eq!(l.take(ip(1), t0, 2), 2);
        l.set(4, 3, t0);
        assert_eq!(l.rates(), (4, 3));
        assert_eq!(l.take(ip(1), t0, 4), 3);
        assert_eq!(l.take(ip(2), t0, 4), 1);
    }
}
