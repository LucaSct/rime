// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M18's first binary-contract proof. These are deliberately CPU-only: the cooker/reader must
// reject malformed hierarchy data before a render cache can turn an unchecked offset or a cycle
// into a GPU allocation, stale visibility id, or unbounded traversal.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/registry.hpp"
#include "rime/assets/virtual_geometry.hpp"
#include "rime/core/byte_cursor.hpp"
#include "rime/platform/filesystem.hpp"

using namespace rime::assets;

namespace {

[[nodiscard]] VirtualGeometryAsset valid_asset() {
    VirtualGeometryAsset asset{};
    asset.source_mesh = AssetId{1};
    asset.attribs = kMeshV1Attribs;
    asset.vertex_stride = expected_vertex_stride(asset.attribs);
    asset.page_bytes.resize(108);
    asset.pages.push_back({0, 108, 0, 1, 0, 0, true});

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

[[nodiscard]] std::vector<std::byte> write_payload(const VirtualGeometryAsset& asset,
                                                   std::uint32_t version = 1) {
    std::vector<std::byte> bytes;
    rime::core::ByteWriter writer(bytes);
    writer.u32(version);
    writer.u64(asset.source_mesh.value);
    writer.u32(static_cast<std::uint32_t>(asset.attribs));
    writer.u32(asset.vertex_stride);
    writer.u32(static_cast<std::uint32_t>(asset.pages.size()));
    writer.u32(static_cast<std::uint32_t>(asset.clusters.size()));
    writer.u32(static_cast<std::uint32_t>(asset.groups.size()));
    writer.u32(static_cast<std::uint32_t>(asset.child_groups.size()));
    writer.u32(static_cast<std::uint32_t>(asset.page_dependencies.size()));
    writer.u32(static_cast<std::uint32_t>(asset.page_bytes.size()));
    writer.u32(asset.coarse_group);
    for (const VirtualGeometryPage& page : asset.pages) {
        writer.u64(page.byte_offset);
        writer.u32(page.byte_size);
        writer.u32(page.first_cluster);
        writer.u32(page.cluster_count);
        writer.u32(page.first_dependency);
        writer.u32(page.dependency_count);
        writer.u8(page.permanently_resident ? 1 : 0);
    }
    for (const VirtualGeometryCluster& cluster : asset.clusters) {
        writer.f32(cluster.bounds.min.x);
        writer.f32(cluster.bounds.min.y);
        writer.f32(cluster.bounds.min.z);
        writer.f32(cluster.bounds.max.x);
        writer.f32(cluster.bounds.max.y);
        writer.f32(cluster.bounds.max.z);
        writer.f32(cluster.lod_error_m);
        writer.u32(cluster.page);
        writer.u32(cluster.vertex_offset);
        writer.u32(cluster.vertex_count);
        writer.u32(cluster.first_index);
        writer.u32(cluster.index_count);
        writer.u32(cluster.material_slot);
        writer.u32(cluster.replacement_group);
    }
    for (const VirtualGeometryGroup& group : asset.groups) {
        writer.u32(group.first_cluster);
        writer.u32(group.cluster_count);
        writer.u32(group.first_child);
        writer.u32(group.child_count);
        writer.f32(group.lod_error_m);
        writer.u8(group.permanently_resident ? 1 : 0);
    }
    for (const std::uint32_t child : asset.child_groups)
        writer.u32(child);
    for (const std::uint32_t dependency : asset.page_dependencies)
        writer.u32(dependency);
    writer.bytes(asset.page_bytes);
    return bytes;
}

[[nodiscard]] std::vector<std::byte>
write_file(const VirtualGeometryAsset& asset,
           std::uint64_t schema = virtual_geometry_schema_hash(),
           std::uint32_t payload_version = 1) {
    const std::vector<std::byte> payload = write_payload(asset, payload_version);
    std::vector<std::byte> file;
    rime::core::ByteWriter writer(file);
    writer.bytes(kCookedMagic);
    writer.u16(kContainerVersion);
    writer.u16(static_cast<std::uint16_t>(AssetKind::VirtualGeometry));
    writer.u64(schema);
    writer.u64(payload.size());
    writer.bytes(payload);
    return file;
}

} // namespace

TEST_CASE("virtual geometry: a complete permanently resident coarse cut validates (M18)") {
    const VirtualGeometryAsset asset = valid_asset();
    CHECK(validate_virtual_geometry(asset) == VirtualGeometryError::None);
}

TEST_CASE("virtual geometry: versioned RMA1 payload round-trips") {
    const VirtualGeometryAsset source = valid_asset();
    AssetError error{};
    AssetId id;
    const std::vector<std::byte> file = write_file(source);
    const auto decoded = read_virtual_geometry(file, error, &id);
    REQUIRE_MESSAGE(decoded.has_value(), to_string(error));
    CHECK(decoded->source_mesh == source.source_mesh);
    CHECK(decoded->attribs == source.attribs);
    CHECK(decoded->vertex_stride == source.vertex_stride);
    CHECK(decoded->pages.size() == 1);
    CHECK(decoded->pages[0].byte_size == 108);
    CHECK(decoded->clusters[0].bounds.max.x == doctest::Approx(1.0f));
    CHECK(decoded->groups[0].permanently_resident);
    CHECK(decoded->page_bytes == source.page_bytes);
    CHECK(id == content_hash(write_payload(source)));
}

TEST_CASE("virtual geometry: registry content-addresses the companion payload") {
    const VirtualGeometryAsset source = valid_asset();
    const std::vector<std::byte> file = write_file(source);
    AssetRegistry registry;
    AssetError first_error = AssetError::Io;
    AssetError second_error = AssetError::Io;

    const VirtualGeometryHandle first =
        registry.load_virtual_geometry_from_memory(file, first_error);
    const VirtualGeometryHandle second =
        registry.load_virtual_geometry_from_memory(file, second_error);

    REQUIRE(first.is_valid());
    CHECK(first_error == AssetError::Io);
    CHECK(second == first);
    CHECK(second_error == AssetError::Io);
    CHECK(registry.virtual_geometry_count() == 1);
    REQUIRE(registry.get(first) != nullptr);
    CHECK(registry.get(first)->source_mesh == source.source_mesh);
    CHECK(registry.get(first)->page_bytes == source.page_bytes);
}

TEST_CASE("virtual geometry: registry loads a companion from disk") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rime_m18_virtual_geometry_registry.rvg";
    std::error_code ec;
    const std::vector<std::byte> file = write_file(valid_asset());
    REQUIRE(rime::platform::write_file(path, file));

    AssetRegistry registry;
    const VirtualGeometryHandle handle = registry.load_virtual_geometry(path);
    REQUIRE(handle.is_valid());
    CHECK(registry.get(handle) != nullptr);
    CHECK(registry.get(handle)->coarse_group == 0);

    std::filesystem::remove(path, ec);
}

TEST_CASE("virtual geometry: registry preserves typed reader failures") {
    const VirtualGeometryAsset source = valid_asset();
    AssetRegistry registry;

    SUBCASE("schema mismatch") {
        AssetError error = AssetError::Io;
        const VirtualGeometryHandle handle =
            registry.load_virtual_geometry_from_memory(write_file(source, 0x1234), error);
        CHECK_FALSE(handle.is_valid());
        CHECK(error == AssetError::SchemaMismatch);
        CHECK(registry.virtual_geometry_count() == 0);
    }
    SUBCASE("malformed payload") {
        VirtualGeometryAsset malformed = source;
        malformed.pages[0].byte_size = 109;
        AssetError error = AssetError::Io;
        const VirtualGeometryHandle handle =
            registry.load_virtual_geometry_from_memory(write_file(malformed), error);
        CHECK_FALSE(handle.is_valid());
        CHECK(error == AssetError::InvalidVirtualGeometry);
        CHECK(registry.virtual_geometry_count() == 0);
    }
}

TEST_CASE("virtual geometry: corrupt version, schema, truncation, and graph are rejected") {
    const VirtualGeometryAsset source = valid_asset();
    SUBCASE("unsupported payload version") {
        AssetError error{};
        CHECK_FALSE(
            read_virtual_geometry(write_file(source, virtual_geometry_schema_hash(), 2), error));
        CHECK(error == AssetError::UnsupportedVersion);
    }
    SUBCASE("schema drift") {
        AssetError error{};
        CHECK_FALSE(read_virtual_geometry(write_file(source, 0x1234), error));
        CHECK(error == AssetError::SchemaMismatch);
    }
    SUBCASE("truncated payload") {
        std::vector<std::byte> file = write_file(source);
        file.pop_back();
        AssetError error{};
        CHECK_FALSE(read_virtual_geometry(file, error));
        CHECK(error == AssetError::SizeMismatch);
    }
    SUBCASE("replacement cycle") {
        VirtualGeometryAsset cyclic = source;
        cyclic.groups[0].first_child = 0;
        cyclic.groups[0].child_count = 1;
        cyclic.child_groups.push_back(0);
        AssetError error{};
        CHECK_FALSE(read_virtual_geometry(write_file(cyclic), error));
        CHECK(error == AssetError::InvalidVirtualGeometry);
    }
}

TEST_CASE("virtual geometry: malformed pages and replacement DAGs never reach a renderer (M18)") {
    SUBCASE("a page byte range cannot extend beyond the payload") {
        VirtualGeometryAsset asset = valid_asset();
        asset.pages[0].byte_size = 109;
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

TEST_CASE("virtual geometry: page view exposes checked vertex and index ranges") {
    VirtualGeometryAsset asset = valid_asset();
    for (std::uint32_t i = 0; i < 3; ++i) {
        const std::size_t offset = 96 + i * 4;
        asset.page_bytes[offset] = static_cast<std::byte>(i + 1);
    }
    VirtualGeometryPageView view;
    CHECK(view_virtual_geometry_page(asset, 0, view) == VirtualGeometryPageViewError::None);
    CHECK(view.page.size() == 108);
    CHECK(view.vertices.size() == 96);
    CHECK(view.indices.size() == 12);
    CHECK(view.vertex_offset == 0);
    CHECK(view.index_offset == 96);
    CHECK(view.index_count == 3);
    std::uint32_t decoded = 0;
    CHECK(read_virtual_geometry_index(view, 0, decoded));
    CHECK(decoded == 1);
    CHECK(read_virtual_geometry_index(view, 2, decoded));
    CHECK(decoded == 3);
    CHECK_FALSE(read_virtual_geometry_index(view, 3, decoded));
}

TEST_CASE("virtual geometry: page view rejects malformed layout ranges") {
    SUBCASE("an unaligned page byte offset remains a byte-span view") {
        VirtualGeometryAsset asset = valid_asset();
        asset.page_bytes.insert(asset.page_bytes.begin(), std::byte{0x7f});
        asset.pages[0].byte_offset = 1;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) == VirtualGeometryPageViewError::None);
        CHECK(view.page.data() == asset.page_bytes.data() + 1);
        CHECK(view.page.size() == 108);
    }
    SUBCASE("vertex range exceeds page") {
        VirtualGeometryAsset asset = valid_asset();
        asset.clusters[0].vertex_count = 4;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) ==
              VirtualGeometryPageViewError::OutOfBounds);
    }
    SUBCASE("a non-multiple-of-four index tail is rejected") {
        VirtualGeometryAsset asset = valid_asset();
        asset.page_bytes.push_back(std::byte{0});
        asset.pages[0].byte_size = 109;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) ==
              VirtualGeometryPageViewError::MisalignedIndices);
    }
    SUBCASE("a vertex range with an overflowing offset is rejected") {
        VirtualGeometryAsset asset = valid_asset();
        asset.clusters[0].vertex_offset = UINT32_MAX - 3;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) ==
              VirtualGeometryPageViewError::InvalidLayout);
    }
    SUBCASE("index range exceeds inferred index section") {
        VirtualGeometryAsset asset = valid_asset();
        asset.clusters[0].first_index = 2;
        asset.clusters[0].index_count = 3;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) ==
              VirtualGeometryPageViewError::OutOfBounds);
    }
    SUBCASE("vertex offset is not stride aligned") {
        VirtualGeometryAsset asset = valid_asset();
        asset.clusters[0].vertex_offset = 1;
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(asset, 0, view) ==
              VirtualGeometryPageViewError::InvalidLayout);
    }
    SUBCASE("unknown cluster is rejected") {
        VirtualGeometryPageView view;
        CHECK(view_virtual_geometry_page(valid_asset(), 1, view) ==
              VirtualGeometryPageViewError::InvalidCluster);
    }
    SUBCASE("a malformed little-endian index offset is rejected") {
        const VirtualGeometryAsset asset = valid_asset();
        VirtualGeometryPageView view;
        view.indices = std::span<const std::byte>(asset.page_bytes).subspan(96);
        view.first_index = UINT32_MAX;
        view.index_count = 1;
        std::uint32_t decoded = 0;
        CHECK_FALSE(read_virtual_geometry_index(view, 0, decoded));
    }
}
