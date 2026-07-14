// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.10 proofs: continuous collision detection via speculative contacts. The headline is the
// DISCRIMINATOR — a body fast enough to cross a thin wall in one step tunnels straight through with
// CCD off and is stopped at the surface with it on — which is the whole reason the feature exists.
// The rest guard the seams: CCD is inert for ordinary (overlapping) contacts, a CCD stop surfaces
// as a contact event carrying the hit's impulse (the M8 damage path), and the determinism contract
// (bit-identical across worker counts) still holds with a CCD body in the scene. All pure CPU.
using namespace rime;

namespace {

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

struct BodyParams {
    physics::MotionType motion = physics::MotionType::Dynamic;
    core::Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    bool ccd = false;
    float restitution = 0.0f;
};

physics::BodyId add_body(physics::PhysicsWorld& w,
                         const physics::ShapeDesc& shape,
                         core::Vec3 pos,
                         const BodyParams& params = {}) {
    physics::BodyDesc d;
    d.motion = params.motion;
    d.shape = shape;
    d.position = pos;
    d.linear_velocity = params.linear_velocity;
    d.ccd = params.ccd;
    d.restitution = params.restitution;
    return w.create_body(d);
}

physics::BodyId
add_static(physics::PhysicsWorld& w, const physics::ShapeDesc& shape, core::Vec3 pos) {
    BodyParams params;
    params.motion = physics::MotionType::Static;
    return add_body(w, shape, pos, params);
}

physics::BodyState state_of(const physics::PhysicsWorld& w, physics::BodyId id) {
    physics::BodyState s{};
    REQUIRE(w.get_body_state(id, s));
    return s;
}

constexpr float kDt = 1.0f / 60.0f;

// Fire a small sphere at a thin static wall along +Z, gravity off to isolate the tunnelling. The
// wall is 10 cm thick (half-extent 0.05 in Z) at the origin; the sphere starts 1 m in front of it.
// A speed of 100 m/s is ~1.67 m/step at 60 Hz — an order of magnitude past the wall thickness, so a
// discrete overlap test never catches it. Returns the sphere's final position.
core::Vec3 fire_at_wall(bool ccd, float speed, int ticks) {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    (void)add_static(w, box({2.0f, 2.0f, 0.05f}), {0.0f, 0.0f, 0.0f});
    BodyParams p;
    p.linear_velocity = {0.0f, 0.0f, speed};
    p.ccd = ccd;
    const physics::BodyId proj = add_body(w, sphere(0.1f), {0.0f, 0.0f, -1.0f}, p);
    for (int k = 0; k < ticks; ++k) {
        w.step(kDt);
    }
    return state_of(w, proj).position;
}

} // namespace

TEST_CASE("CCD: a fast projectile tunnels a thin wall without CCD and is stopped with it") {
    const float speed = 100.0f; // ~1.67 m/step vs a 0.10 m wall — pure tunnelling territory

    // Without CCD the sphere is never sampled overlapping the wall, so nothing stops it: it ends up
    // clean on the far side and keeps going.
    const core::Vec3 through = fire_at_wall(false, speed, 5);
    CHECK(through.z > 0.5f);

    // With CCD the velocity-swept broadphase bound reports the wall in its path and a speculative
    // contact arrests it AT the near face (z = -0.05) minus the 0.1 m radius ≈ -0.15 — it never
    // crosses to the far side, and with no restitution it does not rebound away either.
    const core::Vec3 stopped = fire_at_wall(true, speed, 5);
    CHECK(stopped.z < 0.0f);
    CHECK(stopped.z > -0.35f);
}

TEST_CASE("CCD is inert for an ordinary overlapping contact") {
    // A CCD box dropped a short distance onto the ground must rest exactly where a normal box does:
    // the speculative path only fires for SEPARATED approaching pairs, so once shapes actually
    // overlap the M7.4 solver owns the contact unchanged. Same analytic rest height as the solver
    // test's dropped box (ground top 0.5 + half 0.5 − 5 mm slop ≈ 0.995).
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    BodyParams p;
    p.ccd = true;
    const physics::BodyId cube = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.2f, 0.0f}, p);

    for (int k = 0; k < 240; ++k) {
        w.step(kDt);
    }
    const physics::BodyState s = state_of(w, cube);
    CHECK(s.position.y == doctest::Approx(0.995f).epsilon(0.01));
    CHECK(core::length(s.linear_velocity) <= 0.02f);
    CHECK(std::fabs(s.position.x) <= 0.01f); // no lateral drift from a phantom speculative shove
    CHECK(std::fabs(s.position.z) <= 0.01f);
}

TEST_CASE("a CCD stop surfaces as a contact event carrying the hit's impulse") {
    // CCD composes with the M7.9 event stream: arresting a fast body is a real contact, so it emits
    // a contact event whose normal impulse is the momentum the wall took out of the projectile —
    // the signal M8 destruction turns into damage. (100 m/s on a 1 kg sphere ⇒ a large impulse.)
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::BodyId wall = add_static(w, box({2.0f, 2.0f, 0.05f}), {0.0f, 0.0f, 0.0f});
    BodyParams p;
    p.linear_velocity = {0.0f, 0.0f, 100.0f};
    p.ccd = true;
    const physics::BodyId proj = add_body(w, sphere(0.1f), {0.0f, 0.0f, -1.0f}, p);

    float peak_impulse = 0.0f;
    for (int k = 0; k < 5; ++k) {
        w.step(kDt);
        for (const physics::ContactEvent& e : w.contact_events()) {
            if (e.a.index == wall.index && e.b.index == proj.index) {
                peak_impulse = std::max(peak_impulse, e.normal_impulse);
            }
        }
    }
    CHECK(peak_impulse > 1.0f); // a genuine hit, not a graze
}

namespace {

// A scene with two separated resting stacks (two islands) plus a CCD sphere fired across the floor
// into a thin wall — enough islands to exercise the parallel solve, with the CCD path live.
void build_ccd_scene(physics::PhysicsWorld& w) {
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({20.0f, 0.5f, 20.0f}), {0.0f, 0.0f, 0.0f});
    (void)add_static(
        w, box({0.1f, 2.0f, 4.0f}), {6.0f, 1.0f, 0.0f}); // the wall the projectile hits
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {-6.0f, 1.05f, 0.0f});
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {-6.03f, 2.08f, 0.02f});
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 6.0f});
    BodyParams p;
    p.linear_velocity = {80.0f, 0.0f, 0.0f};
    p.ccd = true;
    (void)add_body(w, sphere(0.2f), {0.0f, 1.2f, 0.0f}, p);
}

} // namespace

TEST_CASE("a CCD scene is bit-identical across worker counts") {
    // The determinism contract (ADR-0026), with CCD live: speculative contacts come from the
    // canonical narrowphase and a per-body flag, and the swept proxy refit is sequential, so
    // nothing CCD adds is thread-order-dependent. workers < 0 is the sequential reference path.
    auto run_hash = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w;
        build_ccd_scene(w);
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        bool multi_island = false;
        for (int k = 0; k < 200; ++k) {
            w.step(kDt);
            if (w.islands_last() > 1) {
                multi_island = true;
            }
        }
        CHECK(multi_island);
        return w.world_hash();
    };

    const std::uint64_t sequential = run_hash(-1);
    CHECK(run_hash(1) == sequential);
    CHECK(run_hash(2) == sequential);
    CHECK(run_hash(4) == sequential);
}
