// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3a — PresentPass, the copy that puts a finished frame on a swapchain image.
//
// It needs a device but NOT a window, which is the whole reason it is testable at all: CI has
// lavapipe and no display, so the pass that would otherwise only ever run on a developer's desktop
// gets a real pixel proof on every runner.
//
// THE CLAIM IS EXACTNESS, not similarity. The pass exists to move an image unchanged; a copy that
// flipped Y, sampled with a half-texel offset, swapped channels, or quietly re-encoded sRGB would
// all still produce a plausible picture. So the assertion is byte-identity against the source read
// back directly — and the source is a real rendered scene rather than a flat colour, because a
// constant image is identical to itself under every one of those bugs.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kSize = 96;

} // namespace

TEST_CASE("m13.3a: the present pass copies a frame EXACTLY") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the present proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(0.5f), "present-cube");
    REQUIRE(cube != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId mat = materials.add({{0.8f, 0.4f, 0.2f, 1.0f}, 0.0f, 0.6f});

    // A scene with structure in it: a few cubes at different depths under an off-axis light, so the
    // image varies in x, y AND channel. A flat colour would be identical to itself under a flipped
    // or offset copy, which is exactly what this pass must not do.
    ecs::World world;
    register_render_components(world);
    for (int i = -1; i <= 1; ++i) {
        core::Transform tf{};
        tf.translation = {static_cast<float>(i) * 1.3f, static_cast<float>(i) * 0.4f, -4.0f};
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    }
    (void)world.spawn_with(ecs::WorldTransform{core::Transform{}}, Camera{});
    core::Transform light_tf{};
    light_tf.translation = {3.0f, 2.5f, 1.0f};
    (void)world.spawn_with(ecs::WorldTransform{light_tf},
                           PointLight{1.0f, 0.95f, 0.85f, 60.0f, 40.0f});

    SceneRenderer renderer(*device, meshes, materials);

    // The target is created with the SAME format the pass is built for — standing in for the
    // swapchain image, which is the only thing this pass ever writes to in production.
    const PresentPass present(*device, kLdrFormat);
    REQUIRE(present.valid());

    RenderGraph graph(*device);
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
    REQUIRE(out.ldr.is_valid());

    RGTextureDesc target_desc{};
    target_desc.extent = {kSize, kSize};
    target_desc.format = kLdrFormat;
    target_desc.debug_name = "present-target";
    const RGTexture target = graph.create_texture(target_desc);
    present.add(graph, out.ldr, target);

    graph.export_texture(out.ldr);
    graph.export_texture(target);
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    const std::vector<std::uint8_t> source =
        read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    const std::vector<std::uint8_t> copied =
        read_texture(*device, graph.physical(target), kSize, kSize, 4);

    REQUIRE(source.size() == copied.size());

    // NON-VACUITY FIRST: the source has to actually contain something, or "the copy matches" is a
    // statement about two black images.
    std::size_t lit = 0;
    for (std::size_t i = 0; i + 3 < source.size(); i += 4) {
        if (source[i] > 8 || source[i + 1] > 8 || source[i + 2] > 8) {
            ++lit;
        }
    }
    REQUIRE(lit > kSize * kSize / 20); // at least 5% of the frame is not background

    std::size_t differing = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] != copied[i]) {
            ++differing;
        }
    }
    // Byte-identical. A tolerance here would accept precisely the bugs worth catching: a half-texel
    // offset, a Y flip on a near-symmetric image, or an sRGB round trip.
    CHECK(differing == 0);
}
