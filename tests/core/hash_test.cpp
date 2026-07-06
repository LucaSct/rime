// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the core FNV-1a hash (M6.1): it matches the published 64-bit vectors, seeding continues
// a hash (so folding fields equals hashing their concatenation — the property the reflection schema
// hash and content ids rely on), the byte and string overloads agree, a one-byte change avalanches,
// and it works in a constant expression.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rime/core/hash.hpp"

using namespace rime::core;

TEST_CASE("fnv1a_64 matches the published 64-bit vectors") {
    CHECK(fnv1a_64(std::string_view("")) == 0xcbf29ce484222325ull); // == offset basis
    CHECK(fnv1a_64(std::string_view("")) == kFnv1a64OffsetBasis);
    CHECK(fnv1a_64(std::string_view("a")) == 0xaf63dc4c8601ec8cull);
    CHECK(fnv1a_64(std::string_view("foobar")) == 0x85944171f73967e8ull);
}

TEST_CASE("seeding continues the hash: fold(a, then b) == hash(a concat b)") {
    const std::uint64_t after_foo = fnv1a_64(std::string_view("foo"));
    CHECK(fnv1a_64(std::string_view("bar"), after_foo) == fnv1a_64(std::string_view("foobar")));
}

TEST_CASE("the byte-span and string overloads agree on identical bytes") {
    const std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    CHECK(fnv1a_64(std::span<const std::byte>(bytes)) == fnv1a_64(std::string_view("abc")));
}

TEST_CASE("a one-byte change avalanches to a different hash") {
    CHECK(fnv1a_64(std::string_view("asset-01")) != fnv1a_64(std::string_view("asset-02")));
}

TEST_CASE("fnv1a_64 is usable in a constant expression") {
    constexpr std::uint64_t h = fnv1a_64(std::string_view("compile-time"));
    static_assert(h != 0, "a non-empty string hashes to something");
    CHECK(h == fnv1a_64(std::string_view("compile-time")));
}
