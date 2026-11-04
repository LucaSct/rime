// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "rime/replication/contention_context.hpp"

using namespace rime::replication;

namespace {

void word(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<std::byte>(value >> (8 * i));
}

ContentionContext sample() {
    ContentionContext value{};
    value.owner_seat = 1;
    value.owner_generation = 4;
    value.context_revision = 9;
    word(value.crate, 24, std::bit_cast<std::uint32_t>(1.0f)); // quaternion w
    for (std::uint16_t seat = 2; seat <= 4; ++seat) {
        ContentionProxy proxy{};
        proxy.seat = seat;
        proxy.seat_generation = 7;
        proxy.net_id = {seat + 10u, 3};
        proxy.position = {static_cast<float>(seat), 1.0f, 2.0f};
        proxy.velocity = {1.0f, 0.0f, 0.0f};
        proxy.axes = {0.5f, -0.5f};
        proxy.held = 1;
        value.proxies.push_back(proxy);
    }
    return value;
}

std::vector<std::byte> wire(const ContentionContext& value) {
    std::array<std::byte, kContentionMaxBytes> bytes{};
    std::size_t written = 0;
    REQUIRE(encode_contention_context(value, bytes, written) == ContentionRefusal::None);
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(written)};
}

void refused(std::span<const std::byte> bytes, ContentionRefusal reason) {
    ContentionContext out = sample();
    CHECK(decode_contention_context(bytes, out) == reason);
    CHECK(out == sample()); // decode is transactional too
}

} // namespace

TEST_CASE("contention context v2 round trips at its 208-byte maximum") {
    const auto original = sample();
    const auto bytes = wire(original);
    REQUIRE(bytes.size() == 208);
    CHECK(kContentionMaxBytes == 208);
    CHECK(bytes[0] == std::byte{2});
    CHECK(bytes[1] == std::byte{3});
    CHECK(bytes[2] == std::byte{0});
    CHECK(bytes[4] == std::byte{1});              // owner seat, little-endian
    CHECK(bytes[6] == std::byte{4});              // owner generation
    CHECK(bytes[8] == std::byte{9});              // context revision
    CHECK(bytes[12 + 24 + 3] == std::byte{0x3F}); // crate quaternion w = 1
    CHECK(bytes[64] == std::byte{2});             // first proxy seat
    CHECK(bytes[64 + 4] == std::byte{12});        // first proxy NetId index
    CHECK(bytes[64 + 44] == std::byte{1});        // first proxy held buttons
    ContentionContext decoded{};
    REQUIRE(decode_contention_context(bytes, decoded) == ContentionRefusal::None);
    CHECK(decoded == original);
}

TEST_CASE("contention context refuses every malformed field without replacing output") {
    const auto good = wire(sample());
    auto bad = good;
    bad.pop_back();
    refused(bad, ContentionRefusal::BadLength);
    bad = good;
    bad.push_back(std::byte{0});
    refused(bad, ContentionRefusal::BadLength);
    bad = good;
    bad[0] = std::byte{3};
    refused(bad, ContentionRefusal::UnknownVersion);
    bad = good;
    bad[1] = std::byte{4};
    refused(bad, ContentionRefusal::TooManyProxies);
    bad = good;
    bad[2] = std::byte{1};
    refused(bad, ContentionRefusal::ReservedNonZero);
    bad = good;
    word(bad, 12 + 28, 0x7fc00000u); // crate linear velocity x
    refused(bad, ContentionRefusal::NonFinite);
    bad = good;
    word(bad, 12 + 24, std::bit_cast<std::uint32_t>(0.5f));
    refused(bad, ContentionRefusal::NonUnitQuaternion);
    bad = good;
    word(bad, 64 + 2 * 48 + 24, 0x7fc00000u); // last proxy velocity x
    refused(bad, ContentionRefusal::NonFinite);
    bad = good;
    bad[64 + 48 + 4] = bad[64 + 4]; // duplicate NetId index's low byte
    bad[64 + 48 + 5] = bad[64 + 5];
    refused(bad, ContentionRefusal::DuplicateProxyIdentity);
    bad = good;
    bad[64] = std::byte{1};
    bad[65] = std::byte{0};
    bad[66] = std::byte{4};
    bad[67] = std::byte{0};
    refused(bad, ContentionRefusal::ProxyIsOwner);
    bad = good;
    bad[64 + 48] = std::byte{0};
    refused(bad, ContentionRefusal::SeatsNotAscending);
}
