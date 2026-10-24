// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "scene.hpp"

#include <cmath>
#include <vector>

#include "rime/scene/scene_format.hpp"

namespace courtyard {
using namespace rime;

void register_components(ecs::World& world) {
    (void)worldkit::register_engine_components(world);
    (void)world.register_component<Part>();
    (void)world.register_component<Station>();
}

void build_scene(ecs::World& world) {
    register_components(world);
    const auto box =
        [&](core::Vec3 position, core::Vec3 scale, unsigned palette, unsigned shape = 0) {
            core::Transform t;
            t.translation = position;
            t.scale = scale;
            return world.spawn_with(ecs::LocalTransform{t}, Part{shape, palette});
        };
    // A deliberately compact service courtyard, with separate exhibits and a saved overview.
    (void)box({0, -.24f, -1}, {25, .4f, 22}, 0);
    (void)box({0, -.015f, 0}, {15, .12f, 9}, 1);
    for (int i = -6; i <= 6; ++i)
        (void)box({float(i), .05f, 0}, {.018f, .012f, 9}, 2);
    for (int i = -4; i <= 4; ++i)
        (void)box({0, .05f, float(i)}, {15, .012f, .018f}, 2);
    // The skyline stays behind the samples; dark glazing and warm lit windows give scale.
    for (int b = 0; b < 6; ++b) {
        const float x = -10.0f + 4.0f * b, h = 4.5f + float(b % 3) * 1.6f;
        (void)box({x, h * .5f, -7}, {3.6f, h, 2.6f}, b % 2 ? 3 : 0);
        (void)box({x, h + .12f, -7}, {3.85f, .24f, 2.85f}, 2);
        for (int floor = 0; floor < 3 + b % 3; ++floor)
            for (int col = 0; col < 3; ++col) {
                (void)box({x - .95f + float(col) * .95f, 1.0f + float(floor) * 1.25f, -5.68f},
                          {.58f, .72f, .035f},
                          (b + floor + col) % 4 == 0 ? 7 : 8);
            }
    }
    for (unsigned k = 0; k < 3; ++k) {
        const float x = -3.8f + float(k) * 3.8f;
        (void)box({x, .20f, 0}, {3.0f, .3f, 2.55f}, 2);
        (void)box({x, .43f, 0}, {2.6f, .22f, 2.15f}, 0);
        for (float side : {-1.0f, 1.0f}) {
            (void)box({x + side * 1.18f, .62f, 0}, {.12f, .35f, 2.05f}, 3);
            (void)box({x, .62f, side * .96f}, {2.45f, .35f, .12f}, 3);
        }
        const auto surface = box({x, .68f, 0}, {2.22f, .055f, 1.8f}, k == 1 ? 5 : 4);
        fluids_scene::ThermalCell c;
        c.enabled = true;
        c.mass_kg = 10;
        c.liquid_mass_fraction = k == 1 ? 0 : 1;
        world.add_component<fluids_scene::ThermalCell>(surface, c);
        world.add_component<fluids_render::ThermalSurface>(surface, {true});
        world.add_component<Station>(surface, {k});
        // Signed powers are explicitly authored, independent from cosmetic fire/light.
        if (k < 2)
            (void)world.spawn_with(
                fluids_scene::ThermalSource{surface, k == 0 ? -100000.0 : 100000.0, true});
        (void)box({x, .63f, 1.25f}, {1.3f, .35f, .12f}, k == 0 ? 6 : (k == 1 ? 7 : 2));
        // One / two / three physical pips identify station order even without colour perception.
        for (unsigned pip = 0; pip <= k; ++pip)
            (void)box({x - .3f + float(pip) * .3f, .68f, 1.33f}, {.12f, .12f, .12f}, 5, 1);
    }
    // Heater brazier and chimney smoke are opt-in existing VFX sources, spatially separated.
    (void)box({0, .35f, -2.8f}, {1.0f, .6f, 1.0f}, 2);
    (void)box({0, .73f, -2.8f}, {.68f, .22f, .68f}, 7, 1);
    core::Transform fire;
    fire.translation = {0, .83f, -2.8f};
    (void)world.spawn_with(ecs::LocalTransform{fire},
                           vfx_scene::FireEmitter{.18f, .1f, .18f, 14, 1, true});
    (void)box({-6, 1.35f, -3.7f}, {.75f, 2.65f, .75f}, 2);
    (void)box({-6, 2.65f, -3.7f}, {1.05f, .2f, 1.05f}, 3);
    core::Transform smoke;
    smoke.translation = {-6, 2.86f, -3.7f};
    (void)world.spawn_with(ecs::LocalTransform{smoke},
                           vfx_scene::SmokeEmitter{.14f, .1f, .14f, 9, .75f, true});
    // Low street lamps frame the thermal experiment without hiding its phase palette.
    for (float x : {-7.0f, 7.0f}) {
        (void)box({x, 1.5f, 1.8f}, {.08f, 3, .08f}, 2);
        (void)box({x, 3.05f, 1.8f}, {.45f, .15f, .45f}, 7);
        core::Transform light;
        light.translation = {x, 2.9f, 1.8f};
        (void)world.spawn_with(ecs::LocalTransform{light},
                               render::PointLight{1, .74f, .43f, 38, 8});
    }
    core::Transform camera;
    camera.translation = {9, 8.5f, 14};
    camera.rotation =
        core::quat_from_axis_angle({0, 1, 0}, .53f) * core::quat_from_axis_angle({1, 0, 0}, -.40f);
    (void)world.spawn_with(ecs::LocalTransform{camera}, render::Camera{.82f, .1f, 150, true});
    core::Transform sun;
    sun.rotation =
        core::quat_from_axis_angle({0, 1, 0}, -.55f) * core::quat_from_axis_angle({1, 0, 0}, -.9f);
    (void)world.spawn_with(ecs::LocalTransform{sun}, render::DirectionalLight{1, .90f, .78f, 2.5f});
    // A dark backdrop closes this small city diorama without depending on sky/atmosphere
    // quality or pretending it is an open-world city. The demo is intentionally self-contained.
    (void)box({0, 12, -12}, {400, 200, .3f}, 8);
}

void prepare(ecs::World& world, render::MeshRegistry& meshes, render::MaterialRegistry& materials) {
    const std::array mesh{meshes.add(render::make_cube(.5f), "courtyard-box"),
                          meshes.add(render::make_uv_sphere(.5f, 16, 24), "courtyard-glow")};
    constexpr std::array<core::Vec3, 9> colors{{{.32f, .29f, .26f},
                                                {.45f, .43f, .39f},
                                                {.055f, .075f, .095f},
                                                {.18f, .24f, .28f},
                                                {.015f, .14f, .32f},
                                                {.70f, .88f, .95f},
                                                {.04f, .45f, .68f},
                                                {1, .28f, .035f},
                                                {.05f, .095f, .14f}}};
    std::array<render::MaterialId, 9> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        render::PbrMaterialDesc m;
        m.base_color[0] = colors[i].x;
        m.base_color[1] = colors[i].y;
        m.base_color[2] = colors[i].z;
        m.roughness = i == 4 ? .12f : (i == 8 ? .25f : .75f);
        m.metallic = i == 2 ? .55f : 0;
        if (i == 7) {
            m.emissive[0] = 1.8f;
            m.emissive[1] = .43f;
            m.emissive[2] = .065f;
        }
        ids[i] = materials.add(m);
    }
    std::vector<std::pair<ecs::Entity, Part>> parts;
    world.query<Part>().for_each([&](ecs::Entity e, const Part& p) { parts.emplace_back(e, p); });
    // Structural edits relocate archetype rows, so collect identities before attaching draws.
    for (const auto& [e, p] : parts) {
        if (p.shape >= mesh.size() || p.palette >= ids.size())
            continue;
        // Part is authored; registry indices are rebuilt for each viewport session.
        world.add_component<ecs::DerivedComponents>(e, {});
        world.add_component<render::MeshRef>(e, {mesh[p.shape]});
        world.add_component<render::MaterialRef>(e, {ids[p.palette]});
    }
}
} // namespace courtyard
