// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-material reader (M6.4). Files are assembled by the test-side writer
// (material_fixture.hpp) exactly to the ADR-0024 material layout. ROUND-TRIP: a valid file decodes
// to exactly the factors, alpha mode, and texture-reference AssetIds written, with the right
// content id. NEGATIVE BATTERY: one crafted file per way a material file can be wrong — bad
// envelope, an unknown alpha mode, a non-finite factor, a payload that is short or over-long — each
// a clean typed error and never a crash (ASan/UBSan is the net). A material is a FIXED-size record,
// so "the payload must be exactly this long" is a first-class check here. The reader trusts nothing
// off disk, as always.
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "material_fixture.hpp"
#include "rime/assets/cooked_reader.hpp"

using namespace rime::assets;
using rime_test::MaterialFileBuilder;

namespace {

// Read a built file and expect a specific failure code.
void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<MaterialAsset> mat = read_material(file, error);
    CHECK_FALSE(mat.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("a valid cooked material round-trips to exactly the written data") {
    MaterialFileBuilder builder;
    const std::vector<std::byte> file = builder.build();

    AssetError error{};
    AssetId id;
    const std::optional<MaterialAsset> mat = read_material(file, error, &id);
    REQUIRE_MESSAGE(mat.has_value(), to_string(error));

    CHECK(mat->base_color[0] == doctest::Approx(0.8f));
    CHECK(mat->base_color[1] == doctest::Approx(0.4f));
    CHECK(mat->base_color[2] == doctest::Approx(0.2f));
    CHECK(mat->base_color[3] == doctest::Approx(1.0f));
    CHECK(mat->emissive[0] == doctest::Approx(0.1f));
    CHECK(mat->emissive[1] == doctest::Approx(0.2f));
    CHECK(mat->emissive[2] == doctest::Approx(0.3f));
    CHECK(mat->metallic == doctest::Approx(0.25f));
    CHECK(mat->roughness == doctest::Approx(0.6f));
    CHECK(mat->normal_scale == doctest::Approx(0.5f));
    CHECK(mat->occlusion_strength == doctest::Approx(0.75f));
    CHECK(mat->alpha_cutoff == doctest::Approx(0.3f));
    CHECK(mat->alpha_mode == AlphaMode::Mask);

    // Texture references survive as content ids; the deliberately-zero occlusion slot round-trips
    // as "no texture" so the loader knows to bind the white-AO fallback.
    CHECK(mat->base_color_tex == AssetId{0x1111'1111'1111'1111ULL});
    CHECK(mat->metallic_roughness_tex == AssetId{0x2222'2222'2222'2222ULL});
    CHECK(mat->normal_tex == AssetId{0x3333'3333'3333'3333ULL});
    CHECK(mat->occlusion_tex == kInvalidAssetId);
    CHECK_FALSE(mat->occlusion_tex.is_valid());
    CHECK(mat->emissive_tex == AssetId{0x5555'5555'5555'5555ULL});

    // The identity is the content hash of the payload the file carried.
    CHECK(id == content_hash(builder.build_payload()));
    CHECK(id.is_valid());
}

TEST_CASE("every alpha mode is accepted and preserved") {
    for (const auto mode : {AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend}) {
        MaterialFileBuilder b;
        b.alpha_mode = static_cast<std::uint32_t>(mode);
        AssetError error{};
        const std::optional<MaterialAsset> mat = read_material(b.build(), error);
        REQUIRE_MESSAGE(mat.has_value(), to_string(error));
        CHECK(mat->alpha_mode == mode);
    }
}

TEST_CASE("an all-textures-absent material round-trips (every slot the fallback)") {
    MaterialFileBuilder b;
    b.base_color_tex = 0;
    b.metallic_roughness_tex = 0;
    b.normal_tex = 0;
    b.occlusion_tex = 0;
    b.emissive_tex = 0;
    AssetError error{};
    const std::optional<MaterialAsset> mat = read_material(b.build(), error);
    REQUIRE_MESSAGE(mat.has_value(), to_string(error));
    CHECK_FALSE(mat->base_color_tex.is_valid());
    CHECK_FALSE(mat->metallic_roughness_tex.is_valid());
    CHECK_FALSE(mat->normal_tex.is_valid());
    CHECK_FALSE(mat->occlusion_tex.is_valid());
    CHECK_FALSE(mat->emissive_tex.is_valid());
}

TEST_CASE("material_schema_hash is the reflected v1 material layout: stable, non-zero, golden") {
    CHECK(material_schema_hash() != 0);
    CHECK(material_schema_hash() == material_schema_hash());
    // Golden value: the reflection type_hash of the v1 material record. Pinned as a regression
    // guard (a change to the record or the hashing must be deliberate) and as the exact constant
    // the Rust cooker embeds — the cross-language golden-fixture test checks that the cooker and
    // this reader agree on it. Distinct from the mesh and texture hashes (different records).
    CHECK(material_schema_hash() == 0xCA4ED4CC434C941AULL);
    CHECK(material_schema_hash() != mesh_schema_hash());
    CHECK(material_schema_hash() != texture_schema_hash());
}

TEST_CASE("negative battery: material envelope errors") {
    SUBCASE("bad magic") {
        MaterialFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        MaterialFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind (a texture file read as a material)") {
        MaterialFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Texture);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        MaterialFileBuilder b;
        b.schema_hash = material_schema_hash() ^ 0x1ULL;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        MaterialFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: material content errors") {
    SUBCASE("an unknown alpha mode") {
        MaterialFileBuilder b;
        b.alpha_mode = 3; // 0/1/2 are the only v1 modes (Opaque/Mask/Blend)
        expect_error(b.build(), AssetError::InvalidMaterial);
    }
    SUBCASE("a NaN factor") {
        MaterialFileBuilder b;
        b.roughness = std::numeric_limits<float>::quiet_NaN();
        expect_error(b.build(), AssetError::InvalidMaterial);
    }
    SUBCASE("an infinite factor") {
        MaterialFileBuilder b;
        b.base_color[1] = std::numeric_limits<float>::infinity();
        expect_error(b.build(), AssetError::InvalidMaterial);
    }
    SUBCASE("trailing bytes past the fixed record") {
        MaterialFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = MaterialFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<MaterialAsset> mat = read_material(clipped, error);
        CHECK_FALSE(mat.has_value()); // never a crash, never a partial success
    }
}
