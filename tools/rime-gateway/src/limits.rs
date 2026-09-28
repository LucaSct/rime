// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Rate limiting for the surface anybody can reach.
//!
//! The session API is behind `AccessPolicy::require_account`, and everything past that is bounded by
//! admission caps. The **account** endpoints are not behind anything — they are how you get an account
//! in the first place — so they are where a stranger can spend this host's CPU, its invitations'
//! guessing budget and its mail reputation. This is the bound on that.
//!
//! ## A token bucket, in integers
//!
//! A bucket holds at most `burst` tokens and refills `burst` of them over `window` seconds. A request
//! costs one. Fixed windows would be simpler and are the usual thing to reach for, but they let twice
//! the intended rate through across a boundary — `burst` at 0:59 and `burst` again at 1:01 — and the
//! whole point here is a number an operator can reason about.
//!
//! The arithmetic is integer and exact: refilling advances `last_refill` by exactly the time the
//! granted tokens cost, so the remainder is carried rather than discarded. Floats would drift, and a
//! limiter that drifts is one nobody can state a bound for.
//!
//! ## The map is bounded, because the keys come from strangers
//!
//! Keying by client address means the key space is chosen by whoever is calling, so an unbounded map
//! is the memory-exhaustion primitive the limiter was supposed to prevent. `MAX_KEYS` caps it; full
//! buckets (the ones at rest) are dropped first, and if every bucket is in use the limiter **denies**
//! rather than growing. Denying under pressure is the failure a rate limiter is allowed to have.
//!
//! ## What is deliberately not limited here
//!
//! **Per-email-address limits.** They read as the obvious next line — cap how often one address can be
//! mailed — and they hand an attacker two things. A `429` keyed on an address is an oracle for whether
//! that address has an account, and an address is something anyone can name, so the limit becomes a
//! way to lock a specific person out of their own recovery. What bounds mail here instead is the
//! per-client limit plus `rime-auth`'s own five-attempts-per-code, which are keyed on things the
//! attacker has to spend rather than on things they can merely assert.

use std::collections::HashMap;

/// How many requests, over how long. `burst` is both the bucket's capacity and its refill per window,
/// so the sustained rate is `burst / window` per second and a client may spend `burst` at once.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rate {
    pub burst: u32,
    pub window_seconds: u64,
}

impl Rate {
    #[must_use]
    pub const fn new(burst: u32, window_seconds: u64) -> Self {
        Self {
            burst,
            window_seconds,
        }
    }
}

/// How many distinct keys a limiter will track. Small on purpose: this host runs three sessions
/// (ADR-0047 §3), so a thousand distinct callers in one window is already an event rather than traffic.
pub const MAX_KEYS: usize = 1024;

/// What the limiter decided, and when it is worth coming back.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    Allow,
    /// Denied; `retry_after` is whole seconds until one token exists, always at least 1 so a client
    /// that honours it does not immediately try again.
    Deny {
        retry_after: u64,
    },
}

#[derive(Debug, Clone, Copy)]
struct Bucket {
    tokens: u32,
    /// The instant the current token count was true as of. Advanced by exactly the cost of the tokens
    /// granted, never set to `now`, so a fractional token is carried instead of lost.
    last_refill: u64,
}

/// One rate, applied per key.
#[derive(Debug)]
pub struct Limiter {
    rate: Rate,
    buckets: HashMap<String, Bucket>,
    denied: u64,
    evicted: u64,
}

impl Limiter {
    #[must_use]
    pub fn new(rate: Rate) -> Self {
        Self {
            rate,
            buckets: HashMap::new(),
            denied: 0,
            evicted: 0,
        }
    }

    /// Requests refused, for the operator's counters. A climb here is the shape of an attack; the
    /// client is told nothing beyond `429`.
    #[must_use]
    pub fn denied(&self) -> u64 {
        self.denied
    }

    /// Buckets dropped to stay under [`MAX_KEYS`].
    #[must_use]
    pub fn evicted(&self) -> u64 {
        self.evicted
    }

    #[must_use]
    pub fn tracked(&self) -> usize {
        self.buckets.len()
    }

    /// Spend one token for `key`, or refuse.
    pub fn check(&mut self, key: &str, now: u64) -> Decision {
        // Degenerate configurations are a refusal to serve rather than a division by zero: a rate of
        // zero burst means "nothing may pass", and it should mean that rather than panic.
        if self.rate.burst == 0 || self.rate.window_seconds == 0 {
            self.denied += 1;
            return Decision::Deny { retry_after: 1 };
        }
        self.sweep(now);
        if !self.buckets.contains_key(key) && self.buckets.len() >= MAX_KEYS {
            // Nothing evictable and no room: refuse rather than grow. See the module note.
            self.denied += 1;
            return Decision::Deny {
                retry_after: self.rate.window_seconds,
            };
        }
        let rate = self.rate;
        let bucket = self.buckets.entry(key.to_string()).or_insert(Bucket {
            tokens: rate.burst,
            last_refill: now,
        });
        refill(bucket, rate, now);
        if bucket.tokens > 0 {
            bucket.tokens -= 1;
            Decision::Allow
        } else {
            self.denied += 1;
            // Time until the next whole token, rounded up and never zero.
            let per_token = seconds_per_token(rate);
            let waited = now.saturating_sub(bucket.last_refill);
            Decision::Deny {
                retry_after: per_token.saturating_sub(waited).max(1),
            }
        }
    }

    /// Drop buckets that are back at full — they are indistinguishable from absent ones, so keeping
    /// them is pure memory. Only runs when the map is close to its cap, because walking it on every
    /// request would make the limiter cost more than what it is protecting.
    fn sweep(&mut self, now: u64) {
        if self.buckets.len() < MAX_KEYS {
            return;
        }
        let rate = self.rate;
        let before = self.buckets.len();
        self.buckets.retain(|_, bucket| {
            let mut probe = *bucket;
            refill(&mut probe, rate, now);
            probe.tokens < rate.burst
        });
        self.evicted += (before - self.buckets.len()) as u64;
    }
}

/// Seconds one token costs, rounded up: with `burst` tokens per `window`, a token is worth
/// `window / burst` seconds, and rounding up keeps the limiter at or under its stated rate rather
/// than slightly over it.
fn seconds_per_token(rate: Rate) -> u64 {
    rate.window_seconds.div_ceil(u64::from(rate.burst)).max(1)
}

fn refill(bucket: &mut Bucket, rate: Rate, now: u64) {
    if now <= bucket.last_refill {
        // A clock that went backwards must not mint tokens. Treat it as no time having passed.
        return;
    }
    let per_token = seconds_per_token(rate);
    let elapsed = now - bucket.last_refill;
    let granted = elapsed / per_token;
    if granted == 0 {
        return;
    }
    let granted_u32 = u32::try_from(granted).unwrap_or(u32::MAX);
    bucket.tokens = bucket.tokens.saturating_add(granted_u32).min(rate.burst);
    // Advance by what was actually granted, not to `now`: the remainder is the fraction of a token
    // already earned, and discarding it would make the real rate lower than the stated one.
    bucket.last_refill = bucket
        .last_refill
        .saturating_add(granted.saturating_mul(per_token));
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: u64 = 1_000_000;

    #[test]
    fn a_burst_is_allowed_and_then_it_is_not() {
        let mut limiter = Limiter::new(Rate::new(3, 60));
        for _ in 0..3 {
            assert_eq!(limiter.check("a", NOW), Decision::Allow);
        }
        let denied = limiter.check("a", NOW);
        assert!(matches!(denied, Decision::Deny { retry_after } if retry_after >= 1));
        assert_eq!(limiter.denied(), 1);
    }

    #[test]
    fn keys_do_not_share_a_budget() {
        let mut limiter = Limiter::new(Rate::new(1, 60));
        assert_eq!(limiter.check("a", NOW), Decision::Allow);
        assert!(matches!(limiter.check("a", NOW), Decision::Deny { .. }));
        // A second caller is unaffected by the first having spent its own.
        assert_eq!(limiter.check("b", NOW), Decision::Allow);
    }

    #[test]
    fn tokens_come_back_at_the_stated_rate_and_no_faster() {
        // Six per minute is one every ten seconds.
        let mut limiter = Limiter::new(Rate::new(6, 60));
        for _ in 0..6 {
            assert_eq!(limiter.check("a", NOW), Decision::Allow);
        }
        assert!(matches!(limiter.check("a", NOW), Decision::Deny { .. }));
        // Nine seconds is not yet a token.
        assert!(matches!(limiter.check("a", NOW + 9), Decision::Deny { .. }));
        assert_eq!(limiter.check("a", NOW + 10), Decision::Allow);
        assert!(matches!(
            limiter.check("a", NOW + 10),
            Decision::Deny { .. }
        ));
        // And a long silence refills to the burst, not beyond it.
        for _ in 0..6 {
            assert_eq!(limiter.check("a", NOW + 10_000), Decision::Allow);
        }
        assert!(matches!(
            limiter.check("a", NOW + 10_000),
            Decision::Deny { .. }
        ));
    }

    /// The remainder has to be CARRIED, not dropped. A token costs ten seconds here; asking at +19
    /// earns one and leaves nine seconds of the next already paid for, so the following token is due
    /// at +20 and not at +29. Setting `last_refill = now` on a refill — the obvious way to write it —
    /// silently turns a 6-per-minute limiter into a slower one that no stated rate describes.
    ///
    /// The earlier version of this test asked at +9 and +10 and passed either way, because a refill
    /// that grants nothing returns before touching the clock at all. It proved nothing, and it took
    /// breaking the code on purpose to find that out.
    #[test]
    fn a_partial_refill_is_carried_rather_than_discarded() {
        let mut limiter = Limiter::new(Rate::new(1, 10));
        assert_eq!(limiter.check("a", NOW), Decision::Allow);
        // Nineteen seconds: one whole token, and nine seconds of the next.
        assert_eq!(limiter.check("a", NOW + 19), Decision::Allow);
        // So the next one is due one second later, not ten.
        assert_eq!(limiter.check("a", NOW + 20), Decision::Allow);
        assert!(matches!(
            limiter.check("a", NOW + 20),
            Decision::Deny { .. }
        ));
    }

    #[test]
    fn a_clock_that_goes_backwards_does_not_mint_tokens() {
        let mut limiter = Limiter::new(Rate::new(1, 60));
        assert_eq!(limiter.check("a", NOW), Decision::Allow);
        assert!(matches!(
            limiter.check("a", NOW - 3_600),
            Decision::Deny { .. }
        ));
    }

    #[test]
    fn the_map_is_bounded_and_refuses_rather_than_growing() {
        let mut limiter = Limiter::new(Rate::new(1, 3_600));
        for i in 0..MAX_KEYS {
            assert_eq!(limiter.check(&format!("k{i}"), NOW), Decision::Allow);
        }
        assert_eq!(limiter.tracked(), MAX_KEYS);
        // Every bucket is spent, so none can be swept, and a new key is refused instead of admitted.
        assert!(matches!(
            limiter.check("one-more", NOW),
            Decision::Deny { .. }
        ));
        assert_eq!(limiter.tracked(), MAX_KEYS);
        // Once they have refilled they are indistinguishable from absent, so they are swept and the
        // new caller gets in.
        assert_eq!(limiter.check("one-more", NOW + 3_600), Decision::Allow);
        assert!(limiter.evicted() > 0);
    }

    #[test]
    fn a_zero_rate_refuses_everything_rather_than_dividing_by_zero() {
        let mut limiter = Limiter::new(Rate::new(0, 60));
        assert!(matches!(limiter.check("a", NOW), Decision::Deny { .. }));
        let mut limiter = Limiter::new(Rate::new(5, 0));
        assert!(matches!(limiter.check("a", NOW), Decision::Deny { .. }));
    }
}
