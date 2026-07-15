// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/core/math/vec.hpp"

// A destructible / fracture pattern (M8.1, ADR-0029): a source shape pre-split into convex
// **parts**, the **bond** graph that glues neighbours, and the **anchors** that pin parts to the
// world. This is the *cooked runtime form* the Rust fracture cook writes and `engine/destruction`
// (M8.2) instances: each part is registered as a convex hull (ADR-0027), the whole as one compound
// body (ADR-0028); on fracture, unsupported parts detach into debris bodies. Geometry only — no
// health/simulation state (that lives on the runtime instance). The C++ reader
// (`decode_destructible`) is the exact counterpart of the cooker's encoder; the byte layout is in
// `docs/design/assets.md`.
namespace rime::assets {

// One convex part: its collision hull (COM-centred CSR geometry, ready to hand straight to
// `PhysicsWorld::register_hull`), where that hull sits in the destructible's frame (`com` — the
// compound child's translation, and the pivot the vertices are stored relative to), its world-frame
// AABB (the set a radius-damage query tests), and its volume (mass fraction under uniform density).
struct DestructiblePart {
    core::Vec3 com{0.0f, 0.0f, 0.0f};
    core::Vec3 aabb_min{0.0f, 0.0f, 0.0f};
    core::Vec3 aabb_max{0.0f, 0.0f, 0.0f};
    float volume = 0.0f;

    // Hull geometry in `HullDesc` shape: `vertices` are COM-centred; `face_counts[f]` is the number
    // of vertices of face f (each in 3..=16), whose indices are the next `face_counts[f]` entries
    // of `face_indices`, wound outward. register_hull(HullDesc{vertices, face_counts,
    // face_indices}) consumes them directly.
    std::vector<core::Vec3> vertices;
    std::vector<std::uint32_t> face_counts;
    std::vector<std::uint32_t> face_indices;
};

// A bond: two parts share a face (are Voronoi-adjacent), glued with a strength proportional to the
// shared area. Stored once per pair with a < b. Damage removes bonds; a connectivity solve over the
// live bonds finds parts no longer connected to an anchor (they detach).
struct DestructibleBond {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    float strength = 0.0f;
};

// A whole cooked destructible.
struct DestructibleAsset {
    core::Vec3 half_extents{0.0f, 0.0f, 0.0f}; // the source box (render/reference)
    // Pattern-wide damage material (uniform in v1, ADR-0029 §3): impulse absorbed before a part
    // erodes, and damage per unit impulse above it.
    float damage_threshold = 0.0f;
    float damage_scale = 0.0f;

    std::vector<DestructiblePart> parts;
    std::vector<DestructibleBond> bonds;
    std::vector<std::uint32_t> anchors; // indices into `parts`

    [[nodiscard]] std::size_t part_count() const noexcept { return parts.size(); }
};

} // namespace rime::assets
