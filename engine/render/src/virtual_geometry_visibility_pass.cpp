// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18 step-1 visibility pass. See the header for the technique; the notes here are about the
// gate order (every rejection is counted exactly once) and the per-cluster upload.

#include "rime/render/virtual_geometry_visibility_pass.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "pick_id.vert.spv.h"
#include "rime/render/passes.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "vg_visibility.frag.spv.h"

namespace rime::render {

namespace {

// Mirrors the pick_id.vert / vg_visibility.frag push block. Flat float[16] for the same reason as
// ScenePicker's DrawPush: core::Mat4 is alignas(16) and would pad the block past its GLSL size.
struct VisibilityPush {
    float mvp[16];
    std::uint32_t id;
};

static_assert(sizeof(VisibilityPush) == 68, "VisibilityPush must match the shaders");

// Only the position is consumed; like the depth pre-pass, the pipeline reads the full interleaved
// vertex at the cooked stride and ignores the rest. Position is always first in the cooked layout
// (validate_virtual_geometry requires the Position bit, and attributes are written in bit order).
constexpr rhi::VertexAttribute kPositionOnly[] = {{0, rhi::Format::RGB32Float, 0}};
constexpr rhi::Format kTargetFormats[] = {rhi::Format::R32Uint, rhi::Format::R32Uint};

// Residency is transitive: a page is drawable only if it and every page it depends on is resident
// (the same rule select_virtual_geometry applies). Permanent pages are resident by contract.
bool page_and_dependencies_resident(const assets::VirtualGeometryAsset& asset,
                                    assets::AssetId id,
                                    const VirtualGeometryResidency& residency,
                                    std::uint32_t page_index) {
    std::vector<std::uint32_t> pending{page_index};
    std::uint32_t visited = 0;
    while (!pending.empty()) {
        // validate_virtual_geometry rejects dependency cycles; the bound is belt-and-braces.
        if (++visited > asset.pages.size() * 4 + 4) {
            return false;
        }
        const std::uint32_t current = pending.back();
        pending.pop_back();
        const assets::VirtualGeometryPage& page = asset.pages[current];
        if (!page.permanently_resident && !residency.is_resident(id, current)) {
            return false;
        }
        for (std::uint32_t i = 0; i < page.dependency_count; ++i) {
            pending.push_back(asset.page_dependencies[page.first_dependency + i]);
        }
    }
    return true;
}

} // namespace

VirtualGeometryVisibilityPass::VirtualGeometryVisibilityPass(rhi::Device& device)
    : device_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = pick_id_vert_spv;
    vs.spirv_size_bytes = sizeof(pick_id_vert_spv);
    vs.debug_name = "pick_id.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = vg_visibility_frag_spv;
    fs.spirv_size_bytes = sizeof(vg_visibility_frag_spv);
    fs.debug_name = "vg_visibility.frag";
    fragment_shader_ = device.create_shader(fs);

    // The picker's visibility decisions (back-face cull, Less depth) so a cluster is "visible"
    // here exactly when the forward pass would have drawn it.
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.vertex_layout.stride = assets::expected_vertex_stride(assets::kMeshV1Attribs);
    pd.vertex_layout.attributes = kPositionOnly;
    pd.color_formats = kTargetFormats;
    pd.cull = rhi::CullMode::Back;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.push_constant_size = sizeof(VisibilityPush);
    pd.debug_name = "vg-visibility";
    pipeline_ = device.create_graphics_pipeline(pd);
}

VirtualGeometryVisibilityPass::~VirtualGeometryVisibilityPass() {
    release_cluster_buffers();
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void VirtualGeometryVisibilityPass::release_cluster_buffers() noexcept {
    if (vertices_.is_valid()) {
        device_.destroy(vertices_);
    }
    if (indices_.is_valid()) {
        device_.destroy(indices_);
    }
    vertices_ = {};
    indices_ = {};
}

bool VirtualGeometryVisibilityPass::declare(RenderGraph& graph,
                                            RGTexture visibility,
                                            RGTexture depth_bits,
                                            RGTexture depth,
                                            const VirtualGeometryVisibilityRequest& request) {
    release_cluster_buffers();

    // Gate order matters only for which counter a doubly-bad request lands in; each rejection
    // bumps exactly one counter and falls through to the clear-only pass below.
    const auto gate = [&]() -> std::uint32_t* {
        const assets::VirtualGeometryAsset* asset = request.asset;
        if (asset == nullptr || request.residency == nullptr || request.selection == nullptr ||
            assets::validate_virtual_geometry(*asset) != assets::VirtualGeometryError::None ||
            asset->vertex_stride != assets::expected_vertex_stride(assets::kMeshV1Attribs) ||
            request.cluster >= asset->clusters.size()) {
            return &stats_.skipped_invalid_request;
        }
        const assets::VirtualGeometryCluster& cluster = asset->clusters[request.cluster];
        const auto& groups = request.selection->groups;
        if (std::find(groups.begin(), groups.end(), cluster.replacement_group) == groups.end()) {
            return &stats_.skipped_not_selected;
        }
        if (asset->groups[cluster.replacement_group].child_count != 0) {
            return &stats_.skipped_not_leaf;
        }
        if (!page_and_dependencies_resident(
                *asset, request.asset_id, *request.residency, cluster.page)) {
            return &stats_.skipped_not_resident;
        }
        if (!pack_virtual_geometry_visibility_id(
                {request.cluster_slot, request.generation, kVirtualGeometryVisibilityMinVersion})) {
            return &stats_.skipped_bad_id;
        }
        return nullptr;
    };

    std::uint32_t* rejected = gate();
    VisibilityPush push{};
    std::uint32_t index_count = 0;
    if (rejected == nullptr) {
        assets::VirtualGeometryPageView view{};
        if (assets::view_virtual_geometry_page(*request.asset, request.cluster, view) !=
            assets::VirtualGeometryPageViewError::None) {
            rejected = &stats_.skipped_page_view;
        } else {
            // Decode indices through the checked reader (page bytes need not be u32-aligned) and
            // reject any index outside the cluster's own vertices: the GPU would otherwise read
            // another cluster's (or no) vertex memory, which no validation layer reports.
            std::vector<std::uint32_t> indices(view.index_count);
            for (std::uint32_t i = 0; i < view.index_count && rejected == nullptr; ++i) {
                if (!assets::read_virtual_geometry_index(view, i, indices[i]) ||
                    indices[i] >= view.vertex_count) {
                    rejected = &stats_.skipped_bad_index;
                }
            }
            if (rejected == nullptr) {
                // Upload just this cluster: its vertex slice becomes vertex 0.., so its
                // cluster-local indices need no base-vertex rebasing.
                const std::span<const std::byte> vertex_bytes = view.vertices.subspan(
                    view.vertex_offset, std::size_t{view.vertex_count} * view.vertex_stride);
                rhi::BufferDesc vbd{};
                vbd.size = vertex_bytes.size();
                vbd.usage = rhi::BufferUsage::Vertex;
                vbd.memory = rhi::MemoryUsage::CpuToGpu;
                vbd.initial_data = vertex_bytes.data();
                vbd.debug_name = "vg-visibility-vertices";
                vertices_ = device_.create_buffer(vbd);

                rhi::BufferDesc ibd{};
                ibd.size = indices.size() * sizeof(std::uint32_t);
                ibd.usage = rhi::BufferUsage::Index;
                ibd.memory = rhi::MemoryUsage::CpuToGpu;
                ibd.initial_data = indices.data();
                ibd.debug_name = "vg-visibility-indices";
                indices_ = device_.create_buffer(ibd);

                std::memcpy(push.mvp, request.clip_from_object.m, sizeof(push.mvp));
                push.id =
                    *pack_virtual_geometry_visibility_id({request.cluster_slot,
                                                          request.generation,
                                                          kVirtualGeometryVisibilityMinVersion});
                index_count = view.index_count;
            }
        }
    }
    if (rejected != nullptr) {
        ++*rejected;
    } else {
        ++stats_.drawn;
    }

    // Both integer targets clear to all-zero bits: the invalid visibility ID and +0.0 depth.
    const RGColorAttachment colors[] = {
        {visibility, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
        {depth_bits, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const RGDepthAttachment depth_att{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, 0, false};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;

    const bool draw = rejected == nullptr;
    graph.add_raster_pass(
        "vg-visibility",
        desc,
        [this, draw, push, index_count, vb = vertices_, ib = indices_](rhi::CommandBuffer& cmd) {
            if (!draw) {
                return; // the attachment clears alone are the "nothing visible" answer
            }
            cmd.bind_pipeline(pipeline_);
            cmd.bind_vertex_buffer(vb);
            cmd.bind_index_buffer(ib, rhi::IndexType::Uint32);
            cmd.push_constants(&push, sizeof(push));
            cmd.draw_indexed(index_count);
        });
    return draw;
}

} // namespace rime::render
