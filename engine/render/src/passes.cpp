// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The pass library's engine side (M5.6): bake each pass's pipeline(s) once, declare them into a
// RenderGraph each frame. The SPIR-V is embedded at build time from engine/render/shaders/ — the
// engine library carries its own shaders the same way the tests and samples do (ADR-0008's
// offline-compile model; an asset-driven shader system is an M6+ concern).

#include "rime/render/passes.hpp"

#include "depth_only.vert.spv.h"
#include "fullscreen.vert.spv.h"
#include "pbr_forward.frag.spv.h"
#include "pbr_forward.vert.spv.h"
#include "tonemap.frag.spv.h"

namespace rime::render {

namespace {

[[nodiscard]] rhi::ShaderHandle make_shader(rhi::Device& device,
                                            rhi::ShaderStage stage,
                                            const std::uint32_t* words,
                                            std::size_t bytes,
                                            std::string_view name) {
    rhi::ShaderDesc sd{};
    sd.stage = stage;
    sd.spirv = words;
    sd.spirv_size_bytes = bytes;
    sd.debug_name = name;
    return device.create_shader(sd);
}

// The geometry passes' shared draw loop. The pipeline is already bound; the frame uniforms are
// attached once (pending bindings persist across draws — ADR-0020), then each draw re-binds only
// what changes: its mesh buffers, its 256-byte slice of the draw-uniform buffer, and (for the
// shading pass) its base-color texture. Draws run in extraction order, unsorted — sorting by
// pipeline/material/depth is a measured optimization for when scenes are big enough to show it.
void record_draws(rhi::CommandBuffer& cmd, const SceneDrawData& data, bool bind_material_texture) {
    cmd.bind_uniform_buffer(0, data.frame_ubo);
    for (std::size_t i = 0; i < data.draws.size(); ++i) {
        const DrawItem& item = data.draws[i];
        const GpuMesh& mesh = data.meshes->get(item.mesh);
        cmd.bind_vertex_buffer(mesh.vertices);
        cmd.bind_index_buffer(mesh.indices, rhi::IndexType::Uint32);
        cmd.bind_uniform_buffer(1, data.draw_ubo, i * kDrawUniformStride, sizeof(GpuDrawUniforms));
        if (bind_material_texture) {
            cmd.bind_texture(2, data.base_color_textures[i], data.material_sampler);
        }
        cmd.draw_indexed(mesh.index_count);
    }
}

} // namespace

// ── DepthPrepass ──────────────────────────────────────────────────────────────────────────────

DepthPrepass::DepthPrepass(rhi::Device& device) : device_(device) {
    vertex_shader_ = make_shader(device,
                                 rhi::ShaderStage::Vertex,
                                 depth_only_vert_spv,
                                 sizeof(depth_only_vert_spv),
                                 "depth_only.vert");

    // The depth-only pipeline shape (the M5.6 RHI addition): no fragment shader, no color
    // attachment. The vertex layout is the full registry vertex — the shader consumes only
    // position; unconsumed attributes are legal and keep one layout shared across every pass.
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex},
        {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.color_format = rhi::Format::Undefined;
    pd.vertex_layout.stride = MeshRegistry::vertex_stride();
    pd.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    pd.cull = rhi::CullMode::Back;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.bindings = bindings;
    pd.debug_name = "depth-prepass";
    pipeline_ = device.create_graphics_pipeline(pd);
}

DepthPrepass::~DepthPrepass() {
    device_.destroy(pipeline_);
    device_.destroy(vertex_shader_);
}

void DepthPrepass::add(RenderGraph& graph, RGTexture depth, const SceneDrawData& data) const {
    // Clear to the far plane, STORE the result — the whole point is that the forward pass loads
    // this depth back. (RGDepthAttachment's default store is DontCare; not here.)
    const RGDepthAttachment depth_att{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false};
    RenderGraph::RasterPassDesc desc{};
    desc.depth = &depth_att;
    graph.add_raster_pass("depth-prepass", desc, [pipe = pipeline_, data](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipe);
        record_draws(cmd, data, /*bind_material_texture=*/false);
    });
}

// ── ForwardPbrPass ────────────────────────────────────────────────────────────────────────────

ForwardPbrPass::ForwardPbrPass(rhi::Device& device) : device_(device) {
    vertex_shader_ = make_shader(device,
                                 rhi::ShaderStage::Vertex,
                                 pbr_forward_vert_spv,
                                 sizeof(pbr_forward_vert_spv),
                                 "pbr_forward.vert");
    fragment_shader_ = make_shader(device,
                                   rhi::ShaderStage::Fragment,
                                   pbr_forward_frag_spv,
                                   sizeof(pbr_forward_frag_spv),
                                   "pbr_forward.frag");

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.vertex_layout.stride = MeshRegistry::vertex_stride();
    pd.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    pd.color_format = kHdrFormat;
    pd.cull = rhi::CullMode::Back;
    pd.depth_test = true;
    pd.depth_format = kDepthFormat;
    pd.bindings = bindings;

    // Two depth disciplines, two pipelines (depth state is baked, not dynamic — ADR-0007's
    // explicit-pipeline model). After a pre-pass: test EQUAL against the depth it stored, write
    // nothing — every surviving fragment is THE visible surface. The invariance contract in
    // depth_only.vert is what makes Equal safe; LessEqual is the documented fallback if a driver
    // ever breaks it. Standalone: classic Less with writes.
    pd.depth_write = false;
    pd.depth_compare = rhi::CompareOp::Equal;
    pd.debug_name = "forward-pbr (after prepass)";
    pipeline_after_prepass_ = device.create_graphics_pipeline(pd);

    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.debug_name = "forward-pbr (standalone)";
    pipeline_standalone_ = device.create_graphics_pipeline(pd);
}

ForwardPbrPass::~ForwardPbrPass() {
    device_.destroy(pipeline_standalone_);
    device_.destroy(pipeline_after_prepass_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void ForwardPbrPass::add(RenderGraph& graph,
                         RGTexture hdr,
                         RGTexture depth,
                         bool depth_prepassed,
                         const SceneDrawData& data) const {
    // Radiance target starts at zero (black = no light); the tonemap maps it back to black, so
    // the background costs nothing and biases no test.
    const RGColorAttachment colors[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
    RGDepthAttachment depth_att{};
    depth_att.texture = depth;
    if (depth_prepassed) {
        // Load what the pre-pass stored; read-only (the declaration the graph rewards with a
        // read-after-write edge instead of write-after-write).
        depth_att.load = rhi::LoadOp::Load;
        depth_att.store = rhi::StoreOp::DontCare;
        depth_att.read_only = true;
    } else {
        depth_att.load = rhi::LoadOp::Clear;
        depth_att.store = rhi::StoreOp::DontCare;
        depth_att.read_only = false;
    }
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    const rhi::PipelineHandle pipe =
        depth_prepassed ? pipeline_after_prepass_ : pipeline_standalone_;
    graph.add_raster_pass("forward-pbr", desc, [pipe, data](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipe);
        record_draws(cmd, data, /*bind_material_texture=*/true);
    });
}

// ── TonemapPass ───────────────────────────────────────────────────────────────────────────────

TonemapPass::TonemapPass(rhi::Device& device) : device_(device) {
    vertex_shader_ = make_shader(device,
                                 rhi::ShaderStage::Vertex,
                                 fullscreen_vert_spv,
                                 sizeof(fullscreen_vert_spv),
                                 "fullscreen.vert");
    fragment_shader_ = make_shader(device,
                                   rhi::ShaderStage::Fragment,
                                   tonemap_frag_spv,
                                   sizeof(tonemap_frag_spv),
                                   "tonemap.frag");

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = kLdrFormat;
    pd.cull = rhi::CullMode::None; // one oversized triangle; nothing to cull
    pd.bindings = bindings;
    pd.debug_name = "tonemap";
    pipeline_ = device.create_graphics_pipeline(pd);

    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Nearest;
    sd.min_filter = rhi::Filter::Nearest;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.debug_name = "tonemap-nearest";
    sampler_ = device.create_sampler(sd);
}

TonemapPass::~TonemapPass() {
    device_.destroy(sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void TonemapPass::add(RenderGraph& graph, RGTexture hdr, RGTexture ldr) const {
    // DontCare load: the triangle covers every pixel, so whatever the LDR target held is dead.
    const RGColorAttachment colors[] = {{ldr, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    const RGTexture sampled[] = {hdr};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    // The λ resolves hdr's physical handle at record time (assign_physicals has run by then) —
    // the same pattern the render-graph tests established.
    graph.add_raster_pass("tonemap", desc, [this, &graph, hdr](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline_);
        cmd.bind_texture(0, graph.physical(hdr), sampler_);
        cmd.draw(3);
    });
}

} // namespace rime::render
