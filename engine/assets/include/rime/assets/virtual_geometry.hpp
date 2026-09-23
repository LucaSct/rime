// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/mesh_asset.hpp"

// The CPU-side contract for M18 virtualized geometry (ADR-0043). This is deliberately a companion
// to MeshAsset, not a wider MeshAsset: conventional indexed meshes remain a stable asset ABI and a
// compatibility draw path. The cooker eventually writes this graph as its own RMA1 payload; the
// renderer owns GPU pages, residency and packed visibility identities, none of which appear here.
namespace rime::assets {

inline constexpr std::uint32_t kInvalidVirtualGeometryIndex =
    std::numeric_limits<std::uint32_t>::max();

// One resident/uploadable byte range. Pages name clusters and dependencies, but do not expose a GPU
// address: compaction can move the bytes between submissions without invalidating cooked data.
struct VirtualGeometryPage {
    std::uint64_t byte_offset = 0;
    std::uint32_t byte_size = 0;
    std::uint32_t first_cluster = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t first_dependency = 0;
    std::uint32_t dependency_count = 0;
    bool permanently_resident = false; // coarse-cut pages may never be evicted
};

// A compact rigid triangle group. Error is the conservative local-space displacement in metres of
// using this representation; projected-error selection converts it with the current camera. A
// group, rather than one independently selected cluster, is the replacement unit that prevents
// mixed LOD boundaries from opening cracks.
struct VirtualGeometryCluster {
    Aabb bounds{};
    float lod_error_m = 0.0f;
    std::uint32_t page = kInvalidVirtualGeometryIndex;
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_slot = 0;
    std::uint32_t replacement_group = kInvalidVirtualGeometryIndex;
};

// A complete representation at one LOD. `children` is an adjacency slice into
// VirtualGeometryAsset::child_groups; it is a DAG rather than an implicit binary tree, because one
// complete parent cut can be replaced by several independent groups. The reader rejects cycles.
struct VirtualGeometryGroup {
    std::uint32_t first_cluster = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t first_child = 0;
    std::uint32_t child_count = 0;
    float lod_error_m = 0.0f;
    bool permanently_resident = false;
};

struct VirtualGeometryAsset {
    // Identity/layout of the source mesh whose material slots and vertex decoding this payload
    // uses. These are cook inputs, not a request to load a second renderable MeshAsset at runtime.
    AssetId source_mesh{};
    VertexAttribs attribs = VertexAttribs::None;
    std::uint32_t vertex_stride = 0;

    std::vector<VirtualGeometryPage> pages;
    std::vector<VirtualGeometryCluster> clusters;
    std::vector<VirtualGeometryGroup> groups;
    std::vector<std::uint32_t> child_groups;
    std::vector<std::uint32_t> page_dependencies;
    std::vector<std::byte> page_bytes;
    std::uint32_t coarse_group = kInvalidVirtualGeometryIndex;
};

enum class VirtualGeometryError : std::uint8_t {
    None,
    InvalidSourceLayout,
    InvalidPage,
    InvalidCluster,
    InvalidGroup,
    CyclicPages,
    CyclicGroups,
    MissingCoarseCut,
};

enum class VirtualGeometryPageViewError : std::uint8_t {
    None,
    InvalidCluster,
    InvalidPage,
    InvalidLayout,
    MisalignedIndices,
    OutOfBounds,
};

// A checked, non-owning view of one cluster's page data. The page format is the cooker contract:
// interleaved vertex bytes first, followed immediately by little-endian u32 indices. Offsets are
// relative to the page, while `page` and `vertices`/`indices` are bounded spans into page_bytes.
struct VirtualGeometryPageView {
    std::span<const std::byte> page{};
    std::span<const std::byte> vertices{};
    std::span<const std::byte> indices{};
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

// Resolve a cluster's upload ranges without allocating or trusting cooked offsets. The vertex
// section is inferred from every cluster assigned to the page, so a cluster cannot make the index
// section overlap another cluster's vertices. This function intentionally returns byte spans:
// page storage is serialized bytes and need not meet uint32_t alignment for a typed C++ view.
[[nodiscard]] inline VirtualGeometryPageViewError
view_virtual_geometry_page(const VirtualGeometryAsset& asset,
                           std::uint32_t cluster_index,
                           VirtualGeometryPageView& out) noexcept {
    out = {};
    if (cluster_index >= asset.clusters.size()) {
        return VirtualGeometryPageViewError::InvalidCluster;
    }
    const VirtualGeometryCluster& cluster = asset.clusters[cluster_index];
    if (cluster.page >= asset.pages.size()) {
        return VirtualGeometryPageViewError::InvalidCluster;
    }
    const VirtualGeometryPage& page = asset.pages[cluster.page];
    if (asset.vertex_stride == 0) {
        return VirtualGeometryPageViewError::InvalidLayout;
    }
    const auto inside = [](std::uint64_t first, std::uint64_t count, std::uint64_t size) {
        return first <= size && count <= size - first;
    };
    if (page.byte_offset > asset.page_bytes.size() ||
        page.byte_size > asset.page_bytes.size() - page.byte_offset || page.byte_size == 0 ||
        !inside(page.first_cluster, page.cluster_count, asset.clusters.size()) ||
        cluster_index < page.first_cluster ||
        cluster_index >= page.first_cluster + page.cluster_count) {
        return VirtualGeometryPageViewError::InvalidPage;
    }
    const std::span<const std::byte> page_bytes =
        std::span<const std::byte>(asset.page_bytes)
            .subspan(static_cast<std::size_t>(page.byte_offset), page.byte_size);

    std::uint64_t vertex_end = 0;
    for (std::uint32_t i = 0; i < page.cluster_count; ++i) {
        const VirtualGeometryCluster& candidate = asset.clusters[page.first_cluster + i];
        if (candidate.page != cluster.page || candidate.vertex_count == 0 ||
            candidate.index_count == 0 || candidate.index_count % 3 != 0 ||
            candidate.vertex_offset % asset.vertex_stride != 0) {
            return VirtualGeometryPageViewError::InvalidLayout;
        }
        const std::uint64_t candidate_vertex_end =
            std::uint64_t{candidate.vertex_offset} +
            std::uint64_t{candidate.vertex_count} * asset.vertex_stride;
        if (candidate_vertex_end > page.byte_size) {
            return VirtualGeometryPageViewError::OutOfBounds;
        }
        vertex_end = std::max(vertex_end, candidate_vertex_end);
    }
    if (asset.vertex_stride == 0 || vertex_end > page.byte_size || vertex_end % 4 != 0) {
        return VirtualGeometryPageViewError::InvalidLayout;
    }
    const std::uint64_t index_bytes = std::uint64_t{page.byte_size} - vertex_end;
    if (vertex_end % 4 != 0 || index_bytes % 4 != 0) {
        return VirtualGeometryPageViewError::MisalignedIndices;
    }
    if (!inside(cluster.vertex_offset,
                std::uint64_t{cluster.vertex_count} * asset.vertex_stride,
                vertex_end) ||
        !inside(std::uint64_t{cluster.first_index} * 4,
                std::uint64_t{cluster.index_count} * 4,
                index_bytes)) {
        return VirtualGeometryPageViewError::OutOfBounds;
    }

    out.page = page_bytes;
    out.vertices = page_bytes.subspan(0, static_cast<std::size_t>(vertex_end));
    out.indices = page_bytes.subspan(static_cast<std::size_t>(vertex_end));
    out.vertex_stride = asset.vertex_stride;
    out.vertex_offset = cluster.vertex_offset;
    out.vertex_count = cluster.vertex_count;
    out.index_offset = static_cast<std::uint32_t>(vertex_end);
    out.first_index = cluster.first_index;
    out.index_count = cluster.index_count;
    return VirtualGeometryPageViewError::None;
}

[[nodiscard]] inline bool read_virtual_geometry_index(const VirtualGeometryPageView& view,
                                                      std::uint32_t index,
                                                      std::uint32_t& out) noexcept {
    if (index >= view.index_count ||
        (std::uint64_t{view.first_index} + index) * 4 + 4 > view.indices.size()) {
        return false;
    }
    const std::size_t offset = (std::size_t{view.first_index} + index) * 4;
    const auto byte = [&view, offset](std::size_t i) {
        return std::to_integer<std::uint32_t>(view.indices[offset + i]);
    };
    out = byte(0) | (byte(1) << 8) | (byte(2) << 16) | (byte(3) << 24);
    return true;
}

[[nodiscard]] inline bool virtual_geometry_finite_bounds(const Aabb& bounds) noexcept {
    return std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) &&
           std::isfinite(bounds.min.z) && std::isfinite(bounds.max.x) &&
           std::isfinite(bounds.max.y) && std::isfinite(bounds.max.z) &&
           bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y &&
           bounds.min.z <= bounds.max.z;
}

// Validate every untrusted, cooked-data invariant before a renderer can size a GPU allocation or
// traverse the replacement graph. This is intentionally CPU-only: whether a page is resident is a
// frame-local render decision, while whether it *can* replace another group is immutable asset
// data.
[[nodiscard]] inline VirtualGeometryError
validate_virtual_geometry(const VirtualGeometryAsset& asset) noexcept {
    const auto inside = [](std::uint64_t first, std::uint64_t count, std::uint64_t size) {
        return first <= size && count <= size - first;
    };
    if (!asset.source_mesh.is_valid() ||
        (static_cast<std::uint32_t>(asset.attribs) & ~kKnownVertexAttribs) != 0 ||
        !has_attrib(asset.attribs, VertexAttribs::Position) ||
        asset.vertex_stride != expected_vertex_stride(asset.attribs)) {
        return VirtualGeometryError::InvalidSourceLayout;
    }
    if (asset.coarse_group >= asset.groups.size()) {
        return VirtualGeometryError::MissingCoarseCut;
    }
    for (std::uint32_t page_index = 0; page_index < asset.pages.size(); ++page_index) {
        const VirtualGeometryPage& page = asset.pages[page_index];
        if (page.byte_size == 0 ||
            !inside(page.byte_offset, page.byte_size, asset.page_bytes.size()) ||
            !inside(page.first_cluster, page.cluster_count, asset.clusters.size()) ||
            !inside(page.first_dependency, page.dependency_count, asset.page_dependencies.size())) {
            return VirtualGeometryError::InvalidPage;
        }
        for (std::uint32_t i = 0; i < page.dependency_count; ++i) {
            if (asset.page_dependencies[page.first_dependency + i] >= asset.pages.size()) {
                return VirtualGeometryError::InvalidPage;
            }
        }
        for (std::uint32_t i = 0; i < page.cluster_count; ++i) {
            if (asset.clusters[page.first_cluster + i].page != page_index) {
                return VirtualGeometryError::InvalidPage;
            }
        }
    }
    for (const VirtualGeometryCluster& cluster : asset.clusters) {
        if (cluster.page >= asset.pages.size() ||
            cluster.replacement_group >= asset.groups.size() ||
            !virtual_geometry_finite_bounds(cluster.bounds) ||
            !std::isfinite(cluster.lod_error_m) || cluster.lod_error_m < 0.0f ||
            cluster.vertex_count == 0 || cluster.index_count == 0 || cluster.index_count % 3 != 0) {
            return VirtualGeometryError::InvalidCluster;
        }
    }
    for (const VirtualGeometryGroup& group : asset.groups) {
        if (group.cluster_count == 0 ||
            !inside(group.first_cluster, group.cluster_count, asset.clusters.size()) ||
            !inside(group.first_child, group.child_count, asset.child_groups.size()) ||
            !std::isfinite(group.lod_error_m) || group.lod_error_m < 0.0f) {
            return VirtualGeometryError::InvalidGroup;
        }
        for (std::uint32_t i = 0; i < group.child_count; ++i) {
            if (asset.child_groups[group.first_child + i] >= asset.groups.size()) {
                return VirtualGeometryError::InvalidGroup;
            }
        }
    }

    const VirtualGeometryGroup& coarse = asset.groups[asset.coarse_group];
    if (!coarse.permanently_resident) {
        return VirtualGeometryError::MissingCoarseCut;
    }
    for (std::uint32_t i = 0; i < coarse.cluster_count; ++i) {
        const VirtualGeometryCluster& cluster = asset.clusters[coarse.first_cluster + i];
        if (cluster.replacement_group != asset.coarse_group ||
            !asset.pages[cluster.page].permanently_resident) {
            return VirtualGeometryError::MissingCoarseCut;
        }
    }
    for (std::uint32_t cluster_index = 0; cluster_index < asset.clusters.size(); ++cluster_index) {
        const VirtualGeometryCluster& cluster = asset.clusters[cluster_index];
        const VirtualGeometryGroup& group = asset.groups[cluster.replacement_group];
        if (!inside(group.first_cluster, group.cluster_count, asset.clusters.size()) ||
            cluster_index < group.first_cluster ||
            cluster_index >= group.first_cluster + group.cluster_count) {
            return VirtualGeometryError::InvalidCluster;
        }
    }

    // White/grey/black DFS detects a cycle without recursion, so a corrupt asset cannot consume an
    // unbounded call stack. Shared children remain legal — that is the point of a DAG.
    const auto has_cycle = [](std::span<const std::uint32_t> starts,
                              std::span<const std::uint32_t> counts,
                              std::span<const std::uint32_t> edges) {
        std::vector<std::uint8_t> colour(starts.size(), 0);
        struct Frame {
            std::uint32_t node = 0;
            std::uint32_t next_edge = 0;
        };
        std::vector<Frame> stack;
        for (std::uint32_t root = 0; root < starts.size(); ++root) {
            if (colour[root] != 0)
                continue;
            colour[root] = 1;
            stack.push_back({root, 0});
            while (!stack.empty()) {
                Frame& frame = stack.back();
                if (frame.next_edge == counts[frame.node]) {
                    colour[frame.node] = 2;
                    stack.pop_back();
                    continue;
                }
                const std::uint32_t child = edges[starts[frame.node] + frame.next_edge++];
                if (colour[child] == 1)
                    return true;
                if (colour[child] == 0) {
                    colour[child] = 1;
                    stack.push_back({child, 0});
                }
            }
        }
        return false;
    };
    std::vector<std::uint32_t> page_starts;
    std::vector<std::uint32_t> page_counts;
    page_starts.reserve(asset.pages.size());
    page_counts.reserve(asset.pages.size());
    for (const VirtualGeometryPage& page : asset.pages) {
        page_starts.push_back(page.first_dependency);
        page_counts.push_back(page.dependency_count);
    }
    if (has_cycle(page_starts, page_counts, asset.page_dependencies)) {
        return VirtualGeometryError::CyclicPages;
    }
    std::vector<std::uint32_t> group_starts;
    std::vector<std::uint32_t> group_counts;
    group_starts.reserve(asset.groups.size());
    group_counts.reserve(asset.groups.size());
    for (const VirtualGeometryGroup& group : asset.groups) {
        group_starts.push_back(group.first_child);
        group_counts.push_back(group.child_count);
    }
    if (has_cycle(group_starts, group_counts, asset.child_groups)) {
        return VirtualGeometryError::CyclicGroups;
    }
    return VirtualGeometryError::None;
}

} // namespace rime::assets
