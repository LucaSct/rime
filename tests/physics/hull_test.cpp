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

// M7.11 proofs: convex hull collision shapes (ADR-0027). Every check is analytic or structural
// and drives the PUBLIC seam only — register_hull/hull_info, create_body, compute_contacts,
// step, queries — never the hull internals (src/hull.hpp is invisible above the seam, by
// design). The recurring trick: author a hull whose closed form we know (a cube, a regular
// tetrahedron, a pre-rotated box) and pin the engine's answers against the formula or against
// the equivalent primitive body, which the earlier bricks already proved.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

physics::ShapeDesc box(core::Vec3 half) {
    return physics::ShapeDesc{physics::ShapeType::Box, 0.0f, half, 0.0f};
}

physics::ShapeDesc sphere(float r) {
    return physics::ShapeDesc{physics::ShapeType::Sphere, r, {}, 0.0f};
}

physics::ShapeDesc capsule(float r, float half_height) {
    return physics::ShapeDesc{physics::ShapeType::Capsule, r, {}, half_height};
}

physics::ShapeDesc hull_shape(physics::HullId id) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::ConvexHull;
    s.hull = id;
    return s;
}

// A box-shaped hull: corner i is on the positive side of axis k iff bit k of i is set; six quad
// faces wound counter-clockwise viewed from outside. Optionally translated (to exercise the COM
// re-centring) or rotated (to exercise the principal-axis path).
struct CubeHullDesc {
    std::vector<core::Vec3> verts;
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> indices;
};

CubeHullDesc make_cube_desc(core::Vec3 half,
                            core::Vec3 offset = {0.0f, 0.0f, 0.0f},
                            core::Quat rot = core::quat_identity()) {
    CubeHullDesc d;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const core::Vec3 corner{(i & 1u) != 0 ? half.x : -half.x,
                                (i & 2u) != 0 ? half.y : -half.y,
                                (i & 4u) != 0 ? half.z : -half.z};
        d.verts.push_back(core::rotate(rot, corner) + offset);
    }
    d.counts = {4, 4, 4, 4, 4, 4};
    d.indices = {
        0, 4, 6, 2, // -X
        1, 3, 7, 5, // +X
        0, 1, 5, 4, // -Y
        2, 6, 7, 3, // +Y
        0, 2, 3, 1, // -Z
        4, 5, 7, 6, // +Z
    };
    return d;
}

physics::HullId register_cube(physics::PhysicsWorld& w,
                              core::Vec3 half,
                              core::Vec3 offset = {0.0f, 0.0f, 0.0f},
                              core::Quat rot = core::quat_identity()) {
    const CubeHullDesc d = make_cube_desc(half, offset, rot);
    return w.register_hull(physics::HullDesc{d.verts, d.counts, d.indices});
}

physics::BodyId add_body(physics::PhysicsWorld& w,
                         const physics::ShapeDesc& shape,
                         core::Vec3 pos,
                         physics::MotionType motion = physics::MotionType::Dynamic,
                         core::Quat q = core::quat_identity(),
                         float mass = 1.0f) {
    physics::BodyDesc d;
    d.motion = motion;
    d.shape = shape;
    d.position = pos;
    d.orientation = q;
    d.mass = mass;
    return w.create_body(d);
}

} // namespace

TEST_CASE("register_hull validates: accepts a cube, rejects broken input") {
    physics::PhysicsWorld w;
    const CubeHullDesc cube = make_cube_desc({0.5f, 0.5f, 0.5f});

    SUBCASE("a well-formed cube registers and reads back") {
        const physics::HullId id =
            w.register_hull(physics::HullDesc{cube.verts, cube.counts, cube.indices});
        REQUIRE(id.is_valid());
        physics::HullInfo info;
        REQUIRE(w.hull_info(id, info));
        CHECK(info.volume == doctest::Approx(1.0f).epsilon(1e-4));

        // A second registration is a distinct id (the store is append-only, ids are
        // registration order).
        const physics::HullId id2 =
            w.register_hull(physics::HullDesc{cube.verts, cube.counts, cube.indices});
        REQUIRE(id2.is_valid());
        CHECK(id2 != id);
    }

    SUBCASE("an open mesh (a face removed) is rejected") {
        std::vector<std::uint32_t> counts(cube.counts.begin(), cube.counts.end() - 1);
        std::vector<std::uint32_t> indices(cube.indices.begin(), cube.indices.end() - 4);
        CHECK_FALSE(w.register_hull(physics::HullDesc{cube.verts, counts, indices}).is_valid());
    }

    SUBCASE("inconsistent winding (one face reversed) is rejected") {
        std::vector<std::uint32_t> indices = cube.indices;
        std::swap(indices[0], indices[3]); // -X face 0,4,6,2 -> 2,4,6,0: reversed traversal
        std::swap(indices[1], indices[2]);
        CHECK_FALSE(
            w.register_hull(physics::HullDesc{cube.verts, cube.counts, indices}).is_valid());
    }

    SUBCASE("a vertex outside the faces (non-convex set) is rejected") {
        std::vector<core::Vec3> verts = cube.verts;
        verts.push_back({2.0f, 0.0f, 0.0f}); // in front of the +X face plane
        CHECK_FALSE(
            w.register_hull(physics::HullDesc{verts, cube.counts, cube.indices}).is_valid());
    }

    SUBCASE("an out-of-range index is rejected") {
        std::vector<std::uint32_t> indices = cube.indices;
        indices[5] = 99;
        CHECK_FALSE(
            w.register_hull(physics::HullDesc{cube.verts, cube.counts, indices}).is_valid());
    }

    SUBCASE("hull_info on a null/unknown id reports false") {
        physics::HullInfo info;
        CHECK_FALSE(w.hull_info(physics::HullId{}, info));
        CHECK_FALSE(w.hull_info(physics::HullId{42, 0}, info));
    }

    SUBCASE("a hull-shaped body with an unresolvable id is refused") {
        physics::BodyDesc d;
        d.shape = hull_shape(physics::HullId{});
        CHECK_FALSE(w.create_body(d).is_valid());
        CHECK(w.body_count() == 0);
    }
}

TEST_CASE("cube hull mass properties match the box closed form") {
    physics::PhysicsWorld w;
    // Distinct extents so the three principal moments are distinct (no accidental symmetry).
    const core::Vec3 half{0.2f, 0.4f, 0.6f};
    const physics::HullId id = register_cube(w, half);
    REQUIRE(id.is_valid());
    physics::HullInfo info;
    REQUIRE(w.hull_info(id, info));

    // Volume (2hx)(2hy)(2hz), centroid at the authored origin.
    CHECK(info.volume == doctest::Approx(8.0f * half.x * half.y * half.z).epsilon(1e-4));
    CHECK(std::fabs(info.centroid.x) < 1e-5f);
    CHECK(std::fabs(info.centroid.y) < 1e-5f);
    CHECK(std::fabs(info.centroid.z) < 1e-5f);

    // Principal moments per unit mass = the box formula I/m = (h_b² + h_c²)/3. The authored
    // frame is already principal (an axis-aligned box), so Jacobi should do nothing: identity
    // rotation, moments in axis order.
    CHECK(info.inertia_per_mass.x ==
          doctest::Approx((half.y * half.y + half.z * half.z) / 3.0f).epsilon(1e-3));
    CHECK(info.inertia_per_mass.y ==
          doctest::Approx((half.x * half.x + half.z * half.z) / 3.0f).epsilon(1e-3));
    CHECK(info.inertia_per_mass.z ==
          doctest::Approx((half.x * half.x + half.y * half.y) / 3.0f).epsilon(1e-3));
    CHECK(core::same_rotation(info.principal_rotation, core::quat_identity(), 1e-4f));
}

TEST_CASE("an off-centre hull re-centres on its centre of mass") {
    physics::PhysicsWorld w;
    const core::Vec3 offset{1.5f, -2.0f, 0.75f};
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f}, offset);
    REQUIRE(id.is_valid());
    physics::HullInfo info;
    REQUIRE(w.hull_info(id, info));

    // The reported centroid is the authored-frame COM — exactly the shift registration applied.
    CHECK(info.centroid.x == doctest::Approx(offset.x).epsilon(1e-4));
    CHECK(info.centroid.y == doctest::Approx(offset.y).epsilon(1e-4));
    CHECK(info.centroid.z == doctest::Approx(offset.z).epsilon(1e-4));
    // Inertia is about the COM, so the offset must NOT have leaked in (no parallel-axis bloat).
    CHECK(info.inertia_per_mass.x == doctest::Approx(0.5f / 3.0f).epsilon(1e-3));

    // And dynamically the body behaves as a centred cube: dropped over a floor, its position
    // (== its COM) rests one half-extent above the floor top, like the centred twin.
    physics::PhysicsWorld w2;
    const physics::HullId centred = register_cube(w2, {0.5f, 0.5f, 0.5f});
    (void)add_body(w2, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    const physics::BodyId shifted = add_body(w2, hull_shape(centred), {0.0f, 1.2f, 0.0f});
    (void)add_body(w, box({5.0f, 0.5f, 5.0f}), {8.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    const physics::BodyId off = add_body(w, hull_shape(id), {8.0f, 1.2f, 0.0f});
    for (int i = 0; i < 240; ++i) {
        w.step(kDt);
        w2.step(kDt);
    }
    physics::BodyState sa;
    physics::BodyState sb;
    REQUIRE(w.get_body_state(off, sa));
    REQUIRE(w2.get_body_state(shifted, sb));
    CHECK(sa.position.y == doctest::Approx(sb.position.y).epsilon(1e-3));
}

TEST_CASE("regular tetrahedron: analytic volume and isotropic inertia") {
    physics::PhysicsWorld w;
    // The unit-coordinate regular tetrahedron (edge 2√2): V = 8/3, I = m·a²/20 = 0.4·m about
    // every axis through the COM (which is the origin by symmetry).
    const std::vector<core::Vec3> verts = {
        {1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {-1.0f, -1.0f, 1.0f}};
    const std::vector<std::uint32_t> counts = {3, 3, 3, 3};
    const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2};
    const physics::HullId id = w.register_hull(physics::HullDesc{verts, counts, indices});
    REQUIRE(id.is_valid());
    physics::HullInfo info;
    REQUIRE(w.hull_info(id, info));

    CHECK(info.volume == doctest::Approx(8.0f / 3.0f).epsilon(1e-4));
    CHECK(std::fabs(info.centroid.x) < 1e-5f);
    CHECK(info.inertia_per_mass.x == doctest::Approx(0.4f).epsilon(1e-3));
    CHECK(info.inertia_per_mass.y == doctest::Approx(0.4f).epsilon(1e-3));
    CHECK(info.inertia_per_mass.z == doctest::Approx(0.4f).epsilon(1e-3));
}

TEST_CASE("a hull cube rests at the box's analytic height") {
    // Two identical scenes, one body a box primitive, the other the SAME solid as a hull. Both
    // must settle at COM height = floor top + half extent (± the solver's 5 mm slop), and at the
    // SAME height as each other far more tightly than the slop — the hull path reproduces the
    // box path's statics, not merely "roughly rests".
    physics::PhysicsWorld w;
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    REQUIRE(id.is_valid());
    (void)add_body(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    const physics::BodyId hull_body = add_body(w, hull_shape(id), {0.0f, 1.6f, 0.0f});
    const physics::BodyId box_body = add_body(w, box({0.5f, 0.5f, 0.5f}), {3.0f, 1.6f, 0.0f});

    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }

    physics::BodyState sh;
    physics::BodyState sb;
    REQUIRE(w.get_body_state(hull_body, sh));
    REQUIRE(w.get_body_state(box_body, sb));
    CHECK(sh.position.y == doctest::Approx(1.0f).epsilon(0.01)); // 0.5 floor top + 0.5 half
    CHECK(sh.position.y == doctest::Approx(sb.position.y).epsilon(2e-3));
    CHECK(std::fabs(sh.linear_velocity.y) < 0.05f); // at rest, not bouncing
}

TEST_CASE("hull-hull face contact: four points, analytic normal and depth") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    REQUIRE(id.is_valid());
    // B overlaps A's top face by 0.1, laterally offset so the clipped patch is unambiguous.
    (void)add_body(w, hull_shape(id), {0.0f, 0.0f, 0.0f}); // slot 0 => manifold body a
    (void)add_body(w, hull_shape(id), {0.3f, 0.9f, 0.0f}); // slot 1 => body b

    std::vector<physics::Manifold> ms;
    w.compute_contacts(ms);
    REQUIRE(ms.size() == 1);
    const physics::Manifold& m = ms[0];
    CHECK(m.count == 4); // a face-on-face patch needs four points or the stack wobbles
    CHECK(m.normal.y == doctest::Approx(1.0f).epsilon(1e-3)); // a -> b is +Y (b sits above)
    for (std::uint8_t i = 0; i < m.count; ++i) {
        CHECK(m.points[i].penetration == doctest::Approx(0.1f).epsilon(0.02));
        // The patch is the overlap rectangle x in [-0.2, 0.5], z in [-0.5, 0.5] at y ~ 0.45.
        CHECK(m.points[i].position.y == doctest::Approx(0.45f).epsilon(0.02));
        CHECK(m.points[i].position.x > -0.25f);
        CHECK(m.points[i].position.x < 0.55f);
    }

    // Distinct feature ids within the manifold (the cache matches on them next frame).
    for (std::uint8_t i = 0; i < m.count; ++i) {
        for (std::uint8_t j = static_cast<std::uint8_t>(i + 1); j < m.count; ++j) {
            CHECK(m.points[i].feature_id != m.points[j].feature_id);
        }
    }
}

TEST_CASE("box vs hull and sphere/capsule vs hull manifolds") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    REQUIRE(id.is_valid());

    SUBCASE("box (primitive) on hull: the general convex path, four points") {
        (void)add_body(w, box({0.5f, 0.5f, 0.5f}), {0.0f, 0.0f, 0.0f}); // a: the box
        (void)add_body(w, hull_shape(id), {0.3f, 0.9f, 0.0f});          // b: the hull, above
        std::vector<physics::Manifold> ms;
        w.compute_contacts(ms);
        REQUIRE(ms.size() == 1);
        CHECK(ms[0].count == 4);
        CHECK(ms[0].normal.y == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(ms[0].points[0].penetration == doctest::Approx(0.1f).epsilon(0.02));
    }

    SUBCASE("sphere sunk into a hull face: one point, radial depth") {
        (void)add_body(w, sphere(0.5f), {0.0f, 0.9f, 0.0f});   // a: sphere above the top face
        (void)add_body(w, hull_shape(id), {0.0f, 0.0f, 0.0f}); // b: the hull
        std::vector<physics::Manifold> ms;
        w.compute_contacts(ms);
        REQUIRE(ms.size() == 1);
        CHECK(ms[0].count == 1);
        CHECK(ms[0].normal.y == doctest::Approx(-1.0f).epsilon(1e-3)); // sphere -> hull is -Y
        CHECK(ms[0].points[0].penetration == doctest::Approx(0.1f).epsilon(0.02));
        CHECK(ms[0].points[0].position.y == doctest::Approx(0.45f).epsilon(0.02));
    }

    SUBCASE("capsule lying into a hull face: contact with the cylinder side") {
        // Horizontal capsule (axis along X) sunk 0.05 into the top face.
        const core::Quat lie = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.5707963f);
        (void)add_body(
            w, capsule(0.3f, 0.5f), {0.0f, 0.75f, 0.0f}, physics::MotionType::Dynamic, lie);
        (void)add_body(w, hull_shape(id), {0.0f, 0.0f, 0.0f});
        std::vector<physics::Manifold> ms;
        w.compute_contacts(ms);
        REQUIRE(ms.size() == 1);
        CHECK(ms[0].count == 1);
        CHECK(ms[0].normal.y == doctest::Approx(-1.0f).epsilon(1e-2)); // capsule -> hull is -Y
        CHECK(ms[0].points[0].penetration == doctest::Approx(0.05f).epsilon(0.3));
    }
}

TEST_CASE("hull contacts warm-start: feature ids are frame-stable") {
    physics::PhysicsWorld w;
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    (void)add_body(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    (void)add_body(w, hull_shape(id), {0.0f, 1.0f, 0.0f}); // starts exactly resting

    for (int i = 0; i < 30; ++i) {
        w.step(kDt);
    }
    // After settling, every point of the resting manifold should match last tick's by feature
    // id — the witness that the hull clipping path's ids are pure functions of contact topology.
    w.step(kDt);
    CHECK(w.contacts_warm_started_last() >= 3);
}

TEST_CASE("principal axes: a pre-rotated hull responds like the rotated box primitive") {
    // The same physical solid two ways: (a) a box primitive posed with orientation R, (b) a hull
    // whose vertices were AUTHORED pre-rotated by R, posed with identity. (b)'s inertia tensor is
    // non-diagonal in its body frame, so this is the end-to-end proof of the registration-time
    // eigendecomposition + the solver's principal-axis compose: the same off-centre impulse must
    // produce the same world-space velocity response, whatever basis Jacobi happened to pick.
    const core::Quat rot =
        core::quat_from_axis_angle(core::normalize(core::Vec3{1.0f, 1.0f, 0.3f}), 0.7f);
    const core::Vec3 half{0.2f, 0.4f, 0.6f};
    const float mass = 3.0f;
    const core::Vec3 impulse{0.0f, 2.0f, 1.0f};
    const core::Vec3 point{0.9f, 0.4f, 0.2f}; // off-COM world point => real torque

    physics::PhysicsWorld wa;
    wa.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::BodyId a =
        add_body(wa, box(half), {0.0f, 0.0f, 0.0f}, physics::MotionType::Dynamic, rot, mass);
    wa.apply_impulse(a, impulse, point);

    physics::PhysicsWorld wb;
    wb.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::HullId id = register_cube(wb, half, {0.0f, 0.0f, 0.0f}, rot);
    REQUIRE(id.is_valid());
    const physics::BodyId b = add_body(wb,
                                       hull_shape(id),
                                       {0.0f, 0.0f, 0.0f},
                                       physics::MotionType::Dynamic,
                                       core::quat_identity(),
                                       mass);
    wb.apply_impulse(b, impulse, point);

    physics::BodyState sa;
    physics::BodyState sb;
    REQUIRE(wa.get_body_state(a, sa));
    REQUIRE(wb.get_body_state(b, sb));
    CHECK(sa.linear_velocity.y == doctest::Approx(sb.linear_velocity.y).epsilon(1e-4));
    CHECK(sa.angular_velocity.x == doctest::Approx(sb.angular_velocity.x).epsilon(2e-3));
    CHECK(sa.angular_velocity.y == doctest::Approx(sb.angular_velocity.y).epsilon(2e-3));
    CHECK(sa.angular_velocity.z == doctest::Approx(sb.angular_velocity.z).epsilon(2e-3));
}

TEST_CASE("a stack of hull cubes stands still") {
    physics::PhysicsWorld w;
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    (void)add_body(w, box({5.0f, 0.5f, 5.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
    std::vector<physics::BodyId> stack;
    for (int i = 0; i < 3; ++i) {
        stack.push_back(
            add_body(w, hull_shape(id), {0.0f, 1.05f + 1.01f * static_cast<float>(i), 0.0f}));
    }
    for (int i = 0; i < 300; ++i) {
        w.step(kDt);
    }
    for (int i = 0; i < 3; ++i) {
        physics::BodyState s;
        REQUIRE(w.get_body_state(stack[static_cast<std::size_t>(i)], s));
        // Each cube rests near floor_top + half + i (each contact may keep up to the 5 mm slop).
        CHECK(s.position.y == doctest::Approx(1.0f + static_cast<float>(i)).epsilon(0.03));
        CHECK(std::fabs(s.position.x) < 0.1f); // no sideways creep: the 4-point patches hold
        CHECK(std::fabs(s.position.z) < 0.1f);
    }
}

TEST_CASE("determinism: a hull scene hashes identically across worker counts") {
    // The M7.5 contract extended over the new shape paths: same inputs => bit-identical
    // world_hash(), sequential and at any worker count. Two islands of mixed hull/primitive
    // bodies keep the parallel solve honest.
    const auto run = [](int workers) {
        physics::PhysicsWorld w;
        const physics::HullId cube = register_cube(w, {0.5f, 0.5f, 0.5f});
        const physics::HullId slab = register_cube(w, {0.4f, 0.2f, 0.3f});
        (void)add_body(
            w, box({10.0f, 0.5f, 10.0f}), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);
        // Island 1: hull stack + a sphere dropping onto it.
        (void)add_body(w, hull_shape(cube), {0.0f, 1.05f, 0.0f});
        (void)add_body(w, hull_shape(slab), {0.1f, 2.0f, 0.05f});
        (void)add_body(w, sphere(0.4f), {0.0f, 3.2f, 0.0f});
        // Island 2, far away: hull vs box vs capsule pile.
        (void)add_body(w, hull_shape(cube), {6.0f, 1.05f, 0.0f});
        (void)add_body(w, box({0.3f, 0.3f, 0.3f}), {6.1f, 2.0f, 0.1f});
        (void)add_body(w, capsule(0.25f, 0.4f), {5.9f, 3.0f, 0.0f});

        std::unique_ptr<core::JobSystem> js;
        if (workers > 0) {
            js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
            w.set_job_system(js.get());
        }
        for (int i = 0; i < 240; ++i) {
            w.step(kDt);
        }
        w.set_job_system(nullptr);
        return w.world_hash();
    };

    const std::uint64_t sequential = run(0);
    CHECK(run(1) == sequential);
    CHECK(run(2) == sequential);
    CHECK(run(4) == sequential);
}

TEST_CASE("raycast and overlap_sphere see hull bodies") {
    physics::PhysicsWorld w;
    const physics::HullId id = register_cube(w, {0.5f, 0.5f, 0.5f});
    const physics::BodyId body =
        add_body(w, hull_shape(id), {0.0f, 0.0f, 0.0f}, physics::MotionType::Static);

    SUBCASE("a ray hits the +X face at the analytic distance and normal") {
        physics::Ray ray;
        ray.origin = {2.0f, 0.1f, 0.1f};
        ray.direction = {-1.0f, 0.0f, 0.0f};
        physics::RayHit hit;
        REQUIRE(w.raycast(ray, hit));
        CHECK(hit.body == body);
        CHECK(hit.distance == doctest::Approx(1.5f).epsilon(1e-3));
        CHECK(hit.normal.x == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(hit.point.x == doctest::Approx(0.5f).epsilon(1e-3));
    }

    SUBCASE("a ray past the corner misses") {
        physics::Ray ray;
        ray.origin = {2.0f, 2.0f, 0.0f};
        ray.direction = {-1.0f, 0.0f, 0.0f};
        physics::RayHit hit;
        CHECK_FALSE(w.raycast(ray, hit));
    }

    SUBCASE("overlap_sphere includes the hull exactly within reach") {
        std::vector<physics::BodyId> out;
        w.overlap_sphere({1.0f, 0.0f, 0.0f}, 0.6f, out); // face at x=0.5, gap 0.5 < 0.6
        REQUIRE(out.size() == 1);
        CHECK(out[0] == body);
        w.overlap_sphere({1.0f, 0.0f, 0.0f}, 0.4f, out); // gap 0.5 > 0.4
        CHECK(out.empty());
    }
}

TEST_CASE("CCD: a fast hull bullet is arrested at a thin wall") {
    physics::PhysicsWorld w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    const physics::HullId id = register_cube(w, {0.05f, 0.05f, 0.05f});
    (void)add_body(w, box({0.05f, 2.0f, 2.0f}), {3.0f, 0.0f, 0.0f}, physics::MotionType::Static);

    physics::BodyDesc d;
    d.shape = hull_shape(id);
    d.position = {0.0f, 0.0f, 0.0f};
    d.linear_velocity = {100.0f, 0.0f, 0.0f}; // ~1.67 m per step vs a 10 cm wall
    d.ccd = true;
    const physics::BodyId bullet = w.create_body(d);
    REQUIRE(bullet.is_valid());

    for (int i = 0; i < 30; ++i) {
        w.step(kDt);
    }
    physics::BodyState s;
    REQUIRE(w.get_body_state(bullet, s));
    CHECK(s.position.x < 3.0f);                   // never crossed the wall plane
    CHECK(s.position.x > 2.5f);                   // and it reached the wall, not a false stop
    CHECK(std::fabs(s.linear_velocity.x) < 1.0f); // arrested
}
