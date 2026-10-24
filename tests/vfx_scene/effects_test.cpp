// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

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

TEST_CASE("environment v2: flame clock freezes, refuses atomically and restarts on Stop") {
    ecs::World w;
    prepare(w);
    const auto e = source(w, fire());
    const auto authored = scene::save_scene_to_string(w);
    vfx_scene::Effects fx;
    REQUIRE(fx.flames().empty());
    REQUIRE(fx.tick(w, 1, 0.25f));
    REQUIRE(fx.flames().size() == 3);
    const auto first = fx.flames()[0];
    CHECK(first.half_size > 0.0f);
    CHECK(first.opacity > 0.0f);
    CHECK(scene::save_scene_to_string(w) == authored);
    for (unsigned paused_frame = 0; paused_frame < 100; ++paused_frame)
        fx.reconcile(w);
    CHECK(fx.flames()[0].position.x == first.position.x);
    CHECK(fx.flames()[0].position.y == first.position.y);
    CHECK(fx.flames()[0].half_size == first.half_size);
    CHECK_FALSE(fx.tick(w, 1, 0.25f));
    CHECK_FALSE(fx.tick(w, 2, std::numeric_limits<float>::infinity()));
    CHECK(fx.flames()[0].position.x == first.position.x);
    REQUIRE(fx.tick(w, 2, 0.25f));
    CHECK(fx.flames()[0].position.x != first.position.x);

    // Extinguish/remove clears visible fire immediately, including in Pause. The existing
    // smoke pool retains its independent lifetime, instead of vanishing with the flame.
    w.get<vfx_scene::FireEmitter>(e)->intensity = 0.0f;
    fx.reconcile(w);
    CHECK(fx.flames().empty());
    CHECK(fx.lights().empty());
    CHECK(fx.field().count() > 0);
    w.get<vfx_scene::FireEmitter>(e)->intensity = 1.0f;
    fx.reset();
    REQUIRE(fx.tick(w, 1, 0.25f));
    REQUIRE(fx.flames().size() == 3);
    CHECK(fx.flames()[0].position.x == first.position.x);
    CHECK(fx.flames()[0].half_size == first.half_size);
    REQUIRE(w.despawn(e));
    fx.reconcile(w);
    CHECK(fx.flames().empty());
}

TEST_CASE("environment v2: flame tongues surge independently, smoothly and loop seamlessly") {
    ecs::World w;
    prepare(w);
    const auto a = source(w, fire());
    const auto b = source(w, fire());
    (void)b;
    constexpr float dt = 0.05f; // 160 accepted ticks per 8 s animation loop
    constexpr unsigned steps = 160;

    struct Sample {
        float x[3], y[3], z[3], size[3], opacity[3];
    };

    const auto capture = [](const vfx_scene::Effects& fx, unsigned source_index) {
        Sample s{};
        for (unsigned t = 0; t < 3; ++t) {
            const auto& f = fx.flames()[source_index * 3 + t];
            s.x[t] = f.position.x;
            s.y[t] = f.position.y;
            s.z[t] = f.position.z;
            s.size[t] = f.half_size;
            s.opacity[t] = f.opacity;
        }
        return s;
    };
    const auto run = [&](vfx_scene::Effects& fx, std::vector<Sample>& out) {
        for (std::uint64_t t = 1; t <= steps + 1; ++t) {
            REQUIRE(fx.tick(w, t, dt));
            REQUIRE(fx.flames().size() == 6);
            out.push_back(capture(fx, 0));
        }
    };
    vfx_scene::Effects fx;
    std::vector<Sample> series;
    run(fx, series);

    // Every tongue visibly varies in size, height and opacity, and the side tongues move more
    // than the anchored root, instead of one synchronized 12% scale.
    float size_span[3], y_span[3], opacity_span[3], x_span[3];
    for (unsigned t = 0; t < 3; ++t) {
        float lo_s = 1e9f, hi_s = -1e9f, lo_y = 1e9f, hi_y = -1e9f, lo_o = 2.0f, hi_o = -1.0f,
              lo_x = 1e9f, hi_x = -1e9f;
        for (const auto& s : series) {
            lo_s = std::min(lo_s, s.size[t]);
            hi_s = std::max(hi_s, s.size[t]);
            lo_y = std::min(lo_y, s.y[t]);
            hi_y = std::max(hi_y, s.y[t]);
            lo_o = std::min(lo_o, s.opacity[t]);
            hi_o = std::max(hi_o, s.opacity[t]);
            lo_x = std::min(lo_x, s.x[t]);
            hi_x = std::max(hi_x, s.x[t]);
            CHECK(s.size[t] > 0.0f);
            CHECK(s.opacity[t] >= 0.0f);
            CHECK(s.opacity[t] <= 1.0f);
        }
        size_span[t] = (hi_s - lo_s) / hi_s;
        y_span[t] = hi_y - lo_y;
        opacity_span[t] = hi_o - lo_o;
        x_span[t] = hi_x - lo_x;
        CHECK(size_span[t] > 0.10f);
        CHECK(y_span[t] > 0.0f);
        CHECK(opacity_span[t] > 0.02f);
    }
    CHECK(size_span[1] > size_span[0]);
    CHECK(opacity_span[1] > opacity_span[0]);
    CHECK(x_span[1] > x_span[0]);

    // Smooth: no tick moves a sprite or its size by more than a fraction of its own size, which
    // a frame-random or sawtooth-reset implementation would violate.
    for (std::size_t i = 1; i < series.size(); ++i)
        for (unsigned t = 0; t < 3; ++t) {
            const float size = series[i].size[t];
            CHECK(std::abs(series[i].size[t] - series[i - 1].size[t]) < 0.12f * size);
            CHECK(std::abs(series[i].x[t] - series[i - 1].x[t]) < 0.25f * size);
            CHECK(std::abs(series[i].y[t] - series[i - 1].y[t]) < 0.25f * size);
            CHECK(std::abs(series[i].opacity[t] - series[i - 1].opacity[t]) < 0.08f);
        }

    // Seamless: exactly one loop later (8 s of accepted time) the pose has returned.
    for (unsigned t = 0; t < 3; ++t) {
        CHECK(series.front().x[t] == doctest::Approx(series.back().x[t]).epsilon(1e-3));
        CHECK(series.front().y[t] == doctest::Approx(series.back().y[t]).epsilon(1e-3));
        CHECK(series.front().size[t] == doctest::Approx(series.back().size[t]).epsilon(1e-3));
        CHECK(series.front().opacity[t] == doctest::Approx(series.back().opacity[t]).epsilon(1e-3));
    }

    // Source identity: a second source with the same emitter is phased differently.
    fx.reset();
    REQUIRE(fx.tick(w, 1, dt));
    const Sample first_a = capture(fx, 0);
    const Sample first_b = capture(fx, 1);
    CHECK(first_a.size[1] != first_b.size[1]);
    CHECK(first_a.y[1] != first_b.y[1]);

    // Frozen outside accepted ticks, deterministic on replay.
    const Sample frozen = capture(fx, 0);
    fx.reconcile(w);
    CHECK_FALSE(fx.tick(w, 1, dt));
    const Sample after = capture(fx, 0);
    for (unsigned t = 0; t < 3; ++t) {
        CHECK(after.x[t] == frozen.x[t]);
        CHECK(after.size[t] == frozen.size[t]);
        CHECK(after.opacity[t] == frozen.opacity[t]);
    }
    fx.reset();
    std::vector<Sample> replay;
    run(fx, replay);
    REQUIRE(replay.size() == series.size());
    for (std::size_t i = 0; i < series.size(); ++i)
        for (unsigned t = 0; t < 3; ++t) {
            CHECK(replay[i].x[t] == series[i].x[t]);
            CHECK(replay[i].size[t] == series[i].size[t]);
        }
    (void)a;
}

TEST_CASE("environment v2: flame sprites remain capped independently of smoke saturation") {
    ecs::World w;
    prepare(w);
    for (std::size_t i = 0; i < vfx_scene::Effects::max_sources + 8; ++i)
        (void)source(w, fire(1024.0f));
    vfx_scene::Effects fx;
    for (std::uint64_t t = 1; t <= 100; ++t) {
        REQUIRE(fx.tick(w, t, 0.25f));
        REQUIRE(fx.flames().size() == vfx_scene::Effects::max_sources * 3);
        REQUIRE(fx.render_flames().size() <= vfx_scene::Effects::max_flames);
        CHECK(std::count_if(fx.render_flames().begin(),
                            fx.render_flames().end(),
                            [](const auto& flame) { return flame.core; }) ==
              vfx_scene::Effects::max_sources * vfx_scene::Effects::core_flames_per_source);
        for (const auto& flame : fx.flames()) {
            CHECK(std::isfinite(flame.position.x));
            CHECK(std::isfinite(flame.position.y));
            CHECK(std::isfinite(flame.position.z));
            CHECK(flame.half_size > 0.0f);
            CHECK(flame.opacity >= 0.0f);
            CHECK(flame.opacity <= 1.0f);
        }
    }
    CHECK(fx.stats().source_cap_drops > 0);
    CHECK(fx.field().stats().dropped_capacity > 0);
    fx.reset();
    CHECK(fx.flames().empty());
}

TEST_CASE("fire motes replay deterministically within the bounded render budget") {
    ecs::World w;
    prepare(w);
    (void)source(w, fire());
    vfx_scene::Effects fx;

    struct Frame {
        std::size_t count;
        core::Vec3 position;
        float size;
        float opacity;
    };

    std::vector<Frame> first;
    const auto run = [&](std::vector<Frame>& frames) {
        bool saw_ember = false;
        bool saw_extra_flame = false;
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            REQUIRE(fx.tick(w, tick, 0.05f));
            const auto rendered = fx.render_flames();
            REQUIRE(fx.flames().size() == 3);
            REQUIRE(rendered.size() <= vfx_scene::Effects::max_flames);
            if (rendered.size() > 3) {
                saw_extra_flame = true;
                const auto& mote = rendered.back();
                frames.push_back({rendered.size(), mote.position, mote.half_size, mote.opacity});
            } else {
                frames.push_back({rendered.size(), {}, 0.0f, 0.0f});
            }
            for (const auto& mote : rendered.subspan(3))
                saw_ember |= mote.half_size < 0.04f;
        }
        CHECK(saw_extra_flame);
        CHECK(saw_ember);
    };
    run(first);
    fx.reset();
    std::vector<Frame> replay;
    run(replay);
    REQUIRE(first.size() == replay.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].count == replay[i].count);
        CHECK(first[i].position.x == replay[i].position.x);
        CHECK(first[i].position.y == replay[i].position.y);
        CHECK(first[i].position.z == replay[i].position.z);
        CHECK(first[i].size == replay[i].size);
        CHECK(first[i].opacity == replay[i].opacity);
    }
}

TEST_CASE("environment v2: smoke has soft lifetime and bounded local fire illumination") {
    vfx::Particle p{};
    p.lifetime = 4.0f;
    CHECK(vfx_scene::smoke_appearance(p, {}).opacity == 0.0f);
    p.age = 1.0f;
    const auto unlit = vfx_scene::smoke_appearance(p, {});
    CHECK(unlit.opacity > 0.0f);
    vfx::FireLightContribution light{{0.0f, 0.0f, 0.0f}, {1.0f, 0.35f, 0.08f}, 1.0f, 2.0f};
    const auto lit = vfx_scene::smoke_appearance(p, {&light, 1});
    CHECK(lit.color.x > unlit.color.x);
    CHECK(lit.color.y > unlit.color.y);
    CHECK(lit.color.x - unlit.color.x > lit.color.z - unlit.color.z);
    CHECK(lit.opacity == unlit.opacity);
    p.position.x = 3.0f;
    const auto outside = vfx_scene::smoke_appearance(p, {&light, 1});
    CHECK(outside.color.x == unlit.color.x);
    p.age = 3.99f;
    CHECK(vfx_scene::smoke_appearance(p, {}).opacity < unlit.opacity * 0.01f);
    p.age = 4.0f;
    CHECK(vfx_scene::smoke_appearance(p, {}).opacity == 0.0f);
    p.age = 1.0f;
    p.lifetime = std::numeric_limits<float>::quiet_NaN();
    CHECK(vfx_scene::smoke_appearance(p, {}).opacity == 0.0f);
    p.lifetime = 4.0f;
    light.radius = std::numeric_limits<float>::infinity();
    CHECK(vfx_scene::smoke_appearance(p, {&light, 1}).color.x == unlit.color.x);
}
