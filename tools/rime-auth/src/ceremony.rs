// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Passkey registration and login — brick 2 of [ADR-0048](../../docs/adr/0048-authenticating-the-hosted-front-end.md).
//!
//! Brick 1 (`lib.rs`) is the durable store: accounts, invitations, credentials, sessions. It holds a
//! credential's bytes without ever looking at them. This module is what turns those bytes into an
//! *authentication*: it runs the two WebAuthn ceremonies against `webauthn-rs`, which is the reason
//! this crate is behind a Cargo feature at all (MPL-2.0, ADR-0048 decision 2).
//!
//! ## Why the public API speaks JSON strings and not library types
//!
//! ADR-0048 decision 3 rules that no `webauthn-rs` type may enter admission, the engine or
//! `rime-protocol`. A ceremony's input is a blob the *browser* produced and its output is a blob the
//! browser consumes, so the honest seam is the wire format itself: the gateway forwards a request body
//! and writes back a response body, and never names a credential type. That also means the day this
//! crate is compiled out, nothing upstream has a hole in its type signatures.
//!
//! ## Why the state lives in memory and dies with the process
//!
//! A ceremony state is a challenge the server issued seconds ago and will accept exactly once. It has
//! no value after that, and a restart that forgets every in-flight ceremony costs a user one retry —
//! whereas a challenge that survives in a file is a replay window with a longer life than the thing it
//! guards. So: bounded map, single-use, five-minute expiry, gone on restart. This is deliberately the
//! opposite of the store's durability rule, for the opposite reason.
//!
//! ## The three bindings every ceremony carries
//!
//! 1. **The challenge id** is what the browser echoes back. It is 128 random bits and it is a
//!    capability, exactly like a session id (`rime-gateway`'s `SessionId`, ADR-0048 decision 4).
//! 2. **The browser binding** is a value the gateway already sets per browser. A finish that arrives
//!    from a different browser than the start is refused even if it carries the right challenge id, so
//!    a leaked id alone does not let a third party complete somebody's registration.
//! 3. **The account** is fixed at *start* from what the server already knows — a redeemed invitation
//!    or a looked-up email — and is never taken from the finish. A finish that could name its own
//!    account would let anyone register a passkey against anyone's account.

use std::collections::HashMap;
use std::fmt;
use std::time::Duration;

use webauthn_rs::prelude::{
    CredentialID, Passkey, PasskeyAuthentication, PasskeyRegistration, PublicKeyCredential,
    RegisterPublicKeyCredential, Url, Uuid, Webauthn, WebauthnBuilder,
};

use crate::{fill_random, to_hex, AccountId, Timestamp};

/// How long a started ceremony may be finished for. Five minutes is the WebAuthn client timeout's
/// order of magnitude: long enough for a user to find a security key or a fingerprint reader, short
/// enough that an abandoned challenge is not a standing target.
pub const CHALLENGE_TTL: Duration = Duration::from_secs(5 * 60);

/// How many ceremonies may be in flight at once. Unbounded server-side state that an anonymous
/// request can create is a memory-exhaustion primitive; three render slots (ADR-0047 §3) do not need
/// more than this, and the refusal is visible rather than a slow death.
pub const MAX_PENDING: usize = 64;

/// A ceremony's handle, as the browser sees it: 128 random bits, hex. It is a **capability** — the
/// only thing that can finish this ceremony — so it is minted from OS entropy and never derived from
/// the account, the email or a counter.
#[derive(Clone, PartialEq, Eq, Hash)]
pub struct ChallengeId(String);

impl ChallengeId {
    /// The value to hand the browser.
    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for ChallengeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// Redacted on purpose: a challenge id in a log line is a live capability, and logs travel.
impl fmt::Debug for ChallengeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("ChallengeId(<redacted>)")
    }
}

/// Where the relying party lives. **Fixed by configuration and never derived from a request header**
/// (ADR-0048 consequences): an RP id taken from `Host` is an invitation to have credentials minted for
/// a domain the operator does not control.
#[derive(Debug, Clone)]
pub struct CeremonyConfig {
    /// The effective domain, e.g. `rime.peekstar.eu`. Development uses `localhost` with its own store;
    /// a passkey is bound to its origin, so the two never interchange.
    pub rp_id: String,
    /// The full origin, e.g. `https://rime.peekstar.eu`.
    pub rp_origin: String,
    /// What the authenticator shows the user.
    pub rp_name: String,
}

/// What `start_*` produced: the handle to finish with, and the options blob to hand the browser.
#[derive(Debug)]
pub struct Started {
    pub challenge_id: ChallengeId,
    /// The `PublicKeyCredentialCreationOptions` / `RequestOptions` JSON, already in the shape
    /// `navigator.credentials` expects.
    pub options_json: String,
}

/// A passkey that just came into existence, in the form [`crate::AuthStore::add_credential`] takes.
#[derive(Debug)]
pub struct NewCredential {
    pub account: AccountId,
    pub credential_id: Vec<u8>,
    pub blob: Vec<u8>,
}

/// A successful login.
#[derive(Debug)]
pub struct Authenticated {
    pub account: AccountId,
    pub credential_id: Vec<u8>,
    /// Set when the authenticator moved its signature counter or backup state, in which case the
    /// store must be told: a counter that is never written back cannot detect a cloned credential,
    /// which is the whole reason the counter exists.
    pub refreshed_blob: Option<Vec<u8>>,
}

/// Why a ceremony did not complete.
#[derive(Debug)]
pub enum CeremonyError {
    /// The relying-party configuration is not usable — a malformed origin, or an id that does not
    /// match it. This is an operator error at startup, not a request error.
    Config(String),
    /// No such ceremony: never started, already finished, or expired and swept.
    UnknownChallenge,
    /// Started, but not within [`CHALLENGE_TTL`].
    Expired,
    /// The right challenge id from the wrong browser.
    WrongBrowser,
    /// [`MAX_PENDING`] ceremonies are already in flight.
    TooManyPending,
    /// A login was asked for against an account with no usable credential.
    NoCredentials,
    /// The authenticator did not verify the user (a PIN, a biometric). ADR-0048 requires it: a
    /// passkey that only proves possession is a bearer token with extra steps.
    UserNotVerified,
    /// The browser sent something that is not the ceremony response it should be.
    Malformed(String),
    /// `webauthn-rs` refused the ceremony. The text is for the operator's log, never for the browser:
    /// a precise reason is a precise oracle.
    Refused(String),
}

impl fmt::Display for CeremonyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CeremonyError::Config(why) => {
                write!(f, "relying-party configuration is unusable: {why}")
            }
            CeremonyError::UnknownChallenge => f.write_str("no such ceremony"),
            CeremonyError::Expired => f.write_str("the ceremony expired"),
            CeremonyError::WrongBrowser => {
                f.write_str("the ceremony was started by another browser")
            }
            CeremonyError::TooManyPending => f.write_str("too many ceremonies in flight"),
            CeremonyError::NoCredentials => f.write_str("the account has no usable credential"),
            CeremonyError::UserNotVerified => {
                f.write_str("the authenticator did not verify the user")
            }
            CeremonyError::Malformed(why) => write!(f, "malformed ceremony response: {why}"),
            CeremonyError::Refused(why) => write!(f, "the ceremony was refused: {why}"),
        }
    }
}

impl std::error::Error for CeremonyError {}

/// One in-flight ceremony. `S` is the `webauthn-rs` state for whichever ceremony it is.
struct Pending<S> {
    state: S,
    account: AccountId,
    binding: [u8; 32],
    expires_at: Timestamp,
}

/// The ceremonies, and the challenges they have in flight.
pub struct Ceremonies {
    webauthn: Webauthn,
    registrations: HashMap<String, Pending<PasskeyRegistration>>,
    authentications: HashMap<String, Pending<PasskeyAuthentication>>,
    ttl: Duration,
}

impl fmt::Debug for Ceremonies {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Never the states themselves: they contain live challenges.
        f.debug_struct("Ceremonies")
            .field("registrations_pending", &self.registrations.len())
            .field("authentications_pending", &self.authentications.len())
            .finish()
    }
}

impl Ceremonies {
    /// Build from a fixed relying-party configuration.
    pub fn new(config: &CeremonyConfig) -> Result<Self, CeremonyError> {
        let origin = Url::parse(&config.rp_origin)
            .map_err(|e| CeremonyError::Config(format!("origin {}: {e}", config.rp_origin)))?;
        let webauthn = WebauthnBuilder::new(&config.rp_id, &origin)
            .map_err(|e| CeremonyError::Config(e.to_string()))?
            .rp_name(&config.rp_name)
            .build()
            .map_err(|e| CeremonyError::Config(e.to_string()))?;
        Ok(Self {
            webauthn,
            registrations: HashMap::new(),
            authentications: HashMap::new(),
            ttl: CHALLENGE_TTL,
        })
    }

    /// How many ceremonies are in flight. For the gateway's counters — an unexplained climb here is
    /// somebody starting ceremonies they never finish.
    pub fn pending(&self) -> usize {
        self.registrations.len() + self.authentications.len()
    }

    /// Drop every ceremony whose time is up. Called on each start, and worth calling on a timer too:
    /// expiry that only happens when someone knocks is expiry that a quiet period suspends.
    pub fn expire(&mut self, now: Timestamp) {
        self.registrations.retain(|_, p| p.expires_at > now);
        self.authentications.retain(|_, p| p.expires_at > now);
    }

    /// Begin registering a passkey for `account`.
    ///
    /// `email` and `display_name` are what the authenticator will show; `existing` is every credential
    /// id the account already has, which becomes the ceremony's exclude list so an authenticator that
    /// already holds one for this account says so instead of silently making a second.
    pub fn start_registration(
        &mut self,
        account: AccountId,
        email: &str,
        display_name: &str,
        existing: &[Vec<u8>],
        browser_binding: &str,
        now: Timestamp,
    ) -> Result<Started, CeremonyError> {
        self.expire(now);
        if self.pending() >= MAX_PENDING {
            return Err(CeremonyError::TooManyPending);
        }
        let exclude: Vec<CredentialID> = existing.iter().cloned().map(CredentialID::from).collect();
        // The WebAuthn user handle. It is the ACCOUNT id, not the email: a user handle is meant to be
        // an opaque, stable identifier that does not change when the address does, and stuffing an
        // address in there would put it inside every authenticator that ever sees this account.
        let (options, state) = self
            .webauthn
            .start_passkey_registration(
                Uuid::from_u128(account.as_u128()),
                email,
                display_name,
                if exclude.is_empty() {
                    None
                } else {
                    Some(exclude)
                },
            )
            .map_err(|e| CeremonyError::Refused(e.to_string()))?;
        let id = self.mint_id()?;
        self.registrations.insert(
            id.0.clone(),
            Pending {
                state,
                account,
                binding: bind(browser_binding),
                expires_at: now + self.ttl.as_secs(),
            },
        );
        Ok(Started {
            challenge_id: id,
            options_json: to_json(&options)?,
        })
    }

    /// Complete a registration. On success the caller stores the credential and tells the user.
    pub fn finish_registration(
        &mut self,
        challenge_id: &str,
        browser_binding: &str,
        response_json: &str,
        now: Timestamp,
    ) -> Result<NewCredential, CeremonyError> {
        let response: RegisterPublicKeyCredential = from_json(response_json)?;
        // Removed before it is judged: a ceremony gets exactly one attempt, so a response that fails
        // verification cannot be retried against the same challenge. Take-then-check, never
        // check-then-take.
        let pending = take(&mut self.registrations, challenge_id, browser_binding, now)?;
        let passkey = self
            .webauthn
            .finish_passkey_registration(&response, &pending.state)
            .map_err(|e| CeremonyError::Refused(e.to_string()))?;
        Ok(NewCredential {
            account: pending.account,
            credential_id: passkey.cred_id().as_ref().to_vec(),
            blob: to_json(&passkey)?.into_bytes(),
        })
    }

    /// Begin authenticating `account` against the credentials it already holds.
    ///
    /// `credential_blobs` are the stored blobs of its ACTIVE credentials — the caller filters out
    /// revoked ones, because a revoked credential that can still start a ceremony is not revoked.
    pub fn start_authentication(
        &mut self,
        account: AccountId,
        credential_blobs: &[Vec<u8>],
        browser_binding: &str,
        now: Timestamp,
    ) -> Result<Started, CeremonyError> {
        self.expire(now);
        if self.pending() >= MAX_PENDING {
            return Err(CeremonyError::TooManyPending);
        }
        let passkeys = credential_blobs
            .iter()
            .map(|b| decode_passkey(b))
            .collect::<Result<Vec<Passkey>, CeremonyError>>()?;
        if passkeys.is_empty() {
            return Err(CeremonyError::NoCredentials);
        }
        let (options, state) = self
            .webauthn
            .start_passkey_authentication(&passkeys)
            .map_err(|e| CeremonyError::Refused(e.to_string()))?;
        let id = self.mint_id()?;
        self.authentications.insert(
            id.0.clone(),
            Pending {
                state,
                account,
                binding: bind(browser_binding),
                expires_at: now + self.ttl.as_secs(),
            },
        );
        Ok(Started {
            challenge_id: id,
            options_json: to_json(&options)?,
        })
    }

    /// Complete a login. The caller mints the session; this only says who proved what.
    pub fn finish_authentication(
        &mut self,
        challenge_id: &str,
        browser_binding: &str,
        response_json: &str,
        credential_blobs: &[Vec<u8>],
        now: Timestamp,
    ) -> Result<Authenticated, CeremonyError> {
        let response: PublicKeyCredential = from_json(response_json)?;
        let pending = take(
            &mut self.authentications,
            challenge_id,
            browser_binding,
            now,
        )?;
        let result = self
            .webauthn
            .finish_passkey_authentication(&response, &pending.state)
            .map_err(|e| CeremonyError::Refused(e.to_string()))?;
        // `webauthn-rs` runs passkey ceremonies with UserVerificationPolicy::Required, so this is a
        // second lock on a door that should already be shut. It is here because ADR-0048 states user
        // verification as a requirement of ours, and a requirement that is only somebody else's
        // default is one library upgrade away from not being a requirement.
        if !result.user_verified() {
            return Err(CeremonyError::UserNotVerified);
        }
        let used = result.cred_id().as_ref().to_vec();
        // Write the counter and backup state back if they moved. `update_credential` returns
        // Some(true) when this passkey is the one that was used AND its state changed.
        let mut refreshed = None;
        if result.needs_update() {
            for blob in credential_blobs {
                let mut passkey = decode_passkey(blob)?;
                if passkey.cred_id().as_ref() == used.as_slice() {
                    if passkey.update_credential(&result) == Some(true) {
                        refreshed = Some(to_json(&passkey)?.into_bytes());
                    }
                    break;
                }
            }
        }
        Ok(Authenticated {
            account: pending.account,
            credential_id: used,
            refreshed_blob: refreshed,
        })
    }

    fn mint_id(&self) -> Result<ChallengeId, CeremonyError> {
        let mut bytes = [0u8; 16];
        fill_random(&mut bytes)
            .map_err(|e| CeremonyError::Config(format!("no OS entropy: {e}")))?;
        Ok(ChallengeId(to_hex(&bytes)))
    }
}

/// Decode a stored blob back into a passkey.
///
/// The blob is the library's own serialisation, kept whole and never reconstructed field by field: a
/// credential's public key, algorithm, counter and backup state are a unit, and a store that rebuilds
/// them is a store that can silently drop one.
pub fn decode_passkey(blob: &[u8]) -> Result<Passkey, CeremonyError> {
    serde_json::from_slice(blob).map_err(|e| CeremonyError::Malformed(format!("credential: {e}")))
}

/// Take a pending ceremony out of its map, refusing the wrong browser and the expired.
///
/// The order matters: the entry is removed FIRST, so every path below — expired, wrong browser,
/// failed verification — consumes the challenge. An error that left the challenge in place would turn
/// it into something an attacker may keep guessing against.
fn take<S>(
    map: &mut HashMap<String, Pending<S>>,
    challenge_id: &str,
    browser_binding: &str,
    now: Timestamp,
) -> Result<Pending<S>, CeremonyError> {
    let pending = map
        .remove(challenge_id)
        .ok_or(CeremonyError::UnknownChallenge)?;
    if pending.expires_at <= now {
        return Err(CeremonyError::Expired);
    }
    if !constant_time_eq(&pending.binding, &bind(browser_binding)) {
        return Err(CeremonyError::WrongBrowser);
    }
    Ok(pending)
}

/// What is stored for a browser binding: the hash, never the value. The binding is a bearer value of
/// the gateway's, and this module has no business being a second copy of it.
fn bind(browser_binding: &str) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(browser_binding.as_bytes());
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest);
    out
}

/// Compare without leaking where the difference is. Both inputs are fixed-length hashes, so this is
/// the whole of it: no early return, no length branch.
fn constant_time_eq(a: &[u8; 32], b: &[u8; 32]) -> bool {
    let mut diff = 0u8;
    for i in 0..32 {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

fn to_json<T: serde::Serialize>(value: &T) -> Result<String, CeremonyError> {
    serde_json::to_string(value).map_err(|e| CeremonyError::Malformed(e.to_string()))
}

fn from_json<T: serde::de::DeserializeOwned>(text: &str) -> Result<T, CeremonyError> {
    serde_json::from_str(text).map_err(|e| CeremonyError::Malformed(e.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use webauthn_authenticator_rs::softpasskey::SoftPasskey;
    use webauthn_authenticator_rs::WebauthnAuthenticator;
    use webauthn_rs::prelude::CreationChallengeResponse;
    use webauthn_rs::prelude::RequestChallengeResponse;

    const ORIGIN: &str = "https://rime.example";
    const NOW: Timestamp = 1_700_000_000;

    fn ceremonies() -> Ceremonies {
        Ceremonies::new(&CeremonyConfig {
            rp_id: "rime.example".to_string(),
            rp_origin: ORIGIN.to_string(),
            rp_name: "Rime".to_string(),
        })
        .expect("a well-formed relying party")
    }

    fn account() -> AccountId {
        "0123456789abcdef0123456789abcdef".parse().unwrap()
    }

    /// Drive a whole registration through a software authenticator and hand back the stored blob —
    /// the same bytes `AuthStore::add_credential` would hold.
    fn register(
        c: &mut Ceremonies,
        auth: &mut WebauthnAuthenticator<SoftPasskey>,
        binding: &str,
    ) -> NewCredential {
        let started = c
            .start_registration(
                account(),
                "claire@example.test",
                "Claire",
                &[],
                binding,
                NOW,
            )
            .expect("registration starts");
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).expect("options are the browser's JSON");
        let response = auth
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .expect("the authenticator makes a credential");
        c.finish_registration(
            started.challenge_id.expose(),
            binding,
            &serde_json::to_string(&response).unwrap(),
            NOW + 1,
        )
        .expect("registration finishes")
    }

    // A real ceremony, not a mock: the software authenticator signs a challenge it did not choose, and
    // `webauthn-rs` verifies the signature against the public key it registered a moment earlier. If
    // any of the challenge, origin or relying-party plumbing were wrong, this would not verify.
    #[test]
    #[cfg(unix)]
    fn a_registered_passkey_can_then_authenticate() {
        let mut c = ceremonies();
        let mut auth = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let credential = register(&mut c, &mut auth, "browser-a");
        assert_eq!(credential.account, account());
        assert!(!credential.credential_id.is_empty());

        let blobs = vec![credential.blob.clone()];
        let started = c
            .start_authentication(account(), &blobs, "browser-a", NOW + 2)
            .expect("authentication starts");
        let options: RequestChallengeResponse =
            serde_json::from_str(&started.options_json).expect("options are the browser's JSON");
        let assertion = auth
            .do_authentication(Url::parse(ORIGIN).unwrap(), options)
            .expect("the authenticator signs");
        let who = c
            .finish_authentication(
                started.challenge_id.expose(),
                "browser-a",
                &serde_json::to_string(&assertion).unwrap(),
                &blobs,
                NOW + 3,
            )
            .expect("authentication finishes");
        assert_eq!(who.account, account());
        assert_eq!(who.credential_id, credential.credential_id);
    }

    #[test]
    #[cfg(unix)]
    fn a_challenge_finished_from_another_browser_is_refused() {
        let mut c = ceremonies();
        let mut auth = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let started = c
            .start_registration(
                account(),
                "claire@example.test",
                "Claire",
                &[],
                "browser-a",
                NOW,
            )
            .unwrap();
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let response = auth
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let outcome = c.finish_registration(
            started.challenge_id.expose(),
            "browser-b",
            &serde_json::to_string(&response).unwrap(),
            NOW + 1,
        );
        assert!(matches!(outcome, Err(CeremonyError::WrongBrowser)));
        // And it is spent: the right browser cannot rescue it either, because a failed finish
        // consumes the challenge rather than leaving it to be guessed against.
        let retry = c.finish_registration(
            started.challenge_id.expose(),
            "browser-a",
            &serde_json::to_string(&response).unwrap(),
            NOW + 1,
        );
        assert!(matches!(retry, Err(CeremonyError::UnknownChallenge)));
    }

    #[test]
    #[cfg(unix)]
    fn a_ceremony_can_be_finished_exactly_once() {
        let mut c = ceremonies();
        let mut auth = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let started = c
            .start_registration(account(), "claire@example.test", "Claire", &[], "b", NOW)
            .unwrap();
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let response = auth
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let body = serde_json::to_string(&response).unwrap();
        assert!(c
            .finish_registration(started.challenge_id.expose(), "b", &body, NOW + 1)
            .is_ok());
        let replay = c.finish_registration(started.challenge_id.expose(), "b", &body, NOW + 2);
        assert!(matches!(replay, Err(CeremonyError::UnknownChallenge)));
    }

    #[test]
    #[cfg(unix)]
    fn an_expired_ceremony_is_refused_even_with_a_valid_response() {
        let mut c = ceremonies();
        let mut auth = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let started = c
            .start_registration(account(), "claire@example.test", "Claire", &[], "b", NOW)
            .unwrap();
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let response = auth
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let late = NOW + CHALLENGE_TTL.as_secs() + 1;
        let outcome = c.finish_registration(
            started.challenge_id.expose(),
            "b",
            &serde_json::to_string(&response).unwrap(),
            late,
        );
        assert!(matches!(outcome, Err(CeremonyError::Expired)));
    }

    // An assertion is bound to the challenge it answers. Finishing ceremony B with ceremony A's
    // signed response must fail, or a captured assertion would be reusable against any live login.
    #[test]
    #[cfg(unix)]
    fn an_assertion_does_not_satisfy_a_different_ceremony() {
        let mut c = ceremonies();
        let mut auth = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let credential = register(&mut c, &mut auth, "b");
        let blobs = vec![credential.blob.clone()];

        let first = c
            .start_authentication(account(), &blobs, "b", NOW + 2)
            .unwrap();
        let second = c
            .start_authentication(account(), &blobs, "b", NOW + 2)
            .unwrap();
        let options: RequestChallengeResponse = serde_json::from_str(&first.options_json).unwrap();
        let assertion = auth
            .do_authentication(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let outcome = c.finish_authentication(
            second.challenge_id.expose(),
            "b",
            &serde_json::to_string(&assertion).unwrap(),
            &blobs,
            NOW + 3,
        );
        assert!(matches!(outcome, Err(CeremonyError::Refused(_))));
    }

    #[test]
    #[cfg(unix)]
    fn authenticating_an_account_with_no_credential_is_refused() {
        let mut c = ceremonies();
        let outcome = c.start_authentication(account(), &[], "b", NOW);
        assert!(matches!(outcome, Err(CeremonyError::NoCredentials)));
    }

    #[test]
    #[cfg(unix)]
    fn the_pending_map_is_bounded() {
        let mut c = ceremonies();
        for _ in 0..MAX_PENDING {
            c.start_registration(account(), "claire@example.test", "Claire", &[], "b", NOW)
                .expect("under the cap");
        }
        let over = c.start_registration(account(), "claire@example.test", "Claire", &[], "b", NOW);
        assert!(matches!(over, Err(CeremonyError::TooManyPending)));
        // Expiry frees the slots, so the cap is a bound on CONCURRENT ceremonies rather than a
        // permanent ceiling a single flood can pin.
        c.expire(NOW + CHALLENGE_TTL.as_secs() + 1);
        assert_eq!(c.pending(), 0);
        assert!(c
            .start_registration(
                account(),
                "claire@example.test",
                "Claire",
                &[],
                "b",
                NOW + CHALLENGE_TTL.as_secs() + 1
            )
            .is_ok());
    }

    #[test]
    fn a_malformed_relying_party_is_a_configuration_error() {
        let outcome = Ceremonies::new(&CeremonyConfig {
            rp_id: "rime.example".to_string(),
            rp_origin: "not a url".to_string(),
            rp_name: "Rime".to_string(),
        });
        assert!(matches!(outcome, Err(CeremonyError::Config(_))));
    }

    #[test]
    fn a_challenge_id_never_prints_itself() {
        let id = ChallengeId("deadbeef".to_string());
        assert_eq!(format!("{id:?}"), "ChallengeId(<redacted>)");
        assert_eq!(format!("{id}"), "deadbeef");
    }
}
