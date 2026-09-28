// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Accounts, invitations and sessions for the hosted front end — [ADR-0048](../../docs/adr/0048-authenticating-the-hosted-front-end.md).
//!
//! This is **brick 1 of three**: the store. The ceremonies (passkey registration and login through
//! `webauthn-rs`, mailed codes through the estate's Resend relay) are brick 2, and the gate
//! (`owner_account_id` through admission, ownership on every route) is brick 3. Nothing here
//! authenticates anybody yet; it is the durable state those bricks stand on.
//!
//! ## Why an append-only log and not SQLite
//!
//! The consult proposed `rusqlite` with `bundled`. ADR-0048 decision 3 declined it, and the reasoning
//! is worth having next to the code: what this store must do is hold a handful of accounts, redeem an
//! invitation exactly once, revoke things, and survive power loss. What it does not need is a query
//! planner, joins, concurrent writers or schema migrations — there is one process, one writer, and the
//! largest table will have tens of rows. `bundled` SQLite would add a C build to three CI platforms and
//! a second licence to audit in exchange for capabilities this data model does not use.
//!
//! **Crash-safety comes out by construction rather than by care.** Every record is one line ending in
//! a newline, prefixed by a CRC32 of the rest of the line. A power loss mid-write leaves a final line
//! that is short, or whose CRC does not match; replay stops at the first such line and the records
//! before it are intact. There is no partially-applied state to reason about because there is no
//! in-place mutation at all — even a revocation is an appended record.
//!
//! ## Why tokens are stored hashed
//!
//! An invitation token and a session token are **bearer credentials**: whoever holds one is the
//! account. Storing them in the clear would mean a readable store file is a set of live credentials.
//! So the store keeps `SHA-256(token)` and compares hashes, and the plaintext exists exactly once —
//! in the return value of the call that mints it, for handing to the person it belongs to.
//!
//! ## What this deliberately does not do
//!
//! No passwords, ever. No email-as-login: ADR-0048 decision 1 rules that mail can deliver an
//! invitation and a verification code but can never authenticate an existing account, because a code
//! that logs you in makes the passkey decorative and reduces the account to the security of a mailbox.

use std::collections::HashMap;
use std::fmt;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use sha2::{Digest, Sha256};

pub mod ceremony;
pub mod codes;
pub mod mail;

/// How long an invitation is good for unless the caller says otherwise. Seven days: long enough to
/// reach somebody who reads mail weekly, short enough that a leaked mailbox is not a permanent way in.
pub const DEFAULT_INVITE_TTL: Duration = Duration::from_secs(7 * 24 * 60 * 60);

/// A user session's absolute lifetime. Absolute rather than sliding, because a sliding expiry on a
/// stolen token renews itself forever as long as the thief keeps using it.
pub const DEFAULT_SESSION_TTL: Duration = Duration::from_secs(7 * 24 * 60 * 60);

/// Bytes of entropy in a minted token. 32 bytes = 256 bits, the same "an id is a capability"
/// reasoning as the gateway's `SessionId`, one size up because these are longer-lived.
const TOKEN_BYTES: usize = 32;

/// Unix seconds. A plain integer rather than a `SystemTime` in the records, so the log stays readable
/// and a clock change cannot make an old record unparseable.
pub type Timestamp = u64;

/// Seconds since the epoch, now.
#[must_use]
pub fn now() -> Timestamp {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// An account id. Random, not sequential — the same capability reasoning as everything else here, and
/// it is also the WebAuthn user handle, which must not leak anything about the user.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct AccountId(u128);

impl AccountId {
    /// The raw id. The WebAuthn user handle is this value (`ceremony.rs`), which is the reason it is
    /// readable at all — nothing else needs it, and nothing else should use it as a key.
    pub fn as_u128(self) -> u128 {
        self.0
    }
}

impl fmt::Display for AccountId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:032x}", self.0)
    }
}

impl std::str::FromStr for AccountId {
    type Err = MalformedId;
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        if text.len() != 32 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(MalformedId);
        }
        u128::from_str_radix(text, 16)
            .map(Self)
            .map_err(|_| MalformedId)
    }
}

/// An id was not 32 hex digits. One spelling per id, for the same reason the gateway's `SessionId` has
/// one: an id with two spellings is one an audit log cannot count.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MalformedId;

impl fmt::Display for MalformedId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "an id is 32 hexadecimal digits")
    }
}

impl std::error::Error for MalformedId {}

/// A freshly minted bearer token, in the clear.
///
/// **This type exists to make the plaintext's lifetime visible.** It is returned once, by the call
/// that mints it, and the store keeps only its hash — so if a caller drops this value without
/// delivering it, the credential is unrecoverable by design rather than by accident.
#[derive(Clone, PartialEq, Eq)]
pub struct Token(String);

impl Token {
    /// The string to put in a mail or a cookie.
    #[must_use]
    pub fn expose(&self) -> &str {
        &self.0
    }
}

// Debug is DELIBERATELY redacted. A bearer token that reaches a log because something printed a struct
// containing it is the most ordinary way secrets leak, and `{:?}` is how it happens.
impl fmt::Debug for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Token(<redacted>)")
    }
}

/// An account as the store holds it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Account {
    pub id: AccountId,
    pub email: String,
    pub created_at: Timestamp,
    pub disabled_at: Option<Timestamp>,
}

impl Account {
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.disabled_at.is_none()
    }
}

/// A passkey, stored as the opaque blob its library produced.
///
/// **Never reconstructed field by field.** The blob carries the public key, the algorithm, the signature
/// counter and the backup state, and a store that took those apart and put them back together would be
/// a second implementation of a security-critical format. Brick 2 fills this in; brick 1 only has to
/// keep bytes safely.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Credential {
    pub credential_id: Vec<u8>,
    pub account_id: AccountId,
    pub label: String,
    pub created_at: Timestamp,
    /// When this credential last completed an authentication, if it ever has. Needed to answer "which
    /// of my passkeys is the one I lost?" before revoking it.
    pub last_used_at: Option<Timestamp>,
    pub revoked_at: Option<Timestamp>,
    pub blob: Vec<u8>,
}

/// What redeeming an invitation produced.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RedeemedInvitation {
    /// The address the invitation was issued to. Registration must use this and not an address the
    /// browser supplied, or the invitation gates nothing.
    pub email: String,
}

/// Why an operation failed.
#[derive(Debug)]
pub enum AuthError {
    Io(std::io::Error),
    /// The log contained a line this build cannot parse. Distinct from a torn tail, which is normal:
    /// a bad line in the MIDDLE means the file was edited or corrupted and is not safe to append to.
    Corrupt {
        line: usize,
        why: String,
    },
    /// A value contained a tab or a newline, which the record format cannot represent.
    UnrepresentableValue(&'static str),
    /// No such invitation, or it was already consumed, or it expired. **One variant on purpose** — see
    /// [`AuthStore::redeem_invitation`].
    InvitationRejected,
    /// The email already has an account.
    EmailTaken,
    /// No such account.
    NoSuchAccount,
}

impl fmt::Display for AuthError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AuthError::Io(e) => write!(f, "auth store i/o: {e}"),
            AuthError::Corrupt { line, why } => {
                write!(f, "auth store is corrupt at line {line}: {why}")
            }
            AuthError::UnrepresentableValue(field) => {
                write!(f, "the {field} field contains a tab or newline")
            }
            AuthError::InvitationRejected => write!(f, "that invitation is not valid"),
            AuthError::EmailTaken => write!(f, "that address already has an account"),
            AuthError::NoSuchAccount => write!(f, "no such account"),
        }
    }
}

impl std::error::Error for AuthError {}

/// Every decision the store made. The counter rule, applied to authentication: a store that is
/// refusing everything must be able to say which refusal, and "nobody can log in" is exactly the
/// report that arrives with nothing else attached.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct AuthCounters {
    pub invitations_issued: u64,
    pub invitations_redeemed: u64,
    pub invitations_rejected_unknown: u64,
    pub invitations_rejected_consumed: u64,
    pub invitations_rejected_expired: u64,
    pub accounts_created: u64,
    pub credentials_added: u64,
    pub credentials_revoked: u64,
    /// Blob write-backs after a login moved an authenticator's counter or backup state.
    pub credentials_updated: u64,
    pub sessions_created: u64,
    pub sessions_authenticated: u64,
    pub sessions_rejected_unknown: u64,
    pub sessions_rejected_expired: u64,
    pub sessions_rejected_revoked: u64,
    pub sessions_revoked: u64,
    /// Records skipped at the end of the log because their CRC did not match — a torn write from a
    /// power loss. Expected to be 0 or 1, never more; a larger number means something else is wrong.
    pub torn_records_discarded: u64,
}

/// The durable store.
///
/// One writer, one file. Not `Sync`-safe by itself on purpose: the caller owns the mutex, because the
/// caller is the one that knows a redemption and an account creation have to be one atomic step.
pub struct AuthStore {
    path: PathBuf,
    log: File,
    accounts: HashMap<AccountId, Account>,
    by_email: HashMap<String, AccountId>,
    credentials: Vec<Credential>,
    invitations: HashMap<String, Invitation>,
    sessions: HashMap<String, StoredSession>,
    counters: AuthCounters,
}

#[derive(Debug, Clone)]
struct Invitation {
    email: String,
    expires_at: Timestamp,
    consumed_at: Option<Timestamp>,
}

#[derive(Debug, Clone)]
struct StoredSession {
    account_id: AccountId,
    expires_at: Timestamp,
    revoked_at: Option<Timestamp>,
}

// Manual, and REDACTED like `Token`'s. A derived `Debug` would print every address, every account id
// and every session hash the moment anything formatted the store — which is how a `{:?}` in an error
// path turns a private database into a log file. Counts answer the questions a debugger actually asks.
impl fmt::Debug for AuthStore {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("AuthStore")
            .field("path", &self.path)
            .field("accounts", &self.accounts.len())
            .field("credentials", &self.credentials.len())
            .field("invitations", &self.invitations.len())
            .field("sessions", &self.sessions.len())
            .field("counters", &self.counters)
            .finish()
    }
}

impl AuthStore {
    /// Open or create the store, replaying the log.
    ///
    /// The file is created `0600` where the platform allows it: the store holds token hashes and
    /// addresses, and a world-readable auth database is the estate's own stated failure mode (its
    /// secret helper "refuses world-readable secret files").
    pub fn open(path: impl AsRef<Path>) -> Result<Self, AuthError> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                std::fs::create_dir_all(parent).map_err(AuthError::Io)?;
            }
        }
        let mut options = OpenOptions::new();
        options.read(true).append(true).create(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let log = options.open(&path).map_err(AuthError::Io)?;

        let mut store = Self {
            path,
            log,
            accounts: HashMap::new(),
            by_email: HashMap::new(),
            credentials: Vec::new(),
            invitations: HashMap::new(),
            sessions: HashMap::new(),
            counters: AuthCounters::default(),
        };
        store.replay()?;
        Ok(store)
    }

    #[must_use]
    pub fn counters(&self) -> AuthCounters {
        self.counters
    }

    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    #[must_use]
    pub fn account_count(&self) -> usize {
        self.accounts.len()
    }

    #[must_use]
    pub fn account(&self, id: AccountId) -> Option<&Account> {
        self.accounts.get(&id)
    }

    #[must_use]
    pub fn account_by_email(&self, email: &str) -> Option<&Account> {
        self.by_email
            .get(&normalise_email(email))
            .and_then(|id| self.accounts.get(id))
    }

    /// Credentials belonging to an account that have not been revoked.
    #[must_use]
    pub fn active_credentials(&self, account: AccountId) -> Vec<&Credential> {
        self.credentials
            .iter()
            .filter(|c| c.account_id == account && c.revoked_at.is_none())
            .collect()
    }

    /// Issue an invitation for `email`, returning the token **once**.
    ///
    /// Email-bound on purpose: an invitation that any address could redeem is a bearer token for
    /// "an account here", and forwarding one mail would hand that to somebody else.
    pub fn issue_invitation(
        &mut self,
        email: &str,
        ttl: Duration,
        now_ts: Timestamp,
    ) -> Result<Token, AuthError> {
        let email = normalise_email(email);
        check_representable(&email, "email")?;
        let token = mint_token()?;
        let hash = hash_token(token.expose());
        let expires_at = now_ts.saturating_add(ttl.as_secs());
        self.append(&["invite", &hash, &email, &expires_at.to_string()])?;
        self.invitations.insert(
            hash,
            Invitation {
                email,
                expires_at,
                consumed_at: None,
            },
        );
        self.counters.invitations_issued += 1;
        Ok(token)
    }

    /// Redeem an invitation, consuming it.
    ///
    /// **Every failure is one variant, and that is a security decision rather than laziness.** Telling
    /// a caller apart "no such invitation" from "already used" from "expired" hands an attacker an
    /// oracle for probing the token space. The three cases are distinguished in the COUNTERS, where the
    /// operator can see them and a client cannot.
    ///
    /// The consume record is appended **before** the caller is told it succeeded, so a crash between
    /// the two cannot produce an invitation that was used and still looks unused.
    pub fn redeem_invitation(
        &mut self,
        token: &str,
        now_ts: Timestamp,
    ) -> Result<RedeemedInvitation, AuthError> {
        let hash = hash_token(token);
        let invitation = match self.invitations.get(&hash) {
            Some(i) => i.clone(),
            None => {
                self.counters.invitations_rejected_unknown += 1;
                return Err(AuthError::InvitationRejected);
            }
        };
        if invitation.consumed_at.is_some() {
            self.counters.invitations_rejected_consumed += 1;
            return Err(AuthError::InvitationRejected);
        }
        if invitation.expires_at <= now_ts {
            self.counters.invitations_rejected_expired += 1;
            return Err(AuthError::InvitationRejected);
        }
        self.append(&["invite-used", &hash, &now_ts.to_string()])?;
        if let Some(i) = self.invitations.get_mut(&hash) {
            i.consumed_at = Some(now_ts);
        }
        self.counters.invitations_redeemed += 1;
        Ok(RedeemedInvitation {
            email: invitation.email,
        })
    }

    /// Create an account for an address that does not have one.
    pub fn create_account(
        &mut self,
        email: &str,
        now_ts: Timestamp,
    ) -> Result<AccountId, AuthError> {
        let email = normalise_email(email);
        check_representable(&email, "email")?;
        if self.by_email.contains_key(&email) {
            return Err(AuthError::EmailTaken);
        }
        let id = AccountId(mint_u128()?);
        self.append(&["acct", &id.to_string(), &email, &now_ts.to_string()])?;
        self.accounts.insert(
            id,
            Account {
                id,
                email: email.clone(),
                created_at: now_ts,
                disabled_at: None,
            },
        );
        self.by_email.insert(email, id);
        self.counters.accounts_created += 1;
        Ok(id)
    }

    /// Attach a passkey to an account. `blob` is whatever the WebAuthn library produced; this store
    /// does not interpret it.
    pub fn add_credential(
        &mut self,
        account: AccountId,
        credential_id: &[u8],
        label: &str,
        blob: &[u8],
        now_ts: Timestamp,
    ) -> Result<(), AuthError> {
        if !self.accounts.contains_key(&account) {
            return Err(AuthError::NoSuchAccount);
        }
        check_representable(label, "label")?;
        self.append(&[
            "cred",
            &to_hex(credential_id),
            &account.to_string(),
            label,
            &now_ts.to_string(),
            &to_hex(blob),
        ])?;
        self.credentials.push(Credential {
            credential_id: credential_id.to_vec(),
            account_id: account,
            label: label.to_string(),
            created_at: now_ts,
            last_used_at: None,
            revoked_at: None,
            blob: blob.to_vec(),
        });
        self.counters.credentials_added += 1;
        Ok(())
    }

    /// Revoke a passkey. Appended, never deleted: the record that a credential existed and was revoked
    /// is exactly what an incident review needs, and an append-only log gives it for free.
    pub fn revoke_credential(
        &mut self,
        credential_id: &[u8],
        now_ts: Timestamp,
    ) -> Result<bool, AuthError> {
        let found = self
            .credentials
            .iter()
            .any(|c| c.credential_id == credential_id && c.revoked_at.is_none());
        if !found {
            return Ok(false);
        }
        self.append(&["cred-revoked", &to_hex(credential_id), &now_ts.to_string()])?;
        for c in &mut self.credentials {
            if c.credential_id == credential_id && c.revoked_at.is_none() {
                c.revoked_at = Some(now_ts);
            }
        }
        self.counters.credentials_revoked += 1;
        Ok(true)
    }

    /// Write back a credential's blob after a successful authentication.
    ///
    /// The blob carries the authenticator's signature counter and backup state, and a counter that is
    /// never written back cannot detect a cloned credential — which is the only thing the counter is
    /// for. So a login that moved it appends a record, and replay takes the last one.
    ///
    /// A revoked credential is not updated: a login against one should not have happened, and writing
    /// to it would quietly resurrect a row an incident review is reading as closed.
    pub fn update_credential_blob(
        &mut self,
        credential_id: &[u8],
        blob: &[u8],
        now_ts: Timestamp,
    ) -> Result<bool, AuthError> {
        let found = self
            .credentials
            .iter()
            .any(|c| c.credential_id == credential_id && c.revoked_at.is_none());
        if !found {
            return Ok(false);
        }
        self.append(&[
            "cred-used",
            &to_hex(credential_id),
            &now_ts.to_string(),
            &to_hex(blob),
        ])?;
        for c in &mut self.credentials {
            if c.credential_id == credential_id && c.revoked_at.is_none() {
                c.blob = blob.to_vec();
                c.last_used_at = Some(now_ts);
            }
        }
        self.counters.credentials_updated += 1;
        Ok(true)
    }

    /// Mint a user session for an account, returning the bearer token **once**.
    pub fn create_session(
        &mut self,
        account: AccountId,
        ttl: Duration,
        now_ts: Timestamp,
    ) -> Result<Token, AuthError> {
        if !self.accounts.contains_key(&account) {
            return Err(AuthError::NoSuchAccount);
        }
        let token = mint_token()?;
        let hash = hash_token(token.expose());
        let expires_at = now_ts.saturating_add(ttl.as_secs());
        self.append(&["sess", &hash, &account.to_string(), &expires_at.to_string()])?;
        self.sessions.insert(
            hash,
            StoredSession {
                account_id: account,
                expires_at,
                revoked_at: None,
            },
        );
        self.counters.sessions_created += 1;
        Ok(token)
    }

    /// Resolve a session token to its account, or refuse.
    ///
    /// `&mut self` only because it counts. The counting is the point: "my login keeps dropping" is
    /// otherwise unanswerable, and expired-versus-revoked-versus-unknown are three different stories.
    pub fn authenticate_session(&mut self, token: &str, now_ts: Timestamp) -> Option<AccountId> {
        let hash = hash_token(token);
        let session = match self.sessions.get(&hash) {
            Some(s) => s.clone(),
            None => {
                self.counters.sessions_rejected_unknown += 1;
                return None;
            }
        };
        if session.revoked_at.is_some() {
            self.counters.sessions_rejected_revoked += 1;
            return None;
        }
        if session.expires_at <= now_ts {
            self.counters.sessions_rejected_expired += 1;
            return None;
        }
        // A disabled account's live sessions must stop working immediately, not at expiry — otherwise
        // "disable this account" is a request that takes up to a week to take effect.
        match self.accounts.get(&session.account_id) {
            Some(a) if a.is_active() => {
                self.counters.sessions_authenticated += 1;
                Some(session.account_id)
            }
            _ => {
                self.counters.sessions_rejected_revoked += 1;
                None
            }
        }
    }

    /// Revoke one session by its token.
    pub fn revoke_session(&mut self, token: &str, now_ts: Timestamp) -> Result<bool, AuthError> {
        let hash = hash_token(token);
        match self.sessions.get(&hash) {
            Some(s) if s.revoked_at.is_none() => {}
            _ => return Ok(false),
        }
        self.append(&["sess-revoked", &hash, &now_ts.to_string()])?;
        if let Some(s) = self.sessions.get_mut(&hash) {
            s.revoked_at = Some(now_ts);
        }
        self.counters.sessions_revoked += 1;
        Ok(true)
    }

    /// Revoke every session of an account — what "lost device" and "recovery completed" both need.
    pub fn revoke_all_sessions(
        &mut self,
        account: AccountId,
        now_ts: Timestamp,
    ) -> Result<usize, AuthError> {
        let hashes: Vec<String> = self
            .sessions
            .iter()
            .filter(|(_, s)| s.account_id == account && s.revoked_at.is_none())
            .map(|(h, _)| h.clone())
            .collect();
        for hash in &hashes {
            self.append(&["sess-revoked", hash, &now_ts.to_string()])?;
            if let Some(s) = self.sessions.get_mut(hash) {
                s.revoked_at = Some(now_ts);
            }
            self.counters.sessions_revoked += 1;
        }
        Ok(hashes.len())
    }

    // ── The log ───────────────────────────────────────────────────────────────────────────────

    /// Append one record and flush it to disk before returning.
    ///
    /// `sync_all` on every append, deliberately. It is the difference between "the invitation is
    /// consumed" being true and being probably-true, and at this write rate — a handful of records per
    /// registration — the cost is irrelevant. A store that loses the last record on power loss loses
    /// exactly the record that says a single-use token was used.
    fn append(&mut self, fields: &[&str]) -> Result<(), AuthError> {
        let body = fields.join("\t");
        let line = format!("{:08x}\t{}\n", crc32(body.as_bytes()), body);
        self.log.write_all(line.as_bytes()).map_err(AuthError::Io)?;
        self.log.sync_all().map_err(AuthError::Io)?;
        Ok(())
    }

    fn replay(&mut self) -> Result<(), AuthError> {
        let file = File::open(&self.path).map_err(AuthError::Io)?;
        let reader = BufReader::new(file);
        let mut lines: Vec<String> = Vec::new();
        for line in reader.lines() {
            lines.push(line.map_err(AuthError::Io)?);
        }
        for (i, line) in lines.iter().enumerate() {
            if line.is_empty() {
                continue;
            }
            let Some((crc_text, body)) = line.split_once('\t') else {
                return self.torn_or_corrupt(i, lines.len(), "no field separator");
            };
            let Ok(expected) = u32::from_str_radix(crc_text, 16) else {
                return self.torn_or_corrupt(i, lines.len(), "unparseable checksum");
            };
            if crc32(body.as_bytes()) != expected {
                return self.torn_or_corrupt(i, lines.len(), "checksum mismatch");
            }
            self.apply(body, i)?;
        }
        Ok(())
    }

    /// A bad record at the END of the log is a torn write and is discarded; a bad record anywhere else
    /// means the file was edited or damaged, and appending to it would build on a lie.
    ///
    /// The distinction is the whole reason the checksum is there. Treating every bad line as torn
    /// would let a corrupted middle silently truncate the store; treating every bad line as corrupt
    /// would make an ordinary power loss unrecoverable.
    fn torn_or_corrupt(&mut self, index: usize, total: usize, why: &str) -> Result<(), AuthError> {
        if index + 1 == total {
            self.counters.torn_records_discarded += 1;
            Ok(())
        } else {
            Err(AuthError::Corrupt {
                line: index + 1,
                why: why.to_string(),
            })
        }
    }

    fn apply(&mut self, body: &str, line: usize) -> Result<(), AuthError> {
        let corrupt = |why: &str| AuthError::Corrupt {
            line: line + 1,
            why: why.to_string(),
        };
        let mut it = body.split('\t');
        let kind = it.next().ok_or_else(|| corrupt("empty record"))?;
        match kind {
            "invite" => {
                let hash = it.next().ok_or_else(|| corrupt("invite: no hash"))?;
                let email = it.next().ok_or_else(|| corrupt("invite: no email"))?;
                let expires: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("invite: no expiry"))?
                    .parse()
                    .map_err(|_| corrupt("invite: bad expiry"))?;
                self.invitations.insert(
                    hash.to_string(),
                    Invitation {
                        email: email.to_string(),
                        expires_at: expires,
                        consumed_at: None,
                    },
                );
            }
            "invite-used" => {
                let hash = it.next().ok_or_else(|| corrupt("invite-used: no hash"))?;
                let at: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("invite-used: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("invite-used: bad timestamp"))?;
                if let Some(i) = self.invitations.get_mut(hash) {
                    i.consumed_at = Some(at);
                }
            }
            "acct" => {
                let id: AccountId = it
                    .next()
                    .ok_or_else(|| corrupt("acct: no id"))?
                    .parse()
                    .map_err(|_| corrupt("acct: bad id"))?;
                let email = it
                    .next()
                    .ok_or_else(|| corrupt("acct: no email"))?
                    .to_string();
                let created: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("acct: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("acct: bad timestamp"))?;
                self.accounts.insert(
                    id,
                    Account {
                        id,
                        email: email.clone(),
                        created_at: created,
                        disabled_at: None,
                    },
                );
                self.by_email.insert(email, id);
            }
            "acct-disabled" => {
                let id: AccountId = it
                    .next()
                    .ok_or_else(|| corrupt("acct-disabled: no id"))?
                    .parse()
                    .map_err(|_| corrupt("acct-disabled: bad id"))?;
                let at: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("acct-disabled: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("acct-disabled: bad timestamp"))?;
                if let Some(a) = self.accounts.get_mut(&id) {
                    a.disabled_at = Some(at);
                }
            }
            "cred" => {
                let cid = from_hex(it.next().ok_or_else(|| corrupt("cred: no id"))?)
                    .ok_or_else(|| corrupt("cred: bad id hex"))?;
                let account: AccountId = it
                    .next()
                    .ok_or_else(|| corrupt("cred: no account"))?
                    .parse()
                    .map_err(|_| corrupt("cred: bad account"))?;
                let label = it
                    .next()
                    .ok_or_else(|| corrupt("cred: no label"))?
                    .to_string();
                let created: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("cred: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("cred: bad timestamp"))?;
                let blob = from_hex(it.next().ok_or_else(|| corrupt("cred: no blob"))?)
                    .ok_or_else(|| corrupt("cred: bad blob hex"))?;
                self.credentials.push(Credential {
                    credential_id: cid,
                    account_id: account,
                    label,
                    created_at: created,
                    last_used_at: None,
                    revoked_at: None,
                    blob,
                });
            }
            "cred-used" => {
                let cid = from_hex(it.next().ok_or_else(|| corrupt("cred-used: no id"))?)
                    .ok_or_else(|| corrupt("cred-used: bad id hex"))?;
                let at: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("cred-used: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("cred-used: bad timestamp"))?;
                let blob = from_hex(it.next().ok_or_else(|| corrupt("cred-used: no blob"))?)
                    .ok_or_else(|| corrupt("cred-used: bad blob hex"))?;
                for c in &mut self.credentials {
                    if c.credential_id == cid && c.revoked_at.is_none() {
                        c.blob.clone_from(&blob);
                        c.last_used_at = Some(at);
                    }
                }
            }
            "cred-revoked" => {
                let cid = from_hex(it.next().ok_or_else(|| corrupt("cred-revoked: no id"))?)
                    .ok_or_else(|| corrupt("cred-revoked: bad id hex"))?;
                let at: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("cred-revoked: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("cred-revoked: bad timestamp"))?;
                for c in &mut self.credentials {
                    if c.credential_id == cid && c.revoked_at.is_none() {
                        c.revoked_at = Some(at);
                    }
                }
            }
            "sess" => {
                let hash = it
                    .next()
                    .ok_or_else(|| corrupt("sess: no hash"))?
                    .to_string();
                let account: AccountId = it
                    .next()
                    .ok_or_else(|| corrupt("sess: no account"))?
                    .parse()
                    .map_err(|_| corrupt("sess: bad account"))?;
                let expires: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("sess: no expiry"))?
                    .parse()
                    .map_err(|_| corrupt("sess: bad expiry"))?;
                self.sessions.insert(
                    hash,
                    StoredSession {
                        account_id: account,
                        expires_at: expires,
                        revoked_at: None,
                    },
                );
            }
            "sess-revoked" => {
                let hash = it.next().ok_or_else(|| corrupt("sess-revoked: no hash"))?;
                let at: Timestamp = it
                    .next()
                    .ok_or_else(|| corrupt("sess-revoked: no timestamp"))?
                    .parse()
                    .map_err(|_| corrupt("sess-revoked: bad timestamp"))?;
                if let Some(s) = self.sessions.get_mut(hash) {
                    s.revoked_at = Some(at);
                }
            }
            // An UNKNOWN record kind is corruption, not something to skip. A future build's records in
            // an older build's store would otherwise be silently dropped — and dropping an
            // `invite-used` record turns a single-use token back into a reusable one.
            other => return Err(corrupt(&format!("unknown record kind '{other}'"))),
        }
        Ok(())
    }
}

/// Lowercase and trim. Addresses are compared case-insensitively in the domain and, in practice, in
/// the local part too — treating `A@x` and `a@x` as different accounts is a way to get two accounts
/// for one mailbox and an invitation that gates nothing.
fn normalise_email(email: &str) -> String {
    email.trim().to_ascii_lowercase()
}

/// The record format is tab-separated and newline-terminated, so a value containing either cannot be
/// represented. Refused at the boundary rather than escaped: escaping is a second format to get right,
/// and no legitimate address or label contains a tab.
fn check_representable(value: &str, field: &'static str) -> Result<(), AuthError> {
    if value.contains('\t') || value.contains('\n') || value.contains('\r') {
        return Err(AuthError::UnrepresentableValue(field));
    }
    Ok(())
}

fn hash_token(token: &str) -> String {
    let digest = Sha256::digest(token.as_bytes());
    to_hex(&digest)
}

fn mint_token() -> Result<Token, AuthError> {
    let mut bytes = [0u8; TOKEN_BYTES];
    fill_random(&mut bytes).map_err(AuthError::Io)?;
    Ok(Token(to_hex(&bytes)))
}

fn mint_u128() -> Result<u128, AuthError> {
    let mut bytes = [0u8; 16];
    fill_random(&mut bytes).map_err(AuthError::Io)?;
    Ok(u128::from_be_bytes(bytes))
}

/// OS entropy. A short read is a hard failure rather than padded: silently degrading the entropy of a
/// capability is worse than refusing to mint one, which is the same rule the gateway's `SessionId`
/// follows.
pub(crate) fn fill_random(out: &mut [u8]) -> std::io::Result<()> {
    #[cfg(unix)]
    {
        use std::io::Read;
        std::fs::File::open("/dev/urandom")?.read_exact(out)
    }
    #[cfg(windows)]
    {
        // The store is Linux-only in production (ADR-0047 §3); this exists so the crate BUILDS and its
        // tests run on the Windows CI leg. It is deliberately not a weak fallback pretending to be
        // strong: `RtlGenRandom` via `BCryptGenRandom` is not reachable without a dependency, so this
        // refuses rather than inventing entropy.
        let _ = out;
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "rime-auth needs an OS entropy source; production is Linux (ADR-0047 §3)",
        ))
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = out;
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "rime-auth needs an OS entropy source",
        ))
    }
}

pub(crate) fn to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push_str(&format!("{b:02x}"));
    }
    out
}

fn from_hex(text: &str) -> Option<Vec<u8>> {
    if !text.len().is_multiple_of(2) {
        return None;
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).ok())
        .collect()
}

/// CRC32 (IEEE), computed bitwise.
///
/// Table-free: the table is 1 KiB of generated constants to save microseconds on records written a
/// handful of times per registration, and the bitwise form is short enough to read and check against
/// the polynomial. This is a torn-write detector, not a security primitive — that is what the SHA-256
/// above is for, and conflating the two would be the mistake.
fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &byte in data {
        crc ^= byte as u32;
        for _ in 0..8 {
            let mask = if crc & 1 != 0 { 0xEDB8_8320 } else { 0 };
            crc = (crc >> 1) ^ mask;
        }
    }
    !crc
}

// Unix-only, for the same reason `rime-gateway::admission` is: the store mints capabilities and needs
// `/dev/urandom`. Production hosting is Linux (ADR-0047 §3), and a weaker fallback nobody runs is the
// one that is quietly weaker — a weaker token is a weaker credential. CI builds this crate on all three
// platforms, so the gate is on the TESTS rather than being an untested stub that compiles.
#[cfg(all(test, unix))]
mod tests {
    use super::*;

    fn temp_path(name: &str) -> PathBuf {
        let mut p = std::env::temp_dir();
        p.push(format!(
            "rime-auth-test-{}-{}-{}",
            name,
            std::process::id(),
            now()
        ));
        let _ = std::fs::remove_file(&p);
        p
    }

    #[test]
    fn crc32_matches_the_known_check_value() {
        // The standard IEEE CRC-32 check value for "123456789". A hand-written CRC that is subtly wrong
        // still detects most corruption, which is exactly why it needs a known-answer test rather than
        // a round-trip one — a round trip passes against its own mistake.
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn an_invitation_is_single_use_and_survives_a_reopen() {
        let path = temp_path("invite");
        let token = {
            let mut store = AuthStore::open(&path).unwrap();
            let t = store
                .issue_invitation("Tester@Example.COM", DEFAULT_INVITE_TTL, 1000)
                .unwrap();
            t.expose().to_string()
        };

        // Reopened: the invitation must have survived, which is the point of the log.
        let mut store = AuthStore::open(&path).unwrap();
        let redeemed = store.redeem_invitation(&token, 1001).unwrap();
        // Normalised on the way in, so the account is bound to the mailbox rather than to its spelling.
        assert_eq!(redeemed.email, "tester@example.com");

        // Second attempt refused, and refused again after ANOTHER reopen — the consume record is what
        // makes single-use durable rather than merely in-memory.
        assert!(matches!(
            store.redeem_invitation(&token, 1002),
            Err(AuthError::InvitationRejected)
        ));
        drop(store);
        let mut store = AuthStore::open(&path).unwrap();
        assert!(matches!(
            store.redeem_invitation(&token, 1003),
            Err(AuthError::InvitationRejected)
        ));
        assert_eq!(store.counters().invitations_rejected_consumed, 1);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn every_invitation_failure_looks_the_same_to_the_caller_and_different_in_the_counters() {
        // The oracle this prevents: distinguishable errors let an attacker probe the token space by
        // reading the response. The operator still needs the three cases apart, so they live in the
        // counters where a client cannot see them.
        let path = temp_path("oracle");
        let mut store = AuthStore::open(&path).unwrap();
        let expired = store
            .issue_invitation("a@b.c", Duration::from_secs(10), 1000)
            .unwrap();
        let good = store
            .issue_invitation("d@e.f", DEFAULT_INVITE_TTL, 1000)
            .unwrap();
        store.redeem_invitation(good.expose(), 1001).unwrap();

        let unknown = store.redeem_invitation("00", 1001);
        let consumed = store.redeem_invitation(good.expose(), 1001);
        let stale = store.redeem_invitation(expired.expose(), 5000);
        for outcome in [&unknown, &consumed, &stale] {
            assert!(matches!(outcome, Err(AuthError::InvitationRejected)));
        }
        // Identical message, too: a different string is the same oracle wearing a hat.
        let text: Vec<String> = [unknown, consumed, stale]
            .iter()
            .map(|r| r.as_ref().err().unwrap().to_string())
            .collect();
        assert_eq!(text[0], text[1]);
        assert_eq!(text[1], text[2]);

        let c = store.counters();
        assert_eq!(c.invitations_rejected_unknown, 1);
        assert_eq!(c.invitations_rejected_consumed, 1);
        assert_eq!(c.invitations_rejected_expired, 1);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_session_authenticates_until_it_expires_or_is_revoked() {
        let path = temp_path("session");
        let mut store = AuthStore::open(&path).unwrap();
        let account = store.create_account("u@example.com", 1000).unwrap();
        let token = store
            .create_session(account, Duration::from_secs(100), 1000)
            .unwrap();

        assert_eq!(
            store.authenticate_session(token.expose(), 1050),
            Some(account)
        );
        // Expiry is absolute: using it does not renew it.
        assert_eq!(store.authenticate_session(token.expose(), 1100), None);
        assert_eq!(store.counters().sessions_rejected_expired, 1);

        let fresh = store
            .create_session(account, Duration::from_secs(100), 2000)
            .unwrap();
        assert_eq!(
            store.authenticate_session(fresh.expose(), 2001),
            Some(account)
        );
        assert!(store.revoke_session(fresh.expose(), 2002).unwrap());
        assert_eq!(store.authenticate_session(fresh.expose(), 2003), None);
        assert_eq!(store.counters().sessions_rejected_revoked, 1);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn disabling_an_account_stops_its_live_sessions_immediately() {
        // Not at expiry. "Disable this account" that takes up to a week to take effect is not a
        // disable, and a revocation path that only covers new logins is the shape of that bug.
        let path = temp_path("disable");
        let mut store = AuthStore::open(&path).unwrap();
        let account = store.create_account("u@example.com", 1000).unwrap();
        let token = store
            .create_session(account, DEFAULT_SESSION_TTL, 1000)
            .unwrap();
        assert_eq!(
            store.authenticate_session(token.expose(), 1001),
            Some(account)
        );

        store
            .append(&["acct-disabled", &account.to_string(), "1002"])
            .unwrap();
        if let Some(a) = store.accounts.get_mut(&account) {
            a.disabled_at = Some(1002);
        }
        assert_eq!(store.authenticate_session(token.expose(), 1003), None);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn revoking_all_sessions_leaves_none_working() {
        let path = temp_path("revoke-all");
        let mut store = AuthStore::open(&path).unwrap();
        let account = store.create_account("u@example.com", 1000).unwrap();
        let tokens: Vec<Token> = (0..3)
            .map(|_| {
                store
                    .create_session(account, DEFAULT_SESSION_TTL, 1000)
                    .unwrap()
            })
            .collect();
        assert_eq!(store.revoke_all_sessions(account, 1001).unwrap(), 3);
        for t in &tokens {
            assert_eq!(store.authenticate_session(t.expose(), 1002), None);
        }
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_torn_final_record_is_discarded_and_a_damaged_middle_one_is_refused() {
        // THE CRASH-SAFETY CLAIM, both halves. Treating every bad line as torn would let a corrupted
        // middle silently truncate the store — and a dropped `invite-used` record turns a single-use
        // token back into a reusable one. Treating every bad line as corrupt would make an ordinary
        // power loss unrecoverable.
        let path = temp_path("torn");
        let token = {
            let mut store = AuthStore::open(&path).unwrap();
            let t = store
                .issue_invitation("a@b.c", DEFAULT_INVITE_TTL, 1000)
                .unwrap();
            store.create_account("a@b.c", 1000).unwrap();
            t.expose().to_string()
        };

        // Simulate a power loss mid-append: a partial line with no newline.
        {
            let mut f = OpenOptions::new().append(true).open(&path).unwrap();
            f.write_all(b"deadbeef\tsess\tabc").unwrap();
        }
        let mut store = AuthStore::open(&path).unwrap();
        assert_eq!(store.counters().torn_records_discarded, 1);
        // Everything before the torn record is intact.
        assert!(store.account_by_email("a@b.c").is_some());
        assert!(store.redeem_invitation(&token, 1001).is_ok());
        drop(store);

        // Now damage a line in the MIDDLE by flipping a byte in the first record's body.
        let mut lines: Vec<String> = std::fs::read_to_string(&path)
            .unwrap()
            .lines()
            .map(str::to_string)
            .collect();
        lines[0].push('x');
        std::fs::write(&path, format!("{}\n", lines.join("\n"))).unwrap();
        let err = AuthStore::open(&path).unwrap_err();
        assert!(matches!(err, AuthError::Corrupt { line: 1, .. }), "{err}");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn the_store_never_holds_a_token_in_the_clear() {
        // The property that makes a stolen store file not a stolen set of credentials.
        let path = temp_path("hashed");
        let (invite, session) = {
            let mut store = AuthStore::open(&path).unwrap();
            let i = store
                .issue_invitation("a@b.c", DEFAULT_INVITE_TTL, 1000)
                .unwrap();
            let account = store.create_account("a@b.c", 1000).unwrap();
            let s = store
                .create_session(account, DEFAULT_SESSION_TTL, 1000)
                .unwrap();
            (i.expose().to_string(), s.expose().to_string())
        };
        let contents = std::fs::read_to_string(&path).unwrap();
        assert!(
            !contents.contains(&invite),
            "the invitation token is on disk in the clear"
        );
        assert!(
            !contents.contains(&session),
            "the session token is on disk in the clear"
        );
        // And the hashes ARE there, so the test is not passing because nothing was written.
        assert!(contents.contains(&hash_token(&invite)));
        assert!(contents.contains(&hash_token(&session)));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_tokens_debug_output_is_redacted() {
        // How secrets usually leak: something printed a struct. Cheap to prevent, invisible if absent.
        let token = mint_token().unwrap();
        let shown = format!("{token:?}");
        assert_eq!(shown, "Token(<redacted>)");
        assert!(!shown.contains(token.expose()));
    }

    #[test]
    fn a_value_with_a_tab_is_refused_rather_than_mangled() {
        // The record format cannot represent it, and escaping would be a second format to get right.
        let path = temp_path("tabs");
        let mut store = AuthStore::open(&path).unwrap();
        assert!(matches!(
            store.issue_invitation("a\tb@c.d", DEFAULT_INVITE_TTL, 1000),
            Err(AuthError::UnrepresentableValue("email"))
        ));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn an_unknown_record_kind_is_corruption_rather_than_something_to_skip() {
        // A future build's records in an older build's store must not be silently dropped: losing an
        // `invite-used` record turns a single-use token back into a reusable one.
        let path = temp_path("future");
        {
            let mut store = AuthStore::open(&path).unwrap();
            store.append(&["totally-new-kind", "x"]).unwrap();
            store
                .append(&["acct", "00000000000000000000000000000001", "a@b.c", "1000"])
                .unwrap();
        }
        let err = AuthStore::open(&path).unwrap_err();
        assert!(matches!(err, AuthError::Corrupt { .. }), "{err}");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn credentials_are_kept_as_opaque_blobs_and_revocation_is_recorded() {
        let path = temp_path("creds");
        let mut store = AuthStore::open(&path).unwrap();
        let account = store.create_account("a@b.c", 1000).unwrap();
        let blob = vec![0x30, 0x59, 0x00, 0xff, 0x7f];
        store
            .add_credential(account, b"cred-1", "phone", &blob, 1000)
            .unwrap();
        store
            .add_credential(account, b"cred-2", "laptop", &blob, 1000)
            .unwrap();
        assert_eq!(store.active_credentials(account).len(), 2);

        assert!(store.revoke_credential(b"cred-1", 1001).unwrap());
        assert!(
            !store.revoke_credential(b"cred-1", 1002).unwrap(),
            "revoking twice is not an event"
        );
        assert_eq!(store.active_credentials(account).len(), 1);
        drop(store);

        // The blob survives a reopen byte for byte — a store that mangled it would produce a passkey
        // that verifies nothing, and the failure would look like the user's device being wrong.
        let store = AuthStore::open(&path).unwrap();
        let live = store.active_credentials(account);
        assert_eq!(live.len(), 1);
        assert_eq!(live[0].label, "laptop");
        assert_eq!(live[0].blob, blob);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn an_account_cannot_be_created_twice_for_one_mailbox() {
        let path = temp_path("dupe");
        let mut store = AuthStore::open(&path).unwrap();
        store.create_account("a@b.c", 1000).unwrap();
        assert!(matches!(
            store.create_account("A@B.C", 1000),
            Err(AuthError::EmailTaken)
        ));
        assert_eq!(store.account_count(), 1);
        let _ = std::fs::remove_file(&path);
    }
}
