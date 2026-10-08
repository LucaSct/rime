// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// ADR-0078 step 1e: temporal jitter, motion vectors and the TAA resolve are ON by default, and
// the sky composite and the SSR march invert the projection the frame was RASTERIZED with.
//
//   1. A default-constructed renderer really runs the velocity pass and the TAA resolve. The
//      flags being true is not evidence of that (the resolve skips itself and forgets its history
//      when there is no velocity target), so the proof reads the declared pass names and the
//      renderer's own "this frame was resolved" counter.
//   2. The sky is rasterized through the same jittered matrix as the geometry. Probe: a sky with a
//      hard-edged sun disc (no geometry). Jitter moves everything the depth buffer was written
//      with by `offset` pixels, so the disc's centroid must move by `offset` too. If the sky
//      composite inverted the UNJITTERED projection, the disc would not move at all.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/sky.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr std::uint32_t kSize = 128;
constexpr float kHalfPi = 1.5707963f;
constexpr float kFovY = 1.1f;

[[nodiscard]] core::Quat pitched(float radians) {
    return core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, radians);
}

[[nodiscard]] bool has_pass(const RenderGraph& graph, const std::string& name) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(graph.pass_count()); ++i) {
        if (graph.pass_name(i) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("default-on: a default renderer declares the velocity pass and the TAA resolve") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping the default-on proofs");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId cube = meshes.add(make_cube(0.5f), "default-on-cube");
    const MaterialId mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f});
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform tf{};
    tf.translation = {0.0f, 0.0f, -6.0f};
    (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});

    SceneRenderer renderer(*device, meshes, materials); // nothing switched on by hand
    REQUIRE(renderer.temporal_jitter_enabled());
    REQUIRE(renderer.motion_vectors_enabled());
    REQUIRE(renderer.taa_resolve_enabled());

    for (int frame = 0; frame < 3; ++frame) {
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        // The flags say what was ASKED; the pass names and the counter say what RAN.
        CHECK(has_pass(graph, "velocity"));
        CHECK(has_pass(graph, "taa-resolve"));
        CHECK(renderer.last_frame_resolved());
        CHECK(renderer.taa_history_allocated());
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
    }
}

namespace {

// The centroid, in pixels, of the sun disc: luminance above the sky behind it, weighted. Relative
// to the same measurement on a jitter-off frame it is a sub-pixel position of the disc's edge.
struct Centroid {
    double x = 0.0;
    double y = 0.0;
    double mass = 0.0;
};

[[nodiscard]] Centroid disc_centroid(const HdrImage& img) {
    const float bg = img.luminance(8, 8); // a corner: atmosphere only
    Centroid c;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const double w = std::max(0.0f, img.luminance(x, y) - bg * 1.5f);
            c.x += w * (x + 0.5);
            c.y += w * (y + 0.5);
            c.mass += w;
        }
    }
    if (c.mass > 0.0) {
        c.x /= c.mass;
        c.y /= c.mass;
    }
    return c;
}

// One sky-only frame, HDR decoded. The sun is straight up and so is the camera.
[[nodiscard]] HdrImage
sky_frame(rhi::Device& device, SceneRenderer& renderer, core::Vec2* offset_out) {
    ecs::World world;
    register_render_components(world);
    core::Transform sun{};
    sun.rotation = pitched(-kHalfPi);
    (void)world.spawn_with(ecs::WorldTransform{sun}, DirectionalLight{1.0f, 1.0f, 1.0f, 1.0f});
    core::Transform cam{};
    cam.rotation = pitched(kHalfPi);
    (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{kFovY, 0.1f, 100.0f, true});
    RenderGraph graph(device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    if (offset_out != nullptr) {
        *offset_out = renderer.last_jitter_offset();
    }
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

} // namespace

TEST_CASE(
    "default-on: the sky is composited through the projection the frame was rasterized with") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping the sky-matrix proof");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;

    SkyParams sky{};
    sky.enabled = true;
    sky.clouds_enabled = false;
    sky.angular_radius = 0.06f; // ~7 px across at this resolution: a hard edge with real extent

    // The reference: the same scene with jitter OFF, so the disc sits where the unjittered camera
    // puts it. The resolve and the velocity buffer are off in both: this reads the raw sky frame.
    SceneRenderer plain(*device, meshes, materials);
    render::test::disable_temporal_aa(plain);
    plain.set_sky(sky);
    const Centroid base = disc_centroid(sky_frame(*device, plain, nullptr));
    REQUIRE(base.mass > 1.0); // the disc exists and was found

    SceneRenderer jittered(*device, meshes, materials);
    render::test::disable_temporal_aa(jittered);
    jittered.set_temporal_jitter_enabled(true);
    jittered.set_sky(sky);

    // For each of the sequence's 8 offsets: the disc must have moved by that offset (the geometry
    // moves by exactly that much, so the sky must too). `err` is how far it missed; `moved` is how
    // far the offsets were in the first place, so err / moved reads 0 for a sky that follows the
    // geometry and 1 for one that ignores the jitter.
    double err = 0.0;
    double moved = 0.0;
    for (std::uint32_t i = 0; i < TemporalJitter::kPeriod; ++i) {
        core::Vec2 off{};
        const Centroid c = disc_centroid(sky_frame(*device, jittered, &off));
        REQUIRE(c.mass > 1.0);
        const double dx = c.x - base.x;
        const double dy = c.y - base.y;
        err += std::abs(dx - off.x) + std::abs(dy - off.y);
        moved += std::abs(off.x) + std::abs(off.y);
        std::printf("  frame %u: offset (%+.3f, %+.3f) px, disc moved (%+.3f, %+.3f) px\n",
                    i,
                    off.x,
                    off.y,
                    dx,
                    dy);
    }
    const double ratio = err / moved;
    std::printf("  sky-follows-jitter miss ratio = %.4f (0 = follows the geometry, 1 = ignores)\n",
                ratio);
    CHECK(moved > 1.0); // the sequence did jitter: the ratio is not 0/0 in disguise
    CHECK(ratio < 0.25);
}
