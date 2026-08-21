// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/physics/physics.hpp"

// Shared scaffolding for the m12.2 character-controller proofs.
//
// Every test in tests/gameplay drives the PUBLIC seams only — gameplay::step_character and
// physics::PhysicsWorld — and builds its geometry out of static boxes, rotated where a slope is
// wanted. Nothing here reaches under engine/physics/src; the one test in the repo that does
// (gjk_test.cpp) argues its exception at the top of that file and stays unique.
//
// The recurring trick is that box geometry has a closed form. A capsule resting on a plane sits a
// known distance from it, a plane's normal and uphill direction are trigonometry, and the speed a
// controller ought to make up a slope falls out of the projection the controller itself performs.
// So the assertions compare against arithmetic rather than against what last run happened to do.
namespace rime_test {

using namespace rime;

inline constexpr float kDt = 1.0f / 60.0f;
inline constexpr core::Vec3 kUp{0.0f, 1.0f, 0.0f};

[[nodiscard]] inline physics::ShapeDesc box(core::Vec3 half) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.half_extents = half;
    return s;
}

[[nodiscard]] inline physics::BodyId add_static_box(physics::PhysicsWorld& w,
                                                    core::Vec3 half,
                                                    core::Vec3 pos,
                                                    core::Quat q = core::quat_identity()) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape = box(half);
    d.position = pos;
    d.orientation = q;
    return w.create_body(d);
}

// ── A SIZE LIMIT ON TEST GEOMETRY, AND WHY IT IS HERE ────────────────────────────────────────
//
// Every box in this suite is kept to a half-extent of about ten metres, and that is a MEASURED
// constraint on the physics beneath, not a stylistic preference.
//
// GJK's overlap predicate loses shallow overlaps against very large convex shapes. Measured at
// m12.2 with a 0.4 m capsule sunk into a box whose top face is y = 0, asking
// PhysicsWorld::penetration() how deep it is:
//
//     box half-extent      deepest overlap MISSED ENTIRELY
//     10 m                 none — correct at every depth tried, down to 1 mm
//     20 m                 1.4 cm
//     30 m and beyond      10 cm
//
// A 200 m floor therefore swallows a character up to its knees and reports nothing. That is a
// defect in the collision core (the same family as the GJK work on #131/#132), not in the
// controller, and no amount of care above the seam can recover a contact the query denies. Sizing
// the fixtures inside the regime where the answers are trustworthy is what lets these tests assert
// the CONTROLLER's behaviour rather than the collision core's error budget; the limitation itself
// is reported upward rather than papered over.
inline constexpr float kMaxTestExtent = 10.0f;

// A floor whose TOP SURFACE is exactly y = 0 — the reference plane every height assertion in this
// suite is measured against.
inline physics::BodyId add_ground(physics::PhysicsWorld& w, float extent = kMaxTestExtent) {
    return add_static_box(w, {extent, 0.5f, extent}, {0.0f, -0.5f, 0.0f});
}

// ── Slopes ────────────────────────────────────────────────────────────────────────────────────
// A ramp is a box rotated about +Z by `theta`. That takes its top face normal from (0,1,0) to
// (-sin θ, cos θ, 0), so the surface rises toward +X and its uphill unit direction is
// (cos θ, sin θ, 0). Both are used below, and both are worth having as functions rather than as
// re-derived expressions in six test files.

[[nodiscard]] inline core::Quat slope_rotation(float theta) {
    return core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, theta);
}

[[nodiscard]] inline core::Vec3 slope_normal(float theta) {
    return core::Vec3{-std::sin(theta), std::cos(theta), 0.0f};
}

[[nodiscard]] inline core::Vec3 slope_uphill(float theta) {
    return core::Vec3{std::cos(theta), std::sin(theta), 0.0f};
}

// A ramp of inclination `theta` whose surface passes exactly through the world origin. Derived,
// not tuned: the box's top face sits `half.y` along its own +Y, which after the rotation is
// `half.y * n`, so putting the face through the origin means placing the body at `-half.y * n`.
inline physics::BodyId add_ramp(physics::PhysicsWorld& w, float theta, core::Vec3 half) {
    const core::Vec3 n = slope_normal(theta);
    return add_static_box(w, half, n * -half.y, slope_rotation(theta));
}

// The PERPENDICULAR clearance the controller settles at: it sweeps a capsule inflated by `skin`
// and stops half an offset short of contact, so the real capsule comes to rest 1.5 offsets clear
// of whatever it is standing on, measured perpendicular to that surface at any angle.
[[nodiscard]] inline float rest_clearance(const gameplay::CharacterConfig& cfg) {
    return 1.5f * cfg.skin;
}

// Where a capsule of `cfg` RESTS on the ramp above the ground point (x, z) — the pose the
// controller's own ground snap converges to, so a character placed here neither sinks nor is
// yanked on tick one.
[[nodiscard]] inline core::Vec3
rest_on_slope(const gameplay::CharacterConfig& cfg, float theta, float x, float z) {
    // The bottom cap's centre sits (radius + clearance) from the plane along n = (-sinθ, cosθ, 0):
    //     -x sin θ + (y - half_height) cos θ == radius + clearance
    const float y = (cfg.radius + rest_clearance(cfg) + x * std::sin(theta)) / std::cos(theta) +
                    cfg.half_height;
    return core::Vec3{x, y, z};
}

// …and the same on flat ground, where it is just the capsule's half-height plus the clearance.
[[nodiscard]] inline float rest_y(const gameplay::CharacterConfig& cfg, float surface_y = 0.0f) {
    return surface_y + cfg.half_height + cfg.radius + rest_clearance(cfg);
}

// ── The character under test ──────────────────────────────────────────────────────────────────
//
// A real kinematic body is created for the character, exactly as a game would have one (ADR-0035
// §3: debris must be able to hit the player through ordinary contact events). It is kept in sync
// with the controller's own position each tick, which is what makes `QueryFilter::exclude` load
// bearing here rather than theoretical — the depenetration query runs with dynamics enabled, and
// without the exclusion the character would find ITSELF deeply overlapping every tick.
struct Character {
    physics::PhysicsWorld* world = nullptr;
    gameplay::CharacterConfig config{};
    gameplay::CharacterState state{};
    physics::BodyId body{};
    gameplay::StepStats stats{}; // accumulated across every tick this character has taken

    void spawn(physics::PhysicsWorld& w,
               const gameplay::CharacterConfig& cfg,
               core::Vec3 position,
               bool grounded = false) {
        world = &w;
        config = cfg;
        (void)gameplay::validate(config);
        state = gameplay::CharacterState{};
        state.position = position;
        state.grounded = grounded;

        physics::BodyDesc d;
        d.motion = physics::MotionType::Kinematic;
        d.shape = gameplay::character_shape(config);
        d.position = position;
        body = w.create_body(d);
    }

    // One tick: step the controller, then push the resulting pose into the kinematic body so the
    // world the NEXT tick queries agrees with where the character actually is.
    void tick(const gameplay::CharacterInput& input) {
        state = gameplay::step_character(state, input, config, *world, body, kDt, &stats);
        physics::BodyState bs;
        if (world->get_body_state(body, bs)) {
            bs.position = state.position;
            (void)world->set_body_state(body, bs);
        }
    }

    void tick_n(const gameplay::CharacterInput& input, int n) {
        for (int i = 0; i < n; ++i) {
            tick(input);
        }
    }

    // "Am I inside anything?", answered through the public seam. penetration() returns false
    // exactly when the posed capsule is clear of every body the filter admits — which is the
    // property the grid test asserts thousands of times and the recovery test watches converge.
    [[nodiscard]] bool overlapping() const {
        physics::QueryFilter f;
        f.exclude = body;
        physics::PenetrationHit hit;
        return world->penetration(
            gameplay::character_shape(config), state.position, core::quat_identity(), hit, f);
    }

    // The lowest point of the capsule — what "standing on y = 0" actually means for a capsule
    // whose position is its centre.
    [[nodiscard]] float feet_y() const {
        return state.position.y - config.half_height - config.radius;
    }
};

// ── Input helpers ─────────────────────────────────────────────────────────────────────────────
// The mover's basis: at yaw = 0, +move_x is world +X and +move_y is world -Z (the engine's
// forward, render/components.hpp). These build the two cases every test needs.

[[nodiscard]] inline gameplay::CharacterInput walk(float move_x, float move_y, float yaw = 0.0f) {
    gameplay::CharacterInput in;
    in.move_x = move_x;
    in.move_y = move_y;
    in.yaw = yaw;
    return in;
}

[[nodiscard]] inline gameplay::CharacterInput idle() {
    return gameplay::CharacterInput{};
}

[[nodiscard]] inline gameplay::CharacterInput jump(float move_x = 0.0f, float move_y = 0.0f) {
    gameplay::CharacterInput in = walk(move_x, move_y);
    in.pressed = gameplay::kActionJump;
    in.held = gameplay::kActionJump;
    return in;
}

// The ground-plane part of a vector — what "did it stop moving" means for a character that may
// legitimately have a vertical component from a slope or a fall.
[[nodiscard]] inline core::Vec3 horizontal_of(core::Vec3 v) {
    return core::Vec3{v.x, 0.0f, v.z};
}

[[nodiscard]] inline bool finite(core::Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace rime_test
