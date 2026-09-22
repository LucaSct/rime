// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M18's first binary-contract proof. These are deliberately CPU-only: the cooker/reader must
// reject malformed hierarchy data before a render cache can turn an unchecked offset or a cycle
// into a GPU allocation, stale visibility id, or unbounded traversal.

#include <doctest/doctest.h>

#include "rime/assets/virtual_geometry.hpp"

using namespace rime::assets;

namespace {

[[nodiscard]] VirtualGeometryAsset valid_asset() {
    VirtualGeometryAsset asset{};
    asset.source_mesh = AssetId{1};
    asset.attribs = kMeshV1Attribs;
    asset.vertex_stride = expected_vertex_stride(asset.attribs);
    asset.page_bytes.resize(64);
    asset.pages.push_back({0, 64, 0, 1, 0, 0, true});

    VirtualGeometryCluster cluster{};
    cluster.bounds.min = {-1.0f, -1.0f, -1.0f};
    cluster.bounds.max = {1.0f, 1.0f, 1.0f};
    cluster.page = 0;
    cluster.vertex_count = 3;
    cluster.index_count = 3;
    cluster.replacement_group = 0;
    asset.clusters.push_back(cluster);
    asset.groups.push_back({0, 1, 0, 0, 0.0f, true});
    asset.coarse_group = 0;
    return asset;
}

} // namespace

TEST_CASE("virtual geometry: a complete permanently resident coarse cut validates (M18)") {
    const VirtualGeometryAsset asset = valid_asset();
    CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::None);
}

TEST_CASE("virtual geometry: malformed pages and replacement DAGs never reach a renderer (M18)") {
    SUBCASE("a page byte range cannot extend beyond the payload") {
        VirtualGeometryAsset asset = valid_asset();
        asset.pages[0].byte_size = 65;
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::InvalidPage);
    }
    SUBCASE("a page cannot claim a cluster that names another page") {
        VirtualGeometryAsset asset = valid_asset();
        asset.clusters[0].page = 1;
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::InvalidPage);
    }
    SUBCASE("a replacement cycle is rejected without recursive traversal") {
        VirtualGeometryAsset asset = valid_asset();
        asset.groups[0].first_child = 0;
        asset.groups[0].child_count = 1;
        asset.child_groups.push_back(0);
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::CyclicGroups);
    }
    SUBCASE("a page-dependency cycle is rejected before it can pin an upload queue") {
        VirtualGeometryAsset asset = valid_asset();
        asset.pages[0].first_dependency = 0;
        asset.pages[0].dependency_count = 1;
        asset.page_dependencies.push_back(0);
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::CyclicPages);
    }
    SUBCASE("the coarse fallback itself is mandatory") {
        VirtualGeometryAsset asset = valid_asset();
        asset.coarse_group = kInvalidVirtualGeometryIndex;
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::MissingCoarseCut);
    }
    SUBCASE("a nominal coarse group cannot point at an evictable page") {
        VirtualGeometryAsset asset = valid_asset();
        asset.pages[0].permanently_resident = false;
        CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::MissingCoarseCut);
    }
}
