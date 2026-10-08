// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine side of the sky's split-sum specular (ADR-0078 section 2): two pipelines, two
// persistent textures, and the decision of when each is rebuilt. The mathematics is in the shaders
// (sky_specular_prefilter.comp, sky_specular_dfg.comp); this file is bookkeeping, and the
// bookkeeping is where the counters live.

#include "rime/render/lighting/sky_specular.hpp"

#include "sky_specular_dfg.comp.spv.h"
#include "sky_specular_prefilter.comp.spv.h"

namespace rime::render {
namespace {

// std140: one uvec4, so the CPU and GLSL sides agree with no padding rule to remember.
struct GpuSpecularParams {
    std::uint32_t counts[4]{};
};

} // namespace

SkySpecular::SkySpecular(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc prefilter_cs{};
    prefilter_cs.stage = rhi::ShaderStage::Compute;
    prefilter_cs.spirv = sky_specular_prefilter_comp_spv;
    prefilter_cs.spirv_size_bytes = sizeof(sky_specular_prefilter_comp_spv);
    prefilter_cs.debug_name = "sky_specular_prefilter.comp";
    prefilter_shader_ = device.create_shader(prefilter_cs);

    rhi::ShaderDesc dfg_cs{};
    dfg_cs.stage = rhi::ShaderStage::Compute;
    dfg_cs.spirv = sky_specular_dfg_comp_spv;
    dfg_cs.spirv_size_bytes = sizeof(sky_specular_dfg_comp_spv);
    dfg_cs.debug_name = "sky_specular_dfg.comp";
    dfg_shader_ = device.create_shader(dfg_cs);

    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute}, // the chain being written
            {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute}, // the sky
            {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = prefilter_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-specular-prefilter";
        prefilter_pipeline_ = device.create_compute_pipeline(pd);
    }
    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
            {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = dfg_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-specular-dfg";
        dfg_pipeline_ = device.create_compute_pipeline(pd);
    }

    // The source is read at fractional texels (a GGX sample direction lands between them), so it is
    // sampled linearly; azimuth wraps and elevation clamps because the sky-view table has a real
    // seam in one axis and real poles in the other (sky.cpp's lut_sampler_ explains both).
    rhi::SamplerDesc ss{};
    ss.mag_filter = rhi::Filter::Linear;
    ss.min_filter = rhi::Filter::Linear;
    ss.address_mode = rhi::AddressMode::ClampToEdge;
    ss.address_mode_u = rhi::AddressMode::Repeat;
    ss.debug_name = "sky-specular-source-sampler";
    source_sampler_ = device.create_sampler(ss);

    // The DFG table is a parameter table with no seam: clamp on both axes.
    ss.address_mode_u = rhi::AddressMode::ClampToEdge;
    ss.debug_name = "sky-specular-dfg-sampler";
    dfg_sampler_ = device.create_sampler(ss);

    // The off-state placeholders, parked in ShaderRead once and never written again. The shaders
    // branch on a flag in the frame uniforms, so their contents are never consulted -- they exist
    // so the forward pipeline's fixed descriptor layout is always satisfied. The array has TWO
    // layers because a single-layer image gets a plain 2-D view, which a sampler2DArray cannot
    // bind.
    {
        rhi::TextureDesc td{};
        td.extent = {1, 1};
        td.array_layers = 2;
        td.format = rhi::Format::RGBA16Float;
        td.usage = rhi::TextureUsage::Sampled;
        td.debug_name = "sky-specular-dummy-prefiltered";
        dummy_prefiltered_ = device.create_texture(td);

        td.array_layers = 1;
        td.debug_name = "sky-specular-dummy-dfg";
        dummy_dfg_ = device.create_texture(td);

        auto cmd = device.begin_commands();
        cmd->texture_barrier(
            dummy_prefiltered_, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
        cmd->texture_barrier(
            dummy_dfg_, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
        device.submit_blocking(*cmd);
    }
}

SkySpecular::~SkySpecular() {
    if (prefiltered_.is_valid())
        device_.destroy(prefiltered_);
    if (dfg_.is_valid())
        device_.destroy(dfg_);
    device_.destroy(dummy_dfg_);
    device_.destroy(dummy_prefiltered_);
    device_.destroy(dfg_sampler_);
    device_.destroy(source_sampler_);
    device_.destroy(dfg_pipeline_);
    device_.destroy(prefilter_pipeline_);
    device_.destroy(dfg_shader_);
    device_.destroy(prefilter_shader_);
}

void SkySpecular::ensure_resources() {
    if (prefiltered_.is_valid())
        return;

    rhi::TextureDesc td{};
    td.extent = {kSkySpecularWidth, kSkySpecularHeight};
    td.array_layers = kSkySpecularLayers;
    td.format = rhi::Format::RGBA16Float;
    // Storage: the bake writes it. Sampled: the forward pass reads it. TransferSrc: a test reads it
    // back to check the bake against independent arithmetic.
    td.usage =
        rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
    td.debug_name = "sky-specular-prefiltered";
    prefiltered_ = device_.create_texture(td);
    prefiltered_state_ = rhi::ResourceState::Undefined;

    td.extent = {kSkySpecularDfgSize, kSkySpecularDfgSize};
    td.array_layers = 1;
    td.debug_name = "sky-specular-dfg";
    dfg_ = device_.create_texture(td);
    dfg_state_ = rhi::ResourceState::Undefined;
}

SkySpecularBinding SkySpecular::add(RenderGraph& graph, RGTexture source, bool source_changed) {
    ensure_resources();

    const RGTexture prefiltered_rg = graph.import_texture(prefiltered_, prefiltered_state_);
    const RGTexture dfg_rg = graph.import_texture(dfg_, dfg_state_);

    // ── The DFG table: a pure function of constants, so once ever. ───────────────────────────
    if (dfg_valid_) {
        ++stats_.dfg_reused;
    } else {
        const GpuSpecularParams p{{kSkySpecularDfgSamples, 0, 0, 0}};
        const RenderGraph::FrameSlice ubo = graph.push_frame_data(&p, sizeof(p));
        const RGTexture writes[] = {dfg_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-specular-dfg",
            desc,
            [pipe = dfg_pipeline_, dfg_rg, ubo, &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(dfg_rg));
                cmd.bind_uniform_buffer(1, ubo.buffer, ubo.offset, sizeof(p));
                cmd.dispatch((kSkySpecularDfgSize + kSkySpecularGroup - 1) / kSkySpecularGroup,
                             (kSkySpecularDfgSize + kSkySpecularGroup - 1) / kSkySpecularGroup,
                             1);
            });
        dfg_valid_ = true;
        ++stats_.dfg_filled;
        // A baking frame ends in ShaderRead because the consumer (the forward pass) samples it. A
        // reusing frame declares no access and keeps whatever the last consumer reported.
        dfg_state_ = rhi::ResourceState::ShaderRead;
    }

    // ── The prefiltered chain: rebuilt when the sky it filters moved. ────────────────────────
    if (prefilter_valid_ && !source_changed) {
        ++stats_.prefilter_reused;
    } else {
        const GpuSpecularParams p{{kSkySpecularLevels, kSkySpecularPrefilterSamples, 0, 0}};
        const RenderGraph::FrameSlice ubo = graph.push_frame_data(&p, sizeof(p));
        const RGTexture sampled[] = {source};
        const RGTexture writes[] = {prefiltered_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.sampled = sampled;
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-specular-prefilter",
            desc,
            [pipe = prefilter_pipeline_,
             prefiltered_rg,
             source,
             ubo,
             smp = source_sampler_,
             &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(prefiltered_rg));
                cmd.bind_texture(1, graph.physical(source), smp);
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(p));
                cmd.dispatch((kSkySpecularWidth + kSkySpecularGroup - 1) / kSkySpecularGroup,
                             (kSkySpecularHeight + kSkySpecularGroup - 1) / kSkySpecularGroup,
                             kSkySpecularLayers);
            });
        prefilter_valid_ = true;
        ++stats_.prefilter_filled;
        prefiltered_state_ = rhi::ResourceState::ShaderRead;
    }

    return SkySpecularBinding{prefiltered_rg, dfg_rg, dfg_sampler_};
}

SkySpecularBinding SkySpecular::empty_binding(RenderGraph& graph) {
    ++stats_.disabled_frames;
    return SkySpecularBinding{
        graph.import_texture(dummy_prefiltered_, rhi::ResourceState::ShaderRead),
        graph.import_texture(dummy_dfg_, rhi::ResourceState::ShaderRead),
        dfg_sampler_};
}

} // namespace rime::render
