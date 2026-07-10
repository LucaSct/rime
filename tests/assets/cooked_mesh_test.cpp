// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-mesh reader (M6.1). Files are assembled by the shared test-side writer
// (mesh_fixture.hpp) exactly to the ADR-0024 spec. ROUND-TRIP: a valid file decodes to exactly the
// data written, with the right content id. NEGATIVE BATTERY: one crafted file per way a file can be
// wrong — bad magic, wrong version/kind/schema, truncation at every boundary, size disagreements,
// broken layout, an out-of-range index, a bad submesh — each a clean typed error and never a crash
// (ASan/UBSan is the net). The engine trusts nothing it reads off disk.
//
// This source file supplies doctest's main() for the whole rime_assets_tests executable.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mesh_fixture.hpp"
#include "rime/assets/cooked_reader.hpp"

using namespace rime::assets;
using rime::core::ByteWriter;
using rime_test::MeshFileBuilder;

namespace {

// Read a built file and expect a specific failure code.
void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<MeshAsset> mesh = read_mesh(file, error);
    CHECK_FALSE(mesh.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("a valid cooked mesh round-trips to exactly the written data") {
    // A known vertex blob: three vertices of eight distinct floats each, so the compared bytes are
    // not all zero.
    std::vector<std::byte> blob;
    ByteWriter bw(blob);
    for (int v = 0; v < 3; ++v) {
        for (int c = 0; c < 8; ++c) {
            bw.f32(static_cast<float>(v * 10 + c));
        }
    }
    REQUIRE(blob.size() == 3 * 32);

    MeshFileBuilder builder;
    builder.vertex_blob = blob;
    builder.indices = std::vector<std::uint32_t>{0, 1, 2};
    const std::vector<std::byte> file = builder.build();

    AssetError error{};
    AssetId id;
    const std::optional<MeshAsset> mesh = read_mesh(file, error, &id);
    REQUIRE(mesh.has_value());

    CHECK(mesh->attribs == kMeshV1Attribs);
    CHECK(mesh->vertex_stride == 32);
    CHECK(mesh->vertex_count == 3);
    CHECK(mesh->indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(mesh->vertices == blob);
    CHECK(mesh->bounds.min.x == -1.0f);
    CHECK(mesh->bounds.max.z == 3.0f);
    REQUIRE(mesh->submeshes.size() == 1);
    CHECK(mesh->submeshes[0].first_index == 0);
    CHECK(mesh->submeshes[0].index_count == 3);

    // The identity is the content hash of the payload the file carried.
    const std::vector<std::byte> payload = builder.build_payload();
    CHECK(id == content_hash(payload));
    CHECK(id.is_valid());
}

TEST_CASE("a skinned mesh (joints + weights) round-trips through the additive attribute seam") {
    // Skinning adds two per-vertex attributes — u16×4 joint indices and f32×4 weights — exactly as
    // tangents did (ADR-0024 decision 6): more flag bits and a wider stride, NOT a new container
    // version or schema hash. The reader already knows these bits (they were reserved at M6.0), so
    // a skinned mesh decodes with no reader change — this pins that. Stride is P(12)+N(12)+UV(8) +
    // Joints(8) + Weights(16) = 56, re-derived from the flags by the reader.
    const VertexAttribs skinned = VertexAttribs::Position | VertexAttribs::Normal |
                                  VertexAttribs::Uv | VertexAttribs::Joints |
                                  VertexAttribs::Weights;
    const std::uint32_t stride = expected_vertex_stride(skinned);
    CHECK(stride == 56);

    // Three vertices whose joints/weights carry real, distinct values, so the opaque blob the
    // reader copies back is not all zeros. Attribute order matches expected_vertex_stride: P, N,
    // UV, then the u16 joints, then the f32 weights.
    std::vector<std::byte> blob;
    ByteWriter bw(blob);
    for (int v = 0; v < 3; ++v) {
        for (int c = 0; c < 8; ++c) { // position(3) + normal(3) + uv(2)
            bw.f32(static_cast<float>(v * 10 + c));
        }
        for (int j = 0; j < 4; ++j) {
            bw.u16(static_cast<std::uint16_t>(v * 4 + j)); // joint indices
        }
        bw.f32(0.5f); // weights, normalized to sum to 1
        bw.f32(0.25f);
        bw.f32(0.15f);
        bw.f32(0.10f);
    }
    REQUIRE(blob.size() == 3 * 56);

    MeshFileBuilder builder;
    builder.attribs = static_cast<std::uint32_t>(skinned);
    builder.stride = stride;
    builder.vertex_count = 3;
    builder.vertex_blob = blob;
    builder.indices = std::vector<std::uint32_t>{0, 1, 2};

    AssetError error{};
    const std::optional<MeshAsset> mesh = read_mesh(builder.build(), error);
    REQUIRE_MESSAGE(mesh.has_value(), to_string(error));
    CHECK(has_attrib(mesh->attribs, VertexAttribs::Joints));
    CHECK(has_attrib(mesh->attribs, VertexAttribs::Weights));
    CHECK(mesh->vertex_stride == 56);
    CHECK(mesh->vertices == blob); // the blob is opaque to the reader; skinning is AN1's concern

    // A stride that disagrees with the skinned flag set is still caught (the flags are the source
    // of truth for addressing), just as it is for a plain mesh.
    MeshFileBuilder wrong = builder;
    wrong.stride = 48; // the tangented stride, not the skinned one
    expect_error(wrong.build(), AssetError::InvalidLayout);
}

TEST_CASE("mesh_schema_hash is the reflected v1 vertex layout: stable, non-zero, golden") {
    CHECK(mesh_schema_hash() != 0);
    CHECK(mesh_schema_hash() == mesh_schema_hash());
    // Golden value: the reflection type_hash of the v1 position/normal/uv vertex layout. Pinned as
    // a regression guard (a change to the layout or to the hashing must be deliberate) and as the
    // exact constant the Rust cooker embeds at M6.2 — the cross-language golden-fixture test checks
    // that the cooker and this reader agree on it.
    CHECK(mesh_schema_hash() == 0x198738A2DDE250ACull);
}

TEST_CASE("negative battery: envelope errors") {
    SUBCASE("bad magic") {
        MeshFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        MeshFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind") {
        MeshFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Mesh) + 1; // a not-yet-defined kind
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        MeshFileBuilder b;
        b.schema_hash = mesh_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        MeshFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("trailing bytes inside the payload") {
        MeshFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        // payload_size counts the trailing bytes, so the envelope is consistent; the mesh decoder
        // then finds more bytes than its counts account for.
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = MeshFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<MeshAsset> mesh = read_mesh(clipped, error);
        CHECK_FALSE(mesh.has_value()); // never a crash, never a partial success
    }

    // Spot-check the boundary semantics: a cut inside the fixed header is Truncated; a cut that
    // leaves a whole header but a short payload is a SizeMismatch (the header's payload_size no
    // longer matches the bytes that follow).
    expect_error({valid.begin(), valid.begin() + 3}, AssetError::Truncated);
    expect_error({valid.begin(), valid.begin() + 10}, AssetError::Truncated);
    expect_error({valid.begin(), valid.begin() + kCookedHeaderSize}, AssetError::SizeMismatch);
}

TEST_CASE("negative battery: payload/layout errors") {
    SUBCASE("an unknown attribute bit") {
        MeshFileBuilder b;
        b.attribs = static_cast<std::uint32_t>(kMeshV1Attribs) | (1u << 6); // bit 6 is undefined
        expect_error(b.build(), AssetError::InvalidLayout);
    }
    SUBCASE("position attribute missing") {
        MeshFileBuilder b;
        b.attribs = static_cast<std::uint32_t>(VertexAttribs::Normal | VertexAttribs::Uv);
        b.stride =
            expected_vertex_stride(VertexAttribs::Normal | VertexAttribs::Uv); // self-consistent
        expect_error(b.build(), AssetError::InvalidLayout);
    }
    SUBCASE("stride disagrees with the attribute flags") {
        MeshFileBuilder b;
        b.stride = 48; // flags say 32
        expect_error(b.build(), AssetError::InvalidLayout);
    }
    SUBCASE("index count is not a whole number of triangles") {
        MeshFileBuilder b;
        b.index_count = 4;
        b.indices = std::vector<std::uint32_t>{0, 1, 2, 0};
        b.submeshes = {Submesh{0, 4, 0}};
        expect_error(b.build(), AssetError::InvalidLayout);
    }
    SUBCASE("an index references a nonexistent vertex") {
        MeshFileBuilder b;
        b.indices = std::vector<std::uint32_t>{0, 1, 5}; // vertex_count is 3
        expect_error(b.build(), AssetError::IndexOutOfRange);
    }
    SUBCASE("a submesh range runs past the index buffer") {
        MeshFileBuilder b;
        b.submeshes = {Submesh{0, 6, 0}}; // 0 + 6 > 3 indices
        expect_error(b.build(), AssetError::BadSubmesh);
    }
}
