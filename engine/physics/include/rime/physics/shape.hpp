// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/math/vec.hpp"

// Collision shapes (the *description* — the backend builds runtime forms later). M7.1 needs only
// enough to give a body its mass distribution; the convex-hull / triangle-mesh / compound shapes
// that destruction leans on arrive at M7.9. A shape is pure data: no GPU, no allocation.
namespace rime::physics {

// The primitive shapes v1. Hull/Mesh/Compound (the destruction shapes) land at M7.9; the enum is
// left room to grow without renumbering the primitives.
enum class ShapeType : std::uint8_t {
    Sphere = 0,
    Box = 1,
    Capsule = 2,
    // Hull, Mesh, Compound — M7.9
};

// A shape description. Only the fields relevant to `type` are read (a tagged union kept as a flat
// POD so it stays trivially copyable and reflection-friendly). Units are metres.
struct ShapeDesc {
    ShapeType type = ShapeType::Sphere;
    float radius = 0.5f;                       // Sphere, Capsule
    core::Vec3 half_extents{0.5f, 0.5f, 0.5f}; // Box (half of each side)
    float half_height = 0.5f;                  // Capsule (cylinder half-height, local Y)
};

// Mass + the body-space principal moments of inertia (diagonal). For our symmetric primitives in
// their local frame the inertia tensor is diagonal, so three numbers suffice; general hulls (M7.9)
// carry a full tensor via a principal-axis rotation.
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
// The capsule's hemispherical caps are folded in at M7.9 (their parallel-axis contribution is a
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
    }
    return mp;
}

} // namespace rime::physics
