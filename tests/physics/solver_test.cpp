// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "rime/core/hash.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.4 proofs: the sequential-impulse solver turns contact manifolds into believable motion. All
// structural/analytic and pure-CPU, in the house pattern: rest heights against closed forms,
// energy and momentum invariants with margins, and the one test that DISTINGUISHES the chosen
// design (a separate NGS position pass) from the rejected one (Baumgarte) — deep penetration must
// resolve with zero kinetic energy injected. Everything drives the public PhysicsWorld seam; the
// solver internals (src/solver.hpp) are exercised only through step(), exactly as the narrowphase
// tests drive GJK/EPA only through compute_contacts().
using namespace rime;

namespace {

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::ShapeDesc capsule(float r, float half_height) {
    return physics::ShapeDesc{physics::ShapeType::Capsule, r, {}, half_height};
}

struct BodyParams {
    physics::MotionType motion = physics::MotionType::Dynamic;
    core::Quat orientation = core::quat_identity();
    core::Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    core::Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
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
    d.orientation = params.orientation;
    d.linear_velocity = params.linear_velocity;
    d.angular_velocity = params.angular_velocity;
    d.friction = params.friction;
    d.restitution = params.restitution;
    return w.create_body(d);
}

physics::BodyId add_static(physics::PhysicsWorld& w,
                           const physics::ShapeDesc& shape,
                           core::Vec3 pos,
                           float friction = 0.5f,
                           float restitution = 0.0f,
                           core::Quat q = core::quat_identity()) {
    BodyParams params;
    params.motion = physics::MotionType::Static;
    params.orientation = q;
    params.friction = friction;
    params.restitution = restitution;
    return add_body(w, shape, pos, params);
}

physics::BodyState state_of(const physics::PhysicsWorld& w, physics::BodyId id) {
    physics::BodyState s{};
    REQUIRE(w.get_body_state(id, s));
    return s;
}

float speed(const physics::BodyState& s) {
    return core::length(s.linear_velocity);
}

// Fold every body's motion state into one FNV-1a hash, in id order (the determinism witness,
// reused). Hash the float VALUES, not the raw BodyState bytes: BodyState carries alignment padding
// (Quat is alignas(16), so the struct is 64 bytes for 52 of data), and those padding bytes are
// indeterminate — `BodyState{}` does NOT reliably zero them on every compiler (Apple-clang/ARM64
// leaves stack garbage), so hashing the raw struct makes the digest depend on the stack and differ
// run to run. Packing the floats is exactly what PhysicsWorld::world_hash does, and for the same
// reason.
std::uint64_t hash_states(const physics::PhysicsWorld& world,
                          const std::vector<physics::BodyId>& ids) {
    std::uint64_t h = core::kFnv1a64OffsetBasis;
    for (const physics::BodyId id : ids) {
        physics::BodyState s{};
        (void)world.get_body_state(id, s);
        const std::array<float, 13> v = {s.position.x,
                                         s.position.y,
                                         s.position.z,
                                         s.orientation.x,
                                         s.orientation.y,
                                         s.orientation.z,
                                         s.orientation.w,
                                         s.linear_velocity.x,
                                         s.linear_velocity.y,
                                         s.linear_velocity.z,
                                         s.angular_velocity.x,
                                         s.angular_velocity.y,
                                         s.angular_velocity.z};
        h = core::fnv1a_64(std::as_bytes(std::span<const float>{v}), h);
    }
    return h;
}

} // namespace

TEST_CASE("a dropped box comes to rest on the ground at the analytic height") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}); // top face at y = 0.5
    const physics::BodyId id = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.2f, 0.0f});

    // 20 cm of free fall, then contact. The analytic rest height is ground_top + half_extent
    // minus the solver's penetration slop (NGS deliberately leaves ~5 mm so the manifold — and
    // its warm-start cache — persists): y ≈ 1.0 - 0.005.
    const float dt = 1.0f / 60.0f;
    for (int k = 0; k < 180; ++k) {
        w.step(dt);
    }

    // Steady state: penetration stays bounded (no sinking) and the box never pops above rest.
    float min_y = 1e9f;
    float max_y = -1e9f;
    for (int k = 0; k < 60; ++k) {
        w.step(dt);
        const physics::BodyState s = state_of(w, id);
        min_y = std::min(min_y, s.position.y);
        max_y = std::max(max_y, s.position.y);
    }
    CHECK(min_y >= 0.98f);  // never sunk more than ~4x slop
    CHECK(max_y <= 1.005f); // never bounced/popped above the touching height

    const physics::BodyState s = state_of(w, id);
    CHECK(s.position.y == doctest::Approx(0.995f).epsilon(0.01));
    CHECK(speed(s) <= 0.01f);
    CHECK(core::length(s.angular_velocity) <= 0.05f);
}

namespace {

// Drop a sphere (radius 0.5) from y = 2 onto a ground whose top face is y = 0.5, so the rest
// height is 1.0 and the drop height above rest is exactly 1 m. Returns the highest point reached
// AFTER first touching down, and the final height.
struct DropResult {
    float apex = 0.0f;
    float final_y = 0.0f;
};

DropResult drop_sphere(float restitution, int ticks) {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({10.0f, 0.5f, 10.0f}), {0.0f, 0.0f, 0.0f});
    BodyParams params;
    params.restitution = restitution;
    const physics::BodyId id = add_body(w, sphere(0.5f), {0.0f, 2.0f, 0.0f}, params);

    DropResult r;
    bool touched = false;
    for (int k = 0; k < ticks; ++k) {
        w.step(1.0f / 120.0f);
        const float y = state_of(w, id).position.y;
        if (!touched && y <= 1.005f) {
            touched = true; // reached the contact region on the way down
        }
        if (touched) {
            r.apex = std::max(r.apex, y);
        }
        r.final_y = y;
    }
    return r;
}

} // namespace

TEST_CASE("restitution: e = 0 does not bounce, e = 1 rebounds to nearly the drop height") {
    // e = 0 — perfectly inelastic: after touching down the sphere must never rise meaningfully
    // above the rest height, and it settles there. (Deep-impact recovery is the NGS pass, which
    // moves the position WITHOUT velocity — a Baumgarte solver fails exactly this apex bound.)
    const DropResult dead = drop_sphere(0.0f, 360); // 3 s
    CHECK(dead.apex <= 1.03f);
    CHECK(dead.final_y == doctest::Approx(1.0f).epsilon(0.03));

    // e = 1 — near-elastic: the rebound apex must come back to ~the 1 m drop height. Discrete
    // stepping measures the impact speed a fraction of a tick late (plus one tick of gravity), so
    // the bounce is allowed a modest energy window rather than an exact equality; the window is
    // still far above what any e < 1 could reach and far below a runaway gain. 1.2 s of sim
    // captures exactly the first rebound apex (fall 0.45 s + rise ~0.45 s).
    const DropResult lively = drop_sphere(1.0f, 144);
    CHECK(lively.apex >= 1.75f);
    CHECK(lively.apex <= 2.15f);
}

namespace {

// A flat box resting on a tilted static slab (both rotated by the same quaternion, so the box
// starts flush with the surface, 2 mm embedded to seed the contact). Returns the displacement
// after two seconds. Downhill is +X: the slab is rotated about Z by -theta, which tips its
// upward normal toward +X.
core::Vec3 incline_displacement(float theta_rad) {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    const core::Quat q = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -theta_rad);
    (void)add_static(w, box({10.0f, 0.5f, 10.0f}), {0.0f, 0.0f, 0.0f}, 0.5f, 0.0f, q);

    // Surface-to-centre offset 0.5 (slab) + 0.25 (box) along the tilted normal, minus 2 mm.
    BodyParams params;
    params.orientation = q;
    params.friction = 0.5f;
    const core::Vec3 start = core::rotate(q, core::Vec3{0.0f, 0.748f, 0.0f});
    const physics::BodyId id = add_body(w, box({0.5f, 0.25f, 0.5f}), start, params);

    for (int k = 0; k < 120; ++k) {
        w.step(1.0f / 60.0f);
    }
    return state_of(w, id).position - start;
}

} // namespace

TEST_CASE("friction: a box holds below the friction angle and slides above it") {
    // Coulomb: the box stays put iff tan(theta) <= mu. Both surfaces have mu = 0.5, combined as
    // sqrt(0.5 * 0.5) = 0.5, so the friction angle is atan(0.5) = 26.6 degrees. The two cases sit
    // well clear on either side. (The contact normal here is Z-free, so the deterministic tangent
    // basis puts t1 exactly along the slope — the pyramid bound is exact, not just within the
    // sqrt(2) diagonal slack.)
    const float deg = 3.14159265f / 180.0f;

    // 15 degrees (tan = 0.27): static friction holds; only millimetre-scale settling allowed.
    const core::Vec3 hold = incline_displacement(15.0f * deg);
    CHECK(core::length(hold) <= 0.05f);

    // 40 degrees (tan = 0.84): slides. Residual acceleration g*(sin - mu*cos) = 2.55 m/s^2 gives
    // ~5 m in 2 s; demand at least 1 m of clearly downhill (+X, downward-Y) motion.
    const core::Vec3 slide = incline_displacement(40.0f * deg);
    CHECK(core::length(slide) >= 1.0f);
    CHECK(slide.x >= 0.5f);
    CHECK(slide.y <= -0.1f);
}

TEST_CASE("a three-box stack stays standing and settles") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});

    // Spawned 2 mm embedded per interface so every contact exists from tick one. Without warm
    // starting this configuration is the classic failure: eight cold iterations per tick cannot
    // rebuild the support impulses and the stack buzzes itself apart sideways.
    const float ys[3] = {0.998f, 1.996f, 2.994f};
    std::vector<physics::BodyId> ids;
    for (const float y : ys) {
        ids.push_back(add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, y, 0.0f}));
    }

    for (int k = 0; k < 360; ++k) {
        w.step(1.0f / 60.0f);
    }

    for (int i = 0; i < 3; ++i) {
        const physics::BodyState s = state_of(w, ids[i]);
        CHECK(std::fabs(s.position.x) <= 0.03f); // no lateral creep
        CHECK(std::fabs(s.position.z) <= 0.03f);
        CHECK(s.position.y >= ys[i] - 0.03f); // no sinking through the one below
        CHECK(s.position.y <= ys[i] + 0.01f); // no jitter-lift
        CHECK(speed(s) <= 0.05f);             // settled, not simmering
    }
}

TEST_CASE("the solver writes impulses and the cache carries them across ticks") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    (void)add_static(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f});
    (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.1f, 0.0f});

    for (int k = 0; k < 180; ++k) {
        w.step(1.0f / 60.0f);
    }

    // The last step's contact build matched the cached points by feature id — warm starting is
    // live. (M7.3 could only ever carry zeros; this is the M7.4 witness that real impulses flow.)
    CHECK(w.contacts_warm_started_last() >= 1);

    // Read the persisted impulses back through the inspection seam (it warm-starts from the
    // step-solved cache and re-commits the same values, so peeking does not perturb them). At
    // rest the solver must supply exactly one tick of gravity per tick: the accumulated normal
    // impulses across the manifold sum to m*g*dt = 1 * 9.81 / 60 = 0.1635 N*s.
    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    REQUIRE(ms[0].count == 4);
    float total_normal = 0.0f;
    for (int i = 0; i < ms[0].count; ++i) {
        CHECK(ms[0].points[i].normal_impulse >= 0.0f); // contacts push, never pull
        total_normal += ms[0].points[i].normal_impulse;
        CHECK(std::fabs(ms[0].points[i].tangent_impulse) <= 0.02f); // nothing slides at rest
    }
    CHECK(total_normal == doctest::Approx(9.81f / 60.0f).epsilon(0.25));
}

namespace {

struct HeadOnResult {
    physics::BodyState a;
    physics::BodyState b;
    float max_momentum_error = 0.0f; // max over ticks of |p_a + p_b| (equal unit masses)
};

// Two identical unit-mass spheres, dead-on collision course at 2 m/s each, no gravity and no
// friction. Total linear momentum starts (and must stay) exactly zero: the solver only ever
// applies equal-and-opposite impulses, so any drift is a sign error in the +-P application.
HeadOnResult head_on(float restitution) {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    BodyParams pa;
    pa.linear_velocity = {2.0f, 0.0f, 0.0f};
    pa.friction = 0.0f;
    pa.restitution = restitution;
    BodyParams pb = pa;
    pb.linear_velocity = {-2.0f, 0.0f, 0.0f};
    const physics::BodyId a = add_body(w, sphere(0.5f), {-1.05f, 0.0f, 0.0f}, pa);
    const physics::BodyId b = add_body(w, sphere(0.5f), {1.05f, 0.0f, 0.0f}, pb);

    HeadOnResult r;
    for (int k = 0; k < 240; ++k) {
        w.step(1.0f / 120.0f);
        const core::Vec3 total = state_of(w, a).linear_velocity + state_of(w, b).linear_velocity;
        r.max_momentum_error = std::max(r.max_momentum_error, core::length(total));
    }
    r.a = state_of(w, a);
    r.b = state_of(w, b);
    return r;
}

} // namespace

TEST_CASE("head-on impact conserves momentum; e = 1 reflects, e = 0 is perfectly inelastic") {
    // Elastic: equal masses swap velocities — each sphere leaves with (nearly) its incoming
    // speed, reversed, and they genuinely separate.
    const HeadOnResult elastic = head_on(1.0f);
    CHECK(elastic.max_momentum_error <= 1e-4f);
    CHECK(elastic.a.linear_velocity.x == doctest::Approx(-2.0f).epsilon(0.1));
    CHECK(elastic.b.linear_velocity.x == doctest::Approx(2.0f).epsilon(0.1));
    CHECK(elastic.b.position.x - elastic.a.position.x >= 1.2f);

    // Inelastic: the pair comes to a joint stop (total momentum was zero) and stays together —
    // the residual overlap is recovered by NGS with no rebound velocity.
    const HeadOnResult inelastic = head_on(0.0f);
    CHECK(inelastic.max_momentum_error <= 1e-4f);
    CHECK(speed(inelastic.a) <= 0.02f);
    CHECK(speed(inelastic.b) <= 0.02f);
    CHECK(core::length(inelastic.a.linear_velocity - inelastic.b.linear_velocity) <= 0.02f);
}

TEST_CASE("NGS, not Baumgarte: deep overlap resolves positionally with zero velocity injected") {
    // The discriminating test for ADR-0026's solver choice. Two boxes spawn 0.3 m interpenetrated
    // and PERFECTLY AT REST, gravity off. A Baumgarte solver folds beta/dt * penetration into the
    // velocity constraint: one step would fling them apart at metres per second (beta=0.2 at
    // 60 Hz over 0.3 m is ~3.6 m/s of invented velocity). The NGS pass corrects positions
    // directly and never touches the velocity arrays, so the speeds must stay at (numerically
    // exactly) zero while the overlap shrinks.
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::BodyId a = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f});
    const physics::BodyId b = add_body(w, box({0.5f, 0.5f, 0.5f}), {0.7f, 0.0f, 0.0f});

    w.step(1.0f / 60.0f);

    physics::BodyState sa = state_of(w, a);
    physics::BodyState sb = state_of(w, b);
    CHECK(speed(sa) <= 1e-6f);
    CHECK(speed(sb) <= 1e-6f);
    CHECK(core::length(sa.angular_velocity) <= 1e-6f);
    CHECK(core::length(sb.angular_velocity) <= 1e-6f);

    const float gap1 = sb.position.x - sa.position.x;
    CHECK(gap1 >= 0.75f); // penetration meaningfully reduced (0.30 -> < 0.25) ...
    CHECK(gap1 <= 0.95f); // ... but rate-limited, not teleported apart in one tick
    // Equal masses split the correction symmetrically (the positional analogue of momentum).
    CHECK(std::fabs(sa.position.x + sb.position.x - 0.7f) <= 1e-4f);

    // Left alone, the pass converges to touching-minus-slop over the following ticks — still
    // without ever inventing velocity.
    for (int k = 0; k < 60; ++k) {
        w.step(1.0f / 60.0f);
    }
    sa = state_of(w, a);
    sb = state_of(w, b);
    CHECK(sb.position.x - sa.position.x >= 0.97f);
    CHECK(sb.position.x - sa.position.x <= 1.0f);
    CHECK(speed(sa) <= 1e-5f);
    CHECK(speed(sb) <= 1e-5f);
}

TEST_CASE("a colliding, rubbing, bouncing scene is bit-identical run to run") {
    // The M7.1 determinism witness, now over the full pipeline: broadphase, GJK/EPA manifolds,
    // warm-started PGS with friction and restitution, and the NGS pass. Same binary + same
    // inputs must produce byte-identical body state after 300 ticks of real contact work.
    const auto run = [] {
        physics::PhysicsWorld w;
        w.set_gravity({0.0f, -9.81f, 0.0f});
        std::vector<physics::BodyId> ids;
        ids.push_back(add_static(w, box({8.0f, 0.5f, 8.0f}), {0.0f, 0.0f, 0.0f}, 0.6f));

        BodyParams p;
        p.friction = 0.5f;
        ids.push_back(add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.05f, 0.0f}, p));
        p.friction = 0.4f;
        p.restitution = 0.2f;
        ids.push_back(add_body(w, box({0.5f, 0.5f, 0.5f}), {0.06f, 2.1f, 0.03f}, p));
        p.friction = 0.3f;
        p.restitution = 0.7f;
        ids.push_back(add_body(w, sphere(0.5f), {1.5f, 3.0f, 0.2f}, p));
        p.restitution = 0.4f;
        ids.push_back(add_body(w, sphere(0.4f), {1.55f, 4.0f, 0.15f}, p));
        p.friction = 0.8f;
        p.restitution = 0.1f;
        p.orientation = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.9f);
        p.angular_velocity = {1.0f, 2.0f, 0.5f};
        ids.push_back(add_body(w, capsule(0.3f, 0.4f), {-1.5f, 2.0f, 0.0f}, p));
        p.orientation = core::quat_from_axis_angle({1.0f, 1.0f, 0.0f}, 0.7f);
        p.angular_velocity = {0.0f, 3.0f, 0.0f};
        p.restitution = 0.5f;
        ids.push_back(add_body(w, box({0.4f, 0.3f, 0.5f}), {-0.2f, 3.4f, -0.1f}, p));

        for (int k = 0; k < 300; ++k) {
            w.step(1.0f / 120.0f);
        }
        return hash_states(w, ids);
    };
    CHECK(run() == run());
}
