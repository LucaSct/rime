// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The ceremonies over HTTP — the surface a browser actually talks to.
//!
//! `rime-auth` has the store, the WebAuthn ceremonies and the flows that order them; `identity.rs`
//! turns a cookie into a principal. Nothing so far *serves* any of it, so this is the routing table
//! that does, and it exists only with the `auth` feature — an exported game bundle has no accounts and
//! gets no account endpoints to attack (ADR-0048 decision 2).
//!
//! ## Two shapes of request, and why
//!
//! A WebAuthn response is a **nested** object, and this crate's JSON parser is deliberately flat — it
//! parses the small command bodies the session API takes and nothing else, which is why it fits in a
//! page and can be read for correctness. Rather than grow a general parser to carry a blob the gateway
//! must not interpret anyway, the ceremony endpoints put the identifier in the **path** and take the
//! browser's response as the **entire body**, passed through to `rime-auth` untouched. So:
//!
//! - small commands (`{"invitation":"…"}`, `{"code":"…"}`) are flat JSON bodies;
//! - a signed ceremony response is the raw body of a URL that already says which ceremony it answers.
//!
//! The gateway therefore never reinterprets a credential, and there is no second parser to disagree
//! with `webauthn-rs` about what the browser said.
//!
//! ## The ceremony cookie
//!
//! `flow` binds every transaction to a browser, and this is where that binding comes from: a second
//! `__Host-` cookie, 128 random bits, set when a registration or recovery begins and required to
//! finish it. It is deliberately **not** the transaction id — a binding that travels in the same value
//! as the thing it binds is not a binding at all. It is also short-lived and cleared when the
//! transaction completes.
//!
//! ## What a failure is allowed to say
//!
//! A wrong invitation, a wrong code, a wrong recovery secret and an address with no account are **one
//! answer**: `403`, with one message. Telling them apart tells an attacker which half to keep working
//! on, and lets anyone ask whether an address has an account here. The distinctions live in the
//! counters, where an operator can see them and a client cannot — the same ruling `rime-auth`'s store
//! makes about invitation failures.
//!
//! ## Not here: rate limiting
//!
//! It needs the peer's address, which arrives at the listener rather than in the request, so it is its
//! own change through `serve_connection` and the binary. Until it exists, the bounded things are the
//! ones `rime-auth` already bounds: five attempts per mailed code, a capped transaction table, and
//! invitations that gate registration entirely.

use rime_auth::flow::{Auth, FlowError};

use crate::admission::SessionId;
use crate::http::{parse_flat_object, Method, Request, Response};
use crate::identity::{cookie, SESSION_COOKIE};
use crate::limits::{Decision, Limiter, Rate};

/// The cookie that binds a ceremony to the browser that started it.
pub const CEREMONY_COOKIE: &str = "__Host-rime-ceremony";

/// How long the browser keeps the session cookie. Matches `rime-auth`'s own session TTL: a cookie that
/// outlives the session it names is a cookie that produces a confusing failure instead of a login.
const SESSION_MAX_AGE: u64 = 7 * 24 * 60 * 60;

/// How long the ceremony cookie lives — the transaction's own lifetime.
const CEREMONY_MAX_AGE: u64 = 20 * 60;

/// What one caller may ask of this surface, and what everybody together may.
///
/// The numbers are set against what a real ceremony costs rather than against an attacker's patience:
/// a registration is four requests and a login is two, so twenty a minute is a browser that is trying
/// repeatedly and failing, not a browser that is working. The global bound exists because a per-client
/// limit is only as good as the client key, and an attacker with many addresses has many keys — it is
/// the one number that still holds when the key space is not theirs to exhaust.
#[derive(Debug, Clone, Copy)]
pub struct LimitPolicy {
    pub per_client: Rate,
    pub global: Rate,
}

impl Default for LimitPolicy {
    fn default() -> Self {
        Self {
            per_client: Rate::new(20, 60),
            global: Rate::new(120, 60),
        }
    }
}

/// What the endpoints have been asked to do. Counters rather than messages, because the client is
/// deliberately told less than this.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct AuthApiCounters {
    pub registrations_begun: u64,
    pub registrations_completed: u64,
    pub logins_completed: u64,
    pub recoveries_begun: u64,
    pub recoveries_completed: u64,
    pub logouts: u64,
    /// A proof was wrong: an invitation, a code, a recovery secret, or an address with no account.
    pub refused_proof: u64,
    /// The transaction was unknown, expired, or asked for the wrong step.
    pub refused_step: u64,
    pub bad_request: u64,
    pub not_found: u64,
    pub method_not_allowed: u64,
    /// The store, the mailer or a ceremony failed — ours, not the client's.
    pub internal: u64,
    /// Requests refused by the rate limiter. The client is told `429` and nothing else; the shape of
    /// what is happening lives here.
    pub rate_limited: u64,
}

/// The account endpoints.
pub struct AuthApi {
    auth: Auth,
    counters: AuthApiCounters,
    per_client: Limiter,
    global: Limiter,
}

impl AuthApi {
    #[must_use]
    pub fn new(auth: Auth) -> Self {
        Self::with_limits(auth, LimitPolicy::default())
    }

    /// Build with a chosen [`LimitPolicy`]. Tests that are about a ceremony rather than about the
    /// limiter raise the numbers rather than removing them, so the limiter is on every path they
    /// exercise.
    #[must_use]
    pub fn with_limits(auth: Auth, limits: LimitPolicy) -> Self {
        Self {
            auth,
            counters: AuthApiCounters::default(),
            per_client: Limiter::new(limits.per_client),
            global: Limiter::new(limits.global),
        }
    }

    /// Refusals by the limiter, split the way an operator needs them: a global limit biting means the
    /// host is under load from many places, and a per-client limit biting means one caller is.
    #[must_use]
    pub fn limit_counters(&self) -> (u64, u64) {
        (self.global.denied(), self.per_client.denied())
    }

    #[must_use]
    pub fn counters(&self) -> AuthApiCounters {
        self.counters
    }

    /// The flows, for the operator-facing calls (issuing an invitation) and for `identity::principal_of`.
    pub fn auth_mut(&mut self) -> &mut Auth {
        &mut self.auth
    }

    /// Does this path belong to the account surface? The session API owns everything else, and a
    /// gateway that guessed would eventually route an account request through the session router.
    #[must_use]
    pub fn handles(path: &str) -> bool {
        path == "/api/auth" || path.starts_with("/api/auth/")
    }

    /// Route one account request on behalf of `client` — the peer's address, as the listener saw it.
    ///
    /// The limiter runs **first**, before the method check and before any parsing, because everything
    /// after it costs something: a JSON parse, a store read, an HMAC, a mail. A limiter that ran after
    /// the work would bound the answers rather than the work.
    ///
    /// `client` is the key and nothing else — it is never logged with the request, never compared for
    /// authorization, and an empty one is a key like any other (it will simply share a bucket with
    /// every other caller the listener could not name, which is the conservative direction).
    pub fn route(&mut self, request: &Request, client: &str, now: u64) -> Response {
        if let Some(refusal) = self.rate_limit(client, now) {
            return refusal;
        }
        let segments: Vec<&str> = request.path.split('/').filter(|s| !s.is_empty()).collect();
        // Every one of these changes state or spends a proof, so every one is a POST. A GET that began
        // a ceremony would be something a link could do to somebody.
        if request.method != Method::Post {
            self.counters.method_not_allowed += 1;
            return Response::error(405, "method not allowed").with_header("Allow", "POST");
        }
        match segments.as_slice() {
            ["api", "auth", "register"] => self.begin_registration(request, now),
            ["api", "auth", "register", tx, "code"] => self.submit_code(request, tx, now),
            ["api", "auth", "register", tx, "options"] => self.passkey_options(request, tx, now),
            ["api", "auth", "register", tx, "passkey"] => {
                self.finish_registration(request, tx, now)
            }
            ["api", "auth", "login", "options"] => self.login_options(request, now),
            ["api", "auth", "login", challenge] => self.finish_login(request, challenge, now),
            ["api", "auth", "recover"] => self.begin_recovery(request, now),
            ["api", "auth", "recover", tx, "proofs"] => self.submit_proofs(request, tx, now),
            ["api", "auth", "recover", tx, "options"] => self.passkey_options(request, tx, now),
            ["api", "auth", "recover", tx, "passkey"] => self.finish_recovery(request, tx, now),
            ["api", "auth", "logout"] => self.logout(request, now),
            _ => {
                self.counters.not_found += 1;
                Response::error(404, "no such route")
            }
        }
    }

    // ── Registration ──────────────────────────────────────────────────────────────────────────

    fn begin_registration(&mut self, request: &Request, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(invitation) = field(&fields, "invitation") else {
            return self.bad_request("missing \"invitation\"");
        };
        match self.auth.begin_registration(&invitation, now) {
            Ok(tx) => {
                self.counters.registrations_begun += 1;
                let binding = match new_binding() {
                    Ok(b) => b,
                    Err(response) => return response,
                };
                Response::json(201, format!("{{\"transaction\":\"{}\"}}", tx.expose())).with_header(
                    "Set-Cookie",
                    set_cookie(CEREMONY_COOKIE, &binding, CEREMONY_MAX_AGE),
                )
            }
            Err(e) => self.refuse(&e),
        }
    }

    fn submit_code(&mut self, request: &Request, tx: &str, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(code) = field(&fields, "code") else {
            return self.bad_request("missing \"code\"");
        };
        match self.auth.submit_code(tx, &code, now) {
            Ok(()) => Response::empty(204),
            Err(e) => self.refuse(&e),
        }
    }

    fn submit_proofs(&mut self, request: &Request, tx: &str, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let (Some(secret), Some(code)) = (field(&fields, "secret"), field(&fields, "code")) else {
            return self.bad_request("recovery needs both \"secret\" and \"code\"");
        };
        match self.auth.submit_recovery_proofs(tx, &secret, &code, now) {
            Ok(()) => Response::empty(204),
            Err(e) => self.refuse(&e),
        }
    }

    fn passkey_options(&mut self, request: &Request, tx: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.wrong_browser();
        };
        match self.auth.start_passkey(tx, &binding, now) {
            Ok(started) => Response::json(
                200,
                format!(
                    "{{\"challenge\":\"{}\",\"options\":{}}}",
                    started.challenge_id.expose(),
                    started.options_json
                ),
            ),
            Err(e) => self.refuse(&e),
        }
    }

    fn finish_registration(&mut self, request: &Request, tx: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.wrong_browser();
        };
        let Ok(body) = std::str::from_utf8(&request.body) else {
            return self.bad_request("body is not UTF-8");
        };
        match self.auth.finish_registration(tx, &binding, body, now) {
            Ok(done) => {
                self.counters.registrations_completed += 1;
                // The recovery secret is in this response and nowhere else — the store has only its
                // hash — so a client that does not show it to the user has lost it for good.
                Response::json(
                    201,
                    format!(
                        "{{\"account\":\"{}\",\"recovery_secret\":\"{}\"}}",
                        done.account, done.recovery_secret
                    ),
                )
                .with_header(
                    "Set-Cookie",
                    set_cookie(SESSION_COOKIE, done.session.expose(), SESSION_MAX_AGE),
                )
                .with_header("Set-Cookie", clear_cookie(CEREMONY_COOKIE))
            }
            Err(e) => self.refuse(&e),
        }
    }

    // ── Login ─────────────────────────────────────────────────────────────────────────────────

    fn login_options(&mut self, request: &Request, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(email) = field(&fields, "email") else {
            return self.bad_request("missing \"email\"");
        };
        // A login ceremony is bound to the browser too, so an options blob captured in transit cannot
        // be finished from somewhere else.
        let binding = match binding_of(request) {
            Some(existing) => existing,
            None => match new_binding() {
                Ok(b) => b,
                Err(response) => return response,
            },
        };
        match self.auth.start_login(&email, &binding, now) {
            Ok(started) => Response::json(
                200,
                format!(
                    "{{\"challenge\":\"{}\",\"options\":{}}}",
                    started.challenge_id.expose(),
                    started.options_json
                ),
            )
            .with_header(
                "Set-Cookie",
                set_cookie(CEREMONY_COOKIE, &binding, CEREMONY_MAX_AGE),
            ),
            Err(e) => self.refuse(&e),
        }
    }

    fn finish_login(&mut self, request: &Request, challenge: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.wrong_browser();
        };
        let Ok(body) = std::str::from_utf8(&request.body) else {
            return self.bad_request("body is not UTF-8");
        };
        match self.auth.finish_login(challenge, &binding, body, now) {
            Ok(who) => {
                self.counters.logins_completed += 1;
                Response::json(200, format!("{{\"account\":\"{}\"}}", who.account))
                    .with_header(
                        "Set-Cookie",
                        set_cookie(SESSION_COOKIE, who.session.expose(), SESSION_MAX_AGE),
                    )
                    .with_header("Set-Cookie", clear_cookie(CEREMONY_COOKIE))
            }
            Err(e) => self.refuse(&e),
        }
    }

    // ── Recovery ──────────────────────────────────────────────────────────────────────────────

    fn begin_recovery(&mut self, request: &Request, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(email) = field(&fields, "email") else {
            return self.bad_request("missing \"email\"");
        };
        match self.auth.begin_recovery(&email, now) {
            Ok(tx) => {
                self.counters.recoveries_begun += 1;
                let binding = match new_binding() {
                    Ok(b) => b,
                    Err(response) => return response,
                };
                Response::json(201, format!("{{\"transaction\":\"{}\"}}", tx.expose())).with_header(
                    "Set-Cookie",
                    set_cookie(CEREMONY_COOKIE, &binding, CEREMONY_MAX_AGE),
                )
            }
            Err(e) => self.refuse(&e),
        }
    }

    fn finish_recovery(&mut self, request: &Request, tx: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.wrong_browser();
        };
        let Ok(body) = std::str::from_utf8(&request.body) else {
            return self.bad_request("body is not UTF-8");
        };
        match self.auth.finish_recovery(tx, &binding, body, now) {
            Ok(done) => {
                self.counters.recoveries_completed += 1;
                Response::json(
                    200,
                    format!(
                        "{{\"account\":\"{}\",\"recovery_secret\":\"{}\",\"credentials_revoked\":{}}}",
                        done.account, done.recovery_secret, done.credentials_revoked
                    ),
                )
                .with_header("Set-Cookie", set_cookie(SESSION_COOKIE, done.session.expose(), SESSION_MAX_AGE))
                .with_header("Set-Cookie", clear_cookie(CEREMONY_COOKIE))
            }
            Err(e) => self.refuse(&e),
        }
    }

    // ── Logout ────────────────────────────────────────────────────────────────────────────────

    /// Revoke the session server-side **and** clear the cookie. Clearing the cookie alone would leave
    /// a live bearer token in whatever captured it, which is the difference between logging out and
    /// looking logged out.
    fn logout(&mut self, request: &Request, now: u64) -> Response {
        if let Some(token) = request
            .header("cookie")
            .and_then(|h| cookie(h, SESSION_COOKIE))
        {
            if self.auth.store_mut().revoke_session(token, now).is_err() {
                self.counters.internal += 1;
                return Response::error(500, "could not complete");
            }
        }
        self.counters.logouts += 1;
        // 204 whether or not there was a session: whether a token was live is not a fact this endpoint
        // should confirm to whoever presents it.
        Response::empty(204).with_header("Set-Cookie", clear_cookie(SESSION_COOKIE))
    }

    // ── Plumbing ──────────────────────────────────────────────────────────────────────────────

    /// The global bound first, then the per-client one: a caller that is inside its own budget must
    /// still not be served when the host as a whole is over, or the global bound would only apply to
    /// whoever asked last.
    fn rate_limit(&mut self, client: &str, now: u64) -> Option<Response> {
        for decision in [
            self.global.check("all", now),
            self.per_client.check(client, now),
        ] {
            if let Decision::Deny { retry_after } = decision {
                self.counters.rate_limited += 1;
                return Some(
                    Response::error(429, "too many requests")
                        .with_header("Retry-After", retry_after.to_string()),
                );
            }
        }
        None
    }

    fn flat(&mut self, request: &Request) -> Result<Vec<(String, String)>, Response> {
        match request.header("content-type") {
            Some(ct) if ct.split(';').next().unwrap_or("").trim() == "application/json" => {}
            _ => {
                self.counters.bad_request += 1;
                return Err(Response::error(
                    415,
                    "expected Content-Type: application/json",
                ));
            }
        }
        parse_flat_object(&request.body).map_err(|why| {
            self.counters.bad_request += 1;
            Response::error(400, why)
        })
    }

    fn bad_request(&mut self, why: &str) -> Response {
        self.counters.bad_request += 1;
        Response::error(400, why)
    }

    fn wrong_browser(&mut self) -> Response {
        self.counters.refused_step += 1;
        Response::error(409, "start again in this browser")
    }

    /// One answer for every wrong proof, and a different one for a transaction that is not where the
    /// caller thinks it is. The detail stays in the counters.
    fn refuse(&mut self, error: &FlowError) -> Response {
        match error {
            FlowError::InvitationRejected
            | FlowError::CodeRejected
            | FlowError::RecoveryRefused
            | FlowError::NoSuchAccount => {
                self.counters.refused_proof += 1;
                Response::error(403, "that did not work")
            }
            FlowError::UnknownTransaction | FlowError::Expired | FlowError::WrongStep => {
                self.counters.refused_step += 1;
                Response::error(409, "start again")
            }
            FlowError::TooManyTransactions => {
                self.counters.refused_step += 1;
                Response::error(429, "too many registrations in flight — try again shortly")
            }
            FlowError::Ceremony(_) => {
                // A ceremony refusal is usually the browser's doing (a wrong origin, a cancelled
                // prompt, a replayed assertion) and never says which.
                self.counters.refused_proof += 1;
                Response::error(403, "that did not work")
            }
            FlowError::Store(_) | FlowError::Mail(_) => {
                self.counters.internal += 1;
                Response::error(500, "could not complete")
            }
        }
    }
}

fn field(fields: &[(String, String)], name: &str) -> Option<String> {
    fields
        .iter()
        .find(|(k, _)| k == name)
        .map(|(_, v)| v.clone())
}

fn binding_of(request: &Request) -> Option<String> {
    request
        .header("cookie")
        .and_then(|h| cookie(h, CEREMONY_COOKIE))
        .map(str::to_string)
}

/// 128 fresh bits for a ceremony binding. The same primitive a session id is, used for a different
/// capability — and minted the same way, because "a random value nobody else can guess" has exactly
/// one correct implementation here.
fn new_binding() -> Result<String, Response> {
    SessionId::generate()
        .map(|id| id.to_string())
        .map_err(|_| Response::error(500, "could not complete"))
}

/// `__Host-` prefixed, so the browser only accepts it when it is `Secure`, `Path=/` and carries no
/// `Domain` — which is what stops a sibling subdomain from setting a cookie this gateway would read as
/// its own. `HttpOnly` keeps it out of scripts and `SameSite=Strict` off cross-site requests.
fn set_cookie(name: &str, value: &str, max_age: u64) -> String {
    format!("{name}={value}; Secure; HttpOnly; SameSite=Strict; Path=/; Max-Age={max_age}")
}

fn clear_cookie(name: &str) -> String {
    format!("{name}=; Secure; HttpOnly; SameSite=Strict; Path=/; Max-Age=0")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::{principal_of, AccountRef, Principal};
    use rime_auth::ceremony::CeremonyConfig;
    use rime_auth::mail::{CapturingMailer, MailError, Mailer, Message};
    use rime_auth::{AuthStore, DEFAULT_INVITE_TTL};
    use std::io::Cursor;
    use std::sync::Arc;
    use webauthn_authenticator_rs::softpasskey::SoftPasskey;
    use webauthn_authenticator_rs::WebauthnAuthenticator;
    use webauthn_rs::prelude::{CreationChallengeResponse, RequestChallengeResponse, Url};

    const ORIGIN: &str = "https://rime.example";
    const NOW: u64 = 1_700_000_000;
    const INVITED: &str = "claire@example.test";

    struct Shared(Arc<CapturingMailer>);
    impl Mailer for Shared {
        fn send(&self, message: &Message) -> Result<(), MailError> {
            self.0.send(message)
        }
    }

    fn api() -> (AuthApi, Arc<CapturingMailer>) {
        let (auth, mailer) = api_parts();
        (AuthApi::new(auth), mailer)
    }

    fn api_parts() -> (rime_auth::flow::Auth, Arc<CapturingMailer>) {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let mut path = std::env::temp_dir();
        path.push(format!(
            "rime-gateway-auth-{}-{}.log",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        let _ = std::fs::remove_file(&path);
        let mailer = Arc::new(CapturingMailer::default());
        let auth = rime_auth::flow::Auth::new(
            AuthStore::open(&path).expect("a store"),
            &CeremonyConfig {
                rp_id: "rime.example".into(),
                rp_origin: ORIGIN.into(),
                rp_name: "Rime".into(),
            },
            Box::new(Shared(Arc::clone(&mailer))),
        )
        .expect("auth builds");
        (auth, mailer)
    }

    fn post(path: &str, body: &str, cookies: &str) -> Request {
        let cookie_line = if cookies.is_empty() {
            String::new()
        } else {
            format!("Cookie: {cookies}\r\n")
        };
        let raw = format!(
            "POST {path} HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\n{cookie_line}\r\n{body}",
            body.len()
        );
        crate::http::read_request(&mut Cursor::new(raw.into_bytes())).unwrap()
    }

    fn body_of(response: &Response) -> String {
        String::from_utf8(response.body.clone()).unwrap()
    }

    fn field_of(response: &Response, name: &str) -> String {
        let body = body_of(response);
        let needle = format!("\"{name}\":\"");
        let start = body.find(&needle).expect("field present") + needle.len();
        let end = body[start..].find('"').unwrap() + start;
        body[start..end].to_string()
    }

    fn set_cookies(response: &Response) -> Vec<String> {
        response
            .extra
            .iter()
            .filter(|(k, _)| *k == "Set-Cookie")
            .map(|(_, v)| v.clone())
            .collect()
    }

    fn cookie_value(response: &Response, name: &str) -> Option<String> {
        set_cookies(response)
            .iter()
            .find_map(|c| c.strip_prefix(&format!("{name}=")))
            .map(|rest| rest.split(';').next().unwrap_or("").to_string())
    }

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

    /// Registration as a browser performs it: four requests, a real signed credential, and a session
    /// cookie at the end that `identity::principal_of` then recognises. If any of the path shapes, the
    /// cookie plumbing or the pass-through of the WebAuthn body were wrong, this would not complete.
    #[test]
    fn a_registration_runs_end_to_end_over_http() {
        let (mut api, mailer) = api();
        let invitation = api
            .auth_mut()
            .store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .unwrap()
            .expose()
            .to_string();

        let begun = api.route(
            &post(
                "/api/auth/register",
                &format!("{{\"invitation\":\"{invitation}\"}}"),
                "",
            ),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(begun.status, 201);
        let tx = field_of(&begun, "transaction");
        let binding = cookie_value(&begun, CEREMONY_COOKIE).expect("a ceremony cookie");
        let jar = format!("{CEREMONY_COOKIE}={binding}");

        let coded = api.route(
            &post(
                &format!("/api/auth/register/{tx}/code"),
                &format!("{{\"code\":\"{}\"}}", mailed_code(&mailer)),
                &jar,
            ),
            "10.0.0.1",
            NOW + 1,
        );
        assert_eq!(coded.status, 204);

        let options = api.route(
            &post(&format!("/api/auth/register/{tx}/options"), "{}", &jar),
            "10.0.0.1",
            NOW + 2,
        );
        assert_eq!(options.status, 200);
        let body = body_of(&options);
        let creation: CreationChallengeResponse =
            serde_json::from_str(&body[body.find("\"options\":").unwrap() + 10..body.len() - 1])
                .expect("the options are the browser's own JSON");

        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let credential = device
            .do_registration(Url::parse(ORIGIN).unwrap(), creation)
            .unwrap();
        let finished = api.route(
            &post(
                &format!("/api/auth/register/{tx}/passkey"),
                &serde_json::to_string(&credential).unwrap(),
                &jar,
            ),
            "10.0.0.1",
            NOW + 3,
        );
        assert_eq!(finished.status, 201);
        let account = field_of(&finished, "account");
        assert_eq!(field_of(&finished, "recovery_secret").len(), 64);

        // The session cookie is real: the identity adapter now names this account from it.
        let session = cookie_value(&finished, SESSION_COOKIE).expect("a session cookie");
        let with_session = post(
            "/api/sessions",
            "{}",
            &format!("{SESSION_COOKIE}={session}"),
        );
        assert_eq!(
            principal_of(api.auth_mut(), &with_session, NOW + 4),
            Principal::Account(AccountRef::from_u128(
                u128::from_str_radix(&account, 16).unwrap()
            ))
        );
        // And the ceremony cookie is cleared, so a finished transaction leaves no binding behind.
        assert!(set_cookies(&finished)
            .iter()
            .any(|c| c.starts_with(&format!("{CEREMONY_COOKIE}=;"))));
    }

    /// Every cookie this surface sets carries the four attributes that make it worth having. A
    /// `__Host-` cookie without `Secure` is not accepted by a browser at all, and one without
    /// `HttpOnly` is readable by any script that gets injected.
    #[test]
    fn every_cookie_is_host_prefixed_secure_httponly_and_same_site() {
        let (mut api, _mailer) = api();
        let response = api.route(
            &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
            "10.0.0.1",
            NOW,
        );
        // That invitation is refused, so use one that gets as far as setting a cookie.
        assert_eq!(response.status, 403);

        let invitation = api
            .auth_mut()
            .store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .unwrap()
            .expose()
            .to_string();
        let begun = api.route(
            &post(
                "/api/auth/register",
                &format!("{{\"invitation\":\"{invitation}\"}}"),
                "",
            ),
            "10.0.0.1",
            NOW,
        );
        for c in set_cookies(&begun) {
            assert!(c.starts_with("__Host-"), "not host-prefixed: {c}");
            assert!(c.contains("; Secure"), "not secure: {c}");
            assert!(c.contains("; HttpOnly"), "not httponly: {c}");
            assert!(c.contains("; SameSite=Strict"), "not same-site: {c}");
            assert!(c.contains("; Path=/"), "no path: {c}");
            assert!(!c.contains("Domain="), "carries a domain: {c}");
        }
    }

    /// A ceremony started in one browser cannot be finished by another, and the gateway's answer does
    /// not depend on whether the transaction was real.
    #[test]
    fn a_finish_without_the_ceremony_cookie_is_refused() {
        let (mut api, _mailer) = api();
        let no_cookie = api.route(
            &post("/api/auth/register/abc/options", "{}", ""),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(no_cookie.status, 409);
        let wrong_cookie = api.route(
            &post(
                "/api/auth/register/abc/options",
                "{}",
                &format!("{CEREMONY_COOKIE}=somebodyelses"),
            ),
            "10.0.0.1",
            NOW,
        );
        // A real cookie and a transaction that is not theirs are the same answer.
        assert_eq!(wrong_cookie.status, 409);
    }

    /// A wrong invitation, a wrong code and an address with no account are one answer. Anything else
    /// tells an attacker which half to keep working on, or whether an address is registered here.
    #[test]
    fn every_wrong_proof_gets_the_same_answer() {
        let (mut api, mailer) = api();
        let bad_invitation = api.route(
            &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(bad_invitation.status, 403);
        assert_eq!(
            body_of(&bad_invitation),
            "{\"error\":\"that did not work\"}"
        );

        let unknown_account = api.route(
            &post(
                "/api/auth/login/options",
                "{\"email\":\"nobody@example.test\"}",
                "",
            ),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(unknown_account.status, 403);
        assert_eq!(body_of(&unknown_account), body_of(&bad_invitation));

        let invitation = api
            .auth_mut()
            .store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .unwrap()
            .expose()
            .to_string();
        let begun = api.route(
            &post(
                "/api/auth/register",
                &format!("{{\"invitation\":\"{invitation}\"}}"),
                "",
            ),
            "10.0.0.1",
            NOW,
        );
        let tx = field_of(&begun, "transaction");
        let jar = format!(
            "{CEREMONY_COOKIE}={}",
            cookie_value(&begun, CEREMONY_COOKIE).unwrap()
        );
        let wrong_code = api.route(
            &post(
                &format!("/api/auth/register/{tx}/code"),
                "{\"code\":\"00000000\"}",
                &jar,
            ),
            "10.0.0.1",
            NOW + 1,
        );
        assert_eq!(wrong_code.status, 403);
        assert_eq!(body_of(&wrong_code), body_of(&bad_invitation));
        // The real code still works afterwards, so the refusal above was about the guess.
        let right = api.route(
            &post(
                &format!("/api/auth/register/{tx}/code"),
                &format!("{{\"code\":\"{}\"}}", mailed_code(&mailer)),
                &jar,
            ),
            "10.0.0.1",
            NOW + 2,
        );
        assert_eq!(right.status, 204);
        assert_eq!(api.counters().refused_proof, 3);
    }

    /// A GET must not be able to begin a ceremony: that is something a link could make somebody do.
    #[test]
    fn only_post_reaches_these_routes() {
        let (mut api, _mailer) = api();
        let raw = "GET /api/auth/register HTTP/1.1\r\n\r\n";
        let request = crate::http::read_request(&mut Cursor::new(raw.as_bytes().to_vec())).unwrap();
        let response = api.route(&request, "10.0.0.1", NOW);
        assert_eq!(response.status, 405);
        assert!(response
            .extra
            .iter()
            .any(|(k, v)| *k == "Allow" && v == "POST"));
    }

    /// Logging out revokes the session server-side as well as clearing the cookie. Clearing it alone
    /// would leave a live bearer token in whatever had captured it.
    #[test]
    fn logout_revokes_the_session_rather_than_only_forgetting_it() {
        let (mut api, _mailer) = api();
        let account = api
            .auth_mut()
            .store_mut()
            .create_account("a@example.test", NOW)
            .unwrap();
        let session = api
            .auth_mut()
            .store_mut()
            .create_session(account, rime_auth::DEFAULT_SESSION_TTL, NOW)
            .unwrap();
        let jar = format!("{SESSION_COOKIE}={}", session.expose());

        let out = api.route(&post("/api/auth/logout", "{}", &jar), "10.0.0.1", NOW + 1);
        assert_eq!(out.status, 204);
        assert!(set_cookies(&out)
            .iter()
            .any(|c| c.starts_with(&format!("{SESSION_COOKIE}=;")) && c.contains("Max-Age=0")));
        // The token is dead, not merely unreferenced.
        assert_eq!(
            principal_of(api.auth_mut(), &post("/api/sessions", "{}", &jar), NOW + 2),
            Principal::Anonymous
        );
        // And logging out twice is still a 204: whether a token was live is not a fact to confirm.
        assert_eq!(
            api.route(&post("/api/auth/logout", "{}", &jar), "10.0.0.1", NOW + 3)
                .status,
            204
        );
    }

    #[test]
    fn a_login_runs_end_to_end_and_the_ceremony_is_browser_bound() {
        let (mut api, mailer) = api();
        // Register first, with the same four steps the end-to-end case covers.
        let invitation = api
            .auth_mut()
            .store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .unwrap()
            .expose()
            .to_string();
        let begun = api.route(
            &post(
                "/api/auth/register",
                &format!("{{\"invitation\":\"{invitation}\"}}"),
                "",
            ),
            "10.0.0.1",
            NOW,
        );
        let tx = field_of(&begun, "transaction");
        let jar = format!(
            "{CEREMONY_COOKIE}={}",
            cookie_value(&begun, CEREMONY_COOKIE).unwrap()
        );
        api.route(
            &post(
                &format!("/api/auth/register/{tx}/code"),
                &format!("{{\"code\":\"{}\"}}", mailed_code(&mailer)),
                &jar,
            ),
            "10.0.0.1",
            NOW + 1,
        );
        let options = api.route(
            &post(&format!("/api/auth/register/{tx}/options"), "{}", &jar),
            "10.0.0.1",
            NOW + 2,
        );
        let body = body_of(&options);
        let creation: CreationChallengeResponse =
            serde_json::from_str(&body[body.find("\"options\":").unwrap() + 10..body.len() - 1])
                .unwrap();
        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let credential = device
            .do_registration(Url::parse(ORIGIN).unwrap(), creation)
            .unwrap();
        api.route(
            &post(
                &format!("/api/auth/register/{tx}/passkey"),
                &serde_json::to_string(&credential).unwrap(),
                &jar,
            ),
            "10.0.0.1",
            NOW + 3,
        );

        // Now log in with the same device.
        let started = api.route(
            &post(
                "/api/auth/login/options",
                &format!("{{\"email\":\"{INVITED}\"}}"),
                "",
            ),
            "10.0.0.1",
            NOW + 10,
        );
        assert_eq!(started.status, 200);
        let challenge = field_of(&started, "challenge");
        let login_jar = format!(
            "{CEREMONY_COOKIE}={}",
            cookie_value(&started, CEREMONY_COOKIE).unwrap()
        );
        let body = body_of(&started);
        let request_options: RequestChallengeResponse =
            serde_json::from_str(&body[body.find("\"options\":").unwrap() + 10..body.len() - 1])
                .unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), request_options)
            .unwrap();
        let signed = serde_json::to_string(&assertion).unwrap();

        // From a different browser the assertion is refused, even though it is genuine.
        let elsewhere = api.route(
            &post(
                &format!("/api/auth/login/{challenge}"),
                &signed,
                &format!("{CEREMONY_COOKIE}=someoneelse"),
            ),
            "10.0.0.1",
            NOW + 11,
        );
        assert_eq!(elsewhere.status, 403);

        // The ceremony is spent by that attempt, so a fresh one is needed — which is the point.
        let started = api.route(
            &post(
                "/api/auth/login/options",
                &format!("{{\"email\":\"{INVITED}\"}}"),
                &login_jar,
            ),
            "10.0.0.1",
            NOW + 12,
        );
        let challenge = field_of(&started, "challenge");
        let body = body_of(&started);
        let request_options: RequestChallengeResponse =
            serde_json::from_str(&body[body.find("\"options\":").unwrap() + 10..body.len() - 1])
                .unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), request_options)
            .unwrap();
        let ok = api.route(
            &post(
                &format!("/api/auth/login/{challenge}"),
                &serde_json::to_string(&assertion).unwrap(),
                &login_jar,
            ),
            "10.0.0.1",
            NOW + 13,
        );
        assert_eq!(ok.status, 200);
        assert!(cookie_value(&ok, SESSION_COOKIE).is_some());
        assert_eq!(api.counters().logins_completed, 1);
    }

    /// The limiter is on the surface, not merely in the module. Twenty is the default per-client
    /// burst; the twenty-first request from the same caller is refused with something it can act on.
    #[test]
    fn a_caller_that_hammers_the_surface_is_refused_with_a_retry_after() {
        let (mut api, _mailer) = api();
        for _ in 0..20 {
            let response = api.route(
                &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
                "10.0.0.1",
                NOW,
            );
            assert_eq!(
                response.status, 403,
                "inside the budget, it is the proof that fails"
            );
        }
        let limited = api.route(
            &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(limited.status, 429);
        let retry = limited
            .extra
            .iter()
            .find(|(k, _)| *k == "Retry-After")
            .map(|(_, v)| v.parse::<u64>().unwrap())
            .expect("a Retry-After a client can honour");
        assert!(retry >= 1);
        assert_eq!(api.counters().rate_limited, 1);
        // A different caller is unaffected: the budget is per key, and the global one is six times
        // larger than the per-client one for exactly this reason.
        assert_eq!(
            api.route(
                &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
                "10.0.0.2",
                NOW,
            )
            .status,
            403
        );
    }

    /// A `429` must cost the host nothing beyond the limiter itself. If the parse, the store read or
    /// the mail happened first, the limit would bound the answers rather than the work.
    #[test]
    fn the_limiter_runs_before_any_of_the_work() {
        let (auth, _mailer) = api_parts();
        let mut api = AuthApi::with_limits(
            auth,
            LimitPolicy {
                per_client: Rate::new(1, 60),
                global: Rate::new(100, 60),
            },
        );
        assert_eq!(
            api.route(&post("/api/auth/register", "{}", ""), "10.0.0.1", NOW)
                .status,
            400,
            "the first one is spent on a real answer"
        );
        let before = api.counters();
        // Malformed body, wrong media type, unknown route: none of it is even looked at.
        let limited = api.route(
            &post("/api/auth/nonsense", "not json at all", ""),
            "10.0.0.1",
            NOW,
        );
        assert_eq!(limited.status, 429);
        let after = api.counters();
        assert_eq!(after.bad_request, before.bad_request, "nothing was parsed");
        assert_eq!(after.not_found, before.not_found, "nothing was routed");
    }

    /// The global bound is what still holds when the client key is not the attacker's constraint.
    #[test]
    fn the_global_bound_holds_across_many_clients() {
        let (auth, _mailer) = api_parts();
        let mut api = AuthApi::with_limits(
            auth,
            LimitPolicy {
                per_client: Rate::new(2, 60),
                global: Rate::new(5, 60),
            },
        );
        let mut allowed = 0;
        for i in 0..20 {
            let response = api.route(
                &post("/api/auth/register", "{\"invitation\":\"nope\"}", ""),
                &format!("10.0.0.{i}"),
                NOW,
            );
            if response.status != 429 {
                allowed += 1;
            }
        }
        // Five got through, one per global token, however many addresses asked.
        assert_eq!(allowed, 5);
        let (global_denied, per_client_denied) = api.limit_counters();
        assert_eq!(global_denied, 15);
        assert_eq!(per_client_denied, 0, "the global bound bit first");
    }
}
