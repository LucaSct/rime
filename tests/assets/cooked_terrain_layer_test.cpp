// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-terrain-layer reader (M19.7a, ADR-0066). The shape of
// cooked_heightfield_test.cpp, for a fixed record like the material's:
//
//   * SCHEMA HASH — pinned, non-zero, distinct from every other kind's fingerprint.
//   * A HAND-BUILT FILE round-trips, field for field, and its id is the payload's content hash.
//   * NEGATIVE BATTERY — one crafted file per way a terrain layer can be wrong (envelope, kind,
//     schema, size, each id, each uv_scale axis, the contrast), each a clean typed error AND each
//     tallied by reason in the caller's AssetRejectCounters.
//   * TRUNCATION at every byte length fails cleanly and is counted every time.
//
// The Rust-cooked fixture (terrain_layer.rtl + its packed texture) is loaded in fixture_test.cpp
// with the other cross-language proofs. doctest's main() lives in cooked_mesh_test.cpp.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

// Every field of the payload, so each negative case corrupts exactly one of them. Distinct,
// non-default values, so a round trip that swapped two fields could not pass.
struct LayerSpec {
    std::uint64_t material = 0x1234'5678'9ABC'DEF0ull;
    std::uint64_t albedo_height = 0x0FED'CBA9'8765'4321ull;
    float uv_x = 2.5f;
    float uv_z = 4.0f;
    float contrast = 8.0f;
};

std::vector<std::byte> payload_of(const LayerSpec& s) {
    std::vector<std::byte> p;
    put_u64(p, s.material);
    put_u64(p, s.albedo_height);
    put_f32(p, s.uv_x);
    put_f32(p, s.uv_z);
    put_f32(p, s.contrast);
    return p;
}

std::vector<std::byte>
wrap(const std::vector<std::byte>& payload,
     std::uint16_t kind = static_cast<std::uint16_t>(AssetKind::TerrainLayer),
     std::uint64_t schema = terrain_layer_schema_hash(),
     std::uint16_t container_version = kContainerVersion) {
    std::vector<std::byte> f{std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'1'}};
    put_u16(f, container_version);
    put_u16(f, kind);
    put_u64(f, schema);
    put_u64(f, payload.size());
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// Load `file`, expect refusal with `expected`, and expect the counter to have seen exactly that.
void expect_rejected(const std::vector<std::byte>& file, AssetError expected) {
    AssetRejectCounters rejects;
    AssetError error = AssetError::Io;
    const std::optional<TerrainLayerAsset> layer =
        read_terrain_layer(file, error, nullptr, &rejects);
    CHECK_FALSE(layer.has_value());
    CHECK_MESSAGE(error == expected, to_string(error));
    CHECK(rejects.total == 1);
    CHECK(rejects.count(expected) == 1);
}

} // namespace

TEST_CASE("terrain layer schema hash is pinned and distinct from every other kind") {
    // The value the Rust cooker embeds (cooked.rs TERRAIN_LAYER_SCHEMA_HASH). If this changes, the
    // TerrainLayerV1 record changed: update the Rust constant and re-cook terrain_layer.rtl.
    CHECK(terrain_layer_schema_hash() == 0xFE7832A27003413Cull);
    CHECK(terrain_layer_schema_hash() != 0);
    CHECK(terrain_layer_schema_hash() != mesh_schema_hash());
    CHECK(terrain_layer_schema_hash() != texture_schema_hash());
    CHECK(terrain_layer_schema_hash() != material_schema_hash());
    CHECK(terrain_layer_schema_hash() != sdf_schema_hash());
    CHECK(terrain_layer_schema_hash() != destructible_schema_hash());
    CHECK(terrain_layer_schema_hash() != heightfield_schema_hash());
    // The wire value is part of the format: append-only, never renumbered.
    CHECK(static_cast<std::uint16_t>(AssetKind::TerrainLayer) == 10);
}

TEST_CASE("a hand-built terrain layer round-trips field for field") {
    const LayerSpec spec;
    AssetRejectCounters rejects;
    AssetError error = AssetError::Io;
    AssetId id;
    const std::optional<TerrainLayerAsset> layer =
        read_terrain_layer(wrap(payload_of(spec)), error, &id, &rejects);
    REQUIRE_MESSAGE(layer.has_value(), to_string(error));
    CHECK(rejects.total == 0);
    CHECK(id == content_hash(payload_of(spec)));
    CHECK(payload_of(spec).size() == 28); // the fixed record: 2 x u64 + 3 x f32
    CHECK(layer->material.value == spec.material);
    CHECK(layer->albedo_height.value == spec.albedo_height);
    CHECK(layer->uv_scale[0] == 2.5f);
    CHECK(layer->uv_scale[1] == 4.0f);
    CHECK(layer->height_contrast == 8.0f);
}

TEST_CASE("a zero height_contrast is valid: it is today's blend, not an error") {
    LayerSpec s;
    s.contrast = 0.0f;
    AssetError error = AssetError::Io;
    const std::optional<TerrainLayerAsset> layer = read_terrain_layer(wrap(payload_of(s)), error);
    REQUIRE_MESSAGE(layer.has_value(), to_string(error));
    CHECK(layer->height_contrast == 0.0f);
}

TEST_CASE("the terrain layer reader refuses every malformed file, and counts each refusal") {
    const LayerSpec good;
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();

    SUBCASE("envelope: bad magic, container version, wrong kind, schema") {
        std::vector<std::byte> f = wrap(payload_of(good));
        f[0] = std::byte{'X'};
        expect_rejected(f, AssetError::BadMagic);
        expect_rejected(wrap(payload_of(good),
                             static_cast<std::uint16_t>(AssetKind::TerrainLayer),
                             terrain_layer_schema_hash(),
                             kContainerVersion + 1),
                        AssetError::UnsupportedVersion);
        // A MATERIAL file handed to the terrain-layer reader: the kind dispatch a builder relies
        // on to tell the two apart (ADR-0066) must refuse, not misread.
        expect_rejected(wrap(payload_of(good),
                             static_cast<std::uint16_t>(AssetKind::Material),
                             terrain_layer_schema_hash()),
                        AssetError::WrongKind);
        expect_rejected(wrap(payload_of(good),
                             static_cast<std::uint16_t>(AssetKind::TerrainLayer),
                             material_schema_hash()),
                        AssetError::SchemaMismatch);
    }

    SUBCASE("a trailing byte is a size mismatch, not ignored") {
        std::vector<std::byte> p = payload_of(good);
        p.push_back(std::byte{0});
        expect_rejected(wrap(p), AssetError::SizeMismatch);
    }

    SUBCASE("zero ids") {
        LayerSpec s = good;
        s.material = 0;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidTerrainLayer);
        s = good;
        s.albedo_height = 0;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidTerrainLayer);
    }

    SUBCASE("uv_scale: zero, negative, NaN, inf — on each axis") {
        for (const float bad : {0.0f, -1.0f, -0.0f, kNan, kInf, -kInf}) {
            LayerSpec s = good;
            s.uv_x = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidTerrainLayer);
            s = good;
            s.uv_z = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidTerrainLayer);
        }
    }

    SUBCASE("height_contrast: negative, NaN, inf") {
        for (const float bad : {-1.0f, -1e-30f, kNan, kInf, -kInf}) {
            LayerSpec s = good;
            s.contrast = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidTerrainLayer);
        }
    }
}

TEST_CASE("a terrain layer truncated at every byte length is refused cleanly and counted") {
    const std::vector<std::byte> payload = payload_of(LayerSpec{});
    // Cut the PAYLOAD (re-wrapping with the shorter length, so the envelope stays consistent and
    // the decoder itself must notice), then cut the whole FILE (the envelope's own size check).
    AssetRejectCounters rejects;
    for (std::size_t n = 0; n < payload.size(); ++n) {
        const std::vector<std::byte> cut(payload.begin(),
                                         payload.begin() + static_cast<std::ptrdiff_t>(n));
        AssetError error = AssetError::Io;
        CHECK_FALSE(read_terrain_layer(wrap(cut), error, nullptr, &rejects).has_value());
        CHECK(error == AssetError::Truncated);
    }
    CHECK(rejects.total == payload.size());
    CHECK(rejects.count(AssetError::Truncated) == payload.size());

    const std::vector<std::byte> file = wrap(payload);
    AssetRejectCounters file_rejects;
    for (std::size_t n = 0; n < file.size(); ++n) {
        const std::span<const std::byte> cut(file.data(), n);
        AssetError error = AssetError::Io;
        CHECK_FALSE(read_terrain_layer(cut, error, nullptr, &file_rejects).has_value());
    }
    CHECK(file_rejects.total == file.size());
}
