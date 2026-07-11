// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-skeleton reader (M6.7). Files are assembled by the test-side writer
// (skeleton_fixture.hpp) exactly to the ADR-0024 skeleton layout. ROUND-TRIP: a valid file decodes
// to exactly the joint tree, bind pose, and inverse-bind matrices written, in topological order,
// with the right content id. NEGATIVE BATTERY: one crafted file per way a skeleton file can be
// wrong — bad envelope, an empty or impossibly-large joint count, a parent that breaks topological
// order, a non-finite bind value, a size disagreement — each a clean typed error and never a crash
// (ASan/UBSan is the net). The reader trusts nothing off disk, as always.
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "skeleton_fixture.hpp"

using namespace rime::assets;
using rime_test::SkeletonFileBuilder;

namespace {

void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<Skeleton> sk = read_skeleton(file, error);
    CHECK_FALSE(sk.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("a valid cooked skeleton round-trips to exactly the written joint tree") {
    SkeletonFileBuilder builder;
    const std::vector<std::byte> file = builder.build();

    AssetError error{};
    AssetId id;
    const std::optional<Skeleton> sk = read_skeleton(file, error, &id);
    REQUIRE_MESSAGE(sk.has_value(), to_string(error));

    REQUIRE(sk->joint_count() == 2);
    // Root A: no parent, name hash 0xA, identity bind, identity inverse bind.
    CHECK(sk->joints[0].parent == Joint::kNoParent);
    CHECK(sk->joints[0].name_hash == 0xAu);
    CHECK(sk->joints[0].local_bind.translation.x == doctest::Approx(0.0f));
    CHECK(sk->joints[0].inverse_bind.m[12] == doctest::Approx(0.0f));
    // Child B: parent 0, name hash 0xB, translated +2 on X, inverse bind translated -2 on X.
    CHECK(sk->joints[1].parent == 0);
    CHECK(sk->joints[1].name_hash == 0xBu);
    CHECK(sk->joints[1].local_bind.translation.x == doctest::Approx(2.0f));
    CHECK(sk->joints[1].local_bind.rotation.w == doctest::Approx(1.0f)); // identity quaternion
    CHECK(sk->joints[1].local_bind.scale.y == doctest::Approx(1.0f));
    CHECK(sk->joints[1].inverse_bind.m[12] == doctest::Approx(-2.0f));

    // The decoded tree honours the topological-order invariant the sampler relies on, and name
    // lookup resolves both joints.
    CHECK(sk->is_topologically_ordered());
    CHECK(sk->find(0xAu) == 0);
    CHECK(sk->find(0xBu) == 1);

    // Identity is the content hash of the payload the file carried.
    CHECK(id == content_hash(builder.build_payload()));
    CHECK(id.is_valid());
}

TEST_CASE("skeleton_schema_hash is the reflected v1 joint layout: stable, non-zero, golden") {
    CHECK(skeleton_schema_hash() != 0);
    CHECK(skeleton_schema_hash() == skeleton_schema_hash());
    // Golden value: the reflection type_hash of the v1 per-joint record (parent, name hash,
    // inverse-bind matrix, bind-pose TRS). Pinned as a regression guard and as the exact constant
    // the Rust cooker embeds — the cross-language fixture test checks that the cooker and this
    // reader agree on it. Distinct from every other kind's hash.
    CHECK(skeleton_schema_hash() == 0xD90A5CB8EBA36DEDull);
    CHECK(skeleton_schema_hash() != mesh_schema_hash());
    CHECK(skeleton_schema_hash() != material_schema_hash());
    CHECK(skeleton_schema_hash() != clip_schema_hash());
}

TEST_CASE("negative battery: skeleton envelope errors") {
    SUBCASE("bad magic") {
        SkeletonFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("unsupported container version") {
        SkeletonFileBuilder b;
        b.container_version = kContainerVersion + 1;
        expect_error(b.build(), AssetError::UnsupportedVersion);
    }
    SUBCASE("wrong asset kind (a mesh file read as a skeleton)") {
        SkeletonFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Mesh);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        SkeletonFileBuilder b;
        b.schema_hash = skeleton_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        SkeletonFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: skeleton content errors") {
    SUBCASE("an empty skeleton (zero joints)") {
        SkeletonFileBuilder b;
        b.joints.clear();
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("a joint count beyond the u16-addressable ceiling") {
        SkeletonFileBuilder b;
        b.joint_count_override = kMaxJoints + 1; // rejected before any record is read
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("a joint count disagreeing with the records present") {
        SkeletonFileBuilder b;
        b.joint_count_override = 3; // two records written
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("a parent that breaks topological order (references a later joint)") {
        SkeletonFileBuilder b;
        b.joints[0].parent = 1; // the root claims a child that appears after it
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("a joint that is its own parent") {
        SkeletonFileBuilder b;
        b.joints[1].parent = 1;
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("a non-finite inverse-bind value") {
        SkeletonFileBuilder b;
        b.joints[1].inverse_bind[0] = std::numeric_limits<float>::quiet_NaN();
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("a non-finite bind-pose translation") {
        SkeletonFileBuilder b;
        b.joints[0].translation[1] = std::numeric_limits<float>::infinity();
        expect_error(b.build(), AssetError::InvalidSkeleton);
    }
    SUBCASE("trailing bytes past the joint table") {
        SkeletonFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: skeleton truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = SkeletonFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<Skeleton> sk = read_skeleton(clipped, error);
        CHECK_FALSE(sk.has_value()); // never a crash, never a partial success
    }
}
