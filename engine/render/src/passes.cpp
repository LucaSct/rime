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
#include "pbr_forward_shadowed.frag.spv.h"
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
void record_draws(rhi::CommandBuffer& cmd, const SceneDrawData& data, bool bind_material_textures) {
    // Binding 0 is FrameUniforms at data.frame_ubo_offset — 0 for the camera pass, cascade c's
    // 256-byte view_proj slice for a CSM depth pass (m10.1). depth_only.vert / the forward shaders
    // read only the leading members of whatever block sits there, so one loop serves every view.
    cmd.bind_uniform_buffer(0, data.frame_ubo, data.frame_ubo_offset);
    for (std::size_t i = 0; i < data.draws.size(); ++i) {
        const DrawItem& item = data.draws[i];
        const GpuMesh& mesh = data.meshes->get(item.mesh);
        cmd.bind_vertex_buffer(mesh.vertices);
        cmd.bind_index_buffer(mesh.indices, rhi::IndexType::Uint32);
        cmd.bind_uniform_buffer(1, data.draw_ubo, i * kDrawUniformStride, sizeof(GpuDrawUniforms));
        if (bind_material_textures) {
            // The five material maps (fallbacks already resolved by the SceneRenderer), bindings
            // 2–6 matching pbr_forward.frag. The depth pre-pass skips them — it has no fragment
            // shader.
            cmd.bind_texture(2, data.base_color_textures[i], data.material_sampler);
            cmd.bind_texture(3, data.metallic_roughness_textures[i], data.material_sampler);
            cmd.bind_texture(4, data.normal_textures[i], data.material_sampler);
            cmd.bind_texture(5, data.occlusion_textures[i], data.material_sampler);
            cmd.bind_texture(6, data.emissive_textures[i], data.material_sampler);
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

void DepthPrepass::add(RenderGraph& graph,
                       RGTexture depth,
                       const SceneDrawData& data,
                       std::uint32_t layer) const {
    // Clear to the far plane, STORE the result — the whole point is that the forward pass (or, for
    // a CSM cascade, the shadow sample) loads this depth back. `layer` aims the pass at one cascade
    // of a layered depth target (m10.1); 0 is the ordinary single-layer case.
    RGDepthAttachment depth_att{};
    depth_att.texture = depth;
    depth_att.load = rhi::LoadOp::Clear;
    depth_att.store = rhi::StoreOp::Store;
    depth_att.clear_depth = 1.0f;
    depth_att.layer = layer;
    RenderGraph::RasterPassDesc desc{};
    desc.depth = &depth_att;
    graph.add_raster_pass("depth-prepass", desc, [pipe = pipeline_, data](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipe);
        record_draws(cmd, data, /*bind_material_textures=*/false);
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
    shadowed_fragment_shader_ = make_shader(device,
                                            rhi::ShaderStage::Fragment,
                                            pbr_forward_shadowed_frag_spv,
                                            sizeof(pbr_forward_shadowed_frag_spv),
                                            "pbr_forward_shadowed.frag");

    // Bindings 0/1 = frame/draw uniforms; 2–6 = the five material maps (base-color,
    // metallic-roughness, normal, occlusion, emissive). One layout, every permutation — untextured
    // slots bind a 1x1 fallback rather than spawning a shader variant (M6.4).
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {3, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {6, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
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

    // The shadowed variants (m10.1 + m10.2): the same two depth disciplines, but the shadowed
    // fragment shader and four extra bindings — 7 = the cascade depth array (sampler2DArrayShadow),
    // 8 = the ShadowUniforms block, 9 = the local (spot) depth array, 10 = the LocalShadowUniforms
    // block. A separate pipeline so the shadow-off path is the byte-identical baseline above
    // (ADR-0032 §11); it is only ever bound when a caller opts shadows in.
    const rhi::BindingDesc shadowed_bindings[] = {
        {0, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {3, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {6, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {7, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // cascade map
        {8, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},        // ShadowUniforms
        {9, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // local (spot) map
        {10, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment}, // LocalShadowUniforms
        {11, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment}, // clustered lights (m10.3)
        {12, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment}, // per-froxel light lists
        {13, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment}, // ClusterUniforms
    };
    // 14 of the RHI's 16 descriptor slots (rhi::kMaxBindings) are now spoken for. The next
    // technique that wants its own buffers — GI probes (m10.5), SSR (m10.7) — is the trigger for a
    // second descriptor set or a bindless table rather than a 15th binding.
    pd.fragment_shader = shadowed_fragment_shader_;
    pd.bindings = shadowed_bindings;
    pd.depth_write = false;
    pd.depth_compare = rhi::CompareOp::Equal;
    pd.debug_name = "forward-pbr shadowed (after prepass)";
    pipeline_shadowed_after_prepass_ = device.create_graphics_pipeline(pd);

    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.debug_name = "forward-pbr shadowed (standalone)";
    pipeline_shadowed_standalone_ = device.create_graphics_pipeline(pd);
}

ForwardPbrPass::~ForwardPbrPass() {
    device_.destroy(pipeline_shadowed_standalone_);
    device_.destroy(pipeline_shadowed_after_prepass_);
    device_.destroy(pipeline_standalone_);
    device_.destroy(pipeline_after_prepass_);
    device_.destroy(shadowed_fragment_shader_);
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
        record_draws(cmd, data, /*bind_material_textures=*/true);
    });
}

void ForwardPbrPass::add_shadowed(RenderGraph& graph,
                                  RGTexture hdr,
                                  RGTexture depth,
                                  bool depth_prepassed,
                                  const SceneDrawData& data,
                                  const ShadowBinding& shadow,
                                  const LocalShadowBinding& local,
                                  const ClusterBinding& clusters) const {
    const RGColorAttachment colors[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
    RGDepthAttachment depth_att{};
    depth_att.texture = depth;
    if (depth_prepassed) {
        depth_att.load = rhi::LoadOp::Load;
        depth_att.store = rhi::StoreOp::DontCare;
        depth_att.read_only = true;
    } else {
        depth_att.load = rhi::LoadOp::Clear;
        depth_att.store = rhi::StoreOp::DontCare;
        depth_att.read_only = false;
    }
    // Both shadow arrays are SAMPLED here — declaring them makes the graph order this pass after
    // the depth passes that wrote them and transition both to ShaderRead (m10.1 cascades, m10.2
    // spots).
    const RGTexture sampled[] = {shadow.map, local.map};
    // Declaring the two cluster buffers is what orders this pass after the cull dispatch that
    // filled them and gets the storage-write → shader-read barrier emitted (m10.3).
    const RGBuffer buffers[] = {clusters.lights, clusters.lists};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    desc.sampled = sampled;
    desc.buffer_reads = buffers;
    const rhi::PipelineHandle pipe =
        depth_prepassed ? pipeline_shadowed_after_prepass_ : pipeline_shadowed_standalone_;
    graph.add_raster_pass("forward-pbr shadowed",
                          desc,
                          [pipe, data, shadow, local, clusters, &graph](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipe);
                              // Bindings 7–13 are attached once (they persist across draws —
                              // ADR-0020); record_draws re-binds only per-draw state on top. The
                              // resources' physical handles resolve now (assign_physicals has run),
                              // the same late-resolve the tonemap pass uses.
                              cmd.bind_texture(7, graph.physical(shadow.map), shadow.sampler);
                              cmd.bind_uniform_buffer(8, shadow.ubo);
                              cmd.bind_texture(9, graph.physical(local.map), local.sampler);
                              cmd.bind_uniform_buffer(10, local.ubo);
                              cmd.bind_storage_buffer(11, graph.physical_buffer(clusters.lights));
                              cmd.bind_storage_buffer(12, graph.physical_buffer(clusters.lists));
                              cmd.bind_uniform_buffer(13, clusters.ubo);
                              record_draws(cmd, data, /*bind_material_textures=*/true);
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
