// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the core little-endian byte cursors (M6.1): every scalar written by ByteWriter reads
// back identically through ByteReader, the wire bytes are little-endian regardless of host, and the
// reader bounds-checks — a short buffer fails cleanly and advances the cursor by nothing, which is
// the property that lets cooked-asset and protocol readers treat untrusted bytes safely.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/byte_cursor.hpp"

using namespace rime::core;

TEST_CASE("ByteWriter/ByteReader round-trip every scalar type and a raw tail") {
    std::vector<std::byte> buf;
    ByteWriter w(buf);
    w.u8(0x12);
    w.u16(0x1234);
    w.u32(0x89ABCDEFu);
    w.u64(0x0123456789ABCDEFull);
    w.i32(-77);
    w.f32(3.5f);
    w.f64(-2.25);
    const std::array<std::byte, 3> tail{std::byte{1}, std::byte{2}, std::byte{3}};
    w.bytes(tail);

    ByteReader r(buf);
    std::uint8_t a = 0;
    std::uint16_t b = 0;
    std::uint32_t c = 0;
    std::uint64_t d = 0;
    std::int32_t e = 0;
    float f = 0.0f;
    double g = 0.0;
    std::span<const std::byte> rest;
    REQUIRE(r.u8(a));
    REQUIRE(r.u16(b));
    REQUIRE(r.u32(c));
    REQUIRE(r.u64(d));
    REQUIRE(r.i32(e));
    REQUIRE(r.f32(f));
    REQUIRE(r.f64(g));
    REQUIRE(r.bytes(rest, 3));

    CHECK(a == 0x12);
    CHECK(b == 0x1234);
    CHECK(c == 0x89ABCDEFu);
    CHECK(d == 0x0123456789ABCDEFull);
    CHECK(e == -77);
    CHECK(f == 3.5f);
    CHECK(g == -2.25);
    CHECK(rest.size() == 3);
    CHECK(std::to_integer<int>(rest[2]) == 3);
    CHECK(r.remaining() == 0);
}

TEST_CASE("multi-byte writes are little-endian on the wire") {
    std::vector<std::byte> buf;
    ByteWriter w(buf);
    w.u32(0x11223344u);
    REQUIRE(buf.size() == 4);
    CHECK(std::to_integer<int>(buf[0]) == 0x44); // low byte first
    CHECK(std::to_integer<int>(buf[1]) == 0x33);
    CHECK(std::to_integer<int>(buf[2]) == 0x22);
    CHECK(std::to_integer<int>(buf[3]) == 0x11);
}

TEST_CASE("a truncated read fails and consumes nothing") {
    std::vector<std::byte> buf;
    ByteWriter w(buf);
    w.u16(0xABCDu); // only two bytes present

    ByteReader r(buf);
    std::uint32_t wide = 0;
    CHECK_FALSE(r.u32(wide));  // wants four, has two
    CHECK(r.remaining() == 2); // and left the cursor untouched
    std::uint16_t narrow = 0;
    CHECK(r.u16(narrow)); // so the two bytes are still readable
    CHECK(narrow == 0xABCDu);
}

TEST_CASE("skip and bytes respect the remaining bound") {
    std::vector<std::byte> buf(4, std::byte{0});
    ByteReader r(buf);
    CHECK_FALSE(r.skip(5));
    std::span<const std::byte> s;
    CHECK_FALSE(r.bytes(s, 5));
    CHECK(r.skip(4));
    CHECK(r.remaining() == 0);
}
