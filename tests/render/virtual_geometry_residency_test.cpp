// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include <array>

#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"

using namespace rime;

namespace {

assets::VirtualGeometryAsset residency_asset() {
    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    asset.page_bytes.resize(192);
    asset.pages = {
        {0, 64, 0, 1, 0, 0, true}, {64, 64, 1, 1, 0, 0, false}, {128, 64, 2, 1, 0, 1, false}};
    asset.page_dependencies = {1};
    for (std::uint32_t page = 0; page < 3; ++page) {
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {-1.0f, -1.0f, -1.0f};
        cluster.bounds.max = {1.0f, 1.0f, 1.0f};
        cluster.page = page;
        cluster.vertex_count = 3;
        cluster.index_count = 3;
        cluster.replacement_group = page;
        asset.clusters.push_back(cluster);
    }
    asset.groups = {{0, 1, 0, 2, 2.0f, true}, {1, 1, 0, 0, 0.1f, false}, {2, 1, 0, 0, 0.1f, false}};
    asset.child_groups = {1, 2};
    asset.coarse_group = 0;
    return asset;
}

} // namespace

TEST_CASE("virtual geometry residency owns coarse pages and dependency-gated requests (M18)") {
    const assets::VirtualGeometryAsset asset = residency_asset();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    render::VirtualGeometryResidency residency;
    REQUIRE(residency.register_asset(assets::AssetId{42}, asset));
    const std::span<const std::uint8_t> initial_residency =
        residency.page_residency_bytes(assets::AssetId{42});
    REQUIRE(initial_residency.size() == 3);
    CHECK(initial_residency[0] == 1);
    CHECK(initial_residency[1] == 0);
    CHECK(initial_residency[2] == 0);

    SUBCASE("dependencies must be resident before a transient page can be in flight") {
        CHECK_FALSE(residency.request_page(assets::AssetId{42}, 2));
        CHECK(residency.request_page(assets::AssetId{42}, 1));
        CHECK(residency.is_in_flight(assets::AssetId{42}, 1));
        CHECK_FALSE(residency.evict_page(assets::AssetId{42}, 1));
        CHECK(residency.complete_page(assets::AssetId{42}, 1));
        CHECK(residency.request_page(assets::AssetId{42}, 2));
        CHECK(residency.complete_page(assets::AssetId{42}, 2));
    }
    SUBCASE("permanent pages cannot be evicted and failed uploads can retry") {
        CHECK_FALSE(residency.evict_page(assets::AssetId{42}, 0));
        CHECK(residency.request_page(assets::AssetId{42}, 1));
        CHECK_FALSE(residency.complete_page(assets::AssetId{42}, 1, false));
        CHECK_FALSE(residency.is_resident(assets::AssetId{42}, 1));
        CHECK(residency.request_page(assets::AssetId{42}, 1));
    }
    SUBCASE("selector can consume the cache bytes directly") {
        const render::VirtualGeometrySelection selected = render::select_virtual_geometry(
            asset,
            {.pixels_per_metre = 2.0f,
             .max_projected_error_px = 1.0f,
             .page_resident = residency.page_residency_bytes(assets::AssetId{42})});
        CHECK(selected.groups == std::vector<std::uint32_t>{0});
        CHECK(selected.refinement_blocked_by_residency == 1);
    }
}
