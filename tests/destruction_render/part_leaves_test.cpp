// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.2d — per-part render leaves, proven headless.
//
// The system under test is the one that has been missing since M8: nothing in the engine turned a
// destructible's PARTS into something a renderer could draw, so `10-destructible-wall` hand-rolled
// it, and `destruction/world.hpp` admitted as much ("per-part render leaves land with the
// [sample]"). At one wall of 60 parts that is fine; at m13.2c's 140 instances and 2,016 parts it is
// not, and m13.3 and m13.5 would each have copied the loop.
//
// WHAT IS PROVEN HERE, and what deliberately is not. Everything about the leaf LIFE CYCLE — created
// once per part, following the sim through standing → detached → frozen, and despawned when
// m13.2b's visual budget evicts its rubble — is pure bookkeeping over transforms, so it runs on
// every CI OS and under both sanitizers with no device. Only "the pixels are there" needs a GPU,
// and that is the lavapipe half in tests/render.
//
// The seam that makes this possible is `register_pattern(..., meshes = nullptr)`: MeshRegistry owns
// an rhi::Device and uploads on add, so minting a MeshId needs a GPU, while the part COMs, the leaf
// entities and their poses do not.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_render/part_leaves.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/components.hpp"

#ifndef RIME_DESTRUCTION_FIXTURE_DIR
#define RIME_DESTRUCTION_FIXTURE_DIR "."
#endif

using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr std::uint64_t kWallAsset = 0x5741'4C4Cull; // 'WALL'

assets::DestructibleAsset load_wall() {
    const std::string path = std::string(RIME_DESTRUCTION_FIXTURE_DIR) + "/wall.rdest";
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open fixture: " << path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    assets::AssetError err{};
    auto asset = assets::read_destructible(bytes, err);
    REQUIRE(asset.has_value());
    return *asset;
}

// A world with `walls` destructibles standing in a row, bound and ready to be leafed.
struct Fixture {
    ecs::World world;
    physics::PhysicsWorld physics;
    destruction::DestructionWorld destruction;
    destruction_render::PartLeafRenderer leaves;
    assets::DestructibleAsset asset = load_wall();
    destruction::PatternId pattern{};

    explicit Fixture(int walls, bool register_meshes = true) {
        ecs::register_transform_components(world);
        destruction::register_destruction_components(world);
        render::register_render_components(world);
        (void)world.register_component<ecs::WorldTransform>();

        pattern = destruction.register_pattern(asset, physics);
        REQUIRE(pattern.is_valid());
        if (register_meshes) {
            // No device in a headless proof, so no MeshIds — the COMs and the leaf entities are
            // what this suite is about.
            (void)leaves.register_pattern(pattern, asset, /*meshes=*/nullptr);
        }

        for (int i = 0; i < walls; ++i) {
            core::Transform tf;
            tf.translation = {static_cast<float>(i) * 6.0f, 2.0f, 0.0f};
            (void)world.spawn_with(ecs::LocalTransform{tf},
                                   destruction::Destructible{kWallAsset},
                                   render::MaterialRef{static_cast<render::MaterialId>(i)});
        }

        const destruction::BindStats bound =
            destruction::bind_destructibles(world, destruction, physics, [this](std::uint64_t a) {
                return a == kWallAsset ? pattern : destruction::PatternId{};
            });
        REQUIRE(bound.bound == static_cast<std::size_t>(walls));
        REQUIRE(bound.unresolved == 0);
    }

    [[nodiscard]] std::uint32_t parts() const {
        return static_cast<std::uint32_t>(asset.parts.size());
    }
};

[[nodiscard]] std::size_t live_leaf_entities(ecs::World& world) {
    // A leaf is an entity with a WorldTransform and a MaterialRef but no Destructible — the slabs
    // themselves are never drawn, only their parts are.
    std::size_t n = 0;
    world.query<ecs::WorldTransform, render::MaterialRef>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform&, render::MaterialRef&) {
            if (world.get<destruction::Destructible>(e) == nullptr) {
                ++n;
            }
        });
    return n;
}

} // namespace

TEST_CASE("part leaves: one leaf per part, and ONE mesh table per pattern") {
    Fixture fx(3);
    const destruction_render::LeafStats stats =
        fx.leaves.update(fx.world, fx.destruction, fx.physics);

    CHECK(stats.leaves_created == 3u * fx.parts());
    CHECK(stats.leaves_live == 3u * fx.parts());
    CHECK(stats.instances_without_meshes == 0);
    CHECK(live_leaf_entities(fx.world) == 3u * fx.parts());

    // THE EFFICIENCY CLAIM, at the level a headless test can prove it: three instances share ONE
    // pattern entry. Uploading per (instance, part) rather than per (pattern, part) is what makes
    // the difference between ~148 vertex buffers for m13.2c's block and 2,016 copies of the same
    // geometry — the naive loop the wall sample can afford and the block cannot. The GPU half (two
    // instances' leaves for the same part carry the identical MeshId) is asserted in tests/render.
    CHECK(fx.leaves.pattern_count() == 1);

    SUBCASE("a second update creates nothing — leaves are built once, then posed") {
        const destruction_render::LeafStats again =
            fx.leaves.update(fx.world, fx.destruction, fx.physics);
        CHECK(again.leaves_created == 0);
        CHECK(again.leaves_live == 3u * fx.parts());
        CHECK(live_leaf_entities(fx.world) == 3u * fx.parts());
    }

    SUBCASE("each leaf wears its own slab's material") {
        // m13.2c puts a MaterialRef on an entity that is never itself drawn, precisely so the
        // palette decides a structure's look once and its 12-28 chunks inherit it.
        for (std::uint32_t i = 0; i < 3; ++i) {
            const auto inst = destruction::InstanceId{i};
            for (const ecs::Entity leaf : fx.leaves.leaves_of(inst)) {
                REQUIRE(leaf != ecs::kNullEntity);
                const auto* mat = fx.world.get<render::MaterialRef>(leaf);
                REQUIRE(mat != nullptr);
                CHECK(mat->material == i);
            }
        }
    }
}

TEST_CASE("part leaves: a standing part is drawn at its instance placement") {
    Fixture fx(2);
    (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);

    for (std::uint32_t i = 0; i < 2; ++i) {
        const auto inst = destruction::InstanceId{i};
        const std::vector<ecs::Entity> leaf = fx.leaves.leaves_of(inst);
        REQUIRE(leaf.size() == fx.parts());
        for (std::uint32_t p = 0; p < fx.parts(); ++p) {
            const auto* tf = fx.world.get<ecs::WorldTransform>(leaf[p]);
            REQUIRE(tf != nullptr);
            const core::Transform want = fx.destruction.part_placement(inst, p);
            CHECK(tf->value.translation.x == doctest::Approx(want.translation.x));
            CHECK(tf->value.translation.y == doctest::Approx(want.translation.y));
            CHECK(tf->value.translation.z == doctest::Approx(want.translation.z));
        }
    }

    // The two walls stand 6 m apart, so their leaves must too. This is the check that catches a
    // bridge which poses every instance from the same placement — every count above would still be
    // right, and every wall would be drawn on top of the first.
    const core::Vec3 a =
        fx.world.get<ecs::WorldTransform>(fx.leaves.leaves_of({0})[0])->value.translation;
    const core::Vec3 b =
        fx.world.get<ecs::WorldTransform>(fx.leaves.leaves_of({1})[0])->value.translation;
    CHECK(b.x - a.x == doctest::Approx(6.0f));
}

TEST_CASE("part leaves: a detached part rides its debris body, and a frozen one stays put") {
    Fixture fx(1);
    (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);

    const auto inst = destruction::InstanceId{0};
    const std::vector<ecs::Entity> leaf = fx.leaves.leaves_of(inst);

    // Where every part started.
    std::vector<core::Vec3> before;
    for (const ecs::Entity e : leaf) {
        before.push_back(fx.world.get<ecs::WorldTransform>(e)->value.translation);
    }

    // Blow the wall apart and let the debris fall (no ground body — they simply fall).
    fx.destruction.apply_damage(inst, {0.0f, 2.0f, 0.0f}, 8.0f, 20.0f, {0.0f, 0.0f, 0.0f});
    std::size_t moved = 0;
    for (int tick = 0; tick < 30; ++tick) {
        fx.physics.step(kDt);
        fx.destruction.update(fx.physics);
        (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);
    }
    REQUIRE(fx.destruction.debris_count() > 0);

    for (std::size_t p = 0; p < leaf.size(); ++p) {
        const core::Vec3 now = fx.world.get<ecs::WorldTransform>(leaf[p])->value.translation;
        if (std::fabs(now.y - before[p].y) > 0.05f) {
            ++moved;
        }
    }
    // The load-bearing assertion of this whole module: the leaves MOVED. Every count in the first
    // test case is satisfied perfectly by a bridge that creates the right leaves and then never
    // poses them again — a wall that shatters in the physics world and stands frozen on screen.
    CHECK(moved > 0);

    SUBCASE("a leaf keeps its last pose once the debris freezes") {
        // m8.5 destroys a settled debris body but keeps its roster row, because ADR-0029 §8 wants
        // "a render leaf can outlive the physics body at its last pose" — that is what keeps rubble
        // on screen for free. So a frozen debris must leave its leaf EXACTLY where it was.
        //
        // The first version of this case snapshotted the poses, ran 400 ticks, and then asserted
        // only that nothing had teleported to the origin — which a leaf that quietly DRIFTS passes
        // perfectly. That is the assert-the-handoff failure this repo has now shipped three times:
        // the state is easy to check and the *holding* of it is what actually breaks. So: enable
        // the lifecycle so freezing really happens, wait until a row is frozen (roster row with a
        // dead body), snapshot at that instant, and demand the pose is bit-identical later.
        destruction::LifecycleConfig life;
        life.enabled = true;
        life.freeze_delay_ticks = 5;
        fx.destruction.configure_lifecycle(life);

        physics::BodyDesc ground;
        ground.motion = physics::MotionType::Static;
        ground.shape.type = physics::ShapeType::Box;
        ground.shape.half_extents = {50.0f, 0.5f, 50.0f};
        ground.position = {0.0f, -0.5f, 0.0f};
        (void)fx.physics.create_body(ground);

        // Run until at least one debris row has frozen, then snapshot every leaf of a frozen row.
        std::vector<std::pair<ecs::Entity, core::Vec3>> frozen_poses;
        for (int tick = 0; tick < 600 && frozen_poses.empty(); ++tick) {
            fx.physics.step(kDt);
            fx.destruction.update(fx.physics);
            (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);
            for (std::size_t d = 0; d < fx.destruction.debris_count(); ++d) {
                if (fx.destruction.debris_retired(d) ||
                    fx.physics.is_alive(fx.destruction.debris_body(d))) {
                    continue; // still simulating, or already gone
                }
                for (const std::uint32_t part : fx.destruction.debris_parts(d)) {
                    if (part < leaf.size() && leaf[part] != ecs::kNullEntity) {
                        frozen_poses.emplace_back(
                            leaf[part],
                            fx.world.get<ecs::WorldTransform>(leaf[part])->value.translation);
                    }
                }
            }
        }
        REQUIRE_FALSE(frozen_poses.empty()); // non-vacuity: something really did freeze

        for (int tick = 0; tick < 200; ++tick) {
            fx.physics.step(kDt);
            fx.destruction.update(fx.physics);
            (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);
        }

        for (const auto& [entity, was] : frozen_poses) {
            const core::Vec3 now = fx.world.get<ecs::WorldTransform>(entity)->value.translation;
            CHECK(now.x == doctest::Approx(was.x));
            CHECK(now.y == doctest::Approx(was.y));
            CHECK(now.z == doctest::Approx(was.z));
        }
    }
}

TEST_CASE("part leaves: m13.2b's visual budget despawns leaves rather than leaking them") {
    Fixture fx(1);

    // A budget far below what one wall produces, so eviction is forced. `max_visual_debris` must
    // stay ABOVE `max_live_debris` (destruction/world.hpp makes that an invariant — inverting it
    // would let a still-simulating debris be visually retired), so both come down together.
    destruction::LifecycleConfig life;
    life.enabled = true;          // the master switch — off, nothing is ever reclaimed at all
    life.freeze_delay_ticks = 10; // freeze quickly, so there is something to retire
    life.max_live_debris = 1000;  // the LIVE cap is not what is under test here
    life.max_visual_debris = 4;   // …this is
    fx.destruction.configure_lifecycle(life);

    physics::BodyDesc ground;
    ground.motion = physics::MotionType::Static;
    ground.shape.type = physics::ShapeType::Box;
    ground.shape.half_extents = {50.0f, 0.5f, 50.0f};
    ground.position = {0.0f, -0.5f, 0.0f};
    (void)fx.physics.create_body(ground);

    (void)fx.leaves.update(fx.world, fx.destruction, fx.physics);
    const std::size_t before = live_leaf_entities(fx.world);
    CHECK(before == fx.parts());

    fx.destruction.apply_damage({0}, {0.0f, 2.0f, 0.0f}, 8.0f, 20.0f, {0.0f, 0.0f, 0.0f});

    // Run until the visual population is back under the cap, rather than for a fixed number of
    // ticks. m13.2b's cap is deliberately SOFT under a burst — only FROZEN debris may be retired,
    // because deleting rubble a player is watching come to rest would be worse than exceeding a
    // budget for a moment — so the contract is "it catches up", not "it is never exceeded".
    //
    // The first version of this case ran 400 ticks and asserted the cap outright. That passed only
    // because the old wall fixture happened to settle within 400; regenerating the fixture (the
    // m13.fracture-clip rewrite) changed the debris pattern and it took ~2400. The test was pinned
    // to an accident of the fixture, not to the behaviour.
    std::size_t retired_total = 0;
    int ticks = 0;
    constexpr int kBound = 6000;
    for (; ticks < kBound; ++ticks) {
        fx.physics.step(kDt);
        fx.destruction.update(fx.physics);
        retired_total += fx.leaves.update(fx.world, fx.destruction, fx.physics).leaves_retired;
        if (ticks > 200 && fx.destruction.visual_debris_count() <= life.max_visual_debris) {
            break;
        }
    }
    MESSAGE("converged after " << ticks << " ticks: " << fx.destruction.visual_debris_count()
                               << " visible of a " << fx.destruction.debris_count() << " roster");
    CHECK(ticks < kBound); // it converged rather than running out the bound

    // The C6 contract, from the render side: rubble the budget evicted stops costing anything.
    CHECK(retired_total > 0);
    CHECK(fx.destruction.visual_debris_count() <= life.max_visual_debris);

    const std::size_t after = live_leaf_entities(fx.world);
    CHECK(after < before);
    CHECK(after + retired_total == before);

    // Despawned, not merely hidden. A leaf left in the world with an invalid mesh still costs an
    // extraction visit and a frustum test every frame, which is the leak C6 exists to stop.
    std::size_t null_slots = 0;
    for (const ecs::Entity e : fx.leaves.leaves_of({0})) {
        if (e == ecs::kNullEntity) {
            ++null_slots;
        }
    }
    CHECK(null_slots == retired_total);
}

TEST_CASE("part leaves: a slab with no material is a COUNT, not an unshaded building") {
    // Leaf materials are captured ONCE, at build. So an ordering mistake — calling apply_palette
    // after the first update() — leaves every part of that structure permanently unshaded, and
    // scene_renderer skips a draw with an invalid material without a word. At block scale that is
    // an entire building that is not there, and it is exactly as invisible as a missing pattern.
    Fixture fx(2);

    // Strip one slab's material, as a wrong-order palette pass would.
    ecs::Entity stripped = ecs::kNullEntity;
    fx.world.query<destruction::Destructible, render::MaterialRef>().for_each(
        [&](ecs::Entity e, destruction::Destructible&, render::MaterialRef& mat) {
            if (stripped == ecs::kNullEntity) {
                stripped = e;
                mat.material = render::kInvalidMaterialId;
            }
        });
    REQUIRE(stripped != ecs::kNullEntity);

    const destruction_render::LeafStats stats =
        fx.leaves.update(fx.world, fx.destruction, fx.physics);

    CHECK(stats.instances_without_material == 1);
    // The leaves still exist and are still posed — only their shading is missing, which is why
    // nothing else in the run would have said so.
    CHECK(stats.leaves_created == 2u * fx.parts());
    CHECK(stats.instances_without_meshes == 0);
}

TEST_CASE("part leaves: an instance whose pattern was never registered is a COUNT, not a silence") {
    // Bind a destructible the bridge has no meshes for. It stands in the physics world and appears
    // nowhere on screen — at block scale that is an entire building that is quietly not there, and
    // nothing else in the frame would say so.
    Fixture fx(2, /*register_meshes=*/false);
    const destruction_render::LeafStats stats =
        fx.leaves.update(fx.world, fx.destruction, fx.physics);

    CHECK(stats.instances_without_meshes == 2);
    CHECK(stats.leaves_created == 0);
    CHECK(stats.leaves_live == 0);
    CHECK(live_leaf_entities(fx.world) == 0);
    CHECK(fx.leaves.pattern_count() == 0);
}
