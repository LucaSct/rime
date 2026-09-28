// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The hosted front end's **surface policy** — the first brick of Track H (ADR-0045, ADR-0046 §2).
//!
//! `rime.peekstar.eu` serves two surfaces over one hostname: an **edit** session and a **play**
//! session. The decision this module implements is that the surface is a property of *the session
//! the gateway created*, never something the connected client asserts. The alternative — the engine
//! reading a "surface" flag out of a message and deciding for itself — would make "may this peer
//! edit?" a question the engine has to answer about an untrusted peer, a long way from where the
//! answer is actually known.
//!
//! So a play session's editor band (`0x0200..=0x02FF`, `engine/stream/.../protocol.hpp:89-90`) is
//! dropped **here**, in both directions, and the engine keeps its existing behaviour unchanged.
//!
//! **Why this is the first brick and not the third.** It needs no HTTP, no TLS, no WebRTC, no
//! browser and no engine process: it is provable over an in-memory pipe. A gateway that routes
//! beautifully and leaks the editor band into a play session is worse than no gateway, so the
//! property that must not be got wrong is the one that lands first.
//!
//! **Every drop is counted.** A filter whose rejections are invisible reads exactly like a filter
//! that passes everything, which is the failure the engine's own counter rule exists to prevent
//! (CLAUDE.md guardrail 5, applied to the gateway). [`SessionCounters`] is that proof.

use std::io::{Read, Write};

use rime_protocol::{Connection, MessageType, Result};

// Both modules below are Unix-only, and for the same reason at bottom: this is a Linux service on
// starbase (ADR-0047 §3). `supervisor` reaches a session over a Unix-domain socket, which is what the
// engine's LocalListener binds on POSIX; `admission` reads `/dev/urandom` to mint session ids.
//
// Gated rather than stubbed or given a portable fallback. A stub that compiles and cannot work is
// worse than an honest absence, and the fallback nobody runs is the one that is quietly weaker — a
// weaker session id is a weaker capability. CI builds this crate on Windows and macOS, so leaving
// either ungated would pass compilation and fail the test run, which is how `supervisor` first went
// red. The surface policy above is portable and stays so.
#[cfg(unix)]
pub mod admission;
#[cfg(unix)]
pub mod supervisor;

#[cfg(unix)]
pub use admission::{AdmissionCounters, AdmissionPolicy, Refusal, Registry, SessionId};
#[cfg(unix)]
pub use supervisor::{spawn_session, SessionHandle, SessionSpec, SpawnError};

// `http` is portable and stays that way: it is a parser over a `BufRead`, so every CI platform tests
// the bounds checks that are the reason it exists.
pub mod http;

/// Which surface a session was created for. Assigned by the gateway when it admits the session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Surface {
    /// The authoring surface: the editor band is carried.
    Edit,
    /// The play surface: the editor band is refused in both directions.
    Play,
}

impl Surface {
    /// May a message with wire code `code` cross this surface?
    ///
    /// Deliberately decided on the **raw wire code** rather than on a parsed enum. An unknown code
    /// inside the editor band still belongs to the editor band, and `MessageType::from_code` maps
    /// anything it does not recognise to `Other(code)` — so a filter written against named variants
    /// would pass every editor message the Rust side has not learned yet, which is precisely the set
    /// most likely to be new.
    #[must_use]
    pub fn allows(self, code: u16) -> bool {
        match self {
            Surface::Edit => true,
            Surface::Play => !MessageType::is_editor(code),
        }
    }
}

/// What the policy actually did. Read by tests and by the supervisor's logs; a nonzero
/// `editor_blocked` on a play session is normal (a client tried) and on an edit session impossible.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct SessionCounters {
    /// Messages forwarded to the caller.
    pub forwarded: u64,
    /// Messages dropped because the session's surface does not carry them.
    pub editor_blocked: u64,
    /// Sends refused for the same reason — the gateway's own bugs, not the client's.
    pub outbound_blocked: u64,
}

/// One framed session with the surface policy applied.
///
/// Wraps [`Connection`] rather than replacing it: the framing, handshake and encoding stay in
/// `rime-protocol`, which is the crate the C++ conformance fixtures already test.
pub struct Session<S: Read + Write> {
    connection: Connection<S>,
    surface: Surface,
    counters: SessionCounters,
}

impl<S: Read + Write> Session<S> {
    /// Adopt a connected transport for `surface`. The caller has already decided the surface; this
    /// type cannot change it, which is the point — there is no setter.
    pub fn new(stream: S, surface: Surface) -> Self {
        Self {
            connection: Connection::new(stream),
            surface,
            counters: SessionCounters::default(),
        }
    }

    /// Exchange the protocol version header. Delegates to `rime-protocol`.
    pub fn handshake(&mut self) -> Result<()> {
        self.connection.handshake()
    }

    #[must_use]
    pub fn surface(&self) -> Surface {
        self.surface
    }

    #[must_use]
    pub fn counters(&self) -> SessionCounters {
        self.counters
    }

    /// Receive the next message this surface carries, dropping and counting any that it does not.
    ///
    /// Returns `Ok(None)` only when the transport is exhausted; a blocked message is not the end of
    /// the stream, so the loop continues past it rather than reporting a close.
    pub fn recv(&mut self) -> Result<Option<(MessageType, Vec<u8>)>> {
        loop {
            let (ty, payload) = match self.connection.recv() {
                Ok(message) => message,
                // ONLY `UnexpectedEof` is a close. `Connection::recv` documents a clean peer close
                // as exactly that, and matching the whole `Io` variant instead would report a real
                // I/O failure — a reset connection, a bad fd — as a tidy end of stream, which is the
                // shape of bug that makes a broken session look like a finished one.
                Err(rime_protocol::Error::Io(e))
                    if e.kind() == std::io::ErrorKind::UnexpectedEof =>
                {
                    return Ok(None)
                }
                Err(other) => return Err(other),
            };
            if self.surface.allows(ty.to_code()) {
                self.counters.forwarded += 1;
                return Ok(Some((ty, payload)));
            }
            self.counters.editor_blocked += 1;
        }
    }

    /// Send a message, refusing one this surface does not carry.
    ///
    /// A refusal here is the gateway catching *itself*: nothing a client sent reaches this path, so
    /// a nonzero `outbound_blocked` means gateway code tried to write the editor band into a play
    /// session. It is counted rather than silently dropped for exactly that reason.
    pub fn send(&mut self, ty: MessageType, payload: &[u8]) -> Result<bool> {
        if !self.surface.allows(ty.to_code()) {
            self.counters.outbound_blocked += 1;
            return Ok(false);
        }
        self.connection.send(ty, payload)?;
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A transport whose reads come from a fixed buffer and whose writes are kept. Enough to drive a
    /// `Session` with no socket, no engine and no network — which is what makes this brick provable
    /// before any of those exist.
    struct Pipe {
        inbound: std::io::Cursor<Vec<u8>>,
        outbound: Vec<u8>,
    }

    impl Pipe {
        fn new(inbound: Vec<u8>) -> Self {
            Self {
                inbound: std::io::Cursor::new(inbound),
                outbound: Vec::new(),
            }
        }
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

    /// One framed message on the wire: `[type:u16][length:u32][payload]`, little-endian, the layout
    /// `Connection::send` writes. Built by hand here so the test does not prove the encoder against
    /// itself.
    fn framed(code: u16, payload: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&code.to_le_bytes());
        bytes.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        bytes.extend_from_slice(payload);
        bytes
    }

    #[test]
    fn the_editor_band_is_exactly_0x0200_through_0x02ff() {
        // The boundary, both sides of it. 0x01FF and 0x0300 are NOT the editor band, and a filter
        // that treated the range as half-open would leak 0x02FF — the code most likely to be a
        // future editor message.
        assert!(Surface::Play.allows(0x01FF));
        assert!(!Surface::Play.allows(0x0200));
        assert!(!Surface::Play.allows(0x0280));
        assert!(!Surface::Play.allows(0x02FF));
        assert!(Surface::Play.allows(0x0300));
        // An edit session carries all of it, including codes this crate has never heard of.
        assert!(Surface::Edit.allows(0x0200));
        assert!(Surface::Edit.allows(0x02FF));
        assert!(Surface::Edit.allows(0xABCD));
    }

    #[test]
    fn a_play_session_drops_the_editor_band_and_counts_it() {
        // Frame, editor message, frame. The editor message must vanish and the two frames must
        // arrive — a filter that closed the stream on a blocked message would pass a weaker test.
        let wire = [
            framed(MessageType::Frame.to_code(), b"one"),
            framed(0x0201, b"SaveScene"),
            framed(MessageType::Frame.to_code(), b"two"),
        ]
        .concat();
        let mut session = Session::new(Pipe::new(wire), Surface::Play);

        let (ty, payload) = session.recv().unwrap().unwrap();
        assert_eq!(ty, MessageType::Frame);
        assert_eq!(payload, b"one");
        let (ty, payload) = session.recv().unwrap().unwrap();
        assert_eq!(ty, MessageType::Frame);
        assert_eq!(payload, b"two");
        assert!(session.recv().unwrap().is_none());

        let counters = session.counters();
        assert_eq!(counters.forwarded, 2);
        assert_eq!(counters.editor_blocked, 1, "the drop must be visible");
    }

    #[test]
    fn an_edit_session_carries_the_editor_band() {
        let wire = framed(0x0201, b"SaveScene");
        let mut session = Session::new(Pipe::new(wire), Surface::Edit);
        let (ty, payload) = session.recv().unwrap().unwrap();
        assert_eq!(ty.to_code(), 0x0201);
        assert_eq!(payload, b"SaveScene");
        assert_eq!(session.counters().editor_blocked, 0);
    }

    #[test]
    fn a_play_session_refuses_to_send_the_editor_band() {
        let mut session = Session::new(Pipe::new(Vec::new()), Surface::Play);
        assert!(!session
            .send(MessageType::Other(0x0201), b"SaveScene")
            .unwrap());
        assert_eq!(session.counters().outbound_blocked, 1);
        // Nothing reached the wire. A refusal that still wrote the bytes would be the worst of both.
        assert!(session.connection.get_ref().outbound.is_empty());
        // A carried message still goes out, so the refusal is not just "sending is broken".
        assert!(session.send(MessageType::Input, b"x").unwrap());
        assert!(!session.connection.get_ref().outbound.is_empty());
    }

    #[test]
    fn a_blocked_message_does_not_desynchronise_the_frame_reader() {
        // The subtle failure: dropping a message by skipping its HEADER but not its PAYLOAD leaves
        // the reader mid-message, and every subsequent parse is garbage that still looks like data.
        // A long editor payload followed by a frame is what catches it.
        let big = vec![0xAAu8; 4096];
        let wire = [
            framed(0x02FF, &big),
            framed(MessageType::Frame.to_code(), b"after"),
        ]
        .concat();
        let mut session = Session::new(Pipe::new(wire), Surface::Play);
        let (ty, payload) = session.recv().unwrap().unwrap();
        assert_eq!(ty, MessageType::Frame);
        assert_eq!(payload, b"after");
        assert_eq!(session.counters().editor_blocked, 1);
    }
}
