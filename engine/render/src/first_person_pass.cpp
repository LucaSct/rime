// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/first_person_pass.hpp"

#include "first_person_depth.frag.spv.h"
#include "fullscreen.vert.spv.h"
#include "rime/render/passes.hpp"

namespace rime::render {

FirstPersonPass::FirstPersonPass(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Vertex;
    sd.spirv = fullscreen_vert_spv;
    sd.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    sd.debug_name = "first-person-depth.vert";
    vertex_shader_ = device_.create_shader(sd);
    sd.stage = rhi::ShaderStage::Fragment;
    sd.spirv = first_person_depth_frag_spv;
    sd.spirv_size_bytes = sizeof(first_person_depth_frag_spv);
    sd.debug_name = "first-person-depth.frag";
    fragment_shader_ = device_.create_shader(sd);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = rhi::Format::Undefined;
    pd.depth_format = kDepthFormat;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Always;
    pd.cull = rhi::CullMode::None;
    pd.bindings = bindings;
    pd.debug_name = "first-person-combine-depth";
    pipeline_ = device_.create_graphics_pipeline(pd);
    rhi::SamplerDesc ss{};
    ss.mag_filter = rhi::Filter::Nearest;
    ss.min_filter = rhi::Filter::Nearest;
    ss.address_mode = rhi::AddressMode::ClampToEdge;
    ss.debug_name = "first-person-depth-sampler";
    sampler_ = device_.create_sampler(ss);
}

FirstPersonPass::~FirstPersonPass() {
    device_.destroy(sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

RGTexture FirstPersonPass::combine_depth(RenderGraph& graph,
                                         RGTexture world_depth,
                                         RGTexture model_depth,
                                         rhi::Extent2D extent) const {
    const RGTexture combined = graph.create_texture({extent, kDepthFormat, "scene-depth-combined"});
    RGDepthAttachment output{};
    output.texture = combined;
    output.load = rhi::LoadOp::DontCare;
    output.store = rhi::StoreOp::Store;
    const RGTexture sampled[] = {world_depth, model_depth};
    RenderGraph::RasterPassDesc desc{};
    desc.depth = &output;
    desc.sampled = sampled;
    auto draw = [this, world_depth, model_depth, &graph](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline_);
        cmd.bind_texture(0, graph.physical(world_depth), sampler_);
        cmd.bind_texture(1, graph.physical(model_depth), sampler_);
        cmd.draw(3);
    };
    graph.add_raster_pass("first-person-combine-depth", desc, draw);
    return combined;
}

} // namespace rime::render
