// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/terrain_pass.hpp"
#include "terrain.vert.spv.h"
#include "terrain_height_probe.frag.spv.h"

// The climbing report describes entering a drawn hill. Controller-only routes cannot rule out
// that failure: their support and overlap checks share the physics surface. This proof therefore
// reads the production GPU vertex stage at sub-cell positions on actual generated hills, using
// the same global-index LOD placement and disabled distance morph as the playable stream.
TEST_CASE("generated playable hills: GPU surface agrees with collision across signed tiles") {
    using namespace rime;
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN"))
            FAIL("required Vulkan device unavailable");
        return;
    }
    constexpr std::uint32_t size = 128;
    rhi::ShaderDesc shader{};
    shader.stage = rhi::ShaderStage::Vertex;
    shader.spirv = terrain_vert_spv;
    shader.spirv_size_bytes = sizeof(terrain_vert_spv);
    const auto vs = device->create_shader(shader);
    shader.stage = rhi::ShaderStage::Fragment;
    shader.spirv = terrain_height_probe_frag_spv;
    shader.spirv_size_bytes = sizeof(terrain_height_probe_frag_spv);
    const auto fs = device->create_shader(shader);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex}};
    rhi::GraphicsPipelineDesc desc{};
    desc.vertex_shader = vs;
    desc.fragment_shader = fs;
    desc.color_format = rhi::Format::R32Uint;
    desc.depth_format = render::kDepthFormat;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.cull = rhi::CullMode::None;
    desc.bindings = bindings;
    desc.push_constant_size = sizeof(render::TerrainPush);
    const auto pipeline = device->create_graphics_pipeline(desc);
    rhi::SamplerDesc sampling{};
    sampling.min_filter = sampling.mag_filter = rhi::Filter::Nearest;
    const auto sampler = device->create_sampler(sampling);
    render::TerrainPass terrain(*device);
    float worst = 0.0f;
    std::uint32_t shifted_rejected = 0;
    const std::array<std::array<std::int32_t, 2>, 8> coords = {
        {{-5, 0}, {-6, -1}, {4, 0}, {5, 2}, {0, -5}, {-1, 4}, {-64, 47}, {63, -48}}};
    for (const auto& coord : coords) {
        CAPTURE(coord[0]);
        CAPTURE(coord[1]);
        const auto asset = assets::generate_procedural_tile({}, coord[0], coord[1], 0);
        REQUIRE(asset.has_value());
        const auto id = terrain.upload(*asset);
        REQUIRE(id != render::kInvalidTerrainTile);
        const auto& tile = terrain.tile(id);
        physics::PhysicsWorld physics;
        physics::HeightfieldDesc heightfield{};
        heightfield.samples = asset->samples;
        heightfield.columns = asset->columns;
        heightfield.rows = asset->rows;
        heightfield.cell_size_x = asset->cell_size_x;
        heightfield.cell_size_z = asset->cell_size_z;
        heightfield.height_scale = asset->height_scale;
        heightfield.height_offset = asset->height_offset;
        physics::BodyDesc body{};
        body.motion = physics::MotionType::Static;
        body.shape.type = physics::ShapeType::Heightfield;
        body.shape.heightfield = physics.register_heightfield(heightfield);
        body.position = asset->origin;
        REQUIRE(physics.create_body(body).is_valid());
        const core::Vec3 eye = asset->origin + core::Vec3{32.0f, 100.0f, 32.0f};
        const auto vp =
            core::ortho(-32.0f, 32.0f, -32.0f, 32.0f, 0.0f, 200.0f) *
            core::look_at(eye, eye + core::Vec3{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        render::TerrainLodDraw lod{};
        lod.enabled = true;
        lod.base_x = coord[0] * 32;
        lod.base_z = coord[1] * 32;
        lod.camera = eye;
        lod.morph_start = lod.morph_end = std::numeric_limits<float>::infinity();
        for (const float render_offset : {0.0f, 0.25f}) {
            lod.grid_origin.y = render_offset;
            const auto push = render::terrain_push(tile, vp, eye, {}, lod);
            render::RenderGraph graph(*device);
            graph.reset();
            const auto output =
                graph.create_texture({{size, size}, rhi::Format::R32Uint, "hill-height"});
            const auto depth =
                graph.create_texture({{size, size}, render::kDepthFormat, "hill-depth"});
            const render::RGColorAttachment colors[] = {
                {output, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}}};
            render::RGDepthAttachment depth_attachment{};
            depth_attachment.texture = depth;
            const render::RGTexture sampled[] = {
                graph.import_texture(tile.heights, rhi::ResourceState::ShaderRead)};
            render::RenderGraph::RasterPassDesc pass{};
            pass.colors = colors;
            pass.depth = &depth_attachment;
            pass.sampled = sampled;
            graph.add_raster_pass("generated-hill-height", pass, [&](rhi::CommandBuffer& cmd) {
                cmd.bind_pipeline(pipeline);
                cmd.bind_texture(0, tile.heights, sampler);
                cmd.bind_index_buffer(tile.indices, rhi::IndexType::Uint32);
                cmd.push_constants(&push, sizeof(push));
                cmd.draw_indexed(tile.index_count);
            });
            graph.export_texture(output);
            auto commands = device->begin_commands();
            graph.execute(*commands);
            device->submit_blocking(*commands);
            rhi::BufferDesc readback_desc{};
            readback_desc.size = size * size * sizeof(float);
            readback_desc.usage = rhi::BufferUsage::TransferDst;
            readback_desc.memory = rhi::MemoryUsage::GpuToCpu;
            const auto readback = device->create_buffer(readback_desc);
            commands = device->begin_commands();
            commands->copy_texture_to_buffer(graph.physical(output), readback);
            device->submit_blocking(*commands);
            std::vector<float> heights(size * size);
            device->read_buffer(readback, heights.data(), readback_desc.size, 0);
            device->destroy(readback);
            for (std::uint32_t y = 3; y < size - 3; y += 7)
                for (std::uint32_t x = 3; x < size - 3; x += 7) {
                    const core::Vec3 origin =
                        asset->origin + core::Vec3{(static_cast<float>(x) + 0.5f) * 0.5f,
                                                   100.0f,
                                                   (static_cast<float>(y) + 0.5f) * 0.5f};
                    physics::RayHit hit{};
                    REQUIRE(physics.raycast({origin, {0.0f, -1.0f, 0.0f}, 200.0f}, hit));
                    const float gpu = heights[y * size + x];
                    // Quantisation is identical; f32 ray cancellation and interpolation dominate.
                    // 2 mm covers those errors at +/-4 km while detecting visible burial.
                    const float error = std::abs(gpu - hit.point.y);
                    if (render_offset == 0.0f) {
                        CHECK(error < 0.002f);
                        worst = std::max(worst, error);
                    } else {
                        // Sensitivity perturbs the real GPU placement, leaving collision unchanged.
                        // Merely adding 25 cm to a readback would not exercise the rendering path.
                        CHECK(error > 0.24f);
                        shifted_rejected += error > 0.24f;
                    }
                }
        }
        terrain.release(id);
    }
    CHECK(shifted_rejected == 8 * 18 * 18);
    MESSAGE("generated hill GPU/collision maximum height error: ", worst, " metres");
    device->destroy(sampler);
    device->destroy(pipeline);
    device->destroy(fs);
    device->destroy(vs);
}
