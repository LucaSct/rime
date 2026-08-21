// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <limits>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/body.hpp"
#include "rime/physics/shape.hpp"

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

    // SELF-EXCLUSION (m12.2): a body this query pretends is not there. Null (the default) excludes
    // nothing, so every existing call site keeps its exact meaning.
    //
    // It exists because the archetypal query asker is standing in the world it is asking about. A
    // character controller's capsule IS a kinematic body — it has to be, or debris could not hit
    // the player through ordinary contact events (ADR-0035 §3) — and kinematic bodies live in the
    // dynamics tree. So every shape_cast the controller fires from its own position hits ITSELF at
    // distance 0 with `initial_overlap` set, and a controller that believes that answer spends
    // every tick depenetrating from its own body. Motion-class flags cannot express this: the
    // capsule and the crate it wants to see are the same class.
    //
    // ONE id, not a list, by choice: v1's asker is one body asking about itself, and an ignore-list
    // is speculative generality (plus a per-leaf loop on a hot path) until something needs it.
    //
    // A STALE id excludes nothing. The check is on the whole handle — index AND generation — so an
    // exclusion naming a destroyed body does not silently start hiding whatever body later reuses
    // that slot. Excluding a dead body is excluding nothing, which is exactly what it means.
    BodyId exclude{};
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

// ── Shape casts (m12.1) ───────────────────────────────────────────────────────────────────────
// A convex shape swept along a straight line: "if I slide this capsule this way, what does it hit
// first, and how far does it get?" It is the query a character controller moves with, and the one a
// ray cannot answer — a ray is infinitely thin, so it happily threads a gap a body could never fit
// through, and a controller built on rays walks through door frames it should have caught on.
//
// The technique is CONSERVATIVE ADVANCEMENT (Mirtich's, by way of Bullet's convex cast, with the
// step van den Bergen's ray cast uses). GJK measures the gap between two posed convex shapes and,
// with it, a SUPPORT PLANE the whole target provably lies behind (src/gjk.hpp — the same machinery
// M7.10's speculative CCD contacts use). If that plane is `b` away and the sweep closes it at rate
// `c`, no contact can happen before an advance of `b / c` — a bound that stays sound however noisy
// the measured direction is, because the bound and the closing rate are two readings of the SAME
// plane (the textbook form divides by the witness-normal direction instead, which m12.1 measured
// turning a noisy normal into a leap through the wall). So: measure, take the largest
// provably-safe step, measure again. It converges in a handful of iterations and needs no new
// geometry code, which is exactly why ADR-0035 §2 put a shape cast here rather than a bespoke
// swept-primitive routine per shape pair.
struct ShapeCast {
    // The shape to sweep. Any convex shape works (the algorithm only asks for support points);
    // sphere and capsule are what a character controller and a thick projectile actually use.
    // A Compound is rejected — it is not convex, so it has no single support function, and a
    // caster made of parts is not something v1 needs.
    ShapeDesc shape{};
    core::Vec3 origin{0.0f, 0.0f, 0.0f};            // where the shape's own origin starts
    core::Quat orientation = core::quat_identity(); // fixed for the whole sweep: no rotational cast
    core::Vec3 direction{0.0f, -1.0f, 0.0f};        // need not be unit; the cast normalizes it

    // How far to sweep, in metres. Unlike `Ray::max_distance` this has NO unbounded default and
    // must be finite: the broadphase step queries the swept AABB of the cast, and the swept AABB of
    // an infinite sweep is the whole world — which would turn an O(log n) query into a scan of
    // every body, silently. A non-finite or non-positive distance is rejected (the cast returns
    // false) rather than quietly clamped.
    float max_distance = 10.0f;
};

// What a shape cast hit. `distance` is how far along the normalized direction the shape travelled
// before touching, so the resting pose is `cast.origin + normalize(cast.direction) * distance`.
struct ShapeHit {
    BodyId body;
    // The witness point on the HIT BODY's surface — where the two shapes touch.
    core::Vec3 point{0.0f, 0.0f, 0.0f};
    // Outward surface normal of the hit body at that point, pointing back toward the caster — the
    // same convention `RayHit::normal` uses, and the vector a collide-and-slide step projects its
    // remaining motion onto.
    core::Vec3 normal{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;

    // TRUE when the cast shape was ALREADY intersecting this body at distance 0, before moving.
    //
    // This flag is the reason `ShapeHit` is not just a `RayHit` with a different name, and getting
    // it wrong is how character controllers acquire their famous pathologies. "I touched something
    // after moving 0 m" and "I started inside a wall" are the same number and completely different
    // situations: the first means stop, the second means the caller must DEPENETRATE first, and a
    // controller that treats the second as the first freezes solid inside the geometry it is stuck
    // in. When this is set, nothing was measured and the fields say so: `point` is the ZERO VECTOR
    // (an overlapping GJK carries no witness points) and `normal` is the reversed sweep direction
    // — a deterministic retreat, not a contact plane. Depenetration needs a penetration axis,
    // which is EPA's job (the contact pipeline), not this query's.
    bool initial_overlap = false;

    // Which compound child was touched (M8.3's convention, as `RayHit::child`): the child index
    // within the hit body's compound shape, 0 for a non-compound body. This is what lets a sweep
    // name the destructible PART it caught on.
    std::uint16_t child = 0;
};

// ── Penetration (m12.2) ───────────────────────────────────────────────────────────────────────
// What a posed shape is stuck INSIDE, and which way to move to get out. This is the query
// `ShapeHit::initial_overlap` is the signal to run: the cast can tell you that you started inside
// something, but it deliberately measures nothing about the overlap, because an overlapping GJK
// carries no witness points and a penetration AXIS is EPA's answer, not GJK's.
//
// Why a controller needs it. "Started inside" is not a hypothetical: a crate the solver pushes into
// the player between ticks, or destruction spawning debris on top of them, both produce it in an
// ordinary frame. A controller with no way to recover freezes solid — it casts, hears
// initial_overlap, has no direction to move, and does that again forever.
struct PenetrationHit {
    BodyId body;
    // Unit; the direction to push THE QUERY SHAPE to separate it (the opposite of the direction
    // you would push the hit body). Stated in the query shape's favour because the caller of this
    // query is the thing that needs to move — that is the whole point of asking.
    core::Vec3 normal{0.0f, 0.0f, 0.0f};
    float depth = 0.0f; // metres of overlap along `normal`; >= 0
    // Which compound child is the deepest overlap, by the M8.3 convention shared with
    // RayHit::child and ContactEvent::child_a/child_b. 0 for a non-compound body.
    std::uint16_t child = 0;
};

} // namespace rime::physics
