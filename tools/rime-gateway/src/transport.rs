// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The browser transport — WebRTC, behind a seam narrow enough to replace.
//!
//! ADR-0045 decision 3 ratified WebRTC with input over a DataChannel. This module is the one place
//! that knows it, and the seam is the point of the file rather than the crate behind it.
//!
//! **What crosses the seam:** owned byte buffers, SDP as opaque text, and connection state. Nothing
//! else. No `Rtc`, no `Mid`, no futures, no SSRCs, no payload types, no `str0m` error type. A
//! replacement implementation could own an async runtime internally and nothing outside this file
//! would change — which is the property that makes a transport decision reversible rather than
//! permanent.
//!
//! **Why `str0m` and not a full stack.** It is sans-IO: it owns no sockets, spawns no threads and
//! forces no runtime on the caller. That matters because this crate's HTTP server and process
//! supervisor are *already written and merged*, blocking. A full-stack alternative would not have
//! added a dependency to new code — it would have forced a rewrite of two landed bricks. The cost is
//! that the run loop below is ours to get right, and it is the part of this file worth reading twice.
//!
//! **The surface policy still applies and is NOT applied here.** A play session must never carry the
//! `0x0200..=0x02FF` editor band, and that filter lives in [`crate::Session`] on the raw wire code.
//! This module moves opaque bytes; it has no opinion about what they mean, and giving it one would put
//! the security decision in two places. The caller decodes a DataChannel payload, runs it through the
//! session's policy, and only then forwards it.
//!
//! **What this brick proves and what it does not.** The test at the bottom stands up a real WebRTC
//! connection — ICE, DTLS-SRTP, SCTP — between this transport and a conformant `str0m` peer, over
//! real loopback UDP sockets, and sends a DataChannel message each way. That proves the run loop,
//! the socket plumbing, the SDP exchange and the wakeup mechanism. It does **not** prove browser
//! interop, which nothing without a browser can; the browser page is a separate brick.

use std::collections::VecDeque;
use std::net::{SocketAddr, UdpSocket};
use std::sync::mpsc::{Receiver, RecvTimeoutError, SyncSender, TrySendError};
use std::sync::{mpsc, Arc};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use str0m::change::SdpOffer;
use str0m::channel::ChannelId;
use str0m::media::{MediaTime, Mid, Pt};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event as RtcEvent, IceConnectionState, Input, Output, Rtc};

/// How many commands may be queued before `submit` refuses.
///
/// Small on purpose. A deep queue turns backpressure into latency, and this transport carries
/// interactive input — a keystroke that arrives late is worse than one refused loudly.
pub const COMMAND_QUEUE: usize = 64;

/// How many events may be queued before the transport thread starts dropping them.
///
/// Dropping is counted, never silent ([`TransportCounters::events_dropped`]): a consumer that stopped
/// reading is a bug in the consumer, and a transport that blocks waiting for it would take the whole
/// session down instead of reporting it.
pub const EVENT_QUEUE: usize = 256;

/// How many messages may wait for the DataChannel to open before the oldest are dropped.
///
/// **This bound exists because the command queue's backpressure is worthless without it.** The run
/// loop drains `COMMAND_QUEUE` eagerly — that is its job — so a caller sending before the channel
/// opens does not fill the command queue at all: it fills the loop's own `pending` deque. Leaving that
/// deque unbounded put a caller-controlled allocation behind a carefully bounded one, which is the
/// same class of bug as an unbounded HTTP header. Found by the backpressure test, which failed for the
/// right reason.
///
/// Oldest-first drop rather than newest: this carries interactive input, and the stale keystroke is
/// the one worth losing.
pub const PENDING_QUEUE: usize = 256;

/// The largest datagram we will read from the socket. 2 KiB covers a path-MTU-sized packet with room
/// to spare; anything larger is not something a conformant peer sends.
const RECV_BUFFER: usize = 2048;

/// The longest the loop will block on the socket, regardless of what the RTC state machine asks for.
///
/// A ceiling rather than the timeout itself. `str0m` may legitimately ask to sleep for seconds when a
/// connection is idle, and the loop must still notice a `Close` that the wakeup datagram lost — a
/// belt to the wakeup's braces, at the cost of one wakeup per 50 ms on an idle session.
const MAX_POLL: Duration = Duration::from_millis(50);

/// Something the caller wants the transport to do.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Send bytes on the input DataChannel. Opaque here: see the module note on the surface policy.
    SendData { payload: Vec<u8> },
    /// One encoded AV1 temporal unit from the engine wire. Capture time is monotonic microseconds
    /// and becomes the 90 kHz RTP timestamp; callers must not send it backwards.
    SendVideo {
        frame: Vec<u8>,
        keyframe: bool,
        capture_micros: u64,
    },
    /// Shut the connection down and end the thread.
    Close,
}

/// Something that happened. Connection state and inbound bytes, and nothing that names a codec, a
/// stream id or an SDP field — those are the implementation's business.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// ICE reached a usable state and the DataChannel is open. The first moment sending is worthwhile.
    Connected,
    /// Bytes arrived on the DataChannel.
    Data { payload: Vec<u8> },
    /// The peer sent PLI/FIR, or a newly writable track needs its first decodable frame. The
    /// caller forwards this as an engine `KeyframeRequest`; no codec detail crosses this seam.
    KeyframeRequested,
    /// The peer went away, or `Close` was honoured. Terminal.
    Disconnected,
    /// The connection failed. Terminal, and the string is for the operator's log rather than the peer.
    Failed(String),
}

/// Why a transport could not be created or fed.
#[derive(Debug)]
pub enum TransportError {
    /// The offer was not parseable SDP.
    BadOffer(String),
    /// The RTC state machine refused the offer (no compatible media, bad fingerprint, …).
    Negotiation(String),
    /// A socket could not be bound, or the bind was refused by policy.
    Io(std::io::Error),
    /// The command queue is full. Backpressure, not a failure — retry or drop.
    Busy,
    /// The transport thread is gone.
    Closed,
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportError::BadOffer(e) => write!(f, "malformed SDP offer: {e}"),
            TransportError::Negotiation(e) => write!(f, "could not accept the offer: {e}"),
            TransportError::Io(e) => write!(f, "transport i/o: {e}"),
            TransportError::Busy => write!(f, "the transport's command queue is full"),
            TransportError::Closed => write!(f, "the transport is closed"),
        }
    }
}

impl std::error::Error for TransportError {}

/// What the transport actually did, for the same reason every other module here counts.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct TransportCounters {
    pub datagrams_in: u64,
    pub datagrams_out: u64,
    /// Datagrams `str0m` could not parse. Normal in small numbers on a public socket (scanners); a
    /// flood is worth noticing, which is why it is a number and not a log line.
    pub datagrams_rejected: u64,
    pub data_messages_in: u64,
    pub data_messages_out: u64,
    /// Frames written to the AV1 track, so a silent video path is visible to the operator.
    pub video_frames_out: u64,
    /// Frames sent before ICE and the negotiated AV1 track were writable; stale frames are dropped.
    pub video_frames_dropped_not_ready: u64,
    /// Frames refused because the offer has no AV1 video sender, while input can still work.
    pub video_frames_dropped_no_track: u64,
    /// Deltas refused before a keyframe, since a fresh decoder could not decode them.
    pub video_deltas_dropped_before_keyframe: u64,
    /// PLI/FIR feedback received, so engine keyframe demand can be diagnosed.
    pub keyframe_requests_in: u64,
    /// Frames the state machine refused at write time (no writer for the mid, or `write` erred).
    /// Should stay zero; a drop path without a counter is one that reads as "video is fine".
    pub video_frames_write_failed: u64,
    /// Sends that found no open channel and were queued. A caller sending before [`Event::Connected`].
    pub data_sends_early: u64,
    /// Sends dropped because [`PENDING_QUEUE`] was full. Never silent: "my input did nothing" has to
    /// have an answer, and this is it.
    pub data_sends_dropped: u64,
    /// Events dropped because the consumer was not reading. See [`EVENT_QUEUE`].
    pub events_dropped: u64,
    /// Wakeup datagrams consumed. Proof the mechanism is in use rather than the 50 ms ceiling
    /// carrying the whole load, which would look identical from outside.
    pub wakeups: u64,
}

/// A live browser connection.
///
/// Owns one thread and one UDP socket. Dropping it closes both: `Drop` sends `Close`, wakes the loop
/// and **joins** the thread, so a dropped transport is a released port rather than a thread still
/// holding one. That is the same discipline [`crate::SessionHandle`] applies to a child process, and
/// for the same reason — a resource whose release is asynchronous is a resource that leaks under load.
pub struct Str0mTransport {
    commands: SyncSender<Command>,
    events: Receiver<Event>,
    /// Where to send a wakeup datagram, and the magic that identifies one.
    waker: Arc<Waker>,
    thread: Option<JoinHandle<TransportCounters>>,
    local_addr: SocketAddr,
    last_counters: TransportCounters,
}

/// How another thread interrupts the loop's blocking socket read.
///
/// **A sans-IO state machine has no way to be woken**, so this is ours to provide. The loop blocks in
/// `recv_from` until either a real packet arrives or the RTC timeout expires; a command submitted
/// meanwhile would otherwise wait out that timeout. So `submit` sends a datagram to the loop's own
/// socket from a throwaway one, and the loop treats that payload as "drain the command queue".
///
/// The magic is 16 random bytes generated per transport rather than a constant, so a remote peer
/// cannot synthesise a wakeup by guessing it. The consequence of a forged one would only be an
/// unnecessary queue drain, but an unguessable token costs nothing and removes the question.
struct Waker {
    target: SocketAddr,
    magic: [u8; 16],
}

impl Waker {
    fn wake(&self) {
        // A fresh ephemeral socket per wakeup: cheap on loopback, and it avoids holding a second fd
        // for the transport's whole life to send a handful of bytes.
        let bind: SocketAddr = if self.target.is_ipv4() {
            "127.0.0.1:0".parse().expect("literal")
        } else {
            "[::1]:0".parse().expect("literal")
        };
        if let Ok(sock) = UdpSocket::bind(bind) {
            let _ = sock.send_to(&self.magic, self.target);
        }
    }
}

impl Str0mTransport {
    /// Accept a browser's SDP offer and return the answer, with the connection already running.
    ///
    /// Synchronous on purpose: signalling is an HTTP request/response, so the answer must be in hand
    /// before the route returns. Everything after this is asynchronous and lives on the thread.
    ///
    /// `bind` is the address the media socket listens on, and it goes through [`crate::http::check_bind`]
    /// for the same reason the HTTP listener does: a wildcard media socket is as much an exposure as a
    /// wildcard HTTP one, and the containment boundary must not have a side door.
    pub fn accept_offer(
        offer_sdp: &str,
        bind: SocketAddr,
    ) -> Result<(Self, String), TransportError> {
        crate::http::check_bind(bind).map_err(|e| {
            TransportError::Io(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                e.to_string(),
            ))
        })?;

        let socket = UdpSocket::bind(bind).map_err(TransportError::Io)?;
        let local_addr = socket.local_addr().map_err(TransportError::Io)?;

        // Restrict the answer to AV1: an offer with another video codec still keeps its data
        // channel, but must never make us claim an encoder format we cannot send.
        let mut rtc = Rtc::builder()
            .set_ice_lite(true)
            .clear_codecs()
            .enable_av1(true)
            .build(Instant::now());
        // ICE-lite because this side is the server: it has a stable, reachable address and does not
        // need to probe the peer's. It halves the state machine's work and removes a class of
        // connectivity-check bug we would otherwise own.
        let candidate = Candidate::host(local_addr, "udp")
            .map_err(|e| TransportError::Negotiation(e.to_string()))?;
        rtc.add_local_candidate(candidate);

        let offer = SdpOffer::from_sdp_string(offer_sdp)
            .map_err(|e| TransportError::BadOffer(e.to_string()))?;
        let answer = rtc
            .sdp_api()
            .accept_offer(offer)
            .map_err(|e| TransportError::Negotiation(e.to_string()))?;
        let answer_sdp = answer.to_sdp_string();
        // The accepted answer is authoritative: an unoffered or rejected video m-line must not
        // become a send target merely because a codec is configured locally.
        let video_answer = av1_video_answer(&answer_sdp);

        let (cmd_tx, cmd_rx) = mpsc::sync_channel::<Command>(COMMAND_QUEUE);
        let (ev_tx, ev_rx) = mpsc::sync_channel::<Event>(EVENT_QUEUE);

        let mut magic = [0u8; 16];
        read_random(&mut magic).map_err(TransportError::Io)?;
        let waker = Arc::new(Waker {
            target: local_addr,
            magic,
        });

        let loop_magic = magic;
        let thread = std::thread::Builder::new()
            .name("rime-gateway-transport".into())
            .spawn(move || run_loop(rtc, socket, cmd_rx, ev_tx, loop_magic, video_answer))
            .map_err(TransportError::Io)?;

        Ok((
            Self {
                commands: cmd_tx,
                events: ev_rx,
                waker,
                thread: Some(thread),
                local_addr,
                last_counters: TransportCounters::default(),
            },
            answer_sdp,
        ))
    }

    /// The address the media socket is listening on. With a port of 0 in `bind`, this is how the
    /// caller learns which port to advertise.
    #[must_use]
    pub fn local_addr(&self) -> SocketAddr {
        self.local_addr
    }

    /// Queue a command and wake the loop. Refuses rather than blocks when the queue is full.
    pub fn submit(&self, command: Command) -> Result<(), TransportError> {
        match self.commands.try_send(command) {
            Ok(()) => {
                self.waker.wake();
                Ok(())
            }
            Err(TrySendError::Full(_)) => Err(TransportError::Busy),
            Err(TrySendError::Disconnected(_)) => Err(TransportError::Closed),
        }
    }

    /// Wait up to `timeout` for the next event.
    ///
    /// Blocking-with-timeout rather than a bare `try_recv`, deliberately: a caller polling a
    /// non-blocking queue in a loop burns a core on an idle session, and "poll faster" is not a
    /// design. A caller that wants no blocking passes `Duration::ZERO`.
    pub fn poll_event(&self, timeout: Duration) -> Option<Event> {
        match self.events.recv_timeout(timeout) {
            Ok(event) => Some(event),
            Err(RecvTimeoutError::Timeout) => None,
            // The thread ended. Report it as the terminal event rather than as nothing, so a caller
            // waiting for Disconnected cannot wait forever.
            Err(RecvTimeoutError::Disconnected) => Some(Event::Disconnected),
        }
    }

    /// Shut down and collect the counters. Idempotent.
    pub fn close(&mut self) -> TransportCounters {
        let _ = self.commands.try_send(Command::Close);
        self.waker.wake();
        if let Some(thread) = self.thread.take() {
            if let Ok(counters) = thread.join() {
                self.last_counters = counters;
            }
        }
        self.last_counters
    }
}

impl Drop for Str0mTransport {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

impl std::fmt::Debug for Str0mTransport {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Str0mTransport")
            .field("local_addr", &self.local_addr)
            .field("running", &self.thread.is_some())
            .finish()
    }
}

/// 128 bits from the OS pool, the same way [`crate::SessionId`] gets its bits and for a related
/// reason: a guessable token is not a token. Kept here rather than shared because this module is
/// portable and `admission` is Unix-only.
fn read_random(out: &mut [u8]) -> std::io::Result<()> {
    #[cfg(unix)]
    {
        use std::io::Read;
        std::fs::File::open("/dev/urandom")?.read_exact(out)
    }
    #[cfg(not(unix))]
    {
        // Windows/macOS, which CI builds and tests. Production hosting is Linux (ADR-0047 §3) and takes
        // the branch above; this only has to make a LOOPBACK wakeup token unpredictable per process.
        //
        // Written without `DefaultHasher` on purpose: the obvious version hashes `Instant::now()`, and
        // `Instant` does not implement `Hash` — a mistake that compiles fine on this Linux workstation
        // because the whole block is behind `cfg(not(unix))` and would only turn red on two of the
        // three CI platforms. That exact shape has cost this crate three red runs already.
        use std::time::{SystemTime, UNIX_EPOCH};
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let mut state = nanos as u64 ^ ((std::process::id() as u64) << 32) ^ (out.as_ptr() as u64);
        for b in out.iter_mut() {
            // SplitMix64: a few lines, no dependency, and good enough for a loopback nonce.
            state = state.wrapping_add(0x9E3779B97F4A7C15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
            *b = (z ^ (z >> 31)) as u8;
        }
        Ok(())
    }
}

/// Queue a payload for the DataChannel, dropping the oldest if the bound is reached.
fn push_bounded(
    pending: &mut VecDeque<Vec<u8>>,
    payload: Vec<u8>,
    counters: &mut TransportCounters,
) {
    while pending.len() >= PENDING_QUEUE {
        pending.pop_front();
        counters.data_sends_dropped += 1;
    }
    pending.push_back(payload);
}

/// The answer is internal signalling state; only its AV1 sending mid and payload type are needed
/// by the loop. A rejected or recvonly video section is not a writable track.
///
/// This parses **our own** answer — text `str0m` just generated from a codec set restricted to AV1 —
/// never the peer's offer, so the format it has to understand is one this module controls. The
/// alternative, waiting for `MediaAdded` and asking the writer for its payload parameters, would make
/// the track's existence depend on event ordering inside the loop.
fn av1_video_answer(sdp: &str) -> Option<(String, u8)> {
    let mut video = false;
    let mut active = false;
    let mut sending = true;
    let mut mid = None;
    let mut pt = None;
    for line in sdp.lines().map(str::trim) {
        if line.starts_with("m=") {
            if video && active && sending {
                if let (Some(mid), Some(pt)) = (mid.take(), pt) {
                    return Some((mid, pt));
                }
            }
            let fields: Vec<_> = line.split_whitespace().collect();
            video = fields.first() == Some(&"m=video");
            active = video && fields.get(1) != Some(&"0");
            sending = true;
            mid = None;
            pt = None;
        } else if video {
            if let Some(value) = line.strip_prefix("a=mid:") {
                mid = Some(value.to_string());
            } else if line == "a=recvonly" || line == "a=inactive" {
                sending = false;
            } else if let Some(value) = line.strip_prefix("a=rtpmap:") {
                if let Some((number, codec)) = value.split_once(' ') {
                    if codec.eq_ignore_ascii_case("AV1/90000") {
                        pt = number.parse().ok();
                    }
                }
            }
        }
    }
    if video && active && sending {
        mid.zip(pt)
    } else {
        None
    }
}

/// The run loop: drive the state machine, move datagrams, translate events.
///
/// The shape is `str0m`'s documented contract and it is not optional. Poll outputs until the state
/// machine asks for a timeout; send every `Transmit` it produced; then block on the socket for at
/// most that long and feed back either the datagram that arrived or the timeout that expired. Getting
/// this wrong does not fail loudly — it produces a connection that negotiates and then stalls,
/// because nothing retransmits.
fn run_loop(
    mut rtc: Rtc,
    socket: UdpSocket,
    commands: Receiver<Command>,
    events: SyncSender<Event>,
    magic: [u8; 16],
    video_answer: Option<(String, u8)>,
) -> TransportCounters {
    let mut counters = TransportCounters::default();
    let mut buf = vec![0u8; RECV_BUFFER];
    let mut channel: Option<ChannelId> = None;
    let mut pending: VecDeque<Vec<u8>> = VecDeque::new();
    let mut announced_connected = false;
    let mut ice_connected = false;
    // The accepted answer already names the negotiated sending Mid. This avoids depending on
    // when str0m emits MediaAdded, which may be later than the first writable ICE state.
    let video_mid = video_answer
        .as_ref()
        .map(|(mid, _)| Mid::from(mid.as_str()));
    let mut announced_writable = false;
    let mut wrote_keyframe = false;
    let mut last_capture_micros = 0;
    let local_addr = socket.local_addr().ok();

    let emit = |counters: &mut TransportCounters, event: Event| match events.try_send(event) {
        Ok(()) => {}
        Err(_) => counters.events_dropped += 1,
    };

    loop {
        if !rtc.is_alive() {
            emit(&mut counters, Event::Disconnected);
            return counters;
        }

        // Anything queued to send that now has a channel to go out on.
        if let Some(id) = channel {
            while let Some(payload) = pending.pop_front() {
                if let Some(mut ch) = rtc.channel(id) {
                    if ch.write(true, &payload).is_ok() {
                        counters.data_messages_out += 1;
                    }
                } else {
                    pending.push_front(payload);
                    break;
                }
            }
        }

        let timeout = loop {
            match rtc.poll_output() {
                Ok(Output::Timeout(t)) => break t,
                Ok(Output::Transmit(t)) => {
                    // A send failure is deliberately NOT fatal: UDP on a transient route error will
                    // succeed on the next retransmit, and tearing the session down would be a worse
                    // answer than letting the state machine retry.
                    if socket.send_to(&t.contents, t.destination).is_ok() {
                        counters.datagrams_out += 1;
                    }
                }
                Ok(Output::Event(event)) => match event {
                    // Both count. As the ICE-lite side this transport never reports `Connected`:
                    // measured on the loopback test, it goes `Checking` -> `Completed`, and a check
                    // for `Connected` alone left the video track unwritable forever.
                    RtcEvent::IceConnectionStateChange(
                        IceConnectionState::Connected | IceConnectionState::Completed,
                    ) => {
                        ice_connected = true;
                    }
                    RtcEvent::IceConnectionStateChange(IceConnectionState::Disconnected) => {
                        emit(&mut counters, Event::Disconnected);
                        return counters;
                    }
                    RtcEvent::ChannelOpen(id, _label) => {
                        channel = Some(id);
                        if !announced_connected {
                            announced_connected = true;
                            emit(&mut counters, Event::Connected);
                        }
                    }
                    RtcEvent::KeyframeRequest(request) => {
                        if video_mid == Some(request.mid) {
                            // PLI/FIR is a decoder recovery signal; the engine owns the encoder,
                            // so pass its demand across the byte-shaped seam without codec data.
                            counters.keyframe_requests_in += 1;
                            emit(&mut counters, Event::KeyframeRequested);
                        }
                    }
                    RtcEvent::ChannelData(data) => {
                        counters.data_messages_in += 1;
                        emit(&mut counters, Event::Data { payload: data.data });
                    }
                    RtcEvent::ChannelClose(_) => {
                        channel = None;
                    }
                    _ => {}
                },
                Err(e) => {
                    emit(&mut counters, Event::Failed(e.to_string()));
                    return counters;
                }
            }
        };

        // ICE and the negotiated Mid/PT must all exist before a write: str0m drops pre-ICE
        // media, and a decoder joining a live stream needs a new keyframe exactly once.
        if ice_connected && video_mid.is_some() && video_answer.is_some() && !announced_writable {
            announced_writable = true;
            emit(&mut counters, Event::KeyframeRequested);
        }

        // The ceiling: see MAX_POLL. `saturating_duration_since` rather than subtraction because a
        // timeout already in the past is normal (the poll above took time) and must become "don't
        // block", not a panic.
        let wait = timeout
            .saturating_duration_since(Instant::now())
            .min(MAX_POLL)
            .max(Duration::from_millis(1));
        if socket.set_read_timeout(Some(wait)).is_err() {
            emit(
                &mut counters,
                Event::Failed("could not set a socket timeout".into()),
            );
            return counters;
        }

        let now = Instant::now();
        match socket.recv_from(&mut buf) {
            Ok((n, source)) => {
                if n == magic.len() && buf[..n] == magic {
                    counters.wakeups += 1;
                } else {
                    counters.datagrams_in += 1;
                    let destination = local_addr.unwrap_or(source);
                    match Receive::new(Protocol::Udp, source, destination, &buf[..n]) {
                        Ok(receive) => {
                            if rtc.handle_input(Input::Receive(now, receive)).is_err() {
                                // A malformed-but-parseable datagram is the peer's problem, not ours.
                                counters.datagrams_rejected += 1;
                            }
                        }
                        Err(_) => counters.datagrams_rejected += 1,
                    }
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) =>
            {
                if rtc.handle_input(Input::Timeout(now)).is_err() {
                    emit(
                        &mut counters,
                        Event::Failed("rtc rejected a timeout".into()),
                    );
                    return counters;
                }
            }
            Err(e) => {
                emit(&mut counters, Event::Failed(format!("socket read: {e}")));
                return counters;
            }
        }

        // Commands last, so a Close is honoured after this iteration's traffic has been processed
        // rather than stranding a datagram that was already in hand.
        while let Ok(command) = commands.try_recv() {
            match command {
                Command::SendData { payload } => match channel {
                    Some(id) => match rtc.channel(id) {
                        Some(mut ch) => {
                            if ch.write(true, &payload).is_ok() {
                                counters.data_messages_out += 1;
                            }
                        }
                        None => push_bounded(&mut pending, payload, &mut counters),
                    },
                    None => {
                        // Before the channel opened. Queued rather than dropped, and counted so
                        // "my input did nothing" has an explanation.
                        counters.data_sends_early += 1;
                        push_bounded(&mut pending, payload, &mut counters);
                    }
                },
                Command::SendVideo {
                    frame,
                    keyframe,
                    capture_micros,
                } => {
                    if video_answer.is_none() {
                        // DataChannel-only offers are valid for input; report missing video via
                        // counters instead of failing the whole session.
                        counters.video_frames_dropped_no_track += 1;
                    } else if !ice_connected || video_mid.is_none() {
                        // Stale video is worse than none, so unlike early input it is never queued.
                        counters.video_frames_dropped_not_ready += 1;
                    } else if !keyframe && !wrote_keyframe {
                        // A delta cannot initialize a decoder even after the track is writable.
                        counters.video_deltas_dropped_before_keyframe += 1;
                    } else if let (Some(mid), Some((_, pt))) = (video_mid, video_answer.as_ref()) {
                        // Saturating monotonic time protects RTP order if a caller supplies an
                        // older timestamp; u128 avoids overflow during the 90 kHz conversion.
                        // Instant::now() describes this write's wallclock, not capture time.
                        let capture = capture_micros.max(last_capture_micros);
                        let ticks =
                            ((capture as u128 * 90_000) / 1_000_000).min(u64::MAX as u128) as u64;
                        let written = rtc.writer(mid).is_some_and(|writer| {
                            writer
                                .write(
                                    Pt::from(*pt),
                                    Instant::now(),
                                    MediaTime::new(
                                        ticks,
                                        std::num::NonZeroU32::new(90_000)
                                            .expect("nonzero RTP rate")
                                            .into(),
                                    ),
                                    frame,
                                )
                                .is_ok()
                        });
                        if written {
                            last_capture_micros = capture;
                            wrote_keyframe |= keyframe;
                            counters.video_frames_out += 1;
                        } else {
                            counters.video_frames_write_failed += 1;
                        }
                    }
                }
                Command::Close => {
                    rtc.disconnect();
                    emit(&mut counters, Event::Disconnected);
                    return counters;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use str0m::change::SdpAnswer;
    use str0m::media::{Direction, KeyframeRequestKind, MediaKind};

    #[test]
    fn the_unspecified_address_is_refused_for_the_media_socket_too() {
        // The HTTP listener refuses a wildcard bind; a media socket that did not would be a side door
        // in the same containment boundary. Checked here because it is easy to add the second
        // listener and forget the first one's rule.
        for wildcard in ["0.0.0.0:0", "[::]:0"] {
            let addr: SocketAddr = wildcard.parse().unwrap();
            let err = Str0mTransport::accept_offer("v=0\r\n", addr).unwrap_err();
            assert!(matches!(err, TransportError::Io(_)), "{err}");
        }
    }

    #[test]
    fn a_malformed_offer_is_refused_before_a_socket_is_kept() {
        let addr: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let err = Str0mTransport::accept_offer("this is not sdp", addr).unwrap_err();
        assert!(matches!(err, TransportError::BadOffer(_)), "{err}");
    }

    /// A conformant peer, built from `str0m` directly.
    ///
    /// Deliberately NOT a second `Str0mTransport`: a test where both ends are the code under test can
    /// pass on a shared misreading of the protocol. Driving a raw `Rtc` as the client means the loop
    /// under test is talking to the library's own state machine, on real loopback sockets, through a
    /// real ICE/DTLS/SCTP handshake.
    ///
    /// It cannot prove BROWSER interop — nothing without a browser can — and that limit is stated
    /// rather than implied.
    struct TestPeer {
        rtc: Rtc,
        socket: UdpSocket,
        channel: Option<ChannelId>,
        received: Vec<Vec<u8>>,
        video_mid: Option<Mid>,
        received_video: Vec<Vec<u8>>,
        connected: bool,
    }

    impl TestPeer {
        fn new() -> (Self, String) {
            Self::new_with_video(false)
        }

        fn new_with_video(video: bool) -> (Self, String) {
            let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
            let local = socket.local_addr().unwrap();
            let mut rtc = Rtc::builder()
                .clear_codecs()
                .enable_av1(true)
                .build(Instant::now());
            rtc.add_local_candidate(Candidate::host(local, "udp").unwrap());
            let mut api = rtc.sdp_api();
            api.add_channel("input".to_string());
            let video_mid = if video {
                // The browser receives video while the same offer opens its input DataChannel.
                Some(api.add_media(MediaKind::Video, Direction::RecvOnly, None, None, None))
            } else {
                None
            };
            let (offer, pending) = api.apply().expect("a channel is a change");
            let sdp = offer.to_sdp_string();
            // The pending offer has to outlive this function, so it is stashed on the struct via a
            // field-less trick: `accept_answer` consumes it, so we keep it in an Option.
            let peer = Self {
                rtc,
                socket,
                channel: None,
                received: Vec::new(),
                video_mid,
                received_video: Vec::new(),
                connected: false,
            };
            PENDING.with(|p| *p.borrow_mut() = Some(pending));
            (peer, sdp)
        }

        fn accept_answer(&mut self, answer_sdp: &str) {
            let answer = SdpAnswer::from_sdp_string(answer_sdp).unwrap();
            let pending = PENDING
                .with(|p| p.borrow_mut().take())
                .expect("offer pending");
            self.rtc.sdp_api().accept_answer(pending, answer).unwrap();
        }

        /// Drive the peer for up to `budget`, returning when `done` says so or the time runs out.
        fn pump(&mut self, budget: Duration, mut done: impl FnMut(&Self) -> bool) -> bool {
            let deadline = Instant::now() + budget;
            let mut buf = vec![0u8; RECV_BUFFER];
            while Instant::now() < deadline {
                if done(self) {
                    return true;
                }
                let timeout = loop {
                    match self.rtc.poll_output().unwrap() {
                        Output::Timeout(t) => break t,
                        Output::Transmit(t) => {
                            let _ = self.socket.send_to(&t.contents, t.destination);
                        }
                        Output::Event(e) => match e {
                            RtcEvent::ChannelOpen(id, _) => {
                                self.channel = Some(id);
                                self.connected = true;
                            }
                            RtcEvent::ChannelData(d) => self.received.push(d.data),
                            RtcEvent::MediaAdded(added) => self.video_mid = Some(added.mid),
                            RtcEvent::MediaData(d) => self.received_video.push(d.data.to_vec()),
                            _ => {}
                        },
                    }
                };
                let wait = timeout
                    .saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(10))
                    .max(Duration::from_millis(1));
                self.socket.set_read_timeout(Some(wait)).unwrap();
                let now = Instant::now();
                match self.socket.recv_from(&mut buf) {
                    Ok((n, source)) => {
                        let dest = self.socket.local_addr().unwrap();
                        if let Ok(r) = Receive::new(Protocol::Udp, source, dest, &buf[..n]) {
                            let _ = self.rtc.handle_input(Input::Receive(now, r));
                        }
                    }
                    Err(_) => {
                        let _ = self.rtc.handle_input(Input::Timeout(now));
                    }
                }
            }
            done(self)
        }

        fn send(&mut self, payload: &[u8]) -> bool {
            match self.channel.and_then(|id| self.rtc.channel(id)) {
                Some(mut ch) => ch.write(true, payload).is_ok(),
                None => false,
            }
        }

        fn request_pli(&mut self) {
            self.rtc
                .writer(self.video_mid.expect("negotiated video mid"))
                .unwrap()
                .request_keyframe(None, KeyframeRequestKind::Pli)
                .unwrap();
        }
    }

    thread_local! {
        static PENDING: std::cell::RefCell<Option<str0m::change::SdpPendingOffer>> =
            const { std::cell::RefCell::new(None) };
    }

    /// 0x12 is a temporal delimiter with its size bit set; 0x00 is its zero length. 0x0a is a
    /// sequence-header OBU with size bit, 0x02 is its length, and 0x01/0x02 are known contents.
    /// 0x32 is a frame OBU with size bit, 0x03 is its length, and A1/B2/C3 are known contents.
    /// A minimal AV1 temporal unit in the low-overhead format (`obu_has_size_field = 1`), which is
    /// what an encoder hands over. The packetizer reads OBU *headers* only, so payloads are markers:
    ///
    /// - `0x12 0x00` — temporal delimiter: header `0b0_0010_0_1_0` (type 2, has_size), size 0;
    /// - `0x0a 0x02 0x01 0x02` — sequence header: type 1, has_size, size 2, two marker bytes;
    /// - `0x32 0x03 0xa1 0xb2 0xc3` — frame OBU: type 6, has_size, size 3, three marker bytes.
    fn av1_test_keyframe() -> Vec<u8> {
        vec![
            0x12, 0x00, 0x0a, 0x02, 0x01, 0x02, 0x32, 0x03, 0xa1, 0xb2, 0xc3,
        ]
    }

    #[test]
    fn the_answer_selects_only_av1_for_video() {
        // SDP negotiation can be checked without a live socket, which also catches an accidental
        // codec fallback before a session starts sending undecodable bytes.
        let mut peer = Rtc::builder()
            .clear_codecs()
            .enable_av1(true)
            .build(Instant::now());
        peer.add_local_candidate(
            Candidate::host("127.0.0.1:40000".parse().unwrap(), "udp").unwrap(),
        );
        let mut api = peer.sdp_api();
        api.add_channel("input".to_string());
        api.add_media(MediaKind::Video, Direction::RecvOnly, None, None, None);
        let (offer, _pending) = api.apply().unwrap();
        let mut server = Rtc::builder()
            .clear_codecs()
            .enable_av1(true)
            .build(Instant::now());
        server.add_local_candidate(
            Candidate::host("127.0.0.1:40001".parse().unwrap(), "udp").unwrap(),
        );
        let answer = server
            .sdp_api()
            .accept_offer(offer)
            .unwrap()
            .to_sdp_string();
        let (answer_mid, pt) = av1_video_answer(&answer).expect("answer must send AV1");
        let mid = Mid::from(answer_mid.as_str());
        // Writer::write validates the negotiated Pt even though media before ICE is dropped.
        assert!(server
            .writer(mid)
            .unwrap()
            .write(
                Pt::from(pt),
                Instant::now(),
                MediaTime::new(90_000, std::num::NonZeroU32::new(90_000).unwrap().into()),
                av1_test_keyframe(),
            )
            .is_ok());
    }

    fn connect_video_peer() -> (TestPeer, Str0mTransport) {
        let (mut peer, offer) = TestPeer::new_with_video(true);
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        assert!(
            av1_video_answer(&answer).is_some(),
            "answer has no AV1 sender"
        );
        peer.accept_answer(&answer);
        let mut connected = false;
        let mut requested = 0;
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline && !(connected && peer.connected && requested == 1) {
            peer.pump(Duration::from_millis(20), |p| p.connected);
            while let Some(event) = transport.poll_event(Duration::ZERO) {
                match event {
                    Event::Connected => connected = true,
                    Event::KeyframeRequested => requested += 1,
                    Event::Failed(why) => panic!("transport failed: {why}"),
                    Event::Disconnected => panic!("transport disconnected during handshake"),
                    Event::Data { .. } => {}
                }
            }
        }
        assert!(connected && peer.connected, "video peer did not connect");
        assert_eq!(requested, 1, "writable track did not ask for one keyframe");
        (peer, transport)
    }

    #[test]
    fn an_av1_keyframe_reaches_the_peer_through_a_negotiated_video_track() {
        let (mut peer, mut transport) = connect_video_peer();
        transport
            .submit(Command::SendVideo {
                frame: av1_test_keyframe(),
                keyframe: true,
                capture_micros: 1_000_000,
            })
            .unwrap();
        assert!(peer.pump(Duration::from_secs(5), |p| !p.received_video.is_empty()));
        // RFC AV1 RTP packetization strips the temporal delimiter; the peer reconstructs both
        // remaining size-bearing OBUs and their exact payload bytes.
        assert_eq!(
            peer.received_video,
            vec![vec![0x0a, 0x02, 0x01, 0x02, 0x32, 0x03, 0xa1, 0xb2, 0xc3]]
        );
        assert_eq!(transport.close().video_frames_out, 1);
    }

    #[test]
    fn the_track_becoming_writable_asks_for_a_keyframe_once() {
        let (mut peer, mut transport) = connect_video_peer();
        peer.pump(Duration::from_millis(200), |_| false);
        let mut extra = 0;
        while let Some(event) = transport.poll_event(Duration::ZERO) {
            if event == Event::KeyframeRequested {
                extra += 1;
            }
        }
        assert_eq!(extra, 0, "writable transition requested more than once");
        transport.close();
    }

    #[test]
    fn a_pli_from_the_peer_becomes_a_keyframe_request_event() {
        let (mut peer, mut transport) = connect_video_peer();
        transport
            .submit(Command::SendVideo {
                frame: av1_test_keyframe(),
                keyframe: true,
                capture_micros: 1_000_000,
            })
            .unwrap();
        assert!(peer.pump(Duration::from_secs(5), |p| !p.received_video.is_empty()));
        peer.request_pli();
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut requested = false;
        while Instant::now() < deadline && !requested {
            peer.pump(Duration::from_millis(20), |_| false);
            if let Some(event) = transport.poll_event(Duration::from_millis(10)) {
                requested = event == Event::KeyframeRequested;
            }
        }
        assert!(requested, "PLI did not reach the transport event seam");
        assert_eq!(transport.close().keyframe_requests_in, 1);
    }

    #[test]
    fn video_before_the_track_is_writable_is_dropped_and_counted() {
        let (mut peer, offer) = TestPeer::new_with_video(true);
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (mut transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        peer.accept_answer(&answer);
        // The peer is deliberately not pumped, so ICE cannot connect before this command.
        transport
            .submit(Command::SendVideo {
                frame: av1_test_keyframe(),
                keyframe: true,
                capture_micros: 1_000_000,
            })
            .unwrap();
        let counters = transport.close();
        assert_eq!(counters.video_frames_dropped_not_ready, 1, "{counters:?}");
        assert_eq!(counters.video_frames_out, 0);
    }

    #[test]
    fn delta_frames_before_the_first_keyframe_are_dropped_and_counted() {
        let (mut peer, mut transport) = connect_video_peer();
        transport
            .submit(Command::SendVideo {
                frame: vec![0x32, 0x03, 0x11, 0x22, 0x33],
                keyframe: false,
                capture_micros: 1_000_000,
            })
            .unwrap();
        peer.pump(Duration::from_millis(200), |_| false);
        let counters = transport.close();
        assert!(peer.received_video.is_empty());
        assert_eq!(
            counters.video_deltas_dropped_before_keyframe, 1,
            "{counters:?}"
        );
        assert_eq!(counters.video_frames_out, 0);
    }

    #[test]
    fn an_offer_without_video_still_carries_the_datachannel_and_counts_video_drops() {
        let (mut peer, offer) = TestPeer::new();
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (mut transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        assert!(av1_video_answer(&answer).is_none());
        peer.accept_answer(&answer);
        assert!(peer.pump(Duration::from_secs(5), |p| p.connected));
        transport
            .submit(Command::SendVideo {
                frame: av1_test_keyframe(),
                keyframe: true,
                capture_micros: 1_000_000,
            })
            .unwrap();
        transport
            .submit(Command::SendData {
                payload: b"down".to_vec(),
            })
            .unwrap();
        assert!(peer.pump(Duration::from_secs(5), |p| !p.received.is_empty()));
        assert_eq!(peer.received, vec![b"down".to_vec()]);
        assert!(peer.send(b"up"));
        let mut inbound = None;
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && inbound.is_none() {
            peer.pump(Duration::from_millis(20), |_| false);
            if let Some(Event::Data { payload }) = transport.poll_event(Duration::from_millis(10)) {
                inbound = Some(payload);
            }
        }
        assert_eq!(inbound, Some(b"up".to_vec()));
        let counters = transport.close();
        assert_eq!(counters.video_frames_dropped_no_track, 1, "{counters:?}");
    }

    #[test]
    fn a_real_webrtc_connection_carries_a_datachannel_message_each_way() {
        // THE PROOF OF THIS BRICK. Real ICE, real DTLS-SRTP, real SCTP, real loopback UDP, no browser
        // and no engine. If the run loop's timeout handling were wrong this would negotiate and then
        // stall, which is precisely the failure that does not announce itself.
        let (mut peer, offer) = TestPeer::new();
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        peer.accept_answer(&answer);

        // The peer has to be pumped from this thread; the transport pumps itself. Interleave: give the
        // peer a slice, then look for the transport's event, until both sides report open.
        let mut server_connected = false;
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline && !(server_connected && peer.connected) {
            peer.pump(Duration::from_millis(50), |p| p.connected);
            if let Some(event) = transport.poll_event(Duration::from_millis(10)) {
                match event {
                    Event::Connected => server_connected = true,
                    Event::Failed(why) => panic!("transport failed: {why}"),
                    Event::Disconnected => panic!("transport disconnected during the handshake"),
                    Event::Data { .. } => {}
                    Event::KeyframeRequested => {}
                }
            }
        }
        assert!(server_connected, "the transport never reported Connected");
        assert!(peer.connected, "the peer never saw its channel open");

        // Browser -> gateway.
        assert!(peer.send(b"up"), "the peer could not write to its channel");
        let mut inbound: Option<Vec<u8>> = None;
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && inbound.is_none() {
            peer.pump(Duration::from_millis(20), |_| false);
            if let Some(Event::Data { payload }) = transport.poll_event(Duration::from_millis(10)) {
                inbound = Some(payload);
            }
        }
        assert_eq!(
            inbound.as_deref(),
            Some(&b"up"[..]),
            "no inbound DataChannel message"
        );

        // Gateway -> browser, which also exercises submit()'s wakeup: the loop is blocked in
        // recv_from when this is called, so without the wakeup datagram this message would wait out
        // the poll ceiling.
        transport
            .submit(Command::SendData {
                payload: b"down".to_vec(),
            })
            .unwrap();
        let got = peer.pump(Duration::from_secs(5), |p| !p.received.is_empty());
        assert!(got, "the peer never received the gateway's message");
        assert_eq!(peer.received[0], b"down".to_vec());

        let mut transport = transport;
        let counters = transport.close();
        assert!(counters.data_messages_in >= 1, "{counters:?}");
        assert!(counters.data_messages_out >= 1, "{counters:?}");
        assert!(
            counters.datagrams_in > 0 && counters.datagrams_out > 0,
            "{counters:?}"
        );
        // The wakeup must have been used, not merely present: if this is zero the 50 ms ceiling is
        // carrying the load and the mechanism is dead code that looks alive.
        assert!(
            counters.wakeups > 0,
            "the wakeup path was never exercised: {counters:?}"
        );
    }

    #[test]
    fn queued_sends_are_bounded_and_the_drops_are_counted() {
        // THE CONTRACT THIS BRICK GOT WRONG FIRST TIME. `submit` refusing when `COMMAND_QUEUE` is full
        // is only backpressure if the loop cannot absorb commands without limit — and the loop's job is
        // to drain them, into `pending`, which was unbounded. So a caller sending before the channel
        // opened could grow the process without ever seeing `Busy`. The bound is the fix and this is
        // the proof: the peer is deliberately NOT pumped, so the channel never opens and everything
        // submitted lands in `pending`.
        let (mut peer, offer) = TestPeer::new();
        let bind: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let (mut transport, answer) = Str0mTransport::accept_offer(&offer, bind).unwrap();
        peer.accept_answer(&answer);

        let sends = PENDING_QUEUE * 3;
        let mut submitted = 0usize;
        for _ in 0..sends {
            match transport.submit(Command::SendData {
                payload: vec![7u8; 64],
            }) {
                Ok(()) => submitted += 1,
                // Also acceptable and also backpressure: the loop was slow enough that the command
                // queue filled. Either path proves the caller cannot grow us without limit.
                Err(TransportError::Busy) => break,
                Err(e) => panic!("unexpected: {e}"),
            }
        }
        // Give the loop a moment to drain what it accepted.
        let _ = transport.poll_event(Duration::from_millis(200));
        let counters = transport.close();
        assert!(
            submitted > PENDING_QUEUE,
            "not enough submitted to reach the bound"
        );
        assert!(
            counters.data_sends_dropped > 0,
            "pending grew without bound — the fix is not in effect: {counters:?}"
        );
        assert!(counters.data_sends_early > 0, "{counters:?}");
    }
}
