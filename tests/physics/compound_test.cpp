// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"

// M7.12 proofs: compound collision shapes (ADR-0028). Every check is analytic or structural and
// drives the PUBLIC seam only — register_compound/compound_info, create_body, compute_contacts,
// step, events, queries — never the store internals (src/compound.hpp is invisible above the
// seam, by design). The recurring tricks: compose a compound whose closed form we know (two
// half-boxes ARE one big box, so the parallel-axis composition must reproduce the big box's
// moments exactly), and build shapes whose *stability itself* discriminates the contact model (a
// dumbbell standing over the gap between its feet stands ONLY if the pair carries one manifold
// per touching child — the multi-region contract this brick exists to deliver).
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

physics::ShapeDesc box(core::Vec3 half) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.half_extents = half;
    return s;
}

physics::ShapeDesc sphere(float r) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Sphere;
    s.radius = r;
    return s;
}

physics::ShapeDesc hull_shape(physics::HullId id) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::ConvexHull;
    s.hull = id;
    return s;
}

physics::ShapeDesc compound_shape(physics::CompoundId id) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Compound;
    s.compound = id;
    return s;
}

// A box-shaped hull (the hull_test cube: corner i positive on axis k iff bit k of i is set, six
// outward-wound quads) — used to prove the hull-child branch of the mass composition.
physics::HullId register_cube_hull(physics::PhysicsWorld& w, core::Vec3 half) {
    std::vector<core::Vec3> verts;
    for (std::uint32_t i = 0; i < 8; ++i) {
        verts.push_back({(i & 1u) != 0 ? half.x : -half.x,
                         (i & 2u) != 0 ? half.y : -half.y,
                         (i & 4u) != 0 ? half.z : -half.z});
    }
    const std::vector<std::uint32_t> counts = {4, 4, 4, 4, 4, 4};
    const std::vector<std::uint32_t> indices = {
        0, 4, 6, 2, // -X
        1, 3, 7, 5, // +X
        0, 1, 5, 4, // -Y
        2, 6, 7, 3, // +Y
        0, 2, 3, 1, // -Z
        4, 5, 7, 6, // +Z
    };
    return w.register_hull(physics::HullDesc{verts, counts, indices});
}

// The workhorse compound: a DUMBBELL — two 0.5 m cube feet a metre and a half apart, nothing
// between them. Its centre of mass sits over the GAP, outside either foot's own footprint, so it
// can only stand if BOTH feet push back — i.e. if the (compound, floor) pair carries one manifold
// per touching child. No single convex box can even have this property (a box's COM is always
// over its own base), which is exactly why the compound shape exists.
physics::CompoundId register_dumbbell(physics::PhysicsWorld& w) {
    const physics::ShapeDesc foot = box({0.25f, 0.25f, 0.25f});
    const std::vector<physics::CompoundChildDesc> kids = {
        {foot, {-0.75f, 0.0f, 0.0f}, core::quat_identity()},
        {foot, {0.75f, 0.0f, 0.0f}, core::quat_identity()},
    };
    return w.register_compound(physics::CompoundDesc{kids});
}

physics::BodyId add_body(physics::PhysicsWorld& w,
                         const physics::ShapeDesc& shape,
                         core::Vec3 pos,
                         physics::MotionType motion = physics::MotionType::Dynamic,
                         float mass = 1.0f) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = shape;
    d.position = pos;
    d.mass = mass;
    return w.create_body(d);
}

// A big static floor whose top surface sits at y = 0 (the islands_test fixture).
physics::BodyId add_ground(physics::PhysicsWorld& w) {
    return add_body(w, box({50.0f, 0.5f, 50.0f}), {0.0f, -0.5f, 0.0f}, physics::MotionType::Static);
}

} // namespace

TEST_CASE("register_compound validates: accepts children, rejects broken input") {
    physics::PhysicsWorld w;
    const physics::ShapeDesc foot = box({0.25f, 0.25f, 0.25f});

    SUBCASE("a well-formed two-box compound registers and reads back") {
        const physics::CompoundId id = register_dumbbell(w);
        REQUIRE(id.is_valid());
        physics::CompoundInfo info;
        REQUIRE(w.compound_info(id, info));
        CHECK(info.child_count == 2);
        CHECK(info.volume == doctest::Approx(2.0f * 0.125f).epsilon(1e-5));

        // A second registration is a distinct id (append-only store, ids are registration order).
        const physics::CompoundId id2 = register_dumbbell(w);
        REQUIRE(id2.is_valid());
        CHECK(id2 != id);
    }

    SUBCASE("an empty child list is rejected") {
        CHECK_FALSE(w.register_compound(physics::CompoundDesc{}).is_valid());
    }

    SUBCASE("a nested compound child is rejected (v1: no nesting, ADR-0028)") {
        const physics::CompoundId inner = register_dumbbell(w);
        REQUIRE(inner.is_valid());
        const std::vector<physics::CompoundChildDesc> kids = {
            {compound_shape(inner), {0.0f, 0.0f, 0.0f}, core::quat_identity()},
        };
        CHECK_FALSE(w.register_compound(physics::CompoundDesc{kids}).is_valid());
    }

    SUBCASE("a hull child with an unresolvable id is rejected") {
        const std::vector<physics::CompoundChildDesc> kids = {
            {foot, {0.0f, 0.0f, 0.0f}, core::quat_identity()},
            {hull_shape(physics::HullId{42, 0}), {1.0f, 0.0f, 0.0f}, core::quat_identity()},
        };
        CHECK_FALSE(w.register_compound(physics::CompoundDesc{kids}).is_valid());
    }

    SUBCASE("a volumeless child is rejected") {
        const std::vector<physics::CompoundChildDesc> kids = {
            {sphere(0.0f), {0.0f, 0.0f, 0.0f}, core::quat_identity()},
        };
        CHECK_FALSE(w.register_compound(physics::CompoundDesc{kids}).is_valid());
    }

    SUBCASE("a degenerate (zero) child orientation is rejected") {
        std::vector<physics::CompoundChildDesc> kids = {
            {foot, {0.0f, 0.0f, 0.0f}, core::Quat{0.0f, 0.0f, 0.0f, 0.0f}},
        };
        CHECK_FALSE(w.register_compound(physics::CompoundDesc{kids}).is_valid());
    }

    SUBCASE("compound_info on a null/unknown id reports false") {
        physics::CompoundInfo info;
        CHECK_FALSE(w.compound_info(physics::CompoundId{}, info));
        CHECK_FALSE(w.compound_info(physics::CompoundId{7, 0}, info));
    }

    SUBCASE("a compound-shaped body with an unresolvable id is refused") {
        physics::BodyDesc d;
        d.shape = compound_shape(physics::CompoundId{});
        CHECK_FALSE(w.create_body(d).is_valid());
        CHECK(w.body_count() == 0);
    }
}

TEST_CASE("compound mass properties: two half-boxes compose to one big box's closed form") {
    // THE parallel-axis proof. Two boxes of half-extents {0.25, 0.2, 0.3} placed at x = ±0.25 ARE
    // one box of half-extents {0.5, 0.2, 0.3} under uniform density, so the composed inertia must
    // reproduce the big box's closed form I/m = (h_b² + h_c²)/3 EXACTLY:
    //   x-axis: no child sits off the x-axis, so no shift — the small boxes' own x-moment already
    //           equals the big box's;
    //   y/z axes: each child's own moment PLUS its d² = 0.25² parallel-axis shift must land on
    //           the big box's value — the theorem doing real work, not a no-op.
    const core::Vec3 small{0.25f, 0.2f, 0.3f};
    const core::Vec3 big{0.5f, 0.2f, 0.3f};
    const float ix = (big.y * big.y + big.z * big.z) / 3.0f;
    const float iy = (big.x * big.x + big.z * big.z) / 3.0f;
    const float iz = (big.x * big.x + big.y * big.y) / 3.0f;

    const auto check_info = [&](const physics::CompoundInfo& info) {
        CHECK(info.volume == doctest::Approx(8.0f * big.x * big.y * big.z).epsilon(1e-4));
        CHECK(std::fabs(info.centroid.x) < 1e-5f);
        CHECK(std::fabs(info.centroid.y) < 1e-5f);
        CHECK(std::fabs(info.centroid.z) < 1e-5f);
        CHECK(info.inertia_per_mass.x == doctest::Approx(ix).epsilon(1e-3));
        CHECK(info.inertia_per_mass.y == doctest::Approx(iy).epsilon(1e-3));
        CHECK(info.inertia_per_mass.z == doctest::Approx(iz).epsilon(1e-3));
        CHECK(core::same_rotation(info.principal_rotation, core::quat_identity(), 1e-4f));
    };

    SUBCASE("both children primitive boxes") {
        physics::PhysicsWorld w;
        const std::vector<physics::CompoundChildDesc> kids = {
            {box(small), {-0.25f, 0.0f, 0.0f}, core::quat_identity()},
            {box(small), {0.25f, 0.0f, 0.0f}, core::quat_identity()},
        };
        const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
        REQUIRE(id.is_valid());
        physics::CompoundInfo info;
        REQUIRE(w.compound_info(id, info));
        check_info(info);
    }

    SUBCASE("one child a primitive box, one a box-shaped HULL — the hull branch composes too") {
        physics::PhysicsWorld w;
        const physics::HullId hull = register_cube_hull(w, small);
        REQUIRE(hull.is_valid());
        const std::vector<physics::CompoundChildDesc> kids = {
            {box(small), {-0.25f, 0.0f, 0.0f}, core::quat_identity()},
            {hull_shape(hull), {0.25f, 0.0f, 0.0f}, core::quat_identity()},
        };
        const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
        REQUIRE(id.is_valid());
        physics::CompoundInfo info;
        REQUIRE(w.compound_info(id, info));
        check_info(info);
    }
}

TEST_CASE("compound COM re-centring: the body's position IS the composed centre of mass") {
    // A single-child compound whose child is authored 5 m off origin. Registration must report
    // that centroid and re-centre the stored pose, so a body made from it behaves EXACTLY like
    // the bare primitive at the body's position — the engine-wide "position is the COM"
    // invariant, held by construction (ADR-0028; the deliberate-COM-offset use ADR-0027 promised
    // compounds would honestly own).
    physics::PhysicsWorld w;
    const std::vector<physics::CompoundChildDesc> kids = {
        {sphere(0.5f), {5.0f, 0.0f, 0.0f}, core::quat_identity()},
    };
    const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
    REQUIRE(id.is_valid());
    physics::CompoundInfo info;
    REQUIRE(w.compound_info(id, info));
    CHECK(info.centroid.x == doctest::Approx(5.0f).epsilon(1e-5));
    CHECK(info.inertia_per_mass.x == doctest::Approx(0.4f * 0.5f * 0.5f).epsilon(1e-3));

    // Drop the compound body and a bare sphere side by side: same shape, same COM ⇒ they must
    // settle at the same height (the re-centred child collides AT the body position, not 5 m
    // away).
    add_ground(w);
    const physics::BodyId c = add_body(w, compound_shape(id), {0.0f, 1.0f, 0.0f});
    const physics::BodyId s = add_body(w, sphere(0.5f), {3.0f, 1.0f, 0.0f});
    REQUIRE(c.is_valid());
    REQUIRE(s.is_valid());
    for (int i = 0; i < 240; ++i) {
        w.step(kDt);
    }
    physics::BodyState cs;
    physics::BodyState ss;
    REQUIRE(w.get_body_state(c, cs));
    REQUIRE(w.get_body_state(s, ss));
    CHECK(cs.position.y == doctest::Approx(ss.position.y).epsilon(1e-3));
    CHECK(std::fabs(cs.position.x) < 1e-3f); // it fell straight down, not 5 m off
}

TEST_CASE("a dumbbell compound stands on BOTH feet: one manifold per touching child pair") {
    // The headline structural proof of the multi-region contact model. The dumbbell's COM is over
    // the GAP between its feet — outside either foot's own footprint — so a single ≤4-point
    // manifold (the pre-M7.12 one-manifold-per-pair model) can only represent one foot's patch
    // and the body would tip over it. Standing still for 300 ticks is therefore only possible if
    // the (floor, dumbbell) pair carries one manifold per touching child.
    physics::PhysicsWorld w;
    add_ground(w);
    const physics::CompoundId id = register_dumbbell(w);
    REQUIRE(id.is_valid());
    const physics::BodyId b = add_body(w, compound_shape(id), {0.0f, 0.3f, 0.0f});
    REQUIRE(b.is_valid());

    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }
    physics::BodyState st;
    REQUIRE(w.get_body_state(b, st));
    CHECK(st.position.y == doctest::Approx(0.25f).epsilon(0.05));             // resting on the feet
    CHECK(core::same_rotation(st.orientation, core::quat_identity(), 5e-3f)); // did NOT tip
    CHECK(w.is_asleep(b)); // it came fully to rest — the island slept

    // Inspect the contact build directly: exactly two manifolds for this pair — one per foot —
    // tagged with the foot's child index on the compound side (the floor was created first, so
    // it is canonical body a and the compound is b).
    std::vector<physics::Manifold> manifolds;
    w.compute_contacts(manifolds);
    REQUIRE(manifolds.size() == 2);
    CHECK(manifolds[0].child_a == 0);
    CHECK(manifolds[0].child_b == 0); // foot 0's region
    CHECK(manifolds[1].child_a == 0);
    CHECK(manifolds[1].child_b == 1); // foot 1's region — a distinct manifold, distinct region
    for (const physics::Manifold& m : manifolds) {
        CHECK(m.count >= 1);
        CHECK(m.normal.y == doctest::Approx(1.0f).epsilon(1e-3)); // floor pushes the dumbbell up
    }
}

TEST_CASE("a compound with overhung mass topples — stability above is not the solver gluing") {
    // The negative control for the standing test: same feet, plus a third child cantilevered out
    // at x = 4, ELEVATED (never touching the ground). The composed COM lands outside the union of
    // the feet's footprints, so no contact arrangement can hold it — the correct physics is to
    // tip. If this fell asleep upright, the standing test above would be proving nothing.
    physics::PhysicsWorld w;
    add_ground(w);
    const physics::ShapeDesc cube = box({0.25f, 0.25f, 0.25f});
    const std::vector<physics::CompoundChildDesc> kids = {
        {cube, {-0.75f, 0.0f, 0.0f}, core::quat_identity()},
        {cube, {0.75f, 0.0f, 0.0f}, core::quat_identity()},
        {cube, {4.0f, 1.0f, 0.0f}, core::quat_identity()},
    };
    const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
    REQUIRE(id.is_valid());
    // Authored feet sit at y = 0 and the COM at y = 1/3; place the body so the feet start on the
    // ground: body y = COM_y − (foot_y − foot_half) = 1/3 + 0.25.
    const physics::BodyId b = add_body(w, compound_shape(id), {0.0f, 1.0f / 3.0f + 0.26f, 0.0f});
    REQUIRE(b.is_valid());

    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }
    physics::BodyState st;
    REQUIRE(w.get_body_state(b, st));
    // It tips about the outer foot's edge until the cantilevered child itself LANDS, then rests
    // as a leaning tripod (~17° here) — a rigid body cannot fall flat when one of its own parts
    // catches the ground first, so the correct end state is "decisively not upright", not "flat
    // on its face". The contrast to pin: the standing dumbbell above holds up.y ≈ 1 to ~1e-5;
    // this one leaves upright by three orders of magnitude more.
    const core::Vec3 up = core::rotate(st.orientation, core::Vec3{0.0f, 1.0f, 0.0f});
    CHECK(up.y < 0.99f);
    CHECK(!core::same_rotation(st.orientation, core::quat_identity(), 5e-3f));
}

TEST_CASE("compound contacts warm-start per region, and a stack on a compound merges islands") {
    physics::PhysicsWorld w;
    add_ground(w);
    const physics::CompoundId id = register_dumbbell(w);
    REQUIRE(id.is_valid());
    const physics::BodyId b = add_body(w, compound_shape(id), {0.0f, 0.26f, 0.0f});
    // A box dropped square onto foot 1: a dynamic-dynamic contact, so the pair must merge into
    // one island (union on the PARENT bodies — children are invisible to islands).
    const physics::BodyId rider = add_body(w, box({0.2f, 0.2f, 0.2f}), {0.75f, 0.8f, 0.0f});
    REQUIRE(b.is_valid());
    REQUIRE(rider.is_valid());

    // Let the dumbbell settle into resting contact (it spawns a centimetre up), then check one
    // more tick: both floor regions persist, and their points' feature ids (child indices folded
    // in) matched last tick's region-keyed cache entries — warm starting is alive per region.
    for (int i = 0; i < 20; ++i) {
        w.step(kDt);
    }
    w.step(kDt);
    CHECK(w.contacts_warm_started_last() >= 4); // ≥ 2 points carried per resting foot region

    for (int i = 0; i < 30; ++i) {
        w.step(kDt);
    }
    CHECK(w.islands_last() == 1); // dumbbell + rider are one island on the shared floor

    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }
    CHECK(w.is_asleep(b));
    CHECK(w.is_asleep(rider));
    physics::BodyState rs;
    REQUIRE(w.get_body_state(rider, rs));
    CHECK(rs.position.y == doctest::Approx(0.7f).epsilon(0.05)); // resting on the 0.5 m foot
}

TEST_CASE("contact events name the compound child that was hit — the M8 damage signal") {
    physics::PhysicsWorld w;
    const physics::CompoundId id = register_dumbbell(w);
    REQUIRE(id.is_valid());
    // STATIC compound (created first ⇒ canonical body a), ball dropped square onto foot 1.
    const physics::BodyId d =
        add_body(w, compound_shape(id), {0.0f, 0.25f, 0.0f}, physics::MotionType::Static);
    const physics::BodyId ball = add_body(w, sphere(0.2f), {0.75f, 1.2f, 0.0f});
    REQUIRE(d.is_valid());
    REQUIRE(ball.is_valid());

    bool saw_began = false;
    bool saw_persisted = false;
    for (int i = 0; i < 120; ++i) {
        w.step(kDt);
        for (const physics::ContactEvent& e : w.contact_events()) {
            if (!(e.a == d && e.b == ball)) {
                continue;
            }
            // The region names the compound child: foot 1 took the hit — exactly what M8 needs
            // to route impulse → damage → detach to the right fracture cell.
            CHECK(e.child_a == 1);
            CHECK(e.child_b == 0);
            if (e.phase == physics::ContactPhase::Began) {
                saw_began = true;
                CHECK(e.normal_impulse > 0.0f); // the impact tick exchanged real momentum
            }
            if (e.phase == physics::ContactPhase::Persisted) {
                saw_persisted = true;
            }
        }
    }
    CHECK(saw_began);
    CHECK(saw_persisted);
}

TEST_CASE("queries treat a compound as its children: raycast and overlap_sphere") {
    physics::PhysicsWorld w;
    const physics::CompoundId id = register_dumbbell(w);
    REQUIRE(id.is_valid());
    const physics::BodyId b =
        add_body(w, compound_shape(id), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    REQUIRE(b.is_valid());

    SUBCASE("a ray down onto a foot hits its top face at the analytic distance") {
        physics::Ray ray;
        ray.origin = {0.75f, 5.0f, 0.0f};
        ray.direction = {0.0f, -1.0f, 0.0f};
        ray.max_distance = 100.0f;
        physics::RayHit hit;
        REQUIRE(w.raycast(ray, hit));
        CHECK(hit.body == b);
        CHECK(hit.distance == doctest::Approx(4.75f).epsilon(1e-4)); // foot top at y = +0.25
        CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(1e-4));
    }

    SUBCASE("a ray down through the gap between the feet hits nothing") {
        physics::Ray ray;
        ray.origin = {0.0f, 5.0f, 0.0f};
        ray.direction = {0.0f, -1.0f, 0.0f};
        ray.max_distance = 100.0f;
        physics::RayHit hit;
        CHECK_FALSE(w.raycast(ray, hit)); // the compound's union AABB spans the gap; its
                                          // children do not — per-child exactness
    }

    SUBCASE("overlap_sphere sees a foot but not the gap") {
        std::vector<physics::BodyId> hits;
        w.overlap_sphere({0.75f, 0.0f, 0.0f}, 0.1f, hits);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0] == b);
        w.overlap_sphere({0.0f, 0.0f, 0.0f}, 0.2f, hits); // gap: nearest foot face 0.5 m away
        CHECK(hits.empty());
    }
}

TEST_CASE("CCD: a fast sphere is arrested at a thin compound wall (per-child speculation)") {
    // A compound has no single support function, so speculative contacts run per child
    // (ADR-0028). The M7.10 discriminator, aimed at one panel of a two-panel compound wall:
    // 100 m/s ≈ 1.67 m/tick against a 10 cm wall — pure discrete stepping would tunnel.
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::ShapeDesc panel = box({0.05f, 1.0f, 1.0f});
    const std::vector<physics::CompoundChildDesc> kids = {
        {panel, {0.0f, 0.0f, -1.0f}, core::quat_identity()},
        {panel, {0.0f, 0.0f, 1.0f}, core::quat_identity()},
    };
    const physics::CompoundId id = w.register_compound(physics::CompoundDesc{kids});
    REQUIRE(id.is_valid());
    const physics::BodyId wall =
        add_body(w, compound_shape(id), {5.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    REQUIRE(wall.is_valid());

    physics::BodyDesc bd;
    bd.shape = sphere(0.1f);
    bd.position = {0.0f, 0.0f, -1.0f}; // dead on panel 0's centre line
    bd.linear_velocity = {100.0f, 0.0f, 0.0f};
    bd.ccd = true;
    const physics::BodyId bullet = w.create_body(bd);
    REQUIRE(bullet.is_valid());

    for (int i = 0; i < 30; ++i) {
        w.step(kDt);
    }
    physics::BodyState st;
    REQUIRE(w.get_body_state(bullet, st));
    CHECK(st.position.x > 1.0f);  // it flew
    CHECK(st.position.x < 4.96f); // …and stopped AT the near face (x = 4.95), not through it
}

TEST_CASE("determinism: a compound scene hashes bit-identically across worker counts") {
    // The ADR-0026 contract, with compounds live: dumbbells (multi-region pairs), a box stack, a
    // hull debris body, and a faller — stepped 300 ticks with sleeping OFF (every island does
    // full solver work every tick, the hard case). The child sub-pair enumeration, the region
    // cache, the folded feature ids, and the per-region events must all be invisible to thread
    // count.
    auto run_hash = [](int workers) -> std::uint64_t {
        physics::PhysicsWorld w;
        w.set_sleeping_enabled(false);
        add_ground(w);
        const physics::CompoundId dumbbell = register_dumbbell(w);
        REQUIRE(dumbbell.is_valid());
        (void)add_body(w, compound_shape(dumbbell), {-4.0f, 0.3f, 0.0f});
        (void)add_body(w, compound_shape(dumbbell), {4.0f, 0.3f, 0.0f});
        (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.5f, 0.0f});
        (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 1.5f, 0.0f});
        const physics::HullId hull = register_cube_hull(w, {0.3f, 0.3f, 0.3f});
        REQUIRE(hull.is_valid());
        (void)add_body(w, hull_shape(hull), {-4.0f, 1.2f, 0.0f}); // debris dropped on a dumbbell
        (void)add_body(w, sphere(0.4f), {4.0f, 2.0f, 0.0f});      // faller onto the other

        std::unique_ptr<core::JobSystem> js;
        if (workers >= 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        for (int i = 0; i < 300; ++i) {
            w.step(kDt);
        }
        CHECK(w.islands_last() > 1); // the parallel path had real work to hand out
        return w.world_hash();
    };

    const std::uint64_t sequential = run_hash(-1);
    CHECK(run_hash(1) == sequential);
    CHECK(run_hash(2) == sequential);
    CHECK(run_hash(4) == sequential);
}
