// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-mesh-SDF reader (M10.4a, ADR-0032 §2). Mirrors
// cooked_destructible_test.cpp's shape:
//
//   * SCHEMA HASH — pinned, non-zero, distinct from every other kind's fingerprint.
//   * A HAND-BUILT MINIMAL FILE round-trips (the negative battery's baseline).
//   * NEGATIVE BATTERY — one crafted file per way an SDF file can be wrong (bad envelope, wrong
//     kind/schema, a size disagreement, an inconsistent header value), each a clean typed error and
//     never a crash (ASan/UBSan is the net).
//   * TRUNCATION at every byte length fails cleanly.
//
// The cross-language fixture (cube.rsdf, cooked from the existing cube.stl fixture) and its
// analytic-correctness checks live in fixture_test.cpp, alongside the other cross-language proofs.
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "rime/assets/cooked_reader.hpp"

using namespace rime::assets;

namespace {

void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}

void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_f32(std::vector<std::byte>& b, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits);
}

// ── A minimal reader-valid SDF file, for the negative battery ───────────────────────────────────
// A tiny 2x2x2 volume (8 voxels) — enough to pass every structural check the reader makes, so a
// single corrupted field is what each case isolates. Little-endian, field by field, exactly as the
// Rust cooker writes and the reader reads (docs/design/assets.md).
struct SdfFileBuilder {
    std::array<std::byte, 4> magic = kCookedMagic;
    std::uint16_t container_version = kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(AssetKind::MeshSdf);
    std::uint64_t schema_hash = sdf_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    float amin[3] = {-1.0f, -1.0f, -1.0f};
    float amax[3] = {1.0f, 1.0f, 1.0f};
    float origin[3] = {-1.0f, -1.0f, -1.0f};
    float voxel_size = 1.0f;
    std::uint32_t res[3] = {2, 2, 2};
    std::uint32_t encoding = 0; // Float32
    float max_abs_distance = 1.5f;
    // 8 voxels, every value inside [-max_abs_distance, max_abs_distance].
    std::vector<float> distances = {-1.5f, -0.5f, 0.5f, 1.5f, -1.0f, 0.0f, 1.0f, 1.2f};

    std::vector<std::byte> trailing;

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> p;
        for (const float f : amin) {
            put_f32(p, f);
        }
        for (const float f : amax) {
            put_f32(p, f);
        }
        for (const float f : origin) {
            put_f32(p, f);
        }
        put_f32(p, voxel_size);
        for (const std::uint32_t r : res) {
            put_u32(p, r);
        }
        put_u32(p, encoding);
        put_f32(p, max_abs_distance);
        for (const float d : distances) {
            put_f32(p, d);
        }
        return p;
    }

    [[nodiscard]] std::vector<std::byte> build() const {
        const std::vector<std::byte> payload = build_payload();
        std::vector<std::byte> file;
        file.insert(file.end(), magic.begin(), magic.end());
        put_u16(file, container_version);
        put_u16(file, kind);
        put_u64(file, schema_hash);
        put_u64(file, payload_size_override.value_or(payload.size()));
        file.insert(file.end(), payload.begin(), payload.end());
        file.insert(file.end(), trailing.begin(), trailing.end());
        return file;
    }
};

void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<MeshSdfAsset> asset = read_mesh_sdf(file, error);
    CHECK_FALSE(asset.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("sdf_schema_hash is the reflected v1 header layout: stable, non-zero, golden") {
    CHECK(sdf_schema_hash() != 0);
    CHECK(sdf_schema_hash() == sdf_schema_hash());
    // Golden value: the reflection type_hash of the v1 SDF header record. Pinned as a regression
    // guard and as the exact constant the Rust cooker embeds (MESH_SDF_SCHEMA_HASH) — the fixture
    // round-trip in fixture_test.cpp is the cross-language check that both agree on it.
    CHECK(sdf_schema_hash() == 0x6EFFA98119033990ull);
    CHECK(sdf_schema_hash() != mesh_schema_hash());
    CHECK(sdf_schema_hash() != texture_schema_hash());
    CHECK(sdf_schema_hash() != material_schema_hash());
    CHECK(sdf_schema_hash() != skeleton_schema_hash());
    CHECK(sdf_schema_hash() != clip_schema_hash());
    CHECK(sdf_schema_hash() != destructible_schema_hash());
}

TEST_CASE("a hand-built minimal SDF round-trips (the negative battery's baseline)") {
    AssetError error{};
    AssetId id;
    const std::optional<MeshSdfAsset> asset = read_mesh_sdf(SdfFileBuilder{}.build(), error, &id);
    REQUIRE_MESSAGE(asset.has_value(), to_string(error));
    CHECK(id.is_valid());
    CHECK(asset->resolution[0] == 2);
    CHECK(asset->resolution[1] == 2);
    CHECK(asset->resolution[2] == 2);
    CHECK(asset->voxel_count() == 8);
    CHECK(asset->distances.size() == 8);
    CHECK(asset->encoding == SdfEncoding::Float32);
    CHECK(asset->voxel_size == doctest::Approx(1.0f));
    CHECK(asset->max_abs_distance == doctest::Approx(1.5f));
    CHECK(asset->local_bounds.min.x == doctest::Approx(-1.0f));
    CHECK(asset->local_bounds.max.x == doctest::Approx(1.0f));
    // index(): x fastest, then y, then z. distances = {-1.5,-0.5,0.5,1.5,-1.0,0.0,1.0,1.2}, so
    // (1,0,0) -> index 1, (0,1,0) -> index 2 (= 0 + 2*(1+2*0)), (0,0,1) -> index 4 (= 0 +
    // 2*(0+2*1)).
    CHECK(asset->distances[asset->index(1, 0, 0)] == doctest::Approx(-0.5f));
    CHECK(asset->distances[asset->index(0, 1, 0)] == doctest::Approx(0.5f));
    CHECK(asset->distances[asset->index(0, 0, 1)] == doctest::Approx(-1.0f));
}

TEST_CASE("negative battery: SDF envelope errors") {
    SUBCASE("bad magic") {
        SdfFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        SdfFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind (a destructible file read as an SDF)") {
        SdfFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Destructible);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        SdfFileBuilder b;
        b.schema_hash = sdf_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        SdfFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("trailing bytes past the distances blob") {
        SdfFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: SDF content errors") {
    SUBCASE("zero resolution on one axis") {
        SdfFileBuilder b;
        b.res[1] = 0;
        b.distances.assign(2 * 0 * 2, 0.0f); // keep the blob consistent with the (invalid) count
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("resolution above the sanity ceiling") {
        SdfFileBuilder b;
        b.res[0] = 2000; // > kMaxSdfResolutionPerAxis (1024); reject before sizing any allocation
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("non-positive voxel size") {
        SdfFileBuilder b;
        b.voxel_size = 0.0f;
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("an inverted (min > max) local AABB") {
        SdfFileBuilder b;
        b.amin[0] = 5.0f; // now > amax[0] == 1.0f
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("an unknown encoding") {
        SdfFileBuilder b;
        b.encoding = 7;
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("a non-finite header value") {
        SdfFileBuilder b;
        b.voxel_size = std::numeric_limits<float>::quiet_NaN();
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("a negative max_abs_distance") {
        SdfFileBuilder b;
        b.max_abs_distance = -1.0f;
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("a non-finite distance sample") {
        SdfFileBuilder b;
        b.distances[0] = std::numeric_limits<float>::infinity();
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
    SUBCASE("a distance sample exceeding the recorded max_abs_distance") {
        SdfFileBuilder b;
        b.distances[0] = 100.0f; // far past max_abs_distance == 1.5 — a corrupt/tampered file
        expect_error(b.build(), AssetError::InvalidMeshSdf);
    }
}

TEST_CASE("negative battery: SDF truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = SdfFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<MeshSdfAsset> asset = read_mesh_sdf(clipped, error);
        CHECK_FALSE(asset.has_value()); // never a crash, never a partial success
    }
}
