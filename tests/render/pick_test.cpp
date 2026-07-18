// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the ID-buffer picker (M9.6). Structural, never golden (the M5.6 rule): the scene is
// planted so each assertion is a geometric certainty with pixels of margin, not a driver-exact
// image. Two layers:
//
//   (1) extraction's draw↔entity mapping (GPU-free): draw_entities is parallel to draws and obeys
//       the same filtering — the array the picker's id→entity resolve leans on.
//   (2) on lavapipe: a planted two-cube occlusion scene picked at KNOWN pixels —
//         a. each lone object's projected center picks its exact entity (index+generation);
//         b. where a near cube overlaps a far one, the NEAR one wins (the depth test at work);
//         c. beside the near cube but still on the far one, the far one is picked (the same two
//            draws, a different pixel — proves per-pixel resolution, not per-draw luck);
//         d. empty space and out-of-bounds pixels answer kNullEntity (the miss sentinel), and a
//            camera-less world misses without touching the GPU.
//
// Pixel math for the margins (camera at (0,0,8) looking down −z, fov_y 0.87266, 256²):
// a face at view depth d, half-extent h, projects to a half-width of (h/d)/tan(fov/2)·128 px.
//   near cube: front face z=3.4 → d=4.6, h=0.4 → ±23.9 px around center.
//   far cube:  front face z=1.0 → d=7.0, h=1.0 → ±39.2 px around center.
// The "beside near, on far" pixel sits 31 px below center: 7.1 px outside the near cube's face,
// 8.2 px inside the far one's — margins that shrug off rasterization edge rules.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_picker.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {

using namespace rime;
using namespace rime::render;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// Drive the asynchronous picker to completion — poll like the editor host's frame loop does, with
// a generous deadline so a wedged submission fails loudly instead of hanging CI.
ecs::Entity resolve(ScenePicker& picker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto r = picker.try_resolve()) {
            return *r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FAIL("pick never resolved");
    return ecs::kNullEntity;
}

// Project a world point to pixel coordinates with the renderer's own math (perspective already
// bakes Vulkan's y-down NDC — no flips), so "the projected center of X" is computed, not guessed.
struct PixelI {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

PixelI project_px(const core::Mat4& view_proj, core::Vec3 world, std::uint32_t size) {
    const core::Vec4 clip = view_proj * core::Vec4{world.x, world.y, world.z, 1.0f};
    return {static_cast<std::int32_t>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(size)),
            static_cast<std::int32_t>((clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(size))};
}

} // namespace

TEST_CASE("pick: extraction maps each draw to its source entity (M9.6)") {
    using ecs::WorldTransform;
    ecs::World world;
    register_render_components(world);

    const ecs::Entity a = world.spawn_with(WorldTransform{}, MeshRef{0}, MaterialRef{0});
    // Filtered: an invalid mesh id draws nothing, so it must not appear in EITHER array — a skew
    // here would map every later pick to the wrong entity.
    (void)world.spawn_with(WorldTransform{}, MeshRef{kInvalidMeshId}, MaterialRef{0});
    const ecs::Entity b = world.spawn_with(WorldTransform{}, MeshRef{1}, MaterialRef{2});

    const ExtractedScene scene = extract_scene(world);
    REQUIRE(scene.draws.size() == 2);
    REQUIRE(scene.draw_entities.size() == 2); // parallel by construction
    CHECK(scene.draw_entities[0] == a);
    CHECK(scene.draw_entities[1] == b);
    CHECK(scene.draws[0].mesh == 0);
    CHECK(scene.draws[1].mesh == 1);
}

TEST_CASE("pick: known pixels answer known entities, nearer wins, empty misses (M9.6)") {
    using ecs::WorldTransform;

    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping pick render proofs");
        return;
    }
    constexpr std::uint32_t kSize = 256;
    constexpr rhi::Extent2D kExtent{kSize, kSize};

    MeshRegistry meshes(*device);
    const MeshId big = meshes.add(make_cube(1.0f), "pick-far-cube");
    const MeshId small = meshes.add(make_cube(0.4f), "pick-near-cube");
    const MeshId lone_mesh = meshes.add(make_cube(0.5f), "pick-lone-cube");
    REQUIRE(big != kInvalidMeshId);

    MaterialRegistry materials;
    const MaterialId mat = materials.add({});

    // The planted scene (all WorldTransform-direct, like the viewport host's): a big cube at the
    // origin, a small cube in front of it on the same camera axis, and a lone cube off to the
    // left. Camera at (0,0,8), identity rotation = looking straight down −z.
    ecs::World world;
    register_render_components(world);
    const auto at = [](float x, float y, float z) {
        core::Transform tf{};
        tf.translation = {x, y, z};
        return WorldTransform{tf};
    };
    const ecs::Entity far_cube = world.spawn_with(at(0, 0, 0), MeshRef{big}, MaterialRef{mat});
    const ecs::Entity near_cube = world.spawn_with(at(0, 0, 3), MeshRef{small}, MaterialRef{mat});
    const ecs::Entity lone = world.spawn_with(at(-3, 0, 0), MeshRef{lone_mesh}, MaterialRef{mat});
    (void)world.spawn_with(at(0, 0, 8), Camera{});

    // The same clip-from-world the picker builds, for computing target pixels.
    const core::Mat4 view_proj = core::perspective(0.87266f, 1.0f, 0.1f, 1000.0f) *
                                 core::inverse(core::to_matrix(
                                     core::Transform{{0, 0, 8}, core::quat_identity(), {1, 1, 1}}));

    ScenePicker picker(*device, meshes);

    // (a) The lone cube: pick the projected center of its FRONT face (z = +0.5) — pixel computed,
    // not guessed, so the assertion is exact entity identity (index AND generation).
    const PixelI lp = project_px(view_proj, {-3.0f, 0.0f, 0.5f}, kSize);
    picker.begin_pick(world, kExtent, lp.x, lp.y);
    CHECK(resolve(picker) == lone);

    // (b) Occlusion: dead centre, BOTH cubes cover the pixel; the near one must win by depth.
    picker.begin_pick(world, kExtent, kSize / 2, kSize / 2);
    CHECK(resolve(picker) == near_cube);

    // (c) 31 px below centre: outside the near cube's ±23.9 px face, inside the far one's
    // ±39.2 px — same draws, different pixel, the far entity answers.
    picker.begin_pick(world, kExtent, kSize / 2, kSize / 2 + 31);
    CHECK(resolve(picker) == far_cube);

    // (d) Misses. A corner pixel (far outside every projection) is empty space…
    picker.begin_pick(world, kExtent, 5, 5);
    CHECK(resolve(picker) == ecs::kNullEntity);
    // …and out-of-bounds pixels answer without any GPU work (resolve on the first poll).
    picker.begin_pick(world, kExtent, -3, 10);
    CHECK(picker.try_resolve().value_or(ecs::Entity{1, 1}) == ecs::kNullEntity);
    picker.begin_pick(world, kExtent, 10, static_cast<std::int32_t>(kSize));
    CHECK(picker.try_resolve().value_or(ecs::Entity{1, 1}) == ecs::kNullEntity);

    // A camera-less world has no view to pick through — an immediate, honest miss.
    ecs::World no_camera;
    register_render_components(no_camera);
    (void)no_camera.spawn_with(at(0, 0, 0), MeshRef{big}, MaterialRef{mat});
    picker.begin_pick(no_camera, kExtent, kSize / 2, kSize / 2);
    CHECK(picker.try_resolve().value_or(ecs::Entity{1, 1}) == ecs::kNullEntity);

    // And a live pick after all of the above still answers (the picker's reuse path: the private
    // graph reset + re-imported targets are exercised by every case in this test).
    picker.begin_pick(world, kExtent, kSize / 2, kSize / 2);
    CHECK(resolve(picker) == near_cube);
}
