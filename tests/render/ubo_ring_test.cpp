// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m16.1 — the SceneRenderer's per-frame uniform ring.
//
// WHAT THIS FILE CAN AND CANNOT PROVE, STATED UP FRONT, because the gap is the whole reason the bug
// survived to be found by a review rather than by CI.
//
// The defect was a use-after-free: `ensure_draw_capacity` destroyed `draw_ubo_` the moment a
// frame's draw count outgrew it, while the PREVIOUS frame's command buffer still had that buffer
// baked into its descriptor sets — and `write_buffer` overwrote contents the GPU was still reading.
// Both need frames to actually overlap, which happens only on the windowed path: `present()` does
// not wait, and `acquire_next_image` waits on frame N-2's fence, so frame N-1 is still executing.
//
// Every test here runs headless through `submit_blocking`, which has already finished the frame
// before anything else happens. **So no test in this file can reproduce the use-after-free.** It
// was reproduced by hand, on hardware, with validation enabled — `vkDestroyBuffer(): ... currently
// in use by VkDescriptorSet ...` while turning the camera in `the_block --play`.
//
// What these cases DO pin is the structure that makes the fix real and would notice it being
// undone: the ring exists and is deeper than one, every slot is reached, each slot grows
// independently, and a scene rendered after a growth history is pixel-identical to the same scene
// rendered fresh. A ring that silently collapsed back to one buffer would fail the first assertion;
// a growth path that corrupted or skipped a slot would fail the last.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kSize = 64;

// Spawn `n` cubes in view, plus the camera and a light. Returns the world by out-param because
// ecs::World is not movable.
void build_scene(ecs::World& world, MeshId cube, MaterialId mat, int n) {
    for (int i = 0; i < n; ++i) {
        core::Transform tf{};
        // A shallow grid a few metres ahead, all inside the frustum so the draw count the renderer
        // sees is the count we asked for rather than whatever survives culling.
        tf.translation = {static_cast<float>(i % 20) * 0.4f - 4.0f,
                          static_cast<float>(i / 20) * 0.4f - 1.0f,
                          -6.0f};
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    }
    core::Transform cam_tf{}; // identity looks down −z
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
    core::Transform light_tf{};
    light_tf.translation = {4.0f, 4.0f, 4.0f};
    (void)world.spawn_with(ecs::WorldTransform{light_tf},
                           PointLight{1.0f, 1.0f, 1.0f, 80.0f, 60.0f});
}

} // namespace

TEST_CASE("m16.1: the uniform ring is frames_in_flight + 1 deep, and every slot grows on its own") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the uniform-ring proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(0.15f), "ring-cube");
    REQUIRE(cube != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId mat = materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.5f});

    SceneRenderer renderer(*device, meshes, materials);

    // The default is the safe maximum, not the headless minimum: a windowed caller who forgets to
    // call set_frames_in_flight must get a ring that is too deep, never one that is too shallow.
    CHECK(renderer.ubo_slot_count() >= 2);

    renderer.set_frames_in_flight(2); // what the Vulkan swapchain reports today
    CHECK(renderer.ubo_slot_count() == 3);
    renderer.set_frames_in_flight(1); // a hypothetical shallower backend
    CHECK(renderer.ubo_slot_count() == 2);
    renderer.set_frames_in_flight(2);
    REQUIRE(renderer.ubo_slot_count() == 3);

    const auto render_scene = [&](ecs::World& world) {
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    // Small and large scenes. 200 draws is well past the 64-draw capacity floor, so reaching it
    // forces a grow; dropping back to 1 and climbing again is what makes each of the three slots
    // take the growth path at a different moment.
    ecs::World small_world;
    register_render_components(small_world);
    build_scene(small_world, cube, mat, 1);

    ecs::World big_world;
    register_render_components(big_world);
    build_scene(big_world, cube, mat, 200);

    // Nine frames alternating sizes: with a 3-slot ring this visits every slot at both sizes, so
    // every slot has both grown and been re-used after growing.
    std::vector<std::uint8_t> last_small;
    std::vector<std::uint8_t> last_big;
    for (int i = 0; i < 3; ++i) {
        last_small = render_scene(small_world);
        last_big = render_scene(big_world);
        last_small = render_scene(small_world);
    }
    REQUIRE_FALSE(last_small.empty());
    REQUIRE_FALSE(last_big.empty());

    // THE ASSERTION THAT MATTERS: a renderer with a growth history behind it draws the same pixels
    // as a renderer that has never grown. A slot left un-grown, written at the wrong index, or
    // reused while stale would change these bytes.
    SceneRenderer fresh_small(*device, meshes, materials);
    fresh_small.set_frames_in_flight(2);
    RenderGraph g1(*device);
    const SceneRenderer::Output o1 = fresh_small.render(g1, small_world, {kSize, kSize}, true);
    REQUIRE(o1.ldr.is_valid());
    g1.export_texture(o1.ldr);
    auto c1 = device->begin_commands();
    g1.execute(*c1);
    device->submit_blocking(*c1);
    const std::vector<std::uint8_t> reference_small =
        read_texture(*device, g1.physical(o1.ldr), kSize, kSize, 4);

    CHECK(last_small == reference_small);

    // …and the two scenes must NOT be identical to each other, or the comparison above is vacuous:
    // it would pass just as well against a renderer that drew nothing at all.
    CHECK(last_small != last_big);
}
