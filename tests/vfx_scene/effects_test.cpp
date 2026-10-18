// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/reflect.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/editorhost/editor_host.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/vfx_scene/components.hpp"
#include "rime/vfx_scene/effects.hpp"
#include "rime/worldkit/profile.hpp"
#include "support/temp_path.hpp"

using namespace rime;

namespace {
void prepare(ecs::World& w) {
    worldkit::register_engine_components(w);
}

template <class T> ecs::Entity source(ecs::World& w, T c = {}) {
    return w.spawn_with(ecs::LocalTransform{}, ecs::WorldTransform{}, c);
}

vfx_scene::SmokeEmitter smoke(float rate = 4.0f) {
    vfx_scene::SmokeEmitter c;
    c.enabled = true;
    c.particles_per_second = rate;
    return c;
}

vfx_scene::FireEmitter fire(float rate = 4.0f) {
    vfx_scene::FireEmitter c;
    c.enabled = true;
    c.particles_per_second = rate;
    return c;
}
} // namespace

TEST_CASE("environment authoring: defaults are inert and runtime is excluded from scene data") {
    ecs::World w;
    prepare(w);
    CHECK(w.is_registered<vfx_scene::SmokeEmitter>());
    CHECK(w.is_registered<vfx_scene::FireEmitter>());
    const auto plume = source(w, vfx_scene::SmokeEmitter{});
    const auto flame = source(w, vfx_scene::FireEmitter{});
    vfx_scene::Effects fx;
    REQUIRE(fx.tick(w, 1, 0.25f));
    CHECK(fx.field().count() == 0);
    CHECK(fx.lights().empty());
    CHECK(fx.source_count() == 0);
    *w.get<vfx_scene::SmokeEmitter>(plume) = smoke();
    *w.get<vfx_scene::FireEmitter>(flame) = fire();
    const auto hash = editorhost::world_content_hash(w);
    REQUIRE(fx.tick(w, 2, 0.25f));
    CHECK(fx.field().count() == 2);
    REQUIRE(fx.lights().size() == 1);
    CHECK(fx.lights()[0].intensity == 1.0f);
    CHECK(editorhost::world_content_hash(w) == hash);
    CHECK(scene::save_scene_to_string(w).find("PointLight") == std::string::npos);
}

TEST_CASE("environment authoring: inspector inverse and save reload preserve both descriptions") {
    ecs::World w;
    prepare(w);
    const auto plume = source(w, smoke());
    const auto flame = source(w, fire());
    const auto old = core::serialize(*w.get<vfx_scene::SmokeEmitter>(plume));
    auto changed = smoke(20.0f);
    changed.half_x = 1.5f;
    const auto bytes = core::serialize(changed);
    const auto hash = core::reflect<vfx_scene::SmokeEmitter>().type_hash;
    // These are the inspector's SetComponent and inverse bytes, applied by the real host seam.
    REQUIRE(editorhost::apply_set_component(w, plume, hash, bytes));
    CHECK(w.get<vfx_scene::SmokeEmitter>(plume)->particles_per_second == 20.0f);
    REQUIRE(editorhost::apply_set_component(w, plume, hash, old));
    CHECK(w.get<vfx_scene::SmokeEmitter>(plume)->particles_per_second == 4.0f);
    REQUIRE(editorhost::apply_set_component(w, plume, hash, bytes));
    w.get<vfx_scene::FireEmitter>(flame)->intensity = 2.0f;
    const auto path = test::temp_path("environment-authoring.rscene");
    editorhost::HostedScene hosted;
    REQUIRE(editorhost::save_hosted_scene(w, hosted, path.string()).ok);
    ecs::World loaded;
    prepare(loaded);
    const auto back = scene::load_scene_file(loaded, path);
    REQUIRE_MESSAGE(back.ok, back.error);
    CHECK(back.skipped_components == 0);
    CHECK(editorhost::world_content_hash(loaded) == editorhost::world_content_hash(w));
    core::JobSystem jobs;
    scene::derive_world_transforms(loaded, jobs);
    vfx_scene::Effects fx;
    REQUIRE(fx.tick(loaded, 1, 0.25f));
    CHECK(fx.field().count() == 7); // 20/s plume + 4/s * intensity 2 fire
    REQUIRE(fx.lights().size() == 1);
    CHECK(fx.lights()[0].intensity == 2.0f);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("environment preview: duplicate ticks pause removal extinguish and Stop are bounded") {
    ecs::World w;
    prepare(w);
    const auto e = source(w, fire());
    editorhost::PlaySession play;
    play.play(w);
    vfx_scene::Effects fx;
    REQUIRE(fx.tick(w, 10, 0.25f));
    REQUIRE(fx.field().count() == 1);
    CHECK_FALSE(fx.tick(w, 10, 0.25f));
    CHECK_FALSE(fx.tick(w, 9, 0.25f));
    CHECK_FALSE(fx.tick(w, 11, 0.0f));
    CHECK_FALSE(fx.tick(w, 11, 0.26f));
    CHECK_FALSE(fx.tick(w, 11, std::numeric_limits<float>::quiet_NaN()));
    CHECK(fx.field().particles()[0].age == 0.0f);
    play.pause(w);
    fx.reconcile(w);
    CHECK(fx.field().particles()[0].age == 0.0f);
    CHECK(fx.lights().size() == 1);
    w.get<vfx_scene::FireEmitter>(e)->intensity = 0.0f;
    fx.reconcile(w);
    CHECK(fx.lights().empty()); // clears even without another simulation tick
    REQUIRE(fx.tick(w, 11, 0.25f));
    CHECK(fx.field().count() == 1); // residual smoke has its normal lifetime
    CHECK(fx.field().particles()[0].age == 0.25f);
    *w.get<vfx_scene::FireEmitter>(e) = fire();
    REQUIRE(fx.tick(w, 12, 0.25f));
    REQUIRE(fx.lights().size() == 1);
    REQUIRE(w.remove_component<vfx_scene::FireEmitter>(e));
    fx.reconcile(w);
    CHECK(fx.lights().empty());
    CHECK(fx.source_count() == 0);
    REQUIRE(play.stop(w));
    fx.reset();
    CHECK(fx.field().count() == 0);
    CHECK(fx.lights().empty());
    REQUIRE(fx.tick(w, 1, 0.25f)); // fresh clock and restored authored fire
    CHECK(fx.field().count() == 1);
    CHECK(fx.lights().size() == 1);
}

TEST_CASE("environment preview: invalid edits retire sources and generation reuse has no debt") {
    ecs::World w;
    prepare(w);
    auto e = source(w, fire(3.0f));
    vfx_scene::Effects fx;
    REQUIRE(fx.tick(w, 1, 0.25f)); // 0.75 particle of debt
    REQUIRE(fx.lights().size() == 1);
    w.get<vfx_scene::FireEmitter>(e)->half_x = -1.0f;
    fx.reconcile(w);
    CHECK(fx.lights().empty());
    CHECK(fx.stats().invalid_sources == 1);
    REQUIRE(w.despawn(e));
    const auto replacement = source(w, fire(3.0f));
    CHECK(replacement != e);
    REQUIRE(fx.tick(w, 2, 0.25f));
    CHECK(fx.field().count() == 0);
    REQUIRE(fx.tick(w, 3, 0.25f));
    CHECK(fx.field().count() == 1);
    w.get<ecs::WorldTransform>(replacement)->value.translation.x =
        std::numeric_limits<float>::infinity();
    fx.reconcile(w);
    CHECK(fx.lights().empty());
    CHECK(fx.source_count() == 0);
}

TEST_CASE("environment preview: moving fire keeps fractional emission and reports bounded debt") {
    ecs::World w;
    prepare(w);
    const auto e = source(w, fire(18.0f));
    vfx_scene::Effects fx;
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        w.get<ecs::WorldTransform>(e)->value.translation.x = static_cast<float>(tick) * 0.01f;
        REQUIRE(fx.tick(w, tick, 1.0f / 60.0f));
    }
    CHECK(fx.field().count() == 18);
    REQUIRE(fx.lights().size() == 1);
    CHECK(fx.lights()[0].position.x == doctest::Approx(0.6f));

    w.get<vfx_scene::FireEmitter>(e)->particles_per_second = 1024.0f;
    REQUIRE(fx.tick(w, 61, 0.25f));
    CHECK(fx.stats().emission_budget_dropped_particles == 232);
    CHECK(fx.stats().deferred_particles == 16);
    w.get<vfx_scene::FireEmitter>(e)->enabled = false;
    fx.reconcile(w);
    CHECK(fx.stats().discarded_source_particles == 16);
    CHECK(fx.stats().deferred_particles == 0);
    CHECK(fx.lights().empty());
}

TEST_CASE("environment preview: transformed bounds and admission ignore archetype ordering") {
    ecs::World w;
    prepare(w);
    auto c = fire();
    c.half_x = 2.0f;
    c.half_y = 0.5f;
    c.half_z = 0.25f;
    const auto e = source(w, c);
    auto& tf = w.get<ecs::WorldTransform>(e)->value;
    tf.translation = {10.0f, 2.0f, -3.0f};
    // 90 degrees about Y swaps local X/Z extents, including the reflected negative scale.
    const float h = std::sqrt(0.5f);
    tf.rotation = {0.0f, h, 0.0f, h};
    tf.scale = {-2.0f, 1.0f, 1.0f};
    vfx_scene::Effects fx;
    REQUIRE(fx.tick(w, 1, 0.25f));
    REQUIRE(fx.lights().size() == 1);
    CHECK(fx.lights()[0].position.x == doctest::Approx(10.0f));
    CHECK(fx.lights()[0].position.y == doctest::Approx(2.0f));
    CHECK(fx.lights()[0].position.z == doctest::Approx(-3.0f));
    CHECK(fx.lights()[0].radius == 8.0f); // transformed width 8m, capped by fire adapter
    for (std::size_t i = 1; i < vfx_scene::Effects::max_sources + 3; ++i)
        (void)source(w, fire());
    // Move the lowest key into a different archetype; it must still be admitted first.
    w.add_component<vfx_scene::SmokeEmitter>(e, smoke());
    fx.reset();
    REQUIRE(fx.tick(w, 1, 0.25f));
    CHECK(fx.source_count() == vfx_scene::Effects::max_sources);
    CHECK(fx.stats().source_cap_drops == 4);
    REQUIRE(fx.lights().size() == 15);
    CHECK(fx.lights()[0].position.x == doctest::Approx(10.0f));
    for (std::uint64_t tick = 2; tick < 300; ++tick)
        REQUIRE(fx.tick(w, tick, 0.25f));
    CHECK(fx.field().count() <= vfx_scene::Effects::max_particles);
    CHECK(fx.field().stats().dropped_capacity > 0);
}
