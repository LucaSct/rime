// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The relay — the engine's socket on one side, a browser's WebRTC connection on the other.
//!
//! Everything before this brick could be proved in isolation: the surface filter over a pipe
//! (`crate::Session`), the process supervisor over a stub host, the routes over a fake launcher, the
//! transport against a conformant `str0m` peer. This module is where those pieces finally touch, and
//! it is the first one whose job is *translation* rather than policy: an engine protocol envelope on
//! one side, a WebRTC video track plus a DataChannel on the other.
//!
//! ## The five decisions worth reading before the code
//!
//! **1. AV1 goes on the video track; nothing else does.** `contract.md` and ADR-0052: the browser
//! decodes AV1 in hardware off a real `MediaStreamTrack`, which is what makes the internet path
//! viable at all. A `Frame` carrying any other codec is the LAN-only lossless path, and it is
//! *refused* unless the operator turned it on — because LZ4 at 1080p is ~100 Mbit/s of DataChannel
//! traffic that would look, from the outside, exactly like a slow connection.
//!
//! **2. Video is droppable, control is not.** They get opposite backpressure policies, and mixing
//! them up is the bug this file is written to avoid. A refused frame is a moment of stale picture; a
//! refused `StreamConfig` is a decoder that never starts, and a refused `Bye` is a session that never
//! ends. So video drops immediately and control retries for a second before the relay gives up and
//! stops — a stall that is visible ([`StopReason::ControlStalled`]) rather than a session that
//! silently half-works.
//!
//! **3. A dropped delta poisons every later delta.** AV1 is an inter-frame codec: frame *n* is
//! decoded against frame *n-1*. So dropping one delta and forwarding the next does not cost one
//! frame of smoothness — it produces a corrupt, drifting picture until the next keyframe, which is
//! strictly worse than a brief freeze. Hence **resync**: the first refusal asks the engine for a
//! keyframe and *everything* non-key is dropped until one arrives.
//!
//! **4. The client's codec list is rewritten, not trusted.** A browser that asks for LZ4 on a public
//! deployment would get it, because the engine has no idea which transport its frames are about to
//! cross. The gateway is the only place that knows, so the gateway edits the list on its way past —
//! the same shape of decision as the surface filter, and made in the same place, for the same reason.
//!
//! **5. Teardown is `shutdown(2)`, never a read timeout.** The engine-reader thread blocks in
//! `Session::recv`, which is two `read_exact`s; a read timeout that expired *between* them would
//! leave the stream desynchronised with a half-read message and no way to resynchronise. So the
//! reader has no timeout at all and is unblocked by shutting the socket down under it, which turns
//! the blocking read into a clean EOF. Every stop path goes through [`Stop::request`], which does
//! exactly that — so a stop decided by either thread unblocks the other one.

use std::io::Write;
use std::net::SocketAddr;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rime_protocol::{Codec, FrameMessage, MessageType};

use crate::media::PortLease;
use crate::transport::{Command, Event, Str0mTransport, TransportCounters, TransportError};
use crate::turn::{IceServer, TurnConfig};
use crate::{Session, SessionCounters};

/// How long the browser-side thread blocks waiting for a transport event before it looks at the stop
/// flag again. Also the worst-case latency of a keyframe request travelling from the reader thread to
/// the engine, which is a recovery path and not a hot one.
const EVENT_POLL: Duration = Duration::from_millis(50);

/// How long a refused control message is retried before the relay gives up.
///
/// A second is a long time for a DataChannel that is ordered and reliable: if SCTP has not drained
/// 64 queued messages in that time the connection is not coming back, and pretending otherwise turns
/// a dead session into a live-looking one.
const CONTROL_RETRY_BUDGET: Duration = Duration::from_secs(1);

/// Between attempts at a refused control message.
const CONTROL_RETRY_PAUSE: Duration = Duration::from_millis(5);

/// Per-write bound on the engine socket.
///
/// The engine reads its socket every frame, so a write that blocks for five seconds means the engine
/// has stopped reading — it is wedged or gone. Without the timeout the browser-side thread would
/// block there forever and the relay could never be torn down.
const ENGINE_WRITE_TIMEOUT: Duration = Duration::from_secs(5);

/// The transport side, as the relay sees it.
///
/// [`Str0mTransport`] implements it; the tests use a fake, which is what keeps every property in this
/// file provable without ICE, DTLS, SCTP or a socket. The seam is deliberately the *same three
/// methods* the transport already had rather than a friendlier abstraction: a trait that reshapes its
/// only real implementation is a trait that hides where the behaviour actually lives.
///
/// `Sync` as well as `Send`, because the relay shares one transport between its two threads through
/// an `Arc`. The alternative — a `Mutex` around the whole transport — would put the browser-side
/// thread's 50 ms event poll in front of every video frame the reader thread submits.
pub trait MediaTransport: Send + Sync + 'static {
    fn submit(&self, command: Command) -> Result<(), TransportError>;
    fn poll_event(&self, timeout: Duration) -> Option<Event>;
    fn close(&mut self) -> TransportCounters;
}

impl MediaTransport for Str0mTransport {
    fn submit(&self, command: Command) -> Result<(), TransportError> {
        Str0mTransport::submit(self, command)
    }

    fn poll_event(&self, timeout: Duration) -> Option<Event> {
        Str0mTransport::poll_event(self, timeout)
    }

    fn close(&mut self) -> TransportCounters {
        Str0mTransport::close(self)
    }
}

/// How the API builds a transport for an accepted offer.
///
/// Injected so the API's own tests can answer an offer without binding a UDP socket — the routes are
/// about ownership, status codes and slot bookkeeping, and none of that should need ICE to be
/// testable. The production implementation is [`Str0mFactory`] and it is four lines.
pub trait TransportFactory: Send {
    /// Accept `offer_sdp` on `bind`, advertising `public` as the address the edge forwards to it, and
    /// return the live transport together with the SDP answer.
    fn accept(
        &self,
        offer_sdp: &str,
        bind: SocketAddr,
        public: SocketAddr,
    ) -> Result<(Box<dyn MediaTransport>, String), TransportError>;
}

/// The production [`TransportFactory`]: a real `str0m` connection on the leased port.
pub struct Str0mFactory;

impl TransportFactory for Str0mFactory {
    fn accept(
        &self,
        offer_sdp: &str,
        bind: SocketAddr,
        public: SocketAddr,
    ) -> Result<(Box<dyn MediaTransport>, String), TransportError> {
        let (transport, answer) = Str0mTransport::accept_offer_at(offer_sdp, bind, Some(public))?;
        Ok((Box::new(transport), answer))
    }
}

/// Where `GET /api/sessions/<id>/ice` gets its answer.
///
/// A trait rather than an `Option<TurnConfig>` read inline, so a deployment without TURN and a
/// deployment with it differ in one injected object instead of in a branch inside a route.
pub trait IceProvider: Send {
    /// The ICE servers for one session, minted now. Empty when there is no relay to offer.
    fn ice_servers(&self, session_id: &str) -> Vec<IceServer>;
}

/// The default: no TURN, so `{"ice_servers":[]}`. A browser on a network that permits UDP needs
/// nothing else, and one that does not will fail honestly rather than against a broken relay.
pub struct NoIceServers;

impl IceProvider for NoIceServers {
    fn ice_servers(&self, _session_id: &str) -> Vec<IceServer> {
        Vec::new()
    }
}

/// One coturn credential per request, expiring an hour later (`turn.rs`).
///
/// The clock is read here rather than passed in because the expiry is only ever "now plus the TTL";
/// a caller-supplied `now` would be a second source of truth for the current time, and the only
/// thing it would buy is a test that does not need it — `mint` already has its own vector test.
pub struct TurnIceProvider(pub TurnConfig);

impl IceProvider for TurnIceProvider {
    fn ice_servers(&self, session_id: &str) -> Vec<IceServer> {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |since| since.as_secs());
        vec![self.0.mint(session_id, now)]
    }
}

/// What the relay needs from a launched session.
///
/// Narrow on purpose: the relay must not know what a session *is* — a child process, in production —
/// only how to read and write it. That is what lets the API's tests hand it a `UnixStream::pair()`.
pub trait MediaSession {
    /// Two views of the engine socket, both carrying this session's surface. See
    /// [`Session::try_clone`] for why a clone rather than a lock, and why no second handshake.
    fn engine_halves(&mut self) -> std::io::Result<(Session<UnixStream>, Session<UnixStream>)>;
}

impl MediaSession for crate::SessionHandle {
    fn engine_halves(&mut self) -> std::io::Result<(Session<UnixStream>, Session<UnixStream>)> {
        let reader = self.session().try_clone()?;
        let writer = self.session().try_clone()?;
        Ok((reader, writer))
    }
}

/// The operator's media policy for one relay.
#[derive(Debug, Clone, Copy, Default)]
pub struct RelayConfig {
    /// Carry non-AV1 frames over the DataChannel (the LAN-only lossless path, ADR-0052), and let a
    /// browser keep `LZ4` in its decoder list. **Off by default**: on a public deployment this is
    /// tens of megabits per second of reliable, ordered, head-of-line-blocked traffic.
    pub lan_lossless: bool,
}

/// Why a relay stopped. Every variant is a different story for an operator, which is the whole reason
/// this is an enum rather than a bool.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopReason {
    /// The gateway asked: `DELETE /api/sessions/<id>`, a sweep, or the relay being dropped.
    Requested,
    /// The engine closed its socket — the session process exited or finished.
    EngineClosed,
    /// The engine socket failed mid-message.
    EngineFailed,
    /// A write to the engine failed or timed out. The engine has stopped reading.
    EngineWriteFailed,
    /// The engine sent `Bye`.
    EngineBye,
    /// The browser sent `Bye`.
    BrowserBye,
    /// The transport reported [`Event::Disconnected`] — the peer went away.
    TransportGone,
    /// The transport reported [`Event::Failed`]. The text goes to the operator's log, not here.
    TransportFailed,
    /// A control message could not be handed to the DataChannel within [`CONTROL_RETRY_BUDGET`].
    ControlStalled,
    /// After rewriting, the browser's decoder list was empty: it can decode nothing this deployment
    /// is willing to send.
    NoCommonCodec,
}

/// What the relay actually did, in both directions.
///
/// Every drop, refusal and rewrite has a counter, and that is not decoration: a relay that silently
/// discards a message class reads *exactly* like a relay that is working, and the difference only
/// shows up as "the picture is frozen" or "my keyboard does nothing" hours later. Same rule as
/// `CLAUDE.md` guardrail 5, applied to a translation layer.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct RelayCounters {
    /// AV1 frames handed to the video track.
    pub frames_to_video: u64,
    /// Frames refused because their codec is not AV1 and the lossless path is off.
    pub frames_refused_codec: u64,
    /// Non-AV1 frames carried over the DataChannel (the LAN lossless path).
    pub frames_to_data: u64,
    /// Frames the engine sent that could not be parsed at all.
    pub frames_malformed: u64,
    /// Frames dropped because the video queue was full.
    pub frames_dropped_busy: u64,
    /// Deltas dropped while resyncing, because a decoder could not use them anyway.
    pub frames_dropped_resync: u64,
    /// Keyframe requests sent to the engine — from the browser's PLI *and* from our own resync.
    pub keyframe_requests_to_engine: u64,
    /// Times the relay entered resync and asked for a fresh chain.
    pub resyncs: u64,
    /// Control envelopes (StreamConfig, the editor band, Bye) handed to the DataChannel.
    pub control_to_browser: u64,
    /// Retries a refused control message needed. Nonzero means the channel is close to stalling.
    pub control_retries: u64,
    /// Messages from the engine the contract has no place for, dropped rather than guessed at.
    pub unexpected_from_engine: u64,
    /// DataChannel payloads that were not exactly one envelope.
    pub malformed_from_browser: u64,
    /// Messages from the browser whose type it may not send at all.
    pub refused_from_browser: u64,
    /// Capabilities messages the gateway rewrote on their way to the engine.
    pub capabilities_rewritten: u64,
    /// Codec entries removed from a browser's decoder list.
    pub codecs_removed: u64,
    /// Messages forwarded to the engine (input, keyframe requests, editor band, Bye).
    pub messages_to_engine: u64,
    /// Editor-band messages the surface filter refused — a play session's client trying to author.
    pub editor_refused_to_engine: u64,
}

/// The whole story of one relay, returned by [`Relay::stop`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RelayReport {
    pub reason: StopReason,
    pub relay: RelayCounters,
    /// Both engine halves added together — the reader's filtering and the writer's refusals.
    pub engine: SessionCounters,
    pub transport: TransportCounters,
}

/// The stop flag, the reason, and the one thing that can unblock a thread parked in `read_exact`.
///
/// One object rather than three, because they must be used together: a stop that records a reason but
/// leaves the reader blocked is a relay that never joins, and a shutdown without a reason is a report
/// that says nothing. [`Stop::request`] is the only way to stop a relay, and it does all three.
struct Stop {
    flag: AtomicBool,
    /// First writer wins. A relay that stopped because the engine said `Bye` and was then asked to
    /// stop must still report the `Bye` — the interesting reason is the earliest one.
    reason: Mutex<Option<StopReason>>,
    /// A third view of the engine socket, kept only to shut it down. `shutdown(2)` acts on the
    /// socket rather than on the descriptor, so this unblocks a `read_exact` on either of the
    /// relay's own clones.
    engine: Option<UnixStream>,
}

impl Stop {
    fn request(&self, reason: StopReason) {
        {
            let mut held = self.reason.lock().unwrap_or_else(|e| e.into_inner());
            if held.is_none() {
                *held = Some(reason);
            }
        }
        self.flag.store(true, Ordering::Release);
        if let Some(engine) = &self.engine {
            // Idempotent in practice: a second shutdown on an already-shut socket is an error we do
            // not care about. A failure here is not recoverable either — it means the reader will
            // only unblock when the engine itself closes.
            let _ = engine.shutdown(std::net::Shutdown::Both);
        }
    }

    fn stopped(&self) -> bool {
        self.flag.load(Ordering::Acquire)
    }

    fn reason(&self) -> StopReason {
        self.reason
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .unwrap_or(StopReason::Requested)
    }
}

/// Shared mutable state: the counters, and the one-shot "ask the engine for a keyframe" flag.
///
/// **Why a flag and not a channel.** The reader thread discovers that it needs a keyframe, and only
/// the browser-side thread may write to the engine. A flag makes the request idempotent by
/// construction — three refused frames in the same millisecond cannot become three requests — where a
/// queue would need that logic written and tested. The cost is up to [`EVENT_POLL`] of latency on a
/// recovery path, which is not a hot path.
struct Shared {
    counters: Mutex<RelayCounters>,
    keyframe_wanted: AtomicBool,
}

impl Shared {
    fn count(&self, edit: impl FnOnce(&mut RelayCounters)) {
        edit(&mut self.counters.lock().unwrap_or_else(|e| e.into_inner()));
    }

    fn snapshot(&self) -> RelayCounters {
        *self.counters.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// One live relay: two threads, a transport, a leased port, and a guard that reaps the engine.
pub struct Relay {
    stop: Arc<Stop>,
    shared: Arc<Shared>,
    transport: Option<Arc<dyn MediaTransport>>,
    reader: Option<JoinHandle<SessionCounters>>,
    writer: Option<JoinHandle<SessionCounters>>,
    /// Returned to the pool when this is dropped, which is at the end of [`Relay::stop`].
    lease: Option<PortLease>,
    /// Owns the engine session; dropping it reaps the child process. Dropped **last**, after both
    /// threads have joined, so nothing is still reading a socket whose process has just been killed.
    on_stop: Option<Box<dyn Send>>,
    /// Set by the first `stop`, and returned unchanged by every later one.
    report: Option<RelayReport>,
}

impl Relay {
    /// Start relaying between `reader`/`writer` (two halves of one engine socket, same surface) and
    /// `transport`.
    ///
    /// `lease` and `on_stop` are held for the relay's life and released in that order at the end of
    /// [`Relay::stop`]: the port goes back to the pool, then the guard drops and reaps the child.
    ///
    /// Infallible on purpose. The failure modes left at this point — a socket that cannot be cloned,
    /// a thread that cannot be spawned — are host exhaustion, and there is no useful difference
    /// between "the relay failed to start" and "the relay started and stopped at once": both end with
    /// the same teardown. A relay whose threads did not start reports `EngineFailed` on its first
    /// `stop`, which is the truth.
    pub fn start(
        reader: Session<UnixStream>,
        writer: Session<UnixStream>,
        transport: Box<dyn MediaTransport>,
        lease: PortLease,
        on_stop: Box<dyn Send>,
        config: RelayConfig,
    ) -> Self {
        // A write that cannot complete in five seconds means the engine has stopped reading; without
        // this the browser-side thread would park in `write_all` and the relay could never be torn
        // down. The READER deliberately gets no timeout at all — see the module note.
        let _ = writer
            .get_ref()
            .set_write_timeout(Some(ENGINE_WRITE_TIMEOUT));

        let stop = Arc::new(Stop {
            flag: AtomicBool::new(false),
            reason: Mutex::new(None),
            engine: reader.get_ref().try_clone().ok(),
        });
        let shared = Arc::new(Shared {
            counters: Mutex::new(RelayCounters::default()),
            keyframe_wanted: AtomicBool::new(false),
        });
        let transport: Arc<dyn MediaTransport> = Arc::from(transport);

        let reader_thread = {
            let (transport, stop, shared) = (
                Arc::clone(&transport),
                Arc::clone(&stop),
                Arc::clone(&shared),
            );
            std::thread::Builder::new()
                .name("rime-relay-engine".into())
                .spawn(move || engine_reader(reader, &*transport, &stop, &shared, config))
        };
        let writer_thread = {
            let (transport, stop, shared) = (
                Arc::clone(&transport),
                Arc::clone(&stop),
                Arc::clone(&shared),
            );
            std::thread::Builder::new()
                .name("rime-relay-browser".into())
                .spawn(move || browser_side(writer, &*transport, &stop, &shared, config))
        };
        // A thread that could not be spawned is a relay that cannot work. Stop at once rather than
        // running half of it: a relay with no reader would negotiate, connect, and show nothing.
        if reader_thread.is_err() || writer_thread.is_err() {
            stop.request(StopReason::EngineFailed);
        }

        Self {
            stop,
            shared,
            transport: Some(transport),
            reader: reader_thread.ok(),
            writer: writer_thread.ok(),
            lease: Some(lease),
            on_stop: Some(on_stop),
            report: None,
        }
    }

    /// Has this relay stopped, for any reason, including one it decided itself?
    ///
    /// True as soon as a thread has *requested* the stop, not only once [`Relay::stop`] has run — the
    /// API uses this to sweep dead relays, and a relay whose engine has exited is dead whether or not
    /// anybody has joined its threads yet.
    #[must_use]
    pub fn is_stopped(&self) -> bool {
        self.report.is_some() || self.stop.stopped()
    }

    /// Stop everything and report. Idempotent: later calls return the first call's report.
    ///
    /// The order is the point. Flag and `shutdown` first, so both threads are on their way out.
    /// Join next, so nothing is touching the transport or the engine socket any more. Only then close
    /// the transport (which releases its UDP port and joins its own thread), return the leased port,
    /// and finally drop the guard that kills the engine process. Reversed anywhere, this leaks
    /// something: a killed child with a thread still reading its socket, or a port handed to the next
    /// session while this one's transport still holds it.
    pub fn stop(&mut self, reason: StopReason) -> RelayReport {
        if let Some(report) = self.report {
            return report;
        }
        self.stop.request(reason);

        // Joined BEFORE the transport is closed, unlike the first sketch of this design: `close`
        // needs `&mut`, and the two threads hold `Arc` clones until they end. Closing first would
        // mean a lock around the transport, and that lock would sit on the video path. Joining is
        // bounded anyway — the browser side wakes every EVENT_POLL and the reader is already EOF.
        let mut engine = SessionCounters::default();
        for handle in [self.reader.take(), self.writer.take()]
            .into_iter()
            .flatten()
        {
            // A panicked relay thread must not panic the router that called `stop`. Its counters are
            // lost, which is the honest outcome; the session still tears down completely.
            if let Ok(counters) = handle.join() {
                engine.forwarded += counters.forwarded;
                engine.editor_blocked += counters.editor_blocked;
                engine.outbound_blocked += counters.outbound_blocked;
            }
        }

        let transport = match self.transport.take() {
            // Both threads are joined, so this is the only remaining reference and `get_mut`
            // succeeds. If it somehow did not, dropping the `Arc` still closes the transport
            // (`Str0mTransport::drop` calls `close`) — only the counters would be lost.
            Some(mut arc) => {
                Arc::get_mut(&mut arc).map_or_else(TransportCounters::default, |t| t.close())
            }
            None => TransportCounters::default(),
        };

        drop(self.lease.take());
        drop(self.on_stop.take());

        let report = RelayReport {
            reason: self.stop.reason(),
            relay: self.shared.snapshot(),
            engine,
            transport,
        };
        self.report = Some(report);
        report
    }

    /// The counters so far, without stopping. For a status route and for tests that want to watch a
    /// live relay rather than a finished one.
    #[must_use]
    pub fn counters(&self) -> RelayCounters {
        self.shared.snapshot()
    }
}

impl Drop for Relay {
    /// A relay dropped without being stopped would leave two threads, a UDP port, a leased media
    /// port and a child process behind — the same discipline `SessionHandle` and `Str0mTransport`
    /// already apply to their own resources, for the same reason.
    fn drop(&mut self) {
        let _ = self.stop(StopReason::Requested);
    }
}

impl std::fmt::Debug for Relay {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Relay")
            .field("stopped", &self.is_stopped())
            .field("counters", &self.shared.snapshot())
            .finish()
    }
}

/// One protocol envelope, byte-identical to what `Connection::send` puts on the engine socket:
/// `[type:u16 LE][length:u32 LE][payload]`. The DataChannel carries exactly this (`contract.md`), so
/// the browser and the engine parse the same bytes with the same code.
fn envelope(code: u16, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(6 + payload.len());
    out.extend_from_slice(&code.to_le_bytes());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(payload);
    out
}

/// Split a DataChannel payload that must be **exactly one** envelope.
///
/// Strict for a reason that is not pedantry: the engine socket is a stream and the DataChannel is a
/// message transport, so "two envelopes in one message" and "half an envelope" are not things a
/// conformant client produces — they are a client that reimplemented the framing wrongly, and
/// accepting either would mean carrying a stream reassembler for a transport that does not need one.
/// A length field that disagrees with the message is refused rather than trusted, which is the same
/// check `Connection::recv` makes before it allocates.
fn one_envelope(payload: &[u8]) -> Option<(u16, &[u8])> {
    if payload.len() < 6 {
        return None;
    }
    let code = u16::from_le_bytes([payload[0], payload[1]]);
    let len = u32::from_le_bytes([payload[2], payload[3], payload[4], payload[5]]) as usize;
    if len != payload.len() - 6 {
        return None;
    }
    Some((code, &payload[6..]))
}

/// The engine → browser thread.
///
/// Blocking, with no read timeout, and unblocked only by `shutdown(2)` — see the module note. It ends
/// on EOF, on an engine error, on the engine's `Bye`, or when the transport is gone.
fn engine_reader(
    mut reader: Session<UnixStream>,
    transport: &dyn MediaTransport,
    stop: &Stop,
    shared: &Shared,
    config: RelayConfig,
) -> SessionCounters {
    // Resync state, and deliberately a LOCAL: only this thread decides whether a delta is worth
    // forwarding, so making it shared would invite a second writer to a decision with one owner.
    let mut resyncing = false;

    while !stop.stopped() {
        let (ty, payload) = match reader.recv() {
            Ok(Some(message)) => message,
            Ok(None) => {
                stop.request(StopReason::EngineClosed);
                break;
            }
            Err(error) => {
                // An EOF caused by our own teardown arrives here as a plain error too; the reason is
                // already recorded by then, and `Stop` keeps the first one.
                eprintln!("rime-relay: engine read failed: {error}");
                stop.request(StopReason::EngineFailed);
                break;
            }
        };

        match ty {
            MessageType::Frame => {
                let frame = match FrameMessage::decode(&payload) {
                    Ok(frame) => frame,
                    Err(error) => {
                        // Not fatal: the envelope was well-formed, so the stream is still in sync,
                        // and one unparseable frame is a frame lost rather than a session lost.
                        eprintln!("rime-relay: undecodable frame from the engine: {error}");
                        shared.count(|c| c.frames_malformed += 1);
                        continue;
                    }
                };
                if frame.codec == Codec::Av1 {
                    if !send_video(frame, transport, stop, shared, &mut resyncing) {
                        break;
                    }
                } else if config.lan_lossless {
                    // The LAN path: the frame crosses the DataChannel exactly as it crossed the
                    // socket, and a WASM decoder on the other side reads it with the same layout.
                    if !send_control(
                        envelope(ty.to_code(), &payload),
                        transport,
                        stop,
                        shared,
                        |c| c.frames_to_data += 1,
                    ) {
                        break;
                    }
                } else {
                    // ADR-0052: the lossless path is an operator decision, not a codec negotiation
                    // outcome. Refusing loudly beats sending 100 Mbit/s of DataChannel to a browser
                    // on a domestic uplink and calling the result "the connection".
                    shared.count(|c| c.frames_refused_codec += 1);
                }
            }
            // The control band. `StreamConfig` stands up the decoder, the editor band carries
            // authoring traffic on an edit surface (the reader's own filter has already dropped it on
            // a play one), and `Bye` is the graceful close.
            MessageType::StreamConfig | MessageType::Bye => {
                let is_bye = ty == MessageType::Bye;
                if !send_control(
                    envelope(ty.to_code(), &payload),
                    transport,
                    stop,
                    shared,
                    |c| c.control_to_browser += 1,
                ) {
                    break;
                }
                if is_bye {
                    stop.request(StopReason::EngineBye);
                    break;
                }
            }
            MessageType::Other(code) if MessageType::is_editor(code) => {
                if !send_control(envelope(code, &payload), transport, stop, shared, |c| {
                    c.control_to_browser += 1
                }) {
                    break;
                }
            }
            // `Input`, `Capabilities`, `KeyframeRequest` and unknown codes outside the editor band
            // are client→server messages: the engine sending one is a bug in the engine, and
            // forwarding it would make this relay the place that invented a meaning for it.
            _ => shared.count(|c| c.unexpected_from_engine += 1),
        }
    }

    reader.counters()
}

/// Hand one AV1 frame to the video track, applying the resync rule. Returns false when the relay must
/// stop.
fn send_video(
    frame: FrameMessage,
    transport: &dyn MediaTransport,
    stop: &Stop,
    shared: &Shared,
    resyncing: &mut bool,
) -> bool {
    if *resyncing && !frame.keyframe {
        // The decoder cannot use this: the chain it would extend is already broken. Forwarding it
        // would show corruption, which is worse than the freeze the viewer is already seeing.
        shared.count(|c| c.frames_dropped_resync += 1);
        return true;
    }
    let keyframe = frame.keyframe;
    match transport.submit(Command::SendVideo {
        frame: frame.data,
        keyframe,
        capture_micros: frame.capture_us,
    }) {
        Ok(()) => {
            shared.count(|c| c.frames_to_video += 1);
            if keyframe {
                *resyncing = false;
            }
            true
        }
        Err(TransportError::Busy) => {
            // Never queued: a frame that waits is a frame the viewer sees late, and by the time the
            // queue drains the engine has newer ones. Drop it and get the chain restarted.
            shared.count(|c| c.frames_dropped_busy += 1);
            // Ask once per broken chain — not once per dropped frame, which at 60 fps would be a
            // request storm on the control path that is *already* congested. The exception is a lost
            // KEYFRAME: the chain we asked for never arrived, so without a second ask the stream
            // would stay frozen forever.
            if !*resyncing || keyframe {
                *resyncing = true;
                shared.count(|c| c.resyncs += 1);
                shared.keyframe_wanted.store(true, Ordering::Release);
            }
            true
        }
        Err(TransportError::Closed) => {
            stop.request(StopReason::TransportGone);
            false
        }
        Err(error) => {
            eprintln!("rime-relay: video submit failed: {error}");
            stop.request(StopReason::TransportFailed);
            false
        }
    }
}

/// Hand one control envelope to the DataChannel, retrying a full queue. Returns false when the relay
/// must stop.
///
/// Control messages are **not droppable** — a lost `StreamConfig` is a decoder that never starts and
/// a lost `Bye` is a session that never ends — so this is the mirror image of [`send_video`]: it
/// waits where video drops, and it gives up loudly where video quietly carries on.
fn send_control(
    payload: Vec<u8>,
    transport: &dyn MediaTransport,
    stop: &Stop,
    shared: &Shared,
    counted: impl Fn(&mut RelayCounters),
) -> bool {
    let deadline = Instant::now() + CONTROL_RETRY_BUDGET;
    let mut payload = Some(payload);
    loop {
        let next = payload.take().expect("payload is put back on every retry");
        match transport.submit(Command::SendData {
            payload: next.clone(),
        }) {
            Ok(()) => {
                shared.count(counted);
                return true;
            }
            Err(TransportError::Busy) => {
                if Instant::now() >= deadline || stop.stopped() {
                    // A reliable, ordered channel that has not drained in a second is not going to.
                    // Stopping makes the failure visible; carrying on with a hole in the control
                    // stream would leave a session that looks alive and behaves strangely.
                    stop.request(StopReason::ControlStalled);
                    return false;
                }
                shared.count(|c| c.control_retries += 1);
                payload = Some(next);
                std::thread::sleep(CONTROL_RETRY_PAUSE);
            }
            Err(TransportError::Closed) => {
                stop.request(StopReason::TransportGone);
                return false;
            }
            Err(error) => {
                eprintln!("rime-relay: control submit failed: {error}");
                stop.request(StopReason::TransportFailed);
                return false;
            }
        }
    }
}

/// The browser → engine thread, and the only thread that writes to the engine socket.
fn browser_side(
    mut writer: Session<UnixStream>,
    transport: &dyn MediaTransport,
    stop: &Stop,
    shared: &Shared,
    config: RelayConfig,
) -> SessionCounters {
    while !stop.stopped() {
        // The reader thread's resync request. Checked before the poll so it is served within one
        // loop turn rather than waiting for the next event to arrive.
        if shared.keyframe_wanted.swap(false, Ordering::AcqRel)
            && !to_engine(
                &mut writer,
                MessageType::KeyframeRequest,
                &[],
                stop,
                shared,
                |c| c.keyframe_requests_to_engine += 1,
            )
        {
            break;
        }

        let Some(event) = transport.poll_event(EVENT_POLL) else {
            continue;
        };
        match event {
            // Informational: the relay is already forwarding whatever the engine sends, and the
            // transport queues what it cannot yet write. Nothing to do but notice.
            Event::Connected => {}
            Event::Data { payload } => {
                if !from_browser(&payload, &mut writer, stop, shared, config) {
                    break;
                }
            }
            // PLI/FIR from the browser: its decoder lost the chain and needs a fresh one. The engine
            // owns the encoder, so the demand crosses as the protocol's own message.
            Event::KeyframeRequested => {
                if !to_engine(
                    &mut writer,
                    MessageType::KeyframeRequest,
                    &[],
                    stop,
                    shared,
                    |c| c.keyframe_requests_to_engine += 1,
                ) {
                    break;
                }
            }
            Event::Disconnected => {
                stop.request(StopReason::TransportGone);
                break;
            }
            Event::Failed(why) => {
                eprintln!("rime-relay: transport failed: {why}");
                stop.request(StopReason::TransportFailed);
                break;
            }
        }
    }

    writer.counters()
}

/// Write one message to the engine through the surface filter. Returns false when the relay must
/// stop.
///
/// A refusal (`Ok(false)`) is the filter doing its job — a play session's client asked to author —
/// and is counted, not fatal. An `Err` is the socket: the engine has stopped reading or is gone, and
/// there is nothing left to relay to.
fn to_engine(
    writer: &mut Session<UnixStream>,
    ty: MessageType,
    payload: &[u8],
    stop: &Stop,
    shared: &Shared,
    counted: impl Fn(&mut RelayCounters),
) -> bool {
    match writer.send(ty, payload) {
        Ok(true) => {
            shared.count(counted);
            true
        }
        Ok(false) => {
            shared.count(|c| c.editor_refused_to_engine += 1);
            true
        }
        Err(error) => {
            eprintln!("rime-relay: engine write failed: {error}");
            stop.request(StopReason::EngineWriteFailed);
            false
        }
    }
}

/// One DataChannel payload on its way to the engine. Returns false when the relay must stop.
fn from_browser(
    payload: &[u8],
    writer: &mut Session<UnixStream>,
    stop: &Stop,
    shared: &Shared,
    config: RelayConfig,
) -> bool {
    let Some((code, body)) = one_envelope(payload) else {
        shared.count(|c| c.malformed_from_browser += 1);
        return true;
    };
    match MessageType::from_code(code) {
        MessageType::Capabilities => {
            let Some(rewritten) = rewrite_capabilities(body, config.lan_lossless, shared) else {
                shared.count(|c| c.malformed_from_browser += 1);
                return true;
            };
            if rewritten.is_empty() {
                // Nothing left that this deployment will send and that browser will decode. The
                // session cannot work, and letting the engine negotiate with an empty list would
                // make it choose something we would then refuse frame by frame.
                stop.request(StopReason::NoCommonCodec);
                return false;
            }
            let mut out = Vec::with_capacity(1 + rewritten.len());
            out.push(rewritten.len() as u8);
            out.extend_from_slice(&rewritten);
            shared.count(|c| c.capabilities_rewritten += 1);
            to_engine(writer, MessageType::Capabilities, &out, stop, shared, |c| {
                c.messages_to_engine += 1
            })
        }
        MessageType::Input | MessageType::KeyframeRequest => to_engine(
            writer,
            MessageType::from_code(code),
            body,
            stop,
            shared,
            |c| c.messages_to_engine += 1,
        ),
        MessageType::Bye => {
            // Forwarded first, then stopped: the engine deserves to hear the close from its client
            // rather than to discover it as a dead socket.
            let sent = to_engine(writer, MessageType::Bye, body, stop, shared, |c| {
                c.messages_to_engine += 1
            });
            stop.request(StopReason::BrowserBye);
            let _ = sent;
            false
        }
        // The editor band. `Session::send` refuses it on a play surface and counts the refusal, so
        // the surface decision stays in the one place that owns it.
        MessageType::Other(other) if MessageType::is_editor(other) => {
            to_engine(writer, MessageType::Other(other), body, stop, shared, |c| {
                c.messages_to_engine += 1
            })
        }
        // `Frame`, `StreamConfig` and anything else are server→client messages. A browser sending
        // one is either confused or probing; either way the engine never has to see it.
        _ => {
            shared.count(|c| c.refused_from_browser += 1);
            true
        }
    }
}

/// Rewrite a `Capabilities` payload (`[count:u8][codec:u8 × count]`) to what this deployment is
/// willing to encode, preserving the client's preference order.
///
/// **Why the gateway and not the engine.** The engine picks the first codec it can encode from the
/// list it is given, and it cannot know which transport the frames will cross — this relay is the
/// only place that does. So a browser asking for LZ4 on a public deployment does not get refused
/// later, frame by frame, with a counter nobody is watching: it gets a list that never contained LZ4.
///
/// Unknown codec bytes are skipped rather than refused, exactly as the C++ `CapabilitiesMessage`
/// decoder skips them: a client advertising a codec from the future is what negotiation is for.
/// Returns `None` only for a payload that is not a codec list at all.
fn rewrite_capabilities(body: &[u8], lan_lossless: bool, shared: &Shared) -> Option<Vec<u8>> {
    let (&count, rest) = body.split_first()?;
    let count = usize::from(count);
    if rest.len() < count {
        return None;
    }
    let mut kept = Vec::with_capacity(count);
    let mut removed = 0u64;
    for &code in &rest[..count] {
        let keep = match code {
            // AV1 on the video track: the internet path, always available.
            c if c == codec_code(Codec::Av1) => true,
            // LZ4 over the DataChannel: lossless, local, and only when the operator said so.
            c if c == codec_code(Codec::Lz4) => lan_lossless,
            // Raw and JPEG have no client and no transport here; a future codec has no encoder.
            _ => false,
        };
        if keep {
            kept.push(code);
        } else {
            removed += 1;
        }
    }
    if removed > 0 {
        shared.count(|c| c.codecs_removed += removed);
    }
    Some(kept)
}

/// The wire byte for a codec. `Codec` keeps its `to_code` private, and re-deriving the mapping here
/// from the one place that documents it (`frame.rs`) beats widening that type's API for one caller.
fn codec_code(codec: Codec) -> u8 {
    match codec {
        Codec::Raw => 0,
        Codec::Lz4 => 1,
        Codec::Jpeg => 2,
        Codec::Av1 => 3,
    }
}

/// Write one framed message onto a raw socket, the way the engine would.
///
/// Used by the tests to play the engine, and by nothing else — hence one `pub(crate)` helper rather
/// than a copy in each test module that needs it.
#[cfg(test)]
pub(crate) fn write_framed(
    stream: &mut impl Write,
    code: u16,
    payload: &[u8],
) -> std::io::Result<()> {
    stream.write_all(&envelope(code, payload))?;
    stream.flush()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::media::MediaPorts;
    use crate::Surface;
    use rime_protocol::{Connection, ImageDesc, PixelFormat};
    use std::collections::VecDeque;
    use std::io::Read;
    use std::net::{IpAddr, Ipv4Addr};
    use std::sync::atomic::AtomicUsize;

    /// A transport with no sockets, no ICE and no threads: a queue of events to hand out and a log of
    /// everything submitted.
    ///
    /// This is what makes every property in this file testable in microseconds. The one test that
    /// needs a real connection says so in its name and stands up a real `str0m` peer.
    #[derive(Default)]
    struct FakeTransport {
        submitted: Mutex<Vec<Command>>,
        events: Mutex<VecDeque<Event>>,
        /// When true, every `SendVideo`/`SendData` is refused with `Busy` — the backpressure the
        /// real transport reports when its command queue is full.
        busy: AtomicBool,
        closed: AtomicBool,
        closes: AtomicUsize,
    }

    impl FakeTransport {
        fn shared() -> Arc<Self> {
            Arc::new(Self::default())
        }

        fn push(&self, event: Event) {
            self.events
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .push_back(event);
        }

        fn commands(&self) -> Vec<Command> {
            self.submitted
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .clone()
        }

        fn videos(&self) -> Vec<(Vec<u8>, bool, u64)> {
            self.commands()
                .into_iter()
                .filter_map(|c| match c {
                    Command::SendVideo {
                        frame,
                        keyframe,
                        capture_micros,
                    } => Some((frame, keyframe, capture_micros)),
                    _ => None,
                })
                .collect()
        }

        fn data(&self) -> Vec<Vec<u8>> {
            self.commands()
                .into_iter()
                .filter_map(|c| match c {
                    Command::SendData { payload } => Some(payload),
                    _ => None,
                })
                .collect()
        }
    }

    /// Submitting through an `Arc` is what the relay does, so the impl is on the `Arc`'s target and
    /// every method takes `&self` — which is also why the trait needs `Sync`.
    impl MediaTransport for Arc<FakeTransport> {
        fn submit(&self, command: Command) -> Result<(), TransportError> {
            if self.closed.load(Ordering::Acquire) {
                return Err(TransportError::Closed);
            }
            if self.busy.load(Ordering::Acquire) {
                return Err(TransportError::Busy);
            }
            self.submitted
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .push(command);
            Ok(())
        }

        fn poll_event(&self, timeout: Duration) -> Option<Event> {
            // The real transport blocks for up to `timeout`; this one sleeps a slice of it so a test
            // that pushes an event from its own thread is still served promptly, and a test that
            // pushes nothing does not spin a core.
            let deadline = Instant::now() + timeout;
            loop {
                if let Some(event) = self
                    .events
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .pop_front()
                {
                    return Some(event);
                }
                if Instant::now() >= deadline {
                    return None;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
        }

        fn close(&mut self) -> TransportCounters {
            self.closed.store(true, Ordering::Release);
            self.closes.fetch_add(1, Ordering::AcqRel);
            TransportCounters::default()
        }
    }

    /// A port pool on loopback. The relay only holds the lease; nothing binds these ports.
    fn pool() -> MediaPorts {
        MediaPorts::new(IpAddr::V4(Ipv4Addr::LOCALHOST), 50000..=50001).unwrap()
    }

    /// The whole rig: an engine end of a socket pair the test drives by hand, a relay wired to a fake
    /// transport, and the pool the lease came from.
    struct Rig {
        engine: UnixStream,
        transport: Arc<FakeTransport>,
        relay: Relay,
        ports: MediaPorts,
    }

    impl Rig {
        fn new(surface: Surface, config: RelayConfig) -> Self {
            let transport = FakeTransport::shared();
            Self::with_transport(surface, config, transport)
        }

        fn with_transport(
            surface: Surface,
            config: RelayConfig,
            transport: Arc<FakeTransport>,
        ) -> Self {
            let (engine, gateway) = UnixStream::pair().unwrap();
            // A read timeout on the ENGINE end only: the test plays the engine and must not hang if
            // a message it expects never arrives. The relay's own reader deliberately has none.
            engine
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let reader = Session::new(gateway.try_clone().unwrap(), surface);
            let writer = Session::new(gateway, surface);
            let ports = pool();
            let lease = ports.lease().unwrap();
            let relay = Relay::start(
                reader,
                writer,
                Box::new(Arc::clone(&transport)),
                lease,
                Box::new(()),
                config,
            );
            Self {
                engine,
                transport,
                relay,
                ports,
            }
        }

        /// Send one framed message as the engine would.
        fn engine_sends(&mut self, code: u16, payload: &[u8]) {
            write_framed(&mut self.engine, code, payload).unwrap();
        }

        /// Read one framed message from the engine end, or fail.
        fn engine_receives(&mut self) -> (MessageType, Vec<u8>) {
            let mut header = [0u8; 6];
            self.engine.read_exact(&mut header).expect("engine read");
            let code = u16::from_le_bytes([header[0], header[1]]);
            let len = u32::from_le_bytes([header[2], header[3], header[4], header[5]]) as usize;
            let mut payload = vec![0u8; len];
            self.engine.read_exact(&mut payload).expect("engine read");
            (MessageType::from_code(code), payload)
        }

        /// Wait for `done` to hold, polling the live counters. Returns false on timeout, so a test
        /// asserts rather than hangs.
        fn wait(&self, done: impl Fn(&RelayCounters) -> bool) -> bool {
            let deadline = Instant::now() + Duration::from_secs(5);
            while Instant::now() < deadline {
                if done(&self.relay.counters()) {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            done(&self.relay.counters())
        }

        fn wait_stopped(&self) -> bool {
            let deadline = Instant::now() + Duration::from_secs(5);
            while Instant::now() < deadline {
                if self.relay.is_stopped() {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            self.relay.is_stopped()
        }
    }

    /// One AV1 frame on the wire, as the engine's `FrameMessage::encode` writes it.
    fn av1_frame(sequence: u64, keyframe: bool, data: &[u8]) -> Vec<u8> {
        FrameMessage {
            sequence,
            capture_us: 1_000_000 + sequence,
            readback_us: 0,
            encode_us: 0,
            wire_us: 0,
            last_input_seq: 0,
            last_input_client_us: 0,
            codec: Codec::Av1,
            keyframe,
            desc: ImageDesc {
                width: 320,
                height: 240,
                format: PixelFormat::Rgba8Unorm,
            },
            data: data.to_vec(),
        }
        .encode()
    }

    fn lz4_frame(data: &[u8]) -> Vec<u8> {
        FrameMessage {
            sequence: 1,
            capture_us: 7,
            readback_us: 0,
            encode_us: 0,
            wire_us: 0,
            last_input_seq: 0,
            last_input_client_us: 0,
            codec: Codec::Lz4,
            keyframe: true,
            desc: ImageDesc {
                width: 320,
                height: 240,
                format: PixelFormat::Rgba8Unorm,
            },
            data: data.to_vec(),
        }
        .encode()
    }

    /// A `Capabilities` envelope as the browser sends it: one message, `[count][codec…]`.
    fn capabilities(codecs: &[u8]) -> Vec<u8> {
        let mut payload = vec![codecs.len() as u8];
        payload.extend_from_slice(codecs);
        envelope(MessageType::Capabilities.to_code(), &payload)
    }

    #[test]
    fn an_av1_keyframe_from_the_engine_becomes_send_video() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.engine_sends(
            MessageType::Frame.to_code(),
            &av1_frame(1, true, b"\xa1\xb2"),
        );
        assert!(rig.wait(|c| c.frames_to_video == 1));
        // The encoded bytes, the keyframe bit and the capture clock all cross the seam — the video
        // track needs all three, and an RTP timestamp taken at write time would jitter.
        assert_eq!(
            rig.transport.videos(),
            vec![(b"\xa1\xb2".to_vec(), true, 1_000_001)]
        );
        // And nothing went out on the DataChannel: AV1 never travels there (contract.md).
        assert!(rig.transport.data().is_empty());
        let report = rig.relay.stop(StopReason::Requested);
        assert_eq!(report.reason, StopReason::Requested);
        assert_eq!(report.relay.frames_to_video, 1);
        // The lease came back, so a stopped relay does not consume one of three forwarded ports.
        assert_eq!(rig.ports.available(), rig.ports.capacity());
    }

    #[test]
    fn a_non_av1_frame_is_refused_without_lan_lossless() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.engine_sends(MessageType::Frame.to_code(), &lz4_frame(b"compressed"));
        assert!(rig.wait(|c| c.frames_refused_codec == 1));
        assert!(rig.transport.data().is_empty(), "the LZ4 frame escaped");
        assert!(rig.transport.videos().is_empty(), "LZ4 on the video track");

        // The same frame with the operator's flag set DOES cross, as the envelope it arrived as —
        // which is what makes the refusal above a policy and not an inability.
        let mut lan = Rig::new(Surface::Play, RelayConfig { lan_lossless: true });
        let payload = lz4_frame(b"compressed");
        lan.engine_sends(MessageType::Frame.to_code(), &payload);
        assert!(lan.wait(|c| c.frames_to_data == 1));
        assert_eq!(
            lan.transport.data(),
            vec![envelope(MessageType::Frame.to_code(), &payload)]
        );
    }

    #[test]
    fn a_busy_video_queue_drops_deltas_until_a_keyframe_and_asks_once() {
        let transport = FakeTransport::shared();
        transport.busy.store(true, Ordering::Release);
        let mut rig = Rig::with_transport(Surface::Play, RelayConfig::default(), transport);

        // The first frame is refused. That is what enters resync and asks the engine — once.
        rig.engine_sends(MessageType::Frame.to_code(), &av1_frame(1, false, b"d1"));
        assert!(rig.wait(|c| c.frames_dropped_busy == 1));
        let (ty, payload) = rig.engine_receives();
        assert_eq!(ty, MessageType::KeyframeRequest);
        assert!(payload.is_empty());

        // Every later delta is dropped BEFORE it is submitted: the chain they would extend is gone,
        // so forwarding them would paint corruption rather than a pause.
        for sequence in 2..=6 {
            rig.engine_sends(
                MessageType::Frame.to_code(),
                &av1_frame(sequence, false, b"dn"),
            );
        }
        assert!(rig.wait(|c| c.frames_dropped_resync == 5));

        // One request, not six. A request per dropped frame would flood the control path at 60 fps —
        // exactly when it is already congested.
        rig.transport.busy.store(false, Ordering::Release);
        rig.engine_sends(MessageType::Frame.to_code(), &av1_frame(7, true, b"key"));
        assert!(rig.wait(|c| c.frames_to_video == 1));
        let counters = rig.relay.counters();
        assert_eq!(counters.resyncs, 1, "{counters:?}");
        assert_eq!(counters.keyframe_requests_to_engine, 1, "{counters:?}");

        // Resync is over: a delta after the accepted keyframe is forwarded again.
        rig.engine_sends(MessageType::Frame.to_code(), &av1_frame(8, false, b"d8"));
        assert!(rig.wait(|c| c.frames_to_video == 2));
        assert_eq!(rig.relay.counters().frames_dropped_resync, 5);
    }

    #[test]
    fn capabilities_are_rewritten_to_av1_only_without_lan_lossless() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        // Raw, LZ4, JPEG, AV1, and a codec byte from the future. Only AV1 may survive.
        rig.transport.push(Event::Data {
            payload: capabilities(&[0, 1, 2, 3, 200]),
        });
        let (ty, payload) = rig.engine_receives();
        assert_eq!(ty, MessageType::Capabilities);
        assert_eq!(payload, vec![1, 3], "the engine must see AV1 and only AV1");
        let counters = rig.relay.counters();
        assert_eq!(counters.capabilities_rewritten, 1);
        assert_eq!(counters.codecs_removed, 4, "{counters:?}");

        // With the operator's flag, LZ4 survives too — and in the client's own preference order,
        // because preference belongs to the client (ADR-0030 §4).
        let mut lan = Rig::new(Surface::Play, RelayConfig { lan_lossless: true });
        lan.transport.push(Event::Data {
            payload: capabilities(&[1, 3]),
        });
        let (ty, payload) = lan.engine_receives();
        assert_eq!(ty, MessageType::Capabilities);
        assert_eq!(payload, vec![2, 1, 3]);
    }

    #[test]
    fn no_common_codec_stops_the_relay() {
        // A browser that can decode only JPEG: nothing survives the rewrite. Letting an empty list
        // through would have the engine pick something we then refuse frame by frame.
        let rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.transport.push(Event::Data {
            payload: capabilities(&[2]),
        });
        assert!(rig.wait_stopped());
        let mut rig = rig;
        assert_eq!(
            rig.relay.stop(StopReason::Requested).reason,
            StopReason::NoCommonCodec,
            "the stop must name the negotiation failure, not the teardown"
        );
    }

    #[test]
    fn input_from_the_browser_reaches_the_engine() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        let event = rime_protocol::InputEvent {
            kind: rime_protocol::InputKind::KeyDown,
            code: 65,
            x: 1,
            y: 2,
            scroll_x: 0.0,
            scroll_y: 0.0,
            mods: 0,
            client_us: 12,
            seq: 9,
        };
        let encoded = event.encode();
        rig.transport.push(Event::Data {
            payload: envelope(MessageType::Input.to_code(), &encoded),
        });
        let (ty, payload) = rig.engine_receives();
        assert_eq!(ty, MessageType::Input);
        assert_eq!(payload, encoded, "the 37 bytes must cross unchanged");

        // A payload that is not exactly one envelope is dropped and counted rather than reassembled:
        // the DataChannel is a message transport, so a stream reassembler here would be a bug
        // pretending to be robustness.
        for malformed in [
            vec![0x01], // shorter than a header
            envelope(MessageType::Input.to_code(), &[1]) // two envelopes in one message
                .into_iter()
                .chain(envelope(MessageType::Input.to_code(), &[2]))
                .collect::<Vec<u8>>(),
            vec![0x01, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x01], // length field disagrees
        ] {
            rig.transport.push(Event::Data { payload: malformed });
        }
        assert!(rig.wait(|c| c.malformed_from_browser == 3));
        assert_eq!(rig.relay.counters().messages_to_engine, 1);
    }

    #[test]
    fn the_editor_band_is_refused_on_a_play_surface() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.transport.push(Event::Data {
            payload: envelope(0x0201, b"SaveScene"),
        });
        assert!(rig.wait(|c| c.editor_refused_to_engine == 1));

        // Nothing reached the engine — proved by sending something that MAY cross afterwards and
        // finding it first. A refusal that still wrote the bytes would be the worst of both.
        rig.transport.push(Event::Data {
            payload: envelope(MessageType::KeyframeRequest.to_code(), &[]),
        });
        let (ty, _) = rig.engine_receives();
        assert_eq!(ty, MessageType::KeyframeRequest);

        // And on an edit session the same message goes through, so the refusal is about the surface
        // and not about the relay being unable to carry the band.
        let mut edit = Rig::new(Surface::Edit, RelayConfig::default());
        edit.transport.push(Event::Data {
            payload: envelope(0x0201, b"SaveScene"),
        });
        let (ty, payload) = edit.engine_receives();
        assert_eq!(ty.to_code(), 0x0201);
        assert_eq!(payload, b"SaveScene");
        assert_eq!(edit.relay.counters().editor_refused_to_engine, 0);
    }

    #[test]
    fn a_keyframe_request_from_the_browser_side_reaches_the_engine() {
        // The transport's PLI/FIR event, not a DataChannel message: the browser's decoder lost the
        // chain and RTCP says so. The engine owns the encoder, so it has to hear about it.
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.transport.push(Event::KeyframeRequested);
        let (ty, payload) = rig.engine_receives();
        assert_eq!(ty, MessageType::KeyframeRequest);
        assert!(payload.is_empty());
        assert_eq!(rig.relay.counters().keyframe_requests_to_engine, 1);
    }

    #[test]
    fn engine_eof_stops_the_relay_and_returns_the_port() {
        let rig = Rig::new(Surface::Play, RelayConfig::default());
        let free_before = rig.ports.available();
        assert_eq!(free_before, rig.ports.capacity() - 1, "the lease is held");

        // The engine process exits: its end of the socket closes.
        let Rig {
            engine,
            mut relay,
            ports,
            transport,
            ..
        } = rig;
        drop(engine);

        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && !relay.is_stopped() {
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(relay.is_stopped(), "engine EOF did not stop the relay");
        let report = relay.stop(StopReason::Requested);
        assert_eq!(report.reason, StopReason::EngineClosed);
        assert_eq!(
            ports.available(),
            ports.capacity(),
            "a stopped relay must give its forwarded port back"
        );
        assert!(
            transport.closed.load(Ordering::Acquire),
            "the transport outlived the relay"
        );
    }

    #[test]
    fn a_disconnected_transport_stops_the_relay() {
        let rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.transport.push(Event::Disconnected);
        assert!(rig.wait_stopped());
        let mut rig = rig;
        assert_eq!(
            rig.relay.stop(StopReason::Requested).reason,
            StopReason::TransportGone
        );
    }

    #[test]
    fn stop_is_idempotent() {
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        rig.engine_sends(MessageType::Frame.to_code(), &av1_frame(1, true, b"k"));
        assert!(rig.wait(|c| c.frames_to_video == 1));

        let first = rig.relay.stop(StopReason::Requested);
        let second = rig.relay.stop(StopReason::BrowserBye);
        assert_eq!(first, second, "a second stop invented a new report");
        // Closed exactly once. A second close would join an already-joined thread and, on the real
        // transport, try to release a port that belongs to the next session by then.
        assert_eq!(rig.transport.closes.load(Ordering::Acquire), 1);
        assert!(rig.relay.is_stopped());
        // And once more after the drop-glue path has run: `Drop` calls `stop` too.
        drop(rig.relay);
        assert_eq!(rig.transport.closes.load(Ordering::Acquire), 1);
    }

    #[test]
    fn a_bye_in_either_direction_is_forwarded_and_then_stops_the_relay() {
        // Not on the spec's list, but the two `Bye` paths are the only ones that both forward a
        // message AND stop, and getting that order backwards would close the session before telling
        // the other end.
        let mut engine_side = Rig::new(Surface::Play, RelayConfig::default());
        engine_side.engine_sends(MessageType::Bye.to_code(), &[]);
        assert!(engine_side.wait(|c| c.control_to_browser == 1));
        assert_eq!(
            engine_side.transport.data(),
            vec![envelope(MessageType::Bye.to_code(), &[])]
        );
        assert!(engine_side.wait_stopped());
        assert_eq!(
            engine_side.relay.stop(StopReason::Requested).reason,
            StopReason::EngineBye
        );

        let mut browser_side = Rig::new(Surface::Play, RelayConfig::default());
        browser_side.transport.push(Event::Data {
            payload: envelope(MessageType::Bye.to_code(), &[]),
        });
        let (ty, _) = browser_side.engine_receives();
        assert_eq!(ty, MessageType::Bye);
        assert!(browser_side.wait_stopped());
        assert_eq!(
            browser_side.relay.stop(StopReason::Requested).reason,
            StopReason::BrowserBye
        );
    }

    #[test]
    fn a_stalled_control_path_stops_the_relay_rather_than_dropping_the_message() {
        // The mirror of the video rule, and the reason the two are written separately: `StreamConfig`
        // is what stands the decoder up, so a relay that dropped it would connect, negotiate, and
        // show nothing at all.
        let transport = FakeTransport::shared();
        transport.busy.store(true, Ordering::Release);
        let mut rig = Rig::with_transport(Surface::Play, RelayConfig::default(), transport);
        let started = Instant::now();
        rig.engine_sends(MessageType::StreamConfig.to_code(), &[3, 0, 1, 0, 0, 0]);
        assert!(rig.wait_stopped(), "a stalled control path did not stop");
        assert_eq!(
            rig.relay.stop(StopReason::Requested).reason,
            StopReason::ControlStalled
        );
        // It retried for about a second before giving up: neither instant (which would drop a
        // message that is not droppable) nor forever.
        assert!(
            started.elapsed() >= CONTROL_RETRY_BUDGET,
            "gave up after {:?}",
            started.elapsed()
        );
        assert!(rig.relay.counters().control_retries > 0);
    }

    #[test]
    fn a_message_the_browser_may_not_send_never_reaches_the_engine() {
        // `Frame` and `StreamConfig` are server→client. A browser sending one is confused or probing;
        // either way the engine's dispatcher never has to have an opinion about it.
        let mut rig = Rig::new(Surface::Play, RelayConfig::default());
        for code in [
            MessageType::Frame.to_code(),
            MessageType::StreamConfig.to_code(),
            0x0999,
        ] {
            rig.transport.push(Event::Data {
                payload: envelope(code, b"nope"),
            });
        }
        assert!(rig.wait(|c| c.refused_from_browser == 3));
        // Proved by what arrives next: the first thing the engine reads is the legitimate message.
        rig.transport.push(Event::Data {
            payload: envelope(MessageType::KeyframeRequest.to_code(), &[]),
        });
        let (ty, _) = rig.engine_receives();
        assert_eq!(ty, MessageType::KeyframeRequest);
    }

    #[test]
    fn an_unexpected_message_from_the_engine_is_counted_not_forwarded() {
        let rig = Rig::new(Surface::Play, RelayConfig::default());
        let mut rig = rig;
        // `Input` is client→server. The engine sending one is an engine bug, and inventing a meaning
        // for it here is how a relay becomes a second, undocumented protocol.
        rig.engine_sends(MessageType::Input.to_code(), &[0u8; 37]);
        assert!(rig.wait(|c| c.unexpected_from_engine == 1));
        assert!(rig.transport.data().is_empty());
    }

    // ── The one test with real sockets ────────────────────────────────────────────────────────────

    /// THE PROOF OF THIS BRICK: a real SDP offer from a conformant `str0m` peer, answered on a real
    /// UDP socket, with a real engine `Frame` going in one end and AV1 RTP coming out the other.
    ///
    /// Everything above runs on the fake transport, which proves the relay's *decisions*. This proves
    /// that the decisions are wired to something that works: ICE, DTLS-SRTP, the AV1 packetizer, the
    /// DataChannel, and the relay's two threads, all at once. It cannot prove browser interop —
    /// nothing without a browser can — and the page brick says so too.
    #[test]
    fn a_real_offer_carries_av1_to_a_str0m_peer() {
        use crate::test_peer::{av1_test_keyframe, TestPeer};

        let (mut peer, offer) = TestPeer::new_with_video(true);
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        peer.accept_answer(&answer);

        // Connect BEFORE the relay starts. The transport drops video that arrives before its track is
        // writable — deliberately, since stale video is worse than none — so a test that raced the
        // handshake would be measuring that drop instead of the relay.
        assert!(
            peer.pump(Duration::from_secs(10), |p| p.connected),
            "the peer never opened its channel"
        );

        let (engine, gateway) = UnixStream::pair().unwrap();
        engine
            .set_read_timeout(Some(Duration::from_secs(10)))
            .unwrap();
        let ports = pool();
        let lease = ports.lease().unwrap();
        let mut relay = Relay::start(
            Session::new(gateway.try_clone().unwrap(), Surface::Play),
            Session::new(gateway, Surface::Play),
            Box::new(transport),
            lease,
            Box::new(()),
            RelayConfig::default(),
        );

        // One AV1 keyframe, framed exactly as the engine frames it.
        let mut engine = Connection::new(engine);
        let frame = FrameMessage {
            sequence: 1,
            capture_us: 1_000_000,
            readback_us: 0,
            encode_us: 0,
            wire_us: 0,
            last_input_seq: 0,
            last_input_client_us: 0,
            codec: Codec::Av1,
            keyframe: true,
            desc: ImageDesc {
                width: 320,
                height: 240,
                format: PixelFormat::Rgba8Unorm,
            },
            data: av1_test_keyframe(),
        };
        engine
            .send(MessageType::Frame, &frame.encode())
            .expect("the engine end writes");

        assert!(
            peer.pump(Duration::from_secs(10), |p| !p.received_video.is_empty()),
            "no AV1 reached the peer's video track"
        );
        // RFC AV1 RTP packetization strips the temporal delimiter; the two size-bearing OBUs and
        // their exact payload bytes are reconstructed. Same expectation as the transport's own test,
        // which is the point: the relay changed nothing about the bytes.
        assert_eq!(
            peer.received_video,
            vec![vec![0x0a, 0x02, 0x01, 0x02, 0x32, 0x03, 0xa1, 0xb2, 0xc3]]
        );

        // The other direction lives too: the newly writable track asked for a keyframe, and that
        // request crossed the relay into the engine socket as the protocol's own message.
        let (ty, payload) = engine.recv().expect("the engine end reads");
        assert_eq!(ty, MessageType::KeyframeRequest);
        assert!(payload.is_empty());

        let report = relay.stop(StopReason::Requested);
        assert_eq!(report.relay.frames_to_video, 1, "{report:?}");
        assert_eq!(report.transport.video_frames_out, 1, "{report:?}");
        assert_eq!(report.relay.keyframe_requests_to_engine, 1, "{report:?}");
        assert_eq!(ports.available(), ports.capacity());
    }
}
