// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/containers/handle.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"

// Collision shapes (the *description* — the backend builds runtime forms later). M7.1 needs only
// enough to give a body its mass distribution; convex hulls (the destruction shape) landed at
// M7.11 (ADR-0027) and compounds (many convex children on one body — the intact destructible) at
// M7.12 (ADR-0028), with the static triangle mesh still to come. A shape is pure data: no GPU, no
// allocation.
namespace rime::physics {

// Phantom tag so a HullId can't be confused with any other engine handle at compile time.
struct HullTag {};

// Handle to a convex hull registered with a PhysicsWorld (ADR-0027: hull geometry is WORLD-OWNED —
// vertices/faces are registered once via PhysicsWorld::register_hull and referenced by this small
// id, which is what keeps ShapeDesc a flat POD despite hulls being variable-length). Default
// constructed ⇒ null (refers to no hull). Ids are only meaningful to the world that issued them.
using HullId = core::Handle<HullTag>;

// Phantom tag for CompoundId — same discipline as HullTag.
struct CompoundTag {};

// Handle to a compound shape registered with a PhysicsWorld (ADR-0028: the child list is
// WORLD-OWNED, registered once via PhysicsWorld::register_compound — the exact storage answer
// hulls got, for the exact same reason: a compound is variable-length data and ShapeDesc is a
// flat POD). Default constructed ⇒ null. Ids are only meaningful to the world that issued them.
using CompoundId = core::Handle<CompoundTag>;

// The shape set: the v1 primitives, the convex hull (M7.11), and the compound (M7.12). The static
// triangle mesh (world geometry) is still to land; the enum leaves room to grow without
// renumbering.
enum class ShapeType : std::uint8_t {
    Sphere = 0,
    Box = 1,
    Capsule = 2,
    ConvexHull = 3, // geometry lives in the world's hull store; `hull` is the reference
    Compound = 4,   // child list lives in the world's compound store; `compound` is the reference
    // Mesh — deferred (ADR-0027 names its home)
};

// A shape description. Only the fields relevant to `type` are read (a tagged union kept as a flat
// POD so it stays trivially copyable and reflection-friendly). Units are metres.
struct ShapeDesc {
    ShapeType type = ShapeType::Sphere;
    float radius = 0.5f;                       // Sphere, Capsule
    core::Vec3 half_extents{0.5f, 0.5f, 0.5f}; // Box (half of each side)
    float half_height = 0.5f;                  // Capsule (cylinder half-height, local Y)
    HullId hull{};                             // ConvexHull (PhysicsWorld::register_hull)
    CompoundId compound{};                     // Compound (PhysicsWorld::register_compound)
};

// One child of a compound shape (M7.12, ADR-0028): a convex shape — any primitive or a registered
// hull, but NOT another compound (nesting is rejected in v1; flatten-at-register is its deferred
// home) — posed in the compound's AUTHORED frame. Registration re-centres the stored poses on the
// combined centre of mass, so author children wherever is natural and read the applied shift back
// from CompoundInfo::centroid (exactly the hull re-centring contract).
struct CompoundChildDesc {
    ShapeDesc shape{};
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Quat orientation = core::quat_identity();
};

// Mass + the body-space principal moments of inertia (diagonal). For our symmetric primitives in
// their local frame the inertia tensor is diagonal, so three numbers suffice; a general hull's
// full tensor is diagonalized at registration and carried as these principal moments plus a
// per-body principal-axis rotation inside the world (M7.11, ADR-0027) — this POD stays diagonal.
struct MassProperties {
    float mass = 1.0f;
    core::Vec3 inertia_diagonal{1.0f, 1.0f, 1.0f};
};

// Analytic solid-body inertia for a uniform-density shape of total `mass`. Derivations (the
// standard integrals ∫ r² dm over each solid) live in docs/math/rigid-body-dynamics.md.
//
//   Sphere (radius r):            I = 2/5 · m · r²           (all axes)
//   Box    (half-extents h):      Iₓ = 1/12 · m · ((2h_y)² + (2h_z)²) = 1/3 · m · (h_y² + h_z²)
//   Capsule ≈ cylinder v1 (axis = local Y, radius r, half-height hh):
//                                 I_y = 1/2 · m · r²,   Iₓ = I_z = 1/12 · m · (3r² + (2hh)²)
// The capsule's hemispherical caps are folded in later (their parallel-axis contribution is a
// small correction; the cylinder body dominates); v1 documents the approximation rather than hiding
// it.
[[nodiscard]] inline MassProperties compute_mass_properties(const ShapeDesc& s,
                                                            float mass) noexcept {
    MassProperties mp;
    mp.mass = mass;
    switch (s.type) {
        case ShapeType::Sphere: {
            const float i = 0.4f * mass * s.radius * s.radius;
            mp.inertia_diagonal = {i, i, i};
            break;
        }
        case ShapeType::Box: {
            const float hx = s.half_extents.x, hy = s.half_extents.y, hz = s.half_extents.z;
            const float k = mass / 3.0f;
            mp.inertia_diagonal = {
                k * (hy * hy + hz * hz), k * (hx * hx + hz * hz), k * (hx * hx + hy * hy)};
            break;
        }
        case ShapeType::Capsule: {
            const float r = s.radius, hh = s.half_height;
            const float i_axis = 0.5f * mass * r * r; // about the local-Y cylinder axis
            const float i_perp = (mass / 12.0f) * (3.0f * r * r + 4.0f * hh * hh);
            mp.inertia_diagonal = {i_perp, i_axis, i_perp};
            break;
        }
        case ShapeType::ConvexHull:
            // A hull's mass properties come from geometry this shape-only helper cannot see: the
            // world owns the hull store (ADR-0027) and computes them at register_hull() by the
            // polyhedral integral (docs/math/polyhedral-mass-properties.md). create_body resolves
            // the id there; this fallback (unit inertia) is only what a caller with no world gets.
            break;
        case ShapeType::Compound:
            // Same story as the hull: the child list lives in the world's compound store
            // (ADR-0028), which composed the combined COM/inertia at register_compound() by the
            // parallel-axis theorem (docs/math/compound-mass-properties.md). create_body resolves
            // the id there; a caller with no world gets the unit-inertia fallback.
            break;
    }
    return mp;
}

} // namespace rime::physics
