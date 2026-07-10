// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked animation-clip reader (M6.7). Files are assembled by the test-side
// writer (clip_fixture.hpp) exactly to the ADR-0024 clip layout. ROUND-TRIP: a valid file decodes to
// exactly the per-joint TRS tracks written — each channel reconstructed into the right joint, path,
// interpolation, times, and values, with silent joints left empty — and with the right content id.
// NEGATIVE BATTERY: one crafted file per way a clip file can be wrong — bad envelope, a bad channel
// path/interpolation, a zero-key or out-of-range channel, non-monotonic times, a non-finite value, a
// table/blob size disagreement — each a clean typed error and never a crash (ASan/UBSan is the net).
//
// doctest's main() lives in cooked_mesh_test.cpp (shared across the rime_assets_tests exe).

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "clip_fixture.hpp"
#include "rime/assets/cooked_reader.hpp"

using namespace rime::assets;
using rime_test::ClipChannelRecord;
using rime_test::ClipFileBuilder;

namespace {

void expect_error(const std::vector<std::byte>& file, AssetError expected) {
    AssetError error{};
    const std::optional<Clip> clip = read_clip(file, error);
    CHECK_FALSE(clip.has_value());
    CHECK(error == expected);
}

} // namespace

TEST_CASE("a valid cooked clip round-trips to exactly the written tracks") {
    ClipFileBuilder builder;
    const std::vector<std::byte> file = builder.build();

    AssetError error{};
    AssetId id;
    const std::optional<Clip> clip = read_clip(file, error, &id);
    REQUIRE_MESSAGE(clip.has_value(), to_string(error));

    CHECK(clip->duration == doctest::Approx(1.0f));
    REQUIRE(clip->joint_count() == 2); // dense: one JointAnimation per joint, silent ones empty

    // The one channel: a LINEAR translation on joint 0, two keys (0,0,0) → (6,0,0).
    const JointAnimation& j0 = clip->joints[0];
    REQUIRE_FALSE(j0.translation.empty());
    CHECK(j0.translation.interp == Interpolation::Linear);
    REQUIRE(j0.translation.times.size() == 2);
    CHECK(j0.translation.times[0] == doctest::Approx(0.0f));
    CHECK(j0.translation.times[1] == doctest::Approx(1.0f));
    REQUIRE(j0.translation.values.size() == 2);
    CHECK(j0.translation.values[0].x == doctest::Approx(0.0f));
    CHECK(j0.translation.values[1].x == doctest::Approx(6.0f));
    // Its other tracks, and joint 1 entirely, are silent → the sampler falls back to the bind pose.
    CHECK(j0.rotation.empty());
    CHECK(j0.scale.empty());
    CHECK(clip->joints[1].translation.empty());
    CHECK(clip->joints[1].rotation.empty());
    CHECK(clip->joints[1].scale.empty());

    CHECK(id == content_hash(builder.build_payload()));
    CHECK(id.is_valid());
}

TEST_CASE("multiple channels reconstruct into the right joint, path, and interpolation") {
    ClipFileBuilder builder;
    builder.joint_count = 2;
    // A STEP rotation on joint 1 (four floats per quaternion key), a LINEAR scale on joint 0, and the
    // default LINEAR translation on joint 0 — three channels spanning both joints and all three paths.
    ClipChannelRecord rot;
    rot.target_joint = 1;
    rot.path = 1; // rotation
    rot.interp = 0; // step
    rot.times = {0.0f, 0.5f};
    rot.values = {0, 0, 0, 1, 0, 0, 1, 0}; // identity, then 180° about +Z (x,y,z,w)
    ClipChannelRecord scl;
    scl.target_joint = 0;
    scl.path = 2; // scale
    scl.interp = 1; // linear
    scl.times = {0.0f, 1.0f};
    scl.values = {1, 1, 1, 2, 2, 2};
    builder.channels = {ClipChannelRecord{}, rot, scl};

    AssetError error{};
    const std::optional<Clip> clip = read_clip(builder.build(), error);
    REQUIRE_MESSAGE(clip.has_value(), to_string(error));

    // joint 0: translation (default) + scale, no rotation.
    CHECK_FALSE(clip->joints[0].translation.empty());
    CHECK_FALSE(clip->joints[0].scale.empty());
    CHECK(clip->joints[0].rotation.empty());
    CHECK(clip->joints[0].scale.interp == Interpolation::Linear);
    CHECK(clip->joints[0].scale.values[1].y == doctest::Approx(2.0f));
    // joint 1: a STEP rotation whose second key is the 180°-about-Z quaternion.
    REQUIRE_FALSE(clip->joints[1].rotation.empty());
    CHECK(clip->joints[1].rotation.interp == Interpolation::Step);
    CHECK(clip->joints[1].rotation.values[1].z == doctest::Approx(1.0f));
    CHECK(clip->joints[1].rotation.values[1].w == doctest::Approx(0.0f));
    CHECK(clip->joints[1].translation.empty());
}

TEST_CASE("clip_schema_hash is the reflected v1 channel record: stable, non-zero, golden") {
    CHECK(clip_schema_hash() != 0);
    CHECK(clip_schema_hash() == clip_schema_hash());
    // Golden value: the reflection type_hash of the v1 channel record (target joint, path, interp,
    // key count). Pinned as a regression guard and as the exact constant the Rust cooker embeds.
    CHECK(clip_schema_hash() == 0x6C84D2A2AAABCE49ull);
    CHECK(clip_schema_hash() != mesh_schema_hash());
    CHECK(clip_schema_hash() != skeleton_schema_hash());
}

TEST_CASE("negative battery: clip envelope errors") {
    SUBCASE("bad magic") {
        ClipFileBuilder b;
        b.magic = {std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'2'}};
        expect_error(b.build(), AssetError::BadMagic);
    }
    SUBCASE("wrong asset kind (a skeleton file read as a clip)") {
        ClipFileBuilder b;
        b.kind = static_cast<std::uint16_t>(AssetKind::Skeleton);
        expect_error(b.build(), AssetError::WrongKind);
    }
    SUBCASE("schema hash mismatch") {
        ClipFileBuilder b;
        b.schema_hash = clip_schema_hash() ^ 0x1ull;
        expect_error(b.build(), AssetError::SchemaMismatch);
    }
    SUBCASE("payload_size larger than the bytes present") {
        ClipFileBuilder b;
        b.payload_size_override = b.build_payload().size() + 8;
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: clip content errors") {
    SUBCASE("a non-finite duration") {
        ClipFileBuilder b;
        b.duration = std::numeric_limits<float>::quiet_NaN();
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a negative duration") {
        ClipFileBuilder b;
        b.duration = -1.0f;
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("zero joints") {
        ClipFileBuilder b;
        b.joint_count = 0;
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a joint count beyond the u16-addressable ceiling") {
        ClipFileBuilder b;
        b.joint_count_override = kMaxJoints + 1;
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a channel targeting a joint outside the count") {
        ClipFileBuilder b;
        b.channels[0].target_joint = 2; // joint_count is 2 → valid indices are 0,1
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("an unknown channel path") {
        ClipFileBuilder b;
        b.channels[0].path = 3; // 0/1/2 are the only paths (T/R/S)
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("an unknown interpolation mode") {
        ClipFileBuilder b;
        b.channels[0].interp = 2; // 0/1 are the only modes (step/linear)
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a zero-key channel") {
        ClipFileBuilder b;
        b.channels[0].times.clear();
        b.channels[0].values.clear();
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("non-monotonic times") {
        ClipFileBuilder b;
        b.channels[0].times = {1.0f, 0.0f}; // must be strictly increasing
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a non-finite keyframe value") {
        ClipFileBuilder b;
        b.channels[0].values[3] = std::numeric_limits<float>::infinity();
        expect_error(b.build(), AssetError::InvalidClip);
    }
    SUBCASE("a key count disagreeing with the blob (table implies more bytes)") {
        ClipFileBuilder b;
        b.channels[0].key_count_override = 3; // two keys actually written
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("a channel count disagreeing with the records present") {
        ClipFileBuilder b;
        b.channel_count_override = 5; // one record written → the table alone overruns the payload
        expect_error(b.build(), AssetError::SizeMismatch);
    }
    SUBCASE("trailing bytes past the keyframe blob") {
        ClipFileBuilder b;
        b.trailing = std::vector<std::byte>(4, std::byte{0xEE});
        expect_error(b.build(), AssetError::SizeMismatch);
    }
}

TEST_CASE("negative battery: clip truncation at every length fails cleanly") {
    const std::vector<std::byte> valid = ClipFileBuilder{}.build();
    for (std::size_t len = 0; len < valid.size(); ++len) {
        const std::span<const std::byte> clipped(valid.data(), len);
        AssetError error{};
        const std::optional<Clip> clip = read_clip(clipped, error);
        CHECK_FALSE(clip.has_value());
    }
}
