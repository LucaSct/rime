// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

#include "player_spawn.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/scene/scene_format.hpp"

// The authored-spawn resolver is pure, so every case here drives it with a hand-built world and
// checks the value it returns. What these tests do NOT prove is that the running server places the
// avatar there or that the client starts looking that way: that wiring lives in main.cpp and is
// checked by running the game (the "authored player spawn" line), not here.

using namespace rime;
using block_play::PlayerSpawn;
using block_play::resolve_player_spawn;
using block_play::SpawnStatus;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// World is neither copyable nor movable, so a fixture registers into the caller's world.
void init(ecs::World& w) {
    block_play::register_block_components(w);
}

// yaw about +Y, then pitch about the yaw-rotated X: FirstPersonView's order, so a test pose and a
// view with the same angles must face the same way.
core::Quat yaw_pitch(float yaw, float pitch) {
    return core::quat_from_axis_angle({0, 1, 0}, yaw) *
           core::quat_from_axis_angle({1, 0, 0}, pitch);
}

core::Transform pose(core::Vec3 t, core::Quat q = core::quat_identity()) {
    core::Transform tf;
    tf.translation = t;
    tf.rotation = q;
    return tf;
}

ecs::Entity add_marker(ecs::World& w, const core::Transform& tf, bool enabled = true) {
    return w.spawn_with(ecs::LocalTransform{tf}, PlayerSpawn{enabled});
}

} // namespace

TEST_CASE("no marker, or only a disabled one, resolves to None (old default stays)") {
    ecs::World w;
    init(w);
    CHECK(resolve_player_spawn(w).status == SpawnStatus::None);
    (void)add_marker(w, pose({1, 2, 3}), /*enabled=*/false);
    const auto r = resolve_player_spawn(w);
    CHECK(r.status == SpawnStatus::None);
    CHECK(r.enabled_count == 0);
    CHECK(r.reason.empty());
}

TEST_CASE("one enabled marker resolves capsule centre and heading, ignoring roll and scale") {
    ecs::World w;
    init(w);
    const float yaw = 0.7f;
    const float pitch = -0.25f;
    // Roll about the forward axis (the pose's local Z) must not leak into yaw/pitch.
    core::Transform tf =
        pose({3, 1.2f, -5}, yaw_pitch(yaw, pitch) * core::quat_from_axis_angle({0, 0, 1}, 0.9f));
    tf.scale = {4, 4, 4}; // never sizes the avatar
    (void)add_marker(w, tf);

    const auto r = resolve_player_spawn(w);
    REQUIRE(r.status == SpawnStatus::Valid);
    CHECK(r.enabled_count == 1);
    CHECK(r.placement.capsule_centre.x == doctest::Approx(3.0f));
    CHECK(r.placement.capsule_centre.y == doctest::Approx(1.2f));
    CHECK(r.placement.capsule_centre.z == doctest::Approx(-5.0f));
    CHECK(r.placement.yaw == doctest::Approx(yaw).epsilon(1e-4));
    CHECK(r.placement.pitch == doctest::Approx(pitch).epsilon(1e-4));

    // Cross-check against the real view convention rather than the same formula: the look
    // direction of a FirstPersonView with the resolved angles is the pose's forward axis.
    gameplay::FirstPersonView v;
    v.yaw = r.placement.yaw;
    v.pitch = r.placement.pitch;
    const core::Vec3 want = core::rotate(core::normalize(tf.rotation), {0, 0, -1});
    const core::Vec3 got = gameplay::look_direction(v);
    CHECK(got.x == doctest::Approx(want.x).epsilon(1e-4));
    CHECK(got.y == doctest::Approx(want.y).epsilon(1e-4));
    CHECK(got.z == doctest::Approx(want.z).epsilon(1e-4));
}

TEST_CASE("a parented marker composes its chain, and the result survives world changes") {
    ecs::World w;
    init(w);
    const float half_pi = 1.5707964f;
    const ecs::Entity root =
        w.spawn_with(ecs::LocalTransform{pose({10, 0, 0}, yaw_pitch(half_pi, 0))});
    const ecs::Entity mid = w.spawn_with(ecs::LocalTransform{pose({0, 1, 0})}, ecs::Parent{root});
    const ecs::Entity marker =
        w.spawn_with(ecs::LocalTransform{pose({0, 0, 2})}, ecs::Parent{mid}, PlayerSpawn{true});

    const auto r = resolve_player_spawn(w);
    REQUIRE(r.status == SpawnStatus::Valid);
    // Local (0,1,2) turned +90 degrees about Y is (2,1,0); plus the root's (10,0,0).
    CHECK(r.placement.capsule_centre.x == doctest::Approx(12.0f).epsilon(1e-4));
    CHECK(r.placement.capsule_centre.y == doctest::Approx(1.0f));
    CHECK(r.placement.capsule_centre.z == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(r.placement.yaw == doctest::Approx(half_pi).epsilon(1e-4));

    // Structural churn: archetype moves, many spawns, then the marker itself despawns. The copy
    // holds no handle, so it must not change (and must not dangle).
    const auto copy = r;
    (void)w.add_component(marker, ecs::WorldTransform{});
    for (int i = 0; i < 2000; ++i) {
        (void)w.spawn_with(ecs::LocalTransform{}, ecs::WorldTransform{});
    }
    CHECK(w.despawn(marker));
    CHECK(r.placement.capsule_centre.x == copy.placement.capsule_centre.x);
    CHECK(r.placement.yaw == copy.placement.yaw);
    CHECK(resolve_player_spawn(w).status == SpawnStatus::None);
}

TEST_CASE("two enabled markers are refused whatever the storage order") {
    // Different component sets put the markers in different archetypes, and spawning them in the
    // opposite order flips which one a query would meet first.
    ecs::World a;
    init(a);
    (void)add_marker(a, pose({1, 0, 0}));
    (void)a.spawn_with(
        ecs::LocalTransform{pose({2, 0, 0})}, ecs::WorldTransform{}, PlayerSpawn{true});

    ecs::World b;
    init(b);
    (void)b.spawn_with(
        ecs::LocalTransform{pose({2, 0, 0})}, ecs::WorldTransform{}, PlayerSpawn{true});
    (void)add_marker(b, pose({1, 0, 0}));
    (void)add_marker(b, pose({9, 9, 9}), /*enabled=*/false); // disabled ones do not count

    for (ecs::World* w : {&a, &b}) {
        const auto r = resolve_player_spawn(*w);
        CHECK(r.status == SpawnStatus::Refused);
        CHECK(r.enabled_count == 2);
        CHECK(r.reason.find("exactly one") != std::string::npos);
    }
}

TEST_CASE("an enabled marker with an unusable pose is refused, never defaulted") {
    const auto refused = [](auto&& build) {
        ecs::World w;
        init(w);
        build(w);
        const auto r = resolve_player_spawn(w);
        return r.status == SpawnStatus::Refused && r.enabled_count == 1 && !r.reason.empty();
    };
    CHECK(refused([](ecs::World& w) { (void)w.spawn_with(PlayerSpawn{true}); })); // no transform
    CHECK(refused([](ecs::World& w) { (void)add_marker(w, pose({kNaN, 0, 0})); }));
    CHECK(refused([](ecs::World& w) { (void)add_marker(w, pose({0, kInf, 0})); }));
    CHECK(refused([](ecs::World& w) { (void)add_marker(w, pose({0, 0, 0}, {kNaN, 0, 0, 1})); }));
    CHECK(refused([](ecs::World& w) { (void)add_marker(w, pose({0, 0, 0}, {0, 0, 0, 0})); }));
    CHECK(refused([](ecs::World& w) { // straight up: yaw is undefined
        (void)add_marker(w, pose({0, 0, 0}, yaw_pitch(0, 1.5707964f)));
    }));
    CHECK(refused([](ecs::World& w) { // dangling parent
        const ecs::Entity gone = w.spawn_with(ecs::LocalTransform{});
        (void)w.spawn_with(ecs::LocalTransform{}, ecs::Parent{gone}, PlayerSpawn{true});
        (void)w.despawn(gone);
    }));
    CHECK(refused([](ecs::World& w) { // parent without a LocalTransform
        const ecs::Entity bare = w.spawn_with(PlayerSpawn{false});
        (void)w.spawn_with(ecs::LocalTransform{}, ecs::Parent{bare}, PlayerSpawn{true});
    }));
    CHECK(refused([](ecs::World& w) { // two-entity parent cycle
        const ecs::Entity a = w.spawn_with(ecs::LocalTransform{}, ecs::Parent{});
        const ecs::Entity m =
            w.spawn_with(ecs::LocalTransform{}, ecs::Parent{a}, PlayerSpawn{true});
        w.get<ecs::Parent>(a)->value = m;
    }));
    CHECK(refused(
        [](ecs::World& w) { (void)add_marker(w, pose({0, 0, 0}, yaw_pitch(0.3f, 0) * 3.0f)); }));
    CHECK(
        refused([](ecs::World& w) { (void)add_marker(w, pose({0, 0, 0}, {1e30f, 0, 0, 1e30f})); }));
    CHECK(refused([](ecs::World& w) {
        const auto parent = w.spawn_with(ecs::LocalTransform{pose({1, 0, 0}, {0, 1, 0, 1})});
        (void)w.spawn_with(
            ecs::LocalTransform{pose({0, 0, 2})}, ecs::Parent{parent}, PlayerSpawn{true});
    }));
    CHECK(refused([](ecs::World& w) {
        auto parent_pose = pose({0, 0, 0});
        parent_pose.scale = {1e30f, 1e30f, 1e30f};
        const auto parent = w.spawn_with(ecs::LocalTransform{parent_pose});
        (void)w.spawn_with(ecs::LocalTransform{pose({1e30f, 1e30f, 1e30f})},
                           ecs::Parent{parent},
                           PlayerSpawn{true});
    }));
}

TEST_CASE("the marker survives a real scene save/load; derived handles are not written") {
    ecs::World a;
    init(a);
    const ecs::Entity parent =
        a.spawn_with(ecs::LocalTransform{pose({4, 0, 0})}, ecs::WorldTransform{pose({4, 0, 0})});
    (void)a.spawn_with(ecs::LocalTransform{pose({0, 1.1f, 6}, yaw_pitch(-0.4f, 0.1f))},
                       ecs::WorldTransform{},
                       ecs::Parent{parent},
                       PlayerSpawn{true});
    const auto before = resolve_player_spawn(a);
    REQUIRE(before.status == SpawnStatus::Valid);

    // WorldTransform is unreflected derived state, so it simply never reaches the file.
    const std::string text = scene::save_scene_to_string(a);
    CHECK(text.find("PlayerSpawn") != std::string::npos);
    CHECK(text.find("WorldTransform") == std::string::npos);

    ecs::World b;
    init(b);
    const auto loaded = scene::load_scene_from_string(b, text);
    REQUIRE(loaded.ok);
    CHECK(loaded.skipped_components == 0);
    const auto after = resolve_player_spawn(b);
    REQUIRE(after.status == SpawnStatus::Valid);
    CHECK(after.placement.capsule_centre.x == doctest::Approx(before.placement.capsule_centre.x));
    CHECK(after.placement.capsule_centre.y == doctest::Approx(before.placement.capsule_centre.y));
    CHECK(after.placement.capsule_centre.z == doctest::Approx(before.placement.capsule_centre.z));
    CHECK(after.placement.yaw == doctest::Approx(before.placement.yaw).epsilon(1e-5));
    CHECK(after.placement.pitch == doctest::Approx(before.placement.pitch).epsilon(1e-5));

    // A strict load into a world that lacks the registration must fail loudly, which is exactly
    // why the host and the game must share one list.
    ecs::World bare;
    (void)worldkit::register_engine_components(bare);
    blockkit::register_blockkit_components(bare);
    CHECK_FALSE(scene::load_scene_from_string(bare, text).ok);
}

TEST_CASE("game and host register the same component set through one helper") {
    ecs::World game;
    init(game);
    ecs::World host;
    init(host);
    block_play::register_block_components(host); // idempotent
    CHECK(ecs::component_schema_hash(game) == ecs::component_schema_hash(host));

    ecs::World without;
    (void)worldkit::register_engine_components(without);
    blockkit::register_blockkit_components(without);
    CHECK(ecs::component_schema_hash(without) != ecs::component_schema_hash(game));
}
