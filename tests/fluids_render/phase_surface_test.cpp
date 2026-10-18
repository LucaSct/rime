// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>

#include "../render/render_test_support.hpp"
#include "rime/fluids_render/phase_surface.hpp"
#include "rime/scene/scene_format.hpp"
using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

TEST_CASE("phase surfaces: opt-in accepted phase, paused bindings and invalid graph clear") {
    ecs::World w;
    fluids_scene::register_fluids_scene_components(w);
    fluids_render::register_fluids_render_components(w);
    fluids_scene::ThermalCell c;
    c.enabled = true;
    c.liquid_mass_fraction = 0;
    const auto e = w.spawn_with(c, fluids_render::ThermalSurface{});
    (void)w.spawn_with(fluids_scene::ThermalSource{e, 10000, true});
    fluids_scene::ThermalPreview p;
    p.reconcile(w);
    CHECK(fluids_render::phase_surfaces(w, p).count == 0);
    w.get<fluids_render::ThermalSurface>(e)->enabled = true;
    const auto saved = scene::save_scene_to_string(w);
    const auto frozen = fluids_render::phase_surfaces(w, p);
    REQUIRE(frozen.count == 1);
    CHECK(frozen.rows[0].roughness == doctest::Approx(.68f));
    for (std::uint64_t tick = 1; tick <= 600; ++tick)
        REQUIRE(p.tick(w, tick, 1. / 60.));
    const auto melted = fluids_render::phase_surfaces(w, p);
    CHECK(melted.rows[0].base_color.x < frozen.rows[0].base_color.x);
    CHECK(melted.rows[0].roughness < frozen.rows[0].roughness);
    CHECK(scene::save_scene_to_string(w) == saved);
    const auto tick = p.field()->tick();
    const auto revision = p.revision();
    w.get<fluids_render::ThermalSurface>(e)->enabled = false;
    p.reconcile(w);
    CHECK(p.field()->tick() == tick);
    CHECK(p.revision() == revision);
    CHECK(fluids_render::phase_surfaces(w, p).count == 0);
    w.get<fluids_render::ThermalSurface>(e)->enabled = true;
    w.get<fluids_scene::ThermalCell>(e)->mass_kg = 0;
    p.reconcile(w);
    CHECK(fluids_render::phase_surfaces(w, p).count == 0);
    p.reset();
    CHECK(fluids_render::phase_surfaces(w, p).count == 0);
}

TEST_CASE("phase surfaces: numerical refusal never publishes tentative appearance") {
    ecs::World w;
    fluids_scene::register_fluids_scene_components(w);
    fluids_render::register_fluids_render_components(w);
    fluids_scene::ThermalCell c;
    c.enabled = true;
    c.temperature_kelvin = 313.0;
    const auto e = w.spawn_with(c, fluids_render::ThermalSurface{true});
    (void)w.spawn_with(fluids_scene::ThermalSource{e, 1e15, true});
    fluids_scene::ThermalPreview p;
    p.reconcile(w);
    const auto before = fluids_render::phase_surfaces(w, p);
    CHECK_FALSE(p.tick(w, 1, 1. / 60.));
    CHECK(p.status() == fluids_scene::PreviewStatus::StepRefused);
    const auto after = fluids_render::phase_surfaces(w, p);
    REQUIRE(after.count == 1);
    CHECK(after.rows[0].base_color.x == before.rows[0].base_color.x);
    CHECK(after.rows[0].roughness == before.rows[0].roughness);
}

TEST_CASE(
    "phase surfaces: real accepted freezing changes pixels, neighbors and clearing are exact") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("Vulkan required");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const auto mesh = meshes.add(make_cube(.45f), "phase-proof");
    const auto material = materials.add({{.8f, .8f, .8f, 1}, 0, .7f});
    ecs::World w;
    register_render_components(w);
    fluids_scene::register_fluids_scene_components(w);
    fluids_render::register_fluids_render_components(w);
    (void)w.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform left;
    left.translation = {-.65f, 0, -3};
    const auto target = w.spawn_with(ecs::WorldTransform{left},
                                     MeshRef{mesh},
                                     MaterialRef{material},
                                     fluids_render::ThermalSurface{true});
    core::Transform right;
    right.translation = {.65f, 0, -3};
    (void)w.spawn_with(ecs::WorldTransform{right}, MeshRef{mesh}, MaterialRef{material});
    fluids_scene::ThermalCell cell;
    cell.enabled = true;
    w.add_component<fluids_scene::ThermalCell>(target, cell);
    (void)w.spawn_with(fluids_scene::ThermalSource{target, -10000, true});
    fluids_scene::ThermalPreview thermal;
    thermal.reconcile(w);
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_temporal_jitter_enabled(false);
    renderer.set_taa_resolve_enabled(false);
    renderer.set_ambient(.5f, .5f, .5f);
    constexpr unsigned size = 128;
    const auto frame = [&] {
        RenderGraph graph(*device);
        const auto out = renderer.render(graph, w, {size, size}, true);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.hdr), size, size, 8);
    };
    const auto baseline = frame();
    auto surfaces = fluids_render::phase_surfaces(w, thermal);
    REQUIRE(surfaces.count == 1);
    REQUIRE(renderer.set_surface_appearances(surfaces.span()));
    surfaces.rows[0].base_color = {
        1, 0, 0}; // caller mutation must not change the renderer's owned copy
    const auto water = frame();
    for (std::uint64_t t = 1; t <= 2100; ++t)
        REQUIRE(thermal.tick(w, t, 1. / 60.));
    REQUIRE(thermal.field()->cells()[0].state().liquid_mass_fraction == 0);
    surfaces = fluids_render::phase_surfaces(w, thermal);
    REQUIRE(renderer.set_surface_appearances(surfaces.span()));
    const auto ice = frame();
    const auto wi = decode_hdr(water, size, size), ii = decode_hdr(ice, size, size);
    CHECK(ii.luminance(33, 64) > wi.luminance(33, 64) * 2);
    const auto pixel = (64 * size + 33) * 3;
    CHECK(wi.rgb[pixel + 2] > wi.rgb[pixel] * 5); // a borrowed red caller edit must fail
    CHECK(ice != water);
    CHECK(water != baseline);
    for (unsigned y = 56; y < 72; ++y)
        for (unsigned x = 88; x < 104; ++x)
            for (unsigned b = 0; b < 8; ++b)
                CHECK(ice[(y * size + x) * 8 + b] == water[(y * size + x) * 8 + b]);
    CHECK(materials.get(material).base_color[0] == .8f);
    CHECK(materials.get(material).roughness == .7f);
    REQUIRE(renderer.set_surface_appearances({}));
    CHECK(frame() == baseline);
    auto stale = surfaces.rows[0];
    ++stale.entity.generation;
    REQUIRE(renderer.set_surface_appearances({&stale, 1}));
    CHECK(frame() == baseline);
    const std::array duplicate{surfaces.rows[0], surfaces.rows[0]};
    REQUIRE(renderer.set_surface_appearances(surfaces.span()));
    REQUIRE(frame() != baseline);
    CHECK_FALSE(renderer.set_surface_appearances(duplicate));
    CHECK(frame() == baseline);
    std::array<SurfaceAppearance, 17> over{};
    REQUIRE(renderer.set_surface_appearances(surfaces.span()));
    CHECK_FALSE(renderer.set_surface_appearances(over));
    CHECK(frame() == baseline);
    auto bad = surfaces.rows[0];
    bad.base_color.x = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(renderer.set_surface_appearances(surfaces.span()));
    CHECK_FALSE(renderer.set_surface_appearances({&bad, 1}));
    CHECK(frame() == baseline);
    CHECK(renderer.surface_appearance_refusals() == 3);
    std::printf("phase pixel proof: ice %.6f vs liquid %.6f radiance; neighbor, Stop/empty and "
                "stale generation exact\n",
                ii.luminance(33, 64),
                wi.luminance(33, 64));
}
