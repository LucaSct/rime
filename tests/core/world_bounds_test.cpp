// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the world-boundary predicate (ADR-0079): one function classifies a position as Hull,
// Band or Exterior; the floor is absolute and strict; the band is a ring; span arithmetic cannot
// overflow; a degenerate hull is all exterior; tile conversion floors and clamps; and the hash of
// the bounds is stable, sensitive to every field, and blind to differences classify() cannot see.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "rime/core/world.hpp"

using namespace rime::core;

namespace {

// A 10x10-tile hull at [0,9]^2 with a 3-tile band and the floor at -100 m.
constexpr WorldBounds make_bounds(std::int32_t band = 3) {
    return WorldBounds{TileSpan{0, 0, 9, 9}, band, -100.0f};
}

constexpr float kAbove = 0.0f; // comfortably above the floor

} // namespace

// The predicate must be usable at compile time, so a world can be validated by static_assert.
static_assert(classify(make_bounds(), 5, 5, kAbove) == Region::Hull);
static_assert(classify(make_bounds(), 11, 5, kAbove) == Region::Band);
static_assert(is_exterior(make_bounds(), 13, 5, kAbove));

TEST_CASE("a point in the hull interior is Hull and destructible") {
    constexpr WorldBounds b = make_bounds();
    CHECK(classify(b, 5, 5, kAbove) == Region::Hull);
    CHECK(classify(b, 0, 0, kAbove) == Region::Hull); // inclusive corners
    CHECK(classify(b, 9, 9, kAbove) == Region::Hull);
    CHECK(destructible(b, 5, 5, kAbove));
    CHECK_FALSE(is_exterior(b, 5, 5, kAbove));
}

TEST_CASE("a point in the band is Band and NOT destructible") {
    constexpr WorldBounds b = make_bounds();
    CHECK(classify(b, 11, 5, kAbove) == Region::Band);
    CHECK_FALSE(destructible(b, 11, 5, kAbove));
    CHECK_FALSE(is_exterior(b, 11, 5, kAbove));
}

TEST_CASE("the band is a ring: all four sides and all four corners at band_tiles out are Band") {
    constexpr WorldBounds b = make_bounds(3);
    CHECK(classify(b, 9 + 3, 5, kAbove) == Region::Band);
    CHECK(classify(b, 0 - 3, 5, kAbove) == Region::Band);
    CHECK(classify(b, 5, 9 + 3, kAbove) == Region::Band);
    CHECK(classify(b, 5, 0 - 3, kAbove) == Region::Band);
    CHECK(classify(b, 12, 12, kAbove) == Region::Band);
    CHECK(classify(b, -3, 12, kAbove) == Region::Band);
    CHECK(classify(b, 12, -3, kAbove) == Region::Band);
    CHECK(classify(b, -3, -3, kAbove) == Region::Band);
}

TEST_CASE("one tile beyond the band is Exterior on each of the four sides") {
    constexpr WorldBounds b = make_bounds(3);
    CHECK(classify(b, 13, 5, kAbove) == Region::Exterior);
    CHECK(classify(b, -4, 5, kAbove) == Region::Exterior);
    CHECK(classify(b, 5, 13, kAbove) == Region::Exterior);
    CHECK(classify(b, 5, -4, kAbove) == Region::Exterior);
    CHECK(classify(b, 13, 13, kAbove) == Region::Exterior);
}

TEST_CASE("band_tiles == 0 makes every tile outside the hull Exterior") {
    constexpr WorldBounds b = make_bounds(0);
    CHECK(classify(b, 9, 9, kAbove) == Region::Hull);
    CHECK(classify(b, 10, 5, kAbove) == Region::Exterior);
    CHECK(classify(b, -1, 5, kAbove) == Region::Exterior);
    CHECK(classify(b, 5, 10, kAbove) == Region::Exterior);
    CHECK(classify(b, 5, -1, kAbove) == Region::Exterior);
}

TEST_CASE("a negative band_tiles behaves exactly like 0, in classify and in hash_bounds") {
    constexpr WorldBounds neg = make_bounds(-5);
    constexpr WorldBounds zero = make_bounds(0);
    for (std::int32_t x = -8; x <= 18; ++x) {
        for (std::int32_t z = -8; z <= 18; ++z) {
            CHECK(classify(neg, x, z, kAbove) == classify(zero, x, z, kAbove));
        }
    }
    // It must not shrink the hull.
    CHECK(classify(neg, 9, 9, kAbove) == Region::Hull);
    CHECK(hash_bounds(neg) == hash_bounds(zero));
}

TEST_CASE("below the floor inside the hull is Exterior and not destructible") {
    constexpr WorldBounds b = make_bounds();
    CHECK(classify(b, 5, 5, -200.0f) == Region::Exterior);
    CHECK(is_exterior(b, 5, 5, -200.0f));
    CHECK_FALSE(destructible(b, 5, 5, -200.0f));
    // The height test is an OR: the band is not a fallback that rescues a sunken point.
    CHECK(classify(b, 11, 5, -200.0f) == Region::Exterior);
}

TEST_CASE("exactly at floor_y is interior and one ULP below is Exterior") {
    constexpr WorldBounds b = make_bounds();
    const float just_below = std::nextafter(b.floor_y, -std::numeric_limits<float>::infinity());
    CHECK(just_below < b.floor_y);
    CHECK(classify(b, 5, 5, b.floor_y) == Region::Hull);
    CHECK(classify(b, 5, 5, just_below) == Region::Exterior);
}

TEST_CASE("a degenerate hull is Exterior everywhere, above and below the floor") {
    const WorldBounds flipped_x{TileSpan{5, 0, 4, 9}, 3, -100.0f};
    const WorldBounds flipped_z{TileSpan{0, 5, 9, 4}, 3, -100.0f};
    for (const WorldBounds& b : {flipped_x, flipped_z}) {
        CHECK(classify(b, 4, 4, kAbove) == Region::Exterior);
        CHECK(classify(b, 5, 5, kAbove) == Region::Exterior);
        CHECK(classify(b, 0, 0, kAbove) == Region::Exterior);
        CHECK(classify(b, 5, 5, -500.0f) == Region::Exterior);
        CHECK_FALSE(destructible(b, 5, 5, kAbove));
    }
}

TEST_CASE("a hull at INT32_MAX with a positive band does not overflow") {
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    constexpr WorldBounds hi{TileSpan{kMax - 10, kMax - 10, kMax, kMax}, 1000, -100.0f};
    // max + band would be 2^31 + 999 in int32: UB. Widened, the band simply covers up to INT32_MAX.
    CHECK(classify(hi, kMax, kMax, kAbove) == Region::Hull);
    CHECK(classify(hi, kMax - 11, kMax, kAbove) == Region::Band);
    CHECK(classify(hi, kMax - 1011, kMax, kAbove) == Region::Exterior);
    // The same on the negative end: min - band would underflow.
    constexpr WorldBounds lo{TileSpan{kMin, kMin, kMin + 10, kMin + 10}, 1000, -100.0f};
    CHECK(classify(lo, kMin, kMin, kAbove) == Region::Hull);
    CHECK(classify(lo, kMin + 11, kMin, kAbove) == Region::Band);
    CHECK(classify(lo, kMin + 1011, kMin, kAbove) == Region::Exterior);
    // Constant evaluation would reject overflow outright.
    static_assert(classify(hi, kMax, kMax, 0.0f) == Region::Hull);
    static_assert(classify(lo, kMin, kMin, 0.0f) == Region::Hull);
}

TEST_CASE("tile_of floors: tile 0 spans [0, pitch) and negatives go to tile -1") {
    constexpr float pitch = 64.0f;
    CHECK(tile_of(0.0f, pitch) == 0);
    CHECK(tile_of(std::nextafter(pitch, 0.0f), pitch) == 0);
    CHECK(tile_of(-0.5f, pitch) == -1);
    CHECK(tile_of(pitch, pitch) == 1);
    CHECK(tile_of(-pitch, pitch) == -1);
    CHECK(tile_of(std::nextafter(-pitch, 0.0f), pitch) == -1);
    CHECK(tile_of(-pitch - 1.0f, pitch) == -2);
}

TEST_CASE("tile_of with no usable grid returns 0") {
    CHECK(tile_of(100.0f, 0.0f) == 0);
    CHECK(tile_of(100.0f, -64.0f) == 0);
    CHECK(tile_of(-100.0f, -64.0f) == 0);
    CHECK(tile_of(100.0f, std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(tile_of(std::numeric_limits<float>::quiet_NaN(), 64.0f) == 0);
}

TEST_CASE("tile_of clamps a huge position instead of invoking undefined behaviour") {
    CHECK(tile_of(1e30f, 1.0f) == std::numeric_limits<std::int32_t>::max());
    CHECK(tile_of(-1e30f, 1.0f) == std::numeric_limits<std::int32_t>::min());
    CHECK(tile_of(std::numeric_limits<float>::infinity(), 1.0f) ==
          std::numeric_limits<std::int32_t>::max());
    CHECK(tile_of(-std::numeric_limits<float>::infinity(), 1.0f) ==
          std::numeric_limits<std::int32_t>::min());
}

TEST_CASE("hash_bounds differs when any one field differs") {
    const WorldBounds base{TileSpan{1, 2, 30, 40}, 4, -100.0f};
    const std::uint64_t h = hash_bounds(base);
    WorldBounds v = base;
    v.hull.min_x = 0;
    CHECK(hash_bounds(v) != h);
    v = base;
    v.hull.min_z = 0;
    CHECK(hash_bounds(v) != h);
    v = base;
    v.hull.max_x = 31;
    CHECK(hash_bounds(v) != h);
    v = base;
    v.hull.max_z = 41;
    CHECK(hash_bounds(v) != h);
    v = base;
    v.band_tiles = 5;
    CHECK(hash_bounds(v) != h);
    v = base;
    v.floor_y = -100.5f;
    CHECK(hash_bounds(v) != h);
}

TEST_CASE("hash_bounds treats 0.0f and -0.0f floors as the same bounds") {
    WorldBounds pos{TileSpan{0, 0, 9, 9}, 3, 0.0f};
    WorldBounds neg = pos;
    neg.floor_y = -0.0f;
    CHECK(std::signbit(neg.floor_y));
    CHECK(hash_bounds(pos) == hash_bounds(neg));
}

TEST_CASE("hash_bounds is stable: a pinned literal breaks loudly if the byte image changes") {
    const WorldBounds b{TileSpan{-4, -4, 59, 59}, 8, -150.0f};
    CHECK(hash_bounds(b) == 0x18b1492416a3cb86ull);
}
