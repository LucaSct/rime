// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// CPU half of the flat parallel virtual-geometry selection compute pass (M18.3 slice A).
// Validation, parent derivation, and residency packing happen here; the GPU only sees immutable
// asset tables and frame constants. Invalid inputs are rejected before dispatch, mirroring the
// CPU oracle exactly.

#include "rime/render/virtual_geometry_gpu_selection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "rime/core/diagnostics/log.hpp"
#include "vg_select.comp.spv.h"

namespace rime::render {

namespace {

constexpr std::uint32_t kWorkgroupSize = 64;

static_assert(kVirtualGeometryGpuSelectionMaxDepth == 256u);

struct GpuGroup {
    std::uint32_t first_cluster = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t first_child = 0;
    std::uint32_t child_count = 0;
    float lod_error_m = 0.0f;
};

struct GpuPage {
    std::uint32_t first_dependency = 0;
    std::uint32_t dependency_count = 0;
};

struct PushConstants {
    float pixels_per_metre = 0.0f;
    float max_projected_error_px = 0.0f;
    std::uint32_t group_count = 0;
    std::uint32_t coarse_group = 0;
};

// Longest path length, in groups, from any source of the child DAG. This is a conservative bound on
// the ancestry chain any shader invocation will try to walk, because the GPU follows the first
// parent it finds and that parent must be at least as close to a source as the coarse root.
std::uint32_t longest_group_chain(const assets::VirtualGeometryAsset& asset) {
    const std::uint32_t group_count = static_cast<std::uint32_t>(asset.groups.size());
    std::vector<std::vector<std::uint32_t>> children(group_count);
    std::vector<std::uint32_t> in_degree(group_count, 0u);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        const assets::VirtualGeometryGroup& group = asset.groups[g];
        for (std::uint32_t i = 0; i < group.child_count; ++i) {
            const std::uint32_t child = asset.child_groups[group.first_child + i];
            if (child < group_count) {
                children[g].push_back(child);
                ++in_degree[child];
            }
        }
    }

    std::vector<std::uint32_t> queue;
    queue.reserve(group_count);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        if (in_degree[g] == 0u)
            queue.push_back(g);
    }

    std::vector<std::uint32_t> depth(group_count, 1u);
    std::uint32_t max_depth = group_count == 0u ? 0u : 1u;
    std::size_t head = 0;
    while (head < queue.size()) {
        const std::uint32_t g = queue[head++];
        for (const std::uint32_t child : children[g]) {
            if (depth[child] < depth[g] + 1u)
                depth[child] = depth[g] + 1u;
            if (max_depth < depth[child])
                max_depth = depth[child];
            if (--in_degree[child] == 0u)
                queue.push_back(child);
        }
    }

    // A cycle is impossible here because validation rejects it, but if the graph somehow became
    // malformed we would rather fall back than dispatch an unbounded shader traversal.
    if (queue.size() < group_count)
        return std::numeric_limits<std::uint32_t>::max();
    return max_depth;
}

} // namespace

VirtualGeometrySelection
select_virtual_geometry_on_gpu(rhi::Device& device,
                               const assets::VirtualGeometryAsset& asset,
                               const VirtualGeometrySelectionInput& input) {
    VirtualGeometrySelection out;

    // Identical validation to the CPU oracle: reject bad assets, non-finite / negative camera
    // scales, and residency spans that do not match the page count.
    if (assets::validate_virtual_geometry(asset) != assets::VirtualGeometryError::None ||
        !std::isfinite(input.pixels_per_metre) || input.pixels_per_metre < 0.0f ||
        !std::isfinite(input.max_projected_error_px) || input.max_projected_error_px < 0.0f ||
        (!input.page_resident.empty() && input.page_resident.size() != asset.pages.size())) {
        out.rejected_invalid_input = 1;
        return out;
    }

    // Conservative pre-check: the shader uses fixed-size arrays of size
    // kVirtualGeometryGpuSelectionMaxDepth for the parent chain and the dependency walk. If the
    // asset needs more space than the shader has, fall back to the CPU oracle rather than risk
    // silent divergence.
    const std::uint32_t group_chain_depth = longest_group_chain(asset);
    // The dependency walk pushes each edge of a page's dependency DAG onto its explicit stack. The
    // worst-case live stack is bounded by the total number of dependency edges plus the starting
    // page; if that fits, the walk can never run out of room.
    const std::uint32_t dependency_stack_bound =
        static_cast<std::uint32_t>(asset.page_dependencies.size()) + 1u;
    if (group_chain_depth > kVirtualGeometryGpuSelectionMaxDepth ||
        dependency_stack_bound > kVirtualGeometryGpuSelectionMaxDepth) {
        out = select_virtual_geometry(asset, input);
        out.gpu_depth_fallback = 1;
        // The CPU oracle never touches the GPU counters, so an overflow witness is zero here.
        out.gpu_depth_overflow = 0;
        return out;
    }

    const std::uint32_t group_count = static_cast<std::uint32_t>(asset.groups.size());
    const std::uint32_t cluster_count = static_cast<std::uint32_t>(asset.clusters.size());
    const std::uint32_t page_count = static_cast<std::uint32_t>(asset.pages.size());

    // Parent index per group, derived from child_groups. The coarse root keeps the sentinel
    // 0xffffffff; every other group has exactly one parent for this flat ancestor-chain walk.
    // Shared children are not expected in the proof asset; the first parent encountered wins.
    std::vector<std::uint32_t> parents(group_count, 0xffffffffu);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        const assets::VirtualGeometryGroup& group = asset.groups[g];
        for (std::uint32_t c = 0; c < group.child_count; ++c) {
            const std::uint32_t child = asset.child_groups[group.first_child + c];
            if (child < group_count && parents[child] == 0xffffffffu) {
                parents[child] = g;
            }
        }
    }

    // page_ready[p] mirrors the CPU oracle's page_ready lambda: permanent pages are always
    // resident, and transient pages are resident when the uploaded byte is non-zero.
    std::vector<std::uint32_t> page_ready(page_count);
    for (std::uint32_t p = 0; p < page_count; ++p) {
        const assets::VirtualGeometryPage& page = asset.pages[p];
        page_ready[p] = (page.permanently_resident ||
                         (!input.page_resident.empty() && input.page_resident[p] != 0))
                            ? 1u
                            : 0u;
    }

    // Pack group metadata for the GPU.
    std::vector<GpuGroup> gpu_groups;
    gpu_groups.reserve(group_count);
    for (const assets::VirtualGeometryGroup& group : asset.groups) {
        GpuGroup gg{};
        gg.first_cluster = group.first_cluster;
        gg.cluster_count = group.cluster_count;
        gg.first_child = group.first_child;
        gg.child_count = group.child_count;
        gg.lod_error_m = group.lod_error_m;
        gpu_groups.push_back(gg);
    }

    // Cluster table: page index per cluster.
    std::vector<std::uint32_t> cluster_pages;
    cluster_pages.reserve(cluster_count);
    for (const assets::VirtualGeometryCluster& cluster : asset.clusters) {
        cluster_pages.push_back(cluster.page);
    }

    // Page dependency table.
    std::vector<GpuPage> gpu_pages;
    gpu_pages.reserve(page_count);
    for (const assets::VirtualGeometryPage& page : asset.pages) {
        GpuPage gp{};
        gp.first_dependency = page.first_dependency;
        gp.dependency_count = page.dependency_count;
        gpu_pages.push_back(gp);
    }

    // Create the compute shader and pipeline.
    rhi::ShaderDesc shader_desc{};
    shader_desc.stage = rhi::ShaderStage::Compute;
    shader_desc.spirv = vg_select_comp_spv;
    shader_desc.spirv_size_bytes = sizeof(vg_select_comp_spv);
    shader_desc.debug_name = "vg_select.comp";
    const rhi::ShaderHandle shader = device.create_shader(shader_desc);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {1, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {3, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {4, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {5, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {6, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {7, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        {8, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
    };
    rhi::ComputePipelineDesc pipeline_desc{};
    pipeline_desc.shader = shader;
    pipeline_desc.bindings = bindings;
    pipeline_desc.push_constant_size = sizeof(PushConstants);
    pipeline_desc.debug_name = "vg-select";
    const rhi::PipelineHandle pipeline = device.create_compute_pipeline(pipeline_desc);

    // VMA does not allow zero-byte allocations, but valid assets may have empty child or
    // dependency tables. Every storage binding is declared in the pipeline, so we bind a 1-element
    // zero buffer for any empty table; the shader never reads past the declared counts.
    const auto make_buffer = [&](std::uint64_t size,
                                 rhi::BufferUsage usage,
                                 rhi::MemoryUsage memory,
                                 const void* initial_data,
                                 std::string_view name) {
        rhi::BufferDesc desc{};
        desc.size = std::max(size, std::uint64_t{sizeof(std::uint32_t)});
        desc.usage = usage;
        desc.memory = memory;
        desc.initial_data = initial_data;
        desc.debug_name = name;
        return device.create_buffer(desc);
    };

    const rhi::BufferHandle group_buffer = make_buffer(gpu_groups.size() * sizeof(GpuGroup),
                                                       rhi::BufferUsage::Storage,
                                                       rhi::MemoryUsage::CpuToGpu,
                                                       gpu_groups.data(),
                                                       "vg-select-groups");
    const rhi::BufferHandle cluster_buffer =
        make_buffer(cluster_pages.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::CpuToGpu,
                    cluster_pages.data(),
                    "vg-select-clusters");
    const rhi::BufferHandle page_buffer = make_buffer(gpu_pages.size() * sizeof(GpuPage),
                                                      rhi::BufferUsage::Storage,
                                                      rhi::MemoryUsage::CpuToGpu,
                                                      gpu_pages.data(),
                                                      "vg-select-pages");

    std::vector<std::uint32_t> dependency_data = asset.page_dependencies;
    if (dependency_data.empty())
        dependency_data.push_back(0u);
    const rhi::BufferHandle dependency_buffer =
        make_buffer(dependency_data.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::CpuToGpu,
                    dependency_data.data(),
                    "vg-select-dependencies");

    const rhi::BufferHandle page_ready_buffer =
        make_buffer(page_ready.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::CpuToGpu,
                    page_ready.data(),
                    "vg-select-page-ready");
    const rhi::BufferHandle parent_buffer = make_buffer(parents.size() * sizeof(std::uint32_t),
                                                        rhi::BufferUsage::Storage,
                                                        rhi::MemoryUsage::CpuToGpu,
                                                        parents.data(),
                                                        "vg-select-parents");

    std::vector<std::uint32_t> child_group_data = asset.child_groups;
    if (child_group_data.empty())
        child_group_data.push_back(0u);
    const rhi::BufferHandle child_group_buffer =
        make_buffer(child_group_data.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::CpuToGpu,
                    child_group_data.data(),
                    "vg-select-child-groups");

    std::vector<std::uint32_t> selected_init(group_count, 0u);
    if (selected_init.empty())
        selected_init.push_back(0u);
    const rhi::BufferHandle selected_buffer =
        make_buffer(selected_init.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::GpuToCpu,
                    selected_init.data(),
                    "vg-select-selected");

    // Counters layout must match the Counters block in vg_select.comp.
    const std::array<std::uint32_t, 2> counter_init{0u, 0u};
    const rhi::BufferHandle counter_buffer =
        make_buffer(counter_init.size() * sizeof(std::uint32_t),
                    rhi::BufferUsage::Storage,
                    rhi::MemoryUsage::GpuToCpu,
                    counter_init.data(),
                    "vg-select-counters");

    PushConstants pc{};
    pc.pixels_per_metre = input.pixels_per_metre;
    pc.max_projected_error_px = input.max_projected_error_px;
    pc.group_count = group_count;
    pc.coarse_group = asset.coarse_group;

    auto cmd = device.begin_commands();
    cmd->bind_compute_pipeline(pipeline);
    cmd->bind_storage_buffer(0, group_buffer);
    cmd->bind_storage_buffer(1, cluster_buffer);
    cmd->bind_storage_buffer(2, page_buffer);
    cmd->bind_storage_buffer(3, dependency_buffer);
    cmd->bind_storage_buffer(4, page_ready_buffer);
    cmd->bind_storage_buffer(5, parent_buffer);
    cmd->bind_storage_buffer(6, child_group_buffer);
    cmd->bind_storage_buffer(7, selected_buffer);
    cmd->bind_storage_buffer(8, counter_buffer);
    cmd->push_constants(&pc, sizeof(pc));
    cmd->dispatch((group_count + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);
    device.submit_blocking(*cmd);

    std::vector<std::uint32_t> selected(group_count);
    device.read_buffer(
        selected_buffer, selected.data(), selected.size() * sizeof(std::uint32_t), 0);
    std::array<std::uint32_t, 2> counters{0u, 0u};
    device.read_buffer(counter_buffer, counters.data(), sizeof(counters), 0);

    for (std::uint32_t g = 0; g < group_count; ++g) {
        if (selected[g] != 0u) {
            out.groups.push_back(g);
        }
    }
    std::sort(out.groups.begin(), out.groups.end());
    out.refinement_blocked_by_residency = counters[0];
    out.gpu_depth_overflow = counters[1];

    device.destroy(selected_buffer);
    device.destroy(counter_buffer);
    device.destroy(child_group_buffer);
    device.destroy(parent_buffer);
    device.destroy(page_ready_buffer);
    device.destroy(dependency_buffer);
    device.destroy(page_buffer);
    device.destroy(cluster_buffer);
    device.destroy(group_buffer);
    device.destroy(pipeline);
    device.destroy(shader);

    return out;
}

} // namespace rime::render
