// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/assets/procedural_terrain.hpp"

#include <algorithm>

namespace rime::assets {

namespace {

// ── Fixed-point conventions ────────────────────────────────────────────────────────────────────
// Interpolation parameters are u16.16 fractions in [0, 65536]. Heights are in quantisation steps
// (1/256 m). Every product below is of two non-negative integers well under 2^32, held in u64, so
// there is no signed shift, no overflow and no rounding mode to depend on: results are rounded to
// nearest with an explicit "+ half" before an exact power-of-two divide.
constexpr std::uint64_t kOne = 65536;

// Height levels, in quantisation steps: (metres - offset) * 256.
constexpr std::uint64_t kStreetLevel = 640;  // -1.5 m
constexpr std::uint64_t kGroundLevel = 1024; //  0.0 m
constexpr std::uint64_t kHillRange = 8192;   // 32 m of hills on top of the ground

// Distances, in 1/256 m. The blend bands, measured outward from the street rectangle.
constexpr std::uint64_t kStreetBlendEnd = 12u * 256u;            // street -> ground
constexpr std::uint64_t kHoldEnd = kStreetBlendEnd + 64u * 256u; // flat outer band
constexpr std::uint64_t kHillBlendEnd = kHoldEnd + 64u * 256u;   // ground -> hills
// Past this many metres from the rectangle on either axis the hills are at full weight, so the
// per-axis distances may be clamped here before squaring: it bounds dx^2 + dz^2 (and the << 16
// below) without changing any answer.
constexpr std::int64_t kDistanceClampMetres = 256;

// smoothstep(t) = t^2 (3 - 2t) on a u16.16 fraction. C1-continuous at both ends, so a blend meets
// the flat band and the hills without a crease a rolling body would feel as a kick.
[[nodiscard]] std::uint64_t smooth(std::uint64_t t) noexcept {
    const std::uint64_t t2 = (t * t) >> 16;
    return (t2 * (3 * kOne - 2 * t)) >> 16;
}

// a + (b - a) * s, for u16.16 s, computed as a weighted sum so nothing is negative.
[[nodiscard]] std::uint64_t lerp(std::uint64_t a, std::uint64_t b, std::uint64_t s) noexcept {
    return (a * (kOne - s) + b * s + kOne / 2) >> 16;
}

// Exact floor(sqrt(n)): the digit-by-digit method, no floating point.
[[nodiscard]] std::uint64_t isqrt(std::uint64_t n) noexcept {
    std::uint64_t result = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= result + bit) {
            n -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// Floor division and the matching non-negative remainder, for a positive divisor. C++ division
// truncates toward zero, which would fold the cells either side of the origin into one double-wide
// cell and put a seam at x = 0.
struct FloorDiv {
    std::int64_t cell;
    std::uint64_t frac; // in [0, divisor)
};

[[nodiscard]] FloorDiv floor_div(std::int64_t v, std::int64_t divisor) noexcept {
    std::int64_t q = v / divisor;
    std::int64_t r = v % divisor;
    if (r < 0) {
        r += divisor;
        --q;
    }
    return {q, static_cast<std::uint64_t>(r)};
}

// A 64-bit finaliser (the murmur3 / splitmix "fmix64" avalanche). Specified here rather than
// borrowed from <random> so the same seed names the same lattice on every standard library.
[[nodiscard]] std::uint64_t mix(std::uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

// The lattice value at integer cell (cx, cz) of one octave: u16, uniform. Signed cell indices are
// converted to u64 by modular arithmetic, which is defined and identical everywhere.
[[nodiscard]] std::uint64_t
lattice(std::uint64_t seed, std::uint32_t octave, std::int64_t cx, std::int64_t cz) noexcept {
    std::uint64_t h = mix(seed ^ (0x9e3779b97f4a7c15ull * (std::uint64_t{octave} + 1)));
    h = mix(h ^ (static_cast<std::uint64_t>(cx) * 0xbf58476d1ce4e5b9ull));
    h = mix(h ^ (static_cast<std::uint64_t>(cz) * 0x94d049bb133111ebull));
    return (h >> 24) & 0xffffu;
}

// One octave of VALUE NOISE: random values on a square lattice of `cell` metres, bilinearly
// blended with a smoothstep fade so the result is C1 across lattice lines. `cell` is a power of
// two, which makes the fraction an exact u16.16 with no division rounding.
[[nodiscard]] std::uint64_t value_noise(std::uint64_t seed,
                                        std::uint32_t octave,
                                        std::int64_t cell,
                                        std::int64_t x,
                                        std::int64_t z) noexcept {
    const FloorDiv fx = floor_div(x, cell);
    const FloorDiv fz = floor_div(z, cell);
    const std::uint64_t tx = smooth(fx.frac * kOne / static_cast<std::uint64_t>(cell));
    const std::uint64_t tz = smooth(fz.frac * kOne / static_cast<std::uint64_t>(cell));
    const std::uint64_t v00 = lattice(seed, octave, fx.cell, fz.cell);
    const std::uint64_t v10 = lattice(seed, octave, fx.cell + 1, fz.cell);
    const std::uint64_t v01 = lattice(seed, octave, fx.cell, fz.cell + 1);
    const std::uint64_t v11 = lattice(seed, octave, fx.cell + 1, fz.cell + 1);
    return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), tz);
}

// Hill height above the ground in quantisation steps, [0, kHillRange]. Three octaves at 128 / 32 /
// 8 m with weights 38/64, 19/64, 7/64 (summing to exactly 64, so the mix is a shift): broad
// swells, rolling mid-scale relief and a little surface roughness that a 2 m grid can resolve.
[[nodiscard]] std::uint64_t hills(std::uint64_t seed, std::int64_t x, std::int64_t z) noexcept {
    const std::uint64_t n =
        (value_noise(seed, 0, 128, x, z) * 38 + value_noise(seed, 1, 32, x, z) * 19 +
         value_noise(seed, 2, 8, x, z) * 7) >>
        6; // [0, 65535]
    return (n * kHillRange + 32767) / 65535;
}

// Distance outside the street rectangle in 1/256 m; zero inside (the rectangle is inclusive).
[[nodiscard]] std::uint64_t
street_distance(const ProceduralTerrainConfig& c, std::int64_t x, std::int64_t z) noexcept {
    const std::int64_t dx = std::max<std::int64_t>({c.street_min_x - x, x - c.street_max_x, 0});
    const std::int64_t dz = std::max<std::int64_t>({c.street_min_z - z, z - c.street_max_z, 0});
    const auto cx = static_cast<std::uint64_t>(std::min(dx, kDistanceClampMetres));
    const auto cz = static_cast<std::uint64_t>(std::min(dz, kDistanceClampMetres));
    return isqrt((cx * cx + cz * cz) << 16); // sqrt(m^2 * 65536) = 256 * m
}

[[nodiscard]] std::uint16_t
sample_unchecked(const ProceduralTerrainConfig& c, std::int64_t x, std::int64_t z) noexcept {
    const std::uint64_t d = street_distance(c, x, z);
    if (d == 0) {
        return static_cast<std::uint16_t>(kStreetLevel);
    }
    if (d <= kStreetBlendEnd) {
        return static_cast<std::uint16_t>(
            lerp(kStreetLevel, kGroundLevel, smooth(d * kOne / kStreetBlendEnd)));
    }
    if (d <= kHoldEnd) {
        return static_cast<std::uint16_t>(kGroundLevel);
    }
    const std::uint64_t h = hills(c.seed, x, z);
    if (d >= kHillBlendEnd) {
        return static_cast<std::uint16_t>(kGroundLevel + h);
    }
    const std::uint64_t t = (d - kHoldEnd) * kOne / (kHillBlendEnd - kHoldEnd);
    return static_cast<std::uint16_t>(kGroundLevel + lerp(0, h, smooth(t)));
}

} // namespace

bool procedural_config_valid(const ProceduralTerrainConfig& c) noexcept {
    const auto fits = [](std::int32_t v) {
        return v >= -kProceduralMaxWorldMetres && v <= kProceduralMaxWorldMetres;
    };
    return c.version == kProceduralTerrainVersion && c.street_min_x <= c.street_max_x &&
           c.street_min_z <= c.street_max_z && fits(c.street_min_x) && fits(c.street_max_x) &&
           fits(c.street_min_z) && fits(c.street_max_z);
}

std::int64_t procedural_tile_width_metres(std::uint32_t lod) noexcept {
    if (lod > kProceduralMaxLod) {
        return 0;
    }
    return std::int64_t{kProceduralTileCells * kProceduralBaseCellMetres} << lod;
}

bool procedural_tile_supported(std::int32_t tile_x,
                               std::int32_t tile_z,
                               std::uint32_t lod) noexcept {
    const std::int64_t width = procedural_tile_width_metres(lod);
    if (width == 0) {
        return false;
    }
    // int32 * (<= 2^14) fits int64 with room to spare; both the near and far edge must be exact.
    const auto edges_fit = [&](std::int32_t t) {
        const std::int64_t near_edge = std::int64_t{t} * width;
        return near_edge >= -kProceduralMaxWorldMetres &&
               near_edge + width <= kProceduralMaxWorldMetres;
    };
    return edges_fit(tile_x) && edges_fit(tile_z);
}

std::optional<std::uint16_t> procedural_terrain_sample(const ProceduralTerrainConfig& config,
                                                       std::int64_t x,
                                                       std::int64_t z) noexcept {
    // |x|, |z| <= 2^40 keeps the (cell + 1) lattice step and the distance subtraction far from
    // int64 overflow. Outside that there is no answer, and nullopt says so rather than clamping.
    constexpr std::int64_t kLimit = std::int64_t{1} << 40;
    if (!procedural_config_valid(config) || x < -kLimit || x > kLimit || z < -kLimit ||
        z > kLimit) {
        return std::nullopt;
    }
    return sample_unchecked(config, x, z);
}

std::optional<HeightfieldAsset> generate_procedural_tile(const ProceduralTerrainConfig& config,
                                                         std::int32_t tile_x,
                                                         std::int32_t tile_z,
                                                         std::uint32_t lod) {
    if (!procedural_config_valid(config) || !procedural_tile_supported(tile_x, tile_z, lod)) {
        return std::nullopt;
    }
    const std::int64_t width = procedural_tile_width_metres(lod);
    const std::int64_t spacing = std::int64_t{kProceduralBaseCellMetres} << lod;
    const std::int64_t origin_x = std::int64_t{tile_x} * width;
    const std::int64_t origin_z = std::int64_t{tile_z} * width;
    const std::uint32_t side = kProceduralTileCells + 1;

    HeightfieldAsset a;
    a.columns = side;
    a.rows = side;
    a.cell_size_x = static_cast<float>(spacing);
    a.cell_size_z = static_cast<float>(spacing);
    // Exact: |origin| <= 2^24 by procedural_tile_supported.
    a.origin = {static_cast<float>(origin_x), 0.0f, static_cast<float>(origin_z)};
    a.height_scale = kProceduralHeightScale;
    a.height_offset = kProceduralHeightOffset;
    a.triangulation = HeightfieldTriangulation::DiagonalMinToMax;
    a.samples.resize(std::size_t{side} * side);
    std::uint16_t lo = 0xffffu;
    std::uint16_t hi = 0;
    for (std::uint32_t j = 0; j < side; ++j) {
        for (std::uint32_t i = 0; i < side; ++i) {
            const std::uint16_t s = sample_unchecked(
                config, origin_x + std::int64_t{i} * spacing, origin_z + std::int64_t{j} * spacing);
            a.samples[a.index(i, j)] = s;
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
    }
    a.min_sample = lo;
    a.max_sample = hi;
    return a;
}

} // namespace rime::assets
