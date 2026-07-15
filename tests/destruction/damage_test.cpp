// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/physics.hpp"

// M8.3 proofs: damage → connectivity → the fracture body swap (ADR-0029 §2–§4), all GPU-free (CPU
// geometry in, physics bodies out). The suite leans on two kinds of fixture:
//
//  - IN-CODE grids of box parts with hand-chosen bonds/anchors — "paper fixtures" whose correct
//    fracture is checkable by eye (kill THIS support ⇒ exactly THAT island detaches). Vertical-
//    only bonds make each column an independent support chain, which is what makes the expected
//    islands unambiguous.
//  - the COOKED Voronoi walls (wall.rdest's 16 parts, wall_100.rdest's 100) for scale and for the
//    M11 determinism contract: scripted damage on real fracture geometry, byte-identical outcomes
//    across runs, physics worker counts, and same-tick input arrival order.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::vector<std::byte> read_fixture(const std::string& name) {
    const std::string path = std::string(RIME_DESTRUCTION_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open fixture: " << path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

assets::DestructibleAsset load_asset(const std::string& name) {
    const std::vector<std::byte> file = read_fixture(name);
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    REQUIRE_MESSAGE(asset.has_value(), name << " failed to decode: " << assets::to_string(err));
    return std::move(*asset);
}

// A box-hull part for in-code patterns: 8 COM-centred corners (bit k of corner i picks the +/−
// face on axis k — the hull_test cube), six outward-wound quads, exact volume and bounds. This is
// the cook's output shape, hand-made small.
assets::DestructiblePart make_box_part(core::Vec3 half, core::Vec3 com) {
    assets::DestructiblePart part;
    part.com = com;
    part.aabb_min = com - half;
    part.aabb_max = com + half;
    part.volume = 8.0f * half.x * half.y * half.z;
    for (std::uint32_t i = 0; i < 8; ++i) {
        part.vertices.push_back({(i & 1u) != 0 ? half.x : -half.x,
                                 (i & 2u) != 0 ? half.y : -half.y,
                                 (i & 4u) != 0 ? half.z : -half.z});
    }
    part.face_counts = {4, 4, 4, 4, 4, 4};
    part.face_indices = {
        0, 4, 6, 2, // -X
        1, 3, 7, 5, // +X
        0, 1, 5, 4, // -Y
        2, 6, 7, 3, // +Y
        0, 2, 3, 1, // -Z
        4, 5, 7, 6, // +Z
    };
    return part;
}

// An nx × ny grid of identical box parts standing on y = 0, VERTICAL bonds only (each column is
// its own support chain — the paper-fixture property), anchored along the bottom row. Part ids
// are row-major from the bottom: p = y·nx + x.
assets::DestructibleAsset
make_grid(int nx, int ny, core::Vec3 half, float damage_threshold, float damage_scale) {
    assets::DestructibleAsset asset;
    asset.half_extents = {static_cast<float>(nx) * half.x, static_cast<float>(ny) * half.y, half.z};
    asset.damage_threshold = damage_threshold;
    asset.damage_scale = damage_scale;
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const core::Vec3 com{(static_cast<float>(x) - static_cast<float>(nx - 1) * 0.5f) *
                                     2.0f * half.x,
                                 (static_cast<float>(y) + 0.5f) * 2.0f * half.y,
                                 0.0f};
            asset.parts.push_back(make_box_part(half, com));
        }
    }
    for (int y = 0; y + 1 < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(y * nx + x);
            asset.bonds.push_back({a, a + static_cast<std::uint32_t>(nx), 1.0f});
        }
    }
    for (int x = 0; x < nx; ++x) {
        asset.anchors.push_back(static_cast<std::uint32_t>(x));
    }
    return asset;
}

physics::BodyId add_ground(physics::PhysicsWorld& w, float top_y = 0.0f) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape.type = physics::ShapeType::Box;
    d.shape.half_extents = {50.0f, 0.5f, 50.0f};
    d.position = {0.0f, top_y - 0.5f, 0.0f};
    return w.create_body(d);
}

// The island compositions, in creation order — the "assert compositions, not counts" helper.
std::vector<std::vector<std::uint32_t>>
debris_compositions(const destruction::DestructionWorld& dw) {
    std::vector<std::vector<std::uint32_t>> out;
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        const auto parts = dw.debris_parts(i);
        out.emplace_back(parts.begin(), parts.end());
    }
    return out;
}

// The index of the debris body whose composition contains `part`, or SIZE_MAX. Since the true-up
// (ADR-0029 §2) a directly-killed part detaches as its own debris body alongside any orphaned
// island, so a test that knows a part left names its piece by that part rather than by a fixed
// roster slot — robust to the canonical-but-shifted creation order.
std::size_t debris_with_part(const destruction::DestructionWorld& dw, std::uint32_t part) {
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        const auto parts = dw.debris_parts(i);
        if (std::find(parts.begin(), parts.end(), part) != parts.end()) {
            return i;
        }
    }
    return SIZE_MAX;
}

} // namespace

TEST_CASE("M8.3 paper fixture: kill a support ⇒ exactly the expected island detaches") {
    // The hand-checkable 2×2: parts 0,1 are the anchored bottom, 2 sits on 0, 3 sits on 1 (bonds
    // 0–2 and 1–3 only). Killing 0 must detach EXACTLY {2}; nothing else moves.
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    REQUIRE(pattern.is_valid());
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    REQUIRE(inst.is_valid());
    const physics::BodyId intact_body = dw.body_of(inst);
    const std::uint64_t hash_before = dw.state_hash();

    // Enough damage at part 0's COM to kill it; radius 0.2 stays inside its own bounds (the
    // nearest neighbour bound is 0.25 away), so this op names ONE part.
    dw.apply_damage(inst, core::Vec3{-0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);

    // Part 0 killed (now its own debris); part 2 (its dependant) detached as an island; 1, 3 stand.
    CHECK_FALSE(dw.part_alive(inst, 0));
    CHECK(dw.part_health(inst, 0) == doctest::Approx(0.0f)); // killed: health spent
    CHECK_FALSE(dw.part_alive(inst, 2));
    CHECK(dw.part_health(inst, 2) == doctest::Approx(1.0f)); // detached: health frozen intact
    CHECK(dw.part_alive(inst, 1));
    CHECK(dw.part_alive(inst, 3));

    // Two debris, canonical order by smallest member: the struck part 0 flew off as its own hull
    // body (ADR-0029 §2), then its dependant 2 detached as an island. Compositions, not counts.
    REQUIRE(dw.debris_count() == 2);
    REQUIRE(dw.debris_parts(0).size() == 1);
    CHECK(dw.debris_parts(0)[0] == 0);
    REQUIRE(dw.debris_parts(1).size() == 1);
    CHECK(dw.debris_parts(1)[0] == 2);
    CHECK(dw.debris_source(0) == inst);
    CHECK(dw.debris_source(1) == inst);
    CHECK(pw.is_alive(dw.debris_body(0)));
    CHECK(pw.is_alive(dw.debris_body(1)));

    // The body swap happened: a NEW static body stands for the remainder {1, 3}, and the child →
    // part remap is those ids in ascending order (ADR-0029 §4).
    const physics::BodyId remainder_body = dw.body_of(inst);
    CHECK(pw.is_alive(remainder_body));
    CHECK_FALSE(remainder_body == intact_body);
    CHECK_FALSE(pw.is_alive(intact_body)); // the old compound body is gone
    CHECK(dw.part_from_child(inst, 0) == 1);
    CHECK(dw.part_from_child(inst, 1) == 3);
    CHECK(dw.part_from_child(inst, 2) == destruction::kInvalidPartIndex);

    // The island {2} is a real dynamic body: step and it falls; the anchored remainder does not.
    physics::BodyState before{};
    REQUIRE(pw.get_body_state(dw.debris_body(1), before));
    for (int i = 0; i < 30; ++i) {
        pw.step(kDt);
        dw.update(pw);
    }
    physics::BodyState after{};
    REQUIRE(pw.get_body_state(dw.debris_body(1), after));
    CHECK(after.position.y < before.position.y - 0.05f); // fell
    CHECK(pw.is_alive(dw.body_of(inst)));                // the wall stump still stands

    CHECK(dw.state_hash() != hash_before); // the fingerprint saw all of it
}

TEST_CASE("M8.3 paper fixture: kill a non-critical part ⇒ only the struck chunk leaves, no island "
          "(and the hole is real)") {
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    // Part 3 is a top part: killing it strands nobody (2 still hangs on anchor 0).
    const core::Vec3 part3_com{0.25f, 0.75f, 0.0f};
    physics::RayHit pre;
    REQUIRE(pw.raycast(physics::Ray{{part3_com.x, part3_com.y, 5.0f}, {0.0f, 0.0f, -1.0f}}, pre));
    CHECK(dw.part_from_child(inst, pre.child) == 3); // the ray names the part (RayHit::child)

    dw.apply_damage(inst, part3_com, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);

    // The struck part left the wall as its OWN debris body (ADR-0029 §2) — but it stranded nobody,
    // so there is no *island*: parts 0, 1, 2 stand untouched.
    REQUIRE(dw.debris_count() == 1);
    CHECK(dw.debris_parts(0).size() == 1);
    CHECK(dw.debris_parts(0)[0] == 3);
    CHECK_FALSE(dw.part_alive(inst, 3));
    CHECK(dw.part_alive(inst, 0));
    CHECK(dw.part_alive(inst, 1));
    CHECK(dw.part_alive(inst, 2));
    // …and it left a real hole in the WALL: the rebuilt remainder compound no longer occludes where
    // part 3 stood (a departed part must stop colliding, or every "hole" would be a ghost). The
    // freed chunk is born in place (the no-pop invariant) and — carrying no impulse — simply rests
    // there on its old neighbour, so the ray still meets a body; but that body is the loose debris,
    // never the standing wall.
    const physics::BodyId wall = dw.body_of(inst);
    physics::RayHit post;
    REQUIRE(pw.raycast(physics::Ray{{part3_com.x, part3_com.y, 5.0f}, {0.0f, 0.0f, -1.0f}}, post));
    CHECK(post.body != wall);
    CHECK(post.body == dw.debris_body(0));
}

TEST_CASE("M8.3: a directly-killed part flies off carrying its killing impulse (ADR-0029 §2)") {
    // The true-up's core behaviour: killing a part with a push does not just erase it — the freed
    // chunk leaves along that push. Part 3 (top-right, non-critical) is killed by an op whose
    // impulse points +X; its debris body must depart in +X, from rest (a static wall imparts none).
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    // A point blast at part 3's COM with a strong +X push; radius 0.2 keeps it to part 3 alone.
    dw.apply_damage(
        inst, core::Vec3{0.25f, 0.75f, 0.0f}, 0.2f, 10.0f, core::Vec3{500.0f, 0.0f, 0.0f});
    dw.update(pw);

    // Part 3 left as its own debris body, launched in +X by the impulse that killed it — read the
    // birth velocity directly (no step yet, so gravity has not entered).
    const std::size_t d3 = debris_with_part(dw, 3);
    REQUIRE(d3 != SIZE_MAX);
    REQUIRE(dw.debris_parts(d3).size() == 1);
    physics::BodyState born{};
    REQUIRE(pw.get_body_state(dw.debris_body(d3), born));
    CHECK(born.linear_velocity.x > 5.0f); // carried the +X killing impulse, not zero
    CHECK(std::abs(born.linear_velocity.y) < born.linear_velocity.x); // dominantly along the push
    CHECK(std::abs(born.linear_velocity.z) < born.linear_velocity.x);
}

TEST_CASE("M8.3 paper fixture: a multi-part island detaches as ONE dynamic compound") {
    // A single 1×3 column anchored at the base: kill the anchor and {1, 2} must leave TOGETHER as
    // one body (the ratified decision: islands keep their shape — the Frostbite look).
    const assets::DestructibleAsset asset = make_grid(1, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    dw.apply_damage(inst, core::Vec3{0.0f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);

    // Two debris (canonical order by smallest member): the killed anchor 0 as its own hull body,
    // then {1, 2} riding off TOGETHER as one dynamic compound (the ratified decision: an island
    // keeps its shape — the Frostbite look — not two loose parts).
    REQUIRE(dw.debris_count() == 2);
    CHECK(dw.debris_parts(0).size() == 1);
    CHECK(dw.debris_parts(0)[0] == 0);
    const auto island = dw.debris_parts(1);
    REQUIRE(island.size() == 2);
    CHECK(island[0] == 1);
    CHECK(island[1] == 2);
    CHECK_FALSE(dw.body_of(inst).is_valid()); // no anchored part left ⇒ nothing stands
    CHECK(dw.part_from_child(inst, 0) == destruction::kInvalidPartIndex);

    // Both are real dynamic bodies; {1, 2} ride ONE rigid body (that single body id is the
    // statement — no render leaves to assert per-part arrangement).
    CHECK(pw.is_alive(dw.debris_body(0)));
    CHECK(pw.is_alive(dw.debris_body(1)));
    CHECK(pw.body_count() ==
          2); // the killed chunk {0} + the island compound {1, 2}; nothing stands
}

TEST_CASE("M8.3: damage on one instance never leaks to another instance of the same pattern") {
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    core::Transform at_a;
    at_a.translation = {-5.0f, 0.0f, 0.0f};
    core::Transform at_b;
    at_b.translation = {5.0f, 0.0f, 0.0f};
    const destruction::InstanceId a = dw.spawn(pattern, at_a, pw);
    const destruction::InstanceId b = dw.spawn(pattern, at_b, pw);

    dw.apply_damage(b, core::Vec3{5.0f - 0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);

    CHECK(dw.debris_count() == 2); // the killed part 0 and the orphaned island {2}, both from b
    CHECK(dw.debris_source(0) == b);
    CHECK(dw.debris_source(1) == b);
    for (std::uint32_t p = 0; p < 4; ++p) {
        CHECK(dw.part_alive(a, p)); // instance A untouched — per-instance state, shared pattern
    }
    CHECK_FALSE(dw.part_alive(b, 0));
}

TEST_CASE("M8.3 no-pop: surfaces stay put across the body swap") {
    // The crux of the swap (brief §5): the rebuilt remainder compound and the newborn debris must
    // stand EXACTLY where their parts stood — a wrong COM recipe teleports the wall on the
    // fracture tick. Proven with raycasts against a 3×3 wall placed off-origin: the surviving
    // column's face answers identically before and after, and the debris body is born at its
    // part's placement.
    const assets::DestructibleAsset asset = make_grid(3, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    core::Transform place;
    place.translation = {2.0f, 1.0f, -3.0f}; // off-origin so the placement recipe is exercised
    const destruction::InstanceId inst = dw.spawn(pattern, place, pw);

    // Column 0's top face, from above: part 6 (row 2, x = −0.5 local). Wall top at local y = 1.5.
    const core::Vec3 col0_top_origin = place.translation + core::Vec3{-0.5f, 5.0f, 0.0f};
    physics::RayHit pre;
    REQUIRE(pw.raycast(physics::Ray{col0_top_origin, {0.0f, -1.0f, 0.0f}}, pre));
    CHECK(pre.body == dw.body_of(inst));
    CHECK(dw.part_from_child(inst, pre.child) == 6);
    CHECK(pre.point.y == doctest::Approx(place.translation.y + 1.5f).epsilon(1e-3));

    // Kill part 4 (column 1's middle): {7} above it detaches, columns 0 and 2 must not move.
    const core::Vec3 part7_before = dw.part_placement(inst, 7).translation;
    dw.apply_damage(
        inst, place.translation + core::Vec3{0.0f, 0.75f, 0.0f}, 0.2f, 10.0f, core::Vec3{});
    dw.update(pw);

    // Two debris: the killed part 4 flew off, and part 7 (which it supported) detached above the
    // hole. The {7} island's birth spot is the no-pop witness.
    REQUIRE(dw.debris_count() == 2);
    REQUIRE(debris_with_part(dw, 4) != SIZE_MAX); // the struck part is its own debris now
    const std::size_t d7 = debris_with_part(dw, 7);
    REQUIRE(d7 != SIZE_MAX);
    REQUIRE(dw.debris_parts(d7).size() == 1);
    CHECK(dw.debris_parts(d7)[0] == 7);

    // (a) The surviving wall presents the identical surface: same ray, same face height, and the
    // child → part remap still names part 6 through the NEW body.
    physics::RayHit post;
    REQUIRE(pw.raycast(physics::Ray{col0_top_origin, {0.0f, -1.0f, 0.0f}}, post));
    CHECK(post.body == dw.body_of(inst));
    CHECK(dw.part_from_child(inst, post.child) == 6);
    CHECK(post.point.y == doctest::Approx(pre.point.y).epsilon(1e-4));
    CHECK(post.distance == doctest::Approx(pre.distance).epsilon(1e-4));

    // (b) The {7} debris body is born exactly at its part's pre-break placement (position IS the
    // part's COM — the hull was cooked COM-centred), with no step in between to blur it.
    physics::BodyState debris_state{};
    REQUIRE(pw.get_body_state(dw.debris_body(d7), debris_state));
    CHECK(debris_state.position.x == doctest::Approx(part7_before.x).epsilon(1e-4));
    CHECK(debris_state.position.y == doctest::Approx(part7_before.y).epsilon(1e-4));
    CHECK(debris_state.position.z == doctest::Approx(part7_before.z).epsilon(1e-4));

    // (c) A ray at the killed part's old spot no longer meets the WALL: the rebuilt remainder has
    // the hole. (The freed {4} chunk, born in place with no impulse, rests there on its neighbour,
    // so the ray still finds a body — the loose debris, never the standing wall.)
    const core::Vec3 hole_origin = place.translation + core::Vec3{0.0f, 0.75f, 5.0f};
    physics::RayHit hole;
    if (pw.raycast(physics::Ray{hole_origin, {0.0f, 0.0f, -1.0f}}, hole)) {
        CHECK(hole.body != dw.body_of(inst));
    }
}

TEST_CASE("M8.3 no-pop under rotation: the COM recipe survives a rotated placement") {
    // Same proof, placement rotated 90° about Y — a broken `transform_point(placement, centroid)`
    // (e.g. forgetting the rotation on the recentred COM) shows up ONLY under rotation.
    const assets::DestructibleAsset asset = make_grid(3, 3, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    core::Transform place;
    place.translation = {1.0f, 0.0f, 2.0f};
    place.rotation = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5707963f);
    const destruction::InstanceId inst = dw.spawn(pattern, place, pw);

    const core::Vec3 part6_before = dw.part_placement(inst, 6).translation;
    const core::Vec3 part7_before = dw.part_placement(inst, 7).translation;
    dw.apply_damage(inst,
                    dw.part_placement(inst, 4).translation, // aim in world space, wherever it is
                    0.2f,
                    10.0f,
                    core::Vec3{});
    dw.update(pw);

    REQUIRE(dw.debris_count() == 2); // the killed part 4 and the orphaned island {7}
    const std::size_t d7 = debris_with_part(dw, 7);
    REQUIRE(d7 != SIZE_MAX);
    CHECK(dw.debris_parts(d7)[0] == 7);
    // {7} born at the rotated part placement; the standing part 6 still reports the same spot.
    physics::BodyState debris_state{};
    REQUIRE(pw.get_body_state(dw.debris_body(d7), debris_state));
    CHECK(debris_state.position.x == doctest::Approx(part7_before.x).epsilon(1e-4));
    CHECK(debris_state.position.y == doctest::Approx(part7_before.y).epsilon(1e-4));
    CHECK(debris_state.position.z == doctest::Approx(part7_before.z).epsilon(1e-4));
    const core::Vec3 part6_after = dw.part_placement(inst, 6).translation;
    CHECK(part6_after.x == doctest::Approx(part6_before.x));
    CHECK(part6_after.y == doctest::Approx(part6_before.y));
    CHECK(part6_after.z == doctest::Approx(part6_before.z));
}

TEST_CASE("M8.3 contact-driven: a CCD marble breaks the panel it hits — and not the one beside") {
    // The full loop with no explicit ops at all: a 100 m/s marble arrests at column 1's middle
    // part (M7.10 CCD), its arrest impulse arrives as a contact event naming that child (M7.12),
    // the part erodes, and the column's top detaches. Columns 0 and 2 must be untouched — per-
    // region events are what make damage PART-addressed rather than body-addressed.
    const assets::DestructibleAsset asset = make_grid(3, 3, {0.25f, 0.25f, 0.15f}, 400.0f, 0.02f);
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    add_ground(pw);

    // 10 kg at 100 m/s ⇒ a ~1000 kg·m/s arrest, comfortably over the 400 threshold even if the
    // solver spreads the stop across two ticks; a settling debris impact (~300) stays under it.
    physics::BodyDesc marble;
    marble.shape.type = physics::ShapeType::Sphere;
    marble.shape.radius = 0.1f;
    marble.mass = 10.0f;
    marble.position = {0.0f, 0.75f, 2.0f}; // dead-on part 4's face
    marble.linear_velocity = {0.0f, 0.0f, -100.0f};
    marble.ccd = true;
    marble.linear_damping = 0.3f; // so the spent marble stops rolling and the scene can sleep
    marble.angular_damping = 0.5f;
    const physics::BodyId bullet = pw.create_body(marble);
    REQUIRE(bullet.is_valid());

    const core::Vec3 part7_before = dw.part_placement(inst, 7).translation;
    bool saw_break = false;
    for (int i = 0; i < 240; ++i) {
        pw.step(kDt);
        dw.update(pw);
        if (!saw_break && dw.debris_count() > 0) {
            saw_break = true;
            // No detach-tick pop: the newborn {7} island sits where its part stood (the update that
            // created it just ran; nothing has stepped it yet). Part 4 is killed the same tick and
            // flies off with the arrest impulse, so name the {7} piece explicitly.
            const std::size_t d7 = debris_with_part(dw, 7);
            REQUIRE(d7 != SIZE_MAX);
            physics::BodyState born{};
            REQUIRE(pw.get_body_state(dw.debris_body(d7), born));
            CHECK(born.position.x == doctest::Approx(part7_before.x).epsilon(1e-3));
            CHECK(born.position.y == doctest::Approx(part7_before.y).epsilon(1e-3));
            CHECK(born.position.z == doctest::Approx(part7_before.z).epsilon(1e-3));
        }
    }
    REQUIRE(saw_break);

    // The struck part 4 was killed — now its own flying debris (ADR-0029 §2) — and its column-mate
    // 7 detached above it; both neighbour columns stand whole. (Part 1 — the stump under the fall —
    // is deliberately not pinned: the slab landing on its own column may erode it further, which is
    // physics, not leakage; and further landings can spawn more debris, so debris_count is a lower
    // bound.)
    CHECK_FALSE(dw.part_alive(inst, 4));
    CHECK(dw.part_health(inst, 4) == doctest::Approx(0.0f));
    REQUIRE(dw.debris_count() >= 2);
    REQUIRE(debris_with_part(dw, 4) != SIZE_MAX); // the struck chunk flew off
    REQUIRE(debris_with_part(dw, 7) != SIZE_MAX); // its dependant detached
    for (const std::uint32_t p : {0u, 2u, 3u, 5u, 6u, 8u}) {
        CHECK(dw.part_alive(inst, p));
    }

    // Sanity: nothing exploded — every body is still "in the room", on or above the ground. The
    // bounds are deliberately roomy: the killed chunk flies off with the full arrest impulse, and
    // the slab lands on the spent marble and can squirt it metres across the floor (physical
    // slapstick, not a solver blow-up — the settle proof below is the real "nothing is diverging"
    // statement).
    const auto sane = [&](physics::BodyId id) {
        physics::BodyState s{};
        REQUIRE(pw.get_body_state(id, s));
        CHECK(s.position.y > -1.0f);
        CHECK(s.position.y < 10.0f);
        CHECK(std::abs(s.position.x) < 50.0f);
        CHECK(std::abs(s.position.z) < 50.0f);
    };
    for (std::size_t i = 0; i < dw.debris_count(); ++i) {
        sane(dw.debris_body(i));
    }
    sane(bullet);

    // And the world settles: run until everything sleeps, then the hash is stable tick over tick
    // (a sleeping world integrates nothing — any drift here would mean phantom state changes).
    int settle = 0;
    for (; settle < 1200 && pw.stats().awake_bodies > 0; ++settle) {
        pw.step(kDt);
        dw.update(pw);
    }
    REQUIRE(pw.stats().awake_bodies == 0);
    const std::uint64_t settled_hash = pw.world_hash();
    const std::uint64_t settled_state = dw.state_hash();
    for (int i = 0; i < 5; ++i) {
        pw.step(kDt);
        dw.update(pw);
        CHECK(pw.world_hash() == settled_hash);
        CHECK(dw.state_hash() == settled_state);
    }
}

namespace {

// One full scripted run of the M11 determinism scenario: the cooked 100-part wall on a ground
// plane, eight blasts over 180 ticks carving a horizontal band (everything above it islands and
// falls), with physics stepped on `workers` job-system threads (−1 = sequential). `perm` permutes
// the ARRIVAL order of the three same-tick blasts at tick 10 — the canonical op sort must erase
// it. Everything the run observed is returned for exact comparison.
struct DeterminismRun {
    std::uint64_t state_hash = 0;
    std::uint64_t world_hash = 0;
    std::size_t detached_parts = 0;
    std::vector<std::vector<std::uint32_t>> compositions;

    bool operator==(const DeterminismRun&) const = default;
};

DeterminismRun run_scripted_wall(const assets::DestructibleAsset& asset, int workers, int perm) {
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    REQUIRE(pattern.is_valid());
    // The wall is 4 × 3 × 0.3 m centred on the origin (y ∈ [−1.5, 1.5]), anchored on its −Y base;
    // stand it on a ground plane at that base so debris lands and does real solver work.
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    REQUIRE(inst.is_valid());
    add_ground(pw, -1.5f);

    std::unique_ptr<core::JobSystem> js;
    if (workers >= 0) {
        js = std::make_unique<core::JobSystem>(static_cast<unsigned>(workers));
        pw.set_job_system(js.get());
    }

    // The band blasts: y = −0.25, spanning the width; radius 0.6 and amount 6 guarantee every
    // part straddling the band line dies (max centre distance ≈ 0.35 ⇒ damage ≥ 2.5 ≥ health 1),
    // so the whole upper slab is severed. The +Z impulse tips the slab off the stump.
    const auto blast = [&](core::Vec3 p) {
        dw.apply_damage(inst, p, 0.6f, 6.0f, core::Vec3{0.0f, 0.0f, 30.0f});
    };
    for (int tick = 0; tick < 180; ++tick) {
        if (tick == 10) {
            // Three blasts whose damage fields OVERLAP, issued in a permuted order: if any trace
            // of arrival order survived into the accumulation, the hashes would split.
            const core::Vec3 a{-1.4f, -0.25f, 0.0f};
            const core::Vec3 b{-0.7f, -0.25f, 0.0f};
            const core::Vec3 c{0.0f, -0.25f, 0.0f};
            if (perm == 0) {
                blast(a);
                blast(b);
                blast(c);
            } else if (perm == 1) {
                blast(c);
                blast(a);
                blast(b);
            } else {
                blast(b);
                blast(c);
                blast(a);
            }
        }
        if (tick == 40) {
            blast({0.7f, -0.25f, 0.0f});
            blast({1.4f, -0.25f, 0.0f});
        }
        if (tick == 70) {
            blast({0.0f, 0.6f, 0.0f}); // chews at the (likely already flying) upper slab region
        }
        if (tick == 100) {
            blast({-1.7f, -1.2f, 0.0f}); // chip a lower corner near the anchors
        }
        if (tick == 130) {
            blast({1.7f, -1.2f, 0.0f});
        }
        pw.step(kDt);
        dw.update(pw);
    }

    DeterminismRun out;
    out.state_hash = dw.state_hash();
    out.world_hash = pw.world_hash();
    out.compositions = debris_compositions(dw);
    for (const auto& c : out.compositions) {
        out.detached_parts += c.size();
    }
    return out;
}

} // namespace

TEST_CASE("M8.3 determinism (the M11 contract): scripted damage on the 100-part wall is "
          "bit-identical across runs, worker counts, and input arrival order") {
    // wall_100.rdest is the committed 100-part fixture (like wall.rdest, regenerate deliberately:
    // `rime fracture --size 4 3 0.3 --parts 100 --seed 7 --out tests/assets/fixtures
    //  --name wall_100`).
    const assets::DestructibleAsset asset = load_asset("wall_100.rdest");
    REQUIRE(asset.parts.size() == 100);

    const DeterminismRun baseline = run_scripted_wall(asset, -1, 0);
    // Real islanding happened — a serious chunk of the wall detached, in at least one island.
    CHECK(baseline.compositions.size() >= 1);
    CHECK(baseline.detached_parts >= 10);

    // (a) The same run twice: identical to the bit.
    CHECK(run_scripted_wall(asset, -1, 0) == baseline);
    // (b) Across physics worker counts — the destruction tail is sequential and the physics step
    // guarantees bit-identity for any count, so the PAIR of hashes must be invariant.
    CHECK(run_scripted_wall(asset, 1, 0) == baseline);
    CHECK(run_scripted_wall(asset, 2, 0) == baseline);
    CHECK(run_scripted_wall(asset, 4, 0) == baseline);
    // (c) Same-tick ops arriving in a different order: the canonical op sort erases arrival.
    CHECK(run_scripted_wall(asset, 2, 1) == baseline);
    CHECK(run_scripted_wall(asset, 4, 2) == baseline);
}

TEST_CASE("M8.3: unknown ids, dead parts, and empty updates are all safe no-ops") {
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;

    // Nothing registered: every entry point shrugs.
    dw.apply_damage(destruction::InstanceId{}, {0.0f, 0.0f, 0.0f}, 1.0f, 5.0f, {});
    dw.apply_damage(destruction::InstanceId{42, 0}, {0.0f, 0.0f, 0.0f}, 1.0f, 5.0f, {});
    dw.apply_damage(destruction::InstanceId{0, 7}, {0.0f, 0.0f, 0.0f}, 1.0f, 5.0f, {});
    dw.update(pw);
    CHECK(dw.debris_count() == 0);
    CHECK_FALSE(dw.debris_body(0).is_valid());
    CHECK_FALSE(dw.debris_source(0).is_valid());
    CHECK(dw.debris_parts(0).empty());
    CHECK(dw.part_from_child(destruction::InstanceId{}, 0) == destruction::kInvalidPartIndex);
    const std::uint64_t empty_hash = dw.state_hash();
    CHECK(dw.state_hash() == empty_hash);

    // A real wall: overkill and re-kill are absorbed; a second update with no new input is a
    // no-op (no phantom re-fracture, no hash drift).
    const assets::DestructibleAsset asset = make_grid(2, 2, {0.25f, 0.25f, 0.15f}, 1000.0f, 1.0f);
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 1000.0f, {}); // massive overkill
    dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 1000.0f, {}); // …twice
    dw.update(pw);
    CHECK(dw.part_health(inst, 0) == doctest::Approx(0.0f)); // clamped, not −1998
    CHECK(dw.debris_count() == 2); // the killed part 0 + its orphaned dependant {2}
    const std::uint64_t after_break = dw.state_hash();
    dw.update(pw); // nothing pending, no events
    CHECK(dw.state_hash() == after_break);
    CHECK(dw.debris_count() == 2);
    // Damage aimed at the already-dead part: absorbed by rubble, nothing changes.
    dw.apply_damage(inst, {-0.25f, 0.25f, 0.0f}, 0.2f, 10.0f, {});
    dw.update(pw);
    CHECK(dw.state_hash() == after_break);
}

TEST_CASE("M8.3 cost: worst-case re-solve + swap on the 100-part wall (the ADR-0029 §2 numbers)") {
    // Not an assertion — a measurement (machine-dependent, so never CHECKed): the cost of one
    // update() that erodes a part and rebuilds a ~99-child remainder, and the bare
    // register_compound cost at that child count. These are the numbers ADR-0029 §2 owes the
    // per-part-statics fallback discussion. Run under Release for the recorded figure; printed
    // with a stable tag so it is grep-able from any test log.
    const assets::DestructibleAsset asset = load_asset("wall_100.rdest");
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);

    // Kill one bottom-centre part (a small point blast): the remainder stays near-full size, so
    // the union-find walks all 100 parts and the swap re-registers ~99 children — the worst case
    // for a single-part loss.
    std::uint32_t victim = 0;
    float best = 1e9f;
    for (std::uint32_t p = 0; p < asset.parts.size(); ++p) {
        const core::Vec3 c = asset.parts[p].com;
        const float d2 = c.x * c.x + (c.y + 1.2f) * (c.y + 1.2f) + c.z * c.z;
        if (d2 < best) {
            best = d2;
            victim = p;
        }
    }
    dw.apply_damage(inst, asset.parts[victim].com, 0.05f, 100.0f, {});

    const auto t0 = std::chrono::steady_clock::now();
    dw.update(pw);
    const auto t1 = std::chrono::steady_clock::now();
    const double update_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    CHECK_FALSE(dw.part_alive(inst, victim)); // the update really did the work

    std::uint32_t standing = 0;
    for (std::uint32_t p = 0; p < 100; ++p) {
        standing += dw.part_alive(inst, p) ? 1u : 0u;
    }

    // The bare runtime register_compound at remainder scale, measured on its own: fresh hulls
    // (the store is append-only; duplicates are harmless), children at their cooked COMs.
    std::vector<physics::HullId> hulls;
    std::vector<physics::CompoundChildDesc> children;
    for (const assets::DestructiblePart& p : asset.parts) {
        const physics::HullId h =
            pw.register_hull(physics::HullDesc{p.vertices, p.face_counts, p.face_indices});
        REQUIRE(h.is_valid());
        physics::ShapeDesc s;
        s.type = physics::ShapeType::ConvexHull;
        s.hull = h;
        children.push_back(physics::CompoundChildDesc{s, p.com, core::quat_identity()});
    }
    const auto t2 = std::chrono::steady_clock::now();
    const physics::CompoundId comp = pw.register_compound(physics::CompoundDesc{children});
    const auto t3 = std::chrono::steady_clock::now();
    REQUIRE(comp.is_valid());
    const double register_us = std::chrono::duration<double, std::micro>(t3 - t2).count();

    std::printf("M8.3-COST update(kill 1 of 100, %u standing): %.1f us | "
                "register_compound(100 children): %.1f us\n",
                standing,
                update_us,
                register_us);
}
