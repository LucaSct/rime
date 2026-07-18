// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! # rime-protocol
//!
//! A hand-rolled Rust implementation of the Rime streaming/editor **wire protocol** — the exact
//! bytes `engine/stream` speaks (Track S / S0.4, versioned from day one; ADR-0016). The editor is a
//! *client of a live engine process*: it launches `rime-engine --editor-host`, connects over the
//! s1.4 local socket, and trades reflection-described component data on the editor channel and
//! (later) a streamed viewport. This crate is that client's tongue.
//!
//! It mirrors the C++ reference **field-by-field** — same little-endian layout, same message type
//! codes, same handshake — rather than sharing a schema, because the two toolchains have no common
//! serialization runtime. What keeps the two honest is a **cross-language conformance test**
//! (`tests/conformance.rs`): the C++ side emits golden byte vectors for each message, and this crate
//! must decode them and re-encode the identical bytes. Drift shows up as a failed test, not a
//! mysterious runtime bug.
//!
//! ## Scope (v1 — the editor channel)
//!
//! Handshake, the length-prefixed message envelope, [`InputEvent`], the editor messages
//! ([`editor`]), and a transport-generic [`Connection`]. [`FrameMessage`] parses its header and
//! hands back the still-encoded pixel bytes; decoding an LZ4 viewport frame to RGBA lands with the
//! viewport panel (it pulls an external lz4 crate — kept out of the dependency-free editor channel).

mod wire;

pub mod connection;
pub mod editor;
pub mod frame;
pub mod input;

pub use connection::Connection;
pub use editor::{
    decode_value, encode_despawn, encode_value, AssetEntry, AssetKind, AssetList, ComponentRef,
    EditorMessage, FieldDesc, FieldKind, Schema, SchemaEntry, SetComponent, Snapshot,
    SnapshotComponent, SnapshotEntity, SpawnEntity, Value,
};
pub use frame::{Codec, FrameMessage, ImageDesc, PixelFormat};
pub use input::{InputEvent, InputKind};

/// Wire identity: ASCII `"RMS1"` (Rime Media Stream), the first four bytes of every connection so a
/// wrong-protocol peer is rejected at the handshake. Mirrors `stream::kProtocolMagic`.
pub const PROTOCOL_MAGIC: u32 = 0x524D_5331;

/// The protocol version exchanged in the handshake. Bumped for any incompatible wire change; the
/// editor channel rides version 3 (s1.3's latency ledger). Mirrors `stream::kProtocolVersion`.
pub const PROTOCOL_VERSION: u16 = 3;

/// Upper bound on one message's payload, so a corrupt/hostile length can't drive a huge allocation.
/// Mirrors `stream::kMaxMessageBytes` (64 MiB).
pub const MAX_MESSAGE_BYTES: u32 = 64 * 1024 * 1024;

/// A framed message's type code — the `u16` at the head of every envelope. These are wire constants
/// (mirroring `stream::MessageType`): append, never renumber. The editor channel lives in the
/// reserved `0x0200..=0x02FF` band (see [`editor::EditorMessage`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageType {
    /// server → client: one encoded video frame ([`FrameMessage`]).
    Frame,
    /// server → client: stream parameters (codec/geometry), before the first frame.
    StreamConfig,
    /// client → server: one input event ([`InputEvent`]).
    Input,
    /// client → server: the client's decoder inventory (codec negotiation).
    Capabilities,
    /// client → server: "start me a fresh delta chain" (no payload).
    KeyframeRequest,
    /// either direction: graceful close (no payload).
    Bye,
    /// Anything else — an editor-channel message (`0x0200..=0x02FF`) or a code this build predates.
    /// The envelope carries the raw `u16` transparently, exactly as the C++ `recv_message` does, so
    /// a dispatcher tests membership itself rather than the transport rejecting an unknown type.
    Other(u16),
}

impl MessageType {
    /// The `u16` wire code for this type.
    pub fn to_code(self) -> u16 {
        match self {
            MessageType::Frame => 0x0001,
            MessageType::StreamConfig => 0x0002,
            MessageType::Input => 0x0101,
            MessageType::Capabilities => 0x0102,
            MessageType::KeyframeRequest => 0x0103,
            MessageType::Bye => 0xFFFF,
            MessageType::Other(code) => code,
        }
    }

    /// The type for a wire code. Unrecognized codes become [`MessageType::Other`] — never an error,
    /// because forward-compatibility is the reservation's whole point.
    pub fn from_code(code: u16) -> Self {
        match code {
            0x0001 => MessageType::Frame,
            0x0002 => MessageType::StreamConfig,
            0x0101 => MessageType::Input,
            0x0102 => MessageType::Capabilities,
            0x0103 => MessageType::KeyframeRequest,
            0xFFFF => MessageType::Bye,
            other => MessageType::Other(other),
        }
    }

    /// True if `code` is in the editor's reserved band `0x0200..=0x02FF`.
    pub fn is_editor(code: u16) -> bool {
        (0x0200..=0x02FF).contains(&code)
    }
}

/// Everything that can go wrong decoding wire bytes or driving a connection. Cheap to match on; the
/// I/O variant carries the OS error's text so a caller can log it.
#[derive(Debug)]
pub enum Error {
    /// A read ran past the end of the buffer (a truncated or malformed message).
    Truncated,
    /// The handshake magic did not match [`PROTOCOL_MAGIC`] — a wrong-protocol peer.
    BadMagic(u32),
    /// The handshake version did not match [`PROTOCOL_VERSION`] — an incompatible peer.
    BadVersion(u16),
    /// A frame carried a codec byte this build does not know.
    BadCodec(u8),
    /// A frame carried a pixel-format byte this build does not know.
    BadFormat(u8),
    /// A length field exceeded [`MAX_MESSAGE_BYTES`].
    TooLarge(u32),
    /// A blob's declared magic tag did not match (a wrong-shaped schema/snapshot).
    BadTag(u32),
    /// An underlying I/O error while reading/writing a connection.
    Io(std::io::Error),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Truncated => write!(f, "truncated message"),
            Error::BadMagic(m) => write!(f, "bad protocol magic {m:#010x}"),
            Error::BadVersion(v) => write!(f, "unsupported protocol version {v}"),
            Error::BadCodec(c) => write!(f, "unknown codec byte {c}"),
            Error::BadFormat(c) => write!(f, "unknown pixel-format byte {c}"),
            Error::TooLarge(n) => write!(f, "message length {n} exceeds the cap"),
            Error::BadTag(t) => write!(f, "bad blob tag {t:#010x}"),
            Error::Io(e) => write!(f, "io error: {e}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

/// The crate's result alias.
pub type Result<T> = std::result::Result<T, Error>;
