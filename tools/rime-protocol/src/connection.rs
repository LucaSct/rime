// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! A framed message stream over any byte transport — the Rust twin of `stream::ProtocolConnection`.
//! Generic over `Read + Write`, so the editor drives it over a `UnixStream` (the s1.4 local fast
//! path) and a test drives it over an in-memory pipe. Both ends call [`Connection::handshake`] once,
//! then trade [`crate::MessageType`]-tagged messages.

use std::io::{Read, Write};

use crate::editor::EditorMessage;
use crate::input::InputEvent;
use crate::wire::{Reader, Writer};
use crate::{Error, MessageType, Result, MAX_MESSAGE_BYTES, PROTOCOL_MAGIC, PROTOCOL_VERSION};

/// One framed connection. Owns the transport. Not `Sync` for concurrent senders/receivers, but —
/// like the C++ side and TCP itself — one sender and one receiver may use a clone/split of the
/// transport at once; keep the split to the transport, not this wrapper.
pub struct Connection<S: Read + Write> {
    stream: S,
}

impl<S: Read + Write> Connection<S> {
    /// Wrap a connected transport (post-connect / post-accept). Call [`Connection::handshake`] next.
    pub fn new(stream: S) -> Self {
        Self { stream }
    }

    /// Exchange and validate the 6-byte version header: send our hello `[magic:u32][version:u16]`,
    /// read the peer's, and check both. A wrong-protocol or wrong-version peer is refused here, not
    /// misparsed later.
    pub fn handshake(&mut self) -> Result<()> {
        let mut w = Writer::new();
        w.u32(PROTOCOL_MAGIC);
        w.u16(PROTOCOL_VERSION);
        self.stream.write_all(&w.into_vec())?;
        self.stream.flush()?;

        let mut hello = [0u8; 6];
        self.stream.read_exact(&mut hello)?;
        let mut r = Reader::new(&hello);
        let magic = r.u32()?;
        if magic != PROTOCOL_MAGIC {
            return Err(Error::BadMagic(magic));
        }
        let version = r.u16()?;
        if version != PROTOCOL_VERSION {
            return Err(Error::BadVersion(version));
        }
        Ok(())
    }

    /// Send one framed message: `[type:u16][length:u32][payload]`. `payload` may be empty (`Bye`).
    pub fn send(&mut self, ty: MessageType, payload: &[u8]) -> Result<()> {
        if payload.len() > MAX_MESSAGE_BYTES as usize {
            return Err(Error::TooLarge(
                u32::try_from(payload.len()).unwrap_or(u32::MAX),
            ));
        }
        let mut w = Writer::new();
        w.u16(ty.to_code());
        w.u32(payload.len() as u32);
        w.bytes(payload);
        self.stream.write_all(&w.into_vec())?;
        self.stream.flush()?;
        Ok(())
    }

    /// Send an editor-channel message (its `0x02xx` code + payload).
    pub fn send_editor(&mut self, msg: EditorMessage, payload: &[u8]) -> Result<()> {
        self.send(MessageType::Other(msg.to_code()), payload)
    }

    /// Send an input event back to the engine.
    pub fn send_input(&mut self, event: &InputEvent) -> Result<()> {
        self.send(MessageType::Input, &event.encode())
    }

    /// Send a graceful `Bye` (no payload).
    pub fn send_bye(&mut self) -> Result<()> {
        self.send(MessageType::Bye, &[])
    }

    /// Receive exactly one framed message: its type and payload bytes. A clean peer close before a
    /// full header/payload surfaces as an [`Error::Io`] of kind `UnexpectedEof`.
    pub fn recv(&mut self) -> Result<(MessageType, Vec<u8>)> {
        let mut header = [0u8; 6];
        self.stream.read_exact(&mut header)?;
        let mut r = Reader::new(&header);
        let ty = MessageType::from_code(r.u16()?);
        let len = r.u32()?;
        if len > MAX_MESSAGE_BYTES {
            return Err(Error::TooLarge(len));
        }
        let mut payload = vec![0u8; len as usize];
        self.stream.read_exact(&mut payload)?;
        Ok((ty, payload))
    }

    /// Borrow the underlying transport (e.g. to set a read timeout on a socket).
    pub fn get_ref(&self) -> &S {
        &self.stream
    }

    /// Recover the underlying transport.
    pub fn into_inner(self) -> S {
        self.stream
    }
}
