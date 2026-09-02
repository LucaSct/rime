// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m16 — the draw path: the per-frame uniform ring (m16.1) and per-submesh draws (m16.2).
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

TEST_CASE("m16.2: a mesh split into submeshes draws identically, and the split is real") {
    // The cook has emitted one submesh per glTF primitive since `Mesh::from_primitives` existed,
    // and the reader has always validated the table — but `mesh_from_cooked` dropped it, because
    // `CpuMesh` had no field it could live in. A multi-material object therefore rendered entirely
    // in one material, and "split by material" was something the author had to do at the
    // Blender-object level.
    //
    // The proof is the culling brick's own discipline turned on submeshes: SPLIT IS IDENTICAL TO
    // WHOLE when both halves name the same material. A tolerance would accept a split that dropped
    // or duplicated triangles, which is exactly the failure to rule out.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the submesh proof");
        return;
    }

    MaterialRegistry materials;
    const MaterialId mat = materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.5f});

    const CpuMesh cube = make_cube(0.5f);
    REQUIRE(cube.indices.size() % 6 == 0); // needs an even split into two whole-triangle halves
    const auto half = static_cast<std::uint32_t>(cube.indices.size() / 2);

    MeshRegistry meshes(*device);

    // The control: no table at all. `add` must synthesise exactly one whole-mesh range, so every
    // pre-m16.2 mesh keeps working with no special case anywhere in the draw path.
    const MeshId whole = meshes.add(cube, "whole");
    REQUIRE(whole != kInvalidMeshId);
    REQUIRE(meshes.get(whole).submeshes.size() == 1);
    CHECK(meshes.get(whole).submeshes[0].first_index == 0);
    CHECK(meshes.get(whole).submeshes[0].index_count == cube.indices.size());

    // The same geometry, declared as two ranges that tile the index buffer exactly.
    CpuMesh split_mesh = cube;
    split_mesh.submeshes = {{0, half, 0}, {half, half, 1}};
    const MeshId split = meshes.add(split_mesh, "split");
    REQUIRE(split != kInvalidMeshId);
    REQUIRE(meshes.get(split).submeshes.size() == 2);

    const auto frame = [&](MeshId id) {
        ecs::World world;
        register_render_components(world);
        core::Transform tf{};
        tf.translation = {0.0f, 0.0f, -3.0f};
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{id}, MaterialRef{mat});
        core::Transform cam_tf{};
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.translation = {2.0f, 3.0f, 2.0f};
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               PointLight{1.0f, 1.0f, 1.0f, 60.0f, 40.0f});

        SceneRenderer renderer(*device, meshes, materials);
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    // THE HEADLINE: byte-identical, not merely similar.
    CHECK(frame(whole) == frame(split));

    // …and the split actually happened. Without this the identity check passes perfectly against a
    // renderer that ignored the table and drew the whole mesh both times — which is precisely the
    // bug being fixed.
    ExtractedScene probe;
    probe.draws.push_back({split, mat, core::Mat4{}});
    probe.draw_entities.push_back(ecs::Entity{});
    const ResolveDrawStats stats = resolve_draws(probe, meshes);
    CHECK(stats.dropped == 0);
    CHECK(stats.expanded == 1); // one EXTRA draw beyond the first
    REQUIRE(probe.draws.size() == 2);
    CHECK(probe.draws[0].first_index == 0);
    CHECK(probe.draws[0].index_count == half);
    CHECK(probe.draws[1].first_index == half);
    // The entity is repeated once per submesh, or the pick pass maps a pixel to the wrong entity.
    CHECK(probe.draw_entities.size() == probe.draws.size());
    CHECK(probe.draw_entities[0] == probe.draw_entities[1]);

    // A range outside the index buffer is DROPPED AND COUNTED, never clamped: clamping would
    // silently draw the wrong triangles, which is the harder failure to notice.
    const std::size_t rejected_before = meshes.rejected_submeshes();
    CpuMesh bad = cube;
    bad.submeshes = {{0, half, 0}, {half, half * 4, 1}}; // second range runs off the end
    const MeshId partly_bad = meshes.add(bad, "partly-bad");
    REQUIRE(partly_bad != kInvalidMeshId);
    CHECK(meshes.rejected_submeshes() == rejected_before + 1);
    CHECK(meshes.get(partly_bad).submeshes.size() == 1); // only the good range survived
}
