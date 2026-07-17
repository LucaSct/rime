// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! One input event on its way back to the engine — the `Input` message's payload. A single tagged
//! struct (rather than one per event kind) keeps both the wire and the server's injection loop
//! simple; fields irrelevant to a `kind` are zero. Fixed 37-byte layout, mirroring
//! `stream::InputEvent`:
//!
//! ```text
//! [kind:u8][code:u32][x:i32][y:i32][scroll_x:f32][scroll_y:f32][mods:u32][client_us:u64][seq:u32]
//! ```

use crate::wire::{Reader, Writer};
use crate::{Error, Result};

/// The kind of input event. Wire values 0..=5 (append, never renumber) — mirrors
/// `stream::InputEvent::Kind`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputKind {
    KeyDown,
    KeyUp,
    PointerMove,
    PointerDown,
    PointerUp,
    PointerScroll,
}

impl InputKind {
    fn to_code(self) -> u8 {
        match self {
            InputKind::KeyDown => 0,
            InputKind::KeyUp => 1,
            InputKind::PointerMove => 2,
            InputKind::PointerDown => 3,
            InputKind::PointerUp => 4,
            InputKind::PointerScroll => 5,
        }
    }

    fn from_code(code: u8) -> Result<Self> {
        match code {
            0 => Ok(InputKind::KeyDown),
            1 => Ok(InputKind::KeyUp),
            2 => Ok(InputKind::PointerMove),
            3 => Ok(InputKind::PointerDown),
            4 => Ok(InputKind::PointerUp),
            5 => Ok(InputKind::PointerScroll),
            // An unknown kind is a malformed event; treat it as truncation-class corruption.
            _ => Err(Error::Truncated),
        }
    }
}

/// One input event. `code` is a key code (Key*) or a button index (Pointer{Down,Up}); `x`/`y` are
/// pointer pixels; `scroll_*` are wheel deltas; `mods` is a client-defined modifier bitmask. The
/// s1.3 latency ledger adds `client_us` (the client-clock send time) and `seq` (a per-client
/// sequence number) — the server echoes them on the frame that first reflects this input, closing an
/// offset-free input-to-photon measurement. Leaving them 0 reads as "un-timed".
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct InputEvent {
    pub kind: InputKind,
    pub code: u32,
    pub x: i32,
    pub y: i32,
    pub scroll_x: f32,
    pub scroll_y: f32,
    pub mods: u32,
    pub client_us: u64,
    pub seq: u32,
}

impl InputEvent {
    /// Serialize this event's payload (37 bytes).
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u8(self.kind.to_code());
        w.u32(self.code);
        w.i32(self.x);
        w.i32(self.y);
        w.f32(self.scroll_x);
        w.f32(self.scroll_y);
        w.u32(self.mods);
        w.u64(self.client_us);
        w.u32(self.seq);
        w.into_vec()
    }

    /// Parse a payload produced by [`InputEvent::encode`] (or the C++ `InputEvent::encode`).
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        Ok(InputEvent {
            kind: InputKind::from_code(r.u8()?)?,
            code: r.u32()?,
            x: r.i32()?,
            y: r.i32()?,
            scroll_x: r.f32()?,
            scroll_y: r.f32()?,
            mods: r.u32()?,
            client_us: r.u64()?,
            seq: r.u32()?,
        })
    }
}
