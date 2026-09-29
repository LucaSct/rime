// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Signing a browser in with your phone — [ADR-0055](../../../../docs/adr/0055-signing-in-a-browser-with-your-phone.md).
//!
//! Some browsers cannot use a passkey at all (Firefox on Linux has no platform authenticator and no
//! hybrid transport), and the account's passkey lives on a phone. So the desktop gets a **session**,
//! approved from the phone, and never a credential:
//!
//! 1. the desktop opens a pairing, bound to its ceremony cookie, and shows the id as a QR code;
//! 2. the phone scans it and runs an ordinary, user-verified passkey assertion **bound to that
//!    pairing**;
//! 3. the first valid assertion flips the pairing to *approved* and mints an eight-digit code, which
//!    only the phone is shown;
//! 4. the user types the code into the desktop, which learns WHICH account approved it and must
//!    confirm that before
//! 5. exactly one ordinary session is minted for the desktop, and the account holder is mailed.
//!
//! ## Why each step is there
//!
//! - **The desktop binding** means that photographing the QR, or the code, is not enough: only the
//!   browser that opened the pairing can poll it, redeem the code or collect the session.
//! - **The code** closes the loop in the other direction: it proves that the person at the desktop
//!   has seen the phone that approved. An approval made by somebody else — who photographed the QR
//!   from across the room and approved it with their own account — does not complete by itself.
//! - **The confirmation of the email** defeats *account substitution*: an attacker who approves your
//!   QR with THEIR account and talks you into typing the code would otherwise sign you into their
//!   account, where everything you then do is theirs to read.
//!
//! ## What it does not stop
//!
//! A real-time relay phish — the victim approves a QR the attacker is showing and types the code into
//! the attacker's page — completes. This flow is not phishing-resistant the way a passkey used
//! directly is; the phone screen's warning is the only mitigation, and ADR-0055 says so.
//!
//! ## One refusal
//!
//! Unknown, expired, finished, wrong-state and not-this-browser are all [`FlowError::PairingRefused`].
//! A pairing id is a capability, and a refusal that differed between "does not exist" and "is not
//! yours" would turn every id an attacker sees into an oracle.

use std::fmt;
use std::time::Duration;

use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;

use super::{random_hex, Auth, FlowError, LoggedIn};
use crate::ceremony::{CeremonyError, Started};
use crate::codes::{self, CodeChallenge, CodeOutcome, MailCode};
use crate::{AccountId, Timestamp, DEFAULT_SESSION_TTL};

/// How long a pairing lives from the moment the desktop opens it, **never extended**. Five minutes is
/// long enough to find the phone, unlock it and approve; a pairing that could be kept alive by polling
/// would be a standing invitation for as long as a tab stayed open.
pub const PAIRING_TTL: Duration = Duration::from_secs(5 * 60);

/// How many pairings may be live at once, across everybody. Opening one needs no account — it is the
/// first thing a signed-out browser does — so this is server state an unauthenticated request can
/// create, and like the ceremony map it has a ceiling. Beyond it the answer is "try again shortly",
/// never growth.
pub const MAX_PAIRINGS: usize = 64;

/// The phone's passkey challenge lives at most this long, and never past the pairing itself.
pub const PAIRING_CHALLENGE_TTL: Duration = Duration::from_secs(2 * 60);

/// The purpose a pairing code is MAC'd under. A different purpose from a mailed registration or
/// recovery code, so a code minted for one can never verify as the other even with the same key.
const PURPOSE_PAIR: &str = "pair";

/// The desktop's user-agent summary is shown on the phone, so it is capped: it is text an
/// unauthenticated request chose.
const USER_AGENT_MAX: usize = 64;

/// What the desktop is handed when it opens a pairing.
pub struct PairingStarted {
    /// The pairing id. 128 random bits, and a capability — it goes into the QR code's URL fragment
    /// and nowhere else.
    pub id: String,
    /// Seconds until the pairing is dead, for the desktop's countdown.
    pub expires_in: u64,
}

// A derived Debug would print the id, and an id is a capability: a trace of this value is a leak.
impl fmt::Debug for PairingStarted {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PairingStarted")
            .field("id", &"<redacted>")
            .field("expires_in", &self.expires_in)
            .finish()
    }
}

/// What the phone may see before it approves anything: the desktop's own description of itself,
/// which is **unverified** (the desktop chose it), and how long ago it asked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairingView {
    pub user_agent: String,
    pub age_secs: u64,
}

/// What the desktop's poll learns. Never the code and never the account: those are the two things
/// that would let whoever holds the desktop binding skip the human in the middle.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PairingStatus {
    Waiting,
    Approved,
    Expired,
}

/// The desktop's last word: sign in as the account it was shown, or throw the pairing away.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PairingDecision {
    Accept,
    Refuse,
}

/// Where one pairing has got to. `Done` and `Dead` are not states here: a finished or killed pairing is
/// **removed**, so it is indistinguishable from one that never existed, by construction rather than
/// by every caller remembering to answer the same way.
enum PairState {
    /// Opened by the desktop; no phone has proved anything yet.
    Waiting,
    /// A phone approved it. Only the code's MAC is kept; the plaintext went to the phone and nowhere
    /// else.
    Approved {
        account: AccountId,
        email: String,
        code: CodeChallenge,
    },
    /// The desktop proved the code and has been told which account approved it. The next step is its
    /// explicit accept.
    Confirming { account: AccountId, email: String },
}

pub(super) struct Pairing {
    /// SHA-256 of the desktop's ceremony cookie — never the value, for the reason `ceremony.rs` gives:
    /// this module has no business being a second copy of a bearer value.
    desktop: [u8; 32],
    created: Timestamp,
    pub(super) expires_at: Timestamp,
    user_agent: String,
    state: PairState,
}

impl fmt::Debug for Pairing {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Pairing(<redacted>)")
    }
}

impl Auth {
    /// Open a pairing for the desktop holding `desktop_binding`. `user_agent` is the gateway's
    /// summary of the desktop's `User-Agent`, shown to the phone labelled as unverified.
    pub fn begin_pairing(
        &mut self,
        desktop_binding: &str,
        user_agent: &str,
        now: Timestamp,
    ) -> Result<PairingStarted, FlowError> {
        self.expire(now);
        if self.pairings.len() >= MAX_PAIRINGS {
            return Err(FlowError::TooManyPairings);
        }
        let id = random_hex()?;
        let user_agent: String = user_agent.chars().take(USER_AGENT_MAX).collect();
        self.pairings.insert(
            id.clone(),
            Pairing {
                desktop: digest(desktop_binding),
                created: now,
                expires_at: now + PAIRING_TTL.as_secs(),
                user_agent,
                state: PairState::Waiting,
            },
        );
        Ok(PairingStarted {
            id,
            expires_in: PAIRING_TTL.as_secs(),
        })
    }

    /// What the phone shows before the user decides anything. A read: it reserves, approves and
    /// extends nothing, so a phone that merely opens the link has changed no state at all.
    pub fn describe_pairing(&self, id: &str, now: Timestamp) -> Result<PairingView, FlowError> {
        let pairing = self.waiting(id, now)?;
        Ok(PairingView {
            user_agent: pairing.user_agent.clone(),
            age_secs: now.saturating_sub(pairing.created),
        })
    }

    /// Start the phone's passkey assertion for this pairing.
    ///
    /// The ceremony is bound to the phone's OWN ceremony cookie **and** to the pairing id (see
    /// [`approval_binding`]), so a challenge from this call cannot finish an ordinary login, cannot
    /// approve a different pairing, and a login challenge cannot approve this one. A session cookie
    /// the phone may already hold plays no part: approving a device needs a fresh, user-verified
    /// assertion, because a stolen session cookie must not be able to add a browser to an account.
    pub fn start_pairing_approval(
        &mut self,
        id: &str,
        email: &str,
        phone_binding: &str,
        now: Timestamp,
    ) -> Result<Started, FlowError> {
        let deadline = self.waiting(id, now)?.expires_at;
        let (account, blobs) = self.active_credentials(email)?;
        // min(2 min, the pairing's own remaining life): a challenge that outlived its pairing would be
        // a proof with nothing left to spend it on — harmless, but a thing to reason about for no gain.
        let ttl = PAIRING_CHALLENGE_TTL.min(Duration::from_secs(deadline - now));
        Ok(self.ceremonies.start_authentication_within(
            account,
            &blobs,
            &approval_binding(id, phone_binding),
            now,
            ttl,
        )?)
    }

    /// Verify the phone's assertion and approve the pairing, returning the code **for the phone only**.
    ///
    /// The account is read off the verified credential, never from the client. Only the first valid
    /// approval wins: the pairing must still be `Waiting` after the assertion verifies, and it is
    /// flipped in the same `&mut self` call, so two phones racing cannot both approve. No session is
    /// minted for the phone — approving a desktop is not logging the phone in.
    pub fn approve_pairing(
        &mut self,
        id: &str,
        challenge_id: &str,
        phone_binding: &str,
        response_json: &str,
        now: Timestamp,
    ) -> Result<MailCode, FlowError> {
        self.waiting(id, now)?;
        let account = self
            .ceremonies
            .authentication_account(challenge_id)
            .ok_or(FlowError::Ceremony(CeremonyError::UnknownChallenge))?;
        let blobs: Vec<Vec<u8>> = self
            .store
            .active_credentials(account)
            .iter()
            .map(|c| c.blob.clone())
            .collect();
        let who = self.ceremonies.finish_authentication(
            challenge_id,
            &approval_binding(id, phone_binding),
            response_json,
            &blobs,
            now,
        )?;
        // The counter write-back `finish_login` does, for the same reason: a cloned authenticator is
        // only detectable if the counter the store holds keeps moving.
        if let Some(blob) = who.refreshed_blob {
            self.store
                .update_credential_blob(&who.credential_id, &blob, now)?;
        }
        let email = self
            .store
            .account(who.account)
            .filter(|a| a.is_active())
            .ok_or(FlowError::NoSuchAccount)?
            .email
            .clone();
        // Re-checked after the assertion: it is the flip from Waiting that makes an approval the first.
        let pairing = self
            .pairings
            .get_mut(id)
            .filter(|p| p.expires_at > now && matches!(p.state, PairState::Waiting))
            .ok_or(FlowError::PairingRefused)?;
        // The code is bound to THIS pairing id (codes.rs MACs purpose, binding and digits together),
        // so a code shown for one pairing cannot redeem another, and it dies with the pairing.
        let (code, challenge) = codes::mint(
            &self.code_key,
            PURPOSE_PAIR,
            id,
            now,
            Duration::from_secs(pairing.expires_at - now),
        )
        .map_err(|e| FlowError::Store(crate::AuthError::Io(e)))?;
        pairing.state = PairState::Approved {
            account: who.account,
            email,
            code: challenge,
        };
        Ok(code)
    }

    /// The desktop's poll. Only the desktop that opened the pairing gets an answer at all.
    pub fn pairing_status(
        &mut self,
        id: &str,
        desktop_binding: &str,
        now: Timestamp,
    ) -> Result<PairingStatus, FlowError> {
        let pairing = self.pairings.get(id).ok_or(FlowError::PairingRefused)?;
        if !same_binding(pairing, desktop_binding) {
            return Err(FlowError::PairingRefused);
        }
        if pairing.expires_at <= now {
            // Its own desktop may be told it expired — it knows the pairing existed. It is removed
            // here, so the next poll gets the ordinary refusal like everybody else.
            self.pairings.remove(id);
            return Ok(PairingStatus::Expired);
        }
        Ok(match pairing.state {
            PairState::Waiting => PairingStatus::Waiting,
            PairState::Approved { .. } | PairState::Confirming { .. } => PairingStatus::Approved,
        })
    }

    /// The desktop types the phone's code. On success the pairing moves to `Confirming` and the
    /// desktop is told which account approved it, so the person can refuse an account that is not
    /// theirs. [`codes::DEFAULT_CODE_ATTEMPTS`] wrong codes in total and the pairing is dead: five
    /// guesses at 10⁸ codes per pairing.
    pub fn redeem_pairing(
        &mut self,
        id: &str,
        desktop_binding: &str,
        code: &str,
        now: Timestamp,
    ) -> Result<String, FlowError> {
        let key = &self.code_key;
        let pairing = desktop_pairing(&mut self.pairings, id, desktop_binding, now)?;
        let PairState::Approved {
            account,
            email,
            code: challenge,
        } = &mut pairing.state
        else {
            return Err(FlowError::PairingRefused);
        };
        match codes::verify(challenge, key, id, code, now) {
            CodeOutcome::Accepted => {
                let (account, email) = (*account, email.clone());
                pairing.state = PairState::Confirming {
                    account,
                    email: email.clone(),
                };
                Ok(email)
            }
            CodeOutcome::Rejected => Err(FlowError::CodeRejected),
            // Exhausted or expired: the pairing is dead, and dead means gone.
            CodeOutcome::Exhausted | CodeOutcome::Expired => {
                self.pairings.remove(id);
                Err(FlowError::CodeRejected)
            }
        }
    }

    /// The desktop's decision. `Accept` is only valid in `Confirming` and mints exactly one ordinary
    /// session, then mails the account holder; `Refuse` kills the pairing from any live state.
    ///
    /// The pairing is removed **before** the session is minted, so there is no window in which a
    /// second `finish` could find it still `Confirming` — and if minting fails, the user starts again
    /// rather than holding a pairing that might yet produce a second session.
    pub fn finish_pairing(
        &mut self,
        id: &str,
        desktop_binding: &str,
        decision: PairingDecision,
        now: Timestamp,
    ) -> Result<Option<LoggedIn>, FlowError> {
        let pairing = desktop_pairing(&mut self.pairings, id, desktop_binding, now)?;
        let confirmed = match &pairing.state {
            PairState::Confirming { account, email } => Some((*account, email.clone())),
            _ => None,
        };
        if decision == PairingDecision::Refuse {
            self.pairings.remove(id);
            return Ok(None);
        }
        let (account, email) = confirmed.ok_or(FlowError::PairingRefused)?;
        self.pairings.remove(id);
        // An account disabled between approval and finish gets nothing.
        if !self.store.account(account).is_some_and(|a| a.is_active()) {
            return Err(FlowError::PairingRefused);
        }
        let session = self
            .store
            .create_session(account, DEFAULT_SESSION_TTL, now)?;
        self.announce(
            &email,
            "a browser was signed in with your phone",
            &format!(
                "A browser was signed in to your account with your phone at {}.\n\nIf this wasn't \
                 you, somebody got you to approve their screen: sign in, remove the passkey you \
                 used from any device you no longer trust, and reply to this mail.",
                utc(now)
            ),
        );
        Ok(Some(LoggedIn { account, session }))
    }

    /// Live pairings, for counters and tests.
    pub fn pairings(&self) -> usize {
        self.pairings.len()
    }

    /// A pairing that is alive and still waiting for a phone.
    fn waiting(&self, id: &str, now: Timestamp) -> Result<&Pairing, FlowError> {
        self.pairings
            .get(id)
            .filter(|p| p.expires_at > now && matches!(p.state, PairState::Waiting))
            .ok_or(FlowError::PairingRefused)
    }
}

/// A live pairing that belongs to this desktop, or the one refusal.
fn desktop_pairing<'a>(
    pairings: &'a mut std::collections::HashMap<String, Pairing>,
    id: &str,
    desktop_binding: &str,
    now: Timestamp,
) -> Result<&'a mut Pairing, FlowError> {
    pairings
        .get_mut(id)
        .filter(|p| p.expires_at > now && same_binding(p, desktop_binding))
        .ok_or(FlowError::PairingRefused)
}

/// The browser binding the phone's ceremony is stored under: the phone's cookie AND the pairing id.
/// Folding the id in is what makes the challenge single-purpose without a second table — the
/// ceremony module compares bindings, and a login (bound to the cookie alone) or another pairing
/// (bound to a different id) simply does not match. The NUL separators keep `("a", "bc")` and
/// `("ab", "c")` from producing the same string.
fn approval_binding(id: &str, phone_binding: &str) -> String {
    format!("pair\0{id}\0{phone_binding}")
}

fn digest(value: &str) -> [u8; 32] {
    Sha256::digest(value.as_bytes()).into()
}

fn same_binding(pairing: &Pairing, binding: &str) -> bool {
    bool::from(pairing.desktop.ct_eq(&digest(binding)))
}

/// `YYYY-MM-DD HH:MM UTC` for the announcement mail, from Unix seconds. Howard Hinnant's
/// `civil_from_days`: shift the epoch to 0000-03-01 so the leap day falls at the END of a year, then
/// count in 400-year eras (146 097 days each), which makes every step an integer division with no
/// table of month lengths. Hand-written because a date library is a large dependency for one line of
/// one mail.
fn utc(ts: Timestamp) -> String {
    let days = (ts / 86_400) as i64;
    let secs = ts % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02} {:02}:{:02} UTC",
        secs / 3_600,
        secs % 3_600 / 60
    )
}

#[cfg(test)]
#[cfg(unix)]
mod tests {
    use super::super::tests::{auth, register, INVITED, NOW, ORIGIN};
    use super::*;
    use webauthn_authenticator_rs::softpasskey::SoftPasskey;
    use webauthn_authenticator_rs::WebauthnAuthenticator;
    use webauthn_rs::prelude::{RequestChallengeResponse, Url};

    const DESKTOP: &str = "desktop-cookie";
    const PHONE: &str = "phone-cookie";

    /// The phone's half: options, then a real signed assertion from the software authenticator.
    fn phone_assertion(
        auth: &mut Auth,
        device: &mut WebauthnAuthenticator<SoftPasskey>,
        id: &str,
        now: Timestamp,
    ) -> (String, String) {
        let started = auth
            .start_pairing_approval(id, INVITED, PHONE, now)
            .expect("the phone's ceremony starts");
        let options: RequestChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), options)
            .expect("the phone signs");
        (
            started.challenge_id.expose().to_string(),
            serde_json::to_string(&assertion).unwrap(),
        )
    }

    fn approved(
        auth: &mut Auth,
        device: &mut WebauthnAuthenticator<SoftPasskey>,
        now: Timestamp,
    ) -> (String, String) {
        let id = auth
            .begin_pairing(DESKTOP, "Firefox on Linux", now)
            .unwrap()
            .id;
        let (challenge, signed) = phone_assertion(auth, device, &id, now + 1);
        let code = auth
            .approve_pairing(&id, &challenge, PHONE, &signed, now + 2)
            .expect("the first valid approval wins");
        (id, code.expose().to_string())
    }

    fn wrong(code: &str) -> &'static str {
        if code == "00000000" {
            "11111111"
        } else {
            "00000000"
        }
    }

    // Happy path: exactly one session, one mail, and the pairing is gone — so a second finish (a
    // replay of the desktop's last request) is refused.
    #[test]
    fn a_pairing_signs_in_exactly_one_desktop_and_mails_once() {
        let (mut auth, mailer) = auth();
        let (done, mut device) = register(&mut auth, &mailer);
        let before = mailer.sent().len();
        let (id, code) = approved(&mut auth, &mut device, NOW + 10);
        assert_eq!(
            auth.pairing_status(&id, DESKTOP, NOW + 13).unwrap(),
            PairingStatus::Approved
        );
        assert_eq!(
            auth.redeem_pairing(&id, DESKTOP, &code, NOW + 14).unwrap(),
            INVITED
        );
        let signed_in = auth
            .finish_pairing(&id, DESKTOP, PairingDecision::Accept, NOW + 15)
            .unwrap()
            .expect("a session");
        assert_eq!(signed_in.account, done.account);
        assert_eq!(
            auth.store_mut()
                .authenticate_session(signed_in.session.expose(), NOW + 16),
            Some(done.account)
        );
        assert_eq!(mailer.sent().len(), before + 1, "one announcement");
        assert!(mailer
            .sent()
            .last()
            .unwrap()
            .body
            .contains("with your phone"));
        assert_eq!(auth.pairings(), 0);
        assert!(matches!(
            auth.finish_pairing(&id, DESKTOP, PairingDecision::Accept, NOW + 17),
            Err(FlowError::PairingRefused)
        ));
    }

    // Photographing the QR (or the code) is not enough: without the desktop's binding every desktop
    // step is the SAME refusal an unknown id gets.
    #[test]
    fn another_browser_gets_the_refusal_an_unknown_id_gets() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let (id, code) = approved(&mut auth, &mut device, NOW + 10);
        let unknown = "0".repeat(32);
        for target in [id.as_str(), unknown.as_str()] {
            assert!(matches!(
                auth.pairing_status(target, "thief", NOW + 11),
                Err(FlowError::PairingRefused)
            ));
            assert!(matches!(
                auth.redeem_pairing(target, "thief", &code, NOW + 11),
                Err(FlowError::PairingRefused)
            ));
            assert!(matches!(
                auth.finish_pairing(target, "thief", PairingDecision::Accept, NOW + 11),
                Err(FlowError::PairingRefused)
            ));
        }
        // And the thief's attempts cost the real desktop nothing.
        assert_eq!(
            auth.redeem_pairing(&id, DESKTOP, &code, NOW + 12).unwrap(),
            INVITED
        );
    }

    // Brute force: five wrong codes and the pairing is dead; the right code cannot rescue it.
    #[test]
    fn five_wrong_codes_kill_the_pairing() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let (id, code) = approved(&mut auth, &mut device, NOW + 10);
        for _ in 0..codes::DEFAULT_CODE_ATTEMPTS {
            assert!(matches!(
                auth.redeem_pairing(&id, DESKTOP, wrong(&code), NOW + 11),
                Err(FlowError::CodeRejected)
            ));
        }
        assert!(matches!(
            auth.redeem_pairing(&id, DESKTOP, &code, NOW + 12),
            Err(FlowError::PairingRefused)
        ));
        assert_eq!(auth.pairings(), 0);
    }

    // Replay and races: a second approval (another phone, or the same assertion again) is refused,
    // and an approval after the pairing's five minutes is refused.
    #[test]
    fn only_the_first_approval_wins_and_none_after_expiry() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let id = auth.begin_pairing(DESKTOP, "x", NOW + 10).unwrap().id;
        let (c1, s1) = phone_assertion(&mut auth, &mut device, &id, NOW + 11);
        let (c2, s2) = phone_assertion(&mut auth, &mut device, &id, NOW + 11);
        auth.approve_pairing(&id, &c1, PHONE, &s1, NOW + 12)
            .unwrap();
        assert!(matches!(
            auth.approve_pairing(&id, &c2, PHONE, &s2, NOW + 12),
            Err(FlowError::PairingRefused)
        ));
        assert!(auth
            .approve_pairing(&id, &c1, PHONE, &s1, NOW + 12)
            .is_err());

        let late = auth.begin_pairing(DESKTOP, "x", NOW + 20).unwrap().id;
        let (c3, s3) = phone_assertion(&mut auth, &mut device, &late, NOW + 21);
        let after = NOW + 20 + PAIRING_TTL.as_secs();
        assert!(matches!(
            auth.approve_pairing(&late, &c3, PHONE, &s3, after),
            Err(FlowError::PairingRefused)
        ));
    }

    // Two desktops racing: an assertion made for one pairing cannot approve another, and a code shown
    // for one cannot redeem the other.
    #[test]
    fn two_pairings_do_not_cross() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let a = auth.begin_pairing(DESKTOP, "x", NOW + 10).unwrap().id;
        let b = auth.begin_pairing(DESKTOP, "x", NOW + 10).unwrap().id;
        let (challenge, signed) = phone_assertion(&mut auth, &mut device, &a, NOW + 11);
        assert!(matches!(
            auth.approve_pairing(&b, &challenge, PHONE, &signed, NOW + 12),
            Err(FlowError::Ceremony(_))
        ));
        let (challenge, signed) = phone_assertion(&mut auth, &mut device, &a, NOW + 13);
        let code_a = auth
            .approve_pairing(&a, &challenge, PHONE, &signed, NOW + 14)
            .unwrap();
        let (challenge, signed) = phone_assertion(&mut auth, &mut device, &b, NOW + 15);
        auth.approve_pairing(&b, &challenge, PHONE, &signed, NOW + 16)
            .unwrap();
        assert!(matches!(
            auth.redeem_pairing(&b, DESKTOP, code_a.expose(), NOW + 17),
            Err(FlowError::CodeRejected)
        ));
    }

    // A stolen phone session (or an ordinary login challenge) is not an approval: only an assertion
    // started FOR this pairing verifies against it.
    #[test]
    fn a_login_challenge_cannot_approve_a_pairing() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let id = auth.begin_pairing(DESKTOP, "x", NOW + 10).unwrap().id;
        let started = auth.start_login(INVITED, PHONE, NOW + 11).unwrap();
        let options: RequestChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        assert!(matches!(
            auth.approve_pairing(
                &id,
                started.challenge_id.expose(),
                PHONE,
                &serde_json::to_string(&assertion).unwrap(),
                NOW + 12
            ),
            Err(FlowError::Ceremony(CeremonyError::WrongBrowser))
        ));
        // And the converse: a pairing challenge cannot be spent as a login to mint a phone session.
        let (challenge, signed) = phone_assertion(&mut auth, &mut device, &id, NOW + 13);
        assert!(auth
            .finish_login(&challenge, PHONE, &signed, NOW + 14)
            .is_err());
    }

    // Unauthenticated requests create pairings, so their number is capped rather than grown.
    #[test]
    fn the_sixty_fifth_live_pairing_is_refused() {
        let (mut auth, _mailer) = auth();
        for _ in 0..MAX_PAIRINGS {
            auth.begin_pairing(DESKTOP, "x", NOW).unwrap();
        }
        assert!(matches!(
            auth.begin_pairing(DESKTOP, "x", NOW),
            Err(FlowError::TooManyPairings)
        ));
        // Expiry frees the table: the cap bounds live state, not the lifetime count.
        assert!(auth
            .begin_pairing(DESKTOP, "x", NOW + PAIRING_TTL.as_secs())
            .is_ok());
    }

    // Account substitution is visible: the desktop is told whose account approved it BEFORE a session
    // exists, and refusing kills the pairing.
    #[test]
    fn the_desktop_can_refuse_the_account_it_is_shown() {
        let (mut auth, mailer) = auth();
        let (_done, mut device) = register(&mut auth, &mailer);
        let (id, code) = approved(&mut auth, &mut device, NOW + 10);
        assert_eq!(
            auth.redeem_pairing(&id, DESKTOP, &code, NOW + 11).unwrap(),
            INVITED
        );
        assert!(auth
            .finish_pairing(&id, DESKTOP, PairingDecision::Refuse, NOW + 12)
            .unwrap()
            .is_none());
        assert!(matches!(
            auth.finish_pairing(&id, DESKTOP, PairingDecision::Accept, NOW + 13),
            Err(FlowError::PairingRefused)
        ));
    }

    #[test]
    fn announcement_times_are_utc_calendar_dates() {
        assert_eq!(utc(0), "1970-01-01 00:00 UTC");
        assert_eq!(utc(1_700_000_000), "2023-11-14 22:13 UTC");
        assert_eq!(utc(951_782_400), "2000-02-29 00:00 UTC");
    }
}
