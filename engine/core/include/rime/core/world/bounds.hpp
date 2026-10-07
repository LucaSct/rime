// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// The world boundary: ONE pure predicate that answers "is this position outside the world?".
// Decision record: docs/adr/0079-the-world-boundary-and-what-lies-beyond-it.md.
//
// ---------------------------------------------------------------------------------------------
// Why one predicate, not three.
// ---------------------------------------------------------------------------------------------
// A game needs a kill plane (fell out of the world), a bounds check (walked off the map) and a
// debris cull (a chunk of rubble flew away). Written separately, those are three tests that can
// quietly disagree: a body the cull deletes but the bounds check still counts, a player the kill
// plane removes on one peer and not another. ADR-0079 therefore decides that all three are the
// same fact, "this position is Exterior", and that fact lives here, once. `is_exterior` and
// `destructible` are defined in terms of `classify` rather than re-derived, so there is no second
// copy of the geometry to drift. Because `classify` is a pure function of integer tile
// coordinates and one height, every peer evaluates it identically, and `hash_bounds` lets the
// destruction hash fold the boundary in: the boundary is then part of the shared truth instead of
// a rule each peer applies on its own.
//
// Why it lives in `core`. `gameplay`, `destruction` and `render` all need to ask. `gameplay`
// deliberately has no edge to `destruction` (see engine/gameplay/CMakeLists.txt), so the
// predicate cannot live in either without a sideways dependency. `core` depends on nothing above
// it, so all three can reach down into it. For the same reason it takes plain `std::int32_t`
// tile coordinates rather than a terrain-tile type from `assets`; the caller converts.
//
// ---------------------------------------------------------------------------------------------
// Why the floor is ABSOLUTE and STRICT.
// ---------------------------------------------------------------------------------------------
// `floor_y` is a single height below the lowest point of the cooked world (ADR-0079: world
// minimum minus 50 m). It is deliberately NOT "below the terrain under the entity". A floor
// relative to the local terrain looks tidier but SEALS DESTRUCTION: a crater, a basement, a cooked
// tunnel or a hole blown through a floor all put legitimate things below the terrain height at
// their own (x, z), and a relative test would delete them. Destruction is this engine's headline
// feature; the boundary must never fight it. If you are tempted to "fix" this by making the floor
// follow the terrain, do not. The test is strict (`y < floor_y`): a point exactly on the floor is
// still inside, so the rule is a clean half-open one with no tolerance to tune.
//
// ---------------------------------------------------------------------------------------------
// Why the arithmetic is widened.
// ---------------------------------------------------------------------------------------------
// The hull plus its band is `hull.max + band_tiles`. For a hull near the limit of `std::int32_t`
// that sum overflows, and signed overflow is undefined behaviour: in a constant expression it is a
// hard compile error, at runtime a silent wrong answer (the compiler may assume it cannot happen).
// All span arithmetic is therefore done in `std::int64_t`, compared there, and never narrowed
// back. Tile coordinates themselves stay integers throughout, never floats, so there is no
// platform-dependent rounding in the decision (ADR-0079).
//
// ---------------------------------------------------------------------------------------------
// Why the hash image is built by hand.
// ---------------------------------------------------------------------------------------------
// Hashing the struct's bytes would hash its padding and its layout, both of which a compiler or
// platform may change, and would rehash every saved world when someone reordered a field.
// `hash_bounds` instead serialises each field little-endian into a fixed 24-byte image and hashes
// that, so the value is byte-identical on every platform and only a deliberate change to the
// image changes it. Values that `classify` cannot tell apart also hash identically: a negative
// `band_tiles` is the same as 0, and `-0.0f` is the same floor as `0.0f`.

#include <cstdint>

namespace rime::core {

// Shell A and the ring of Shell B around it, in integer tile coordinates. Inclusive on all four
// sides: a 1x1 hull is min == max. A span with min > max on either axis is empty.
struct TileSpan {
    std::int32_t min_x = 0;
    std::int32_t min_z = 0;
    std::int32_t max_x = 0;
    std::int32_t max_z = 0;
};

struct WorldBounds {
    TileSpan hull{};             // Shell A: cooked, destructible
    std::int32_t band_tiles = 0; // Shell B: the ring of this many tiles around `hull`. 0 = no band.
    float floor_y = 0.0f;        // the ABSOLUTE floor. ADR-0079: world_min - 50 m.
};

enum class Region : std::uint8_t {
    Hull = 0,     // Shell A: cooked, destructible, full gameplay
    Band = 1,     // Shell B: drivable, destruction-inert
    Exterior = 2, // beyond the band, or below the floor
};

// Which region a position is in. Below the floor is Exterior wherever it is horizontally (the
// height test is an OR with the tile test, not a fallback), and an empty hull is Exterior for
// every input: a world with no interior has nothing inside it, and that is a usable answer.
// A negative `band_tiles` is treated as 0 rather than as a hull that shrinks.
[[nodiscard]] constexpr Region classify(const WorldBounds& b, std::int32_t tile_x,
                                        std::int32_t tile_z, float y) noexcept {
    const std::int64_t min_x = b.hull.min_x;
    const std::int64_t min_z = b.hull.min_z;
    const std::int64_t max_x = b.hull.max_x;
    const std::int64_t max_z = b.hull.max_z;
    if (min_x > max_x || min_z > max_z) {
        return Region::Exterior;
    }
    if (y < b.floor_y) {
        return Region::Exterior;
    }
    const std::int64_t band = b.band_tiles > 0 ? b.band_tiles : 0;
    const std::int64_t x = tile_x;
    const std::int64_t z = tile_z;
    if (x >= min_x && x <= max_x && z >= min_z && z <= max_z) {
        return Region::Hull;
    }
    if (x >= min_x - band && x <= max_x + band && z >= min_z - band && z <= max_z + band) {
        return Region::Band;
    }
    return Region::Exterior;
}

// The predicate ADR-0079 names. Exactly `classify(...) == Region::Exterior`.
[[nodiscard]] constexpr bool is_exterior(const WorldBounds& b, std::int32_t tile_x,
                                         std::int32_t tile_z, float y) noexcept {
    return classify(b, tile_x, tile_z, y) == Region::Exterior;
}

// True only in Shell A. ADR-0079: weapons cannot damage cooked buildings in the band, so the edge
// can never affect the destruction hash.
[[nodiscard]] constexpr bool destructible(const WorldBounds& b, std::int32_t tile_x,
                                          std::int32_t tile_z, float y) noexcept {
    return classify(b, tile_x, tile_z, y) == Region::Hull;
}

// Tile coordinate of a world-space axis position: floor(world_axis / pitch), so tile 0 spans
// [0, pitch) and -0.5 m with a 64 m pitch lands in tile -1. `pitch` is metres per tile and must be
// > 0; a pitch <= 0 (or NaN) returns 0, since a caller with no grid has no tiles. The result is
// clamped into `std::int32_t`'s range before narrowing, because a float can hold far larger
// values and the cast would be undefined. Not constexpr: std::floor is not usable in a constant
// expression in C++20.
[[nodiscard]] std::int32_t tile_of(float world_axis, float pitch) noexcept;

// Folds the bounds into a 64-bit value, for ADR-0079's requirement that the boundary be part of
// the destruction hash rather than a second truth. Byte-identical on every platform.
[[nodiscard]] std::uint64_t hash_bounds(const WorldBounds& b) noexcept;

} // namespace rime::core
