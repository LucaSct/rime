// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <algorithm>
#include <cstdint>

#include "rime/assets/sdf_asset.hpp"

// CPU-side trilinear sampling of a cooked MeshSdfAsset (M10.4a). See sdf_asset.hpp for the grid
// layout and docs/math/sdf.md for why trilinear interpolation between voxel centres is the right
// reconstruction for a sampled distance field. Mirrors clip_sampler.cpp's split: the *_asset.hpp
// header is pure data, the behaviour that reads it lives in its own small translation unit.
namespace rime::assets {

namespace {

// Map one axis's world coordinate to a CLAMPED "continuous voxel index" in [0, res-1], where 0 is
// voxel 0's centre and res-1 is the last voxel's centre. The clamp is what makes a query past the
// grid's edge read the nearest edge value instead of extrapolating (or, worse, indexing out of
// bounds) — the field is only defined inside the cooked, padded volume (docs/math/sdf.md §4).
[[nodiscard]] float
continuous_index(float coord, float origin, float voxel_size, std::uint32_t resolution) noexcept {
    const float voxel_units = (coord - origin) / voxel_size - 0.5f;
    return std::clamp(voxel_units, 0.0f, static_cast<float>(resolution - 1));
}

} // namespace

float sample_mesh_sdf(const MeshSdfAsset& sdf, core::Vec3 point) noexcept {
    if (sdf.voxel_count() == 0 || !(sdf.voxel_size > 0.0f)) {
        return 0.0f; // an empty/degenerate volume has nothing to sample
    }

    // Continuous (fractional) voxel coordinates, clamped per axis.
    const float cx =
        continuous_index(point.x, sdf.grid_origin.x, sdf.voxel_size, sdf.resolution[0]);
    const float cy =
        continuous_index(point.y, sdf.grid_origin.y, sdf.voxel_size, sdf.resolution[1]);
    const float cz =
        continuous_index(point.z, sdf.grid_origin.z, sdf.voxel_size, sdf.resolution[2]);

    // The lattice cell containing the query: (i0,j0,k0) is the lower corner, (i1,j1,k1) the upper —
    // clamped a second time so a query landing exactly on the last voxel's centre (frac == 0) never
    // reads one voxel past the end.
    const auto ix0 = static_cast<std::uint32_t>(cx);
    const auto iy0 = static_cast<std::uint32_t>(cy);
    const auto iz0 = static_cast<std::uint32_t>(cz);
    const std::uint32_t ix1 = std::min(ix0 + 1, sdf.resolution[0] - 1);
    const std::uint32_t iy1 = std::min(iy0 + 1, sdf.resolution[1] - 1);
    const std::uint32_t iz1 = std::min(iz0 + 1, sdf.resolution[2] - 1);

    const float tx = cx - static_cast<float>(ix0);
    const float ty = cy - static_cast<float>(iy0);
    const float tz = cz - static_cast<float>(iz0);

    // Standard trilinear blend: interpolate the 8 corner samples along x, then y, then z. `at`
    // reads a corner unconditionally — every index above is already clamped into range.
    const auto at = [&sdf](std::uint32_t i, std::uint32_t j, std::uint32_t k) noexcept {
        return sdf.distances[sdf.index(i, j, k)];
    };
    const float c00 = at(ix0, iy0, iz0) * (1.0f - tx) + at(ix1, iy0, iz0) * tx;
    const float c10 = at(ix0, iy1, iz0) * (1.0f - tx) + at(ix1, iy1, iz0) * tx;
    const float c01 = at(ix0, iy0, iz1) * (1.0f - tx) + at(ix1, iy0, iz1) * tx;
    const float c11 = at(ix0, iy1, iz1) * (1.0f - tx) + at(ix1, iy1, iz1) * tx;
    const float c0 = c00 * (1.0f - ty) + c10 * ty;
    const float c1 = c01 * (1.0f - ty) + c11 * ty;
    return c0 * (1.0f - tz) + c1 * tz;
}

} // namespace rime::assets
