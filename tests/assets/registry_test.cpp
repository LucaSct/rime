// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the AssetRegistry (M6.1): content-addressed de-duplication (loading the same bytes
// twice coalesces to one stored asset and one handle), distinct content yields distinct handles, a
// stale/invalid handle reads back as nullptr, and the synchronous load_mesh(path) reads a cooked
// file from disk — failing cleanly on a missing file rather than crashing.

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <vector>

#include "mesh_fixture.hpp"
#include "rime/assets/registry.hpp"
#include "rime/platform/filesystem.hpp"

using namespace rime::assets;
using rime_test::MeshFileBuilder;

TEST_CASE("loading the same content twice returns the same handle and stores it once") {
    AssetRegistry registry;
    const std::vector<std::byte> file = MeshFileBuilder{}.build();

    AssetError e1{};
    AssetError e2{};
    const MeshHandle h1 = registry.load_mesh_from_memory(file, e1);
    const MeshHandle h2 = registry.load_mesh_from_memory(file, e2);

    REQUIRE(h1.is_valid());
    CHECK(h1 == h2); // content-addressed identity coalesces the two loads
    CHECK(registry.mesh_count() == 1);
    REQUIRE(registry.get(h1) != nullptr);
    CHECK(registry.get(h1)->vertex_count == 3);
}

TEST_CASE("different content yields different handles") {
    AssetRegistry registry;
    MeshFileBuilder a;
    MeshFileBuilder b;
    b.aabb_max = {9.0f, 9.0f, 9.0f}; // change the payload bytes => different content hash

    AssetError err{};
    const MeshHandle ha = registry.load_mesh_from_memory(a.build(), err);
    const MeshHandle hb = registry.load_mesh_from_memory(b.build(), err);

    REQUIRE(ha.is_valid());
    REQUIRE(hb.is_valid());
    CHECK(ha != hb);
    CHECK(registry.mesh_count() == 2);
}

TEST_CASE("an invalid handle reads back as nullptr") {
    AssetRegistry registry;
    CHECK(registry.get(MeshHandle{}) == nullptr);
}

TEST_CASE("a corrupt in-memory file fails with the reader's error and stores nothing") {
    AssetRegistry registry;
    MeshFileBuilder b;
    b.magic = {std::byte{'N'}, std::byte{'O'}, std::byte{'P'}, std::byte{'E'}};
    AssetError error{};
    const MeshHandle handle = registry.load_mesh_from_memory(b.build(), error);
    CHECK_FALSE(handle.is_valid());
    CHECK(error == AssetError::BadMagic);
    CHECK(registry.mesh_count() == 0);
}

TEST_CASE("load_mesh reads a cooked file from disk") {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    REQUIRE_FALSE(ec);
    const std::filesystem::path path = dir / "rime_m6_1_registry_test.rmesh";

    const std::vector<std::byte> file = MeshFileBuilder{}.build();
    REQUIRE(rime::platform::write_file(path, file));

    AssetRegistry registry;
    const MeshHandle handle = registry.load_mesh(path);
    REQUIRE(handle.is_valid());
    REQUIRE(registry.get(handle) != nullptr);
    CHECK(registry.get(handle)->indices.size() == 3);

    // A missing file fails cleanly (an invalid handle), never a crash.
    const MeshHandle missing = registry.load_mesh(dir / "rime_m6_1_does_not_exist.rmesh");
    CHECK_FALSE(missing.is_valid());

    std::filesystem::remove(path, ec);
}
