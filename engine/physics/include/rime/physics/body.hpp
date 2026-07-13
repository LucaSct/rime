// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/containers/handle.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/shape.hpp"

// Rigid bodies: the description you create one from, the opaque id you get back, and the state you
// read out. All POD, all in world units (metres, kilograms, seconds). The runtime storage is a
// data-oriented SoA pool hidden behind PhysicsWorld — a BodyId never dereferences a pointer.
namespace rime::physics {

// Phantom tag so a BodyId can't be confused with any other engine handle at compile time.
struct BodyTag {};

// A generational handle into the body pool: stable across the pool relocating its arrays, and
// use-after-free-detecting (a destroyed slot bumps its generation, so a stale BodyId reads as dead
// rather than aliasing whatever body reused the slot — the churn safety destruction relies on).
using BodyId = core::Handle<BodyTag>;

// How a body participates in simulation.
//   Static    — never moves; infinite mass; the world/anchors (M8's intact walls).
//   Kinematic — moved by the game (its transform is pushed in), pushes dynamics, is not pushed
//   back. Dynamic   — fully simulated: gravity, forces, contacts.
enum class MotionType : std::uint8_t {
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
};

// Everything needed to create a body. Mass is explicit; the shape supplies the inertia
// *distribution* (via compute_mass_properties). Static/Kinematic bodies are treated as
// infinite-mass regardless of `mass`. Damping models air drag / numerical bleed; gravity_factor
// scales this body's gravity (0 = floats, 1 = normal). Friction/restitution are the solver's
// contact materials (M7.4); across a touching pair they combine as μ = √(μ_a·μ_b) and
// e = max(e_a, e_b) — see docs/math/sequential-impulse.md.
struct BodyDesc {
    MotionType motion = MotionType::Dynamic;
    ShapeDesc shape{};
    float mass = 1.0f;

    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Quat orientation = core::quat_identity();
    core::Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    core::Vec3 angular_velocity{0.0f, 0.0f, 0.0f};

    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    float gravity_factor = 1.0f;

    float friction = 0.5f;    // Coulomb μ — used by the M7.4 solver
    float restitution = 0.0f; // bounciness — used by the M7.4 solver
};

// A snapshot of a body's motion state, read out of the pool.
struct BodyState {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Quat orientation = core::quat_identity();
    core::Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    core::Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
};

} // namespace rime::physics
