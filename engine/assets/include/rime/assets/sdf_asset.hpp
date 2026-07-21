// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "rime/assets/mesh_asset.hpp" // Aabb
#include "rime/core/math/vec.hpp"

// A cooked mesh signed-distance field (M10.4a, ADR-0032 §2): a per-mesh (or per-destructible-PART,
// for a coarser cook) sampled signed-distance VOLUME, built offline so the SDF-traced DDGI probes
// (m10.5) and the destruction-aware runtime clipmap (m10.4b's compose pass) have a ray-traceable
// proxy for the scene's geometry without carrying triangles onto that path. This header is the
// *cooked, CPU-resident* form the RMA1 reader hands back (see cooked_reader.hpp's
// decode_mesh_sdf/read_mesh_sdf and docs/design/assets.md for the byte layout). The runtime clipmap
// this composes into — a 3-D GPU texture, R16Snorm, rebuilt incrementally as destruction changes
// the world — is m10.4b's problem; nothing here touches the RHI or a device. The derivation (why an
// SDF, point-triangle distance, the sign problem and the angle-weighted-pseudonormal fix, the
// resolution/feature-size relationship) lives in docs/math/sdf.md.
namespace rime::assets {

// How a MeshSdf's `distances` blob is encoded on disk (the `encoding` header field). v1 always
// cooks Float32 — exact, and the cook is not the memory-constrained side (the *runtime clipmap* is,
// which is why THAT composes into a separate, compressed R16Snorm format at m10.4b — a different
// format, downstream of this one). This field exists so a future brick can cook a
// normalized/quantized encoding directly, if profiling ever shows the cooked (not the composed)
// volume needs to shrink, without a container-version bump: append a value, never renumber, and the
// reader rejects unknown values rather than misinterpreting bytes meant for a different scale.
enum class SdfEncoding : std::uint32_t {
    Float32 = 0,
};

// A cooked signed-distance volume in memory: a regular grid of `resolution[0] * resolution[1] *
// resolution[2]` samples, each the signed distance (negative INSIDE the source surface, positive
// OUTSIDE, in local-space units) from that voxel's CENTRE to the nearest point on the source
// triangle mesh. `grid_origin` is the local-space corner of voxel (0,0,0) — so voxel (i,j,k)'s
// centre sits at `grid_origin + (i+0.5, j+0.5, k+0.5) * voxel_size`, and its stored value is
// `distances[index(i,j,k)]`, with `index` walking **x fastest, then y, then z** (the same
// convention a 3-D texture upload walks — the m10.4b compose pass reads this layout directly).
// `local_bounds` is the SOURCE mesh's own tight (unpadded) AABB — handy for placement without
// recomputing it, and deliberately distinct from the grid's own (padded, voxel-quantized) extent.
// See docs/math/sdf.md for the resolution/padding policy and why sign uses angle-weighted
// pseudonormals rather than a naive nearest-triangle-normal dot.
struct MeshSdfAsset {
    Aabb local_bounds{};
    core::Vec3 grid_origin{0.0f, 0.0f, 0.0f};
    float voxel_size = 0.0f;
    std::array<std::uint32_t, 3> resolution{0, 0, 0};
    SdfEncoding encoding = SdfEncoding::Float32;
    // The largest |distance| appearing anywhere in `distances` — free metadata computed at cook
    // time (a running max alongside the sampling loop) that a future normalized/compressed
    // encoding (m10.4b's R16Snorm clipmap) uses as its reconstruction scale. The reader also uses
    // it as an integrity check: no stored sample may exceed it in magnitude.
    float max_abs_distance = 0.0f;
    std::vector<float> distances;

    [[nodiscard]] std::size_t voxel_count() const noexcept {
        return std::size_t{resolution[0]} * resolution[1] * resolution[2];
    }

    // Flatten a voxel coordinate to its index in `distances` (x fastest, then y, then z) — the one
    // true layout every reader/writer of this struct must agree on. Callers that already validated
    // i<resolution[0] etc. (the decoder does, before ever calling this) may index unconditionally.
    [[nodiscard]] std::size_t
    index(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
        return std::size_t{i} +
               std::size_t{resolution[0]} * (std::size_t{j} + std::size_t{resolution[1]} * k);
    }
};

// Trilinearly sample the field at an arbitrary local-space point. A query outside the sampled
// volume CLAMPS to the nearest edge voxel-centre lattice coordinate rather than extrapolating or
// reading out of bounds — the field is only *defined* inside `[grid_origin, grid_origin +
// resolution*voxel_size]` (padded slightly past the source mesh's surface by the cook's padding
// margin; see docs/math/sdf.md), and clamping is the honest thing to do a few voxels further out
// than that. Returns 0 for an empty (zero-resolution) volume. This is a CPU reference used by tests
// and tooling, not a hot-path/runtime API — the runtime samples its own GPU clipmap (m10.4b).
[[nodiscard]] float sample_mesh_sdf(const MeshSdfAsset& sdf, core::Vec3 point) noexcept;

} // namespace rime::assets
