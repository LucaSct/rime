// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The SSR resolve pass's engine side (m10.7b, ADR-0032 §5). Owns the fullscreen graphics pipeline,
// the uniform block ssr_resolve.frag reads, and the point+clamp sampler the march reads
// depth/colour through; declares one raster pass per frame that writes scene_color + reflection
// into a second HDR colour target. See ssr.hpp for why this is a raster pass, not a compute
// dispatch.

#include "rime/render/lighting/ssr.hpp"

#include "fullscreen.vert.spv.h"
#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp" // kHdrFormat — the resolve target's format
#include "ssr_resolve.frag.spv.h"

namespace rime::render {

SsrPass::SsrPass(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = fullscreen_vert_spv;
    vs.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    vs.debug_name = "fullscreen.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = ssr_resolve_frag_spv;
    fs.spirv_size_bytes = sizeof(ssr_resolve_frag_spv);
    fs.debug_name = "ssr_resolve.frag";
    fragment_shader_ = device.create_shader(fs);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // scene colour (HDR)
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // G-buffer (m10.7a)
        {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // scene depth
        {3, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},        // SsrParams
        // m10.7c probe fallback: the DDGI atlases + sample-params the miss/rough path reads. Always
        // bound (empty_binding's 1x1 dummies when DDGI is off) — the pipeline layout is fixed.
        {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // DDGI irradiance
        {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // DDGI visibility
        {6, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},        // DdgiSampleParams
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = kHdrFormat;  // writes the second HDR target the tonemap reads
    pd.cull = rhi::CullMode::None; // one oversized triangle; nothing to cull
    pd.bindings = bindings;
    pd.debug_name = "ssr-resolve";
    pipeline_ = device.create_graphics_pipeline(pd);

    rhi::BufferDesc ub{};
    ub.size = sizeof(GpuSsrUniforms);
    ub.usage = rhi::BufferUsage::Uniform;
    ub.memory = rhi::MemoryUsage::CpuToGpu;
    ub.debug_name = "ssr-uniforms";
    uniforms_ = device.create_buffer(ub);

    // Point + clamp: a blended depth is a fictional surface (the march must read exact depths), and
    // a ray that walks off the screen must read the border pixel, not wrap to the far side.
    rhi::SamplerDesc ss{};
    ss.mag_filter = rhi::Filter::Nearest;
    ss.min_filter = rhi::Filter::Nearest;
    ss.address_mode = rhi::AddressMode::ClampToEdge;
    ss.debug_name = "ssr-sampler";
    sampler_ = device.create_sampler(ss);
}

SsrPass::~SsrPass() {
    device_.destroy(sampler_);
    device_.destroy(uniforms_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void SsrPass::add(RenderGraph& graph,
                  RGTexture scene_color,
                  RGTexture gbuffer,
                  RGTexture depth,
                  RGTexture out_hdr,
                  const SsrInputs& inputs,
                  RGTexture ddgi_irradiance,
                  RGTexture ddgi_visibility,
                  rhi::BufferHandle ddgi_params,
                  rhi::SamplerHandle ddgi_sampler) {
    // The inverse projection is what turns a uv + depth back into a view-space position — computed
    // once here, on the CPU, rather than every one of the march's steps re-inverting it on the GPU.
    // inv_view (m10.7c) does the same job for the probe fallback: view space back to the WORLD the
    // DDGI lattice is expressed in.
    GpuSsrUniforms u{};
    u.proj = inputs.proj;
    u.inv_proj = core::inverse(inputs.proj);
    u.view = inputs.view;
    u.inv_view = core::inverse(inputs.view);
    u.extent_near_far[0] = static_cast<float>(inputs.extent.width);
    u.extent_near_far[1] = static_cast<float>(inputs.extent.height);
    u.extent_near_far[2] = inputs.z_near;
    u.extent_near_far[3] = inputs.z_far;
    u.params[0] = static_cast<float>(inputs.max_steps);
    u.params[1] = inputs.thickness;
    u.params[3] = inputs.max_distance;
    u.ambient[0] = inputs.ambient[0];
    u.ambient[1] = inputs.ambient[1];
    u.ambient[2] = inputs.ambient[2];
    device_.write_buffer(uniforms_, &u, sizeof(u));

    // out_hdr is the colour attachment (DontCare load: the fullscreen triangle writes every pixel);
    // declaring the sampled reads orders this pass after the passes that wrote them — the forward
    // pass (scene_color, gbuffer, depth; depth transitioned DepthAttachment → ShaderRead) and the
    // DDGI blend passes (the two atlases). The tonemap then samples out_hdr through the ordinary
    // colour-attachment → sampled path. The DDGI atlases carry their own linear sampler (the one
    // that makes the octahedral border ring do its job, ddgi.md §3), distinct from SSR's own
    // point+clamp; depth/colour must not interpolate, the atlases must.
    const RGColorAttachment colors[] = {{out_hdr, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    const RGTexture sampled[] = {scene_color, gbuffer, depth, ddgi_irradiance, ddgi_visibility};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    graph.add_raster_pass("ssr-resolve",
                          desc,
                          [pipe = pipeline_,
                           ubo = uniforms_,
                           smp = sampler_,
                           scene_color,
                           gbuffer,
                           depth,
                           ddgi_irradiance,
                           ddgi_visibility,
                           ddgi_params,
                           ddgi_sampler,
                           &graph](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipe);
                              cmd.bind_texture(0, graph.physical(scene_color), smp);
                              cmd.bind_texture(1, graph.physical(gbuffer), smp);
                              cmd.bind_texture(2, graph.physical(depth), smp);
                              cmd.bind_uniform_buffer(3, ubo);
                              cmd.bind_texture(4, graph.physical(ddgi_irradiance), ddgi_sampler);
                              cmd.bind_texture(5, graph.physical(ddgi_visibility), ddgi_sampler);
                              cmd.bind_uniform_buffer(6, ddgi_params);
                              cmd.draw(3);
                          });
}

} // namespace rime::render
