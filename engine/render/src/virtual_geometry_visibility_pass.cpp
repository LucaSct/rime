// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18 step-1 visibility pass. See the header for the technique; the notes here are about the
// gate order (every rejection is counted exactly once) and the per-request upload.

#include "rime/render/virtual_geometry_visibility_pass.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "rime/render/passes.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "vg_visibility.frag.spv.h"
#include "vg_visibility.vert.spv.h"

namespace rime::render {

namespace {

// Mirrors the vg_visibility.{vert,frag} push block. Flat float[16] for the same reason as
// ScenePicker's DrawPush: core::Mat4 is alignas(16) and would pad the block past its GLSL size.
struct VisibilityPush {
    float mvp[16];
    std::uint32_t id;
    std::uint32_t index_base;
    std::uint32_t vertex_base;
    std::uint32_t stride_words;
};

static_assert(sizeof(VisibilityPush) == 80, "VisibilityPush must match the shaders");

constexpr rhi::Format kTargetFormats[] = {rhi::Format::R32Uint, rhi::Format::R32Uint};
constexpr rhi::BindingDesc kBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
};

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

rhi::BufferHandle
make_storage(rhi::Device& device, const void* data, std::size_t size, std::string_view name) {
    rhi::BufferDesc bd{};
    bd.size = size;
    bd.usage = rhi::BufferUsage::Storage;
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.initial_data = data;
    bd.debug_name = name;
    return device.create_buffer(bd);
}

// One accepted cluster's draw, recorded at declare time and replayed in the pass body.
struct ClusterDraw {
    VisibilityPush push;
    std::uint32_t index_count;
};

} // namespace

VirtualGeometryVisibilityPass::VirtualGeometryVisibilityPass(rhi::Device& device)
    : device_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = vg_visibility_vert_spv;
    vs.spirv_size_bytes = sizeof(vg_visibility_vert_spv);
    vs.debug_name = "vg_visibility.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = vg_visibility_frag_spv;
    fs.spirv_size_bytes = sizeof(vg_visibility_frag_spv);
    fs.debug_name = "vg_visibility.frag";
    fragment_shader_ = device.create_shader(fs);

    // The picker's visibility decisions (back-face cull, Less depth) so a cluster is "visible"
    // here exactly when the forward pass would have drawn it. No vertex layout: vertex pulling.
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_formats = kTargetFormats;
    pd.cull = rhi::CullMode::Back;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.bindings = kBindings;
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
    for (rhi::BufferHandle* b : {&buffers_.vertices, &buffers_.indices, &buffers_.clusters}) {
        if (b->is_valid()) {
            device_.destroy(*b);
        }
    }
    buffers_ = {};
}

bool VirtualGeometryVisibilityPass::declare(RenderGraph& graph,
                                            RGTexture visibility,
                                            RGTexture depth_bits,
                                            RGTexture depth,
                                            const VirtualGeometryVisibilityRequest& request) {
    release_cluster_buffers();

    const assets::VirtualGeometryAsset* asset = request.asset;
    const bool request_ok =
        asset != nullptr && request.residency != nullptr && request.selection != nullptr &&
        assets::validate_virtual_geometry(*asset) == assets::VirtualGeometryError::None &&
        asset->vertex_stride == assets::expected_vertex_stride(assets::kMeshV1Attribs);
    const std::uint32_t stride_words = request_ok ? asset->vertex_stride / 4u : 0u;

    // Everything accepted is appended to these three host arrays and uploaded once.
    std::vector<std::byte> vertex_bytes;
    std::vector<std::uint32_t> all_indices;
    std::vector<VirtualGeometryGpuCluster> table;
    std::vector<ClusterDraw> draws;

    for (const VirtualGeometryClusterDraw& item : request.clusters) {
        // Gate order matters only for which counter a doubly-bad cluster lands in; each rejection
        // bumps exactly one counter and the cluster is simply not drawn.
        const auto gate = [&]() -> std::uint32_t* {
            if (!request_ok || item.cluster >= asset->clusters.size()) {
                return &stats_.skipped_invalid_request;
            }
            const assets::VirtualGeometryCluster& cluster = asset->clusters[item.cluster];
            const auto& groups = request.selection->groups;
            if (std::find(groups.begin(), groups.end(), cluster.replacement_group) ==
                groups.end()) {
                return &stats_.skipped_not_selected;
            }
            if (asset->groups[cluster.replacement_group].child_count != 0) {
                return &stats_.skipped_not_leaf;
            }
            if (!page_and_dependencies_resident(
                    *asset, request.asset_id, *request.residency, cluster.page)) {
                return &stats_.skipped_not_resident;
            }
            if (!pack_virtual_geometry_visibility_id({item.cluster_slot, item.generation})) {
                return &stats_.skipped_bad_id;
            }
            if (cluster.index_count / 3u > kVirtualGeometryVisibilityV2MaxTriangle + 1u) {
                return &stats_.skipped_too_many_triangles;
            }
            if (item.cluster_slot < table.size() && table[item.cluster_slot].valid != 0) {
                return &stats_.skipped_duplicate_slot;
            }
            return nullptr;
        };

        std::uint32_t* rejected = gate();
        assets::VirtualGeometryPageView view{};
        std::vector<std::uint32_t> indices;
        if (rejected == nullptr) {
            if (assets::view_virtual_geometry_page(*asset, item.cluster, view) !=
                assets::VirtualGeometryPageViewError::None) {
                rejected = &stats_.skipped_page_view;
            } else {
                // Decode indices through the checked reader (page bytes need not be u32-aligned)
                // and reject any index outside the cluster's own vertices, or a partial triangle:
                // the GPU would otherwise read another cluster's (or no) vertex memory, which no
                // validation layer reports for a storage-buffer fetch.
                indices.resize(view.index_count);
                if (view.index_count % 3u != 0) {
                    rejected = &stats_.skipped_bad_index;
                }
                for (std::uint32_t i = 0; i < view.index_count && rejected == nullptr; ++i) {
                    if (!assets::read_virtual_geometry_index(view, i, indices[i]) ||
                        indices[i] >= view.vertex_count) {
                        rejected = &stats_.skipped_bad_index;
                    }
                }
            }
        }
        if (rejected != nullptr) {
            ++*rejected;
            continue;
        }
        ++stats_.drawn;

        // Append: this cluster's vertices start at vertex_base, its indices at index_base, and
        // its indices stay cluster-local — the shaders add vertex_base, so no CPU rebasing.
        const auto vertex_base =
            static_cast<std::uint32_t>(vertex_bytes.size() / view.vertex_stride);
        const auto index_base = static_cast<std::uint32_t>(all_indices.size());
        const std::span<const std::byte> slice = view.vertices.subspan(
            view.vertex_offset, std::size_t{view.vertex_count} * view.vertex_stride);
        vertex_bytes.insert(vertex_bytes.end(), slice.begin(), slice.end());
        all_indices.insert(all_indices.end(), indices.begin(), indices.end());

        if (table.size() <= item.cluster_slot) {
            table.resize(std::size_t{item.cluster_slot} + 1);
        }
        table[item.cluster_slot] = {vertex_base,
                                    index_base,
                                    view.index_count / 3u,
                                    asset->clusters[item.cluster].material_slot,
                                    item.generation,
                                    1u,
                                    {}};

        ClusterDraw draw{};
        std::memcpy(draw.push.mvp, request.clip_from_object.m, sizeof(draw.push.mvp));
        draw.push.id = *pack_virtual_geometry_visibility_id({item.cluster_slot, item.generation});
        draw.push.index_base = index_base;
        draw.push.vertex_base = vertex_base;
        draw.push.stride_words = stride_words;
        draw.index_count = view.index_count;
        draws.push_back(draw);
    }

    if (!draws.empty()) {
        buffers_.vertices =
            make_storage(device_, vertex_bytes.data(), vertex_bytes.size(), "vg-cluster-vertices");
        buffers_.indices = make_storage(device_,
                                        all_indices.data(),
                                        all_indices.size() * sizeof(std::uint32_t),
                                        "vg-cluster-indices");
        buffers_.clusters = make_storage(device_,
                                         table.data(),
                                         table.size() * sizeof(VirtualGeometryGpuCluster),
                                         "vg-cluster-table");
        buffers_.cluster_count = static_cast<std::uint32_t>(table.size());
        buffers_.vertex_stride_words = stride_words;
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

    graph.add_raster_pass(
        "vg-visibility",
        desc,
        [this, draws = std::move(draws), vb = buffers_.vertices, ib = buffers_.indices](
            rhi::CommandBuffer& cmd) {
            if (draws.empty()) {
                return; // the attachment clears alone are "nothing visible"
            }
            cmd.bind_pipeline(pipeline_);
            cmd.bind_storage_buffer(0, vb);
            cmd.bind_storage_buffer(1, ib);
            for (const ClusterDraw& d : draws) {
                cmd.push_constants(&d.push, sizeof(d.push));
                cmd.draw(d.index_count); // one invocation per triangle corner
            }
        });
    return buffers_.clusters.is_valid();
}

} // namespace rime::render
