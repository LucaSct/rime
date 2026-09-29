// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The listener that serves the session and account routers over one port.
//!
//! **Why a thread per connection, bounded, with one lock around the routers.** Both routers are
//! `&mut self`, so routing is serialized whatever we do — and at three sessions (ADR-0047 §3) that
//! costs nothing. What must *not* be serialized is **reading**: a request arrives at the peer's pace,
//! and a single-threaded accept loop hands that pace to whoever connects. One socket that opens and
//! sends nothing would stall every other visitor, the login page included, for as long as the peer
//! cares to wait. So each connection reads its request on its own thread with no lock held, under a
//! deadline for the **whole** request, and only then takes the lock for the microseconds it takes to
//! route. (Routing is not always instant — a launch waits up to the launcher's startup timeout, and
//! a mailed code waits on the relay's — but those are our own bounded steps behind admission and the
//! rate limiter, not a pace a stranger chooses.) The thread count is capped, and a connection over the cap is refused at once rather than
//! queued, because a queue is the same stall with extra steps.
//!
//! A per-read timeout would not be enough: a peer that trickles one byte just inside it could hold a
//! thread for the full 88 KiB of request line, headers and body — hours. The deadline is re-derived
//! before every read, so the total is bounded no matter how the bytes are paced.

use std::io::{self, BufReader, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::api::{Catalogue, CatalogueEntry, Launcher, MediaConfig, SessionApi};
use crate::http::{self, ParseError, Request, Response};
use crate::identity::{AccessPolicy, Principal};
use crate::relay::IceProvider;
#[cfg(feature = "auth")]
use crate::{auth_api::AuthApi, identity::principal_of};
use crate::{AdmissionPolicy, Surface};

pub struct ServerConfig {
    pub bind: SocketAddr,
    pub catalogue: Catalogue,
    pub admission: AdmissionPolicy,
    pub access: AccessPolicy,
    /// Trust `X-Forwarded-For` for the client's address, but only on a request whose TCP peer is
    /// loopback — see [`Server::client_address`] for why that combination, and nothing weaker, is safe.
    pub trust_forwarded_from_loopback: bool,
    /// `None` on a host with no media flags, which then answers every offer `503`. One option rather
    /// than four fields, for the reason [`MediaConfig`] documents: the parts are only useful together.
    pub media: Option<MediaConfig>,
    /// Where `GET /api/sessions/<id>/ice` gets its answer. The default hands out an empty list, which
    /// is the honest answer for a deployment with no TURN relay.
    pub ice: Option<Box<dyn IceProvider>>,
}

/// Counters `respond` updates directly, for an operator to watch outside of any one request.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ServerCounters {
    /// `--trust-forwarded-from-loopback` was set, the peer was loopback, and `X-Forwarded-For` was
    /// missing or unparseable, so the request was answered `400` rather than guess. A climb here means
    /// Caddy's `header_up X-Forwarded-For` line is missing or the reverse proxy changed, not that a
    /// visitor is doing anything — a visitor cannot reach this gateway directly to trigger it.
    pub forwarded_rejected: u64,
}

/// How long connections may take, and how many may be open at once.
#[derive(Clone, Copy, Debug)]
pub struct ConnectionLimits {
    /// Time from accept to the last byte of the request. Every real request here is a few hundred
    /// bytes from a browser; ten seconds is generous for that and short for an attacker.
    pub request_deadline: Duration,
    /// Per-write bound on sending the response, so a peer that stops reading cannot hold a thread.
    pub write_timeout: Duration,
    /// Concurrent connections, and therefore threads. Far above what three sessions need, far below
    /// what would exhaust the host.
    pub max_connections: usize,
}

impl Default for ConnectionLimits {
    fn default() -> Self {
        Self {
            request_deadline: Duration::from_secs(10),
            write_timeout: Duration::from_secs(10),
            max_connections: 64,
        }
    }
}

pub struct Server<L: Launcher> {
    bind: SocketAddr,
    limits: ConnectionLimits,
    sessions: SessionApi<L>,
    trust_forwarded_from_loopback: bool,
    counters: ServerCounters,
    #[cfg(feature = "auth")]
    accounts: Option<AuthApi>,
}

impl<L: Launcher> Server<L> {
    pub fn new(config: ServerConfig, launcher: L) -> Self {
        let ServerConfig {
            bind,
            catalogue,
            admission,
            access,
            trust_forwarded_from_loopback,
            media,
            ice,
        } = config;
        let mut sessions = SessionApi::new(catalogue, admission, launcher)
            .with_access(access)
            .with_media(media);
        if let Some(ice) = ice {
            sessions = sessions.with_ice(ice);
        }
        Self {
            bind,
            limits: ConnectionLimits::default(),
            sessions,
            trust_forwarded_from_loopback,
            counters: ServerCounters::default(),
            #[cfg(feature = "auth")]
            accounts: None,
        }
    }

    /// Counters this server has updated directly (not those of the routers it holds).
    #[must_use]
    pub fn counters(&self) -> ServerCounters {
        self.counters
    }

    #[cfg(feature = "auth")]
    #[must_use]
    pub fn with_accounts(mut self, auth: AuthApi) -> Self {
        self.accounts = Some(auth);
        self
    }

    #[must_use]
    pub fn with_limits(mut self, limits: ConnectionLimits) -> Self {
        self.limits = limits;
        self
    }

    /// Read and answer one request on an already accepted connection. `peer` is the TCP peer's own
    /// address — see [`Server::client_address`] for how (and when) that differs from the visitor's.
    pub fn serve_one<S: Read + Write>(
        &mut self,
        stream: &mut S,
        peer: &str,
        now: u64,
    ) -> io::Result<()> {
        let request = http::read_request(&mut BufReader::new(&mut *stream));
        match self.respond(request, peer, now) {
            Some(response) => response.write_to(stream),
            None => Ok(()),
        }
    }

    /// Route a parsed request (or answer its parse error). `None` means write nothing: a peer that
    /// connected and sent no request has nothing to answer, exactly as `serve_connection` treats it.
    fn respond(
        &mut self,
        request: Result<Request, ParseError>,
        peer: &str,
        now: u64,
    ) -> Option<Response> {
        let request = match request {
            Ok(request) => request,
            Err(ParseError::NoRequest) => return None,
            Err(error) => return Some(self.sessions.reject(&error)),
        };

        let client = match self.client_address(peer, &request) {
            Ok(client) => client,
            Err(rejection) => return Some(rejection),
        };

        #[cfg(feature = "auth")]
        let who = self
            .accounts
            .as_mut()
            .map_or(Principal::Anonymous, |accounts| {
                principal_of(accounts.auth_mut(), &request, now)
            });
        #[cfg(not(feature = "auth"))]
        let who = Principal::Anonymous;

        if !account_path(&request.path) {
            return Some(self.sessions.route(&request, who));
        }
        #[cfg(feature = "auth")]
        if let Some(accounts) = self.accounts.as_mut() {
            return Some(accounts.route(&request, &client, now));
        }
        #[cfg(not(feature = "auth"))]
        let _ = (client, now);
        // The route exists; this deployment has no account service. 404 would claim otherwise.
        Some(Response::error(503, "accounts are not configured"))
    }

    /// Derive the request's client address: the TCP `peer`, unless the operator has enabled
    /// `--trust-forwarded-from-loopback` *and* `peer` is loopback — the combination that means "this
    /// connection is Caddy" (`deploy/starbase/Caddyfile`'s `header_up X-Forwarded-For {remote_host}`,
    /// reached only via the PROXY v2 wrapper it trusts from blackStar alone). Only then is
    /// `X-Forwarded-For` Caddy's own write rather than something a visitor could forge: a visitor
    /// cannot reach this gateway directly (it binds loopback), so a non-loopback peer sending the
    /// header is presumed hostile and simply ignored — trusting it would let anyone pick their own
    /// rate-limit bucket (`limits.rs`) and defeat the whole point of keying on the client at all.
    ///
    /// The **last** comma-separated entry is used, never the first: every entry before Caddy's own is
    /// something the visitor supplied and cannot be trusted. The result is re-rendered through
    /// `IpAddr`'s own `Display`, with an IPv4-mapped `::ffff:a.b.c.d` folded to plain `a.b.c.d` first,
    /// so the two spellings of the same address cannot mint two rate-limit buckets.
    ///
    /// Missing or unparseable when trust applies is answered `400` rather than falling back to `peer`:
    /// every visitor is `peer` (Caddy) in that case, which is the exact collapse this exists to end.
    fn client_address(&mut self, peer: &str, request: &Request) -> Result<String, Response> {
        let peer_is_loopback = peer.parse::<IpAddr>().is_ok_and(|ip| ip.is_loopback());
        if !self.trust_forwarded_from_loopback || !peer_is_loopback {
            return Ok(peer.to_string());
        }
        let forwarded = request
            .header("x-forwarded-for")
            .and_then(|value| value.rsplit(',').next())
            .map(str::trim)
            .filter(|entry| !entry.is_empty())
            .and_then(|entry| entry.parse::<IpAddr>().ok());
        match forwarded {
            Some(ip) => Ok(canonical(ip).to_string()),
            None => {
                self.counters.forwarded_rejected += 1;
                Err(Response::error(400, "missing or invalid X-Forwarded-For"))
            }
        }
    }
}

/// Fold an IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) to the plain IPv4 spelling, so both forms of
/// the same address key the same rate-limit bucket.
fn canonical(ip: IpAddr) -> IpAddr {
    match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map_or(IpAddr::V6(v6), IpAddr::V4),
        v4 => v4,
    }
}

impl<L> Server<L>
where
    L: Launcher + Send + 'static,
    L::Session: Send,
{
    /// Refuse a wildcard address, bind, and serve until a router panics.
    pub fn listen(self) -> io::Result<()> {
        http::check_bind(self.bind)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        let listener = TcpListener::bind(self.bind)?;
        self.run(listener)
    }

    /// Serve an already bound listener. Separate from [`Server::listen`] so a test can bind port 0
    /// and learn the port.
    pub fn run(self, listener: TcpListener) -> io::Result<()> {
        let limits = self.limits;
        let shared = Arc::new(Mutex::new(self));
        let open = Arc::new(AtomicUsize::new(0));
        for accepted in listener.incoming() {
            // A panic inside a router leaves its state half-updated. Serving on from that would be
            // guessing, so the listener stops and the container's supervisor starts a clean one.
            if shared.is_poisoned() {
                return Err(io::Error::other("a router panicked; stopping"));
            }
            let stream = match accepted {
                Ok(stream) => stream,
                Err(error) => {
                    // Out of descriptors, or a peer that reset inside the handshake: the listener
                    // itself is fine. The pause keeps a persistent EMFILE from becoming a hot loop.
                    eprintln!("rime-gateway: accept failed: {error}");
                    std::thread::sleep(Duration::from_millis(50));
                    continue;
                }
            };
            // This is the RAW TCP peer, always — the port is dropped (it changes on every connection,
            // so keying on it would give every request a fresh bucket and make the limiter a no-op),
            // but nothing here yet asks who the *visitor* is. Behind Caddy in this container that peer
            // is Caddy itself, on loopback; `Server::respond` (via `client_address`) is where the real
            // visitor address is recovered from `X-Forwarded-For`, and only when this peer is loopback
            // and the operator opted in — see that method for the reasoning.
            let peer = match stream.peer_addr() {
                Ok(peer) => peer.ip().to_string(),
                // The peer reset between accept and here. Nothing to answer, nobody to answer.
                Err(_) => continue,
            };
            let guard = match Slot::take(&open, limits.max_connections) {
                Some(guard) => guard,
                None => {
                    let _ = stream.set_write_timeout(Some(limits.write_timeout));
                    let _ = Response::error(503, "too many connections").write_to(&mut &stream);
                    continue;
                }
            };
            let shared = Arc::clone(&shared);
            std::thread::spawn(move || {
                let _guard = guard;
                if let Err(error) = serve_connection(&shared, stream, &peer, limits) {
                    eprintln!("rime-gateway: connection from {peer} failed: {error}");
                }
            });
        }
        Ok(())
    }
}

/// One connection, on its own thread: read with no lock held, route under the lock, write after it.
fn serve_connection<L: Launcher>(
    shared: &Mutex<Server<L>>,
    stream: TcpStream,
    peer: &str,
    limits: ConnectionLimits,
) -> io::Result<()> {
    let request = http::read_request(&mut BufReader::new(Deadline {
        stream: &stream,
        until: Instant::now() + limits.request_deadline,
    }));
    // The clock is read after the request is in, so a slow peer cannot present a stale `now`.
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_secs();
    let response = match shared.lock() {
        Ok(mut server) => server.respond(request, peer, now),
        Err(_) => Some(Response::error(500, "gateway is stopping")),
    };
    match response {
        Some(response) => {
            stream.set_write_timeout(Some(limits.write_timeout))?;
            response.write_to(&mut &stream)
        }
        None => Ok(()),
    }
}

/// Does this path belong to the account surface? One definition: with the `auth` feature it *is*
/// `AuthApi::handles`; without it, the same prefix, so a build without accounts still answers 503 on
/// an account route instead of passing it to the session router.
fn account_path(path: &str) -> bool {
    #[cfg(feature = "auth")]
    {
        AuthApi::handles(path)
    }
    #[cfg(not(feature = "auth"))]
    {
        path == "/api/auth" || path.starts_with("/api/auth/")
    }
}

/// A reader with a deadline for the whole request rather than for each read: the socket's timeout is
/// set to whatever time remains before every read, so trickling bytes does not reset the clock.
struct Deadline<'a> {
    stream: &'a TcpStream,
    until: Instant,
}

impl Read for Deadline<'_> {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        let left = self.until.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Err(io::ErrorKind::TimedOut.into());
        }
        self.stream.set_read_timeout(Some(left))?;
        // Unix reports an expired socket timeout as WouldBlock; name it for what it is, so the
        // parser's error maps to 408 rather than to a generic 400.
        (&*self.stream).read(buffer).map_err(|error| {
            if error.kind() == io::ErrorKind::WouldBlock {
                io::ErrorKind::TimedOut.into()
            } else {
                error
            }
        })
    }
}

/// One of the `max_connections` slots, released when the connection's thread ends — however it ends.
struct Slot(Arc<AtomicUsize>);

impl Slot {
    fn take(open: &Arc<AtomicUsize>, max: usize) -> Option<Self> {
        open.fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| {
            (n < max).then_some(n + 1)
        })
        .ok()
        .map(|_| Slot(Arc::clone(open)))
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

impl Catalogue {
    /// Parse the operator's line-based catalogue without silently accepting unknown settings.
    pub fn from_config(text: &str) -> Result<Self, String> {
        #[derive(Default)]
        struct Entry {
            id: String,
            title: Option<String>,
            program: Option<PathBuf>,
            args: Option<Vec<String>>,
            surfaces: Option<Vec<Surface>>,
        }

        fn finish(entry: Entry, catalogue: &mut Catalogue) -> Result<(), String> {
            let title = entry
                .title
                .ok_or_else(|| format!("[{}] missing title", entry.id))?;
            let program = entry
                .program
                .ok_or_else(|| format!("[{}] missing program", entry.id))?;
            let surfaces = entry
                .surfaces
                .ok_or_else(|| format!("[{}] missing surfaces", entry.id))?;
            catalogue.entries.push(CatalogueEntry {
                id: entry.id,
                title,
                program,
                args: entry.args.unwrap_or_default(),
                surfaces,
            });
            Ok(())
        }

        let mut catalogue = Catalogue::default();
        let mut current: Option<Entry> = None;
        for (index, raw) in text.lines().enumerate() {
            let line = raw.split('#').next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            if let Some(id) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
                if id.trim() != id || id.is_empty() {
                    return Err(format!("line {}: invalid section id", index + 1));
                }
                if let Some(entry) = current.take() {
                    finish(entry, &mut catalogue)?;
                }
                // A duplicate section could replace what the operator thought was running.
                if catalogue.find(id).is_some() {
                    return Err(format!("line {}: duplicate section [{id}]", index + 1));
                }
                current = Some(Entry {
                    id: id.into(),
                    ..Entry::default()
                });
                continue;
            }
            let entry = current
                .as_mut()
                .ok_or_else(|| format!("line {}: key before section", index + 1))?;
            let (key, value) = line
                .split_once('=')
                .ok_or_else(|| format!("line {}: expected key = value", index + 1))?;
            let (key, value) = (key.trim(), value.trim());
            match key {
                "title" if entry.title.is_none() && !value.is_empty() => {
                    entry.title = Some(value.into())
                }
                "program" if entry.program.is_none() && !value.is_empty() => {
                    entry.program = Some(value.into())
                }
                "args" if entry.args.is_none() => {
                    entry.args = Some(value.split_whitespace().map(str::to_string).collect())
                }
                "surfaces" if entry.surfaces.is_none() => {
                    let surfaces: Option<Vec<_>> =
                        value.split_whitespace().map(Surface::from_wire).collect();
                    let surfaces =
                        surfaces.ok_or_else(|| format!("line {}: unknown surface", index + 1))?;
                    if surfaces.is_empty() {
                        return Err(format!("line {}: surfaces must not be empty", index + 1));
                    }
                    entry.surfaces = Some(surfaces);
                }
                "title" | "program" | "args" | "surfaces" => {
                    return Err(format!("line {}: duplicate or empty {key}", index + 1))
                }
                // Ignoring an unknown key would make the config lie about what it is running.
                _ => return Err(format!("line {}: unknown key {key}", index + 1)),
            }
        }
        if let Some(entry) = current {
            finish(entry, &mut catalogue)?;
        }
        Ok(catalogue)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    struct FakeLauncher;

    /// A launched session with no socket behind it.
    ///
    /// These tests are about the listener — threads, deadlines, the connection cap, which router a path
    /// reaches — and none of them signals, so `engine_halves` is an honest error rather than a socket
    /// pair nothing would read. A relay that tried to start here would fail loudly, which is the
    /// correct outcome for a fake that cannot carry media.
    struct FakeSession;

    impl crate::relay::MediaSession for FakeSession {
        fn engine_halves(
            &mut self,
        ) -> io::Result<(
            crate::Session<std::os::unix::net::UnixStream>,
            crate::Session<std::os::unix::net::UnixStream>,
        )> {
            Err(io::Error::other("the serve tests never start a relay"))
        }
    }

    impl Launcher for FakeLauncher {
        type Session = FakeSession;

        fn launch(
            &mut self,
            _entry: &CatalogueEntry,
            _surface: Surface,
        ) -> Result<FakeSession, String> {
            Ok(FakeSession)
        }
    }

    struct Pipe {
        inbound: Cursor<Vec<u8>>,
        outbound: Vec<u8>,
    }

    impl Pipe {
        fn new(input: &str) -> Self {
            Self {
                inbound: Cursor::new(input.as_bytes().to_vec()),
                outbound: Vec::new(),
            }
        }

        fn output(&self) -> String {
            String::from_utf8(self.outbound.clone()).unwrap()
        }
    }

    impl Read for Pipe {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            self.inbound.read(buffer)
        }
    }

    impl Write for Pipe {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            self.outbound.extend_from_slice(bytes);
            Ok(bytes.len())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    fn server(bind: &str) -> Server<FakeLauncher> {
        server_with(bind, false)
    }

    fn server_with(bind: &str, trust_forwarded_from_loopback: bool) -> Server<FakeLauncher> {
        Server::new(
            ServerConfig {
                bind: bind.parse().unwrap(),
                catalogue: Catalogue::default(),
                admission: AdmissionPolicy {
                    max_sessions: 3,
                    max_play_sessions: 1,
                    max_sessions_per_account: 2,
                },
                access: AccessPolicy {
                    require_account: false,
                },
                trust_forwarded_from_loopback,
                media: None,
                ice: None,
            },
            FakeLauncher,
        )
    }

    #[cfg(feature = "auth")]
    fn accounts_with_limits(limits: crate::auth_api::LimitPolicy) -> AuthApi {
        use rime_auth::ceremony::CeremonyConfig;
        use rime_auth::flow::Auth;
        use rime_auth::mail::CapturingMailer;
        use rime_auth::AuthStore;
        use std::sync::atomic::{AtomicU64, Ordering};

        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "rime-gateway-serve-{}-{}.log",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let auth = Auth::new(
            AuthStore::open(&path).unwrap(),
            &CeremonyConfig {
                rp_id: "rime.example".into(),
                rp_origin: "https://rime.example".into(),
                rp_name: "Rime".into(),
            },
            Box::new(CapturingMailer::default()),
        )
        .unwrap();
        AuthApi::with_limits(auth, limits)
    }

    #[cfg(feature = "auth")]
    fn accounts() -> AuthApi {
        accounts_with_limits(crate::auth_api::LimitPolicy::default())
    }

    #[cfg(feature = "auth")]
    #[test]
    fn an_account_path_reaches_the_account_router() {
        let mut server = server("127.0.0.1:0").with_accounts(accounts());
        let mut pipe = Pipe::new("GET /api/auth/register HTTP/1.1\r\n\r\n");
        server.serve_one(&mut pipe, "10.0.0.1", 100).unwrap();
        assert!(pipe.output().starts_with("HTTP/1.1 405 "));
    }

    #[test]
    fn a_session_path_reaches_the_session_router() {
        let mut server = server("127.0.0.1:0");
        let mut pipe = Pipe::new("GET /api/catalogue HTTP/1.1\r\n\r\n");
        server.serve_one(&mut pipe, "10.0.0.1", 100).unwrap();
        assert!(pipe.output().starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(pipe.output().contains("\"games\""));
    }

    #[test]
    fn an_account_route_without_accounts_configured_is_503() {
        let mut server = server("127.0.0.1:0");
        let mut pipe = Pipe::new("POST /api/auth/register HTTP/1.1\r\n\r\n");
        server.serve_one(&mut pipe, "10.0.0.1", 100).unwrap();
        assert!(pipe.output().starts_with("HTTP/1.1 503 "));
    }

    #[test]
    fn a_request_with_no_bytes_writes_nothing() {
        let mut server = server("127.0.0.1:0");
        let mut pipe = Pipe::new("");
        server.serve_one(&mut pipe, "10.0.0.1", 100).unwrap();
        assert!(pipe.outbound.is_empty());
    }

    /// Parse a raw request the way a connection would hand one to `Server::respond`, without going
    /// through a `Pipe` — these tests are about `client_address` alone.
    fn parsed(raw: &str) -> Request {
        http::read_request(&mut BufReader::new(Cursor::new(raw.as_bytes()))).unwrap()
    }

    #[test]
    fn forwarded_for_is_ignored_without_the_flag() {
        let mut server = server("127.0.0.1:0");
        let request = parsed("GET /api/catalogue HTTP/1.1\r\nX-Forwarded-For: 9.9.9.9\r\n\r\n");
        assert_eq!(
            server.client_address("127.0.0.1", &request).unwrap(),
            "127.0.0.1"
        );
    }

    #[test]
    fn forwarded_for_is_ignored_from_a_non_loopback_peer() {
        let mut server = server_with("127.0.0.1:0", true);
        let request = parsed("GET /api/catalogue HTTP/1.1\r\nX-Forwarded-For: 9.9.9.9\r\n\r\n");
        assert_eq!(
            server.client_address("203.0.113.5", &request).unwrap(),
            "203.0.113.5"
        );
    }

    #[test]
    fn the_last_forwarded_entry_is_the_client() {
        let mut server = server_with("127.0.0.1:0", true);
        let request = parsed(
            "GET /api/catalogue HTTP/1.1\r\nX-Forwarded-For: 203.0.113.5, 10.77.0.1, 9.9.9.9\r\n\r\n",
        );
        assert_eq!(
            server.client_address("127.0.0.1", &request).unwrap(),
            "9.9.9.9"
        );
    }

    #[test]
    fn a_loopback_request_without_forwarded_for_is_400() {
        let mut server = server_with("127.0.0.1:0", true);
        let request = parsed("GET /api/catalogue HTTP/1.1\r\n\r\n");
        let response = server.client_address("127.0.0.1", &request).unwrap_err();
        assert_eq!(response.status, 400);
        assert_eq!(server.counters().forwarded_rejected, 1);
    }

    /// The property this brick exists for: two visitors sharing one Caddy peer must not share one
    /// rate-limit bucket. A tight per-client budget (one request per window) makes "exhausted" mean
    /// something observable in a handful of requests rather than twenty.
    #[cfg(feature = "auth")]
    #[test]
    fn two_visitors_behind_caddy_get_separate_limits() {
        use crate::auth_api::LimitPolicy;
        use crate::limits::Rate;

        let accounts = accounts_with_limits(LimitPolicy {
            per_client: Rate::new(1, 60),
            global: Rate::new(100, 60),
        });
        let mut server = server_with("127.0.0.1:0", true).with_accounts(accounts);
        let request = |xff: &str| {
            format!(
                "POST /api/auth/register HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\nX-Forwarded-For: {xff}\r\n\r\n{{}}"
            )
        };

        let mut first = Pipe::new(&request("1.1.1.1"));
        server.serve_one(&mut first, "127.0.0.1", 100).unwrap();
        assert!(
            !first.output().starts_with("HTTP/1.1 429 "),
            "{}",
            first.output()
        );

        // The same visitor again, in the same window: its one token is already spent.
        let mut spent = Pipe::new(&request("1.1.1.1"));
        server.serve_one(&mut spent, "127.0.0.1", 100).unwrap();
        assert!(
            spent.output().starts_with("HTTP/1.1 429 "),
            "{}",
            spent.output()
        );

        // A different visitor behind the same Caddy peer must not be caught by the first one's limit.
        let mut second = Pipe::new(&request("2.2.2.2"));
        server.serve_one(&mut second, "127.0.0.1", 100).unwrap();
        assert!(
            !second.output().starts_with("HTTP/1.1 429 "),
            "{}",
            second.output()
        );
    }

    #[test]
    fn the_catalogue_config_round_trips_two_entries() {
        let catalogue = Catalogue::from_config("# games\n[hello]\ntitle = Hello Game\nprogram = /opt/rime/hello\nargs = --quiet --windowed\nsurfaces = play\n\n[block]\ntitle = The Block\nprogram = /opt/rime/block\nsurfaces = edit play\n").unwrap();
        assert_eq!(catalogue.entries.len(), 2);
        let hello = catalogue.find("hello").unwrap();
        assert_eq!(hello.title, "Hello Game");
        assert_eq!(hello.program, PathBuf::from("/opt/rime/hello"));
        assert_eq!(hello.args, ["--quiet", "--windowed"]);
        assert_eq!(hello.surfaces, [Surface::Play]);
        let block = catalogue.find("block").unwrap();
        assert_eq!(block.title, "The Block");
        assert!(block.args.is_empty());
        assert_eq!(block.surfaces, [Surface::Edit, Surface::Play]);
    }

    #[test]
    fn an_unknown_catalogue_key_is_refused() {
        assert!(Catalogue::from_config(
            "[hello]\ntitle = Hello\nprogram = /bin/hello\nsurfaces = play\nunknown = yes\n"
        )
        .unwrap_err()
        .contains("unknown key"));
    }

    #[test]
    fn a_catalogue_entry_missing_its_program_is_refused() {
        assert!(
            Catalogue::from_config("[hello]\ntitle = Hello\nsurfaces = play\n")
                .unwrap_err()
                .contains("missing program")
        );
    }

    #[test]
    fn a_wildcard_bind_is_refused_before_binding() {
        let error = server("0.0.0.0:0").listen().unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        assert!(error.to_string().contains("refusing to listen"));
    }

    /// Serve on an ephemeral loopback port from a background thread; the thread outlives the test,
    /// which is harmless in a test binary.
    fn spawn(limits: ConnectionLimits) -> SocketAddr {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = server("127.0.0.1:0").with_limits(limits);
        std::thread::spawn(move || server.run(listener));
        addr
    }

    fn read_all(stream: &mut TcpStream) -> String {
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut out = String::new();
        let _ = stream.read_to_string(&mut out);
        out
    }

    #[test]
    fn an_idle_connection_does_not_stall_the_next_one() {
        let addr = spawn(ConnectionLimits::default());
        // Connected, silent, and kept open for the whole test — the single-threaded loop's killer.
        let _idle = TcpStream::connect(addr).unwrap();
        let mut next = TcpStream::connect(addr).unwrap();
        next.write_all(b"GET /api/catalogue HTTP/1.1\r\n\r\n")
            .unwrap();
        assert!(read_all(&mut next).starts_with("HTTP/1.1 200 OK\r\n"));
    }

    #[test]
    fn a_trickled_request_is_cut_off_at_its_deadline_not_per_read() {
        let addr = spawn(ConnectionLimits {
            request_deadline: Duration::from_millis(300),
            ..ConnectionLimits::default()
        });
        let mut stream = TcpStream::connect(addr).unwrap();
        let mut writer = stream.try_clone().unwrap();
        let started = Instant::now();
        // One byte every 100 ms for three seconds: each read succeeds well inside 300 ms, so only a
        // deadline over the whole request can end this before the peer does.
        std::thread::spawn(move || {
            for byte in b"GET /api/catalogue-and-then-some-more-path-bytes"
                .iter()
                .cycle()
                .take(30)
            {
                if writer.write_all(&[*byte]).is_err() {
                    return;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        });
        let response = read_all(&mut stream);
        assert!(response.starts_with("HTTP/1.1 408 "), "{response}");
        assert!(started.elapsed() < Duration::from_millis(1500));
    }

    #[test]
    fn a_connection_over_the_cap_is_refused_at_once() {
        let addr = spawn(ConnectionLimits {
            max_connections: 1,
            ..ConnectionLimits::default()
        });
        // Accepts are sequential and the slot is taken before the next accept, so this one holds it.
        let _idle = TcpStream::connect(addr).unwrap();
        let mut over = TcpStream::connect(addr).unwrap();
        assert!(read_all(&mut over).starts_with("HTTP/1.1 503 "));
    }
}
