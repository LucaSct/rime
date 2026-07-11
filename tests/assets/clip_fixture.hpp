// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/byte_cursor.hpp"

// A test-side writer for RMA1 cooked animation-clip files, the sibling of skeleton_fixture.hpp's
// builder. It defaults to a valid clip with one LINEAR translation channel (so a round-trip has
// real keyframes to compare) and lets a negative test override exactly one field — a channel's
// path, interpolation, key count, its times/values, or the envelope — to isolate the failure it
// provokes. A clip is a header + a sparse channel table + a keyframe blob (times then values per
// channel, in table order), so the builder writes those three parts and, deliberately, does NOT
// force key_count to match the times/values it emits — that lets the battery craft a table/blob
// size disagreement.
namespace rime_test {

// One channel in wire order. `path`: 0 = translation, 1 = rotation, 2 = scale. `interp`: 0 = step,
// 1 = linear. `times` and `values` are written verbatim into the blob; `key_count_override`, when
// set, is written into the table instead of times.size() (to craft a size mismatch).
struct ClipChannelRecord {
    std::uint32_t target_joint = 0;
    std::uint32_t path = 0;   // translation
    std::uint32_t interp = 1; // linear
    std::vector<float> times = {0.0f, 1.0f};
    std::vector<float> values = {0.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f}; // two Vec3 keys
    std::optional<std::uint32_t> key_count_override;

    [[nodiscard]] std::uint32_t key_count() const {
        return key_count_override.value_or(static_cast<std::uint32_t>(times.size()));
    }
};

struct ClipFileBuilder {
    std::array<std::byte, 4> magic = rime::assets::kCookedMagic;
    std::uint16_t container_version = rime::assets::kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(rime::assets::AssetKind::AnimationClip);
    std::uint64_t schema_hash = rime::assets::clip_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    float duration = 1.0f;
    std::uint32_t joint_count = 2;
    std::optional<std::uint32_t> joint_count_override;
    std::optional<std::uint32_t> channel_count_override; // craft a count ≠ the records written

    std::vector<ClipChannelRecord> channels = {ClipChannelRecord{}};
    std::vector<std::byte> trailing;

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> payload;
        rime::core::ByteWriter w(payload);
        w.f32(duration);
        w.u32(joint_count_override.value_or(joint_count));
        w.u32(channel_count_override.value_or(static_cast<std::uint32_t>(channels.size())));
        // The channel table, then the keyframe blob (times then values, per channel, in table
        // order).
        for (const ClipChannelRecord& c : channels) {
            w.u32(c.target_joint);
            w.u32(c.path);
            w.u32(c.interp);
            w.u32(c.key_count());
        }
        for (const ClipChannelRecord& c : channels) {
            for (const float t : c.times) {
                w.f32(t);
            }
            for (const float v : c.values) {
                w.f32(v);
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
