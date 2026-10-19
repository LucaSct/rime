// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "fullscreen.vert.spv.h"
#include "render_test_support.hpp"
#include "rime/render/lighting/shadows.hpp"
#include "shadow_transition_probe.frag.spv.h"

using namespace rime;
using namespace rime::render;

TEST_CASE("shadows: shared mesh and terrain sampler blends cascade boundaries") {
    auto device = rhi::create_device({});
    if (!device) {
        if (test::vulkan_required())
            FAIL("Vulkan required for cascade transition proof");
        return;
    }
    if (test::shadow_depth_sampling_unsupported(*device)) {
        MESSAGE("layered depth sampling unavailable on portability device");
        return;
    }
    constexpr std::uint32_t width = 512, height = 4, map_size = 64;
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Vertex;
    sd.spirv = fullscreen_vert_spv;
    sd.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    const auto vs = device->create_shader(sd);
    sd.stage = rhi::ShaderStage::Fragment;
    sd.spirv = shadow_transition_probe_frag_spv;
    sd.spirv_size_bytes = sizeof(shadow_transition_probe_frag_spv);
    const auto fs = device->create_shader(sd);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment}};
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.color_format = kHdrFormat;
    pd.cull = rhi::CullMode::None;
    pd.bindings = bindings;
    pd.push_constant_size = 32;
    const auto pipeline = device->create_graphics_pipeline(pd);
    rhi::SamplerDesc sampler_desc{};
    sampler_desc.mag_filter = rhi::Filter::Linear;
    sampler_desc.min_filter = rhi::Filter::Linear;
    sampler_desc.address_mode = rhi::AddressMode::ClampToEdge;
    sampler_desc.compare_enable = true;
    sampler_desc.compare_op = rhi::CompareOp::LessEqual;
    const auto sampler = device->create_sampler(sampler_desc);

    // Deliberately disagreeing constant-depth maps make a cascade switch express full contrast.
    // A real scene can disagree through undersampling; a constant/always-lit implementation
    // cannot pass the contrasting interior/exterior controls below. The fragment wrapper includes
    // the exact production sampler shared by terrain and mesh, not a CPU copy of its formula.
    const auto render = [&](bool depth_sweep, std::uint32_t count, float coarse_depth) {
        RenderGraph graph(*device);
        graph.reset();
        const auto maps =
            graph.create_texture({{map_size, map_size}, kDepthFormat, "transition-maps", 2, true});
        for (std::uint32_t layer = 0; layer < 2; ++layer) {
            RGDepthAttachment attachment{};
            attachment.texture = maps;
            attachment.layer = layer;
            attachment.load = rhi::LoadOp::Clear;
            attachment.store = rhi::StoreOp::Store;
            attachment.clear_depth = layer == 0 ? 0.0f : coarse_depth;
            RenderGraph::RasterPassDesc clear{};
            clear.depth = &attachment;
            graph.add_raster_pass(
                layer ? "clear-coarse" : "clear-fine", clear, [](rhi::CommandBuffer&) {});
        }
        GpuShadowUniforms uniform{};
        uniform.cascade_view_proj[0] = {};
        uniform.cascade_view_proj[1] = {};
        uniform.cascade_view_proj[1].at(0, 0) = 0.5f;
        uniform.cascade_view_proj[1].at(1, 1) = 0.5f;
        uniform.cascade_view_proj[1].at(2, 2) = 0.25f;
        uniform.cascade_view_proj[1].at(2, 3) = 0.375f;
        uniform.params[0] = static_cast<float>(count);
        uniform.params[1] = 1.0f;
        uniform.texel[0] = 1.0f / map_size;
        const auto slice = graph.push_frame_data(&uniform, sizeof(uniform));
        const auto color = graph.create_texture({{width, height}, kHdrFormat, "transition-output"});
        const RGColorAttachment colors[] = {{color, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}}};
        const RGTexture sampled[] = {maps};
        RenderGraph::RasterPassDesc desc{};
        desc.colors = colors;
        desc.sampled = sampled;
        struct Probe {
            float sweep[4];
            std::int32_t mode[4];
        };
        const Probe probe{
            {depth_sweep ? -0.05f : -1.2f, depth_sweep ? 0.1f : 1.2f, float(width), float(height)},
            {depth_sweep ? 1 : 0, 0, 0, 0}};
        graph.add_raster_pass(
            "shared-shadow-probe", desc, [&, maps, slice, probe](rhi::CommandBuffer& cmd) {
                cmd.bind_pipeline(pipeline);
                cmd.bind_texture(0, graph.physical(maps), sampler);
                cmd.bind_uniform_buffer(1, slice.buffer, slice.offset, sizeof(GpuShadowUniforms));
                cmd.push_constants(&probe, sizeof(probe));
                cmd.draw(3);
            });
        graph.export_texture(color);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto raw = test::read_texture(*device, graph.physical(color), width, height, 8);
        return test::decode_hdr(raw, width, height);
    };
    const auto x = render(false, 2, 1.0f);
    const auto z = render(true, 2, 1.0f);
    const auto no_shadows = render(false, 0, 1.0f);
    const auto both_shadow = render(false, 2, 0.0f);
    const auto visibility = [](const test::HdrImage& image, std::uint32_t i) {
        return image.rgb[(width + i) * 3];
    };
    float max_x_step = 0.0f, max_z_step = 0.0f;
    for (std::uint32_t i = 1; i < width; ++i) {
        REQUIRE(std::isfinite(visibility(x, i)));
        REQUIRE(std::isfinite(visibility(z, i)));
        max_x_step = std::max(max_x_step, std::abs(visibility(x, i) - visibility(x, i - 1)));
        max_z_step = std::max(max_z_step, std::abs(visibility(z, i) - visibility(z, i - 1)));
        CHECK(visibility(no_shadows, i) == doctest::Approx(1.0f));
        CHECK(visibility(both_shadow, i) == doctest::Approx(0.0f));
    }
    CHECK(visibility(x, 0) > 0.99f);
    CHECK(visibility(x, width / 2) < 0.01f);
    CHECK(visibility(x, width - 1) > 0.99f);
    CHECK(visibility(z, 0) > 0.99f);
    CHECK(visibility(z, width - 1) < 0.01f);
    CHECK(max_x_step < 0.1f); // The old hard switch jumps the entire unit contrast in one pixel.
    CHECK(max_z_step < 0.03f);
    MESSAGE("cascade transition steps x=", max_x_step, ", z=", max_z_step);
    device->destroy(sampler);
    device->destroy(pipeline);
    device->destroy(fs);
    device->destroy(vs);
}
