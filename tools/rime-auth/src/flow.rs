// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The ceremonies as a user experiences them — [ADR-0048](../../docs/adr/0048-authenticating-the-hosted-front-end.md) brick 2.
//!
//! The other modules are each one mechanism: a durable store, a WebAuthn ceremony, a mailed code, a
//! relay. This is the module that puts them in an order, and the order **is** the security property.
//! ADR-0048 decision 1 rules that an invitation authorises registration, a passkey authenticates, and
//! mail can never do either on its own. Nothing enforces that ruling except the state machine here, so
//! it is written as a state machine rather than as a set of helpers a caller may combine freely:
//!
//! - **Registration** needs an unconsumed invitation, *then* a code mailed to the address that
//!   invitation names, *then* a passkey. The invitation is consumed at the very end, together with the
//!   account and the credential.
//! - **Login** needs a passkey assertion and nothing else. There is deliberately no mail step, because
//!   a login that mail can complete is a login that mail can be made to complete.
//! - **Recovery** needs **both** the recovery secret shown once at registration **and** a fresh mailed
//!   code, and it can only register a replacement passkey. Completing it revokes every old credential,
//!   every session, and the secret itself.
//!
//! ## Why the address is never taken from the browser
//!
//! `begin_registration` takes an invitation token and reads the address **out of the invitation**. If
//! the caller supplied the address, a valid invitation would become a way to mail a code anywhere, and
//! the invitation would gate nothing but the existence of a code. Same rule in recovery: the address
//! comes from the stored account.
//!
//! ## What is deliberately not here
//!
//! Rate limiting. It needs the requester's address and the request's shape, which live in the gateway
//! (brick 3), and a limiter that can only see this crate's calls would count the wrong things.

use std::collections::HashMap;
use std::fmt;
use std::time::Duration;

mod pair;
pub use pair::{
    PairingDecision, PairingStarted, PairingStatus, PairingView, MAX_PAIRINGS,
    PAIRING_CHALLENGE_TTL, PAIRING_TTL,
};

use crate::ceremony::{Ceremonies, CeremonyConfig, CeremonyError, Started};
use crate::codes::{self, CodeChallenge, CodeKey, CodeOutcome, DEFAULT_CODE_TTL};
use crate::mail::{MailError, Mailer, Message};
use crate::{
    fill_random, to_hex, AccountId, AuthError, AuthStore, Timestamp, Token, DEFAULT_SESSION_TTL,
};

/// How long an unfinished registration or recovery lives. Longer than a mailed code's ten minutes, so
/// that a code expiring is what a user is told about rather than the transaction vanishing underneath
/// them; short enough that abandoned transactions are not a standing table of half-proofs.
pub const TRANSACTION_TTL: Duration = Duration::from_secs(20 * 60);

/// How many registrations and recoveries may be in flight. The same reasoning as the ceremony map:
/// server-side state an unauthenticated request can create has to have a ceiling.
pub const MAX_TRANSACTIONS: usize = 32;

/// The recovery secret's size. 256 bits, shown once, never stored in the clear.
const RECOVERY_SECRET_BYTES: usize = 32;

const PURPOSE_REGISTER: &str = "register";
const PURPOSE_RECOVER: &str = "recover";

/// A registration or recovery in progress. Opaque, single-use, and a capability like every other id
/// here — 128 random bits.
#[derive(Clone, PartialEq, Eq, Hash)]
pub struct TransactionId(String);

impl TransactionId {
    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl fmt::Debug for TransactionId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("TransactionId(<redacted>)")
    }
}

/// What a completed registration hands back. The recovery secret exists in this struct and nowhere
/// else — the store has only its hash — so a caller that does not show it to the user has lost it.
#[derive(Debug)]
pub struct Registered {
    pub account: AccountId,
    pub session: Token,
    pub recovery_secret: String,
}

/// A completed login.
#[derive(Debug)]
pub struct LoggedIn {
    pub account: AccountId,
    pub session: Token,
}

/// A completed recovery: a new passkey, a new session, a new recovery secret, and nothing left of
/// what came before.
#[derive(Debug)]
pub struct Recovered {
    pub account: AccountId,
    pub session: Token,
    pub recovery_secret: String,
    /// How many credentials the recovery revoked. Worth surfacing: a user who expected to lose one
    /// device and sees three is being told something.
    pub credentials_revoked: usize,
}

/// Why a flow did not proceed.
#[derive(Debug)]
pub enum FlowError {
    /// No such transaction: never started, already finished, or expired.
    UnknownTransaction,
    /// The transaction exists but this is not the step it is waiting for. A caller that could skip a
    /// step could skip a proof.
    WrongStep,
    /// The transaction's own deadline passed.
    Expired,
    /// The mailed code was wrong, is out of attempts, or has expired. One variant on purpose: which
    /// one it was belongs in the operator's counters, not in a reply to whoever is guessing.
    CodeRejected,
    /// Recovery was asked for with a wrong or absent recovery secret.
    RecoveryRefused,
    /// The invitation is unknown, spent or expired — one variant, same reasoning as the store's.
    InvitationRejected,
    /// No account for that address, or it is disabled.
    NoSuchAccount,
    /// [`MAX_TRANSACTIONS`] are already in flight.
    TooManyTransactions,
    /// A pairing (ADR-0055) is unknown, expired, finished, not in the state this step needs, or not
    /// this browser's. One variant for all five on purpose: a pairing id is a capability, and "this is
    /// real but not yours" is the one sentence a capability must never say.
    PairingRefused,
    /// [`MAX_PAIRINGS`] are already live.
    TooManyPairings,
    Store(AuthError),
    Ceremony(CeremonyError),
    Mail(MailError),
}

impl fmt::Display for FlowError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownTransaction => f.write_str("no such registration or recovery"),
            Self::WrongStep => f.write_str("that is not the step this transaction is waiting for"),
            Self::Expired => f.write_str("the transaction expired"),
            Self::CodeRejected => f.write_str("the mailed code was not accepted"),
            Self::RecoveryRefused => f.write_str("recovery needs both proofs"),
            Self::InvitationRejected => f.write_str("the invitation was not accepted"),
            Self::NoSuchAccount => f.write_str("no such account"),
            Self::TooManyTransactions => f.write_str("too many registrations in flight"),
            Self::PairingRefused => f.write_str("no such pairing"),
            Self::TooManyPairings => f.write_str("too many pairings in flight"),
            Self::Store(e) => write!(f, "store: {e}"),
            Self::Ceremony(e) => write!(f, "ceremony: {e}"),
            Self::Mail(e) => write!(f, "mail: {e}"),
        }
    }
}

impl std::error::Error for FlowError {}

impl From<AuthError> for FlowError {
    fn from(e: AuthError) -> Self {
        Self::Store(e)
    }
}
impl From<CeremonyError> for FlowError {
    fn from(e: CeremonyError) -> Self {
        Self::Ceremony(e)
    }
}
impl From<MailError> for FlowError {
    fn from(e: MailError) -> Self {
        Self::Mail(e)
    }
}

/// Which flow a transaction is, and what it already established.
enum Kind {
    /// Registration, holding the invitation token it validated but has not yet consumed.
    Registration { invitation: String },
    /// Recovery of an existing account.
    Recovery { account: AccountId },
}

/// Where a transaction has got to. A step is only reachable from the one before it.
enum Step {
    /// A code has been mailed and is waiting to be proved.
    AwaitingCode(CodeChallenge),
    /// Every proof this flow needs is in. The passkey ceremony may start.
    Proved,
    /// A passkey ceremony is open; its id is what finishes it.
    PasskeyOpen(String),
}

struct Transaction {
    kind: Kind,
    email: String,
    step: Step,
    expires_at: Timestamp,
    /// The account id a registration's ceremony was run with, once it has been. It is the WebAuthn
    /// user handle the authenticator signed over, so the account MUST be created with this id and not
    /// a fresh one — see `AuthStore::create_account_with_id`.
    pending_account: Option<AccountId>,
}

/// The whole of authentication, as one object the gateway holds.
pub struct Auth {
    store: AuthStore,
    ceremonies: Ceremonies,
    code_key: CodeKey,
    mailer: Box<dyn Mailer>,
    transactions: HashMap<String, Transaction>,
    /// Phone-approved sign-ins in flight (ADR-0055), by pairing id. In memory only: a restart
    /// invalidates every pairing, which costs a user one rescan and keeps half-approved states out of
    /// the durable log.
    pairings: HashMap<String, pair::Pairing>,
    display_name: String,
    /// The relying party's origin, fixed by configuration. Kept here so the gateway's `Origin` check
    /// compares against the SAME value the ceremonies were built with, never a second copy that can
    /// drift from it — and never anything taken from a request.
    rp_origin: String,
}

impl fmt::Debug for Auth {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Auth")
            .field("accounts", &self.store.account_count())
            .field("transactions", &self.transactions.len())
            .field("pairings", &self.pairings.len())
            .finish()
    }
}

impl Auth {
    /// Build from an opened store, a fixed relying party, and somewhere to post mail.
    pub fn new(
        store: AuthStore,
        config: &CeremonyConfig,
        mailer: Box<dyn Mailer>,
    ) -> Result<Self, FlowError> {
        let display_name = config.rp_name.clone();
        let rp_origin = config.rp_origin.clone();
        Ok(Self {
            store,
            ceremonies: Ceremonies::new(config)?,
            code_key: CodeKey::generate().map_err(|e| FlowError::Store(AuthError::Io(e)))?,
            mailer,
            transactions: HashMap::new(),
            pairings: HashMap::new(),
            display_name,
            rp_origin,
        })
    }

    /// The configured relying-party origin, e.g. `https://rime.example`.
    pub fn rp_origin(&self) -> &str {
        &self.rp_origin
    }

    /// The store, for the operator-facing things brick 3 needs (issuing invitations, counters).
    pub fn store(&self) -> &AuthStore {
        &self.store
    }

    /// The store, mutably. Brick 3 issues invitations through this.
    pub fn store_mut(&mut self) -> &mut AuthStore {
        &mut self.store
    }

    /// How many registrations and recoveries are open.
    pub fn transactions(&self) -> usize {
        self.transactions.len()
    }

    // ── Registration ──────────────────────────────────────────────────────────────────────────

    /// Validate an invitation and mail a verification code **to the address the invitation names**.
    ///
    /// The invitation is not consumed here. It is consumed in `finish_registration`, in the same
    /// moment the account and credential are created, so an abandoned registration does not burn one
    /// and a crash cannot leave a spent invitation with no account behind it.
    pub fn begin_registration(
        &mut self,
        invitation_token: &str,
        now: Timestamp,
    ) -> Result<TransactionId, FlowError> {
        self.expire(now);
        if self.transactions.len() >= MAX_TRANSACTIONS {
            return Err(FlowError::TooManyTransactions);
        }
        let email = self
            .store
            .invitation_email(invitation_token, now)
            .ok_or(FlowError::InvitationRejected)?
            .to_string();
        let (code, challenge) = codes::mint(
            &self.code_key,
            PURPOSE_REGISTER,
            &email,
            now,
            DEFAULT_CODE_TTL,
        )
        .map_err(|e| FlowError::Store(AuthError::Io(e)))?;
        self.mailer.send(&Message {
            to: email.clone(),
            subject: format!("{} — your verification code", self.display_name),
            body: format!(
                "Your {} verification code is {}.\n\nIt is good for ten minutes and five attempts. If \
                 you did not ask to register, nothing has happened to any account — you can ignore \
                 this.\n",
                self.display_name,
                code.expose()
            ),
        })?;
        self.open(
            Kind::Registration {
                invitation: invitation_token.to_string(),
            },
            email,
            Step::AwaitingCode(challenge),
            now,
        )
    }

    /// Prove the mailed code. Works for both registration and recovery's mail half.
    pub fn submit_code(
        &mut self,
        transaction: &str,
        code: &str,
        now: Timestamp,
    ) -> Result<(), FlowError> {
        let key = &self.code_key;
        let tx = live(&mut self.transactions, transaction, now)?;
        let Step::AwaitingCode(challenge) = &mut tx.step else {
            return Err(FlowError::WrongStep);
        };
        match codes::verify(challenge, key, &tx.email, code, now) {
            CodeOutcome::Accepted => {}
            // Wrong, expired or exhausted — all one answer outward. The transaction stays open on a
            // plain rejection so the user may try again; the code's own attempt counter is what ends
            // it, and it is terminal once spent.
            _ => return Err(FlowError::CodeRejected),
        }
        match &tx.kind {
            // Registration needs only the mailed code before the passkey. Recovery needs the secret
            // as well, so the code alone must not advance it — `submit_recovery_proofs` is the only
            // way a recovery reaches `Proved`.
            Kind::Registration { .. } => {
                tx.step = Step::Proved;
                Ok(())
            }
            Kind::Recovery { .. } => Err(FlowError::WrongStep),
        }
    }

    /// Begin the passkey ceremony for a transaction that has proved what it needs to.
    pub fn start_passkey(
        &mut self,
        transaction: &str,
        browser_binding: &str,
        now: Timestamp,
    ) -> Result<Started, FlowError> {
        let tx = live(&mut self.transactions, transaction, now)?;
        if !matches!(tx.step, Step::Proved) {
            return Err(FlowError::WrongStep);
        }
        let (account, email, existing) = match &tx.kind {
            // A registration has no account yet, so the ceremony runs against the id the account is
            // about to be created with. It is minted here rather than at finish because the WebAuthn
            // user handle is signed over by the authenticator — it cannot be decided afterwards — and
            // it is remembered on the transaction so `finish_registration` creates the account with
            // THAT id rather than a fresh one.
            Kind::Registration { .. } => (pending_account_id()?, tx.email.clone(), Vec::new()),
            Kind::Recovery { account } => (*account, tx.email.clone(), Vec::new()),
        };
        let started = self.ceremonies.start_registration(
            account,
            &email,
            &email,
            &existing,
            browser_binding,
            now,
        )?;
        let id = started.challenge_id.expose().to_string();
        if let Some(tx) = self.transactions.get_mut(transaction) {
            tx.step = Step::PasskeyOpen(id);
            tx.pending_account = Some(account);
        }
        Ok(started)
    }

    /// Finish a registration: consume the invitation, create the account, store the passkey, mint a
    /// session, and hand back the recovery secret to show exactly once.
    ///
    /// The invitation is consumed **first**. Two registrations that both validated the same
    /// invitation race here, and the loser fails before anything of its own exists.
    pub fn finish_registration(
        &mut self,
        transaction: &str,
        browser_binding: &str,
        response_json: &str,
        now: Timestamp,
    ) -> Result<Registered, FlowError> {
        let tx = self
            .transactions
            .remove(transaction)
            .ok_or(FlowError::UnknownTransaction)?;
        if tx.expires_at <= now {
            return Err(FlowError::Expired);
        }
        let (Kind::Registration { invitation }, Step::PasskeyOpen(ceremony)) = (&tx.kind, &tx.step)
        else {
            return Err(FlowError::WrongStep);
        };
        let credential =
            self.ceremonies
                .finish_registration(ceremony, browser_binding, response_json, now)?;
        let redeemed = self
            .store
            .redeem_invitation(invitation, now)
            .map_err(|_| FlowError::InvitationRejected)?;
        // The id the ceremony ran with, never a fresh one: the authenticator signed over it.
        let account = self.store.create_account_with_id(
            tx.pending_account.ok_or(FlowError::WrongStep)?,
            &redeemed.email,
            now,
        )?;
        self.store.add_credential(
            account,
            &credential.credential_id,
            "first passkey",
            &credential.blob,
            now,
        )?;
        let secret = mint_recovery_secret()?;
        self.store.set_recovery_secret(account, &secret, now)?;
        let session = self
            .store
            .create_session(account, DEFAULT_SESSION_TTL, now)?;
        self.announce(
            &redeemed.email,
            "a passkey was added",
            "A passkey was added to your account.",
        );
        Ok(Registered {
            account,
            session,
            recovery_secret: secret,
        })
    }

    // ── Login ─────────────────────────────────────────────────────────────────────────────────

    /// Begin a login. A passkey assertion is the whole of it — no code, no mail, no second factor to
    /// phish.
    pub fn start_login(
        &mut self,
        email: &str,
        browser_binding: &str,
        now: Timestamp,
    ) -> Result<Started, FlowError> {
        let (account, blobs) = self.active_credentials(email)?;
        Ok(self
            .ceremonies
            .start_authentication(account, &blobs, browser_binding, now)?)
    }

    /// Finish a login, writing back the authenticator's counter if it moved, and mint the session.
    /// The account is read off the ceremony rather than taken as an argument: a caller that named the
    /// account here could hand the write-back a different account's credentials, and an argument that
    /// only has to be *consistent* is one that will eventually be inconsistent.
    pub fn finish_login(
        &mut self,
        challenge_id: &str,
        browser_binding: &str,
        response_json: &str,
        now: Timestamp,
    ) -> Result<LoggedIn, FlowError> {
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
            browser_binding,
            response_json,
            &blobs,
            now,
        )?;
        if let Some(blob) = who.refreshed_blob {
            self.store
                .update_credential_blob(&who.credential_id, &blob, now)?;
        }
        let session = self
            .store
            .create_session(who.account, DEFAULT_SESSION_TTL, now)?;
        Ok(LoggedIn {
            account: who.account,
            session,
        })
    }

    // ── Recovery ──────────────────────────────────────────────────────────────────────────────

    /// Begin recovery for an account whose passkeys are all gone. Mails a code; proves nothing yet.
    pub fn begin_recovery(
        &mut self,
        email: &str,
        now: Timestamp,
    ) -> Result<TransactionId, FlowError> {
        self.expire(now);
        if self.transactions.len() >= MAX_TRANSACTIONS {
            return Err(FlowError::TooManyTransactions);
        }
        let account = self
            .store
            .account_by_email(email)
            .filter(|a| a.is_active())
            .ok_or(FlowError::NoSuchAccount)?;
        let (id, address) = (account.id, account.email.clone());
        let (code, challenge) = codes::mint(
            &self.code_key,
            PURPOSE_RECOVER,
            &address,
            now,
            DEFAULT_CODE_TTL,
        )
        .map_err(|e| FlowError::Store(AuthError::Io(e)))?;
        self.mailer.send(&Message {
            to: address.clone(),
            subject: format!("{} — account recovery", self.display_name),
            body: format!(
                "Your {} recovery code is {}.\n\nRecovery ALSO needs the recovery secret you were \
                 shown when you registered. This code on its own cannot change anything about your \
                 account, so if you did not start a recovery you can ignore this.\n",
                self.display_name,
                code.expose()
            ),
        })?;
        self.open(
            Kind::Recovery { account: id },
            address,
            Step::AwaitingCode(challenge),
            now,
        )
    }

    /// Prove **both** halves of recovery at once: the recovery secret and the mailed code.
    ///
    /// Deliberately one call. Two calls would mean a state in which one proof is banked and the other
    /// is not, and a state like that is the one an attacker works on — the secret alone would become a
    /// thing worth stealing on its own schedule.
    pub fn submit_recovery_proofs(
        &mut self,
        transaction: &str,
        recovery_secret: &str,
        code: &str,
        now: Timestamp,
    ) -> Result<(), FlowError> {
        let account = {
            let tx = live(&mut self.transactions, transaction, now)?;
            let Kind::Recovery { account } = tx.kind else {
                return Err(FlowError::WrongStep);
            };
            account
        };
        // The secret first, and its failure does not consume a code attempt: a wrong secret says
        // nothing about the mailbox, and burning a mailed attempt for it would let anyone exhaust
        // somebody's code by guessing the other proof.
        if !self.store.check_recovery_secret(account, recovery_secret) {
            return Err(FlowError::RecoveryRefused);
        }
        let key = &self.code_key;
        let tx = live(&mut self.transactions, transaction, now)?;
        let Step::AwaitingCode(challenge) = &mut tx.step else {
            return Err(FlowError::WrongStep);
        };
        match codes::verify(challenge, key, &tx.email, code, now) {
            CodeOutcome::Accepted => {
                tx.step = Step::Proved;
                Ok(())
            }
            _ => Err(FlowError::CodeRejected),
        }
    }

    /// Finish recovery: register the replacement passkey, then revoke every older credential, every
    /// session, and the recovery secret that was just used.
    ///
    /// Order matters. The replacement is created **before** anything is revoked, so a failure at the
    /// last step leaves the user with the account they had rather than one with no way in at all.
    pub fn finish_recovery(
        &mut self,
        transaction: &str,
        browser_binding: &str,
        response_json: &str,
        now: Timestamp,
    ) -> Result<Recovered, FlowError> {
        let tx = self
            .transactions
            .remove(transaction)
            .ok_or(FlowError::UnknownTransaction)?;
        if tx.expires_at <= now {
            return Err(FlowError::Expired);
        }
        let (Kind::Recovery { account }, Step::PasskeyOpen(ceremony)) = (&tx.kind, &tx.step) else {
            return Err(FlowError::WrongStep);
        };
        let account = *account;
        let credential =
            self.ceremonies
                .finish_registration(ceremony, browser_binding, response_json, now)?;
        let old: Vec<Vec<u8>> = self
            .store
            .active_credentials(account)
            .iter()
            .map(|c| c.credential_id.clone())
            .collect();
        self.store.add_credential(
            account,
            &credential.credential_id,
            "recovery passkey",
            &credential.blob,
            now,
        )?;
        let mut revoked = 0;
        for id in &old {
            if self.store.revoke_credential(id, now)? {
                revoked += 1;
            }
        }
        self.store.revoke_all_sessions(account, now)?;
        let secret = mint_recovery_secret()?;
        self.store.set_recovery_secret(account, &secret, now)?;
        let session = self
            .store
            .create_session(account, DEFAULT_SESSION_TTL, now)?;
        self.announce(
            &tx.email,
            "your account was recovered",
            "A recovery completed on your account: a new passkey was added, and every passkey and \
             session that existed before it was revoked. If this was not you, reply to this mail.",
        );
        Ok(Recovered {
            account,
            session,
            recovery_secret: secret,
            credentials_revoked: revoked,
        })
    }

    // ── Plumbing ──────────────────────────────────────────────────────────────────────────────

    /// Drop transactions whose deadline passed. Called at every entry point that can create one.
    pub fn expire(&mut self, now: Timestamp) {
        self.transactions.retain(|_, t| t.expires_at > now);
        self.pairings.retain(|_, p| p.expires_at > now);
        self.ceremonies.expire(now);
    }

    fn open(
        &mut self,
        kind: Kind,
        email: String,
        step: Step,
        now: Timestamp,
    ) -> Result<TransactionId, FlowError> {
        let id = TransactionId(random_hex()?);
        self.transactions.insert(
            id.0.clone(),
            Transaction {
                kind,
                email,
                step,
                expires_at: now + TRANSACTION_TTL.as_secs(),
                pending_account: None,
            },
        );
        Ok(id)
    }

    fn active_credentials(&self, email: &str) -> Result<(AccountId, Vec<Vec<u8>>), FlowError> {
        let account = self
            .store
            .account_by_email(email)
            .filter(|a| a.is_active())
            .ok_or(FlowError::NoSuchAccount)?;
        let id = account.id;
        Ok((
            id,
            self.store
                .active_credentials(id)
                .iter()
                .map(|c| c.blob.clone())
                .collect(),
        ))
    }

    /// Tell the account holder that something changed. **Never a hard failure**: an account that has
    /// just been recovered must not be left half-recovered because a relay was down, and the change
    /// is already in the log whether or not the mail lands.
    fn announce(&self, to: &str, subject: &str, body: &str) {
        let _ = self.mailer.send(&Message {
            to: to.to_string(),
            subject: format!("{} — {subject}", self.display_name),
            body: format!("{body}\n"),
        });
    }
}

/// Find a transaction that is still alive, or say which way it is not.
fn live<'a>(
    map: &'a mut HashMap<String, Transaction>,
    id: &str,
    now: Timestamp,
) -> Result<&'a mut Transaction, FlowError> {
    let tx = map.get_mut(id).ok_or(FlowError::UnknownTransaction)?;
    if tx.expires_at <= now {
        return Err(FlowError::Expired);
    }
    Ok(tx)
}

/// The account id a registration's ceremony will use, minted before the account exists because the
/// WebAuthn user handle is signed over by the authenticator and cannot be chosen afterwards.
fn pending_account_id() -> Result<AccountId, FlowError> {
    let mut bytes = [0u8; 16];
    fill_random(&mut bytes).map_err(|e| FlowError::Store(AuthError::Io(e)))?;
    let hex = to_hex(&bytes);
    hex.parse::<AccountId>()
        .map_err(|_| FlowError::Store(AuthError::Io(std::io::Error::other("unrepresentable id"))))
}

fn mint_recovery_secret() -> Result<String, FlowError> {
    let mut bytes = [0u8; RECOVERY_SECRET_BYTES];
    fill_random(&mut bytes).map_err(|e| FlowError::Store(AuthError::Io(e)))?;
    Ok(to_hex(&bytes))
}

fn random_hex() -> Result<String, FlowError> {
    let mut bytes = [0u8; 16];
    fill_random(&mut bytes).map_err(|e| FlowError::Store(AuthError::Io(e)))?;
    Ok(to_hex(&bytes))
}

#[cfg(test)]
#[cfg(unix)]
mod tests {
    use super::*;
    use crate::mail::CapturingMailer;
    use crate::DEFAULT_INVITE_TTL;
    use std::sync::Arc;
    use webauthn_authenticator_rs::softpasskey::SoftPasskey;
    use webauthn_authenticator_rs::WebauthnAuthenticator;
    use webauthn_rs::prelude::{CreationChallengeResponse, RequestChallengeResponse, Url};

    pub(super) const ORIGIN: &str = "https://rime.example";
    pub(super) const NOW: Timestamp = 1_700_000_000;
    pub(super) const INVITED: &str = "claire@example.test";

    /// A `Mailer` that both the test and the `Auth` can see. `Auth` takes a `Box<dyn Mailer>`, so the
    /// shared side is an `Arc` the box forwards to.
    struct Shared(Arc<CapturingMailer>);
    impl Mailer for Shared {
        fn send(&self, message: &Message) -> Result<(), MailError> {
            self.0.send(message)
        }
    }

    fn temp_store() -> AuthStore {
        // A counter, not just the thread id: a ThreadId may be reused once its thread has exited, and
        // a test that silently shares another's store file is a test that passes for the wrong reason.
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let mut path = std::env::temp_dir();
        path.push(format!(
            "rime-auth-flow-{}-{}.log",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        let _ = std::fs::remove_file(&path);
        AuthStore::open(&path).expect("a fresh store")
    }

    pub(super) fn auth() -> (Auth, Arc<CapturingMailer>) {
        let mailer = Arc::new(CapturingMailer::default());
        let config = CeremonyConfig {
            rp_id: "rime.example".to_string(),
            rp_origin: ORIGIN.to_string(),
            rp_name: "Rime".to_string(),
        };
        let auth = Auth::new(temp_store(), &config, Box::new(Shared(Arc::clone(&mailer))))
            .expect("auth builds");
        (auth, mailer)
    }

    /// Pull the eight digits back out of whatever was mailed last. The test plays the mailbox.
    fn mailed_code(mailer: &CapturingMailer) -> String {
        let sent = mailer.sent();
        let body = sent.last().expect("something was mailed").body.clone();
        body.chars()
            .collect::<Vec<_>>()
            .windows(8)
            .find(|w| w.iter().all(char::is_ascii_digit))
            .map(|w| w.iter().collect())
            .expect("a code in the body")
    }

    fn do_passkey(
        auth: &mut Auth,
        device: &mut WebauthnAuthenticator<SoftPasskey>,
        tx: &str,
        now: Timestamp,
    ) -> String {
        let started = auth
            .start_passkey(tx, "browser", now)
            .expect("ceremony starts");
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let response = device
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .expect("the device makes a credential");
        serde_json::to_string(&response).unwrap()
    }

    fn invite(auth: &mut Auth) -> String {
        auth.store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .expect("an invitation")
            .expose()
            .to_string()
    }

    pub(super) fn register(
        auth: &mut Auth,
        mailer: &CapturingMailer,
    ) -> (Registered, WebauthnAuthenticator<SoftPasskey>) {
        let token = invite(auth);
        let tx = auth
            .begin_registration(&token, NOW)
            .expect("registration begins");
        let code = mailed_code(mailer);
        auth.submit_code(tx.expose(), &code, NOW + 1)
            .expect("the code is accepted");
        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let response = do_passkey(auth, &mut device, tx.expose(), NOW + 2);
        let done = auth
            .finish_registration(tx.expose(), "browser", &response, NOW + 3)
            .expect("registration completes");
        (done, device)
    }

    #[test]
    fn a_registration_goes_invitation_then_code_then_passkey() {
        let (mut auth, mailer) = auth();
        let (done, _device) = register(&mut auth, &mailer);
        // The code went to the address the INVITATION names — the caller never supplied one.
        assert_eq!(mailer.sent().first().expect("a code mail").to, INVITED);
        assert_eq!(done.recovery_secret.len(), 64);
        assert_eq!(
            auth.store_mut()
                .authenticate_session(done.session.expose(), NOW + 4),
            Some(done.account)
        );
        assert_eq!(auth.store().active_credentials(done.account).len(), 1);
    }

    // The account id is the WebAuthn user handle, and the authenticator SIGNS over it. So the account
    // that gets created has to be the one the ceremony named — a fresh id at the end would leave every
    // credential carrying a handle that names nothing.
    #[test]
    fn the_account_is_created_with_the_id_the_authenticator_signed() {
        let (mut auth, mailer) = auth();
        let token = invite(&mut auth);
        let tx = auth.begin_registration(&token, NOW).unwrap();
        let code = mailed_code(&mailer);
        auth.submit_code(tx.expose(), &code, NOW + 1).unwrap();

        let started = auth.start_passkey(tx.expose(), "browser", NOW + 2).unwrap();
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let handle: Vec<u8> = options.public_key.user.id.as_ref().to_vec();

        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let response = device
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let done = auth
            .finish_registration(
                tx.expose(),
                "browser",
                &serde_json::to_string(&response).unwrap(),
                NOW + 3,
            )
            .unwrap();
        assert_eq!(handle, done.account.as_u128().to_be_bytes().to_vec());
    }

    #[test]
    fn the_passkey_step_is_unreachable_without_the_code() {
        let (mut auth, _mailer) = auth();
        let token = invite(&mut auth);
        let tx = auth.begin_registration(&token, NOW).unwrap();
        let skipped = auth.start_passkey(tx.expose(), "browser", NOW + 1);
        assert!(matches!(skipped, Err(FlowError::WrongStep)));
    }

    #[test]
    fn a_wrong_code_is_refused_and_the_fifth_one_ends_the_transaction() {
        let (mut auth, _mailer) = auth();
        let token = invite(&mut auth);
        let tx = auth.begin_registration(&token, NOW).unwrap();
        for _ in 0..5 {
            assert!(matches!(
                auth.submit_code(tx.expose(), "00000000", NOW + 1),
                Err(FlowError::CodeRejected)
            ));
        }
        // Even the right code cannot rescue an exhausted challenge.
        assert!(matches!(
            auth.submit_code(tx.expose(), "00000000", NOW + 1),
            Err(FlowError::CodeRejected)
        ));
    }

    #[test]
    fn an_invitation_is_spent_only_by_a_completed_registration() {
        let (mut auth, mailer) = auth();
        let token = invite(&mut auth);
        let tx = auth.begin_registration(&token, NOW).unwrap();
        let code = mailed_code(&mailer);
        auth.submit_code(tx.expose(), &code, NOW + 1).unwrap();
        // Still unspent half way through: an abandoned registration must not burn an invitation.
        assert!(auth.store().invitation_email(&token, NOW + 1).is_some());
        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let response = do_passkey(&mut auth, &mut device, tx.expose(), NOW + 2);
        auth.finish_registration(tx.expose(), "browser", &response, NOW + 3)
            .unwrap();
        assert!(auth.store().invitation_email(&token, NOW + 4).is_none());
    }

    #[test]
    fn a_login_mails_nothing() {
        let (mut auth, mailer) = auth();
        let (done, mut device) = register(&mut auth, &mailer);
        let before = mailer.sent().len();

        let started = auth.start_login(INVITED, "browser", NOW + 10).unwrap();
        let options: RequestChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        let who = auth
            .finish_login(
                started.challenge_id.expose(),
                "browser",
                &serde_json::to_string(&assertion).unwrap(),
                NOW + 11,
            )
            .unwrap();
        assert_eq!(who.account, done.account);
        // ADR-0048 decision 1: mail can never log anybody in, so a login does not touch it at all.
        assert_eq!(mailer.sent().len(), before);
    }

    #[test]
    fn recovery_refuses_each_proof_on_its_own() {
        let (mut auth, mailer) = auth();
        let (done, _device) = register(&mut auth, &mailer);

        let tx = auth.begin_recovery(INVITED, NOW + 20).unwrap();
        let code = mailed_code(&mailer);
        // The mailed code alone is not a way through: `submit_code` refuses to advance a recovery.
        assert!(matches!(
            auth.submit_code(tx.expose(), &code, NOW + 21),
            Err(FlowError::WrongStep)
        ));
        // The secret alone is not either.
        assert!(matches!(
            auth.submit_recovery_proofs(tx.expose(), &done.recovery_secret, "00000000", NOW + 21),
            Err(FlowError::CodeRejected)
        ));
        // And a wrong secret with the RIGHT code is still a refusal.
        assert!(matches!(
            auth.submit_recovery_proofs(tx.expose(), &"0".repeat(64), &code, NOW + 21),
            Err(FlowError::RecoveryRefused)
        ));
    }

    #[test]
    fn a_completed_recovery_revokes_the_old_passkeys_sessions_and_secret() {
        let (mut auth, mailer) = auth();
        let (done, _lost_device) = register(&mut auth, &mailer);
        let old_credential = auth.store().active_credentials(done.account)[0]
            .credential_id
            .clone();

        let tx = auth.begin_recovery(INVITED, NOW + 20).unwrap();
        let code = mailed_code(&mailer);
        auth.submit_recovery_proofs(tx.expose(), &done.recovery_secret, &code, NOW + 21)
            .expect("both proofs");
        let mut replacement = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let response = do_passkey(&mut auth, &mut replacement, tx.expose(), NOW + 22);
        let recovered = auth
            .finish_recovery(tx.expose(), "browser", &response, NOW + 23)
            .expect("recovery completes");

        assert_eq!(recovered.account, done.account);
        assert_eq!(recovered.credentials_revoked, 1);
        // Exactly one credential is usable, and it is not the one that was lost.
        let active = auth.store().active_credentials(done.account);
        assert_eq!(active.len(), 1);
        assert_ne!(active[0].credential_id, old_credential);
        // The session minted at registration is gone.
        assert_eq!(
            auth.store_mut()
                .authenticate_session(done.session.expose(), NOW + 24),
            None
        );
        // And the secret that completed this recovery cannot complete another.
        assert!(!auth
            .store()
            .check_recovery_secret(done.account, &done.recovery_secret));
        assert!(auth
            .store()
            .check_recovery_secret(done.account, &recovered.recovery_secret));
    }

    #[test]
    fn a_transaction_expires() {
        let (mut auth, _mailer) = auth();
        let token = invite(&mut auth);
        let tx = auth.begin_registration(&token, NOW).unwrap();
        let late = NOW + TRANSACTION_TTL.as_secs() + 1;
        assert!(matches!(
            auth.submit_code(tx.expose(), "00000000", late),
            Err(FlowError::Expired)
        ));
        auth.expire(late);
        assert_eq!(auth.transactions(), 0);
    }
}
