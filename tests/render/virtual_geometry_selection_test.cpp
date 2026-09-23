// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include <array>
#include <limits>

#include "rime/render/virtual_geometry_selection.hpp"

using namespace rime;

namespace {

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

} // namespace

TEST_CASE("virtual geometry selection: refinement is an all-or-nothing replacement cut (M18)") {
    const assets::VirtualGeometryAsset asset = hierarchy_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    SUBCASE("a missing member preserves the permanent coarse fallback without a hole") {
        const std::array<std::uint8_t, 3> residency = {1, 0, 1};
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset,
            {.pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = residency});
        CHECK(selection.groups == std::vector<std::uint32_t>{0});
        CHECK(selection.refinement_blocked_by_residency == 1);
    }
    SUBCASE("only a complete resident replacement may replace its ancestor") {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset,
            {.pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = residency});
        CHECK(selection.groups == std::vector<std::uint32_t>{1, 2});
        CHECK(selection.refinement_blocked_by_residency == 0);
    }
    SUBCASE("within the error threshold the resident coarse cut remains selected") {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        const render::VirtualGeometrySelection selection =
            render::select_virtual_geometry(asset,
                                            {.pixels_per_metre = 0.25f,
                                             .max_projected_error_px = 1.0f,
                                             .page_resident = residency});
        CHECK(selection.groups == std::vector<std::uint32_t>{0});
    }
}

TEST_CASE("virtual geometry selection: degenerate inputs return an empty selection") {
    const assets::VirtualGeometryAsset valid_asset = hierarchy_asset();

    SUBCASE("invalid asset fails validation and returns empty") {
        assets::VirtualGeometryAsset empty_asset{};
        const render::VirtualGeometrySelection selection =
            render::select_virtual_geometry(empty_asset, {});
        CHECK(selection.groups.empty());
        CHECK(selection.refinement_blocked_by_residency == 0);
    }

    SUBCASE("negative pixels-per-metre is rejected") {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        const render::VirtualGeometrySelection selection =
            render::select_virtual_geometry(valid_asset,
                                            {.pixels_per_metre = -1.0f,
                                             .max_projected_error_px = 1.0f,
                                             .page_resident = residency});
        CHECK(selection.groups.empty());
        CHECK(selection.refinement_blocked_by_residency == 0);
    }

    SUBCASE("non-finite error threshold is rejected") {
        const std::array<std::uint8_t, 3> residency = {1, 1, 1};
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            valid_asset,
            {.pixels_per_metre = 2.0f,
             .max_projected_error_px = std::numeric_limits<float>::infinity(),
             .page_resident = residency});
        CHECK(selection.groups.empty());
        CHECK(selection.refinement_blocked_by_residency == 0);
    }

    SUBCASE("mismatched residency span size is rejected") {
        const std::array<std::uint8_t, 2> residency = {1, 1};
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            valid_asset,
            {.pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = residency});
        CHECK(selection.groups.empty());
        CHECK(selection.refinement_blocked_by_residency == 0);
    }
}

TEST_CASE("virtual geometry selection: empty residency span uses permanent pages only") {
    const assets::VirtualGeometryAsset asset = hierarchy_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    SUBCASE("permanent coarse fallback is selected when refinement is not needed") {
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset,
            {.pixels_per_metre = 0.25f, .max_projected_error_px = 1.0f, .page_resident = {}});
        CHECK(selection.groups == std::vector<std::uint32_t>{0});
        CHECK(selection.refinement_blocked_by_residency == 0);
    }

    SUBCASE("missing non-permanent children block refinement and are counted") {
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset, {.pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = {}});
        CHECK(selection.groups == std::vector<std::uint32_t>{0});
        CHECK(selection.refinement_blocked_by_residency == 1);
    }
}

TEST_CASE("virtual geometry selection: error threshold boundary is non-strict") {
    const assets::VirtualGeometryAsset asset = hierarchy_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    const std::array<std::uint8_t, 3> residency = {1, 1, 1};

    SUBCASE("exactly the boundary value keeps the coarser group") {
        // group 0 has lod_error_m = 2.0 m, pixels_per_metre = 2.0 px/m -> 4.0 px == max.
        const render::VirtualGeometrySelection selection = render::select_virtual_geometry(
            asset,
            {.pixels_per_metre = 2.0f, .max_projected_error_px = 4.0f, .page_resident = residency});
        CHECK(selection.groups == std::vector<std::uint32_t>{0});
        CHECK(selection.refinement_blocked_by_residency == 0);
    }

    SUBCASE("just above the boundary value refines") {
        const render::VirtualGeometrySelection selection =
            render::select_virtual_geometry(asset,
                                            {.pixels_per_metre = 2.0f,
                                             .max_projected_error_px = 3.999f,
                                             .page_resident = residency});
        CHECK(selection.groups == std::vector<std::uint32_t>{1, 2});
        CHECK(selection.refinement_blocked_by_residency == 0);
    }
}

TEST_CASE("virtual geometry selection: identical inputs produce identical outputs") {
    const assets::VirtualGeometryAsset asset = hierarchy_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    const std::array<std::uint8_t, 3> residency = {1, 0, 1};
    const render::VirtualGeometrySelectionInput input = {
        .pixels_per_metre = 2.0f, .max_projected_error_px = 1.0f, .page_resident = residency};

    const render::VirtualGeometrySelection first = render::select_virtual_geometry(asset, input);
    const render::VirtualGeometrySelection second = render::select_virtual_geometry(asset, input);

    CHECK(first.groups == second.groups);
    CHECK(first.refinement_blocked_by_residency == second.refinement_blocked_by_residency);
}
