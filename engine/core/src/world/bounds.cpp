// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The two non-constexpr pieces of the world-boundary contract. The reasoning behind both (why the
// hash image is hand-built, why the tile conversion clamps) is in rime/core/world/bounds.hpp.

#include "rime/core/world/bounds.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

#include "rime/core/hash.hpp"

namespace rime::core {

std::int32_t tile_of(float world_axis, float pitch) noexcept {
    // `!(pitch > 0)` also rejects NaN, which `pitch <= 0` would let through.
    if (!(pitch > 0.0f) || std::isnan(world_axis)) {
        return 0;
    }
    // Divide in double: in float, (pitch - 1 ulp) / pitch rounds up to exactly 1.0f and would put
    // a point just inside tile 0 into tile 1. Widening float to double is exact, and double has 29
    // more mantissa bits than float, which removes that case. It does not make the quotient
    // exact: a correctly rounded double quotient could in principle still land on the far side of
    // an integer, it is just vastly less likely than in float.
    const double tile = std::floor(static_cast<double>(world_axis) / static_cast<double>(pitch));
    constexpr double lo = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr double hi = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    if (tile <= lo) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (tile >= hi) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(tile);
}

namespace {

// Writes `value` little-endian at `out[offset..offset+4)`, independent of host byte order.
void put_u32_le(std::array<std::byte, 24>& out, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

} // namespace

std::uint64_t hash_bounds(const WorldBounds& b) noexcept {
    // Normalise so bounds that classify() cannot distinguish hash identically. Known limit: a
    // non-finite `floor_y` is a misconfiguration whose hash is not canonical (NaN has many bit
    // patterns, so two NaN floors that classify identically can hash differently).
    const std::int32_t band = b.band_tiles > 0 ? b.band_tiles : 0;
    const float floor_y = (b.floor_y == 0.0f) ? 0.0f : b.floor_y; // folds -0.0f onto +0.0f

    std::array<std::byte, 24> image{};
    put_u32_le(image, 0, static_cast<std::uint32_t>(b.hull.min_x));
    put_u32_le(image, 4, static_cast<std::uint32_t>(b.hull.min_z));
    put_u32_le(image, 8, static_cast<std::uint32_t>(b.hull.max_x));
    put_u32_le(image, 12, static_cast<std::uint32_t>(b.hull.max_z));
    put_u32_le(image, 16, static_cast<std::uint32_t>(band));
    put_u32_le(image, 20, std::bit_cast<std::uint32_t>(floor_y));
    return fnv1a_64(std::span<const std::byte>(image));
}

} // namespace rime::core

namespace rime::core {

Region classify_position(const WorldBounds& b,
                         float x,
                         float y,
                         float z,
                         float pitch_x,
                         float pitch_z) noexcept {
    // Finiteness first, before any conversion; the reason is in bounds.hpp.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return Region::Exterior;
    }
    return classify(b, tile_of(x, pitch_x), tile_of(z, pitch_z), y);
}

} // namespace rime::core
