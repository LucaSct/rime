// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/world.hpp"
#include "rime/physics/physics.hpp"

// M8.2 proofs: DestructionWorld loads a cooked fracture pattern (the m8.1 `wall.rdest` fixture),
// registers it into a real PhysicsWorld (hulls + one compound), and stands instances of it. All
// structural and GPU-free (destruction is CPU geometry → physics bodies): the pattern's counts and
// connectivity survive the round trip; the intact wall stands exactly where a plain static box
// would (a raycast hits its face); a bound-but-untouched wall costs the simulation nothing
// (WorldStats stays at zero awake bodies — the "≈ static baseline" property); the whole thing is
// deterministic; and unknown/ malformed inputs are handled without UB. This is "load, stand, bind";
// damage and the fracture body-swap are m8.3.
using namespace rime;

namespace {

// The cooked wall: a 2 × 1.5 × 0.3 m box fractured into 16 parts (the committed m8.1 fixture,
// shared with the assets oracle test). Half-extents 1.0 × 0.75 × 0.15; the two the raycasts use:
constexpr float kHalfX = 1.0f;
constexpr float kHalfY = 0.75f;

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

assets::DestructibleAsset load_wall() {
    const std::vector<std::byte> file = read_fixture("wall.rdest");
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    REQUIRE_MESSAGE(asset.has_value(), "wall.rdest failed to decode: " << assets::to_string(err));
    return std::move(*asset);
}

} // namespace

TEST_CASE("M8.2: a cooked wall registers as a pattern and stands as one static compound") {
    const assets::DestructibleAsset asset = load_wall();
    REQUIRE(asset.parts.size() >= 8);

    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;

    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    REQUIRE(pattern.is_valid());
    CHECK(dw.pattern_count() == 1);
    // The pattern's shape survives the round trip: part count, bonds, and anchors all match the
    // cook.
    CHECK(dw.part_count(pattern) == static_cast<std::uint32_t>(asset.parts.size()));
    CHECK(dw.bonds(pattern).size() == asset.bonds.size());
    CHECK(dw.anchors(pattern).size() == asset.anchors.size());

    const destruction::InstanceId inst =
        dw.spawn(pattern, core::Transform{}, pw); // placed at the origin
    REQUIRE(inst.is_valid());
    CHECK(dw.instance_count() == 1);

    // The instance stands as exactly one live physics body (the intact compound), and it is the
    // only body in the world.
    const physics::BodyId body = dw.body_of(inst);
    CHECK(pw.is_alive(body));
    CHECK(pw.body_count() == 1);

    // Every part starts alive with full health — the damage substrate m8.3 fills in.
    CHECK(dw.instance_part_count(inst) == static_cast<std::uint32_t>(asset.parts.size()));
    for (std::uint32_t i = 0; i < dw.instance_part_count(inst); ++i) {
        CHECK(dw.part_alive(inst, i));
        CHECK(dw.part_health(inst, i) == doctest::Approx(1.0f));
    }
}

TEST_CASE("M8.2: the intact wall stands where a plain static box would — a raycast hits its face") {
    const assets::DestructibleAsset asset = load_wall();
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);
    REQUIRE(pattern.is_valid());
    const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
    const physics::BodyId body = dw.body_of(inst);

    // Fire a ray from outside the +X face straight at the wall. A 2 m wall centred on the origin
    // has its +X face at x = +1.0 (kHalfX); the compound of Voronoi parts must present that same
    // surface a plain static box would — so the ray hits our instance's body at x ≈ 1.0.
    physics::RayHit hit;
    const bool got = pw.raycast(physics::Ray{{5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, hit);
    REQUIRE(got);
    CHECK(hit.body.index == body.index);
    CHECK(hit.point.x == doctest::Approx(kHalfX).epsilon(0.02));
    CHECK(hit.point.x <= kHalfX + 0.02f); // never behind the face
    // A ray aimed past the top of the wall misses entirely.
    physics::RayHit miss;
    CHECK_FALSE(pw.raycast(physics::Ray{{5.0f, kHalfY + 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, miss));
}

TEST_CASE(
    "M8.2: a standing wall costs the simulation nothing (≈ static baseline) and is deterministic") {
    auto run = []() -> std::uint64_t {
        const assets::DestructibleAsset asset = load_wall();
        destruction::DestructionWorld dw;
        physics::PhysicsWorld pw;
        const destruction::PatternId pattern = dw.register_pattern(asset, pw);
        const destruction::InstanceId inst = dw.spawn(pattern, core::Transform{}, pw);
        (void)inst;
        // Step a while. The wall is static, so it never integrates or solves.
        for (int i = 0; i < 60; ++i) {
            pw.step(1.0f / 60.0f);
        }
        const physics::WorldStats s = pw.stats();
        // The "≈ static baseline" property: a bound, untouched destructible adds nothing to the
        // tick's solve load — no dynamic bodies, nothing awake, no active islands.
        CHECK(s.body_count == 1);
        CHECK(s.dynamic_bodies == 0);
        CHECK(s.static_bodies == 1);
        CHECK(s.awake_bodies == 0);
        CHECK(s.active_islands == 0);
        return pw.world_hash();
    };
    // Two identical runs hash identically — the intact wall is deterministic (the M11 contract).
    CHECK(run() == run());
}

TEST_CASE("M8.2: part_placement carries a part's COM through the instance transform") {
    const assets::DestructibleAsset asset = load_wall();
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw);

    // Place the wall 10 m up and rotate it 90° about Y.
    core::Transform placement;
    placement.translation = core::Vec3{0.0f, 10.0f, 0.0f};
    placement.rotation = core::quat_from_axis_angle(core::Vec3{0.0f, 1.0f, 0.0f}, 1.5707963f);
    const destruction::InstanceId inst = dw.spawn(pattern, placement, pw);

    // Part 0's world placement = its cooked COM carried through the instance transform, with the
    // instance's rotation. Compare against the core transform helper directly.
    const core::Transform p0 = dw.part_placement(inst, 0);
    const core::Vec3 expect = core::transform_point(placement, asset.parts[0].com);
    CHECK(p0.translation.x == doctest::Approx(expect.x));
    CHECK(p0.translation.y == doctest::Approx(expect.y));
    CHECK(p0.translation.z == doctest::Approx(expect.z));
}

TEST_CASE("M8.2: unknown ids are safe and a malformed pattern is rejected") {
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;

    // Nothing registered/spawned yet: every accessor is a safe no-op on a null/unknown id.
    CHECK(dw.pattern_count() == 0);
    CHECK(dw.instance_count() == 0);
    CHECK_FALSE(dw.body_of(destruction::InstanceId{}).is_valid());
    CHECK(dw.part_count(destruction::PatternId{}) == 0);
    CHECK(dw.instance_part_count(destruction::InstanceId{}) == 0);
    CHECK_FALSE(dw.part_alive(destruction::InstanceId{}, 0));
    CHECK(dw.part_health(destruction::InstanceId{}, 0) == 0.0f);
    CHECK(dw.bonds(destruction::PatternId{}).empty());
    CHECK(dw.anchors(destruction::PatternId{}).empty());
    CHECK_FALSE(dw.spawn(destruction::PatternId{}, core::Transform{}, pw).is_valid());

    // A malformed pattern: one part whose "hull" is four coincident points — degenerate, so
    // register_hull rejects it, so register_pattern rejects the whole pattern (no partial state).
    assets::DestructibleAsset bad;
    bad.half_extents = core::Vec3{1.0f, 1.0f, 1.0f};
    assets::DestructiblePart part;
    part.com = core::Vec3{0.0f, 0.0f, 0.0f};
    part.volume = 1.0f;
    part.vertices = {core::Vec3{0.0f, 0.0f, 0.0f},
                     core::Vec3{0.0f, 0.0f, 0.0f},
                     core::Vec3{0.0f, 0.0f, 0.0f},
                     core::Vec3{0.0f, 0.0f, 0.0f}};
    part.face_counts = {3, 3, 3, 3};
    part.face_indices = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    bad.parts.push_back(part);
    CHECK_FALSE(dw.register_pattern(bad, pw).is_valid());
    CHECK(dw.pattern_count() == 0); // nothing was committed
}

TEST_CASE("M8.2: many instances of one pattern (spawn churn — the shape economy)") {
    const assets::DestructibleAsset asset = load_wall();
    destruction::DestructionWorld dw;
    physics::PhysicsWorld pw;
    const destruction::PatternId pattern = dw.register_pattern(asset, pw); // registered ONCE
    REQUIRE(pattern.is_valid());

    // Stamp out a grid of instances — all share the one registered pattern (one hull set, one
    // compound). Exercises the spawn/allocation path (ASan/UBSan cover it in CI).
    constexpr int kN = 25;
    for (int i = 0; i < kN; ++i) {
        core::Transform t;
        t.translation = core::Vec3{static_cast<float>(i) * 4.0f, 0.0f, 0.0f};
        const destruction::InstanceId inst = dw.spawn(pattern, t, pw);
        CHECK(inst.is_valid());
        CHECK(pw.is_alive(dw.body_of(inst)));
    }
    CHECK(dw.pattern_count() == 1); // still one pattern
    CHECK(dw.instance_count() == static_cast<std::size_t>(kN));
    CHECK(pw.body_count() == static_cast<std::size_t>(kN));
}
