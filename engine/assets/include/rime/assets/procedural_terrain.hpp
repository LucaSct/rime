// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

#include "rime/assets/heightfield_asset.hpp"

// A PURE, BOUNDED procedural terrain source: a function from (config, tile, lod) to a
// HeightfieldAsset, with no files, no clock, no global state and no floating-point noise.
//
// WHY INTEGERS ALL THE WAY DOWN. Terrain collision is simulation state: a server and a client that
// disagree about a sample by one quantisation step disagree about where a body rests, and the
// disagreement is invisible until two machines compare world hashes. Floating-point noise (sin,
// cos, a float lattice) is allowed by the language to differ across compilers and libm versions —
// the project has already measured one commit flipping a gate between macOS and Linux for exactly
// that reason. So every sample here is computed from integers with a specified hash, a specified
// fixed-point interpolation and specified rounding, and is bit-identical on every platform. The
// only floats in the result are the ones the asset format itself stores (origin, cell size, scale
// and offset), and all of those are exact binary values.
//
// WHAT "ENDLESS" DOES NOT MEAN. Samples are computed from GLOBAL integer METRE coordinates in
// int64, so the noise itself has no seam and no overflow anywhere an int32 tile index can name. But
// a tile's origin is a float, and a float names every integer exactly only up to 2^24. A tile whose
// far edge lies beyond +/-2^24 m (`kProceduralMaxWorldMetres`) is therefore REFUSED rather than
// handed out with a rounded origin. This is a finite-precision engine world, not a claim of
// endless coordinates.
//
// THE SHAPE OF THE WORLD (all distances in integer metres, measured from the street rectangle):
//
//   inside the street rectangle          height -1.5 m  (below the y[-1, 0] slab: no coplanar
//                                                        z-fighting and no second floor contact)
//   0 .. 12 m outside                    smoothstep blend  -1.5 m -> 0 m
//   12 .. 76 m                           flat 0 m  (the outer band)
//   76 .. 140 m                          smoothstep blend  0 m -> hills
//   beyond 140 m                         0 m + hills in [0, 32] m
//
// The flat region is a function of the SIGNED DISTANCE to the rectangle and of nothing else, so
// every LOD agrees on it: a coarse tile is a subsample of the fine one, never a different surface.
namespace rime::assets {

inline constexpr std::uint32_t kProceduralTerrainVersion = 1; // the only version this build knows
inline constexpr std::uint32_t kProceduralTileCells = 32; // cells per tile side: 33 x 33 samples
inline constexpr std::uint32_t kProceduralBaseCellMetres = 2; // LOD 0 sample spacing
inline constexpr std::uint32_t kProceduralMaxLod = 8;
// Largest |coordinate| at which a float still names every integer metre exactly.
inline constexpr std::int64_t kProceduralMaxWorldMetres = std::int64_t{1} << 24;
// Quantisation: height = kProceduralHeightOffset + kProceduralHeightScale * sample. Both exact in
// binary, so decoding a sample is exact in float too.
inline constexpr float kProceduralHeightScale = 1.0f / 256.0f;
inline constexpr float kProceduralHeightOffset = -4.0f;

struct ProceduralTerrainConfig {
    std::uint64_t seed = 0x52494d45; // "RIME"
    std::uint32_t version = kProceduralTerrainVersion;
    // The flat street footprint, INCLUSIVE, in integer metres. Derived by integration from the
    // original sample's GroundSurface.
    std::int32_t street_min_x = -16;
    std::int32_t street_max_x = 60;
    std::int32_t street_min_z = -38;
    std::int32_t street_max_z = 38;
};

// Known version, ordered bounds, and bounds a float can still name exactly.
[[nodiscard]] bool procedural_config_valid(const ProceduralTerrainConfig& config) noexcept;

// Side of one tile at `lod`, in metres (64 << lod). Zero when lod is unsupported.
[[nodiscard]] std::int64_t procedural_tile_width_metres(std::uint32_t lod) noexcept;

// Can this tile be generated: lod in range and both edges within +/-kProceduralMaxWorldMetres.
[[nodiscard]] bool
procedural_tile_supported(std::int32_t tile_x, std::int32_t tile_z, std::uint32_t lod) noexcept;

// The quantised height sample at global integer metre (x, z). nullopt for an invalid config.
// nullopt too for |x| or |z| beyond 2^40 m, where the int64 lattice arithmetic stops being safe.
[[nodiscard]] std::optional<std::uint16_t>
procedural_terrain_sample(const ProceduralTerrainConfig& config,
                          std::int64_t x,
                          std::int64_t z) noexcept;

// THE VISUAL BACKDROP. Distant ridged mountain ranges for the renderer, deliberately NOT part of
// `procedural_terrain_sample`: that is the collision truth (and carries a pinned golden hash and a
// 32 m ceiling), whereas the backdrop is far-field relief the play area does not reach. Zero
// within 150 m of the street rectangle, growing to up to ~219 m of extra rise by 800 m. Pure
// integer, a function of the global metre like everything else, so every LOD agrees on it.
[[nodiscard]] std::uint32_t procedural_backdrop_rise(const ProceduralTerrainConfig& config,
                                                     std::int64_t x,
                                                     std::int64_t z) noexcept;

// Add `procedural_backdrop_rise` to every sample of a tile made by generate_procedural_tile
// (adjusts min/max_sample; clamps at the u16 ceiling). Collision tiles call this only when
// ProceduralTileSource::Options::backdrop is set.
void apply_procedural_backdrop(const ProceduralTerrainConfig& config, HeightfieldAsset& tile);

// Tile (tile_x, tile_z) at `lod`: 33 x 33 samples, spacing 2 << lod metres, origin
// (tile_x * width, 0, tile_z * width). nullopt — never an empty or flat tile — for an invalid
// config, an lod above kProceduralMaxLod, or a tile outside the supported world.
[[nodiscard]] std::optional<HeightfieldAsset>
generate_procedural_tile(const ProceduralTerrainConfig& config,
                         std::int32_t tile_x,
                         std::int32_t tile_z,
                         std::uint32_t lod);

} // namespace rime::assets
