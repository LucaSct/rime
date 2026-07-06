// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M6.2 cross-language proof: a mesh cooked by the Rust `tools/asset-pipeline` (the committed
// tests/assets/fixtures/quad.rmesh) loads through this C++ reader and decodes to exactly the values
// the source glTF describes. The Rust side's cook_fixture.rs proves the cooker still produces these
// bytes; this side proves the reader ingests them — together, the two languages cannot drift on the
// RMA1 format without a red test. RIME_ASSETS_FIXTURE_DIR is injected by CMake.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/platform/filesystem.hpp"

using namespace rime::assets;

namespace {
// Read a float out of the interleaved vertex blob at a byte offset (memcpy, so no
// aliasing/alignment UB — the negative-battery suite runs under ASan/UBSan).
float blob_f32(const std::vector<std::byte>& blob, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}
} // namespace

TEST_CASE("a Rust-cooked glTF mesh (quad.rmesh) loads and matches its known values") {
    const std::filesystem::path path =
        std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "quad.rmesh";
    const std::optional<std::vector<std::byte>> bytes = rime::platform::read_file(path);
    REQUIRE_MESSAGE(bytes.has_value(), "missing fixture: ", path.string());

    AssetError error{};
    AssetId id;
    const std::optional<MeshAsset> mesh = read_mesh(*bytes, error, &id);
    REQUIRE(mesh.has_value());

    // The quad: 4 vertices, 6 indices, position|normal|uv, one submesh.
    CHECK(mesh->attribs == kMeshV1Attribs);
    CHECK(mesh->vertex_stride == 32);
    CHECK(mesh->vertex_count == 4);
    CHECK(mesh->indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    REQUIRE(mesh->submeshes.size() == 1);
    CHECK(mesh->submeshes[0].first_index == 0);
    CHECK(mesh->submeshes[0].index_count == 6);
    CHECK(id.is_valid());

    // The source node is translated by (1,2,3); the cooker flattened it into the vertices, so the
    // bounds and the first vertex are offset by it.
    CHECK(mesh->bounds.min.x == doctest::Approx(1.0f));
    CHECK(mesh->bounds.min.y == doctest::Approx(2.0f));
    CHECK(mesh->bounds.min.z == doctest::Approx(3.0f));
    CHECK(mesh->bounds.max.x == doctest::Approx(2.0f));
    CHECK(mesh->bounds.max.y == doctest::Approx(3.0f));

    // Vertex 0: local (0,0,0) → world (1,2,3), normal +Z, uv (0,0). Layout: px,py,pz,nx,ny,nz,u,v.
    REQUIRE(mesh->vertices.size() == 4u * 32u);
    CHECK(blob_f32(mesh->vertices, 0) == doctest::Approx(1.0f));  // px
    CHECK(blob_f32(mesh->vertices, 4) == doctest::Approx(2.0f));  // py
    CHECK(blob_f32(mesh->vertices, 8) == doctest::Approx(3.0f));  // pz
    CHECK(blob_f32(mesh->vertices, 20) == doctest::Approx(1.0f)); // nz
    CHECK(blob_f32(mesh->vertices, 24) == doctest::Approx(0.0f)); // u
}
