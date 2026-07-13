// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "rime/physics/aabb.hpp"
#include "rime/physics/physics.hpp"

// M7.2 proofs: the dual-tree broadphase reports EXACTLY the candidate pairs a brute-force O(n²)
// scan over the *stored* fat AABBs would (the oracle test), in canonical deterministic order, and
// never for two static bodies; the fat margin makes small motion a no-op; and the trees stay
// structurally valid under heavy create/destroy churn (the ASan net for a pooled, index-linked
// structure). compute_aabb is checked analytically on its own, independent of any tree.
using namespace rime;

namespace {

// Deterministic pseudo-random source (Numerical Recipes LCG). std::mt19937 is reproducible too,
// but the <random> *distributions* are not specified bit-exactly across standard libraries; a
// hand-rolled generator keeps these scenes identical on every platform, which the determinism
// cases (and CI triage) depend on.
struct Lcg {
    std::uint32_t state;

    explicit Lcg(std::uint32_t seed) : state(seed) {}

    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    // Uniform in [lo, hi), from the top 24 bits (an LCG's low bits are the weak ones).
    float uniform(float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(next() >> 8) * (1.0f / 16777216.0f));
    }
};

using IndexPair = std::pair<std::uint32_t, std::uint32_t>;
using PairSet = std::set<IndexPair>;

// The O(n²) oracle: a candidate pair is two live bodies whose STORED fat boxes overlap and that
// are not both static. "Stored" is the load-bearing word — after stepping, a proxy's fat box is
// staler (fatter) than the body's current tight bounds, because move_proxy is a no-op until the
// body escapes the stored box. The broadphase's contract is over what it stored, so the oracle
// reads the same boxes back (broadphase_aabb) rather than recomputing tight+margin.
PairSet brute_force_pairs(const physics::PhysicsWorld& world,
                          const std::vector<physics::BodyId>& ids,
                          const std::vector<physics::MotionType>& motion) {
    PairSet s;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            if (motion[i] == physics::MotionType::Static &&
                motion[j] == physics::MotionType::Static) {
                continue;
            }
            physics::Aabb a{};
            physics::Aabb b{};
            REQUIRE(world.broadphase_aabb(ids[i], a));
            REQUIRE(world.broadphase_aabb(ids[j], b));
            if (physics::overlaps(a, b)) {
                s.insert(
                    {std::min(ids[i].index, ids[j].index), std::max(ids[i].index, ids[j].index)});
            }
        }
    }
    return s;
}

// Run the broadphase and collapse it to an index-pair set, asserting canonical form on the way
// through: within a pair the smaller slot comes first, and the list is STRICTLY ascending by
// (a, b) — strict, so one comparison proves both "sorted" and "de-duplicated".
PairSet broadphase_pairs(const physics::PhysicsWorld& world) {
    std::vector<physics::PhysicsWorld::Pair> pairs;
    world.compute_pairs(pairs);
    PairSet s;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        CHECK(pairs[i].a.index < pairs[i].b.index);
        if (i > 0) {
            CHECK(IndexPair{pairs[i - 1].a.index, pairs[i - 1].b.index} <
                  IndexPair{pairs[i].a.index, pairs[i].b.index});
        }
        s.insert({pairs[i].a.index, pairs[i].b.index});
    }
    CHECK(s.size() == pairs.size());
    return s;
}

physics::ShapeDesc random_shape(Lcg& rng) {
    switch (rng.next() % 3u) {
        case 0:
            return {physics::ShapeType::Sphere, rng.uniform(0.2f, 0.8f), {}, 0.0f};
        case 1:
            return {physics::ShapeType::Box,
                    0.0f,
                    {rng.uniform(0.2f, 0.8f), rng.uniform(0.2f, 0.8f), rng.uniform(0.2f, 0.8f)},
                    0.0f};
        default:
            return {
                physics::ShapeType::Capsule, rng.uniform(0.15f, 0.4f), {}, rng.uniform(0.2f, 0.6f)};
    }
}

physics::MotionType random_motion(Lcg& rng) {
    // ~25% static, ~25% kinematic, ~50% dynamic — enough of every class that static–static,
    // static–dynamic, and moving–moving pairs all actually occur in the oracle scenes.
    const std::uint32_t kind = rng.next() % 4u;
    return kind == 0   ? physics::MotionType::Static
           : kind == 1 ? physics::MotionType::Kinematic
                       : physics::MotionType::Dynamic;
}

physics::BodyDesc random_body(Lcg& rng, float span) {
    physics::BodyDesc d;
    d.motion = random_motion(rng);
    d.shape = random_shape(rng);
    // Braced initializers evaluate left-to-right (sequenced), so the rng draw order — and with it
    // the scene — is identical on every compiler.
    d.position = {rng.uniform(-span, span), rng.uniform(-span, span), rng.uniform(-span, span)};
    d.linear_velocity = {
        rng.uniform(-1.5f, 1.5f), rng.uniform(-1.5f, 1.5f), rng.uniform(-1.5f, 1.5f)};
    d.angular_velocity = {
        rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f)};
    return d;
}

struct Scene {
    std::vector<physics::BodyId> ids;
    std::vector<physics::MotionType> motion;
};

// A dense mixed cloud: bodies packed into a few metres so fat boxes overlap a lot, with random
// velocities so stepping rearranges the cloud (and forces fat-box re-inserts along the way).
Scene build_mixed_scene(physics::PhysicsWorld& world, std::uint32_t seed, int count) {
    Scene scene;
    Lcg rng(seed);
    for (int i = 0; i < count; ++i) {
        const physics::BodyDesc d = random_body(rng, 4.0f);
        scene.ids.push_back(world.create_body(d));
        scene.motion.push_back(d.motion);
    }
    return scene;
}

// Bitwise-equal bounds, no epsilon: the fat-box fast path either rewrites the stored box or leaves
// it untouched — "almost equal" would mean it wrote when it claimed not to.
bool same_aabb(const physics::Aabb& a, const physics::Aabb& b) {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z && a.max.x == b.max.x &&
           a.max.y == b.max.y && a.max.z == b.max.z;
}

} // namespace

TEST_CASE("broadphase pairs match the brute-force oracle, before and after stepping") {
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, -2.0f, 0.0f}); // gentle, so the cloud stays mixed while it falls
    const Scene scene = build_mixed_scene(world, 0xC0FFEEu, 64);

    // Freshly built, every stored fat box is exactly tight+margin: the easy case.
    REQUIRE(world.validate_broadphase());
    PairSet expect = brute_force_pairs(world, scene.ids, scene.motion);
    CHECK(!expect.empty()); // a vacuous oracle proves nothing — the cloud must actually overlap
    CHECK(broadphase_pairs(world) == expect);

    // Step ~100 ticks: dynamics fall and drift, some escape their fat boxes (re-insert), others
    // coast inside stale-but-still-valid ones. That divergence between stored and tight bounds is
    // exactly what this half exercises — the broadphase must still agree with the oracle over the
    // boxes it stored.
    for (int k = 0; k < 100; ++k) {
        world.step(1.0f / 60.0f);
    }
    REQUIRE(world.validate_broadphase());
    expect = brute_force_pairs(world, scene.ids, scene.motion);
    CHECK(!expect.empty());
    CHECK(broadphase_pairs(world) == expect);
}

TEST_CASE("pair list is canonical and deterministic across identical runs") {
    // Two independently built, identical worlds must produce identical pair lists — generations
    // included — both freshly built and after stepping. This is the same-binary determinism
    // contract (ADR-0026) at the broadphase layer: replays (M11) and M7.5's thread-count-
    // independent parallel step both stand on it.
    using PairLog =
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>>;
    const auto run = [] {
        physics::PhysicsWorld world;
        world.set_gravity({0.0f, -9.81f, 0.0f});
        build_mixed_scene(world, 0x5EEDBA5Eu, 48);
        PairLog log;
        const auto capture = [&] {
            (void)broadphase_pairs(world); // asserts canonical order + de-duplication
            std::vector<physics::PhysicsWorld::Pair> pairs;
            world.compute_pairs(pairs);
            for (const auto& p : pairs) {
                log.emplace_back(p.a.index, p.a.generation, p.b.index, p.b.generation);
            }
        };
        capture();
        for (int k = 0; k < 50; ++k) {
            world.step(1.0f / 120.0f);
        }
        capture();
        return log;
    };
    CHECK(run() == run());
}

TEST_CASE("a both-static overlap is never reported; a static-dynamic one is") {
    physics::PhysicsWorld world;
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape = physics::ShapeDesc{physics::ShapeType::Box, 0.0f, {1.0f, 1.0f, 1.0f}, 0.0f};
    d.position = {0.0f, 0.0f, 0.0f};
    const physics::BodyId s0 = world.create_body(d);
    d.position = {0.5f, 0.0f, 0.0f}; // clearly interpenetrating s0
    const physics::BodyId s1 = world.create_body(d);

    // Neither body can ever move, so the pair could never produce a new contact — reporting it
    // would be permanent narrowphase busywork. The dual-tree split makes it impossible, not
    // merely filtered: static bodies never query.
    std::vector<physics::PhysicsWorld::Pair> pairs;
    world.compute_pairs(pairs);
    CHECK(pairs.empty());

    // Drop a dynamic body overlapping both statics: exactly the two static-dynamic pairs appear.
    d.motion = physics::MotionType::Dynamic;
    d.position = {0.25f, 0.5f, 0.0f};
    const physics::BodyId dyn = world.create_body(d);
    world.compute_pairs(pairs);
    CHECK(pairs.size() == 2);
    const PairSet expect = {{std::min(s0.index, dyn.index), std::max(s0.index, dyn.index)},
                            {std::min(s1.index, dyn.index), std::max(s1.index, dyn.index)}};
    CHECK(broadphase_pairs(world) == expect);
}

TEST_CASE("compute_aabb matches the closed forms for box, sphere, and capsule") {
    const core::Vec3 pos{1.0f, 2.0f, 3.0f};

    // A unit box (half-extents 0.5) rotated 45° about Z: its x/y shadow is the diagonal of the
    // half-cross-section, |cos45|·0.5 + |sin45|·0.5 = √½ ≈ 0.7071; z is unaffected by a Z-spin.
    {
        const physics::ShapeDesc box{physics::ShapeType::Box, 0.0f, {0.5f, 0.5f, 0.5f}, 0.0f};
        const core::Quat q = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, core::kPi / 4.0f);
        const physics::Aabb bb = physics::compute_aabb(box, pos, q);
        const float r = std::sqrt(0.5f);
        CHECK(bb.min.x == doctest::Approx(pos.x - r));
        CHECK(bb.max.x == doctest::Approx(pos.x + r));
        CHECK(bb.min.y == doctest::Approx(pos.y - r));
        CHECK(bb.max.y == doctest::Approx(pos.y + r));
        CHECK(bb.min.z == doctest::Approx(pos.z - 0.5f));
        CHECK(bb.max.z == doctest::Approx(pos.z + 0.5f));
    }

    // A sphere's bound ignores orientation entirely: pos ± r on every axis, any rotation.
    {
        const physics::ShapeDesc sphere{physics::ShapeType::Sphere, 0.75f, {}, 0.0f};
        const core::Quat q = core::quat_from_axis_angle({1.0f, 2.0f, -0.5f}, 1.3f);
        const physics::Aabb bb = physics::compute_aabb(sphere, pos, q);
        CHECK(bb.min.x == doctest::Approx(pos.x - 0.75f));
        CHECK(bb.max.x == doctest::Approx(pos.x + 0.75f));
        CHECK(bb.min.y == doctest::Approx(pos.y - 0.75f));
        CHECK(bb.max.y == doctest::Approx(pos.y + 0.75f));
        CHECK(bb.min.z == doctest::Approx(pos.z - 0.75f));
        CHECK(bb.max.z == doctest::Approx(pos.z + 0.75f));
    }

    // A capsule bound is the union of its two end-spheres. Rotated 90° about Z the local-Y axis
    // lands on world X, so the closed form is hand-computable: ±(hh + r) along x, ±r elsewhere.
    {
        const physics::ShapeDesc cap{physics::ShapeType::Capsule, 0.3f, {}, 0.6f};
        const core::Quat q = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, core::kPi / 2.0f);
        const physics::Aabb bb = physics::compute_aabb(cap, pos, q);
        CHECK(bb.min.x == doctest::Approx(pos.x - 0.9f));
        CHECK(bb.max.x == doctest::Approx(pos.x + 0.9f));
        CHECK(bb.min.y == doctest::Approx(pos.y - 0.3f));
        CHECK(bb.max.y == doctest::Approx(pos.y + 0.3f));
        CHECK(bb.min.z == doctest::Approx(pos.z - 0.3f));
        CHECK(bb.max.z == doctest::Approx(pos.z + 0.3f));
    }

    // And for an arbitrary tilt: still exactly the merge of the two end-sphere boxes, no slack.
    {
        const physics::ShapeDesc cap{physics::ShapeType::Capsule, 0.25f, {}, 0.5f};
        const core::Quat q = core::quat_from_axis_angle({1.0f, 1.0f, 0.0f}, 0.7f);
        const core::Vec3 axis = core::rotate(q, core::Vec3{0.0f, 0.5f, 0.0f});
        const core::Vec3 r{0.25f, 0.25f, 0.25f};
        const physics::Aabb expect = physics::merge(physics::Aabb{pos - axis - r, pos - axis + r},
                                                    physics::Aabb{pos + axis - r, pos + axis + r});
        const physics::Aabb bb = physics::compute_aabb(cap, pos, q);
        CHECK(bb.min.x == doctest::Approx(expect.min.x));
        CHECK(bb.max.x == doctest::Approx(expect.max.x));
        CHECK(bb.min.y == doctest::Approx(expect.min.y));
        CHECK(bb.max.y == doctest::Approx(expect.max.y));
        CHECK(bb.min.z == doctest::Approx(expect.min.z));
        CHECK(bb.max.z == doctest::Approx(expect.max.z));
    }
}

TEST_CASE("fat margin: small motion keeps the stored box; larger motion re-inserts") {
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, 0.0f, 0.0f});

    physics::BodyDesc d;
    d.motion = physics::MotionType::Dynamic;
    d.shape = physics::ShapeDesc{physics::ShapeType::Box, 0.0f, {0.5f, 0.5f, 0.5f}, 0.0f};
    d.linear_velocity = {1.0f, 0.0f, 0.0f}; // 1 m/s glide: no gravity, no linear damping
    const physics::BodyId id = world.create_body(d);

    // The stored box is fat: it strictly contains the tight bounds (pos ± 0.5 at creation).
    physics::Aabb fat0{};
    REQUIRE(world.broadphase_aabb(id, fat0));
    const physics::Aabb tight0{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    CHECK(physics::contains(fat0, tight0));
    CHECK(fat0.min.x < tight0.min.x);
    CHECK(fat0.max.x > tight0.max.x);

    // One 60 Hz step moves the body ~1.7 cm — far inside the fat margin, so move_proxy must take
    // its no-op path: the stored bounds are BIT-identical, not merely close. (The exact margin is
    // an M7.10 tuning knob; this test only assumes it exceeds one step's travel.)
    world.step(1.0f / 60.0f);
    physics::Aabb fat1{};
    REQUIRE(world.broadphase_aabb(id, fat1));
    CHECK(same_aabb(fat0, fat1));
    CHECK(world.validate_broadphase());

    // Half a metre of travel blows well past any sane margin: the proxy must have been re-fattened
    // around the new position, so the stored bounds moved forward with the body.
    for (int k = 0; k < 30; ++k) {
        world.step(1.0f / 60.0f);
    }
    physics::Aabb fat2{};
    REQUIRE(world.broadphase_aabb(id, fat2));
    CHECK(!same_aabb(fat0, fat2));
    CHECK(fat2.min.x > fat0.min.x);
    CHECK(world.validate_broadphase());
}

TEST_CASE("trees stay valid and oracle-exact under heavy create/destroy churn") {
    // The ASan-relevant test: an AABB tree is a pooled, index-linked structure, so the failure
    // modes of churn are stale parent/child indices and free-list corruption — exactly what
    // interleaved create/destroy/step/query plus validate_broadphase() and the oracle will trip.
    physics::PhysicsWorld world;
    world.set_gravity({0.0f, -9.81f, 0.0f});
    Lcg rng(20260713u);

    std::vector<physics::BodyId> ids;
    std::vector<physics::MotionType> motion;

    for (int iter = 0; iter < 400; ++iter) {
        const bool create = ids.size() < 8 || (ids.size() < 96 && rng.next() % 3u != 0u);
        if (create) {
            const physics::BodyDesc d = random_body(rng, 6.0f);
            ids.push_back(world.create_body(d));
            motion.push_back(d.motion);
        } else {
            const std::size_t victim = rng.next() % ids.size();
            world.destroy_body(ids[victim]);
            CHECK_FALSE(world.is_alive(ids[victim]));
            ids[victim] = ids.back();
            ids.pop_back();
            motion[victim] = motion.back();
            motion.pop_back();
        }
        if (iter % 7 == 0) {
            world.step(1.0f / 60.0f);
        }
        if (iter % 25 == 0) {
            REQUIRE(world.validate_broadphase());
            CHECK(broadphase_pairs(world) == brute_force_pairs(world, ids, motion));
        }
    }
    REQUIRE(world.validate_broadphase());
    CHECK(broadphase_pairs(world) == brute_force_pairs(world, ids, motion));
    CHECK(world.body_count() == ids.size());

    // Tear everything down: both trees must empty cleanly through the remove/free-list path.
    for (const physics::BodyId id : ids) {
        world.destroy_body(id);
    }
    CHECK(world.body_count() == 0);
    std::vector<physics::PhysicsWorld::Pair> pairs;
    world.compute_pairs(pairs);
    CHECK(pairs.empty());
    CHECK(world.validate_broadphase());
}
