// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "../render/render_test_support.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/render/terrain_scene.hpp"

namespace {
using namespace rime;
using namespace rime::render;
constexpr std::uint32_t kSize = 64;

assets::HeightfieldAsset flat_tile() {
    assets::HeightfieldAsset a{};
    a.columns = 9;
    a.rows = 9;
    a.cell_size_x = 1.0f;
    a.cell_size_z = 1.0f;
    a.origin = {-4.0f, 0.0f, -4.0f};
    a.height_scale = 1.0f / 65535.0f;
    a.samples.assign(81, 0);
    return a;
}

float f32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

float f16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint16_t bits = 0;
    std::memcpy(&bits, bytes.data() + offset, sizeof(bits));
    return test::half_to_float(bits);
}

std::size_t pixel(std::uint32_t x = 32, std::uint32_t y = 32) {
    return static_cast<std::size_t>(y) * kSize + x;
}

struct Probe {
    float depth = 1.0f;
    float hdr = 0.0f;
    std::array<float, 4> gbuffer{};
    std::array<float, 4> material{};
    std::array<float, 2> velocity{};
};

Probe draw_scene_tile(rhi::Device& device,
                      TerrainPass& pass,
                      TerrainTileId tile,
                      bool gbuffer,
                      bool velocity,
                      float camera_x,
                      float previous_x) {
    RenderGraph graph(device);
    graph.reset();
    const auto hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "terrain-hdr"});
    const auto depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "terrain-depth"});
    const auto gb = gbuffer
                        ? graph.create_texture({{kSize, kSize}, kGbufferFormat, "terrain-gbuffer"})
                        : RGTexture{};
    const auto gm =
        gbuffer ? graph.create_texture({{kSize, kSize}, kGbufferMaterialFormat, "terrain-material"})
                : RGTexture{};
    const auto vel =
        velocity ? graph.create_texture({{kSize, kSize}, kVelocityFormat, "terrain-velocity"})
                 : RGTexture{};
    RGColorAttachment colors[4] = {{hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}}};
    std::size_t count = 1;
    if (gbuffer) {
        colors[count++] = {gb, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}};
        colors[count++] = {gm, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}};
    }
    if (velocity)
        colors[count++] = {vel, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}};
    const RGDepthAttachment dclear{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    RenderGraph::RasterPassDesc clear{};
    clear.colors = std::span<const RGColorAttachment>(colors, count);
    clear.depth = &dclear;
    graph.add_raster_pass("clear", clear, [](rhi::CommandBuffer&) {});

    const auto vp = [](float x) {
        return core::ortho(-4.0f, 4.0f, -4.0f, 4.0f, 0.0f, 20.0f) *
               core::look_at(core::Vec3{x, 8.0f, 0.0f},
                             core::Vec3{x, 0.0f, 0.0f},
                             core::Vec3{0.0f, 0.0f, -1.0f});
    };
    const TerrainSceneUniforms matrices{vp(camera_x), vp(previous_x)};
    const auto slice = graph.push_frame_data(&matrices, sizeof(matrices));
    TerrainLight light{};
    light.sun_irradiance = 0.0f;
    light.ambient = 0.5f;
    pass.add_scene(graph,
                   hdr,
                   depth,
                   gb,
                   gm,
                   vel,
                   tile,
                   vp(camera_x),
                   {camera_x, 8.0f, 0.0f},
                   light,
                   {},
                   {},
                   slice);
    CHECK(graph.pass_name(graph.pass_count() - 1) == "terrain-scene");
    graph.export_texture(hdr);
    graph.export_texture(depth);
    if (gbuffer) {
        graph.export_texture(gb);
        graph.export_texture(gm);
    }
    if (velocity)
        graph.export_texture(vel);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    Probe result{};
    result.depth =
        f32(test::read_texture(device, graph.physical(depth), kSize, kSize, 4), pixel() * 4);
    result.hdr = f16(test::read_texture(device, graph.physical(hdr), kSize, kSize, 8), pixel() * 8);
    if (gbuffer) {
        const auto bytes = test::read_texture(device, graph.physical(gb), kSize, kSize, 8);
        const auto material = test::read_texture(device, graph.physical(gm), kSize, kSize, 8);
        for (std::size_t i = 0; i < 4; ++i) {
            result.gbuffer[i] = f16(bytes, pixel() * 8 + i * 2);
            result.material[i] = f16(material, pixel() * 8 + i * 2);
        }
    }
    if (velocity) {
        const auto bytes = test::read_texture(device, graph.physical(vel), kSize, kSize, 4);
        for (std::size_t i = 0; i < 2; ++i)
            result.velocity[i] = f16(bytes, pixel() * 4 + i * 2);
    }
    return result;
}
} // namespace

TEST_CASE("terrain scene variants write depth, material and unjittered camera velocity") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan device required");
        return;
    }
    TerrainPass pass(*device);
    const auto tile = pass.upload(flat_tile());
    REQUIRE(tile != kInvalidTerrainTile);
    for (bool gbuffer : {false, true}) {
        for (bool velocity : {false, true}) {
            const Probe still = draw_scene_tile(*device, pass, tile, gbuffer, velocity, 0.0f, 0.0f);
            CHECK(still.depth < 0.9f);
            CHECK(still.hdr > 0.01f);
            if (gbuffer) {
                CHECK(still.gbuffer[3] == doctest::Approx(1.0f));
                CHECK(still.gbuffer[2] == doctest::Approx(1.0f));
                CHECK(still.material[0] > 0.1f);
                CHECK(still.material[3] == doctest::Approx(0.0f));
            }
            if (velocity) {
                CHECK(still.velocity[0] == 0.0f);
                CHECK(still.velocity[1] == 0.0f);
                const Probe moved =
                    draw_scene_tile(*device, pass, tile, gbuffer, velocity, 0.5f, 0.0f);
                CHECK(moved.depth < 0.9f);
                CHECK(moved.velocity[0] < -0.10f);
                CHECK(std::abs(moved.velocity[1]) < 0.01f);
                // Regression control: deliberately omit the previous camera change. Zero then
                // demonstrates this probe distinguishes correct reprojection from zero velocity.
                const Probe omitted =
                    draw_scene_tile(*device, pass, tile, gbuffer, velocity, 0.5f, 0.5f);
                CHECK(omitted.velocity[0] == 0.0f);
            }
        }
    }
}

TEST_CASE("terrain scene is declared between opaque and sky/TAA, with an empty default") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan device required");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    SceneRenderer renderer(*device, meshes, materials);
    SkyParams sky{};
    sky.enabled = true;
    renderer.set_sky(sky);
    RenderGraph empty(*device);
    empty.reset();
    (void)renderer.render(empty, world, {kSize, kSize});
    const auto default_count = empty.pass_count();
    TerrainPass pass(*device);
    const auto tile = pass.upload(flat_tile());
    REQUIRE(tile != kInvalidTerrainTile);
    const std::array<TerrainSceneDraw, 1> draws{{{tile, {}}}};
    renderer.set_terrain_draws(&pass, draws);
    RenderGraph graph(*device);
    graph.reset();
    (void)renderer.render(graph, world, {kSize, kSize});
    CHECK(graph.pass_count() == default_count + 1);
    std::size_t terrain_index = graph.pass_count();
    std::size_t sky_index = graph.pass_count();
    std::size_t taa_index = graph.pass_count();
    for (std::size_t i = 0; i < graph.pass_count(); ++i) {
        if (graph.pass_name(i) == "terrain-scene")
            terrain_index = i;
        if (graph.pass_name(i) == "sky")
            sky_index = i;
        if (graph.pass_name(i) == "taa-resolve")
            taa_index = i;
    }
    CHECK(terrain_index < graph.pass_count());
    CHECK(terrain_index > 0);
    CHECK(terrain_index < sky_index);
    CHECK(sky_index < taa_index);
}
