// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A matched pair of little-endian byte cursors: ByteWriter appends fields to a growing buffer,
// ByteReader consumes them back with a bounds check on every read. The whole point is that we
// serialize *field by field* — never memcpy a struct — so the bytes are identical on every compiler
// and CPU regardless of struct padding or host byte order. That portability is what a persisted or
// transmitted format needs, and the bounds-checked reader is what lets us treat foreign bytes (a
// cooked asset off disk, a message off the wire) as untrusted: a truncated or hostile buffer
// becomes a clean `false` return, never an out-of-bounds read.
//
// This is the "trust nothing you read" discipline first written for the S0.4 stream protocol,
// promoted here to core so the asset loader (ADR-0024) and the editor's scene format (M9) share one
// tested implementation instead of each re-deriving it. (engine/stream still carries its own
// private copy from before this existed; it can adopt this later — a mechanical change, deferred to
// avoid churning a Track-S module inside an asset brick.)
namespace rime::core {

// Append little-endian integers/floats to a byte buffer. Every multi-byte value is decomposed into
// bytes explicitly, so the result never depends on the host's endianness.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) noexcept : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFFu));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    }

    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v & 0xFFFFu));
        u16(static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
        u32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    }

    // Floats go through bit_cast to their IEEE-754 bit pattern, then out as the matching-width
    // integer — so a float's bytes are as portable as an integer's (we target IEEE-754 hosts).
    void i32(std::int32_t v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }

    // Append raw bytes verbatim (a vertex blob, an index run, a magic tag).
    void bytes(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }

private:
    std::vector<std::byte>& out_;
};

// Read little-endian integers/floats from a byte span, bounds-checked. Every reader returns false
// on underflow and advances the cursor by nothing on failure, so a caller can chain reads with `&&`
// and treat any false as "truncated/corrupt" without risking a read past the end.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> in) noexcept : in_(in) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

    [[nodiscard]] bool u8(std::uint8_t& v) noexcept {
        if (remaining() < 1) {
            return false;
        }
        v = std::to_integer<std::uint8_t>(in_[pos_++]);
        return true;
    }

    // The multi-byte readers check the *full* width up front, so a read that would run off the end
    // fails having advanced nothing — never leaving the cursor stranded mid-value. (The composed
    // sub-reads below therefore cannot fail; the guard is what makes "&&-chain then check once"
    // safe.)
    [[nodiscard]] bool u16(std::uint16_t& v) noexcept {
        if (remaining() < 2) {
            return false;
        }
        std::uint8_t a = 0, b = 0;
        (void)u8(a);
        (void)u8(b);
        v = static_cast<std::uint16_t>(static_cast<unsigned>(a) | (static_cast<unsigned>(b) << 8));
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& v) noexcept {
        if (remaining() < 4) {
            return false;
        }
        std::uint16_t lo = 0, hi = 0;
        (void)u16(lo);
        (void)u16(hi);
        v = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& v) noexcept {
        if (remaining() < 8) {
            return false;
        }
        std::uint32_t lo = 0, hi = 0;
        (void)u32(lo);
        (void)u32(hi);
        v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& v) noexcept {
        std::uint32_t u = 0;
        if (!u32(u)) {
            return false;
        }
        v = std::bit_cast<std::int32_t>(u);
        return true;
    }

    [[nodiscard]] bool f32(float& v) noexcept {
        std::uint32_t u = 0;
        if (!u32(u)) {
            return false;
        }
        v = std::bit_cast<float>(u);
        return true;
    }

    [[nodiscard]] bool f64(double& v) noexcept {
        std::uint64_t u = 0;
        if (!u64(u)) {
            return false;
        }
        v = std::bit_cast<double>(u);
        return true;
    }

    // Read exactly `count` raw bytes into `dst` (as a fresh view into the source span). Fails
    // without advancing if fewer than `count` remain — so an attacker-controlled length can never
    // make us read (or, at a higher layer, allocate) past what the buffer actually holds.
    [[nodiscard]] bool bytes(std::span<const std::byte>& dst, std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        dst = in_.subspan(pos_, count);
        pos_ += count;
        return true;
    }

    // Skip `count` bytes; fails (advancing nothing) if fewer remain.
    [[nodiscard]] bool skip(std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        pos_ += count;
        return true;
    }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

} // namespace rime::core
