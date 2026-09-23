// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/assets/virtual_geometry.hpp"

// The CPU reference selector for M18's replacement DAG. This is deliberately render-owned: asset
// data says which replacement cuts are legal, while this policy decides which legal cut a camera
// and the current page cache may draw. GPU selection must agree with this oracle before it replaces
// it; no selected cluster is expanded into a SceneDrawData CPU draw list.
namespace rime::render {

struct VirtualGeometrySelectionInput {
    // A conservative screen-space scale at the instance. The first implementation keeps the
    // camera part explicit and testable; per-group distance/cone evaluation belongs beside GPU
    // culling, not in the cooked ABI.
    float pixels_per_metre = 0.0f;
    float max_projected_error_px = 1.0f;

    // One byte per cooked page: zero means unavailable and nonzero means resident. A permanent
    // page is considered resident by contract, so callers may pass an empty span for an asset
    // whose entire coarse fallback is permanent.
    std::span<const std::uint8_t> page_resident = {};
};

struct VirtualGeometrySelection {
    std::vector<std::uint32_t> groups;
    std::uint32_t refinement_blocked_by_residency = 0;
    // Inputs that fail asset validation or have nonsensical camera/ residency parameters are
    // dropped; this counter is a witness that the oracle rejected something, so tests cannot
    // pass by silently ignoring bad inputs.
    std::uint32_t rejected_invalid_input = 0;
    // GPU-only witnesses. The CPU oracle never falls back or overflows the GPU's fixed-size
    // stacks, so it leaves these at zero. A test that cannot see what was skipped still reads
    // a passing result because every path that gives up writes a counter.
    std::uint32_t gpu_depth_fallback = 0;
    std::uint32_t gpu_depth_overflow = 0;
};

[[nodiscard]] inline VirtualGeometrySelection
select_virtual_geometry(const assets::VirtualGeometryAsset& asset,
                        const VirtualGeometrySelectionInput& input) {
    VirtualGeometrySelection selected;
    if (assets::validate_virtual_geometry(asset) != assets::VirtualGeometryError::None ||
        !std::isfinite(input.pixels_per_metre) || input.pixels_per_metre < 0.0f ||
        !std::isfinite(input.max_projected_error_px) || input.max_projected_error_px < 0.0f ||
        (!input.page_resident.empty() && input.page_resident.size() != asset.pages.size())) {
        selected.rejected_invalid_input = 1;
        return selected;
    }

    const auto page_ready = [&](std::uint32_t page_index) {
        const assets::VirtualGeometryPage& page = asset.pages[page_index];
        return page.permanently_resident ||
               (!input.page_resident.empty() && input.page_resident[page_index] != 0);
    };
    const auto page_and_dependencies_ready = [&](std::uint32_t page_index) {
        std::vector<std::uint32_t> pending{page_index};
        while (!pending.empty()) {
            const std::uint32_t current = pending.back();
            pending.pop_back();
            if (!page_ready(current)) {
                return false;
            }
            const assets::VirtualGeometryPage& page = asset.pages[current];
            for (std::uint32_t dependency_offset = 0; dependency_offset < page.dependency_count;
                 ++dependency_offset) {
                pending.push_back(
                    asset.page_dependencies[page.first_dependency + dependency_offset]);
            }
        }
        return true;
    };
    const auto group_ready = [&](std::uint32_t group_index) {
        const assets::VirtualGeometryGroup& group = asset.groups[group_index];
        for (std::uint32_t cluster_offset = 0; cluster_offset < group.cluster_count;
             ++cluster_offset) {
            const assets::VirtualGeometryCluster& cluster =
                asset.clusters[group.first_cluster + cluster_offset];
            if (!page_and_dependencies_ready(cluster.page)) {
                return false;
            }
        }
        return true;
    };

    std::vector<std::uint32_t> pending{asset.coarse_group};
    while (!pending.empty()) {
        const std::uint32_t group_index = pending.back();
        pending.pop_back();
        const assets::VirtualGeometryGroup& group = asset.groups[group_index];
        const bool needs_refinement =
            group.lod_error_m * input.pixels_per_metre > input.max_projected_error_px;
        bool children_ready = group.child_count != 0;
        if (needs_refinement) {
            for (std::uint32_t child_offset = 0; child_offset < group.child_count; ++child_offset) {
                if (!group_ready(asset.child_groups[group.first_child + child_offset])) {
                    children_ready = false;
                    break;
                }
            }
        }
        if (needs_refinement && children_ready) {
            // Reverse push preserves the cooker-defined child order in `selected.groups`.
            for (std::uint32_t child_offset = group.child_count; child_offset != 0;
                 --child_offset) {
                pending.push_back(asset.child_groups[group.first_child + child_offset - 1]);
            }
        } else {
            selected.groups.push_back(group_index);
            if (needs_refinement && group.child_count != 0) {
                ++selected.refinement_blocked_by_residency;
            }
        }
    }
    return selected;
}

} // namespace rime::render
