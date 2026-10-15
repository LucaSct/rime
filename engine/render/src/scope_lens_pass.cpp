// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/scope_lens_pass.hpp"

#include <algorithm>
#include <cmath>

#include "scope_lens.frag.spv.h"
#include "scope_lens.vert.spv.h"
#include "scope_lens_sky.frag.spv.h"

namespace rime::render {
namespace {
struct LensUniforms {
    core::Mat4 view_proj, model, normal_matrix, optic_from_world, world_from_optic,
        optical_view_proj;
    float main_eye[4];
    float aperture[4];
    float optical[4];
    float glass[4];
};

static_assert(sizeof(LensUniforms) == 448 && offsetof(LensUniforms, main_eye) == 384 &&
              offsetof(LensUniforms, aperture) == 400 && offsetof(LensUniforms, optical) == 416 &&
              offsetof(LensUniforms, glass) == 432);

bool finite(const core::Mat4& matrix) {
    return std::all_of(
        std::begin(matrix.m), std::end(matrix.m), [](float x) { return std::isfinite(x); });
}

bool positive(float value) {
    return std::isfinite(value) && value > 0.0f;
}
} // namespace

ScopeLensPass::ScopeLensPass(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc shader{};
    shader.stage = rhi::ShaderStage::Vertex;
    shader.spirv = scope_lens_vert_spv;
    shader.spirv_size_bytes = sizeof(scope_lens_vert_spv);
    shader.debug_name = "scope-lens.vert";
    vertex_shader_ = device_.create_shader(shader);
    shader.stage = rhi::ShaderStage::Fragment;
    shader.spirv = scope_lens_frag_spv;
    shader.spirv_size_bytes = sizeof(scope_lens_frag_spv);
    shader.debug_name = "scope-lens.frag";
    fragment_shader_ = device_.create_shader(shader);
    shader.spirv = scope_lens_sky_frag_spv;
    shader.spirv_size_bytes = sizeof(scope_lens_sky_frag_spv);
    shader.debug_name = "scope-lens-sky.frag";
    sky_fragment_shader_ = device_.create_shader(shader);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::UniformBuffer, rhi::StageMask::Vertex | rhi::StageMask::Fragment},
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pipeline{};
    pipeline.vertex_shader = vertex_shader_;
    pipeline.fragment_shader = fragment_shader_;
    pipeline.vertex_layout.stride = MeshRegistry::vertex_stride();
    pipeline.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    pipeline.color_format = kHdrFormat;
    pipeline.depth_format = kDepthFormat;
    pipeline.depth_test = true;
    pipeline.depth_write = false;
    pipeline.depth_compare = rhi::CompareOp::LessEqual;
    pipeline.cull = rhi::CullMode::None;
    pipeline.bindings = std::span(bindings, 2);
    pipeline.debug_name = "scope-lens";
    pipeline_ = device_.create_graphics_pipeline(pipeline);
    pipeline.fragment_shader = sky_fragment_shader_;
    pipeline.bindings = bindings;
    pipeline.debug_name = "scope-lens-sky";
    sky_pipeline_ = device_.create_graphics_pipeline(pipeline);

    rhi::SamplerDesc sampler{};
    sampler.mag_filter = rhi::Filter::Linear;
    sampler.min_filter = rhi::Filter::Linear;
    sampler.address_mode = rhi::AddressMode::ClampToEdge;
    sampler.debug_name = "scope-lens-clamp";
    sampler_ = device_.create_sampler(sampler);
}

ScopeLensPass::~ScopeLensPass() {
    device_.destroy(sampler_);
    device_.destroy(sky_pipeline_);
    device_.destroy(pipeline_);
    device_.destroy(sky_fragment_shader_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

RGTexture ScopeLensPass::add(RenderGraph& graph,
                             RGTexture parent_hdr,
                             RGTexture model_depth,
                             RGTexture scope_hdr,
                             RGTexture prefiltered_sky,
                             rhi::Extent2D extent,
                             const MeshRegistry& meshes,
                             const DrawItem& lens,
                             const Params& params) const {
    // Refuse unusable inputs explicitly, including sampling the attachment being written. The
    // caller's parent projection is never altered, and refusal does not allocate scratch/passes.
    if (!parent_hdr.is_valid() || !model_depth.is_valid() || !scope_hdr.is_valid() ||
        parent_hdr.index == scope_hdr.index || !extent.width || !extent.height ||
        !meshes.contains(lens.mesh) || !lens.index_count || !finite(lens.model) ||
        !finite(params.main_view_proj) || !finite(params.optic_from_world) ||
        !finite(params.optical_view_proj) || !std::isfinite(params.main_eye.x) ||
        !std::isfinite(params.main_eye.y) || !std::isfinite(params.main_eye.z) ||
        !positive(params.lens_radius) || !positive(params.eye_relief) ||
        !positive(params.eye_relief_tolerance) || !positive(params.eye_box_radius) ||
        !positive(params.focus_distance) || !positive(params.optical_tan_half_fov) ||
        !std::isfinite(params.intensity) || !std::isfinite(params.reflection_strength) ||
        !std::isfinite(params.glass_roughness) || !std::isfinite(params.reticle_strength))
        return parent_hdr;
    const auto& mesh = meshes.get(lens.mesh);
    if (lens.first_index > mesh.index_count ||
        lens.index_count > mesh.index_count - lens.first_index ||
        std::abs(core::determinant(lens.model)) < 1.0e-12f ||
        std::abs(core::determinant(params.optic_from_world)) < 1.0e-12f)
        return parent_hdr;

    const LensUniforms uniforms{
        params.main_view_proj,
        lens.model,
        core::transpose(core::inverse(lens.model)),
        params.optic_from_world,
        core::inverse(params.optic_from_world),
        params.optical_view_proj,
        {params.main_eye.x, params.main_eye.y, params.main_eye.z, 1.0f},
        {params.lens_radius, params.eye_relief, params.eye_relief_tolerance, params.eye_box_radius},
        {params.focus_distance,
         params.optical_tan_half_fov,
         std::max(0.0f, params.intensity),
         std::max(0.0f, params.reflection_strength)},
        {std::clamp(params.glass_roughness, 0.0f, 1.0f),
         std::clamp(params.reticle_strength, 0.0f, 1.0f),
         0.0f,
         0.0f}};
    const auto uniform_slice = graph.push_frame_data(&uniforms, sizeof(uniforms));
    // LOAD creates a read dependency on the preceding world/first-person HDR writer. Depth is
    // read-only: housing/foreground already in that buffer occludes this actual indexed surface.
    const RGColorAttachment color[] = {{parent_hdr, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    RGDepthAttachment depth{};
    depth.texture = model_depth;
    depth.load = rhi::LoadOp::Load;
    depth.store = rhi::StoreOp::Store;
    depth.read_only = true;
    const bool sky = prefiltered_sky.is_valid();
    const RGTexture sampled[] = {scope_hdr, prefiltered_sky};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = color;
    desc.depth = &depth;
    desc.sampled = std::span(sampled, sky ? 2 : 1);
    const auto vertices = mesh.vertices;
    const auto indices = mesh.indices;
    const auto first_index = lens.first_index;
    const auto index_count = lens.index_count;
    graph.add_raster_pass("scope-lens",
                          desc,
                          [this,
                           &graph,
                           scope_hdr,
                           prefiltered_sky,
                           uniform_slice,
                           vertices,
                           indices,
                           first_index,
                           index_count,
                           sky](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(sky ? sky_pipeline_ : pipeline_);
                              // Retain the graph's attachment-sized viewport AND scissor; requested
                              // extents can never expand rasterization beyond the actual parent
                              // target.
                              cmd.bind_uniform_buffer(0,
                                                      uniform_slice.buffer,
                                                      uniform_slice.offset,
                                                      uniform_slice.size);
                              cmd.bind_texture(1, graph.physical(scope_hdr), sampler_);
                              if (sky)
                                  cmd.bind_texture(2, graph.physical(prefiltered_sky), sampler_);
                              cmd.bind_vertex_buffer(vertices);
                              cmd.bind_index_buffer(indices, rhi::IndexType::Uint32);
                              cmd.draw_indexed(index_count, 1, first_index);
                          });
    return parent_hdr;
}

} // namespace rime::render
