// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18 step-2 material resolve. See the header for the contract and vg_resolve.frag for the
// analytic-barycentric technique; the notes here are about binding and gating.

#include "rime/render/virtual_geometry_resolve_pass.hpp"

#include <array>
#include <cstring>

#include "fullscreen.vert.spv.h"
#include "rime/assets/mesh_asset.hpp"
#include "vg_resolve.frag.spv.h"

namespace rime::render {

namespace {

// Mirrors vg_resolve.frag's push block (std430-style push layout: mat4, vec2, then four uints).
struct ResolvePush {
    float mvp[16];
    float viewport[2];
    std::uint32_t cluster_count;
    std::uint32_t stride_words;
    std::uint32_t uv_word;
    std::uint32_t material_count;
};

static_assert(sizeof(ResolvePush) == 88, "ResolvePush must match vg_resolve.frag");

constexpr rhi::Format kOutputFormats[] = {rhi::Format::R32Uint,
                                          rhi::Format::RGBA32Float,
                                          rhi::Format::RGBA8Unorm};

constexpr rhi::BindingDesc kBindings[] = {
    {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment},
    {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment},
    {3, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment},
    {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    {6, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    {7, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
};

// The cooked v1 vertex is position (3 words), normal (3), uv (2); the visibility pass only
// accepts that stride, so the UV sits at word 6. A new vertex layout must revisit this.
constexpr std::uint32_t kMeshV1UvWord = 6;
static_assert(assets::expected_vertex_stride(assets::kMeshV1Attribs) == 32);

} // namespace

VirtualGeometryResolvePass::VirtualGeometryResolvePass(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = fullscreen_vert_spv;
    vs.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    vs.debug_name = "fullscreen.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = vg_resolve_frag_spv;
    fs.spirv_size_bytes = sizeof(vg_resolve_frag_spv);
    fs.debug_name = "vg_resolve.frag";
    fragment_shader_ = device.create_shader(fs);

    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_formats = kOutputFormats;
    pd.cull = rhi::CullMode::None;
    pd.bindings = kBindings;
    pd.push_constant_size = sizeof(ResolvePush);
    pd.debug_name = "vg-resolve";
    pipeline_ = device.create_graphics_pipeline(pd);

    // The ID target is read with texelFetch, which ignores filtering — but an integer image may
    // only ever be bound with a Nearest sampler, so that is what it gets.
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Nearest;
    sd.min_filter = rhi::Filter::Nearest;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.debug_name = "vg-resolve-ids";
    id_sampler_ = device.create_sampler(sd);
}

VirtualGeometryResolvePass::~VirtualGeometryResolvePass() {
    device_.destroy(id_sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

bool VirtualGeometryResolvePass::declare(RenderGraph& graph,
                                         RGTexture visibility,
                                         rhi::Extent2D extent,
                                         RGTexture material_ids,
                                         RGTexture uv,
                                         RGTexture albedo,
                                         const VirtualGeometryResolveRequest& request) {
    const VirtualGeometryClusterBuffers* buffers = request.clusters;
    bool resolve = true;
    if (buffers == nullptr || !buffers->clusters.is_valid() || !buffers->vertices.is_valid() ||
        !buffers->indices.is_valid() || buffers->cluster_count == 0) {
        ++stats_.skipped_no_clusters;
        resolve = false;
    } else {
        bool materials_ok = !request.materials.empty() &&
                            request.materials.size() <= kVirtualGeometryResolveMaxMaterials;
        for (const VirtualGeometryResolveMaterial& m : request.materials) {
            materials_ok = materials_ok && m.albedo.is_valid() && m.sampler.is_valid();
        }
        if (!materials_ok) {
            ++stats_.skipped_bad_materials;
            resolve = false;
        }
    }

    ResolvePush push{};
    std::array<VirtualGeometryResolveMaterial, kVirtualGeometryResolveMaxMaterials> materials{};
    rhi::BufferHandle vb, ib, cb;
    if (resolve) {
        ++stats_.resolved;
        std::memcpy(push.mvp, request.clip_from_object.m, sizeof(push.mvp));
        push.viewport[0] = static_cast<float>(extent.width);
        push.viewport[1] = static_cast<float>(extent.height);
        push.cluster_count = buffers->cluster_count;
        push.stride_words = buffers->vertex_stride_words;
        push.uv_word = kMeshV1UvWord;
        push.material_count = static_cast<std::uint32_t>(request.materials.size());
        // Every declared binding must hold a valid descriptor even if the shader's branch never
        // reaches it, so unused slots alias material 0 (the shader never samples past the count).
        for (std::size_t i = 0; i < materials.size(); ++i) {
            materials[i] = request.materials[i < request.materials.size() ? i : 0];
        }
        vb = buffers->vertices;
        ib = buffers->indices;
        cb = buffers->clusters;
    }

    const RGColorAttachment colors[] = {
        {material_ids, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
        {uv, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
        {albedo, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const RGTexture sampled[] = {visibility};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;

    graph.add_raster_pass(
        "vg-resolve",
        desc,
        [this, &graph, resolve, push, materials, vb, ib, cb, visibility](rhi::CommandBuffer& cmd) {
            if (!resolve) {
                return; // the clears alone are the answer
            }
            cmd.bind_pipeline(pipeline_);
            cmd.bind_texture(0, graph.physical(visibility), id_sampler_);
            cmd.bind_storage_buffer(1, vb);
            cmd.bind_storage_buffer(2, ib);
            cmd.bind_storage_buffer(3, cb);
            for (std::uint32_t i = 0; i < kVirtualGeometryResolveMaxMaterials; ++i) {
                cmd.bind_texture(4 + i, materials[i].albedo, materials[i].sampler);
            }
            cmd.push_constants(&push, sizeof(push));
            cmd.draw(3);
        });
    return resolve;
}

} // namespace rime::render
