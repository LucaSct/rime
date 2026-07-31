// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m11.6b: extraction draws an entity at its RenderTransform when it has one, and at its
// WorldTransform when it does not.
//
// This is the seam that lets network interpolation reach the renderer without the two modules ever
// meeting: `rime::render` links rhi/ecs/assets and `rime::replication` links ecs/net, so neither
// can call the other, and the shared `ecs` component is the whole conversation. The test lives on
// the render side and mentions no netcode for exactly that reason — the renderer's contract is
// "draw the presentation pose if one was deposited", not "know who deposited it".
//
// GPU-free: extract_scene is a plain function over a World, which is what makes the conventions
// pinnable without a device (the M5.6 pattern).

#include <doctest/doctest.h>

#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;

namespace {

[[nodiscard]] core::Transform at_x(float x) {
    core::Transform t{};
    t.translation.x = x;
    return t;
}

} // namespace

TEST_CASE("extraction prefers the presentation pose, and falls back when there is none (m11.6b)") {
    ecs::World world;
    register_render_components(world);
    (void)world.register_component<ecs::WorldTransform>();
    (void)world.register_component<ecs::RenderTransform>();

    MeshRef mesh{};
    mesh.mesh = MeshId{1};
    MaterialRef material{};
    material.material = MaterialId{1};

    // Simulated at x=10, presented at x=5 — a mirror halfway through a blend. The two are far
    // apart on purpose: a test whose poses differed by a rounding error would pass whichever one
    // extraction actually read.
    const ecs::Entity blending = world.spawn_with(
        ecs::WorldTransform{at_x(10.0f)}, ecs::RenderTransform{at_x(5.0f)}, mesh, material);
    // No RenderTransform at all — the shape of every entity in every scene that does not replicate.
    const ecs::Entity plain = world.spawn_with(ecs::WorldTransform{at_x(-3.0f)}, mesh, material);

    const ExtractedScene scene = extract_scene(world);
    REQUIRE(scene.draws.size() == 2);
    REQUIRE(scene.draw_entities.size() == 2);

    // Query order is archetype order, so find each draw by its entity rather than assuming a slot.
    const auto x_of = [&](ecs::Entity e) {
        for (std::size_t i = 0; i < scene.draw_entities.size(); ++i) {
            if (scene.draw_entities[i] == e) {
                return scene.draws[i].model.m[12]; // translation.x of a column-major T*R*S
            }
        }
        return 0.0f;
    };

    CHECK(x_of(blending) == doctest::Approx(5.0f)); // the override won
    CHECK(x_of(plain) == doctest::Approx(-3.0f));   // and cost the entity without one nothing
}

TEST_CASE("a camera and a light also honour the presentation pose (m11.6b)") {
    ecs::World world;
    register_render_components(world);
    (void)world.register_component<ecs::WorldTransform>();
    (void)world.register_component<ecs::RenderTransform>();

    // Unreachable over the wire today — no player controller exists yet (M12), and a replicated
    // viewpoint is what would produce one. Extraction treats every WorldTransform reader the same
    // way regardless, because the alternative is a renderer where meshes interpolate and the light
    // rig they are lit by does not, which is a bug that only shows up as a look.
    Camera cam{};
    cam.active = true;
    (void)world.spawn_with(ecs::WorldTransform{at_x(0.0f)}, ecs::RenderTransform{at_x(8.0f)}, cam);

    PointLight light{};
    light.intensity = 1.0f;
    light.color_r = 1.0f;
    light.radius = 5.0f;
    (void)world.spawn_with(
        ecs::WorldTransform{at_x(0.0f)}, ecs::RenderTransform{at_x(-6.0f)}, light);

    const ExtractedScene scene = extract_scene(world);
    REQUIRE(scene.camera.found);
    CHECK(scene.camera.position[0] == doctest::Approx(8.0f));
    REQUIRE(scene.point_lights.size() == 1);
    CHECK(scene.point_lights[0].position[0] == doctest::Approx(-6.0f));
}
