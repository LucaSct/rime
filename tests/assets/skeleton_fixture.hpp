// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/skeleton_asset.hpp"
#include "rime/core/byte_cursor.hpp"

// A test-side writer for RMA1 cooked-skeleton files, the sibling of material_fixture.hpp's
// MaterialFileBuilder. It defaults to a valid two-joint skeleton (a root and one child, so the
// topological-order invariant is actually exercised) with distinct values a round-trip test can
// tell apart; a negative test overrides exactly one field to isolate the failure it provokes. A
// skeleton is a fixed-size joint table, so this is a straight run of the standard container
// envelope plus the joint records. The Rust cooker is the real writer from this brick on — the
// checked-in golden fixture cross-checks the two — but a programmatic builder lets the negative
// battery craft files no honest cooker would emit.
namespace rime_test {

// One joint record in wire order: parent, name hash, the 16-float column-major inverse-bind matrix,
// then the bind-pose local translation / rotation (quaternion x,y,z,w) / scale.
struct SkelJointRecord {
    std::int32_t parent = rime::assets::Joint::kNoParent;
    std::uint64_t name_hash = 0;
    std::array<float, 16> inverse_bind = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 3> translation = {0.0f, 0.0f, 0.0f};
    std::array<float, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
};

struct SkeletonFileBuilder {
    std::array<std::byte, 4> magic = rime::assets::kCookedMagic;
    std::uint16_t container_version = rime::assets::kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(rime::assets::AssetKind::Skeleton);
    std::uint64_t schema_hash = rime::assets::skeleton_schema_hash();
    std::optional<std::uint64_t> payload_size_override;
    std::optional<std::uint32_t> joint_count_override; // craft a count ≠ the records written

    // A root joint A at the origin, and a child B two units along +X whose inverse bind is the
    // inverse of that placement — distinct, non-default values throughout so a round-trip asserts
    // each landed in the right joint and field.
    std::vector<SkelJointRecord> joints = {
        SkelJointRecord{rime::assets::Joint::kNoParent,
                        0xAu,
                        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
                        {0, 0, 0},
                        {0, 0, 0, 1},
                        {1, 1, 1}},
        SkelJointRecord{0,
                        0xBu,
                        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 0, 0, 1},
                        {2, 0, 0},
                        {0, 0, 0, 1},
                        {1, 1, 1}},
    };
    std::vector<std::byte> trailing;

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> payload;
        rime::core::ByteWriter w(payload);
        w.u32(joint_count_override.value_or(static_cast<std::uint32_t>(joints.size())));
        for (const SkelJointRecord& j : joints) {
            w.i32(j.parent);
            w.u64(j.name_hash);
            for (const float m : j.inverse_bind) {
                w.f32(m);
            }
            for (const float t : j.translation) {
                w.f32(t);
            }
            for (const float r : j.rotation) {
                w.f32(r);
            }
            for (const float s : j.scale) {
                w.f32(s);
            }
        }
        w.bytes(trailing);
        return payload;
    }

    [[nodiscard]] std::vector<std::byte> build() const {
        const std::vector<std::byte> payload = build_payload();
        std::vector<std::byte> file;
        rime::core::ByteWriter w(file);
        w.bytes(magic);
        w.u16(container_version);
        w.u16(kind);
        w.u64(schema_hash);
        w.u64(payload_size_override.value_or(payload.size()));
        w.bytes(payload);
        return file;
    }
};

} // namespace rime_test
