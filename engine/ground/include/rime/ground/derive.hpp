// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/vec.hpp"
#include "rime/ground/surface.hpp"
#include "rime/physics/shape.hpp"
#include "rime/render/mesh.hpp"

// The two functions allowed to decide the ground's geometry, and the one that reports its size.
//
// EVERYTHING that needs to know how big the ground is goes through `half_extents` — the drawn mesh,
// the collider, and the GI proxy that traces it. That is the entire mechanism by which the three
// disagreeing extents in `99-the-block` become one number, and it is why these are free functions
// on the authored struct rather than methods that could be bypassed.
//
// Both derivations are PURE and GPU-free: same input, same bytes, on every peer. That matters
// because level geometry is stood up independently by the server and by each client rather than
// replicated (the split `99-the-block` and `13-networked-player` both make), so two peers agreeing
// about where the floor is depends on the derivation being a function rather than a procedure.
namespace rime::ground {

// Half-extents in the surface's own frame: {half_x, thickness/2, half_z}. The y is the COLLIDER's
// half-depth, not a visual height — the drawn surface is the slab's top face at local y = 0.
[[nodiscard]] core::Vec3 half_extents(const GroundSurface& s) noexcept;

// The drawn surface: a `cells_x` x `cells_z` grid of quads over the extent at local y = 0, +y
// normals, +x tangents. uv is METRE-BASED — `(local_xz + half) / tile_metres` — so the texture's
// scale on the ground is a property of the surface and not of how large the surface happens to be.
// That is the fix for the old plane, whose `uv_tiles = 24` spread 24 repeats across whatever span
// it was scaled to, so making the street bigger silently stretched its (absent) texture.
//
// Tessellated even though it is flat: a flat grid is a heightfield whose heights are zero, so M18
// changes this function's body and nothing else. It also makes the cost of a tessellated ground a
// number the perf run can measure today rather than discover later.
[[nodiscard]] render::CpuMesh derive_mesh(const GroundSurface& s);

// The collider, and its offset from the entity's origin. A `ShapeDesc` rather than bare
// half-extents because that is already the tagged union that grows by world-owned store id
// (`HullId`, `CompoundId`), which is the shape a future heightfield collider must take —
// `physics::Collider`, the authored component, carries primitives only and could not express one.
struct GroundCollider {
    physics::ShapeDesc shape{};
    core::Vec3 offset{0.0f, 0.0f, 0.0f}; // surface origin -> body centre, in the surface's frame
};

[[nodiscard]] GroundCollider derive_collider(const GroundSurface& s) noexcept;

} // namespace rime::ground
