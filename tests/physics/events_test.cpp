// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "rime/core/hash.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.9 proofs: step() reports its contact and sleep events as buffered, deterministic data
// (events.hpp) — the M8 damage input. All pure-CPU and structural in the house pattern: the
// contact lifecycle (began → persisted → ended) observed on a landing box, the resting event's
// impulse tied to the same m·g·dt closed form the solver test reads, sleep/wake transitions
// evented exactly once, a settled pile going silent, and — the headline — the whole event STREAM
// bit-identical across worker counts (the M7.5 determinism thesis, now over the events too).
// Everything drives the public PhysicsWorld seam.
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
    float friction = 0.5f;
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
    d.friction = params.friction;
    d.restitution = params.restitution;
    return w.create_body(d);
}

physics::BodyId
add_static(physics::PhysicsWorld& w, const physics::ShapeDesc& shape, core::Vec3 pos) {
    BodyParams params;
    params.motion = physics::MotionType::Static;
    return add_body(w, shape, pos, params);
}

bool same_body(physics::BodyId a, physics::BodyId b) {
    return a.index == b.index && a.generation == b.generation;
}

float state_of_y(const physics::PhysicsWorld& w, physics::BodyId id) {
    physics::BodyState s{};
    REQUIRE(w.get_body_state(id, s));
    return s.position.y;
}

constexpr float kDt = 1.0f / 60.0f;

} // namespace

TEST_CASE("a landing box reports Began, then Persisted, carrying the resting support impulse") {
    // Sleeping OFF so the resting contact stays in continuous solve and its event never goes silent
    // — this test is about the impulse payload, not deactivation (that is the next test).
    physics::PhysicsWorld w;
    w.set_sleeping_enabled(false);
    w.set_gravity({0.0f, -9.81f, 0.0f});
    const physics::BodyId floor = add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    const physics::BodyId cube = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.2f, 0.0f});

    // Fall (20 cm) and catch the first-contact tick. The pair is canonical: the floor was created
    // first (lower slot) so it is `a`, the cube is `b`, and the normal points a → b = straight up.
    int began_tick = -1;
    for (int k = 0; k < 60 && began_tick < 0; ++k) {
        w.step(kDt);
        const std::span<const physics::ContactEvent> ev = w.contact_events();
        if (!ev.empty()) {
            began_tick = k;
            REQUIRE(ev.size() == 1);
            CHECK(ev[0].phase == physics::ContactPhase::Began);
            CHECK(same_body(ev[0].a, floor));
            CHECK(same_body(ev[0].b, cube));
            CHECK(ev[0].normal.y > 0.9f);       // a(floor) → b(cube) is up
            CHECK(ev[0].normal_impulse > 0.0f); // an impact transfers momentum
        }
    }
    REQUIRE(began_tick >= 0);

    // The very next tick the same pair is still touching → Persisted, not a second Began.
    w.step(kDt);
    {
        const std::span<const physics::ContactEvent> ev = w.contact_events();
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].phase == physics::ContactPhase::Persisted);
    }

    // At rest the solver supplies exactly one tick of gravity to hold the box up, so the event's
    // total normal impulse == m·g·dt = 1 · 9.81 / 60 = 0.1635 N·s — the identical quantity the
    // solver test reads out of the warm-start cache, here surfaced as an event payload.
    for (int k = 0; k < 300; ++k) {
        w.step(kDt);
    }
    const std::span<const physics::ContactEvent> ev = w.contact_events();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].phase == physics::ContactPhase::Persisted);
    CHECK(ev[0].normal_impulse == doctest::Approx(9.81f / 60.0f).epsilon(0.25));
    CHECK(std::fabs(ev[0].tangent_impulse) <= 0.02f); // nothing slides at rest
    CHECK(ev[0].normal.y == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(ev[0].point.y == doctest::Approx(0.5f).epsilon(0.2)); // on the floor's top face
}

TEST_CASE("a settling body emits Slept exactly once, then the pile goes silent") {
    physics::PhysicsWorld w; // sleeping ON (default)
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    const physics::BodyId cube = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 0.0f});

    int slept_count = 0;
    for (int k = 0; k < 240; ++k) {
        w.step(kDt);
        for (const physics::SleepEvent& e : w.sleep_events()) {
            if (same_body(e.body, cube) && e.phase == physics::SleepPhase::Slept) {
                ++slept_count;
            }
        }
    }
    CHECK(w.is_asleep(cube));
    CHECK(slept_count == 1); // the awake → asleep transition is evented once, not every tick

    // A settled, sleeping pile costs nothing and says nothing: no contact events (the box-floor
    // contact is suppressed because its only dynamic member is asleep) and no repeat sleep events.
    for (int k = 0; k < 60; ++k) {
        w.step(kDt);
        CHECK(w.contact_events().empty());
        CHECK(w.sleep_events().empty());
    }
}

TEST_CASE("launching a resting body off the ground reports Ended") {
    physics::PhysicsWorld w;
    w.set_sleeping_enabled(false);
    w.set_gravity({0.0f, -9.81f, 0.0f});
    const physics::BodyId floor = add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    const physics::BodyId cube = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 0.0f});

    for (int k = 0; k < 30; ++k) {
        w.step(kDt); // settle into a persistent contact
    }
    REQUIRE(!w.contact_events().empty());
    CHECK(w.contact_events()[0].phase == physics::ContactPhase::Persisted);

    w.apply_central_impulse(cube, {0.0f, 8.0f, 0.0f}); // launch straight up

    bool saw_ended = false;
    for (int k = 0; k < 60 && !saw_ended; ++k) {
        w.step(kDt);
        for (const physics::ContactEvent& e : w.contact_events()) {
            if (e.phase == physics::ContactPhase::Ended && same_body(e.a, floor) &&
                same_body(e.b, cube)) {
                saw_ended = true;
                CHECK(e.normal_impulse == 0.0f); // nothing was exchanged the tick they separated
                CHECK(e.tangent_impulse == 0.0f);
            }
        }
    }
    CHECK(saw_ended);
    CHECK(state_of_y(w, cube) > 1.1f); // clearly airborne now

    // The pair is gone entirely: no lingering contact events once separated.
    w.step(kDt);
    CHECK(w.contact_events().empty());
}

TEST_CASE("a faller wakes a sleeping body and the wake is evented") {
    physics::PhysicsWorld w; // sleeping ON
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    const physics::BodyId lower = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 0.0f});

    for (int k = 0; k < 240 && !w.is_asleep(lower); ++k) {
        w.step(kDt);
    }
    REQUIRE(w.is_asleep(lower));

    // Drop a second box onto the sleeper. When its manifold with the lower box forms, the two share
    // an island; the awake faller reactivates the island (M7.5 stage 5), which is a Woke event.
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 3.0f, 0.0f});
    bool saw_woke = false;
    for (int k = 0; k < 120 && !saw_woke; ++k) {
        w.step(kDt);
        for (const physics::SleepEvent& e : w.sleep_events()) {
            if (same_body(e.body, lower) && e.phase == physics::SleepPhase::Woke) {
                saw_woke = true;
            }
        }
    }
    CHECK(saw_woke);
    CHECK(!w.is_asleep(lower)); // roused by the impact
}

namespace {

// Two separated two-box stacks plus two dropped balls: multiple islands, and a churn of
// began/persisted/ended and sleep/wake events over a few seconds — the surface the determinism
// proof needs.
void build_event_scene(physics::PhysicsWorld& w) {
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({20.0f, 0.5f, 20.0f}), {0.0f, 0.0f, 0.0f});
    BodyParams p;
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 0.0f}, p);
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.03f, 2.08f, 0.02f}, p);
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {8.0f, 1.05f, 8.0f}, p);
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {8.02f, 2.06f, 8.0f}, p);
    p.restitution = 0.3f;
    (void)add_body(w, sphere(0.5f), {0.1f, 4.0f, 0.0f}, p);
    (void)add_body(w, sphere(0.4f), {8.0f, 4.5f, 8.1f}, p);
}

// Fold this tick's whole event stream — counts, order, ids, phases, and float payloads — into a
// running FNV-1a hash. Floats are packed into an array first (never hashed as struct bytes, which
// carry indeterminate alignment padding — the same discipline as world_hash / hash_states).
std::uint64_t fold_events(std::uint64_t h, const physics::PhysicsWorld& w) {
    for (const physics::ContactEvent& e : w.contact_events()) {
        const std::array<std::uint32_t, 5> ids = {e.a.index,
                                                  e.a.generation,
                                                  e.b.index,
                                                  e.b.generation,
                                                  static_cast<std::uint32_t>(e.phase)};
        h = core::fnv1a_64(std::as_bytes(std::span<const std::uint32_t>{ids}), h);
        const std::array<float, 8> f = {e.point.x,
                                        e.point.y,
                                        e.point.z,
                                        e.normal.x,
                                        e.normal.y,
                                        e.normal.z,
                                        e.normal_impulse,
                                        e.tangent_impulse};
        h = core::fnv1a_64(std::as_bytes(std::span<const float>{f}), h);
    }
    for (const physics::SleepEvent& e : w.sleep_events()) {
        const std::array<std::uint32_t, 3> u = {
            e.body.index, e.body.generation, static_cast<std::uint32_t>(e.phase)};
        h = core::fnv1a_64(std::as_bytes(std::span<const std::uint32_t>{u}), h);
    }
    return h;
}

} // namespace

TEST_CASE("the event stream is bit-identical across worker counts") {
    // Contact events derive from the canonical manifolds and the solver's converged impulses, and
    // sleep events from the sequential sleep pass — all thread-count-invariant (M7.5). So the whole
    // stream must hash identically whether the islands solve sequentially or across N workers. This
    // is the test that would catch any accidental read of the impulses from inside the parallel
    // region. workers < 0 means the sequential reference path.
    auto run = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w; // sleeping ON — deactivation events are in the stream too
        build_event_scene(w);
        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        std::uint64_t h = core::kFnv1a64OffsetBasis;
        bool multi_island = false;
        for (int k = 0; k < 400; ++k) {
            w.step(1.0f / 120.0f);
            h = fold_events(h, w);
            if (w.islands_last() > 1) {
                multi_island = true;
            }
        }
        CHECK(multi_island); // the parallel path really was exercised
        return h;
    };

    const std::uint64_t sequential = run(-1);
    CHECK(run(1) == sequential);
    CHECK(run(2) == sequential);
    CHECK(run(4) == sequential);
}
