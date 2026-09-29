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
//! **Signalling is one round trip, and it is the only place a relay is born.** `POST
//! /api/sessions/<id>/offer` takes the browser's SDP offer and answers with the SDP answer
//! (`contract.md`); there is no `/answer` route, because ICE-lite plus non-trickle ICE means the
//! exchange is finished when the answer arrives. Everything that makes a media session expensive — a
//! forwarded UDP port, a public address, two threads, a running engine — is acquired here, in that
//! order, and every failure path after the first acquisition hands it back. That is the same
//! discipline as [`Registry::try_admit`]'s slot, applied to three more resources.
//!
//! **A relay that stopped on its own frees its slot on the next request.** The alternative — a
//! background reaper thread — would need the router's lock from outside the router, and the only
//! moment the truth actually matters is when somebody asks. So [`SessionApi::route`] sweeps first
//! (see `sweep_stopped_relays`), which is why a session whose engine exited answers `404` rather than
//! holding one of three slots until a timer fires.

use std::io::{BufReader, Read, Write};
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;

use crate::admission::{AdmissionCounters, AdmissionPolicy, Refusal, Registry, SessionId};
use crate::http::{self, json_string, parse_flat_object, Method, ParseError, Request, Response};
use crate::identity::{AccessPolicy, Principal};
use crate::media::{MediaPorts, PublicAddress};
use crate::relay::{
    IceProvider, MediaSession, NoIceServers, Relay, RelayConfig, StopReason, TransportFactory,
};
use crate::transport::TransportError;
use crate::turn::ice_servers_json;
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
    ///
    /// `MediaSession` because a session that cannot hand the relay its engine socket cannot carry
    /// video, and the bound belongs here rather than on the routes: a launcher whose sessions have no
    /// socket is a deployment that can never answer an offer, and that is worth refusing at compile
    /// time. `Send + 'static` because the relay takes ownership of the session as the guard whose
    /// drop reaps the engine, on a thread of its own.
    type Session: MediaSession + Send + 'static;

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
    /// Offers refused because this deployment has no media configuration at all. A deployment fault,
    /// and distinguishable from the two below — which are a busy host and a broken network.
    pub media_unconfigured: u64,
    /// Offers refused because every forwarded UDP port was leased. The host is full of media, which
    /// is not the same as full of sessions: the session cap and the port pool are separate numbers.
    pub media_ports_exhausted: u64,
    /// Offers refused because the public address could not be resolved or was not usable.
    pub public_address_failed: u64,
    /// Offers refused because the SDP could not be negotiated. The client's fault, usually a browser
    /// that offered no AV1.
    pub offer_rejected: u64,
    /// Relays started. The count of sessions that actually reached the point of carrying pixels,
    /// which `created` does not tell you.
    pub relays_started: u64,
    /// Relays stopped by this router — a `DELETE`, or the sweep finding one already dead.
    pub relays_stopped: u64,
    /// Second offers on a session that already has a live relay.
    pub offer_conflict: u64,
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

/// Everything the offer route needs, and the one object whose absence makes a deployment
/// media-less.
///
/// A single struct rather than four optional fields, because the four are only ever useful together:
/// a port pool with no public address advertises an unreachable candidate, and a public address with
/// no pool has nothing to advertise. `Option<MediaConfig>` is therefore exactly the question the
/// route asks — "can this host carry media at all?" — with no half-configured third state.
pub struct MediaConfig {
    /// The forwarded UDP ports (ADR-0053 decision 1). One lease per relay.
    pub ports: MediaPorts,
    /// What the browser is told to send to. Resolved per offer, because a home address changes.
    pub public: Box<dyn PublicAddress>,
    /// How a transport is built. Injected so the tests here need no ICE.
    pub factory: Box<dyn TransportFactory>,
    /// Carried into every relay this host starts.
    pub relay: RelayConfig,
}

/// One admitted session's live parts.
///
/// The session is an `Option` because starting a relay **moves it into the relay**, as the guard whose
/// drop reaps the engine process. Two owners for one child process is how a gateway ends up either
/// leaking engines or killing one while a thread is still reading its socket, so there is exactly one:
/// before signalling the slot holds it, and afterwards the relay does.
///
/// `pub` only because [`SessionApi::registry`] is: its fields are this module's bookkeeping, and the
/// numbers a consumer wants are in [`ApiCounters`] and [`AdmissionCounters`].
pub struct Live<S> {
    session: Option<S>,
    relay: Option<Relay>,
}

/// The routing table plus the state it decides over.
pub struct SessionApi<L: Launcher> {
    catalogue: Catalogue,
    registry: Registry<Live<L::Session>>,
    launcher: L,
    counters: ApiCounters,
    access: AccessPolicy,
    /// `None` on a host with no media flags: every offer is then a `503` that says so, rather than a
    /// route that appears to work and answers an SDP nothing can reach.
    media: Option<MediaConfig>,
    ice: Box<dyn IceProvider>,
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
            media: None,
            ice: Box::new(NoIceServers),
        }
    }

    /// Replace the access policy — the one call that makes this gateway serve anonymous requests.
    #[must_use]
    pub fn with_access(mut self, access: AccessPolicy) -> Self {
        self.access = access;
        self
    }

    /// Give this host a media path. Without it, `offer` answers `503`.
    #[must_use]
    pub fn with_media(mut self, media: Option<MediaConfig>) -> Self {
        self.media = media;
        self
    }

    /// Replace the ICE-server source. The default hands out an empty list, which is the honest answer
    /// for a deployment with no TURN relay.
    #[must_use]
    pub fn with_ice(mut self, ice: Box<dyn IceProvider>) -> Self {
        self.ice = ice;
        self
    }

    pub fn counters(&self) -> ApiCounters {
        self.counters
    }

    #[must_use]
    pub fn admission_counters(&self) -> AdmissionCounters {
        self.registry.counters()
    }

    /// The admitted set. Its value type is this module's own bookkeeping — a session, and a relay once
    /// one exists — so a caller reads its size and nothing else.
    #[must_use]
    pub fn registry(&self) -> &Registry<Live<L::Session>> {
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
        // Before the access check, because a slot held by a dead relay is wrong whatever the answer to
        // this request turns out to be — and because an anonymous request on a host that requires an
        // account must not be the one thing that leaves a corpse in the registry.
        self.sweep_stopped_relays();
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
            // Signalling. **Ownership is checked first, before the method and before the body** —
            // the ordering ADR-0048 brick 3 exists to enforce, kept here now that these routes do
            // something: a stranger must learn nothing from them, not even that the id is real, and a
            // `415` on somebody else's session would already have said too much.
            //
            // There is no `/answer`. ICE-lite plus non-trickle ICE makes the exchange one round trip
            // (`contract.md`), so a second route would be a step with nothing to do — and a client
            // built against it would wait for a message that never comes.
            ["api", "sessions", id, "offer"] => {
                if let Some(refusal) = self.owned_or_absent(id, who) {
                    return refusal;
                }
                let id = id.to_string();
                match request.method {
                    Method::Post => self.offer(&id, request),
                    _ => self.method_not_allowed(&[Method::Post]),
                }
            }
            ["api", "sessions", id, "ice"] => {
                if let Some(refusal) = self.owned_or_absent(id, who) {
                    return refusal;
                }
                let id = id.to_string();
                match request.method {
                    Method::Get => {
                        Response::json(200, ice_servers_json(&self.ice.ice_servers(&id)))
                    }
                    _ => self.method_not_allowed(&[Method::Get]),
                }
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
        // The relay is stopped BEFORE the slot is released, and explicitly rather than by dropping it.
        // Both matter: `Relay::stop` joins its two threads and closes the transport, so when this
        // returns nothing is still reading a socket whose engine is about to be killed — and the
        // reason and counters reach the log instead of disappearing into drop glue.
        if let Some(relay) = self
            .registry
            .get_mut(id)
            .and_then(|live| live.relay.as_mut())
        {
            let report = relay.stop(StopReason::Requested);
            eprintln!(
                "rime-gateway: session {id} stopped by request ({:?}); {:?}",
                report.reason, report.relay
            );
            self.counters.relays_stopped += 1;
        }
        match self.registry.release(id) {
            Some(attached) => {
                // Dropping this is what reaps the engine process — `SessionHandle::drop` kills and
                // then *waits*, so the child is gone rather than a zombie, before this returns.
                // Whether the handle sits in the slot or inside a relay that has just stopped, the
                // drop chain ends at the same place.
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

    /// Release every slot whose relay has stopped on its own.
    ///
    /// A relay stops without being asked when the engine exits, the browser goes away, the control
    /// path stalls or negotiation finds no common codec. The slot it was holding is then worth
    /// nothing, but nobody has told the registry — and on a host with three slots (ADR-0047 §3) two
    /// corpses are two thirds of the capacity.
    ///
    /// Swept at the top of routing rather than by a reaper thread: the registry lives behind the
    /// router's lock, so a thread would have to reach through it from outside, and the only moment the
    /// answer matters is when somebody asks. The visible consequence is the right one — a session
    /// whose engine exited answers `404` on the next request instead of `200` for a corpse.
    fn sweep_stopped_relays(&mut self) {
        for id in self.registry.ids() {
            let stopped = self
                .registry
                .get_mut(id)
                .and_then(|live| live.relay.as_ref())
                .is_some_and(Relay::is_stopped);
            if !stopped {
                continue;
            }
            if let Some(mut live) = self.registry.release(id).flatten() {
                // Stopped explicitly rather than by dropping it, so the threads are joined and the
                // reason is logged here instead of vanishing into drop glue.
                if let Some(relay) = live.relay.as_mut() {
                    let report = relay.stop(StopReason::Requested);
                    eprintln!(
                        "rime-gateway: session {id} relay ended ({:?}); {:?}",
                        report.reason, report.relay
                    );
                }
                self.counters.relays_stopped += 1;
            }
        }
    }

    /// `POST /api/sessions/<id>/offer` — the whole signalling exchange.
    ///
    /// Ownership has already been checked by the caller. What is left is, in order: is this host able
    /// to carry media at all, is the request shaped like an offer, does this session already have one,
    /// and only then the four acquisitions — port, public address, transport, relay. **Every failure
    /// after the lease hands the lease back**, which is what `PortLease`'s `Drop` is for: the `?`-less
    /// early returns below each drop it on the way out, so there is no path that loses a forwarded
    /// port.
    fn offer(&mut self, raw: &str, request: &Request) -> Response {
        // A host with no media flags. `503` and not `501`: the route is implemented, this deployment
        // simply has nowhere to put the media, and the operator is the one who can fix it.
        if self.media.is_none() {
            self.counters.media_unconfigured += 1;
            return Response::error(503, "media is not configured");
        }
        // Checked, not sniffed — the same rule as `create_session`. A body we would parse whatever the
        // client called it is a body a form post can deliver.
        match request.header("content-type") {
            Some(ct) if ct.split(';').next().unwrap_or("").trim() == "application/sdp" => {}
            _ => {
                self.counters.unsupported_media_type += 1;
                return Response::error(415, "expected Content-Type: application/sdp");
            }
        }
        if request.body.is_empty() {
            return self.bad_request("the body must be the browser's SDP offer");
        }
        let Ok(sdp) = std::str::from_utf8(&request.body) else {
            return self.bad_request("the SDP offer is not UTF-8");
        };
        let sdp = sdp.to_string();
        let Some(id) = parse_id(raw) else {
            return self.bad_request("malformed session id");
        };

        // A live relay already. `409` rather than replacing it: a second offer is a client that lost
        // track of its own connection, and tearing down a working session to serve the duplicate would
        // let one stray retry blank somebody's screen. (A relay that has *stopped* was already swept
        // away by `route`, so this only ever refuses a live one.)
        match self.registry.get_mut(id) {
            Some(live) => {
                if live.relay.is_some() {
                    self.counters.offer_conflict += 1;
                    return Response::error(409, "this session already has a transport");
                }
            }
            None => {
                self.counters.not_found += 1;
                return Response::error(404, "no such session");
            }
        }

        let media = self.media.as_mut().expect("checked above");
        // One of the three forwarded UDP ports (ADR-0053 decision 1). A socket on any other port is
        // unreachable from the internet however well it negotiates, so an exhausted pool is a `503`
        // and not an ephemeral port.
        let Some(lease) = media.ports.lease() else {
            self.counters.media_ports_exhausted += 1;
            return Response::error(503, "no media port is free");
        };
        // Resolved per offer rather than cached: the home address changes, and a stale answer is a
        // candidate the browser sends to somebody else.
        let public = match media.public.resolve() {
            Ok(ip) => ip,
            Err(why) => {
                eprintln!("rime-gateway: public address unavailable: {why}");
                self.counters.public_address_failed += 1;
                return Response::error(503, "the public media address is unavailable");
            }
        };
        let bind = lease.addr();
        // The public port is the LEASED one: the edge forwards port to identical port (ADR-0053), so
        // advertising anything else would advertise a path that does not exist.
        let public = SocketAddr::new(IpAddr::V4(public), bind.port());
        let (transport, answer) = match media.factory.accept(&sdp, bind, public) {
            Ok(accepted) => accepted,
            Err(error) => {
                eprintln!("rime-gateway: could not accept the offer: {error}");
                // A bad or unnegotiable offer is the client's: a browser that offered no AV1, or SDP
                // that does not parse. A socket that would not bind is ours.
                return match error {
                    TransportError::BadOffer(_) | TransportError::Negotiation(_) => {
                        self.counters.offer_rejected += 1;
                        self.counters.bad_request += 1;
                        Response::error(400, "the SDP offer could not be accepted")
                    }
                    _ => Response::error(503, "the media transport could not be started"),
                };
            }
        };
        let relay_config = media.relay;

        let Some(live) = self.registry.get_mut(id) else {
            // Released between the check above and here. Impossible under the router's lock today,
            // and handled anyway rather than unwrapped: this function must not be the reason a
            // concurrency change becomes a panic.
            self.counters.not_found += 1;
            return Response::error(404, "no such session");
        };
        let halves = live
            .session
            .as_mut()
            .ok_or_else(|| "the session has already been handed to a relay".to_string())
            .and_then(|session| session.engine_halves().map_err(|e| e.to_string()));
        let (reader, writer) = match halves {
            Ok(halves) => halves,
            Err(why) => {
                eprintln!("rime-gateway: session {id} cannot be relayed: {why}");
                return Response::error(500, "the session could not be connected");
            }
        };
        // The session moves into the relay as the guard whose drop reaps the engine — see `Live`.
        let guard = live.session.take().expect("present, just borrowed");
        live.relay = Some(Relay::start(
            reader,
            writer,
            transport,
            lease,
            Box::new(guard),
            relay_config,
        ));
        self.counters.relays_started += 1;
        Response::raw(200, "application/sdp", answer.into_bytes())
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
                self.registry.attach(
                    id,
                    Live {
                        session: Some(session),
                        relay: None,
                    },
                );
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
    use crate::media::FixedPublicAddress;
    use crate::relay::MediaTransport;
    use crate::transport::{Command, Event, TransportCounters};
    use crate::turn::TurnConfig;
    use crate::Session;
    use std::io::Cursor;
    use std::net::Ipv4Addr;
    use std::os::unix::net::UnixStream;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    /// A session with no process behind it: a socket pair, so the relay has something real to read and
    /// write, and the engine end kept where the test can watch it.
    ///
    /// This is the shape `SessionHandle` has in production — a child plus a filtered connection to it —
    /// with the child left out, which is exactly the part these tests are not about.
    struct FakeSession {
        gateway: UnixStream,
        surface: Surface,
    }

    impl MediaSession for FakeSession {
        fn engine_halves(&mut self) -> std::io::Result<(Session<UnixStream>, Session<UnixStream>)> {
            Ok((
                Session::new(self.gateway.try_clone()?, self.surface),
                Session::new(self.gateway.try_clone()?, self.surface),
            ))
        }
    }

    /// A launcher with no process behind it: enough to drive every route, and able to fail on demand
    /// so the slot-release path is testable without breaking a real binary.
    struct FakeLauncher {
        fail: bool,
        launched: Vec<(String, Surface)>,
        /// The engine ends of everything launched, in order, for the tests that need to see what
        /// reached the engine.
        engines: Vec<UnixStream>,
    }

    impl Launcher for FakeLauncher {
        type Session = FakeSession;
        fn launch(
            &mut self,
            entry: &CatalogueEntry,
            surface: Surface,
        ) -> Result<Self::Session, String> {
            self.launched.push((entry.id.clone(), surface));
            if self.fail {
                return Err("no such binary".into());
            }
            let (engine, gateway) = UnixStream::pair().map_err(|e| e.to_string())?;
            engine
                .set_read_timeout(Some(std::time::Duration::from_secs(5)))
                .map_err(|e| e.to_string())?;
            // The ENGINE end is kept by the launcher and by nobody else. That is deliberate: the
            // session is moved into its relay as the reaping guard, so a copy of the engine end
            // inside it would keep the socket open after the "engine" was supposed to have exited,
            // and the sweep test could never observe an EOF.
            self.engines.push(engine);
            Ok(FakeSession { gateway, surface })
        }
    }

    /// A transport that negotiates nothing, binds nothing and remembers whether it was closed.
    ///
    /// The routes are about status codes, ownership and slot bookkeeping; none of that should need ICE
    /// to be testable, and a test that stood up a real connection to check a `409` would be proving
    /// `str0m` instead of the router.
    #[derive(Default)]
    struct FakeTransport {
        closed: Arc<AtomicBool>,
    }

    impl MediaTransport for FakeTransport {
        fn submit(&self, _command: Command) -> Result<(), TransportError> {
            Ok(())
        }

        fn poll_event(&self, timeout: std::time::Duration) -> Option<Event> {
            // Blocks like the real one rather than returning at once, so the relay's browser-side
            // thread does not spin a core for the length of a test.
            std::thread::sleep(timeout);
            None
        }

        fn close(&mut self) -> TransportCounters {
            self.closed.store(true, Ordering::Release);
            TransportCounters::default()
        }
    }

    /// Hands out `FakeTransport`s and keeps every "was it closed?" flag, so a test can prove that a
    /// `DELETE` actually tore the transport down rather than merely forgetting about it.
    #[derive(Default, Clone)]
    struct FakeFactory {
        /// The SDP to answer with, and the offers it was asked to accept.
        offers: Arc<Mutex<Vec<String>>>,
        closed: Arc<Mutex<Vec<Arc<AtomicBool>>>>,
        /// When set, every `accept` fails with it — the browser-offered-no-AV1 case.
        refuse: Arc<Mutex<Option<String>>>,
    }

    impl FakeFactory {
        fn all_closed(&self) -> bool {
            let closed = self.closed.lock().unwrap();
            !closed.is_empty() && closed.iter().all(|f| f.load(Ordering::Acquire))
        }
    }

    impl TransportFactory for FakeFactory {
        fn accept(
            &self,
            offer_sdp: &str,
            _bind: SocketAddr,
            _public: SocketAddr,
        ) -> Result<(Box<dyn MediaTransport>, String), TransportError> {
            if let Some(why) = self.refuse.lock().unwrap().clone() {
                return Err(TransportError::Negotiation(why));
            }
            self.offers.lock().unwrap().push(offer_sdp.to_string());
            let transport = FakeTransport::default();
            self.closed
                .lock()
                .unwrap()
                .push(Arc::clone(&transport.closed));
            Ok((
                Box::new(transport),
                "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n".to_string(),
            ))
        }
    }

    fn media(factory: FakeFactory) -> MediaConfig {
        MediaConfig {
            ports: MediaPorts::new(IpAddr::V4(Ipv4Addr::new(10, 77, 0, 22)), 50000..=50001)
                .unwrap(),
            public: Box::new(FixedPublicAddress(Ipv4Addr::new(95, 89, 215, 226))),
            factory: Box::new(factory),
            relay: RelayConfig::default(),
        }
    }

    /// An offer request for `id`, with the media type and body the contract specifies.
    fn offer_request(id: &str, sdp: &str) -> Request {
        request(&format!(
            "POST /api/sessions/{id}/offer HTTP/1.1\r\nContent-Type: application/sdp\r\nContent-Length: {}\r\n\r\n{}",
            sdp.len(),
            sdp
        ))
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
                engines: Vec::new(),
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
                engines: Vec::new(),
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
    fn the_offer_route_requires_sdp() {
        // The media type is checked and not sniffed, for the same reason `POST /api/sessions` checks
        // it: a body we would parse whatever the client called it is a body a form post can deliver.
        let factory = FakeFactory::default();
        let mut api = api(false).with_media(Some(media(factory.clone())));
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));

        let wrong_type = api.route(
            &request(&format!(
                "POST /api/sessions/{id}/offer HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{{}}"
            )),
            Principal::Anonymous,
        );
        assert_eq!(wrong_type.status, 415);
        let empty = api.route(
            &request(&format!(
                "POST /api/sessions/{id}/offer HTTP/1.1\r\nContent-Type: application/sdp\r\n\r\n"
            )),
            Principal::Anonymous,
        );
        assert_eq!(empty.status, 400);
        // Neither refusal started anything: an offer that never reached the transport must not have
        // consumed a forwarded port either.
        assert!(factory.offers.lock().unwrap().is_empty());
        assert_eq!(api.counters().relays_started, 0);

        // A GET on the same path is a 405 with Allow, not a 404 — the route exists.
        let wrong_method = api.route(
            &request(&format!("GET /api/sessions/{id}/offer HTTP/1.1\r\n\r\n")),
            Principal::Anonymous,
        );
        assert_eq!(wrong_method.status, 405);

        // And the real thing: the answer comes back as SDP, in the media type the page hands to
        // `setRemoteDescription`.
        let accepted = api.route(
            &offer_request(&id, "v=0\r\nbrowser offer\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(accepted.status, 200, "{}", body_of(&accepted));
        assert_eq!(accepted.content_type, "application/sdp");
        assert!(body_of(&accepted).starts_with("v=0"));
        assert_eq!(
            factory.offers.lock().unwrap().as_slice(),
            ["v=0\r\nbrowser offer\r\n"],
            "the browser's own SDP must reach the transport unedited"
        );
        assert_eq!(api.counters().relays_started, 1);
    }

    #[test]
    fn a_second_offer_is_409() {
        // A duplicate offer is a client that lost track of its own connection. Replacing the live
        // transport would let one stray retry blank somebody's screen, so the first one wins.
        let factory = FakeFactory::default();
        let mut api = api(false).with_media(Some(media(factory.clone())));
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        assert_eq!(
            api.route(
                &offer_request(&id, "v=0\r\nfirst\r\n"),
                Principal::Anonymous
            )
            .status,
            200
        );
        let second = api.route(
            &offer_request(&id, "v=0\r\nsecond\r\n"),
            Principal::Anonymous,
        );
        assert_eq!(second.status, 409, "{}", body_of(&second));
        assert_eq!(api.counters().offer_conflict, 1);
        // One transport, not two — and so one leased port, not two.
        assert_eq!(factory.offers.lock().unwrap().len(), 1);
        assert_eq!(api.counters().relays_started, 1);
    }

    #[test]
    fn a_strangers_offer_is_404() {
        // The ownership rule, kept now that these routes DO something: a stranger must learn nothing,
        // not even that the id is real — and the check runs before the media type, so a `415` cannot
        // leak it either.
        let factory = FakeFactory::default();
        let mut api = hosted().with_media(Some(media(factory.clone())));
        let alice = account(1);
        let id = created_id(&api.route(&post("block", "edit"), alice));

        for path in ["offer", "ice"] {
            let stranger = api.route(
                &request(&format!("POST /api/sessions/{id}/{path} HTTP/1.1\r\n\r\n")),
                account(2),
            );
            assert_eq!(stranger.status, 404, "a stranger learned {path} exists");
        }
        // Not even with a well-formed offer body.
        let stranger = api.route(&offer_request(&id, "v=0\r\nmine now\r\n"), account(2));
        assert_eq!(stranger.status, 404);
        assert!(
            factory.offers.lock().unwrap().is_empty(),
            "a stranger started a transport"
        );

        // The owner still reaches it, so the refusals were about Bob rather than about the route.
        assert_eq!(
            api.route(&offer_request(&id, "v=0\r\nhers\r\n"), alice)
                .status,
            200
        );
        assert_eq!(api.counters().not_owner, 3);

        // `/answer` is gone (contract.md: one round trip), so it is a 404 for its OWNER too — the
        // honest answer for a route that does not exist, rather than a step that waits forever.
        let answer = api.route(
            &request(&format!("POST /api/sessions/{id}/answer HTTP/1.1\r\n\r\n")),
            alice,
        );
        assert_eq!(answer.status, 404);
    }

    #[test]
    fn the_offer_without_media_configured_is_503() {
        // `503` and not `501`: the route is implemented, this deployment has nowhere to put media, and
        // the operator is the one who can fix it. A `501` would say "come back after the next brick".
        let mut api = api(false);
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        let response = api.route(&offer_request(&id, "v=0\r\n"), Principal::Anonymous);
        assert_eq!(response.status, 503);
        assert!(body_of(&response).contains("media is not configured"));
        assert_eq!(api.counters().media_unconfigured, 1);
        assert_eq!(api.counters().relays_started, 0);
    }

    #[test]
    fn an_exhausted_port_pool_and_an_unnegotiable_offer_are_told_apart() {
        // Not on the brick's list, but the three ways an offer can fail after ownership all answered
        // the same status in the first draft, and "the host is busy", "our network is broken" and
        // "your browser offered no AV1" are three different things to be told.
        let factory = FakeFactory::default();
        let mut api = api(false).with_media(Some(media(factory.clone())));
        // The pool holds two ports; take them both with two sessions (one play, one edit).
        let first = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        let second = created_id(&api.route(&post("block", "edit"), Principal::Anonymous));
        let third = created_id(&api.route(&post("block", "edit"), Principal::Anonymous));
        for id in [&first, &second] {
            assert_eq!(
                api.route(&offer_request(id, "v=0\r\n"), Principal::Anonymous)
                    .status,
                200
            );
        }
        let exhausted = api.route(&offer_request(&third, "v=0\r\n"), Principal::Anonymous);
        assert_eq!(exhausted.status, 503, "{}", body_of(&exhausted));
        assert_eq!(api.counters().media_ports_exhausted, 1);

        // Free one port again, so the next refusal is about negotiation and not about the pool — the
        // whole point of this test is that the two cannot be confused.
        assert_eq!(
            api.route(
                &request(&format!("DELETE /api/sessions/{first} HTTP/1.1\r\n\r\n")),
                Principal::Anonymous
            )
            .status,
            204
        );

        // A refused negotiation is the client's fault, so it is a 400 rather than the 503 above.
        *factory.refuse.lock().unwrap() = Some("no AV1 in the offer".into());
        let refused = api.route(&offer_request(&third, "v=0\r\n"), Principal::Anonymous);
        assert_eq!(refused.status, 400, "{}", body_of(&refused));
        assert_eq!(api.counters().offer_rejected, 1);

        // And the port that refusal had already leased came straight back: the same session now
        // succeeds, which it could not if the failed attempt had kept it.
        *factory.refuse.lock().unwrap() = None;
        assert_eq!(
            api.route(&offer_request(&third, "v=0\r\n"), Principal::Anonymous)
                .status,
            200,
            "a port leased by a failed offer was not released"
        );
    }

    #[test]
    fn ice_is_owner_only_and_empty_by_default() {
        // An empty list is a valid answer, not a missing one: a browser on a network that permits UDP
        // needs no relay, and inventing a `501` here would make the page branch on it.
        let mut api = hosted();
        let alice = account(1);
        let id = created_id(&api.route(&post("block", "edit"), alice));
        let response = api.route(
            &request(&format!("GET /api/sessions/{id}/ice HTTP/1.1\r\n\r\n")),
            alice,
        );
        assert_eq!(response.status, 200);
        assert_eq!(body_of(&response), "{\"ice_servers\":[]}");
        assert_eq!(response.content_type, "application/json");

        let stranger = api.route(
            &request(&format!("GET /api/sessions/{id}/ice HTTP/1.1\r\n\r\n")),
            account(2),
        );
        assert_eq!(stranger.status, 404);
        // A POST is a 405 with Allow, not a 404: the route exists and takes GET.
        let wrong_method = api.route(
            &request(&format!("POST /api/sessions/{id}/ice HTTP/1.1\r\n\r\n")),
            alice,
        );
        assert_eq!(wrong_method.status, 405);
    }

    #[test]
    fn ice_mints_a_turn_credential_when_configured() {
        // coturn's REST scheme: the username carries its own expiry, so the credential expires with
        // nothing stored on either side (`turn.rs`). What matters here is that the route mints one
        // *per session* — a credential shared between sessions would be a standing relay key.
        let path = std::env::temp_dir().join(format!("rime-api-turn-{}", std::process::id()));
        std::fs::write(&path, b"0123456789abcdef0123456789abcdef").unwrap();
        let turn = TurnConfig::from_secret_file(
            "turns:turn.rime.peekstar.eu:443?transport=tcp".into(),
            &path,
        )
        .unwrap();
        let mut api = hosted().with_ice(Box::new(crate::relay::TurnIceProvider(turn)));
        let alice = account(1);
        let id = created_id(&api.route(&post("block", "edit"), alice));
        let body = body_of(&api.route(
            &request(&format!("GET /api/sessions/{id}/ice HTTP/1.1\r\n\r\n")),
            alice,
        ));
        assert!(
            body.contains("\"urls\":[\"turns:turn.rime.peekstar.eu:443?transport=tcp\"]"),
            "{body}"
        );
        // `username` is "<expiry>:<session id>", so the session id has to be in it, and the expiry
        // has to be in the future.
        let marker = format!(":{id}\"");
        assert!(body.contains(&marker), "{body}");
        let expiry: u64 = body
            .split("\"username\":\"")
            .nth(1)
            .and_then(|rest| rest.split(':').next())
            .unwrap()
            .parse()
            .unwrap();
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();
        assert!(expiry > now, "the credential is already expired");
        assert!(expiry <= now + crate::turn::DEFAULT_TTL_SECS);
        assert!(
            !body.contains("0123456789abcdef"),
            "the secret leaked: {body}"
        );
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn delete_stops_the_relay() {
        // A `DELETE` that only released the slot would leave two threads, a UDP socket and a leased
        // port behind — and the engine process killed underneath a thread still reading its socket.
        let factory = FakeFactory::default();
        let mut api = api(false).with_media(Some(media(factory.clone())));
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        assert_eq!(
            api.route(&offer_request(&id, "v=0\r\n"), Principal::Anonymous)
                .status,
            200
        );
        assert!(
            !factory.all_closed(),
            "the transport closed before the DELETE"
        );

        let deleted = api.route(
            &request(&format!("DELETE /api/sessions/{id} HTTP/1.1\r\n\r\n")),
            Principal::Anonymous,
        );
        assert_eq!(deleted.status, 204);
        assert!(factory.all_closed(), "the transport outlived the session");
        assert_eq!(api.counters().relays_stopped, 1);
        assert!(api.registry().is_empty());
        // And the play slot is genuinely free again, which a leaked relay would have kept.
        assert_eq!(
            api.route(&post("hello", "play"), Principal::Anonymous)
                .status,
            201
        );
    }

    #[test]
    fn a_relay_that_stopped_on_its_own_frees_its_slot_on_the_next_request() {
        // The sweep. Not on the brick's list, but it is the other half of `delete_stops_the_relay`:
        // without it a session whose engine exited holds one of three slots until somebody happens to
        // delete it, and on this host that is a third of the capacity lost to a corpse.
        let factory = FakeFactory::default();
        let mut api = api(false).with_media(Some(media(factory.clone())));
        let id = created_id(&api.route(&post("hello", "play"), Principal::Anonymous));
        assert_eq!(
            api.route(&offer_request(&id, "v=0\r\n"), Principal::Anonymous)
                .status,
            200
        );
        // The engine exits: drop the engine end of its socket, which the launcher kept.
        api.launcher.engines.clear();

        // The relay notices asynchronously, so the first request that sees it may be the second one.
        let mut gone = false;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while std::time::Instant::now() < deadline && !gone {
            gone = api
                .route(
                    &request(&format!("GET /api/sessions/{id} HTTP/1.1\r\n\r\n")),
                    Principal::Anonymous,
                )
                .status
                == 404;
            if !gone {
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
        }
        assert!(gone, "a dead relay kept its session alive");
        assert!(api.registry().is_empty());
        assert!(
            factory.all_closed(),
            "the swept relay did not close its transport"
        );
        assert_eq!(api.counters().relays_stopped, 1);
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

    // Ownership is checked before the signalling routes do ANYTHING — before the method, before the
    // media type, before a port is leased. A stranger cannot tell a real id from an invented one by
    // watching which refusal comes back. The full case is `a_strangers_offer_is_404`; this one pins
    // the ordering against an api with no media configured at all, where the owner's own answer is a
    // 503 and a stranger's must still be a 404.
    #[test]
    fn signalling_refuses_a_stranger_before_it_looks_at_anything_else() {
        let mut api = hosted();
        let alice = account(1);
        let id = created_id(&api.route(&post("block", "edit"), alice));

        let owner = api.route(&offer_request(&id, "v=0\r\n"), alice);
        assert_eq!(owner.status, 503, "the owner reaches the media check");
        let stranger = api.route(&offer_request(&id, "v=0\r\n"), account(2));
        assert_eq!(stranger.status, 404, "a stranger does not learn it exists");
        assert_eq!(api.counters().not_owner, 1);
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
