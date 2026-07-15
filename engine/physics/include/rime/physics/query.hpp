// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <limits>

#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"

// Scene queries (M7.7): the world becomes *askable*. A raycast is the workhorse — hitscan weapons,
// line-of-sight, mouse picking (M9), AI probes, the "what's under the crosshair" the physics
// playground fires along. Overlap answers "what is inside this volume" (explosion radius, trigger
// pre-check). Both run through the same broadphase BVH the collision pipeline uses (one structure,
// several customers — see src/aabb_tree.hpp), so a query is O(log n), not a scan of every body.
//
// These are the *description* types; the query methods live on PhysicsWorld (world.hpp). All are
// read-only and const — a query never mutates the simulation — so they are safe to call between
// steps (not concurrently with step(); the threading contract is documented on the methods).
namespace rime::physics {

// A ray to cast. `direction` need not be unit length — the cast normalizes it and reports the hit
// distance in world units (metres) along it, so callers can pass a raw "look" vector.
// `max_distance` bounds the cast; the default is effectively unbounded.
struct Ray {
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
    float max_distance = std::numeric_limits<float>::max();
};

// Which bodies a query considers, by motion class. Defaults include everything; clear a flag to,
// say, raycast only the movable world (a projectile that ignores the level geometry) or only the
// static world (a grounded-ness probe). `dynamics` covers both Dynamic and Kinematic bodies — the
// broadphase keeps those two in one tree and the static world in the other, so a filter that drops
// one class simply skips that tree.
struct QueryFilter {
    bool statics = true;
    bool dynamics = true;
};

// The nearest thing a raycast hit. `distance` is measured from the ray origin along the normalized
// direction, so `point == ray.origin + normalize(ray.direction) * distance`. `normal` is the
// outward surface normal at the hit (points back toward the ray for an exterior hit).
struct RayHit {
    BodyId body;
    core::Vec3 point{0.0f, 0.0f, 0.0f};
    core::Vec3 normal{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
    // Which compound child the ray pierced (M8.3, ADR-0029): the same convention as
    // ContactEvent::child_a/child_b — the child index within the hit body's compound shape, 0 for
    // a non-compound body. This is what lets hitscan name the destructible PART it hit (child
    // index == part index on an intact destructible), exactly as contact events already do for
    // impacts. On an exact tie between children the lowest index wins (the compound raycast's
    // strict-< scan), so the answer is deterministic.
    std::uint16_t child = 0;
};

} // namespace rime::physics
