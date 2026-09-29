// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Signing a browser in with your phone, over HTTP — [ADR-0055](../../../../docs/adr/0055-signing-in-a-browser-with-your-phone.md).
//!
//! `rime-auth`'s `flow::pair` is the state machine; this is its routing table. Every route is a `POST`
//! under `/api/auth/pair`:
//!
//! | route | who | does |
//! |---|---|---|
//! | `/api/auth/pair` | desktop | open a pairing bound to its ceremony cookie; answer `{id, qr_svg, expires_in}` |
//! | `/<id>/describe` | phone | the desktop's (unverified) user-agent summary and age — a read |
//! | `/<id>/options` `{email}` | phone | start a passkey assertion bound to the phone AND this pairing |
//! | `/<id>/approve/<challenge>` | phone | the signed assertion as the whole body; answer the code |
//! | `/<id>/status` | desktop | `waiting` / `approved` / `expired` — never the code or the account |
//! | `/<id>/redeem` `{code}` | desktop | the phone's code; answer the approving account's email |
//! | `/<id>/finish` `{decision}` | desktop | `accept` mints one session; `refuse` kills the pairing |
//!
//! The approval puts the challenge id in the **path** and the WebAuthn response as the raw body — the
//! same shape `/api/auth/login/<challenge>` uses, for the reason this module's parent gives: the
//! response is nested JSON, the gateway's parser is deliberately flat, and the gateway must not
//! reinterpret a credential anyway.
//!
//! ## The fences, all applied in [`AuthApi::pair_route`]
//!
//! - **The rate limiter** has already run by the time a request gets here — `route` calls it first
//!   for every account route, pairing included.
//! - **An exact `Origin`**, equal to the configured relying-party origin. The cookies are already
//!   `SameSite=Strict`; this is the second fence, and it costs one string comparison.
//! - **`Cache-Control: no-store` and `Referrer-Policy: no-referrer`** on every answer, refusals
//!   included: a pairing id or a code in a cache, or in a `Referer` sent to some other host, is a
//!   capability left lying around.
//! - **One refusal** — `404`, one message — for unknown, expired, finished, wrong-state and
//!   not-this-browser, the same rule the session gate applies to render sessions. Nothing here logs
//!   an id, a code, a binding or a challenge.

use rime_auth::flow::{FlowError, PairingDecision, PairingStatus};

use super::{
    binding_of, clear_cookie, field, new_binding, set_cookie, AuthApi, CEREMONY_COOKIE,
    CEREMONY_MAX_AGE, SESSION_MAX_AGE,
};
use crate::http::{json_string, Request, Response};
use crate::identity::SESSION_COOKIE;

/// The one thing a refused pairing step says.
pub(super) const REFUSAL: &str = "no such pairing";

impl AuthApi {
    /// Route one `/api/auth/pair…` request. `rest` is the path after `pair`.
    pub(super) fn pair_route(&mut self, request: &Request, rest: &[&str], now: u64) -> Response {
        let response = if request.header("origin") != Some(self.auth.rp_origin()) {
            self.counters.wrong_origin += 1;
            Response::error(403, "wrong origin")
        } else {
            match rest {
                [] => self.pair_begin(request, now),
                [id, "describe"] => self.pair_describe(id, now),
                [id, "options"] => self.pair_options(request, id, now),
                [id, "approve", challenge] => self.pair_approve(request, id, challenge, now),
                [id, "status"] => self.pair_status(request, id, now),
                [id, "redeem"] => self.pair_redeem(request, id, now),
                [id, "finish"] => self.pair_finish(request, id, now),
                _ => self.pair_refusal(),
            }
        };
        response
            .with_header("Cache-Control", "no-store")
            .with_header("Referrer-Policy", "no-referrer")
    }

    /// Desktop: open a pairing. The ceremony cookie is reused if the browser has one and set if it
    /// does not, exactly as a login's options step does — it is what every later desktop step is
    /// checked against.
    fn pair_begin(&mut self, request: &Request, now: u64) -> Response {
        let binding = match binding_of(request) {
            Some(existing) => existing,
            None => match new_binding() {
                Ok(b) => b,
                Err(response) => return response,
            },
        };
        let agent = summarize_user_agent(request.header("user-agent").unwrap_or(""));
        let started = match self.auth.begin_pairing(&binding, &agent, now) {
            Ok(started) => started,
            Err(e) => return self.refuse(&e),
        };
        // The id rides in the FRAGMENT: a browser never sends the part after `#` to a server, so the
        // capability the phone opens never appears in a request line or an access log. The origin is
        // the configured one — a Host header chose nothing here.
        let url = format!("{}/#pair={}", self.auth.rp_origin(), started.id);
        let Some(svg) = qr_svg(&url) else {
            self.counters.internal += 1;
            return Response::error(500, "could not complete");
        };
        self.counters.pairings_begun += 1;
        Response::json(
            201,
            format!(
                "{{\"id\":\"{}\",\"qr_svg\":{},\"expires_in\":{}}}",
                started.id,
                json_string(&svg),
                started.expires_in
            ),
        )
        .with_header(
            "Set-Cookie",
            set_cookie(CEREMONY_COOKIE, &binding, CEREMONY_MAX_AGE),
        )
    }

    /// Phone: what to show before the user decides. Changes nothing.
    fn pair_describe(&mut self, id: &str, now: u64) -> Response {
        match self.auth.describe_pairing(id, now) {
            Ok(view) => Response::json(
                200,
                format!(
                    "{{\"user_agent\":{},\"age_secs\":{}}}",
                    json_string(&view.user_agent),
                    view.age_secs
                ),
            ),
            Err(e) => self.refuse(&e),
        }
    }

    /// Phone: start the passkey assertion. Its OWN ceremony cookie, never the desktop's — the two
    /// devices share nothing but the id in the QR code.
    fn pair_options(&mut self, request: &Request, id: &str, now: u64) -> Response {
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(email) = field(&fields, "email") else {
            return self.bad_request("missing \"email\"");
        };
        let binding = match binding_of(request) {
            Some(existing) => existing,
            None => match new_binding() {
                Ok(b) => b,
                Err(response) => return response,
            },
        };
        match self.auth.start_pairing_approval(id, &email, &binding, now) {
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

    /// Phone: the signed assertion. The answer is the code, to the phone and nobody else — and no
    /// session cookie: approving a desktop does not log the phone in. A session cookie the phone
    /// sends is ignored entirely; only the assertion counts.
    fn pair_approve(&mut self, request: &Request, id: &str, challenge: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.wrong_browser();
        };
        let Ok(body) = std::str::from_utf8(&request.body) else {
            return self.bad_request("body is not UTF-8");
        };
        match self
            .auth
            .approve_pairing(id, challenge, &binding, body, now)
        {
            Ok(code) => {
                self.counters.pairings_approved += 1;
                Response::json(200, format!("{{\"code\":\"{}\"}}", code.expose()))
            }
            Err(e) => self.refuse(&e),
        }
    }

    /// Desktop: the poll.
    fn pair_status(&mut self, request: &Request, id: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.pair_refusal();
        };
        match self.auth.pairing_status(id, &binding, now) {
            Ok(status) => {
                let word = match status {
                    PairingStatus::Waiting => "waiting",
                    PairingStatus::Approved => "approved",
                    PairingStatus::Expired => "expired",
                };
                Response::json(200, format!("{{\"status\":\"{word}\"}}"))
            }
            Err(e) => self.refuse(&e),
        }
    }

    /// Desktop: the code from the phone. The answer is the approving account's email and nothing
    /// else, so the person can see whose account they are about to be signed in to.
    fn pair_redeem(&mut self, request: &Request, id: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.pair_refusal();
        };
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let Some(code) = field(&fields, "code") else {
            return self.bad_request("missing \"code\"");
        };
        match self.auth.redeem_pairing(id, &binding, &code, now) {
            Ok(email) => Response::json(200, format!("{{\"email\":{}}}", json_string(&email))),
            Err(e) => self.refuse(&e),
        }
    }

    /// Desktop: accept (one session, the ceremony cookie cleared) or refuse (the pairing dies).
    fn pair_finish(&mut self, request: &Request, id: &str, now: u64) -> Response {
        let Some(binding) = binding_of(request) else {
            return self.pair_refusal();
        };
        let fields = match self.flat(request) {
            Ok(fields) => fields,
            Err(response) => return response,
        };
        let decision = match field(&fields, "decision").as_deref() {
            Some("accept") => PairingDecision::Accept,
            Some("refuse") => PairingDecision::Refuse,
            _ => return self.bad_request("\"decision\" must be \"accept\" or \"refuse\""),
        };
        match self.auth.finish_pairing(id, &binding, decision, now) {
            Ok(Some(who)) => {
                self.counters.pairings_completed += 1;
                Response::json(200, format!("{{\"account\":\"{}\"}}", who.account))
                    .with_header(
                        "Set-Cookie",
                        set_cookie(SESSION_COOKIE, who.session.expose(), SESSION_MAX_AGE),
                    )
                    .with_header("Set-Cookie", clear_cookie(CEREMONY_COOKIE))
            }
            Ok(None) => Response::empty(204),
            Err(e) => self.refuse(&e),
        }
    }

    /// The one refusal, for a desktop step that arrives with no ceremony cookie at all — the same
    /// bytes the flow's `PairingRefused` becomes in `refuse`, so a missing cookie is not a tell.
    fn pair_refusal(&mut self) -> Response {
        self.refuse(&FlowError::PairingRefused)
    }
}

/// Render the QR code as a standalone SVG. Error-correction level M (15% of the symbol recoverable)
/// is the usual choice for a code shown on a screen: enough to survive glare and a slightly
/// off-angle phone, without growing the symbol the way H would. Dark on light regardless of the
/// page's theme, because inverted QR codes are the ones some scanners refuse.
fn qr_svg(url: &str) -> Option<String> {
    use qrcode::render::svg;
    let code =
        qrcode::QrCode::with_error_correction_level(url.as_bytes(), qrcode::EcLevel::M).ok()?;
    Some(
        code.render::<svg::Color<'_>>()
            .min_dimensions(240, 240)
            .dark_color(svg::Color("#000000"))
            .light_color(svg::Color("#ffffff"))
            .build(),
    )
}

/// "Firefox on Linux" — a summary from a fixed vocabulary, never the raw header. The phone shows it
/// labelled as unverified (the desktop chose it), and a fixed vocabulary means an attacker's header
/// cannot put a sentence of their own on the phone's screen ("Approved by Rime support — continue").
/// Order matters: an Android UA also says Linux, Edge's also says Chrome, and Chrome's also says
/// Safari.
fn summarize_user_agent(ua: &str) -> String {
    let browser = [
        ("Edg/", "Edge"),
        ("OPR/", "Opera"),
        ("Firefox/", "Firefox"),
        ("FxiOS/", "Firefox"),
        ("CriOS/", "Chrome"),
        ("Chrome/", "Chrome"),
        ("Safari/", "Safari"),
    ]
    .iter()
    .find(|(needle, _)| ua.contains(needle))
    .map_or("A browser", |(_, name)| name);
    let system = [
        ("Android", "Android"),
        ("iPhone", "iOS"),
        ("iPad", "iOS"),
        ("Windows", "Windows"),
        ("CrOS", "ChromeOS"),
        ("Macintosh", "macOS"),
        ("Linux", "Linux"),
    ]
    .iter()
    .find(|(needle, _)| ua.contains(needle))
    .map_or("an unknown system", |(_, name)| name);
    format!("{browser} on {system}")
}

#[cfg(test)]
#[cfg(unix)]
mod tests {
    use super::super::tests::{
        api_parts, body_of, cookie_value, field_of, mailed_code, set_cookies, INVITED, NOW, ORIGIN,
    };
    use super::super::{AuthApi, LimitPolicy, CEREMONY_COOKIE};
    use super::*;
    use crate::identity::{principal_of, Principal};
    use crate::limits::Rate;
    use rime_auth::mail::CapturingMailer;
    use rime_auth::DEFAULT_INVITE_TTL;
    use std::io::Cursor;
    use std::sync::Arc;
    use webauthn_authenticator_rs::softpasskey::SoftPasskey;
    use webauthn_authenticator_rs::WebauthnAuthenticator;
    use webauthn_rs::prelude::{CreationChallengeResponse, RequestChallengeResponse, Url};

    const FIREFOX: &str = "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0";

    /// A pairing API with an account already registered, and the device that holds its passkey. The
    /// limiter is raised rather than removed (as the parent's tests do), so it is still on every path.
    fn world() -> (
        AuthApi,
        Arc<CapturingMailer>,
        WebauthnAuthenticator<SoftPasskey>,
    ) {
        let (mut auth, mailer) = api_parts();
        let token = auth
            .store_mut()
            .issue_invitation(INVITED, DEFAULT_INVITE_TTL, NOW)
            .unwrap()
            .expose()
            .to_string();
        let tx = auth.begin_registration(&token, NOW).unwrap();
        auth.submit_code(tx.expose(), &mailed_code(&mailer), NOW)
            .unwrap();
        let started = auth.start_passkey(tx.expose(), "reg", NOW).unwrap();
        let options: CreationChallengeResponse =
            serde_json::from_str(&started.options_json).unwrap();
        let mut device = WebauthnAuthenticator::new(SoftPasskey::new(true));
        let credential = device
            .do_registration(Url::parse(ORIGIN).unwrap(), options)
            .unwrap();
        auth.finish_registration(
            tx.expose(),
            "reg",
            &serde_json::to_string(&credential).unwrap(),
            NOW,
        )
        .unwrap();
        let api = AuthApi::with_limits(
            auth,
            LimitPolicy {
                per_client: Rate::new(1000, 60),
                global: Rate::new(1000, 60),
            },
        );
        (api, mailer, device)
    }

    fn post_from(path: &str, body: &str, cookies: &str, origin: Option<&str>) -> Request {
        let mut raw = format!(
            "POST {path} HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\nUser-Agent: {FIREFOX}\r\n",
            body.len()
        );
        if let Some(origin) = origin {
            raw.push_str(&format!("Origin: {origin}\r\n"));
        }
        if !cookies.is_empty() {
            raw.push_str(&format!("Cookie: {cookies}\r\n"));
        }
        raw.push_str("\r\n");
        raw.push_str(body);
        crate::http::read_request(&mut Cursor::new(raw.into_bytes())).unwrap()
    }

    fn call(api: &mut AuthApi, path: &str, body: &str, cookies: &str, now: u64) -> Response {
        api.route(
            &post_from(path, body, cookies, Some(ORIGIN)),
            "10.0.0.1",
            now,
        )
    }

    fn jar(response: &Response) -> String {
        format!(
            "{CEREMONY_COOKIE}={}",
            cookie_value(response, CEREMONY_COOKIE).expect("a ceremony cookie")
        )
    }

    /// The desktop opens a pairing; returns its id and cookie jar.
    fn open(api: &mut AuthApi, now: u64) -> (String, String) {
        let begun = call(api, "/api/auth/pair", "{}", "", now);
        assert_eq!(begun.status, 201, "{}", body_of(&begun));
        // A real SVG, escaped into a JSON string the page can insert as-is.
        assert!(body_of(&begun).contains("\"qr_svg\":\"<?xml"));
        assert!(body_of(&begun).contains("<svg"));
        (field_of(&begun, "id"), jar(&begun))
    }

    /// The phone approves; returns the code it is shown.
    fn approve(
        api: &mut AuthApi,
        device: &mut WebauthnAuthenticator<SoftPasskey>,
        id: &str,
        now: u64,
    ) -> Response {
        let options = call(
            api,
            &format!("/api/auth/pair/{id}/options"),
            &format!("{{\"email\":\"{INVITED}\"}}"),
            "",
            now,
        );
        assert_eq!(options.status, 200, "{}", body_of(&options));
        let phone = jar(&options);
        let challenge = field_of(&options, "challenge");
        let body = body_of(&options);
        let request: RequestChallengeResponse =
            serde_json::from_str(&body[body.find("\"options\":").unwrap() + 10..body.len() - 1])
                .unwrap();
        let assertion = device
            .do_authentication(Url::parse(ORIGIN).unwrap(), request)
            .unwrap();
        call(
            api,
            &format!("/api/auth/pair/{id}/approve/{challenge}"),
            &serde_json::to_string(&assertion).unwrap(),
            &phone,
            now + 1,
        )
    }

    fn wrong(code: &str) -> &'static str {
        if code == "00000000" {
            "11111111"
        } else {
            "00000000"
        }
    }

    // The whole ceremony over HTTP: one session cookie that the identity adapter recognises, one
    // announcement mail, and a replayed `finish` refused because the pairing is gone. Also: every
    // answer carries no-store and no-referrer, and the code appears in no desktop-facing body.
    #[test]
    fn a_phone_signs_a_desktop_in_end_to_end() {
        let (mut api, mailer, mut device) = world();
        let mails = mailer.sent().len();
        let (id, desktop) = open(&mut api, NOW + 10);

        let described = call(
            &mut api,
            &format!("/api/auth/pair/{id}/describe"),
            "{}",
            "",
            NOW + 11,
        );
        assert_eq!(described.status, 200);
        assert_eq!(field_of(&described, "user_agent"), "Firefox on Linux");

        let waiting = call(
            &mut api,
            &format!("/api/auth/pair/{id}/status"),
            "{}",
            &desktop,
            NOW + 11,
        );
        assert_eq!(body_of(&waiting), "{\"status\":\"waiting\"}");

        let approved = approve(&mut api, &mut device, &id, NOW + 12);
        assert_eq!(approved.status, 200, "{}", body_of(&approved));
        let code = field_of(&approved, "code");
        assert_eq!(code.len(), 8);
        // No phone session: approving is not logging in.
        assert!(cookie_value(&approved, SESSION_COOKIE).is_none());

        let status = call(
            &mut api,
            &format!("/api/auth/pair/{id}/status"),
            "{}",
            &desktop,
            NOW + 14,
        );
        assert_eq!(body_of(&status), "{\"status\":\"approved\"}");
        let redeemed = call(
            &mut api,
            &format!("/api/auth/pair/{id}/redeem"),
            &format!("{{\"code\":\"{code}\"}}"),
            &desktop,
            NOW + 15,
        );
        assert_eq!(redeemed.status, 200);
        assert_eq!(body_of(&redeemed), format!("{{\"email\":\"{INVITED}\"}}"));
        // The code never flows back towards the desktop.
        for body in [body_of(&status), body_of(&redeemed), body_of(&waiting)] {
            assert!(!body.contains(&code), "the code leaked into {body}");
        }

        let finished = call(
            &mut api,
            &format!("/api/auth/pair/{id}/finish"),
            "{\"decision\":\"accept\"}",
            &desktop,
            NOW + 16,
        );
        assert_eq!(finished.status, 200);
        let session = cookie_value(&finished, SESSION_COOKIE).expect("one session cookie");
        assert!(set_cookies(&finished)
            .iter()
            .any(|c| c.starts_with(&format!("{CEREMONY_COOKIE}=;"))));
        assert!(matches!(
            principal_of(
                api.auth_mut(),
                &post_from(
                    "/api/sessions",
                    "{}",
                    &format!("{SESSION_COOKIE}={session}"),
                    None
                ),
                NOW + 17
            ),
            Principal::Account(_)
        ));
        assert_eq!(mailer.sent().len(), mails + 1, "exactly one announcement");
        assert_eq!(api.counters().pairings_completed, 1);

        let again = call(
            &mut api,
            &format!("/api/auth/pair/{id}/finish"),
            "{\"decision\":\"accept\"}",
            &desktop,
            NOW + 18,
        );
        assert_eq!(again.status, 404);
        for response in [
            &described, &waiting, &approved, &redeemed, &finished, &again,
        ] {
            assert!(response
                .extra
                .iter()
                .any(|(k, v)| *k == "Cache-Control" && v == "no-store"));
            assert!(response
                .extra
                .iter()
                .any(|(k, v)| *k == "Referrer-Policy" && v == "no-referrer"));
        }
    }

    // Photographing the QR is not enough: status, redeem and finish from another browser (or from
    // none) are byte-for-byte the answer an id that never existed gets.
    #[test]
    fn another_browser_cannot_tell_a_real_pairing_from_none() {
        let (mut api, _mailer, mut device) = world();
        let (id, _desktop) = open(&mut api, NOW + 10);
        let code = field_of(&approve(&mut api, &mut device, &id, NOW + 11), "code");
        let unknown = "0".repeat(32);
        for (route, body) in [
            ("status", "{}".to_string()),
            ("redeem", format!("{{\"code\":\"{code}\"}}")),
            ("finish", "{\"decision\":\"accept\"}".to_string()),
        ] {
            for thief in ["", &format!("{CEREMONY_COOKIE}=somebodyelses")] {
                let real = call(
                    &mut api,
                    &format!("/api/auth/pair/{id}/{route}"),
                    &body,
                    thief,
                    NOW + 12,
                );
                let none = call(
                    &mut api,
                    &format!("/api/auth/pair/{unknown}/{route}"),
                    &body,
                    thief,
                    NOW + 12,
                );
                assert_eq!(real.status, 404, "{route}");
                assert_eq!(
                    (real.status, body_of(&real)),
                    (none.status, body_of(&none)),
                    "{route}"
                );
            }
        }
    }

    // Brute force: five wrong codes and the pairing is dead; the right code afterwards is refused.
    #[test]
    fn five_wrong_codes_kill_the_pairing() {
        let (mut api, _mailer, mut device) = world();
        let (id, desktop) = open(&mut api, NOW + 10);
        let code = field_of(&approve(&mut api, &mut device, &id, NOW + 11), "code");
        for _ in 0..5 {
            let r = call(
                &mut api,
                &format!("/api/auth/pair/{id}/redeem"),
                &format!("{{\"code\":\"{}\"}}", wrong(&code)),
                &desktop,
                NOW + 12,
            );
            assert_eq!(r.status, 403);
        }
        let right = call(
            &mut api,
            &format!("/api/auth/pair/{id}/redeem"),
            &format!("{{\"code\":\"{code}\"}}"),
            &desktop,
            NOW + 13,
        );
        assert_eq!(right.status, 404);
    }

    // Replay and expiry: a second approval of the same pairing is refused, and so is one that arrives
    // after the pairing's five minutes.
    #[test]
    fn a_pairing_is_approved_once_and_never_after_expiry() {
        let (mut api, _mailer, mut device) = world();
        let (id, _desktop) = open(&mut api, NOW + 10);
        assert_eq!(approve(&mut api, &mut device, &id, NOW + 11).status, 200);
        // The second phone cannot even start: the pairing is no longer waiting.
        let second = call(
            &mut api,
            &format!("/api/auth/pair/{id}/options"),
            &format!("{{\"email\":\"{INVITED}\"}}"),
            "",
            NOW + 12,
        );
        assert_eq!(second.status, 404);

        let (late, _desktop) = open(&mut api, NOW + 20);
        let expired = approve(
            &mut api,
            &mut device,
            &late,
            NOW + 20 + rime_auth::flow::PAIRING_TTL.as_secs() - 1,
        );
        // Options started one second inside the window; the signed approval landed as it closed.
        assert_eq!(expired.status, 404);
    }

    // A stolen session cookie is not an approval: with a live session and the phone's ceremony
    // cookie but no fresh assertion, the approval is refused and the pairing is still waiting.
    #[test]
    fn a_session_cookie_without_a_fresh_assertion_approves_nothing() {
        let (mut api, _mailer, _device) = world();
        let account = api.auth_mut().store().account_by_email(INVITED).unwrap().id;
        let session = api
            .auth_mut()
            .store_mut()
            .create_session(account, rime_auth::DEFAULT_SESSION_TTL, NOW)
            .unwrap();
        let (id, desktop) = open(&mut api, NOW + 10);
        let options = call(
            &mut api,
            &format!("/api/auth/pair/{id}/options"),
            &format!("{{\"email\":\"{INVITED}\"}}"),
            "",
            NOW + 11,
        );
        let challenge = field_of(&options, "challenge");
        let cookies = format!("{}; {SESSION_COOKIE}={}", jar(&options), session.expose());
        for body in ["{}", ""] {
            let r = call(
                &mut api,
                &format!("/api/auth/pair/{id}/approve/{challenge}"),
                body,
                &cookies,
                NOW + 12,
            );
            assert_ne!(r.status, 200);
            assert!(!body_of(&r).contains("code"));
        }
        let status = call(
            &mut api,
            &format!("/api/auth/pair/{id}/status"),
            "{}",
            &desktop,
            NOW + 13,
        );
        assert_eq!(body_of(&status), "{\"status\":\"waiting\"}");
    }

    // The second fence: a request from another origin, or with none, is refused on every pair route
    // before any of them does anything.
    #[test]
    fn a_wrong_origin_is_refused_on_every_pair_route() {
        let (mut api, _mailer, _device) = world();
        let (id, desktop) = open(&mut api, NOW + 10);
        let before = api.auth_mut().pairings();
        for route in [
            "/api/auth/pair".to_string(),
            format!("/api/auth/pair/{id}/describe"),
            format!("/api/auth/pair/{id}/options"),
            format!("/api/auth/pair/{id}/approve/abc"),
            format!("/api/auth/pair/{id}/status"),
            format!("/api/auth/pair/{id}/redeem"),
            format!("/api/auth/pair/{id}/finish"),
        ] {
            for origin in [
                Some("https://evil.example"),
                Some("http://rime.example"),
                None,
            ] {
                let r = api.route(
                    &post_from(&route, "{}", &desktop, origin),
                    "10.0.0.1",
                    NOW + 11,
                );
                assert_eq!(r.status, 403, "{route} from {origin:?}");
                assert_eq!(body_of(&r), "{\"error\":\"wrong origin\"}");
            }
        }
        assert_eq!(api.auth_mut().pairings(), before, "nothing was opened");
        assert_eq!(api.counters().wrong_origin, 21);
    }

    // Opening a pairing needs no account, so the table is capped: the 65th live one is refused.
    #[test]
    fn the_sixty_fifth_live_pairing_is_refused() {
        let (mut api, _mailer, _device) = world();
        for _ in 0..rime_auth::flow::MAX_PAIRINGS {
            open(&mut api, NOW + 10);
        }
        let refused = call(&mut api, "/api/auth/pair", "{}", "", NOW + 10);
        assert_eq!(refused.status, 429);
    }

    #[test]
    fn user_agents_are_summarised_from_a_fixed_vocabulary() {
        assert_eq!(summarize_user_agent(FIREFOX), "Firefox on Linux");
        assert_eq!(
            summarize_user_agent(
                "Mozilla/5.0 (Linux; Android 15) AppleWebKit/537.36 Chrome/140.0 Mobile Safari/537.36"
            ),
            "Chrome on Android"
        );
        assert_eq!(
            summarize_user_agent("Approved by Rime support \u{2014} continue"),
            "A browser on an unknown system"
        );
    }
}
