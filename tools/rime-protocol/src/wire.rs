// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Little-endian byte cursors — the Rust twins of C++'s `core::ByteWriter`/`ByteReader`
//! (engine/core/byte_cursor.hpp). Every multi-byte value is written/read explicitly in
//! little-endian order, so the bytes are identical to the C++ side on every platform regardless of
//! host endianness. The reader is bounds-checked: a short buffer is a clean `Err(Truncated)`, never
//! a panic or an out-of-bounds read — the "trust nothing you read" discipline the whole wire relies
//! on.

use crate::Error;

/// Appends little-endian integers/floats to a growing buffer.
#[derive(Default)]
pub(crate) struct Writer {
    buf: Vec<u8>,
}

impl Writer {
    pub(crate) fn new() -> Self {
        Self { buf: Vec::new() }
    }

    pub(crate) fn u8(&mut self, v: u8) {
        self.buf.push(v);
    }

    pub(crate) fn u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub(crate) fn u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub(crate) fn u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    /// A signed 32-bit value goes out as its two's-complement bit pattern — the same bytes as C++'s
    /// `bit_cast<uint32_t>` path, so `-1` is `ff ff ff ff` on both sides.
    pub(crate) fn i32(&mut self, v: i32) {
        self.u32(v as u32);
    }

    /// A float goes out as its IEEE-754 bit pattern (matching C++'s `bit_cast<uint32_t>(float)`).
    pub(crate) fn f32(&mut self, v: f32) {
        self.u32(v.to_bits());
    }

    pub(crate) fn bytes(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
    }

    pub(crate) fn into_vec(self) -> Vec<u8> {
        self.buf
    }
}

/// Reads little-endian integers/floats from a byte slice, bounds-checked. Every read that would run
/// past the end returns `Err(Truncated)` having advanced nothing.
pub(crate) struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    pub(crate) fn new(buf: &'a [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    pub(crate) fn remaining(&self) -> usize {
        self.buf.len() - self.pos
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        if self.remaining() < n {
            return Err(Error::Truncated);
        }
        let slice = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Ok(slice)
    }

    pub(crate) fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }

    pub(crate) fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }

    pub(crate) fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    pub(crate) fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }

    pub(crate) fn i32(&mut self) -> Result<i32, Error> {
        Ok(self.u32()? as i32)
    }

    pub(crate) fn f32(&mut self) -> Result<f32, Error> {
        Ok(f32::from_bits(self.u32()?))
    }

    /// Read exactly `n` raw bytes (a name, a component blob, a frame's pixels).
    pub(crate) fn take_bytes(&mut self, n: usize) -> Result<&'a [u8], Error> {
        self.take(n)
    }

    /// The not-yet-read tail (a message's variable-length payload).
    pub(crate) fn rest(&mut self) -> &'a [u8] {
        let tail = &self.buf[self.pos..];
        self.pos = self.buf.len();
        tail
    }
}
