// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// GPU proof for virtual-geometry replacement-cut selection (M18.3 slice A). The GPU selector must
// produce the same group set and counters as the CPU oracle across a grid of camera scales,
// error thresholds, and residency patterns. Output order is normalized by sorting both sides.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "rime/render/virtual_geometry_gpu_selection.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/rhi/rhi.hpp"

using namespace rime;

namespace {

[[nodiscard]] bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

[[nodiscard]] assets::VirtualGeometryAsset hierarchy_asset() {
    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    asset.page_bytes.resize(192);
    asset.pages = {
        {0, 64, 0, 1, 0, 0, true}, {64, 64, 1, 1, 0, 0, false}, {128, 64, 2, 1, 0, 0, false}};
    for (std::uint32_t index = 0; index < 3; ++index) {
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-1.0f, -1.0f, -1.0f};
        cluster.bounds.max = {1.0f, 1.0f, 1.0f};
        cluster.page = index;
        cluster.vertex_count = 3;
        cluster.index_count = 3;
        cluster.replacement_group = index;
        asset.clusters.push_back(cluster);
    }
    asset.groups = {{0, 1, 0, 2, 2.0f, true}, {1, 1, 0, 0, 0.1f, false}, {2, 1, 0, 0, 0.1f, false}};
    asset.child_groups = {1, 2};
    asset.coarse_group = 0;
    return asset;
}

// Build a valid asset whose group chain is deeper than the GPU's fixed stack. Each group has one
// child, so the hierarchy depth is the number of groups. The asset must pass validation and the
// CPU oracle must be able to traverse it; the GPU path is expected to fall back to the CPU
// oracle instead of silently giving up.
[[nodiscard]] assets::VirtualGeometryAsset deep_chain_asset(std::uint32_t group_count) {
    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);

    constexpr std::uint32_t kBytesPerPage = 64u;
    asset.page_bytes.resize(static_cast<std::size_t>(group_count) * kBytesPerPage);

    for (std::uint32_t i = 0; i < group_count; ++i) {
        asset.pages.push_back(
            {static_cast<std::uint64_t>(i) * kBytesPerPage, kBytesPerPage, i, 1, 0, 0, i == 0});

        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-1.0f, -1.0f, -1.0f};
        cluster.bounds.max = {1.0f, 1.0f, 1.0f};
        cluster.page = i;
        cluster.vertex_count = 3;
        cluster.index_count = 3;
        cluster.replacement_group = i;
        asset.clusters.push_back(cluster);

        assets::VirtualGeometryGroup group{};
        group.first_cluster = i;
        group.cluster_count = 1;
        group.first_child = i;
        group.child_count = (i + 1 < group_count) ? 1u : 0u;
        group.lod_error_m = (i == 0) ? 2.0f : 0.1f;
        group.permanently_resident = (i == 0);
        asset.groups.push_back(group);

        if (i + 1 < group_count)
            asset.child_groups.push_back(i + 1);
    }

    asset.coarse_group = 0;
    return asset;
}

[[nodiscard]] std::vector<std::uint32_t> sorted(std::vector<std::uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

void check_matches(const assets::VirtualGeometryAsset& asset,
                   const render::VirtualGeometrySelectionInput& input) {
    const render::VirtualGeometrySelection cpu = render::select_virtual_geometry(asset, input);
    auto device = rhi::create_device({});
    REQUIRE(device);
    const render::VirtualGeometrySelection gpu =
        render::select_virtual_geometry_on_gpu(*device, asset, input);

    CHECK(sorted(gpu.groups) == sorted(cpu.groups));
    CHECK(gpu.refinement_blocked_by_residency == cpu.refinement_blocked_by_residency);
    CHECK(gpu.rejected_invalid_input == cpu.rejected_invalid_input);
    CHECK(gpu.gpu_depth_fallback == 0);
    CHECK(gpu.gpu_depth_overflow == 0);
}

} // namespace

TEST_CASE("virtual geometry GPU selection matches the CPU oracle (M18.3 slice A)") {
    if (vulkan_required()) {
        auto device = rhi::create_device({});
        if (!device)
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
    }

    auto device = rhi::create_device({});
    if (!device) {
        MESSAGE("no Vulkan device available — skipping GPU selection proof");
        return;
    }

    const assets::VirtualGeometryAsset asset = hierarchy_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    const std::array<float, 6> pixels_per_metre = {0.0f, 0.25f, 1.0f, 2.0f, 2.0000002f, 8.0f};
    const std::array<float, 5> max_projected_error_px = {0.0f, 1.0f, 3.999f, 4.0f, 4.001f};

    const std::array<std::vector<std::uint8_t>, 4> residency_patterns = {
        std::vector<std::uint8_t>{1, 1, 1},
        std::vector<std::uint8_t>{1, 0, 1},
        std::vector<std::uint8_t>{1, 1, 0},
        std::vector<std::uint8_t>{},
    };

    std::size_t combinations = 0;
    for (const float ppm : pixels_per_metre) {
        for (const float err : max_projected_error_px) {
            for (const auto& residency : residency_patterns) {
                const render::VirtualGeometrySelectionInput input = {.pixels_per_metre = ppm,
                                                                     .max_projected_error_px = err,
                                                                     .page_resident = residency};
                check_matches(asset, input);
                ++combinations;
            }
        }
    }

    // The four invalid-input cases from the CPU oracle's degenerate-input test.
    {
        assets::VirtualGeometryAsset empty_asset{};
        const render::VirtualGeometrySelectionInput bad_input = {};
        check_matches(empty_asset, bad_input);
        ++combinations;
    }
    {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        check_matches(asset,
                      {.pixels_per_metre = -1.0f,
                       .max_projected_error_px = 1.0f,
                       .page_resident = residency});
        ++combinations;
    }
    {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        check_matches(asset,
                      {.pixels_per_metre = 2.0f,
                       .max_projected_error_px = std::numeric_limits<float>::infinity(),
                       .page_resident = residency});
        ++combinations;
    }
    {
        const std::array<std::uint8_t, 2> residency = {1, 1};
        check_matches(
            asset,
            {.pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = residency});
        ++combinations;
    }

    MESSAGE("GPU selection matched oracle across ", combinations, " input combinations");
}

TEST_CASE("GPU virtual geometry selection falls back when the hierarchy exceeds the depth cap") {
    auto device = rhi::create_device({});
    if (!device) {
        MESSAGE("no Vulkan device available — skipping GPU depth fallback test");
        return;
    }

    constexpr std::uint32_t kGroupCount = 300u;
    static_assert(kGroupCount > render::kVirtualGeometryGpuSelectionMaxDepth);

    const assets::VirtualGeometryAsset asset = deep_chain_asset(kGroupCount);
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    const std::vector<std::uint8_t> residency(asset.pages.size(), 1);
    const render::VirtualGeometrySelectionInput input = {
        .pixels_per_metre = 8.0f,
        .max_projected_error_px = 1.0f,
        .page_resident = residency,
    };

    const render::VirtualGeometrySelection cpu = render::select_virtual_geometry(asset, input);
    const render::VirtualGeometrySelection gpu =
        render::select_virtual_geometry_on_gpu(*device, asset, input);

    CHECK(sorted(gpu.groups) == sorted(cpu.groups));
    CHECK(gpu.refinement_blocked_by_residency == cpu.refinement_blocked_by_residency);
    CHECK(gpu.rejected_invalid_input == cpu.rejected_invalid_input);
    CHECK(gpu.gpu_depth_fallback == 1);
    CHECK(gpu.gpu_depth_overflow == 0);
}
