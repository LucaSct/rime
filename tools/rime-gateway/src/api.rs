// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The session API — the routes `rime.peekstar.eu` is, before a pixel moves.
//!
//! Four things a browser can ask for: *what can I run* (the catalogue), *start one* (`POST
//! /api/sessions`), *what is running* (`GET`), and *stop it* (`DELETE`). The surface — edit or play —
//! is chosen **at creation** and is then a property of the session, exactly as
//! [`crate::Surface`] documents; no later request can change it, because no route accepts it again.
//!
//! **The client names a catalogue entry, never a program.** This is the load-bearing decision in this
//! module. A `POST` carrying a path or an argv would make the gateway a remote-execution service with
//! a JSON front end; a `POST` carrying `"game":"hello"` can only ever start something the operator
//! put in the catalogue. So [`CatalogueEntry`] holds the `program` and `args` and the serialised form
//! omits both — the client cannot see them and could not use them if it could.
//!
//! **A reserved slot is released on every failure path.** [`Registry::try_admit`] takes the slot
//! *before* anything is spawned, which is what makes the cap real under concurrency — and it means a
//! launch that then fails must hand the slot back, or the host silently loses capacity until it
//! restarts. The [`ApiCounters::launch_failed`] counter and the test below exist because that leak is
//! invisible: the symptom is "the host is full" some hours later, with nothing running.
//!
//! **Routing is a pure function of the request.** No socket, no engine, no browser: [`SessionApi`]
//! answers a parsed [`Request`] with a [`Response`], which is what lets the whole API be tested with
//! a fake launcher. The transport is [`serve_connection`], twenty lines at the bottom.
//!
//! **WebRTC signalling is present as a refusal, not as a stub.** `/api/sessions/<id>/offer` answers
//! `501` naming the brick that will implement it (ADR-0045's "signalling, ICE and SDP before anything
//! renders"). An endpoint that answers plausibly and does nothing is worse than an absent one: a
//! client would build against it.

use std::io::{BufReader, Read, Write};
use std::path::PathBuf;

use crate::admission::{AdmissionCounters, AdmissionPolicy, Refusal, Registry, SessionId};
use crate::http::{self, json_string, parse_flat_object, Method, ParseError, Request, Response};
use crate::identity::{AccessPolicy, Principal};
use crate::Surface;

impl Surface {
    /// The wire name. One spelling, used by both the request body and every response, so a client
    /// never has to know two.
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Surface::Edit => "edit",
            Surface::Play => "play",
        }
    }

    /// Parse the wire name. Anything else is a `400`, not a default — silently defaulting an
    /// unrecognised surface to `play` (or worse, `edit`) is how a typo becomes an authoring session.
    #[must_use]
    pub fn from_wire(name: &str) -> Option<Self> {
        match name {
            "edit" => Some(Surface::Edit),
            "play" => Some(Surface::Play),
            _ => None,
        }
    }
}

/// One thing the operator has decided may be run, and how.
#[derive(Debug, Clone)]
pub struct CatalogueEntry {
    /// The client-facing name, the only part of this struct a client ever sees or sends.
    pub id: String,
    /// Shown in the browser's list.
    pub title: String,
    /// The binary. **Never serialised** — see the module note on why the client names an entry.
    pub program: PathBuf,
    /// Arguments before the supervisor's own `--editor-host <socket>`.
    pub args: Vec<String>,
    /// Which surfaces this entry offers. An exported game offers `play` only; the editor host offers
    /// both. Checked at creation, so "can this be edited?" is answered from the catalogue rather than
    /// from the request.
    pub surfaces: Vec<Surface>,
}

/// What this host offers.
#[derive(Debug, Clone, Default)]
pub struct Catalogue {
    pub entries: Vec<CatalogueEntry>,
}

impl Catalogue {
    #[must_use]
    pub fn find(&self, id: &str) -> Option<&CatalogueEntry> {
        self.entries.iter().find(|e| e.id == id)
    }

    fn to_json(&self) -> String {
        let games: Vec<String> = self
            .entries
            .iter()
            .map(|e| {
                let surfaces: Vec<String> =
                    e.surfaces.iter().map(|s| json_string(s.as_str())).collect();
                format!(
                    "{{\"id\":{},\"title\":{},\"surfaces\":[{}]}}",
                    json_string(&e.id),
                    json_string(&e.title),
                    surfaces.join(",")
                )
            })
            .collect();
        format!("{{\"games\":[{}]}}", games.join(","))
    }
}

/// How the API brings a session up once admission has said yes.
///
/// A trait rather than a direct call to [`crate::spawn_session`] so the routing can be tested without
/// a process, a socket or a GPU — the same reason admission knows nothing about spawning. The real
/// implementation is [`ProcessLauncher`].
pub trait Launcher {
    /// Whatever represents a live session. Dropping it must reap it; [`crate::SessionHandle`] does.
    type Session;

    /// Bring `entry` up for `surface`. The returned `Err` is a message for the operator's log, and
    /// the client is told only that the session could not be started.
    fn launch(&mut self, entry: &CatalogueEntry, surface: Surface)
        -> Result<Self::Session, String>;
}

/// Every routing decision, so a gateway that is answering nothing but errors can say which error.
///
/// The counter rule again (CLAUDE.md guardrail 5, applied to the gateway): `refused_admission` and
/// `launch_failed` look identical from outside — "starting a session did not work" — and mean
/// completely different things, one a full host and the other a broken deployment.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ApiCounters {
    pub requests: u64,
    pub created: u64,
    pub deleted: u64,
    pub bad_request: u64,
    pub not_found: u64,
    pub method_not_allowed: u64,
    pub unsupported_media_type: u64,
    /// Admission said no: the host is full. Not a fault.
    pub refused_admission: u64,
    /// Admission said yes and the launch then failed. Always a fault, and always accompanied by a
    /// released slot.
    pub launch_failed: u64,
    /// A route that exists on the roadmap and not yet in the code.
    pub not_implemented: u64,
    /// The request could not be parsed at all. Counted separately because these never reach a route.
    pub malformed: u64,
    /// Requests refused because this host requires an account and the request carried none.
    pub unauthenticated: u64,
    /// Requests that named a session belonging to somebody else. They are answered `404`, so without
    /// this counter they would be indistinguishable in the log from a typo — and the difference
    /// between "a client mistyped an id" and "somebody is walking the id space" is the whole point of
    /// having counters.
    pub not_owner: u64,
}

/// The routing table plus the state it decides over.
pub struct SessionApi<L: Launcher> {
    catalogue: Catalogue,
    registry: Registry<L::Session>,
    launcher: L,
    counters: ApiCounters,
    access: AccessPolicy,
}

impl<L: Launcher> SessionApi<L> {
    #[must_use]
    /// Build with the default [`AccessPolicy`], which **requires an account**. A deployment that
    /// wants ADR-0045's open LAN behaviour says so with [`Self::with_access`], where a reader can find
    /// it, rather than inheriting it from a default nobody had to write down.
    pub fn new(catalogue: Catalogue, policy: AdmissionPolicy, launcher: L) -> Self {
        Self {
            catalogue,
            registry: Registry::new(policy),
            launcher,
            counters: ApiCounters::default(),
            access: AccessPolicy::default(),
        }
    }

    /// Replace the access policy — the one call that makes this gateway serve anonymous requests.
    #[must_use]
    pub fn with_access(mut self, access: AccessPolicy) -> Self {
        self.access = access;
        self
    }

    pub fn counters(&self) -> ApiCounters {
        self.counters
    }

    #[must_use]
    pub fn admission_counters(&self) -> AdmissionCounters {
        self.registry.counters()
    }

    #[must_use]
    pub fn registry(&self) -> &Registry<L::Session> {
        &self.registry
    }

    /// Answer a request that failed to parse. Separate from [`Self::route`] because a malformed
    /// request has no method and no path, so there is nothing to route on — and because the count
    /// belongs in a different bucket than a routed refusal.
    pub fn reject(&mut self, error: &ParseError) -> Response {
        self.counters.malformed += 1;
        Response::error(error.status(), &error.to_string())
    }

    /// Route one parsed request.
    /// Route one request on behalf of `who`.
    ///
    /// The principal is an argument rather than something this function derives, because deriving it
    /// means reading a cookie and asking the store, and neither belongs in a router. The caller
    /// resolves identity once, at the connection, and hands the answer down — so there is exactly one
    /// place where a request becomes a person, and it is not here.
    pub fn route(&mut self, request: &Request, who: Principal) -> Response {
        self.counters.requests += 1;
        // Before anything else. A host that requires an account must not let an anonymous request
        // learn the catalogue, the session list, or whether an id exists.
        if self.access.require_account && !who.is_account() {
            self.counters.unauthenticated += 1;
            return Response::error(401, "this host requires an account")
                .with_header("WWW-Authenticate", "Rime-Session".to_string());
        }
        // Split once, so `/api/sessions/<id>/offer` and `/api/sessions` are the same match arm shape
        // and a trailing slash cannot produce a third path spelling that routes differently.
        let segments: Vec<&str> = request.path.split('/').filter(|s| !s.is_empty()).collect();
        match segments.as_slice() {
            ["api", "catalogue"] => self.method(request, &[Method::Get], |api, _| {
                Response::json(200, api.catalogue.to_json())
            }),
            ["api", "sessions"] => match request.method {
                Method::Get => self.list_sessions(who),
                Method::Post => self.create_session(request, who),
                _ => self.method_not_allowed(&[Method::Get, Method::Post]),
            },
            ["api", "sessions", id] => {
                let id = id.to_string();
                match request.method {
                    Method::Get => self.get_session(&id, who),
                    Method::Delete => self.delete_session(&id, who),
                    _ => self.method_not_allowed(&[Method::Get, Method::Delete]),
                }
            }
            // The signalling endpoints, honestly absent. Named here so the shape is reviewable and so
            // a client gets `501` rather than `404` — `404` would say "this is not a route", which is
            // untrue and would send someone looking for a typo.
            // The signalling endpoints, honestly absent — but ownership is checked HERE, before the
            // 501, and not left for whoever implements them. A route that learns to do something
            // before it learns who may do it is the ordering ADR-0048 brick 3 exists to prevent, and
            // there is a test asserting a stranger gets 404 rather than 501 from these.
            ["api", "sessions", id, "offer"] | ["api", "sessions", id, "answer"] => {
                if let Some(refusal) = self.owned_or_absent(id, who) {
                    return refusal;
                }
                self.counters.not_implemented += 1;
                Response::error(
                    501,
                    "WebRTC signalling is not implemented yet (Track H, the transport brick)",
                )
            }
            _ => {
                self.counters.not_found += 1;
                Response::error(404, "no such route")
            }
        }
    }

    fn method<F>(&mut self, request: &Request, allowed: &[Method], handler: F) -> Response
    where
        F: FnOnce(&mut Self, &Request) -> Response,
    {
        if allowed.contains(&request.method) {
            handler(self, request)
        } else {
            self.method_not_allowed(allowed)
        }
    }

    fn method_not_allowed(&mut self, allowed: &[Method]) -> Response {
        self.counters.method_not_allowed += 1;
        let allow = allowed
            .iter()
            .map(Method::as_str)
            .collect::<Vec<_>>()
            .join(", ");
        Response::error(405, "method not allowed").with_header("Allow", allow)
    }

    fn bad_request(&mut self, message: &str) -> Response {
        self.counters.bad_request += 1;
        Response::error(400, message)
    }

    /// Only the caller's own sessions. A list that showed everything would hand every id on the host
    /// to anyone who asked, which is the same leak `owned_or_absent` refuses one id at a time.
    fn list_sessions(&mut self, who: Principal) -> Response {
        let mut ids: Vec<(SessionId, Surface)> = self
            .registry
            .iter()
            .filter(|(_, _, owner)| *owner == who)
            .map(|(id, surface, _)| (id, surface))
            .collect();
        // Sorted, because `HashMap` iteration order is deliberately randomised per process: an
        // unsorted list would be a response that differs between identical requests, which is both
        // unpleasant to consume and untestable.
        ids.sort_by_key(|(id, _)| *id);
        let items: Vec<String> = ids
            .iter()
            .map(|(id, surface)| session_json(*id, *surface))
            .collect();
        Response::json(200, format!("{{\"sessions\":[{}]}}", items.join(",")))
    }

    /// `None` when `who` may act on the session, otherwise the refusal to return.
    ///
    /// **Somebody else's session is `404`, not `403`.** A `403` would confirm that the id exists,
    /// which turns the session list into something a stranger can enumerate one id at a time; a
    /// session id is a capability (ADR-0048 decision 4) and "this capability is real but not yours" is
    /// exactly the sentence a capability must never say.
    fn owned_or_absent(&mut self, raw: &str, who: Principal) -> Option<Response> {
        let Some(id) = parse_id(raw) else {
            return Some(self.bad_request("malformed session id"));
        };
        match self.registry.owner_of(id) {
            Some(owner) if owner == who => None,
            Some(_) => {
                self.counters.not_owner += 1;
                self.counters.not_found += 1;
                Some(Response::error(404, "no such session"))
            }
            None => {
                self.counters.not_found += 1;
                Some(Response::error(404, "no such session"))
            }
        }
    }

    fn get_session(&mut self, raw: &str, who: Principal) -> Response {
        if let Some(refusal) = self.owned_or_absent(raw, who) {
            return refusal;
        }
        let id = match parse_id(raw) {
            Some(id) => id,
            None => return self.bad_request("malformed session id"),
        };
        match self.registry.surface_of(id) {
            Some(surface) => Response::json(200, session_json(id, surface)),
            None => {
                self.counters.not_found += 1;
                Response::error(404, "no such session")
            }
        }
    }

    fn delete_session(&mut self, raw: &str, who: Principal) -> Response {
        if let Some(refusal) = self.owned_or_absent(raw, who) {
            return refusal;
        }
        let Some(id) = parse_id(raw) else {
            return self.bad_request("malformed session id");
        };
        match self.registry.release(id) {
            Some(attached) => {
                // Dropping the handle is what reaps the engine process — `SessionHandle::drop` kills
                // and then *waits*, so the child is gone rather than a zombie, before this returns.
                drop(attached);
                self.counters.deleted += 1;
                Response::empty(204)
            }
            None => {
                self.counters.not_found += 1;
                Response::error(404, "no such session")
            }
        }
    }

    fn create_session(&mut self, request: &Request, who: Principal) -> Response {
        // The media type is checked rather than sniffed. A body we would parse regardless of what the
        // client called it is a body a confused-deputy request can deliver from a form post.
        match request.header("content-type") {
            Some(ct) if ct.split(';').next().unwrap_or("").trim() == "application/json" => {}
            _ => {
                self.counters.unsupported_media_type += 1;
                return Response::error(415, "expected Content-Type: application/json");
            }
        }
        let fields = match parse_flat_object(&request.body) {
            Ok(fields) => fields,
            Err(why) => return self.bad_request(why),
        };
        let field = |name: &str| {
            fields
                .iter()
                .find(|(k, _)| k == name)
                .map(|(_, v)| v.clone())
        };
        let Some(game) = field("game") else {
            return self.bad_request("missing \"game\"");
        };
        let Some(surface_name) = field("surface") else {
            return self.bad_request("missing \"surface\"");
        };
        let Some(surface) = Surface::from_wire(&surface_name) else {
            return self.bad_request("\"surface\" must be \"edit\" or \"play\"");
        };
        let Some(entry) = self.catalogue.find(&game).cloned() else {
            self.counters.not_found += 1;
            return Response::error(404, "no such game");
        };
        if !entry.surfaces.contains(&surface) {
            return self.bad_request("that game does not offer that surface");
        }

        // The owner is fixed here, from the principal the connection proved — never from the body.
        let id = match self.registry.try_admit(surface, who) {
            Ok(id) => id,
            Err(refusal) => {
                let status = match refusal {
                    // Full is a `503`: the request was fine and will succeed later, which is exactly
                    // what `503` means and `429` (the client sent too many) does not.
                    Refusal::HostFull { .. } | Refusal::PlayFull { .. } => {
                        self.counters.refused_admission += 1;
                        503
                    }
                    // `429`, not `503`: this one IS about who is asking, and retrying without closing
                    // a session of its own will never succeed, however long the client waits.
                    Refusal::AccountFull { .. } => {
                        self.counters.refused_admission += 1;
                        429
                    }
                    Refusal::NoEntropy(_) => 500,
                };
                return Response::error(status, &refusal.to_string());
            }
        };

        match self.launcher.launch(&entry, surface) {
            Ok(session) => {
                self.registry.attach(id, session);
                self.counters.created += 1;
                Response::json(
                    201,
                    format!(
                        "{{\"session\":{},\"surface\":{},\"game\":{}}}",
                        json_string(&id.to_string()),
                        json_string(surface.as_str()),
                        json_string(&entry.id)
                    ),
                )
                .with_header("Location", format!("/api/sessions/{id}"))
            }
            Err(why) => {
                // Hand the slot back. Without this the cap leaks on every failed launch and the host
                // reports itself full with nothing running — see the module note.
                self.registry.release(id);
                self.counters.launch_failed += 1;
                // The reason goes to the log, not to the client: it names paths and argv.
                eprintln!("rime-gateway: session {id} failed to start: {why}");
                Response::error(500, "the session could not be started")
            }
        }
    }
}

fn session_json(id: SessionId, surface: Surface) -> String {
    format!(
        "{{\"session\":{},\"surface\":{}}}",
        json_string(&id.to_string()),
        json_string(surface.as_str())
    )
}

fn parse_id(raw: &str) -> Option<SessionId> {
    raw.parse().ok()
}

/// Launch a real engine process. The production [`Launcher`].
///
/// Holds the socket directory rather than a path: one socket per session, named after the session id,
/// so two concurrent sessions cannot collide on a filename — and so a leftover file names the session
/// it belonged to.
#[derive(Debug, Clone)]
pub struct ProcessLauncher {
    /// **Must not be shared with another gateway process.** The socket names come from a private
    /// counter (see [`Launcher::launch`]), which is unique within this process and says nothing about
    /// any other — two gateways sharing a directory would collide, and `spawn_session`'s stale-file
    /// removal would then delete a live session's socket.
    pub socket_dir: PathBuf,
    pub startup_timeout: std::time::Duration,
    pub deadline: std::time::Duration,
    next: u64,
}

impl ProcessLauncher {
    #[must_use]
    pub fn new(socket_dir: PathBuf) -> Self {
        Self {
            socket_dir,
            startup_timeout: std::time::Duration::from_secs(20),
            deadline: std::time::Duration::from_secs(60 * 30),
            next: 0,
        }
    }
}

impl Launcher for ProcessLauncher {
    type Session = crate::SessionHandle;

    fn launch(
        &mut self,
        entry: &CatalogueEntry,
        surface: Surface,
    ) -> Result<Self::Session, String> {
        // Named from a private counter rather than from the session id: a Unix socket path is visible
        // to anything that can read the directory, and the id is a capability (see `admission`). A
        // filename that leaks it would hand the capability to every process on the box.
        self.next += 1;
        let socket = self
            .socket_dir
            .join(format!("rime-session-{}.sock", self.next));
        let spec = crate::SessionSpec {
            program: entry.program.clone(),
            args: entry.args.clone(),
            socket,
            surface,
            startup_timeout: self.startup_timeout,
            deadline: self.deadline,
        };
        crate::spawn_session(&spec).map_err(|e| e.to_string())
    }
}

/// Read one request from `stream`, answer it, and return.
///
/// One request per connection, matching [`Response::write_to`]'s unconditional `Connection: close`.
/// Keep-alive is a thing to add with a real event loop and a read timeout, not something to leave
/// half-true in the meantime: a server that advertises keep-alive and then closes makes every client
/// retry, and one that holds the connection with no timeout is a slot exhausted by an idle peer.
/// Serve one connection on behalf of an already-resolved principal.
///
/// Identity is resolved by the caller — it needs the store, which this crate can be compiled without
/// (ADR-0048 decision 2) — and passed in, so a build with no authentication at all still routes, and
/// still enforces ownership, with every request anonymous.
pub fn serve_connection<S: Read + Write, L: Launcher>(
    api: &mut SessionApi<L>,
    stream: &mut S,
    who: Principal,
) -> std::io::Result<()> {
    let mut reader = BufReader::new(&mut *stream);
    let response = match http::read_request(&mut reader) {
        Ok(request) => api.route(&request, who),
        // A peer that connected and said nothing gets nothing back. Answering `400` to a bare TCP
        // health probe would fill the log with failures that are not failures.
        Err(ParseError::NoRequest) => return Ok(()),
        Err(e) => api.reject(&e),
    };
    response.write_to(stream)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::AccountRef;
    use std::io::Cursor;

    /// A launcher with no process behind it: enough to drive every route, and able to fail on demand
    /// so the slot-release path is testable without breaking a real binary.
    struct FakeLauncher {
        fail: bool,
        launched: Vec<(String, Surface)>,
    }

    impl Launcher for FakeLauncher {
        type Session = String;
        fn launch(
            &mut self,
            entry: &CatalogueEntry,
            surface: Surface,
        ) -> Result<Self::Session, String> {
            self.launched.push((entry.id.clone(), surface));
            if self.fail {
                Err("no such binary".into())
            } else {
                Ok(format!("{}:{}", entry.id, surface.as_str()))
            }
        }
    }

    fn catalogue() -> Catalogue {
        Catalogue {
            entries: vec![
                CatalogueEntry {
                    id: "hello".into(),
                    title: "Hello Game".into(),
                    program: "/opt/rime/hello".into(),
                    args: vec!["--quiet".into()],
                    surfaces: vec![Surface::Play],
                },
                CatalogueEntry {
                    id: "block".into(),
                    title: "The Block".into(),
                    program: "/opt/rime/block".into(),
                    args: vec![],
                    surfaces: vec![Surface::Edit, Surface::Play],
                },
            ],
        }
    }

    fn api(fail: bool) -> SessionApi<FakeLauncher> {
        SessionApi::new(
            catalogue(),
            AdmissionPolicy {
                max_sessions: 3,
                max_play_sessions: 1,
                max_sessions_per_account: 2,
            },
            FakeLauncher {
                fail,
                launched: Vec::new(),
            },
        )
        // These cases are about routing, not about identity, so they run as the open LAN host
        // ADR-0045 describes. The ownership cases below build their own API and say so.
        .with_access(AccessPolicy {
            require_account: false,
        })
    }

    fn request(raw: &str) -> Request {
        http::read_request(&mut Cursor::new(raw.as_bytes().to_vec())).unwrap()
    }

    fn post(game: &str, surface: &str) -> Request {
        let body = format!("{{\"game\":\"{game}\",\"surface\":\"{surface}\"}}");
        request(&format!(
            "POST /api/sessions HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        ))
    }

    /// An API that requires an account — the hosted deployment, as opposed to `api()`'s open LAN one.
    fn hosted() -> SessionApi<FakeLauncher> {
        SessionApi::new(
            catalogue(),
            AdmissionPolicy {
                max_sessions: 3,
                max_play_sessions: 1,
                max_sessions_per_account: 2,
            },
            FakeLauncher {
                fail: false,
                launched: Vec::new(),
            },
        )
    }

    fn account(n: u128) -> Principal {
        Principal::Account(AccountRef::from_u128(n))
    }

    fn body_of(response: &Response) -> String {
        String::from_utf8(response.body.clone()).unwrap()
    }

    fn created_id(response: &Response) -> String {
        let body = body_of(response);
        let start = body.find("\"session\":\"").unwrap() + 11;
        body[start..start + 32].to_string()
    }

    #[test]
    fn the_catalogue_never_serialises_a_program_path_or_argv() {
        // The module's central decision, as a test. If a path ever appears here, a client can see
        // what to ask for — and the next step is a client that asks for it directly.
        let response = api(false).route(
            &request("GET /api/catalogue HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(response.status, 200);
        let body = body_of(&response);
        assert!(body.contains("\"hello\""));
        assert!(body.contains("\"Hello Game\""));
        assert!(body.contains("\"surfaces\":[\"edit\",\"play\"]"));
        assert!(!body.contains("/opt/rime"), "leaked a program path: {body}");
        assert!(!body.contains("--quiet"), "leaked argv: {body}");
    }

    #[test]
    fn creating_a_session_assigns_the_surface_and_returns_a_capability() {
        let mut api = api(false);
        let response = api.route(&post("hello", "play"), Principal::Anonymous);
        assert_eq!(response.status, 201);
        let id = created_id(&response);
        assert_eq!(id.len(), 32, "a session id is 128 random bits in hex");
        assert!(body_of(&response).contains("\"surface\":\"play\""));
        assert!(response
            .extra
            .iter()
            .any(|(k, v)| *k == "Location" && v == &format!("/api/sessions/{id}")));
        assert_eq!(api.counters().created, 1);
        assert_eq!(api.launcher.launched, vec![("hello".into(), Surface::Play)]);
    }

    #[test]
    fn a_session_can_be_fetched_and_deleted_and_then_is_gone() {
        let mut api = api(false);
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        let path = format!("/api/sessions/{id}");

        assert_eq!(
            api.route(
                &request(&format!("GET {path} HTTP/1.1\r\n\r\n")),
                Principal::Anonymous
            )
            .status,
            200
        );
        let list = api.route(
            &request("GET /api/sessions HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        );
        assert!(body_of(&list).contains(&id));

        let deleted = api.route(
            &request(&format!("DELETE {path} HTTP/1.1\r\n\r\n")),
            Principal::Anonymous,
        );
        assert_eq!(deleted.status, 204);
        assert!(deleted.body.is_empty());
        // Gone means gone: the slot is free AND the id no longer resolves. A `DELETE` that only freed
        // the slot would leave a live id addressing a released session.
        assert_eq!(
            api.route(
                &request(&format!("GET {path} HTTP/1.1\r\n\r\n")),
                Principal::Anonymous
            )
            .status,
            404
        );
        assert_eq!(
            api.route(
                &request(&format!("DELETE {path} HTTP/1.1\r\n\r\n")),
                Principal::Anonymous
            )
            .status,
            404
        );
        assert_eq!(api.counters().deleted, 1);
        assert!(api.registry().is_empty());
    }

    #[test]
    fn a_failed_launch_gives_the_slot_back() {
        // The leak this guards is invisible from outside: without the release, the play cap is
        // consumed by a session that never existed, and the host reports itself full forever.
        let mut api = api(true);
        assert_eq!(
            api.route(&post("hello", "play"), Principal::Anonymous)
                .status,
            500
        );
        assert_eq!(api.counters().launch_failed, 1);
        assert!(api.registry().is_empty(), "the slot leaked");
        assert_eq!(api.admission_counters().released, 1);
        // And the cap is genuinely still available afterwards: the SAME api, whose launcher now
        // succeeds, admits a play session — which it could not if the slot were still held.
        api.launcher.fail = false;
        assert_eq!(
            api.route(&post("hello", "play"), Principal::Anonymous)
                .status,
            201
        );
    }

    #[test]
    fn the_play_cap_is_enforced_and_is_a_503_not_a_400() {
        let mut api = api(false);
        assert_eq!(
            api.route(&post("hello", "play"), Principal::Anonymous)
                .status,
            201
        );
        let second = api.route(&post("block", "play"), Principal::Anonymous);
        assert_eq!(second.status, 503, "{}", body_of(&second));
        assert_eq!(api.counters().refused_admission, 1);
        // An edit session is cheaper and still admitted, which is the whole reason the two caps are
        // separate rather than one number.
        assert_eq!(
            api.route(&post("block", "edit"), Principal::Anonymous)
                .status,
            201
        );
    }

    #[test]
    fn the_total_cap_is_enforced_after_the_play_cap() {
        let mut api = api(false);
        assert_eq!(
            api.route(&post("block", "edit"), Principal::Anonymous)
                .status,
            201
        );
        assert_eq!(
            api.route(&post("block", "edit"), Principal::Anonymous)
                .status,
            201
        );
        assert_eq!(
            api.route(&post("block", "edit"), Principal::Anonymous)
                .status,
            201
        );
        let full = api.route(&post("block", "edit"), Principal::Anonymous);
        assert_eq!(full.status, 503);
        assert!(body_of(&full).contains("host is full"));
    }

    #[test]
    fn a_client_cannot_name_a_program_only_a_catalogue_entry() {
        let mut api = api(false);
        // An absolute path is not an entry id, and there is no field that would carry one.
        let body = "{\"game\":\"/bin/sh\",\"surface\":\"play\"}";
        let req = request(&format!(
            "POST /api/sessions HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        ));
        assert_eq!(api.route(&req, Principal::Anonymous).status, 404);
        assert!(api.launcher.launched.is_empty(), "attempted a launch");
    }

    #[test]
    fn a_surface_the_entry_does_not_offer_is_refused() {
        // `hello` is an exported game: play only. Editing it is not a missing feature, it is a
        // property of the catalogue entry — so the answer comes from the catalogue, not the client.
        let mut api = api(false);
        let response = api.route(&post("hello", "edit"), Principal::Anonymous);
        assert_eq!(response.status, 400);
        assert!(body_of(&response).contains("does not offer"));
        assert!(api.launcher.launched.is_empty());
        assert!(api.registry().is_empty(), "the slot leaked on a refusal");
    }

    #[test]
    fn an_unknown_surface_name_is_refused_rather_than_defaulted() {
        let mut api = api(false);
        for name in ["Edit", "EDIT", "author", "", "play "] {
            let response = api.route(&post("block", name), Principal::Anonymous);
            assert_eq!(response.status, 400, "accepted surface {name:?}");
        }
        assert!(api.launcher.launched.is_empty());
    }

    #[test]
    fn a_post_without_the_json_media_type_is_refused() {
        let mut api = api(false);
        let body = "{\"game\":\"hello\",\"surface\":\"play\"}";
        let req = request(&format!(
            "POST /api/sessions HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        ));
        assert_eq!(api.route(&req, Principal::Anonymous).status, 415);
        // A charset parameter is not a different media type, though.
        let req = request(&format!(
            "POST /api/sessions HTTP/1.1\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        ));
        assert_eq!(api.route(&req, Principal::Anonymous).status, 201);
    }

    #[test]
    fn a_malformed_session_id_is_a_400_and_a_well_formed_unknown_one_is_a_404() {
        // The distinction matters: `400` says "that is not an id", `404` says "that id is not here",
        // and collapsing them would let a client probe the id space by watching the status.
        let mut api = api(false);
        assert_eq!(
            api.route(
                &request("GET /api/sessions/nope HTTP/1.1\r\n\r\n"),
                Principal::Anonymous
            )
            .status,
            400
        );
        assert_eq!(
            api.route(
                &request("GET /api/sessions/00000000000000000000000000000000 HTTP/1.1\r\n\r\n"),
                Principal::Anonymous
            )
            .status,
            404
        );
    }

    #[test]
    fn wrong_methods_answer_405_with_allow() {
        let mut api = api(false);
        let response = api.route(
            &request("DELETE /api/sessions HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(response.status, 405);
        assert!(response
            .extra
            .iter()
            .any(|(k, v)| *k == "Allow" && v == "GET, POST"));
        let response = api.route(
            &request("POST /api/catalogue HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(response.status, 405);
        assert_eq!(api.counters().method_not_allowed, 2);
    }

    #[test]
    fn signalling_answers_501_rather_than_404_or_a_plausible_stub() {
        let mut api = api(false);
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        let response = api.route(
            &request(&format!("POST /api/sessions/{id}/offer HTTP/1.1\r\n\r\n")),
            Principal::Anonymous,
        );
        assert_eq!(response.status, 501);
        assert_eq!(api.counters().not_implemented, 1);
    }

    #[test]
    fn unknown_routes_and_trailing_slashes_do_not_produce_two_spellings() {
        let mut api = api(false);
        assert_eq!(
            api.route(
                &request("GET /api/nothing HTTP/1.1\r\n\r\n"),
                Principal::Anonymous
            )
            .status,
            404
        );
        // `/api/sessions/` must be the collection, not a session whose id is the empty string.
        let response = api.route(
            &request("GET /api/sessions/ HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(response.status, 200);
        assert!(body_of(&response).contains("\"sessions\""));
    }

    #[test]
    fn the_listing_is_ordered_so_two_identical_requests_agree() {
        // `HashMap` iteration is randomised per process, so an unsorted listing would differ between
        // calls within one run — untestable, and unpleasant for a browser diffing it.
        let mut api = api(false);
        api.route(&post("block", "edit"), Principal::Anonymous);
        api.route(&post("block", "edit"), Principal::Anonymous);
        api.route(&post("hello", "play"), Principal::Anonymous);
        let first = body_of(&api.route(
            &request("GET /api/sessions HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        ));
        let second = body_of(&api.route(
            &request("GET /api/sessions HTTP/1.1\r\n\r\n"),
            Principal::Anonymous,
        ));
        assert_eq!(first, second);
        assert_eq!(first.matches("\"session\":").count(), 3);
    }

    #[test]
    fn a_malformed_request_is_answered_from_its_parse_error() {
        let mut api = api(false);
        let raw = format!("POST /x HTTP/1.1\r\nContent-Length: {}\r\n\r\n", 1 << 30);
        let err = http::read_request(&mut Cursor::new(raw.into_bytes())).unwrap_err();
        let response = api.reject(&err);
        assert_eq!(response.status, 413);
        assert_eq!(api.counters().malformed, 1);
        // A malformed request never reached a route, so it must not be counted as one.
        assert_eq!(api.counters().requests, 0);
    }

    #[test]
    fn serve_connection_answers_over_a_transport() {
        // End-to-end over one in-memory stream: parse, route, respond. No socket and no engine, which
        // is what keeps this runnable in CI on every OS.
        struct Pipe {
            inbound: Cursor<Vec<u8>>,
            outbound: Vec<u8>,
        }
        impl Read for Pipe {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                self.inbound.read(buf)
            }
        }
        impl Write for Pipe {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                self.outbound.extend_from_slice(buf);
                Ok(buf.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        let mut api = api(false);
        let mut pipe = Pipe {
            inbound: Cursor::new(b"GET /api/catalogue HTTP/1.1\r\nHost: rime\r\n\r\n".to_vec()),
            outbound: Vec::new(),
        };
        serve_connection(&mut api, &mut pipe, Principal::Anonymous).unwrap();
        let text = String::from_utf8(pipe.outbound).unwrap();
        assert!(text.starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(text.contains("Content-Type: application/json\r\n"));
        assert!(text.contains("\"games\""));

        // A peer that says nothing gets no response at all, rather than a logged 400.
        let mut silent = Pipe {
            inbound: Cursor::new(Vec::new()),
            outbound: Vec::new(),
        };
        serve_connection(&mut api, &mut silent, Principal::Anonymous).unwrap();
        assert!(silent.outbound.is_empty());
    }

    // THE SECOND-ACCOUNT TEST (ADR-0048 brick 3). One account creates a session; another account,
    // holding its real id, must not be able to read it, delete it, signal on it, or even learn that it
    // exists. Everything about ownership in this module is here to make this case fail closed.
    #[test]
    fn a_second_account_can_neither_see_nor_touch_the_first_ones_session() {
        let mut api = hosted();
        let alice = account(1);
        let bob = account(2);

        let created = api.route(&post("block", "edit"), alice);
        assert_eq!(created.status, 201);
        let id = created_id(&created);

        // Reading it: 404, not 403 — a 403 would confirm the id is real.
        let get = api.route(
            &request(&format!("GET /api/sessions/{id} HTTP/1.1\r\n\r\n")),
            bob,
        );
        assert_eq!(get.status, 404);
        // Deleting it: refused, and the session is still there afterwards.
        let delete = api.route(
            &request(&format!("DELETE /api/sessions/{id} HTTP/1.1\r\n\r\n")),
            bob,
        );
        assert_eq!(delete.status, 404);
        // Listing: Bob sees nothing at all, and Alice still sees hers.
        let bobs = api.route(&request("GET /api/sessions HTTP/1.1\r\n\r\n"), bob);
        assert_eq!(body_of(&bobs), "{\"sessions\":[]}");
        let alices = api.route(&request("GET /api/sessions HTTP/1.1\r\n\r\n"), alice);
        assert!(body_of(&alices).contains(&id));
        // And the owner can still delete it, so the refusals above were about Bob and not about the
        // session having quietly broken.
        let owner_delete = api.route(
            &request(&format!("DELETE /api/sessions/{id} HTTP/1.1\r\n\r\n")),
            alice,
        );
        assert_eq!(owner_delete.status, 204);
        assert_eq!(api.counters().not_owner, 2);
    }

    // The signalling routes are 501s today. Ownership is checked BEFORE the 501, so the check cannot
    // be forgotten by whoever implements them — and a stranger cannot use a 501-vs-404 difference to
    // learn that an id exists.
    #[test]
    fn signalling_refuses_a_stranger_before_it_answers_not_implemented() {
        let mut api = hosted();
        let alice = account(1);
        let created = api.route(&post("block", "edit"), alice);
        let id = created_id(&created);

        for verb in ["offer", "answer"] {
            let owner = api.route(
                &request(&format!("POST /api/sessions/{id}/{verb} HTTP/1.1\r\n\r\n")),
                alice,
            );
            assert_eq!(
                owner.status, 501,
                "the owner reaches the unimplemented route"
            );
            let stranger = api.route(
                &request(&format!("POST /api/sessions/{id}/{verb} HTTP/1.1\r\n\r\n")),
                account(2),
            );
            assert_eq!(stranger.status, 404, "a stranger does not learn it exists");
        }
    }

    #[test]
    fn a_host_that_requires_an_account_refuses_every_anonymous_route() {
        let mut api = hosted();
        for raw in [
            "GET /api/catalogue HTTP/1.1\r\n\r\n",
            "GET /api/sessions HTTP/1.1\r\n\r\n",
            "GET /api/sessions/0123456789abcdef0123456789abcdef HTTP/1.1\r\n\r\n",
        ] {
            let response = api.route(&request(raw), Principal::Anonymous);
            assert_eq!(response.status, 401, "anonymous: {raw}");
        }
        assert_eq!(
            api.route(&post("block", "edit"), Principal::Anonymous)
                .status,
            401
        );
        assert_eq!(api.counters().unauthenticated, 4);
        assert!(api.registry().is_empty(), "nothing was admitted");
    }

    // The cap is per account and it is not the host cap: the host still has a free slot when the
    // second account's first session is admitted, which is what makes this a fair-share rule rather
    // than a queue.
    #[test]
    fn an_account_is_capped_before_the_host_is() {
        let mut api = hosted();
        let alice = account(1);
        assert_eq!(api.route(&post("block", "edit"), alice).status, 201);
        assert_eq!(api.route(&post("block", "edit"), alice).status, 201);
        // 429, not 503: waiting will never help, closing one of her own will.
        let third = api.route(&post("block", "edit"), alice);
        assert_eq!(third.status, 429);
        // The host itself is not full — somebody else gets the remaining slot.
        assert_eq!(api.route(&post("block", "edit"), account(2)).status, 201);
        assert_eq!(api.admission_counters().refused_account_full, 1);
    }

    // An open LAN host has exactly one principal, so a per-account cap applied to it would cap the
    // whole host at two. It is deliberately not applied to anonymous requests.
    #[test]
    fn the_per_account_cap_does_not_shut_down_an_anonymous_host() {
        let mut api = api(false);
        for _ in 0..3 {
            assert_eq!(
                api.route(&post("block", "edit"), Principal::Anonymous)
                    .status,
                201
            );
        }
        // Three admitted, so the refusal that follows is the HOST cap and not the account one.
        let fourth = api.route(&post("block", "edit"), Principal::Anonymous);
        assert_eq!(fourth.status, 503);
        assert_eq!(api.admission_counters().refused_account_full, 0);
    }
}
