// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "../render/render_test_support.hpp"
#include "authored_environment.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/vfx_scene/components.hpp"

using namespace rime;
using block_play::AuthoredEnvironment;

namespace {
void prepare(ecs::World& world) {
    (void)world.register_component<ecs::WorldTransform>();
    vfx_scene::register_vfx_scene_components(world);
    fluids_scene::register_fluids_scene_components(world);
    fluids_render::register_fluids_render_components(world);
}

ecs::Entity fire(ecs::World& world) {
    vfx_scene::FireEmitter source;
    source.enabled = true;
    source.particles_per_second = 16;
    core::Transform pose;
    pose.translation = {0, 0, -3};
    return world.spawn_with(source, ecs::WorldTransform{pose});
}

std::vector<std::string>
passes(render::SceneRenderer& renderer, ecs::World& world, rhi::Device& device) {
    render::RenderGraph graph(device);
    (void)renderer.render(graph, world, {64, 64}, true);
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < graph.pass_count(); ++i)
        names.emplace_back(graph.pass_name(i));
    return names;
}

bool has(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
} // namespace

// One fire source draws its fixed flame layers (three stable tongues, the fuel cores and the
// upper tongues) plus a seeded, time-varying number of motes. Assert that envelope rather than
// a literal, so retuning the fire's look does not silently break the "fire emits" contract.
[[nodiscard]] static bool one_fire_of_flames(std::size_t count) {
    using E = rime::vfx_scene::Effects;
    constexpr std::size_t fixed = 3 + E::core_flames_per_source + E::upper_tongues_per_source;
    return count >= fixed && count <= fixed + E::max_fire_motes_per_source;
}

TEST_CASE("city authored environment: inert sources leave legacy ownership and atlas untouched") {
    ecs::World world;
    prepare(world);
    (void)world.spawn_with(vfx_scene::FireEmitter{}, ecs::WorldTransform{});
    (void)world.spawn_with(vfx_scene::SmokeEmitter{}, ecs::WorldTransform{});
    (void)world.spawn_with(fluids_scene::ThermalCell{}, fluids_render::ThermalSurface{true});
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 1, 1. / 60.));
    const auto& snapshot = environment.prepare(world);
    CHECK_FALSE(snapshot.override_smoke);
    CHECK(snapshot.enabled_sources == 0);
    CHECK(snapshot.smoke_count == 0);
    CHECK(snapshot.flame_count == 0);
    CHECK(snapshot.light_count == 0);
    CHECK(snapshot.surfaces.count == 0);
    CHECK_FALSE(environment.detail_allocated());
}

TEST_CASE("city authored environment: accepted fire ticks emit without the scripted preview flag") {
    ecs::World world;
    prepare(world);
    (void)fire(world);
    vfx_scene::SmokeEmitter plume;
    plume.enabled = true;
    plume.particles_per_second = 16;
    (void)world.spawn_with(plume, ecs::WorldTransform{});
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 10, .25));
    auto first = environment.prepare(world);
    REQUIRE(one_fire_of_flames(first.flame_count));
    REQUIRE(first.light_count == 1);
    REQUIRE(first.smoke_count > 0);
    CHECK(first.enabled_sources == 2);
    CHECK(environment.effects().source_count() == 2);
    CHECK(first.override_smoke);
    const auto age = environment.effects().field().particles()[0].age;
    const auto accepted = environment.effects().stats().accepted_ticks;
    const auto again = environment.prepare(world);
    CHECK(again.smoke_count == first.smoke_count);
    CHECK(std::memcmp(again.flames.data(), first.flames.data(), sizeof(first.flames)) == 0);
    CHECK(environment.effects().field().particles()[0].age == age);
    CHECK(environment.effects().stats().accepted_ticks == accepted);
    CHECK_FALSE(environment.tick(world, 10, .25));
    CHECK_FALSE(environment.tick(world, 9, .25));
    CHECK_FALSE(environment.tick(world, 11, 0));
    CHECK_FALSE(environment.tick(world, 11, .250000001));
    CHECK_FALSE(environment.tick(world, 11, std::numeric_limits<double>::quiet_NaN()));
    CHECK(environment.effects().field().particles()[0].age == age);
    REQUIRE(environment.tick(world, 11, .25));
    CHECK(environment.effects().field().particles()[0].age > age);
}

TEST_CASE("city authored environment: composition bounds legacy and authored smoke separately") {
    ecs::World world;
    prepare(world);
    (void)fire(world);
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 1, .25));
    render::GpuParticle legacy{{7, 8, 9, 1}, {.1f, .2f, .3f, .4f}};
    const auto& combined = environment.prepare(world, {&legacy, 1});
    CHECK(combined.smoke_count == environment.effects().field().count() + 1);
    CHECK(std::memcmp(combined.smoke.data(), &legacy, sizeof(legacy)) == 0);
    std::vector<render::GpuParticle> over(300, legacy);
    const auto& bounded = environment.prepare(world, over);
    CHECK(bounded.smoke_count == environment.effects().field().count() + 256);
    CHECK(bounded.smoke_count <= AuthoredEnvironment::smoke_capacity);
    CHECK(environment.legacy_smoke_dropped() == 44);
}

TEST_CASE("city authored environment: removal clears fire while residual smoke retires and reset "
          "restarts") {
    ecs::World world;
    prepare(world);
    const auto source = fire(world);
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 10, .25));
    REQUIRE(one_fire_of_flames(environment.prepare(world).flame_count));
    REQUIRE(world.despawn(source));
    const auto& removed = environment.prepare(world);
    CHECK(removed.flame_count == 0);
    CHECK(removed.light_count == 0);
    CHECK(removed.smoke_count > 0);
    CHECK(environment.effects().source_count() == 0);
    for (std::uint64_t tick = 11; tick < 200; ++tick)
        REQUIRE(environment.tick(world, tick, .25));
    CHECK(environment.prepare(world).smoke_count == 0);
    CHECK_FALSE(environment.prepare(world).override_smoke);
    (void)fire(world);
    REQUIRE(environment.tick(world, 200, .25));
    REQUIRE(environment.prepare(world).light_count == 1);
    environment.reset();
    CHECK(environment.snapshot().flame_count == 0);
    CHECK(environment.snapshot().light_count == 0);
    CHECK(environment.effects().field().count() == 0);
    REQUIRE(environment.tick(world, 1, .25));
    CHECK(one_fire_of_flames(environment.prepare(world).flame_count));
}

TEST_CASE(
    "city authored environment: thermal opt-in uses committed phase and refuses invalid edits") {
    ecs::World world;
    prepare(world);
    fluids_scene::ThermalCell cell;
    cell.enabled = true;
    cell.liquid_mass_fraction = 0;
    const auto entity = world.spawn_with(cell, fluids_render::ThermalSurface{});
    (void)world.spawn_with(fluids_scene::ThermalSource{entity, 10000, true});
    AuthoredEnvironment environment;
    CHECK(environment.prepare(world).surfaces.count == 0);
    CHECK_FALSE(environment.detail_allocated());
    world.get<fluids_render::ThermalSurface>(entity)->enabled = true;
    const auto frozen = environment.prepare(world).surfaces;
    REQUIRE(frozen.count == 1);
    CHECK(frozen.rows[0].entity == entity);
    CHECK(frozen.rows[0].roughness == doctest::Approx(.22f));
    for (std::uint64_t tick = 1; tick <= 600; ++tick)
        REQUIRE(environment.tick(world, tick, 1. / 60.));
    const auto melted = environment.prepare(world).surfaces;
    CHECK(melted.rows[0].roughness < frozen.rows[0].roughness);
    CHECK(melted.rows[0].base_color.x < frozen.rows[0].base_color.x);
    const auto seconds = environment.thermal().presentation_seconds();
    const auto field_tick = environment.thermal().field()->tick();
    (void)environment.prepare(world);
    CHECK(environment.thermal().presentation_seconds() == seconds);
    CHECK(environment.thermal().field()->tick() == field_tick);
    world.get<fluids_scene::ThermalCell>(entity)->mass_kg = 0;
    CHECK(environment.prepare(world).surfaces.count == 0);
    CHECK(environment.thermal().status() == fluids_scene::PreviewStatus::InvalidDescription);
    CHECK_FALSE(environment.thermal().field());
    CHECK_FALSE(environment.detail_allocated());
    CHECK_FALSE(environment.tick(world, 601, 1. / 60.));
}

TEST_CASE("city authored environment: thermal numerical refusal holds committed appearance") {
    ecs::World world;
    prepare(world);
    fluids_scene::ThermalCell cell;
    cell.enabled = true;
    cell.temperature_kelvin = 313;
    const auto entity = world.spawn_with(cell, fluids_render::ThermalSurface{true});
    (void)world.spawn_with(fluids_scene::ThermalSource{entity, 10000, true});
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 1, .001));
    const auto before = environment.prepare(world).surfaces;
    const auto seconds = environment.thermal().presentation_seconds();
    CHECK_FALSE(environment.tick(world, 2, .25));
    CHECK(environment.thermal().status() == fluids_scene::PreviewStatus::StepRefused);
    const auto after = environment.prepare(world).surfaces;
    REQUIRE(after.count == 1);
    CHECK(after.rows[0].base_color.x == before.rows[0].base_color.x);
    CHECK(after.rows[0].roughness == before.rows[0].roughness);
    CHECK(environment.thermal().presentation_seconds() == seconds);
}

TEST_CASE("city authored environment: render publication preserves legacy toggle and adds authored "
          "passes") {
    auto device = rhi::create_device({});
    if (!device) {
        if (render::test::vulkan_required())
            FAIL("Vulkan required");
        return;
    }
    ecs::World world;
    prepare(world);
    render::register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, render::Camera{});
    render::MeshRegistry meshes(*device);
    render::MaterialRegistry materials;
    AuthoredEnvironment environment(device.get()); // outlives both renderer copies of its maps
    render::SceneRenderer main(*device, meshes, materials);
    render::SceneRenderer scope(*device, meshes, materials);
    main.set_temporal_jitter_enabled(false);
    main.set_taa_resolve_enabled(false);
    scope.set_temporal_jitter_enabled(false);
    scope.set_taa_resolve_enabled(false);
    const auto baseline = passes(main, world, *device);
    environment.publish(world, {}, main, &scope);
    CHECK(passes(main, world, *device) == baseline);
    CHECK_FALSE(environment.detail_allocated());
    const render::GpuParticle legacy{{0, 0, -3, 1}, {.1f, .2f, .3f, .4f}};
    render::FxSettings on;
    on.enabled = true;
    main.set_smoke_particles({&legacy, 1}, on);
    environment.publish(world, {&legacy, 1}, main, &scope);
    CHECK(has(passes(main, world, *device), "fx-smoke"));
    main.set_smoke_particles({}, {});
    environment.publish(world, {}, main, &scope);
    CHECK_FALSE(has(passes(main, world, *device), "fx-smoke"));
    (void)fire(world);
    REQUIRE(environment.tick(world, 1, .25));
    environment.publish(world, {&legacy, 1}, main, &scope);
    const auto main_passes = passes(main, world, *device);
    const auto scope_passes = passes(scope, world, *device);
    CHECK(has(main_passes, "fx-smoke"));
    CHECK(has(main_passes, "fx-flame"));
    CHECK(has(scope_passes, "fx-smoke"));
    CHECK(has(scope_passes, "fx-flame"));
    CHECK(environment.snapshot().smoke_count == environment.effects().field().count() + 1);
    const auto ticks = environment.effects().stats().accepted_ticks;
    environment.apply_to(scope);
    CHECK(environment.effects().stats().accepted_ticks == ticks);
    CHECK_FALSE(environment.detail_allocated());
}

TEST_CASE("city authored environment: invalid enabled sources remain observable and inert") {
    ecs::World world;
    prepare(world);
    const auto entity = fire(world);
    world.get<vfx_scene::FireEmitter>(entity)->particles_per_second = 4096;
    AuthoredEnvironment environment;
    REQUIRE(environment.tick(world, 1, .25));
    const auto& snapshot = environment.prepare(world);
    CHECK(snapshot.enabled_sources == 1);
    CHECK(environment.effects().source_count() == 0);
    CHECK(environment.effects().stats().invalid_sources > 0);
    CHECK_FALSE(snapshot.override_smoke);
    CHECK(snapshot.flame_count == 0);
    CHECK(snapshot.light_count == 0);
    CHECK_FALSE(environment.detail_allocated());
}

TEST_CASE("city authored environment: lazy phase maps follow accepted time and clear across "
          "generations") {
    auto device = rhi::create_device({});
    if (!device) {
        if (render::test::vulkan_required())
            FAIL("Vulkan required");
        return;
    }
    ecs::World world;
    prepare(world);
    render::register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, render::Camera{});
    render::MeshRegistry meshes(*device);
    render::MaterialRegistry materials;
    const auto mesh = meshes.add(render::make_cube(.6f), "authored-phase-cube");
    const auto material = materials.add({{.8f, .8f, .8f, 1}, 0, .7f});
    core::Transform pose;
    pose.translation = {0, 0, -3};
    fluids_scene::ThermalCell cell;
    cell.enabled = true;
    cell.liquid_mass_fraction = .5;
    const auto target = world.spawn_with(ecs::WorldTransform{pose},
                                         render::MeshRef{mesh},
                                         render::MaterialRef{material},
                                         cell,
                                         fluids_render::ThermalSurface{});
    AuthoredEnvironment environment(device.get());
    render::SceneRenderer renderer(*device, meshes, materials);
    renderer.set_temporal_jitter_enabled(false);
    renderer.set_taa_resolve_enabled(false);
    renderer.set_ambient(.5f, .5f, .5f);
    const auto frame = [&] {
        render::RenderGraph graph(*device);
        const auto output = renderer.render(graph, world, {64, 64}, true);
        graph.export_texture(output.hdr);
        auto commands = device->begin_commands();
        graph.execute(*commands);
        device->submit_blocking(*commands);
        return render::test::read_texture(*device, graph.physical(output.hdr), 64, 64, 8);
    };
    environment.publish(world, {}, renderer);
    CHECK_FALSE(environment.detail_allocated()); // a valid cell alone is not visual opt-in
    const auto baseline = frame();
    world.get<fluids_render::ThermalSurface>(target)->enabled = true;
    environment.publish(world, {}, renderer);
    REQUIRE(environment.detail_ready());
    REQUIRE(environment.snapshot().surfaces.count == 1);
    const auto initial = environment.snapshot().surfaces.rows[0];
    REQUIRE(initial.override_detail);
    CHECK(initial.entity == target);
    const auto phase = frame();
    CHECK(phase != baseline); // actual game-publication rows affect rendered pixels
    environment.publish(world, {}, renderer);
    CHECK(environment.snapshot().surfaces.rows[0].normal_texture == initial.normal_texture);
    CHECK(frame() == phase); // repeated render/Pause cannot advance immutable map selection
    REQUIRE(environment.tick(world, 1, .25));
    environment.publish(world, {}, renderer);
    // Phase shading is continuous: the detail map stays fixed and the accepted presentation
    // clock reaches the shader per row, so time shows up as the row's seconds and as pixels.
    CHECK(environment.snapshot().surfaces.rows[0].normal_texture == initial.normal_texture);
    CHECK(initial.seconds == 0.f);
    CHECK(environment.snapshot().surfaces.rows[0].seconds == .25f);
    CHECK(frame() != phase);
    CHECK(environment.thermal().presentation_seconds() == .25);
    environment.reset();
    environment.apply_to(renderer);
    CHECK(frame() == baseline);
    CHECK(environment.detail_allocated()); // borrowed handles live until both renderers die
    REQUIRE(world.despawn(target));
    const auto replacement = world.spawn_with(
        ecs::WorldTransform{pose}, render::MeshRef{mesh}, render::MaterialRef{material});
    CHECK(replacement != target);
    environment.publish(world, {}, renderer);
    CHECK(environment.snapshot().surfaces.count == 0);
    CHECK(frame() == baseline); // old entity generation cannot tint its replacement
}
