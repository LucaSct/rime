// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! A conformant WebRTC peer for tests, and the AV1 temporal unit they send through it.
//!
//! Shared by [`crate::transport`] (which proves the run loop against it) and [`crate::relay`] (which
//! proves the whole engine-to-browser path against it). It lives in its own module rather than inside
//! one of their test modules for a reason worth stating: a second copy of a test peer is a test peer
//! that drifts, and then two bricks are proved against two slightly different ideas of the protocol.
//!
//! Nothing here is compiled into a release build — the module is `#[cfg(test)]` at its declaration in
//! `lib.rs`.

use std::net::UdpSocket;
use std::time::{Duration, Instant};

use str0m::change::SdpAnswer;
use str0m::channel::ChannelId;
use str0m::media::{Direction, KeyframeRequestKind, MediaKind, Mid};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event as RtcEvent, Input, Output, Rtc};

/// The largest datagram the peer will read. Mirrors the transport's own buffer.
const RECV_BUFFER: usize = 2048;

/// A conformant peer, built from `str0m` directly.
///
/// Deliberately NOT a second `Str0mTransport`: a test where both ends are the code under test can
/// pass on a shared misreading of the protocol. Driving a raw `Rtc` as the client means the loop
/// under test is talking to the library's own state machine, on real loopback sockets, through a
/// real ICE/DTLS/SCTP handshake.
///
/// It cannot prove BROWSER interop — nothing without a browser can — and that limit is stated
/// rather than implied.
pub(crate) struct TestPeer {
    pub(crate) rtc: Rtc,
    socket: UdpSocket,
    pub(crate) channel: Option<ChannelId>,
    pub(crate) received: Vec<Vec<u8>>,
    pub(crate) video_mid: Option<Mid>,
    pub(crate) received_video: Vec<Vec<u8>>,
    pub(crate) connected: bool,
}

impl TestPeer {
    pub(crate) fn new() -> (Self, String) {
        Self::new_with_video(false)
    }

    pub(crate) fn new_with_video(video: bool) -> (Self, String) {
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

    pub(crate) fn accept_answer(&mut self, answer_sdp: &str) {
        let answer = SdpAnswer::from_sdp_string(answer_sdp).unwrap();
        let pending = PENDING
            .with(|p| p.borrow_mut().take())
            .expect("offer pending");
        self.rtc.sdp_api().accept_answer(pending, answer).unwrap();
    }

    /// Drive the peer for up to `budget`, returning when `done` says so or the time runs out.
    pub(crate) fn pump(&mut self, budget: Duration, mut done: impl FnMut(&Self) -> bool) -> bool {
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

    pub(crate) fn send(&mut self, payload: &[u8]) -> bool {
        match self.channel.and_then(|id| self.rtc.channel(id)) {
            Some(mut ch) => ch.write(true, payload).is_ok(),
            None => false,
        }
    }

    pub(crate) fn request_pli(&mut self) {
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
pub(crate) fn av1_test_keyframe() -> Vec<u8> {
    vec![
        0x12, 0x00, 0x0a, 0x02, 0x01, 0x02, 0x32, 0x03, 0xa1, 0xb2, 0xc3,
    ]
}
