// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18 step-1 visibility pass. See the header for the technique; the notes here are about the
// gate order (every rejection is counted exactly once) and the per-request upload.

#include "rime/render/virtual_geometry_visibility_pass.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

#include "rime/render/passes.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "vg_visibility.frag.spv.h"
#include "vg_visibility.vert.spv.h"

namespace rime::render {

namespace {

// Mirrors the vg_visibility.{vert,frag} push block. The MVP is the only value that is the same
// across every cluster in a request; per-cluster data moved to VirtualGeometryDrawRecord.
struct VisibilityPush {
    float mvp[16];
};

static_assert(sizeof(VisibilityPush) == 64, "VisibilityPush must match the shaders");

constexpr rhi::Format kTargetFormats[] = {rhi::Format::RG32Uint, rhi::Format::R32Uint};
constexpr rhi::BindingDesc kBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
    {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Vertex},
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
    VirtualGeometryDrawRecord record;
    std::uint32_t index_count = 0;
};

// Vulkan VkDrawIndexedIndirectCommand layout, mirrored here so the CPU can fill the buffer.
struct IndexedIndirectCommand {
    std::uint32_t index_count;
    std::uint32_t instance_count;
    std::uint32_t first_index;
    std::int32_t vertex_offset;
    std::uint32_t first_instance;
};

static_assert(sizeof(IndexedIndirectCommand) == 20, "must match VkDrawIndexedIndirectCommand");

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
    for (rhi::BufferHandle* b : {&buffers_.vertices,
                                 &buffers_.indices,
                                 &buffers_.clusters,
                                 &buffers_.records,
                                 &buffers_.indirect,
                                 &buffers_.identity_index}) {
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
            if (!pack_virtual_geometry_visibility_id64({item.cluster_slot, item.generation})) {
                return &stats_.skipped_bad_id;
            }
            if (cluster.index_count / 3u > kVirtualGeometryVisibilityV3MaxTriangle + 1u) {
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
        if (draws.size() >= kVirtualGeometryMaxIndirectDraws) {
            ++stats_.skipped_over_capacity;
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
        const VirtualGeometryVisibilityWords id =
            *pack_virtual_geometry_visibility_id64({item.cluster_slot, item.generation});
        draw.record.id_lo = id.lo;
        draw.record.id_hi = id.hi;
        draw.record.index_base = index_base;
        draw.record.vertex_base = vertex_base;
        draw.record.stride_words = stride_words;
        draw.record.cluster_slot = item.cluster_slot;
        draw.index_count = view.index_count;
        draws.push_back(draw);
    }

    if (!draws.empty()) {
        // Identity index buffer trick: we want gl_VertexIndex to keep naming the merged cluster
        // index so the vertex shader can keep pulling from storage. By binding an identity
        // sequence 0,1,2,...,total_indices-1 and setting first_index to each cluster's index_base,
        // gl_VertexIndex becomes the global cluster index. This avoids gl_PrimitiveID, which would
        // need the geometryShader feature and is not available on MoltenVK.
        std::vector<std::uint32_t> identity(all_indices.size());
        std::iota(identity.begin(), identity.end(), 0u);

        std::vector<VirtualGeometryDrawRecord> records;
        records.reserve(draws.size());
        for (const ClusterDraw& d : draws) {
            records.push_back(d.record);
        }

        // Fixed-capacity indirect buffer. A later brick will build these commands on the GPU;
        // keeping the CPU draw_count constant lets a GPU-decided visible count reach the draw
        // without a same-frame readback. Entries past the accepted clusters are zero-initialized,
        // so their instance_count is 0 and they are legal no-ops.
        std::vector<IndexedIndirectCommand> indirect_cmds(kVirtualGeometryMaxIndirectDraws);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(draws.size()); ++i) {
            indirect_cmds[i] = {draws[i].index_count, 1u, draws[i].record.index_base, 0, 0};
        }

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
        buffers_.records = make_storage(device_,
                                        records.data(),
                                        records.size() * sizeof(VirtualGeometryDrawRecord),
                                        "vg-draw-records");

        rhi::BufferDesc id_desc{};
        id_desc.size = identity.size() * sizeof(std::uint32_t);
        id_desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        id_desc.memory = rhi::MemoryUsage::CpuToGpu;
        id_desc.initial_data = identity.data();
        id_desc.debug_name = "vg-identity-index";
        buffers_.identity_index = device_.create_buffer(id_desc);

        rhi::BufferDesc ind_desc{};
        ind_desc.size = kVirtualGeometryMaxIndirectDraws * sizeof(IndexedIndirectCommand);
        ind_desc.usage = rhi::BufferUsage::Indirect;
        ind_desc.memory = rhi::MemoryUsage::CpuToGpu;
        ind_desc.initial_data = indirect_cmds.data();
        ind_desc.debug_name = "vg-indirect-commands";
        buffers_.indirect = device_.create_buffer(ind_desc);

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

    VisibilityPush push{};
    std::memcpy(push.mvp, request.clip_from_object.m, sizeof(push.mvp));
    const RGBuffer indirect_rg =
        draws.empty() ? RGBuffer{}
                      : graph.import_buffer(buffers_.indirect, rhi::ResourceState::IndirectRead);
    const RGBuffer indirect_reads[] = {indirect_rg};
    if (!draws.empty()) {
        desc.indirect_reads = indirect_reads;
    }

    graph.add_raster_pass("vg-visibility",
                          desc,
                          [this,
                           draws = std::move(draws),
                           push,
                           vb = buffers_.vertices,
                           ib = buffers_.indices,
                           rb = buffers_.records,
                           identity = buffers_.identity_index,
                           indirect = buffers_.indirect](rhi::CommandBuffer& cmd) {
                              if (draws.empty()) {
                                  return; // the attachment clears alone are "nothing visible"
                              }
                              cmd.bind_pipeline(pipeline_);
                              cmd.bind_storage_buffer(0, vb);
                              cmd.bind_storage_buffer(1, ib);
                              cmd.bind_storage_buffer(2, rb);
                              cmd.bind_index_buffer(identity, rhi::IndexType::Uint32);
                              cmd.push_constants(&push, sizeof(push));
                              cmd.draw_indexed_indirect(indirect, kVirtualGeometryMaxIndirectDraws);
                          });
    return buffers_.clusters.is_valid();
}

} // namespace rime::render
