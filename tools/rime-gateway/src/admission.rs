// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Who gets a session, and who is told to wait — Track H's admission control.
//!
//! This is the only "placement" decision the gateway has to make, and it is small on purpose.
//! [ADR-0047](../../../docs/adr/0047-two-machines-and-the-starbase-tier.md) settled that hosting
//! happens on **one** machine: the workstation never serves a hosted session, so there is no host
//! discovery, no liveness protocol and no cross-host scheduling. What remains is "can this host take
//! another session right now?".
//!
//! **Why a cap at all, and why it is not a guess.** `docs/design/hosted-rime.md` concluded that GPU
//! time runs out before VRAM but VRAM is what *kills* the box — one tenant's asset load evicting
//! everyone — and that Vulkan offers no per-process quota, so the limit has to be enforced outside
//! the GPU. That analysis assumed 12 GB. The machine that actually hosts has **6 GB** (measured,
//! `docs/perf/m18.3d-starbase/`), so the ceiling is tighter than the design doc's arithmetic and the
//! number belongs in configuration rather than in this file.
//!
//! **Admission is deliberately separate from spawning.** It knows nothing about processes, sockets or
//! the supervisor: it decides *whether*, and the caller then brings a session up and attaches it.
//! Keeping it pure is what lets the policy be tested without a GPU, an engine or a network — and it
//! stops "are we full?" from being answerable only by trying.

use std::collections::HashMap;
use std::fmt;

use crate::identity::Principal;
use crate::Surface;

/// An opaque session handle. **Random, never sequential**, because a session id is a capability: it
/// is what a client presents to reach its own session, and a counter would let anyone enumerate
/// everybody else's by subtracting one.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SessionId(u128);

impl SessionId {
    /// Draw a fresh id from the OS entropy pool.
    ///
    /// Reads `/dev/urandom` rather than taking a dependency: this crate is otherwise
    /// dependency-free, and the whole requirement is "unguessable", which the kernel already
    /// provides. A short read is treated as a hard failure rather than padded — silently degrading
    /// the entropy of a capability is worse than refusing to make one.
    pub fn generate() -> std::io::Result<Self> {
        use std::io::Read;
        let mut bytes = [0u8; 16];
        std::fs::File::open("/dev/urandom")?.read_exact(&mut bytes)?;
        Ok(Self(u128::from_be_bytes(bytes)))
    }
}

impl fmt::Display for SessionId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:032x}", self.0)
    }
}

impl std::str::FromStr for SessionId {
    type Err = MalformedSessionId;

    /// Parse the 32-hex-digit form [`fmt::Display`] writes.
    ///
    /// **Exactly 32 digits, nothing shorter.** `u128::from_str_radix` would happily accept `"1"`, and
    /// then a client could address a session by its numeric value with the leading zeros stripped —
    /// which is not an attack by itself, but it means an id has more than one spelling, and an id with
    /// more than one spelling is one a rate limiter or an audit log cannot count.
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        if text.len() != 32 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(MalformedSessionId);
        }
        u128::from_str_radix(text, 16)
            .map(Self)
            .map_err(|_| MalformedSessionId)
    }
}

/// The only thing that can go wrong parsing a [`SessionId`]: it was not 32 hex digits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MalformedSessionId;

impl fmt::Display for MalformedSessionId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "a session id is 32 hexadecimal digits")
    }
}

impl std::error::Error for MalformedSessionId {}

/// What this host is willing to run at once.
#[derive(Debug, Clone, Copy)]
pub struct AdmissionPolicy {
    /// Sessions of any surface, together. The binding constraint on a 6 GB GPU.
    pub max_sessions: usize,
    /// Play sessions specifically, which own a full-rate render and an encoder. Separated from the
    /// total because an editor session is much cheaper — `message_affects_frame` means an idle
    /// editor renders nothing at all — so a host can carry several editors and only one game.
    pub max_play_sessions: usize,
    /// Sessions **one account** may hold at once (ADR-0048 brick 3). The global cap alone is not a
    /// fair-share rule: with three slots and three invited people, one account opening three sessions
    /// is indistinguishable from a full host to the other two, and nothing in the protocol tells them
    /// apart. An anonymous host (`AccessPolicy::require_account` off) has one principal by
    /// construction, so this cap would shut it down entirely — it is therefore only applied to
    /// requests that carry an account.
    pub max_sessions_per_account: usize,
}

impl Default for AdmissionPolicy {
    /// The starbase tier, as measured rather than hoped: one play session and a couple of editors.
    /// `hosted-rime.md` found the block "barely holding 60 Hz and already missing at p95" on a 3060
    /// *before* capture and encode, and the hosting GPU is 1.9x slower than that (measured,
    /// `docs/perf/m18.3d-starbase/`). A second concurrent play session is a thing to demonstrate,
    /// not to assume.
    fn default() -> Self {
        Self {
            max_sessions: 3,
            max_play_sessions: 1,
            // One editor and one play session at a time: enough to author something and then try it
            // without closing the editor, and not enough to fill the host alone.
            max_sessions_per_account: 2,
        }
    }
}

/// Why a session was not admitted. Distinct variants because they mean different things to a client:
/// a full host is worth queueing for, and an entropy failure is not the client's problem at all.
#[derive(Debug)]
pub enum Refusal {
    /// The host is at `max_sessions`.
    HostFull { limit: usize },
    /// The host will take another session, but not another *play* session.
    PlayFull { limit: usize },
    /// This account already holds as many sessions as it may. Distinct from `HostFull` because it is
    /// actionable by the client — closing one of its own sessions fixes it — and because telling a
    /// user the host is full when it is not would be a lie the counters would later contradict.
    AccountFull { limit: usize },
    /// The id could not be generated. Not the client's fault and not retryable by it.
    NoEntropy(std::io::Error),
}

impl fmt::Display for Refusal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Refusal::HostFull { limit } => write!(f, "host is full ({limit} sessions)"),
            Refusal::PlayFull { limit } => write!(f, "host is full for play ({limit})"),
            Refusal::AccountFull { limit } => {
                write!(f, "this account already holds {limit} sessions")
            }
            Refusal::NoEntropy(e) => write!(f, "could not generate a session id: {e}"),
        }
    }
}

impl std::error::Error for Refusal {}

/// Every decision the registry made, so a host that is refusing everything can say why.
///
/// The counter rule, applied to admission: a gateway that silently refuses is indistinguishable from
/// one that is broken, and "nobody can start a session" is exactly the report that arrives with no
/// other information attached.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct AdmissionCounters {
    pub admitted: u64,
    pub refused_host_full: u64,
    pub refused_play_full: u64,
    pub refused_no_entropy: u64,
    /// Refusals because the requesting account already held its maximum.
    pub refused_account_full: u64,
    pub released: u64,
}

/// The admitted set. `T` is whatever the caller wants to hang off a session — the supervisor's
/// handle, in practice — which this module deliberately knows nothing about.
#[derive(Debug)]
pub struct Registry<T> {
    policy: AdmissionPolicy,
    sessions: HashMap<SessionId, Slot<T>>,
    counters: AdmissionCounters,
}

/// One admitted session: what it is for, **who it belongs to**, and whatever the caller attached.
///
/// The owner is recorded at admission rather than at attach, for the same reason the slot is taken at
/// admission: a fact established later than the thing it governs is a fact something can happen
/// before.
#[derive(Debug)]
struct Slot<T> {
    surface: Surface,
    owner: Principal,
    value: Option<T>,
}

impl<T> Registry<T> {
    #[must_use]
    pub fn new(policy: AdmissionPolicy) -> Self {
        Self {
            policy,
            sessions: HashMap::new(),
            counters: AdmissionCounters::default(),
        }
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.sessions.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.sessions.is_empty()
    }

    #[must_use]
    pub fn play_count(&self) -> usize {
        self.sessions
            .values()
            .filter(|slot| slot.surface == Surface::Play)
            .count()
    }

    /// How many sessions a principal holds.
    #[must_use]
    pub fn count_for(&self, owner: Principal) -> usize {
        self.sessions
            .values()
            .filter(|slot| slot.owner == owner)
            .count()
    }

    /// Who an admitted session belongs to, if it exists.
    #[must_use]
    pub fn owner_of(&self, id: SessionId) -> Option<Principal> {
        self.sessions.get(&id).map(|slot| slot.owner)
    }

    #[must_use]
    pub fn counters(&self) -> AdmissionCounters {
        self.counters
    }

    /// Reserve a slot for `surface`, or say why not.
    ///
    /// The slot is taken **before** the caller spawns anything, so two concurrent requests cannot
    /// both pass the check and then both spawn — the classic way a cap becomes advisory.
    pub fn try_admit(&mut self, surface: Surface, owner: Principal) -> Result<SessionId, Refusal> {
        if self.sessions.len() >= self.policy.max_sessions {
            self.counters.refused_host_full += 1;
            return Err(Refusal::HostFull {
                limit: self.policy.max_sessions,
            });
        }
        if surface == Surface::Play && self.play_count() >= self.policy.max_play_sessions {
            self.counters.refused_play_full += 1;
            return Err(Refusal::PlayFull {
                limit: self.policy.max_play_sessions,
            });
        }
        // Only for a named account: anonymous requests are all one principal, so a per-account cap
        // applied to them would cap the whole host at `max_sessions_per_account`.
        if owner.is_account() && self.count_for(owner) >= self.policy.max_sessions_per_account {
            self.counters.refused_account_full += 1;
            return Err(Refusal::AccountFull {
                limit: self.policy.max_sessions_per_account,
            });
        }
        let id = match SessionId::generate() {
            Ok(id) => id,
            Err(e) => {
                self.counters.refused_no_entropy += 1;
                return Err(Refusal::NoEntropy(e));
            }
        };
        self.sessions.insert(
            id,
            Slot {
                surface,
                owner,
                value: None,
            },
        );
        self.counters.admitted += 1;
        Ok(id)
    }

    /// Attach the live session to an admitted slot. Returns false for an id that was never admitted
    /// or has already been released — which is what a stale client request looks like.
    pub fn attach(&mut self, id: SessionId, value: T) -> bool {
        match self.sessions.get_mut(&id) {
            Some(slot) => {
                slot.value = Some(value);
                true
            }
            None => false,
        }
    }

    #[must_use]
    pub fn surface_of(&self, id: SessionId) -> Option<Surface> {
        self.sessions.get(&id).map(|slot| slot.surface)
    }

    pub fn get_mut(&mut self, id: SessionId) -> Option<&mut T> {
        self.sessions
            .get_mut(&id)
            .and_then(|slot| slot.value.as_mut())
    }

    /// Every admitted session and its surface, for a listing. Yields the surface rather than `&T` so
    /// a caller that only wants to *report* the set does not have to be able to touch the live
    /// sessions — the API's `GET /api/sessions` is exactly that caller.
    pub fn iter(&self) -> impl Iterator<Item = (SessionId, Surface, Principal)> + '_ {
        self.sessions
            .iter()
            .map(|(id, slot)| (*id, slot.surface, slot.owner))
    }

    /// Every admitted id, so a caller can walk the attached values one at a time with
    /// [`Registry::get_mut`].
    ///
    /// A `Vec` rather than an iterator on purpose: the caller that needs this is the API's sweep of
    /// relays that stopped on their own, and it has to *mutate* the registry — release the dead ones —
    /// while walking it, which a borrow of the map would forbid.
    #[must_use]
    pub fn ids(&self) -> Vec<SessionId> {
        self.sessions.keys().copied().collect()
    }

    /// Free the slot and hand back whatever was attached, so the caller can shut it down. Dropping
    /// the returned value is what reaps the process, if `T` is the supervisor's handle.
    pub fn release(&mut self, id: SessionId) -> Option<Option<T>> {
        let removed = self.sessions.remove(&id);
        if removed.is_some() {
            self.counters.released += 1;
        }
        removed.map(|slot| slot.value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn policy(total: usize, play: usize) -> AdmissionPolicy {
        AdmissionPolicy {
            max_sessions: total,
            max_play_sessions: play,
            max_sessions_per_account: usize::MAX,
        }
    }

    #[test]
    fn session_ids_are_random_and_not_sequential() {
        // A session id is a capability. If it were a counter, holding one would mean being able to
        // guess every other tenant's by arithmetic — so this asserts unpredictability, not
        // uniqueness, which a counter would also satisfy.
        let ids: Vec<SessionId> = (0..64).map(|_| SessionId::generate().unwrap()).collect();
        let unique: std::collections::HashSet<_> = ids.iter().collect();
        assert_eq!(unique.len(), ids.len(), "ids must not repeat");
        // No two consecutive draws differ by a small amount, which is what a counter (or a poorly
        // seeded generator) would show.
        for pair in ids.windows(2) {
            let a = format!("{}", pair[0]);
            let b = format!("{}", pair[1]);
            assert_ne!(a, b);
            let delta = pair[0].0.abs_diff(pair[1].0);
            assert!(delta > 1_000_000, "ids look sequential: {a} then {b}");
        }
        // 32 hex characters, so 128 bits actually reach the wire format.
        assert_eq!(format!("{}", ids[0]).len(), 32);
    }

    #[test]
    fn the_total_cap_refuses_and_counts() {
        let mut reg: Registry<()> = Registry::new(policy(2, 2));
        assert!(reg.try_admit(Surface::Edit, Principal::Anonymous).is_ok());
        assert!(reg.try_admit(Surface::Edit, Principal::Anonymous).is_ok());
        let err = reg
            .try_admit(Surface::Edit, Principal::Anonymous)
            .expect_err("host is full");
        assert!(matches!(err, Refusal::HostFull { limit: 2 }), "{err:?}");
        assert_eq!(reg.counters().admitted, 2);
        assert_eq!(reg.counters().refused_host_full, 1);
        assert_eq!(reg.len(), 2);
    }

    #[test]
    fn the_play_cap_is_separate_from_the_total() {
        // An editor session is much cheaper than a play session (an idle editor renders nothing at
        // all), so a host that is full for play may still take an editor. A single cap could not
        // express that, which is the reason there are two.
        let mut reg: Registry<()> = Registry::new(policy(3, 1));
        assert!(reg.try_admit(Surface::Play, Principal::Anonymous).is_ok());
        let err = reg
            .try_admit(Surface::Play, Principal::Anonymous)
            .expect_err("play is full");
        assert!(matches!(err, Refusal::PlayFull { limit: 1 }), "{err:?}");
        assert!(
            reg.try_admit(Surface::Edit, Principal::Anonymous).is_ok(),
            "an editor must still fit"
        );
        assert_eq!(reg.counters().refused_play_full, 1);
        assert_eq!(reg.play_count(), 1);
    }

    #[test]
    fn releasing_a_play_session_frees_its_slot() {
        let mut reg: Registry<&str> = Registry::new(policy(3, 1));
        let id = reg.try_admit(Surface::Play, Principal::Anonymous).unwrap();
        assert!(reg.attach(id, "engine"));
        assert!(reg.try_admit(Surface::Play, Principal::Anonymous).is_err());
        assert_eq!(reg.release(id), Some(Some("engine")));
        assert_eq!(reg.counters().released, 1);
        assert!(
            reg.try_admit(Surface::Play, Principal::Anonymous).is_ok(),
            "the slot must come back, or the host leaks capacity until restart"
        );
    }

    #[test]
    fn the_slot_is_taken_at_admission_not_at_attach() {
        // If the cap were only enforced once a session was attached, two requests could both pass
        // the check and both spawn — the classic way a limit becomes advisory. Admitting without
        // ever attaching must still consume capacity.
        let mut reg: Registry<()> = Registry::new(policy(1, 1));
        let _id = reg.try_admit(Surface::Edit, Principal::Anonymous).unwrap();
        assert!(reg.try_admit(Surface::Edit, Principal::Anonymous).is_err());
        assert_eq!(reg.len(), 1);
    }

    #[test]
    fn an_unknown_id_cannot_be_attached_or_released() {
        let mut reg: Registry<()> = Registry::new(policy(2, 1));
        let stale = SessionId::generate().unwrap();
        assert!(
            !reg.attach(stale, ()),
            "a never-admitted id must not attach"
        );
        assert!(reg.release(stale).is_none());
        assert!(reg.surface_of(stale).is_none());
        assert_eq!(reg.counters().released, 0);
    }

    #[test]
    fn the_default_policy_is_the_measured_starbase_tier() {
        // Not a taste: one play session, because the block already misses p95 on a GPU 1.9x faster
        // than the hosting one. A second concurrent play session is to be demonstrated, not assumed.
        let p = AdmissionPolicy::default();
        assert_eq!(p.max_play_sessions, 1);
        assert!(p.max_sessions >= p.max_play_sessions);
    }
}
