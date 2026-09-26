// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18 step-1 visibility pass. See the header for the technique; the notes here are about the
// gate order (every rejection is counted exactly once) and the per-request upload.

#include "rime/render/virtual_geometry_visibility_pass.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <span>
#include <vector>

#include "rime/render/passes.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "vg_build_draws.comp.spv.h"
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

// Mirrors the PushConstants block in vg_build_draws.comp.
struct BuildPush {
    std::uint32_t candidate_count = 0;
    std::uint32_t group_count = 0;
    std::uint32_t capacity = 0;
    std::uint32_t max_draws = 0;
};

constexpr std::uint32_t kBuildGroupSize = 64;

constexpr rhi::BindingDesc kBuildBindings[] = {
    {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // candidates
    {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // selection flags
    {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // draw records out
    {3, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // indirect commands out
    {4, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // build counters out
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

// One accepted cluster's draw, recorded at declare time and replayed in the pass body. `group` and
// `coarse` are only consulted on the GPU-built path, where they become the candidate's gate.
struct ClusterDraw {
    VirtualGeometryDrawRecord record;
    std::uint32_t index_count = 0;
    std::uint32_t group = 0;
    bool coarse = false;
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

    rhi::ShaderDesc cs{};
    cs.stage = rhi::ShaderStage::Compute;
    cs.spirv = vg_build_draws_comp_spv;
    cs.spirv_size_bytes = sizeof(vg_build_draws_comp_spv);
    cs.debug_name = "vg_build_draws.comp";
    build_shader_ = device.create_shader(cs);

    rhi::ComputePipelineDesc bd{};
    bd.shader = build_shader_;
    bd.bindings = kBuildBindings;
    bd.push_constant_size = sizeof(BuildPush);
    bd.debug_name = "vg-build-draws";
    build_pipeline_ = device.create_compute_pipeline(bd);
}

VirtualGeometryVisibilityPass::~VirtualGeometryVisibilityPass() {
    release_cluster_buffers();
    device_.destroy(build_pipeline_);
    device_.destroy(build_shader_);
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
                                 &buffers_.identity_index,
                                 &buffers_.candidates,
                                 &buffers_.build_counters}) {
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

    // Refuse loudly rather than draw a truncated cut. The single indexed-indirect submission below
    // needs multiDrawIndirect (or a driver issues only the first command) and shaderDrawParameters
    // (or gl_DrawID does not resolve); AdapterInfo::gpu_driven_draw is both, and the RHI already
    // warned once at device creation. Refusing the whole request — rather than each cluster for a
    // reason that is not the cluster's — keeps every skip counter's meaning honest.
    const std::span<const VirtualGeometryClusterDraw> gated_clusters =
        device_.adapter().gpu_driven_draw ? request.clusters
                                          : std::span<const VirtualGeometryClusterDraw>{};
    if (gated_clusters.empty() && !request.clusters.empty()) {
        stats_.skipped_no_gpu_driven_draw += static_cast<std::uint32_t>(request.clusters.size());
    }

    // GPU-built submission (M18.3c). A verdict buffer that is present but invalid is refused rather
    // than silently downgraded to CPU gating — see skipped_no_gpu_verdict.
    const bool verdict_missing =
        request.gpu_selection != nullptr && !request.gpu_selection->selected_flags.is_valid();
    if (verdict_missing && !gated_clusters.empty()) {
        stats_.skipped_no_gpu_verdict += static_cast<std::uint32_t>(gated_clusters.size());
    }
    const bool gpu_build = request_ok && !verdict_missing && request.gpu_selection != nullptr;
    const std::span<const VirtualGeometryClusterDraw> accepted_clusters =
        verdict_missing ? std::span<const VirtualGeometryClusterDraw>{} : gated_clusters;
    const std::uint32_t draw_capacity =
        request.max_draws == 0 ? kVirtualGeometryMaxIndirectDraws
                               : std::min(request.max_draws, kVirtualGeometryMaxIndirectDraws);
    // The CPU path can only accept what it can submit; the GPU path uploads candidates and lets the
    // builder apply draw_capacity, so its ceiling is the candidate ceiling instead.
    const std::uint32_t cpu_accept_limit =
        gpu_build ? kVirtualGeometryMaxDrawCandidates : draw_capacity;

    for (const VirtualGeometryClusterDraw& item : accepted_clusters) {
        // Gate order matters only for which counter a doubly-bad cluster lands in; each rejection
        // bumps exactly one counter and the cluster is simply not drawn.
        const auto gate = [&]() -> std::uint32_t* {
            if (!request_ok || item.cluster >= asset->clusters.size()) {
                return &stats_.skipped_invalid_request;
            }
            const assets::VirtualGeometryCluster& cluster = asset->clusters[item.cluster];
            const bool is_coarse = cluster.replacement_group == asset->coarse_group;
            // On the GPU path this gate belongs to vg_build_draws.comp: the cut lives in GPU memory
            // and asking the CPU's copy here would reintroduce the readback the brick removes.
            if (!gpu_build) {
                const auto& groups = request.selection->groups;
                if (std::find(groups.begin(), groups.end(), cluster.replacement_group) ==
                    groups.end()) {
                    return &stats_.skipped_not_selected;
                }
            }
            // This pass still draws leaf clusters only (see the header) — with one exception on the
            // GPU path: the coarse group's clusters are the overflow fallback, and the overflow is
            // not known until the builder has run, so they must already be uploaded. The coarse
            // group has children whenever the asset has more than one LOD, so without this the
            // fallback could never be drawn.
            if (asset->groups[cluster.replacement_group].child_count != 0 &&
                !(gpu_build && is_coarse)) {
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
        if (draws.size() >= cpu_accept_limit) {
            ++(gpu_build ? stats_.skipped_over_candidate_cap : stats_.skipped_over_capacity);
            continue;
        }
        if (gpu_build) {
            ++stats_.candidates_offered;
        } else {
            ++stats_.drawn;
        }

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
        draw.group = asset->clusters[item.cluster].replacement_group;
        draw.coarse = draw.group == asset->coarse_group;
        draws.push_back(draw);
    }

    if (!draws.empty()) {
        // Identity index buffer trick: we want gl_VertexIndex to keep naming the merged cluster
        // index so the vertex shader can keep pulling from storage. By binding an identity
        // sequence 0,1,2,...,total_indices-1 and setting first_index to each cluster's index_base,
        // gl_VertexIndex becomes the global cluster index. This avoids gl_PrimitiveID, which would
        // need the geometryShader feature and is not available on MoltenVK.
        //
        // Known cost, deliberately unoptimized: this buffer's contents depend only on its LENGTH,
        // so rebuilding and re-uploading it per declare() is pure redundancy (~1.5 MB at the 1024-
        // draw capacity). It becomes a grow-only persistent buffer when the next brick moves the
        // command build onto the GPU and the per-frame upload disappears anyway; optimizing it
        // before that measurement would be guessing.
        std::vector<std::uint32_t> identity(all_indices.size());
        std::iota(identity.begin(), identity.end(), 0u);

        std::vector<VirtualGeometryDrawRecord> records;
        records.reserve(draws.size());
        for (const ClusterDraw& d : draws) {
            records.push_back(d.record);
        }

        // On the GPU path the records and the commands are OUTPUTS of the builder, so they are
        // allocated at full capacity and zeroed. Zeroed matters: a record the builder does not
        // write must read as an all-zero visibility ID rather than as whatever the last frame left
        // there.
        std::vector<VirtualGeometryDrawCandidate> candidates;
        if (gpu_build) {
            candidates.reserve(draws.size());
            for (const ClusterDraw& d : draws) {
                VirtualGeometryDrawCandidate c{};
                c.record = d.record;
                c.index_count = d.index_count;
                c.group = d.group;
                c.coarse = d.coarse ? 1u : 0u;
                candidates.push_back(c);
            }
            records.assign(kVirtualGeometryMaxIndirectDraws, VirtualGeometryDrawRecord{});
        }

        // Fixed-capacity indirect buffer. A later brick will build these commands on the GPU;
        // keeping the CPU draw_count constant lets a GPU-decided visible count reach the draw
        // without a same-frame readback. Entries past the accepted clusters are zero-initialized,
        // so their instance_count is 0 and they are legal no-ops.
        std::vector<IndexedIndirectCommand> indirect_cmds(kVirtualGeometryMaxIndirectDraws);
        if (!gpu_build) {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(draws.size()); ++i) {
                indirect_cmds[i] = {draws[i].index_count, 1u, draws[i].record.index_base, 0, 0};
            }
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
        // The records are host-written on the CPU path and a compute output on the GPU path, so
        // their memory follows: write-combined host-visible for a CPU upload, device-local for a
        // buffer the GPU writes every frame and the CPU only seeds with zeroes.
        rhi::BufferDesc rec_desc{};
        rec_desc.size = records.size() * sizeof(VirtualGeometryDrawRecord);
        rec_desc.usage = rhi::BufferUsage::Storage;
        rec_desc.memory = gpu_build ? rhi::MemoryUsage::GpuOnly : rhi::MemoryUsage::CpuToGpu;
        rec_desc.initial_data = records.data();
        rec_desc.debug_name = "vg-draw-records";
        buffers_.records = device_.create_buffer(rec_desc);

        rhi::BufferDesc id_desc{};
        id_desc.size = identity.size() * sizeof(std::uint32_t);
        id_desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        id_desc.memory = rhi::MemoryUsage::CpuToGpu;
        id_desc.initial_data = identity.data();
        id_desc.debug_name = "vg-identity-index";
        buffers_.identity_index = device_.create_buffer(id_desc);

        rhi::BufferDesc ind_desc{};
        ind_desc.size = kVirtualGeometryMaxIndirectDraws * sizeof(IndexedIndirectCommand);
        // Storage as well as Indirect on the GPU path: the same bytes are a compute shader's output
        // and the command processor's input, which is the whole trick of GPU-driven submission.
        ind_desc.usage = gpu_build ? (rhi::BufferUsage::Indirect | rhi::BufferUsage::Storage)
                                   : rhi::BufferUsage::Indirect;
        ind_desc.memory = gpu_build ? rhi::MemoryUsage::GpuOnly : rhi::MemoryUsage::CpuToGpu;
        ind_desc.initial_data = indirect_cmds.data();
        ind_desc.debug_name = "vg-indirect-commands";
        buffers_.indirect = device_.create_buffer(ind_desc);

        if (gpu_build) {
            buffers_.candidates =
                make_storage(device_,
                             candidates.data(),
                             candidates.size() * sizeof(VirtualGeometryDrawCandidate),
                             "vg-draw-candidates");
            const VirtualGeometryGpuBuildCounters zero_counters{};
            // GpuToCpu so a test can read what the builder decided. The frame path never does: the
            // point of the brick is that no readback stands between the verdict and the draw.
            rhi::BufferDesc cd{};
            cd.size = sizeof(VirtualGeometryGpuBuildCounters);
            cd.usage = rhi::BufferUsage::Storage;
            cd.memory = rhi::MemoryUsage::GpuToCpu;
            cd.initial_data = &zero_counters;
            cd.debug_name = "vg-build-counters";
            buffers_.build_counters = device_.create_buffer(cd);
            buffers_.candidate_count = static_cast<std::uint32_t>(candidates.size());
        }

        buffers_.cluster_count = static_cast<std::uint32_t>(table.size());
        buffers_.vertex_stride_words = stride_words;
    }
    stats_.gpu_built = (gpu_build && !draws.empty()) ? 1u : 0u;

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
    // The commands enter the graph in the state they are actually in. On the CPU path the host
    // wrote them, so they are ready for the command processor; on the GPU path they are still
    // zeroes waiting for a dispatch, and the graph derives the ShaderWrite -> IndirectRead barrier
    // between the two passes below. Getting this wrong is invisible on a desktop driver and wrong
    // on a tiler, which is why it is declared rather than assumed.
    const RGBuffer indirect_rg =
        draws.empty()
            ? RGBuffer{}
            : graph.import_buffer(buffers_.indirect,
                                  stats_.gpu_built != 0u ? rhi::ResourceState::ShaderRead
                                                         : rhi::ResourceState::IndirectRead);
    const RGBuffer records_rg =
        draws.empty() ? RGBuffer{}
                      : graph.import_buffer(buffers_.records, rhi::ResourceState::ShaderRead);
    const RGBuffer indirect_reads[] = {indirect_rg};
    const RGBuffer raster_buffer_reads[] = {records_rg};
    if (!draws.empty()) {
        desc.indirect_reads = indirect_reads;
        // The records are a compute output on the GPU path, so the vertex stage's read of them is
        // an edge the graph must see; on the CPU path declaring it costs one redundant barrier and
        // keeps a single code path.
        desc.buffer_reads = raster_buffer_reads;
    }

    if (stats_.gpu_built != 0u) {
        const RGBuffer candidates_rg =
            graph.import_buffer(buffers_.candidates, rhi::ResourceState::ShaderRead);
        const RGBuffer flags_rg = graph.import_buffer(request.gpu_selection->selected_flags,
                                                      rhi::ResourceState::ShaderRead);
        const RGBuffer counters_rg =
            graph.import_buffer(buffers_.build_counters, rhi::ResourceState::ShaderRead);
        const RGBuffer build_reads[] = {candidates_rg, flags_rg};
        const RGBuffer build_writes[] = {records_rg, indirect_rg, counters_rg};
        RenderGraph::ComputePassDesc build{};
        build.buffer_reads = build_reads;
        build.buffer_writes = build_writes;
        // Keep the counters readable after execute(): they are the only witness to what the builder
        // chose, and a proof that cannot see what was skipped still reads as passing.
        graph.export_buffer(counters_rg);

        BuildPush bp{};
        bp.candidate_count = buffers_.candidate_count;
        bp.group_count = request.gpu_selection->group_count;
        bp.capacity = draw_capacity;
        bp.max_draws = kVirtualGeometryMaxIndirectDraws;
        // Every slot of the command buffer needs an invocation, not just every candidate: the tail
        // past the emitted list is zeroed by the same dispatch (see the shader).
        const std::uint32_t threads = std::max(bp.candidate_count, bp.max_draws);
        graph.add_compute_pass("vg-build-draws",
                               build,
                               [pipe = build_pipeline_,
                                bp,
                                threads,
                                &graph,
                                candidates_rg,
                                flags_rg,
                                records_rg,
                                indirect_rg,
                                counters_rg](rhi::CommandBuffer& cmd) {
                                   cmd.bind_compute_pipeline(pipe);
                                   cmd.bind_storage_buffer(0, graph.physical_buffer(candidates_rg));
                                   cmd.bind_storage_buffer(1, graph.physical_buffer(flags_rg));
                                   cmd.bind_storage_buffer(2, graph.physical_buffer(records_rg));
                                   cmd.bind_storage_buffer(3, graph.physical_buffer(indirect_rg));
                                   cmd.bind_storage_buffer(4, graph.physical_buffer(counters_rg));
                                   cmd.push_constants(&bp, sizeof(bp));
                                   cmd.dispatch(
                                       (threads + kBuildGroupSize - 1) / kBuildGroupSize, 1, 1);
                               });
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
