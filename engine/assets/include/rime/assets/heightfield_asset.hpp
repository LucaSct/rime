// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/vec.hpp"

// A cooked terrain HEIGHTFIELD (M19.1, ADR-0056-m19.1-heightfield): a regular grid of height
// samples over the local XZ plane — the one piece of data terrain collision (engine/physics), and
// later terrain rendering, splat blending and streaming, all read. This header is the cooked,
// CPU-resident form the RMA1 reader hands back (cooked_reader.hpp: decode_heightfield /
// read_heightfield). The byte layout is in tools/asset-pipeline/FORMAT.md; the Rust cooker
// (tools/asset-pipeline/src/heightfield.rs) is its writer of record.
//
// WHY THE HEIGHTS ARE QUANTISED u16 WITH A SCALE IN THE HEADER (the decision the ADR records):
//
//   height(i, j) = height_offset + height_scale * float(samples[i + columns * j])
//
//  - It is what terrain SOURCES already are. Terrain is authored and exchanged as 16-bit
//    grayscale (World Machine, Gaea, Houdini, every DEM tool's PNG/RAW export). Storing u16 means
//    the cook re-quantises nothing: the cooked sample IS the source sample, so the cook is exact
//    and trivially deterministic, and the only quantisation error in the whole pipeline is the one
//    the artist's tool already made.
//  - It is half the bytes of f32, and terrain is the largest single data set a world streams.
//  - It is UNIFORM precision: every metre of altitude gets the same step, height_scale. f16 would
//    halve memory too, but its step grows with magnitude (a 2 km peak would be quantised to 1 m).
//  - The renderer can upload `samples` directly as an R16_UNORM texture and apply the same
//    scale/offset, so the surface the player SEES and the surface bodies COLLIDE with are built
//    from the identical integers — no second, drifting copy of the terrain.
//
// The price is a bounded vertical error: a surface quantised to u16 over a height range R has a
// step of R / 65535 and a worst-case rounding error of half that (7.6 mm for a full 1 km range,
// 1.5 mm for a 200 m one). tests/physics/heightfield_test.cpp derives and checks that bound.
namespace rime::assets {

// How each grid cell is split into two triangles. The TRIANGULATION IS PART OF THE FORMAT, not an
// implementation detail of whoever reads it: a non-planar cell (four corners not coplanar) has two
// different surfaces depending on which diagonal splits it, and physics and rendering must pick the
// same one or a ball visibly rests above or below the drawn ground. v1 has exactly one convention;
// the field exists so a later cook can add, say, per-cell "best fit" diagonals as an appended value
// (append, never renumber — the reader rejects values it does not know).
enum class HeightfieldTriangulation : std::uint32_t {
    // Every cell (i, j) is split along the diagonal from sample (i, j) to sample (i+1, j+1) — the
    // "minimum corner to maximum corner" diagonal, u == v in the cell's unit coordinates.
    DiagonalMinToMax = 0,
};

// A cooked heightfield in memory. Sample (i, j) — i along local +X in [0, columns), j along local
// +Z in [0, rows) — sits at local position (i * cell_size_x, height(i, j), j * cell_size_z);
// `samples` is ROW-MAJOR, x fastest (i + columns * j), the same walk a 2-D texture upload makes.
// `origin` is where the authoring sidecar placed local (0, 0, 0) in the world; a physics body or a
// render instance is positioned there.
struct HeightfieldAsset {
    std::uint32_t columns = 0; // samples along local X (>= 2: at least one cell)
    std::uint32_t rows = 0;    // samples along local Z (>= 2)
    float cell_size_x = 0.0f;  // metres between neighbouring samples along X (> 0)
    float cell_size_z = 0.0f;  // metres between neighbouring samples along Z (> 0)
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    float height_scale = 0.0f;  // metres per quantisation step (> 0)
    float height_offset = 0.0f; // metres at sample value 0
    HeightfieldTriangulation triangulation = HeightfieldTriangulation::DiagonalMinToMax;
    // The smallest and largest sample anywhere in `samples` — cook-time metadata (a running
    // min/max) that gives the vertical bounds without a scan, and that the reader turns into an
    // exact integrity check: no sample may fall outside it.
    std::uint16_t min_sample = 0;
    std::uint16_t max_sample = 0;
    std::vector<std::uint16_t> samples;

    [[nodiscard]] std::size_t sample_count() const noexcept {
        return std::size_t{columns} * std::size_t{rows};
    }

    [[nodiscard]] std::size_t index(std::uint32_t i, std::uint32_t j) const noexcept {
        return std::size_t{i} + std::size_t{columns} * std::size_t{j};
    }

    // Dequantised height of sample (i, j), in metres (local frame). Callers index in range.
    [[nodiscard]] float height(std::uint32_t i, std::uint32_t j) const noexcept {
        return height_offset + height_scale * static_cast<float>(samples[index(i, j)]);
    }
};

} // namespace rime::assets
