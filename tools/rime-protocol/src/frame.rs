// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The `Frame` message — one encoded video frame, server → client. This crate parses the header and
//! hands back the still-encoded pixel bytes in [`FrameMessage::data`]; turning those bytes into RGBA
//! (LZ4 for the editor viewport) lands with the viewport panel, which pulls an lz4 crate the
//! editor channel deliberately does without. Mirrors `stream::FrameMessage`:
//!
//! ```text
//! [seq:u64][capture_us:u64][readback_us:u64][encode_us:u64][wire_us:u64]
//! [last_input_seq:u32][last_input_client_us:u64][codec:u8][fmt:u8][w:u32][h:u32][data...]
//! ```

use crate::wire::{Reader, Writer};
use crate::{Error, Result};

/// Which codec produced (and must decode) a frame's bytes — one wire byte. Mirrors `stream::Codec`.
/// The editor viewport uses [`Codec::Lz4`] (lossless, local).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Codec {
    Raw,
    Lz4,
    Jpeg,
    Av1,
}

impl Codec {
    fn to_code(self) -> u8 {
        match self {
            Codec::Raw => 0,
            Codec::Lz4 => 1,
            Codec::Jpeg => 2,
            Codec::Av1 => 3,
        }
    }

    fn from_code(code: u8) -> Result<Self> {
        match code {
            0 => Ok(Codec::Raw),
            1 => Ok(Codec::Lz4),
            2 => Ok(Codec::Jpeg),
            3 => Ok(Codec::Av1),
            other => Err(Error::BadCodec(other)),
        }
    }
}

/// The pixel format of a frame — the four 8-bit, 4-channel colour formats the codecs speak. These
/// are the protocol's own stable `WireFormat` codes (independent of the engine's internal
/// `rhi::Format` numbering), mirroring `stream`'s `wire_format_of`/`rhi_format_of`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
}

impl PixelFormat {
    fn to_code(self) -> u8 {
        match self {
            PixelFormat::Rgba8Unorm => 0,
            PixelFormat::Rgba8Srgb => 1,
            PixelFormat::Bgra8Unorm => 2,
            PixelFormat::Bgra8Srgb => 3,
        }
    }

    fn from_code(code: u8) -> Result<Self> {
        match code {
            0 => Ok(PixelFormat::Rgba8Unorm),
            1 => Ok(PixelFormat::Rgba8Srgb),
            2 => Ok(PixelFormat::Bgra8Unorm),
            3 => Ok(PixelFormat::Bgra8Srgb),
            other => Err(Error::BadFormat(other)),
        }
    }
}

/// A frame's geometry + pixel format. `byte_size` is the raw (decoded) size — every supported format
/// is 4 bytes/pixel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ImageDesc {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
}

impl ImageDesc {
    /// The decoded frame's size in bytes (`width * height * 4`).
    pub fn byte_size(&self) -> usize {
        self.width as usize * self.height as usize * 4
    }
}

/// One encoded frame plus the server half of the s1.3 latency ledger (all `*_us` are the server's
/// monotonic clock; their differences give server-side stage costs with no clock sync). `data` is
/// the codec's still-encoded output.
#[derive(Debug, Clone, PartialEq)]
pub struct FrameMessage {
    pub sequence: u64,
    pub capture_us: u64,
    pub readback_us: u64,
    pub encode_us: u64,
    pub wire_us: u64,
    pub last_input_seq: u32,
    pub last_input_client_us: u64,
    pub codec: Codec,
    pub desc: ImageDesc,
    pub data: Vec<u8>,
}

impl FrameMessage {
    /// Serialize the full payload (header + encoded data).
    pub fn encode(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.u64(self.sequence);
        w.u64(self.capture_us);
        w.u64(self.readback_us);
        w.u64(self.encode_us);
        w.u64(self.wire_us);
        w.u32(self.last_input_seq);
        w.u64(self.last_input_client_us);
        w.u8(self.codec.to_code());
        w.u8(self.desc.format.to_code());
        w.u32(self.desc.width);
        w.u32(self.desc.height);
        w.bytes(&self.data);
        w.into_vec()
    }

    /// Parse a payload produced by [`FrameMessage::encode`] (or the C++ `FrameMessage::encode`). The
    /// tail after the fixed header is the still-encoded pixel `data`.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut r = Reader::new(payload);
        let sequence = r.u64()?;
        let capture_us = r.u64()?;
        let readback_us = r.u64()?;
        let encode_us = r.u64()?;
        let wire_us = r.u64()?;
        let last_input_seq = r.u32()?;
        let last_input_client_us = r.u64()?;
        let codec = Codec::from_code(r.u8()?)?;
        let format = PixelFormat::from_code(r.u8()?)?;
        let width = r.u32()?;
        let height = r.u32()?;
        let data = r.rest().to_vec();
        Ok(FrameMessage {
            sequence,
            capture_us,
            readback_us,
            encode_us,
            wire_us,
            last_input_seq,
            last_input_client_us,
            codec,
            desc: ImageDesc {
                width,
                height,
                format,
            },
            data,
        })
    }
}
