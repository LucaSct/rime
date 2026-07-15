// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-destructible reader (M8.1, ADR-0029). Two halves:
//
//   * CROSS-LANGUAGE ORACLE — the committed tests/assets/fixtures/wall.rdest (cooked by the Rust
//     fracture cook; its cook_fixture.rs re-cooks the identical bytes) decodes here, and every part
//     REGISTERS into a real rime::physics::PhysicsWorld: register_hull accepts each convex part
//     (its trust-nothing validator — closed/convex/outward/positive-volume — is the cook's true
//     acceptance gate), register_compound accepts the whole pattern, and the summed hull volumes
//     match the source box. This is the M8.1 "done when": the cooker and the runtime agree on the
//     geometry, checked through the seam, not by eyeballing.
//   * NEGATIVE BATTERY — one crafted file per way a destructible file can be wrong (bad envelope,
//     wrong kind/schema, a size disagreement, an out-of-range index, a bad face size, a non-finite
//     or non-positive value), each a clean typed error and never a crash (ASan/UBSan is the net).
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"

using namespace rime::assets;

namespace {

std::optional<std::vector<std::byte>> load_fixture(const char* name) {
    return rime::platform::read_file(std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / name);
}

// ── A minimal reader-valid destructible file, for the negative battery ──────────────────────────
// One part: a tetrahedron (4 vertices, 4 triangular faces) — enough to pass every structural check
// the reader makes, so a single corrupted field is what each case isolates. (The real geometry the
// oracle registers comes from the committed fixture, not this.) Little-endian, field by field,
// exactly as the Rust cooker writes and the reader reads.
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

struct DestructibleFileBuilder {
    std::array<std::byte, 4> magic = kCookedMagic;
    std::uint16_t container_version = kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(AssetKind::Destructible);
    std::uint64_t schema_hash = destructible_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    // Header counts (mutable so a case can make them disagree with the blobs).
    std::uint32_t part_count = 1;
    std::uint32_t bond_count = 0;
    std::uint32_t anchor_count = 0;
    std::uint32_t total_verts = 4;
    std::uint32_t total_face_counts = 4;
    std::uint32_t total_face_indices = 12;

    // The one part's record + geometry (a tetra).
    float volume = 1.0f;
    std::uint32_t vertex_count = 4;
    std::uint32_t face_count = 4;
    std::uint32_t face_index_count = 12;
    std::vector<float> verts = {1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, 1}; // 4 × xyz
    std::vector<std::uint32_t> face_counts = {3, 3, 3, 3};
    std::vector<std::uint32_t> face_indices = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};

    std::vector<std::byte> trailing;

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> p;
        put_u32(p, part_count);
        put_u32(p, bond_count);
        put_u32(p, anchor_count);
        put_u32(p, total_verts);
        put_u32(p, total_face_counts);
        put_u32(p, total_face_indices);
        put_f32(p, 1.0f); // half x/y/z
        put_f32(p, 1.0f);
        put_f32(p, 1.0f);
        put_f32(p, 5.0f); // damage threshold / scale
        put_f32(p, 1.0f);
        // The one part record: com, aabb min/max, volume, then the three counts.
        for (int i = 0; i < 3; ++i) {
            put_f32(p, 0.0f); // com
        }
        for (int i = 0; i < 3; ++i) {
            put_f32(p, -1.0f); // aabb min
        }
        for (int i = 0; i < 3; ++i) {
            put_f32(p, 1.0f); // aabb max
        }
        put_f32(p, volume);
        put_u32(p, vertex_count);
        put_u32(p, face_count);
        put_u32(p, face_index_count);
        // Geometry blobs.
        for (const float f : verts) {
            put_f32(p, f);
        }
        for (const std::uint32_t c : face_counts) {
            put_u32(p, c);
        }
        for (const std::uint32_t ix : face_indices) {
            put_u32(p, ix);
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
    const std::optional<DestructibleAsset> asset = read_destructible(file, error);
    CHECK_FALSE(asset.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("destructible_schema_hash is the reflected v1 part layout: stable, non-zero, golden") {
    CHECK(destructible_schema_hash() != 0);
    CHECK(destructible_schema_hash() == destructible_schema_hash());
    // Golden value: the reflection type_hash of the v1 per-part record. Pinned as a regression
    // guard and as the exact constant the Rust cooker embeds (DESTRUCTIBLE_SCHEMA_HASH) — the
    // fixture round-trip below is the cross-language check that both agree on it.
    CHECK(destructible_schema_hash() == 0x8F2D17FBF58485E2ull);
    CHECK(destructible_schema_hash() != mesh_schema_hash());
    CHECK(destructible_schema_hash() != texture_schema_hash());
    CHECK(destructible_schema_hash() != material_schema_hash());
    CHECK(destructible_schema_hash() != skeleton_schema_hash());
    CHECK(destructible_schema_hash() != clip_schema_hash());
}

TEST_CASE("the committed wall.rdest fixture decodes to a valid partition") {
    const auto file = load_fixture("wall.rdest");
    REQUIRE_MESSAGE(file.has_value(), "wall.rdest fixture is missing");

    AssetError error{};
    AssetId id;
    const std::optional<DestructibleAsset> asset = read_destructible(*file, error, &id);
    REQUIRE_MESSAGE(asset.has_value(), to_string(error));
    CHECK(id.is_valid());

    // A real wall: several parts, a shared-face graph, and parts anchored to its base.
    CHECK(asset->part_count() >= 8);
    CHECK_FALSE(asset->bonds.empty());
    CHECK_FALSE(asset->anchors.empty());
    CHECK(asset->half_extents.x == doctest::Approx(1.0f));
    CHECK(asset->half_extents.y == doctest::Approx(0.75f));
    CHECK(asset->half_extents.z == doctest::Approx(0.15f));

    for (const DestructiblePart& part : asset->parts) {
        CHECK(part.volume > 0.0f);
        CHECK(part.vertices.size() >= 4);
        CHECK(part.face_counts.size() >= 4);
        std::uint32_t index_total = 0;
        for (const std::uint32_t c : part.face_counts) {
            CHECK(c >= 3);
            CHECK(c <= 16);
            index_total += c;
        }
        CHECK(index_total == part.face_indices.size());
        for (const std::uint32_t ix : part.face_indices) {
            CHECK(ix < part.vertices.size());
        }
    }
    // Bonds are canonical (a < b, both real parts); anchors name real parts.
    for (const DestructibleBond& b : asset->bonds) {
        CHECK(b.a < b.b);
        CHECK(b.b < asset->part_count());
        CHECK(b.strength > 0.0f);
    }
    for (const std::uint32_t a : asset->anchors) {
        CHECK(a < asset->part_count());
    }
}

TEST_CASE("the M8.1 oracle: every cooked part registers into a real PhysicsWorld") {
    // The done-when. The runtime's register_hull is the cook's real acceptance test — a part that
    // is not a genuine closed/convex/outward hull with positive volume comes back invalid.
    // Registering every part, and the whole pattern as one compound, proves the Rust cook produces
    // exactly the geometry the C++ physics core validates.
    const auto file = load_fixture("wall.rdest");
    REQUIRE(file.has_value());
    AssetError error{};
    const std::optional<DestructibleAsset> asset = read_destructible(*file, error);
    REQUIRE_MESSAGE(asset.has_value(), to_string(error));

    rime::physics::PhysicsWorld world;

    // register_hull each part; collect the ids for the compound and sum the world's own volumes.
    std::vector<rime::physics::HullId> hull_ids;
    hull_ids.reserve(asset->part_count());
    float summed_hull_volume = 0.0f;
    float summed_part_volume = 0.0f;
    for (const DestructiblePart& part : asset->parts) {
        const rime::physics::HullDesc desc{std::span<const rime::core::Vec3>(part.vertices),
                                           std::span<const std::uint32_t>(part.face_counts),
                                           std::span<const std::uint32_t>(part.face_indices)};
        const rime::physics::HullId id = world.register_hull(desc);
        REQUIRE(id.is_valid()); // the cook's geometry is a hull the validator accepts

        rime::physics::HullInfo info;
        REQUIRE(world.hull_info(id, info));
        summed_hull_volume += info.volume;
        summed_part_volume += part.volume;
        hull_ids.push_back(id);
    }

    // The whole intact pattern is ONE compound body: each part a hull child at its COM (identity
    // rotation — parts are not rotated relative to the destructible). 16 parts is well under the
    // ADR-0028 256-child cap.
    std::vector<rime::physics::CompoundChildDesc> children;
    children.reserve(hull_ids.size());
    for (std::size_t i = 0; i < hull_ids.size(); ++i) {
        rime::physics::CompoundChildDesc child;
        child.shape.type = rime::physics::ShapeType::ConvexHull;
        child.shape.hull = hull_ids[i];
        child.position = asset->parts[i].com;
        children.push_back(child);
    }
    const rime::physics::CompoundId compound =
        world.register_compound(rime::physics::CompoundDesc{std::span(children)});
    CHECK(compound.is_valid());

    // Volume agreement, three ways: the world's integrated hull volumes ≈ the cook's part volumes ≈
    // the source box (2 × 1.5 × 0.3 m). ~1% tolerance absorbs the float paths on both sides.
    const float box_volume = (2.0f * 1.0f) * (2.0f * 0.75f) * (2.0f * 0.15f);
    CHECK(summed_hull_volume == doctest::Approx(summed_part_volume).epsilon(0.01));
    CHECK(summed_hull_volume == doctest::Approx(box_volume).epsilon(0.01));

    rime::physics::CompoundInfo cinfo;
    REQUIRE(world.compound_info(compound, cinfo));
    CHECK(cinfo.volume == doctest::Approx(box_volume).epsilon(0.01));
    CHECK(cinfo.child_count == asset->part_count());
}

TEST_CASE("a hand-built minimal destructible round-trips (the negative battery's baseline)") {
    // The builder's default is a valid one-part file — so a failing negative case is the
    // corruption, not a broken baseline.
    AssetError error{};
    const std::optional<DestructibleAsset> asset =
        read_destructible(DestructibleFileBuilder{}.build(), error);
    REQUIRE_MESSAGE(asset.has_value(), to_string(error));
    CHECK(asset->part_count() == 1);
    CHECK(asset->parts[0].vertices.size() == 4);
    CHECK(asset->parts[0].face_counts.size() == 4);
    CHECK(asset->parts[0].face_indices.size() == 12);
}

TEST_CASE("negative battery: destructible envelope errors") {
    SUBCASE("bad magic") {
        DestructibleFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        DestructibleFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind (a skeleton file read as a destructible)") {
        DestructibleFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Skeleton);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        DestructibleFileBuilder b;
        b.schema_hash = destructible_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        DestructibleFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("trailing bytes past the tables") {
        DestructibleFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: destructible content errors") {
    SUBCASE("zero parts") {
        DestructibleFileBuilder b;
        b.part_count = 0;
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("per-part counts disagreeing with the header totals") {
        DestructibleFileBuilder b;
        b.total_verts = 5; // the part still declares 4 → sums won't match
        expect_error(b.build(),
                     AssetError::SizeMismatch); // caught first by the payload-length check
    }
    SUBCASE("a face-vertex count below the 3 minimum") {
        DestructibleFileBuilder b;
        b.face_counts[0] = 2;
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("a face-vertex count above the 16 hull cap") {
        DestructibleFileBuilder b;
        b.face_counts[0] = 17;
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("a face index that references a non-existent vertex") {
        DestructibleFileBuilder b;
        b.face_indices[0] = 99; // >= vertex_count (4)
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("a non-positive part volume") {
        DestructibleFileBuilder b;
        b.volume = 0.0f;
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("a non-finite vertex") {
        DestructibleFileBuilder b;
        b.verts[0] = std::numeric_limits<float>::quiet_NaN();
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
    SUBCASE("too few vertices to be a solid") {
        DestructibleFileBuilder b;
        b.vertex_count = 3;
        b.total_verts = 3;
        b.verts.resize(9); // keep the blob consistent with the count so the size check passes
        expect_error(b.build(), AssetError::InvalidDestructible);
    }
}

TEST_CASE("negative battery: destructible truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = DestructibleFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<DestructibleAsset> asset = read_destructible(clipped, error);
        CHECK_FALSE(asset.has_value()); // never a crash, never a partial success
    }
}
