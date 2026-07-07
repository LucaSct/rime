// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/core/byte_cursor.hpp"

// A test-side writer for RMA1 cooked-material files, the sibling of texture_fixture.hpp's
// TextureFileBuilder. It defaults to a valid, non-trivial material (every field a distinct value so a
// round-trip test can tell them apart); a negative test overrides exactly one field to isolate the
// failure it provokes. A material has no variable-length tail, so this is just the fixed record inside
// the standard container envelope. The Rust cooker is the real writer from M6.4 on — the checked-in
// golden fixture cross-checks the two languages — but a programmatic builder lets the negative battery
// craft files no honest cooker would emit.
namespace rime_test {

struct MaterialFileBuilder {
    std::array<std::byte, 4> magic = rime::assets::kCookedMagic;
    std::uint16_t container_version = rime::assets::kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(rime::assets::AssetKind::Material);
    std::uint64_t schema_hash = rime::assets::material_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    // Factor fields, in wire order (must match decode_material / MaterialV1). Distinct, non-default
    // values so a round-trip test asserts each landed in the right place.
    float base_color[4] = {0.8f, 0.4f, 0.2f, 1.0f};
    float emissive[3] = {0.1f, 0.2f, 0.3f};
    float metallic = 0.25f;
    float roughness = 0.6f;
    float normal_scale = 0.5f;
    float occlusion_strength = 0.75f;
    float alpha_cutoff = 0.3f;
    std::uint32_t alpha_mode = static_cast<std::uint32_t>(rime::assets::AlphaMode::Mask);

    // Texture-reference AssetIds. occlusion is 0 on purpose — it exercises the "no texture" slot that
    // the loader turns into a fallback, and proves 0 round-trips as kInvalidAssetId.
    std::uint64_t base_color_tex = 0x1111'1111'1111'1111ULL;
    std::uint64_t metallic_roughness_tex = 0x2222'2222'2222'2222ULL;
    std::uint64_t normal_tex = 0x3333'3333'3333'3333ULL;
    std::uint64_t occlusion_tex = 0;
    std::uint64_t emissive_tex = 0x5555'5555'5555'5555ULL;

    std::vector<std::byte> trailing; // extra bytes counted into the payload (the over-long case)

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> payload;
        rime::core::ByteWriter w(payload);
        w.f32(base_color[0]);
        w.f32(base_color[1]);
        w.f32(base_color[2]);
        w.f32(base_color[3]);
        w.f32(emissive[0]);
        w.f32(emissive[1]);
        w.f32(emissive[2]);
        w.f32(metallic);
        w.f32(roughness);
        w.f32(normal_scale);
        w.f32(occlusion_strength);
        w.f32(alpha_cutoff);
        w.u32(alpha_mode);
        w.u64(base_color_tex);
        w.u64(metallic_roughness_tex);
        w.u64(normal_tex);
        w.u64(occlusion_tex);
        w.u64(emissive_tex);
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
