// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Who is asking — [ADR-0048](../../docs/adr/0048-authenticating-the-hosted-front-end.md) brick 3.
//!
//! Bricks 1 and 2 built a store and the ceremonies that fill it. Neither of them guards anything: a
//! gateway that knows who you are and still hands you somebody else's session has authenticated for
//! decoration. This module is the gateway's own vocabulary for identity, and `api.rs` is where it
//! turns into refusals.
//!
//! ## Why the gateway has its own account type
//!
//! ADR-0048 decision 3: no `rime-auth` type may enter admission, the engine or `rime-protocol`. So a
//! principal here is an [`AccountRef`] — 128 bits and a spelling — and the `auth` feature provides the
//! one conversion from the real account id. Compile the feature out and this module still compiles,
//! still has a principal, and the ownership checks below still run; what disappears is the only way to
//! become anything other than [`Principal::Anonymous`].
//!
//! ## Anonymous equals anonymous, deliberately
//!
//! Two anonymous requests are the **same** principal, so on a host with no accounts every request owns
//! every session. That is exactly right for the LAN deployment ADR-0045 describes — there is nobody
//! else on the network by assumption — and catastrophic anywhere else, which is why
//! [`AccessPolicy::require_account`] defaults to **true**: a deployment that wants the open behaviour
//! has to say so, and saying so is a line somebody can find.

use std::fmt;

/// An account, as the gateway refers to one: the id's value, and nothing about how it was proved.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct AccountRef(u128);

impl AccountRef {
    #[must_use]
    pub fn from_u128(value: u128) -> Self {
        Self(value)
    }

    #[must_use]
    pub fn as_u128(self) -> u128 {
        self.0
    }
}

/// The same 32-hex spelling `rime-auth` uses, so one id reads the same in both logs. An id with two
/// spellings is one an audit log cannot count — the rule `SessionId` already follows.
impl fmt::Display for AccountRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:032x}", self.0)
    }
}

/// Who a request is from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Principal {
    /// Nobody in particular: no session cookie, or one that did not authenticate.
    Anonymous,
    /// A proved account.
    Account(AccountRef),
}

impl Principal {
    /// Did an account actually prove itself?
    #[must_use]
    pub fn is_account(self) -> bool {
        matches!(self, Principal::Account(_))
    }
}

impl fmt::Display for Principal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Principal::Anonymous => f.write_str("anonymous"),
            Principal::Account(a) => write!(f, "account {a}"),
        }
    }
}

/// What the deployment requires of a request before it may do anything.
#[derive(Debug, Clone, Copy)]
pub struct AccessPolicy {
    /// Refuse anonymous requests outright. **Default true**: an open host is a decision, not a
    /// default, and ADR-0045's "v1 is LAN and un-authenticated" is a deployment saying so out loud.
    pub require_account: bool,
}

impl Default for AccessPolicy {
    fn default() -> Self {
        Self {
            require_account: true,
        }
    }
}

/// The cookie the browser session lives in.
///
/// `__Host-` is not decoration: the prefix is only accepted by a browser when the cookie is `Secure`,
/// `Path=/` and carries **no `Domain`**, which is what stops a sibling subdomain from setting a cookie
/// this gateway would then read as its own.
pub const SESSION_COOKIE: &str = "__Host-rime";

/// Pull one cookie's value out of a `Cookie:` header.
///
/// Hand-rolled for the same reason the HTTP parser is, and deliberately strict: names are compared
/// whole, so `x__Host-rime` and `__Host-rime2` do not match, and a value is taken verbatim up to the
/// next `;` with surrounding spaces trimmed. A cookie value containing a `;` cannot be represented in
/// the header at all, so there is nothing to unescape and no second parsing path to disagree.
#[must_use]
pub fn cookie<'a>(header: &'a str, name: &str) -> Option<&'a str> {
    for part in header.split(';') {
        let part = part.trim();
        let Some((key, value)) = part.split_once('=') else {
            continue;
        };
        if key.trim() == name {
            return Some(value.trim());
        }
    }
    None
}

/// Resolve a request's principal from its session cookie — the ONE place a request becomes a person.
///
/// Only compiled with the `auth` feature, which is what makes the rest of this module honest: without
/// it there is no conversion from any credential into [`Principal::Account`], so an unauthenticated
/// build cannot accidentally grow one. With it, the answer comes from `rime-auth`'s store and nothing
/// else — not from a header the client chose, not from the body.
///
/// A missing, malformed, expired or revoked cookie is [`Principal::Anonymous`], never an error: "who
/// is this?" always has an answer, and whether anonymous is *allowed* is [`AccessPolicy`]'s question,
/// asked one layer up. Keeping those two apart is what stops a route from being reachable because
/// somebody handled the error case differently.
#[cfg(feature = "auth")]
#[must_use]
pub fn principal_of(
    auth: &mut rime_auth::flow::Auth,
    request: &crate::http::Request,
    now: u64,
) -> Principal {
    let Some(header) = request.header("cookie") else {
        return Principal::Anonymous;
    };
    let Some(token) = cookie(header, SESSION_COOKIE) else {
        return Principal::Anonymous;
    };
    match auth.store_mut().authenticate_session(token, now) {
        Some(account) => Principal::Account(AccountRef::from_u128(account.as_u128())),
        None => Principal::Anonymous,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_cookie_is_matched_by_its_whole_name() {
        let header = "theme=dark; __Host-rime=abc123; other=1";
        assert_eq!(cookie(header, SESSION_COOKIE), Some("abc123"));
        // A prefix or suffix of the name is a different cookie, not this one.
        assert_eq!(cookie(header, "Host-rime"), None);
        assert_eq!(cookie("x__Host-rime=nope", SESSION_COOKIE), None);
        assert_eq!(cookie("__Host-rime2=nope", SESSION_COOKIE), None);
    }

    #[test]
    fn a_missing_or_malformed_cookie_header_yields_nothing() {
        assert_eq!(cookie("", SESSION_COOKIE), None);
        assert_eq!(cookie("__Host-rime", SESSION_COOKIE), None);
        assert_eq!(cookie("=value", SESSION_COOKIE), None);
    }

    #[test]
    fn an_account_reads_the_same_here_as_in_the_store() {
        let a = AccountRef::from_u128(0x0123_4567_89ab_cdef_0123_4567_89ab_cdef);
        assert_eq!(a.to_string(), "0123456789abcdef0123456789abcdef");
        assert_eq!(Principal::Account(a).to_string(), format!("account {a}"));
        assert!(!Principal::Anonymous.is_account());
    }

    /// The adapter is the only route from a credential to an account, so it gets a test that drives a
    /// real registration through `rime-auth` and then presents the cookie it produced.
    #[cfg(feature = "auth")]
    #[test]
    fn a_session_cookie_names_the_account_that_holds_it() {
        use rime_auth::ceremony::CeremonyConfig;
        use rime_auth::flow::Auth;
        use rime_auth::mail::CapturingMailer;
        use rime_auth::{AuthStore, DEFAULT_SESSION_TTL};

        let mut path = std::env::temp_dir();
        path.push(format!("rime-gateway-identity-{}.log", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let store = AuthStore::open(&path).expect("a store");
        let config = CeremonyConfig {
            rp_id: "rime.example".into(),
            rp_origin: "https://rime.example".into(),
            rp_name: "Rime".into(),
        };
        let mut auth = Auth::new(store, &config, Box::new(CapturingMailer::default())).unwrap();
        let account = auth
            .store_mut()
            .create_account("a@example.test", 100)
            .unwrap();
        let session = auth
            .store_mut()
            .create_session(account, DEFAULT_SESSION_TTL, 100)
            .unwrap();

        let with_cookie = |value: &str| crate::http::Request {
            method: crate::http::Method::Get,
            path: "/api/sessions".into(),
            query: String::new(),
            headers: vec![("cookie".into(), value.into())],
            body: Vec::new(),
        };

        assert_eq!(
            principal_of(
                &mut auth,
                &with_cookie(&format!("{SESSION_COOKIE}={}", session.expose())),
                101
            ),
            Principal::Account(AccountRef::from_u128(account.as_u128()))
        );
        // A token that is not a session, and no cookie at all, are both simply anonymous.
        assert_eq!(
            principal_of(
                &mut auth,
                &with_cookie(&format!("{SESSION_COOKIE}=nope")),
                101
            ),
            Principal::Anonymous
        );
        assert_eq!(
            principal_of(&mut auth, &with_cookie("theme=dark"), 101),
            Principal::Anonymous
        );
        // And a revoked session stops naming anybody.
        auth.store_mut()
            .revoke_session(session.expose(), 102)
            .unwrap();
        assert_eq!(
            principal_of(
                &mut auth,
                &with_cookie(&format!("{SESSION_COOKIE}={}", session.expose())),
                103
            ),
            Principal::Anonymous
        );
    }
}
