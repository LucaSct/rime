// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-texture reader (M6.3). Files are assembled by the test-side writer
// (texture_fixture.hpp) exactly to the ADR-0024 texture layout. ROUND-TRIP: a valid file decodes to
// exactly the extent, format, mip table, and pixels written, with the right content id. NEGATIVE
// BATTERY: one crafted file per way a texture file can be wrong — bad envelope, unknown format, a
// mip table inconsistent with the base extent (wrong extent / size / offset / count), a pixel blob
// that is short or over-long — each a clean typed error and never a crash (ASan/UBSan is the net).
// The reader trusts nothing off disk, exactly as the mesh reader does.
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "texture_fixture.hpp"

using namespace rime::assets;
using rime_test::TextureFileBuilder;

namespace {

// Read a built file and expect a specific failure code.
void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<TextureAsset> tex = read_texture(file, error);
    CHECK_FALSE(tex.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("a valid cooked texture round-trips to exactly the written data") {
    TextureFileBuilder builder; // 4×2 sRGB, a 3-level chain
    const std::vector<std::byte> file = builder.build();

    AssetError error{};
    AssetId id;
    const std::optional<TextureAsset> tex = read_texture(file, error, &id);
    REQUIRE_MESSAGE(tex.has_value(), to_string(error));

    CHECK(tex->width == 4);
    CHECK(tex->height == 2);
    CHECK(tex->format == TextureFormat::Rgba8Srgb);

    // 4×2 → 2×1 → 1×1, sizes 32 / 8 / 4 bytes, offsets tiling the 44-byte blob.
    REQUIRE(tex->mips.size() == 3);
    CHECK(tex->mips[0].width == 4);
    CHECK(tex->mips[0].height == 2);
    CHECK(tex->mips[0].offset == 0);
    CHECK(tex->mips[0].size == 32);
    CHECK(tex->mips[1].width == 2);
    CHECK(tex->mips[1].height == 1);
    CHECK(tex->mips[1].offset == 32);
    CHECK(tex->mips[1].size == 8);
    CHECK(tex->mips[2].width == 1);
    CHECK(tex->mips[2].height == 1);
    CHECK(tex->mips[2].offset == 40);
    CHECK(tex->mips[2].size == 4);

    // Each level was filled with a distinct constant (0x10, 0x20, 0x30) by the builder: the pixels
    // survive verbatim, and each mip slices the blob at its own offset.
    REQUIRE(tex->pixels.size() == 44);
    CHECK(tex->pixels[0] == std::byte{0x10});
    CHECK(tex->pixels[31] == std::byte{0x10});
    CHECK(tex->pixels[32] == std::byte{0x20});
    CHECK(tex->pixels[39] == std::byte{0x20});
    CHECK(tex->pixels[40] == std::byte{0x30});
    CHECK(tex->pixels[43] == std::byte{0x30});

    // The identity is the content hash of the payload the file carried.
    CHECK(id == content_hash(builder.build_payload()));
    CHECK(id.is_valid());
}

TEST_CASE("a linear (UNORM) texture is accepted and its format preserved") {
    TextureFileBuilder builder;
    builder.format = static_cast<std::uint32_t>(TextureFormat::Rgba8Unorm);
    AssetError error{};
    const std::optional<TextureAsset> tex = read_texture(builder.build(), error);
    REQUIRE_MESSAGE(tex.has_value(), to_string(error));
    CHECK(tex->format == TextureFormat::Rgba8Unorm);
}

TEST_CASE("a 1×1 texture is a single-level chain") {
    TextureFileBuilder builder;
    builder.width = 1;
    builder.height = 1;
    AssetError error{};
    const std::optional<TextureAsset> tex = read_texture(builder.build(), error);
    REQUIRE_MESSAGE(tex.has_value(), to_string(error));
    REQUIRE(tex->mips.size() == 1);
    CHECK(tex->mips[0].size == 4);
    CHECK(tex->pixels.size() == 4);
}

TEST_CASE("texture_schema_hash is the reflected v1 mip-record layout: stable, non-zero, golden") {
    CHECK(texture_schema_hash() != 0);
    CHECK(texture_schema_hash() == texture_schema_hash());
    // Golden value: the reflection type_hash of the v1 {width,height,offset,size} mip record. Pinned
    // as a regression guard (a change to the record or the hashing must be deliberate) and as the
    // exact constant the Rust cooker embeds — the cross-language golden-fixture test checks that the
    // cooker and this reader agree on it. Distinct from the mesh hash (different record).
    CHECK(texture_schema_hash() == 0xAB8A2B884141F736ull);
    CHECK(texture_schema_hash() != mesh_schema_hash());
}

TEST_CASE("negative battery: texture envelope errors") {
    SUBCASE("bad magic") {
        TextureFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        TextureFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind (a mesh file read as a texture)") {
        TextureFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Mesh);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        TextureFileBuilder b;
        b.schema_hash = texture_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        TextureFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: texture header/format errors") {
    SUBCASE("an unknown pixel format") {
        TextureFileBuilder b;
        b.format = 2; // 0/1 are the only v1 formats; BC values are reserved, not yet cooked
        expect_error(b.build(), AssetError::InvalidTexture);
    }
    SUBCASE("a zero extent") {
        TextureFileBuilder b;
        b.width = 0;
        expect_error(b.build(), AssetError::InvalidTexture);
    }
    SUBCASE("mip_count disagrees with the base extent") {
        TextureFileBuilder b;
        b.mip_count_override = full_mip_count(b.width, b.height) + 1; // must be exactly the chain
        expect_error(b.build(), AssetError::InvalidTexture);
    }
}

TEST_CASE("negative battery: mip table inconsistent with the base extent is rejected") {
    SUBCASE("a level's extent is not the base halved") {
        TextureFileBuilder b;
        auto table = b.default_table();
        table[1].width = 3; // must be 2 (=4>>1)
        b.mip_table = table;
        expect_error(b.build(), AssetError::InvalidTexture);
    }
    SUBCASE("a level's byte size is not width*height*4") {
        TextureFileBuilder b;
        auto table = b.default_table();
        table[1].size += 4; // 8 is the only correct size for a 2×1 RGBA8 level
        b.mip_table = table;
        expect_error(b.build(), AssetError::InvalidTexture);
    }
    SUBCASE("a level's offset leaves a gap in the blob") {
        TextureFileBuilder b;
        auto table = b.default_table();
        table[1].offset += 4; // offsets must tile with no gap/overlap
        b.mip_table = table;
        expect_error(b.build(), AssetError::InvalidTexture);
    }
}

TEST_CASE("negative battery: pixel blob length errors") {
    SUBCASE("the blob is shorter than the mip sizes sum to") {
        TextureFileBuilder b;
        std::vector<std::byte> px = TextureFileBuilder::default_pixels(b.default_table());
        px.resize(px.size() - 4); // drop the last level's last texel
        b.pixels = px;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("trailing bytes past the last mip") {
        TextureFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = TextureFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<TextureAsset> tex = read_texture(clipped, error);
        CHECK_FALSE(tex.has_value()); // never a crash, never a partial success
    }
}
