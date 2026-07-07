// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/core/byte_cursor.hpp"

// A test-side writer for RMA1 cooked-texture files, the sibling of mesh_fixture.hpp's
// MeshFileBuilder. It defaults to a valid, full-mip-chain RGBA8 texture; a negative test overrides
// exactly one field (or mutates one entry of `default_table()`) to isolate the failure it provokes.
// The Rust cooker is the real writer from M6.3 on — the checked-in golden fixture cross-checks the
// two languages — but a programmatic builder is what lets the negative battery craft files no
// honest cooker would emit.
namespace rime_test {

struct TextureFileBuilder {
    // One mip-table record, matching decode_texture's {width, height, offset, size} layout.
    struct Mip {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t offset;
        std::uint32_t size;
    };

    std::array<std::byte, 4> magic = rime::assets::kCookedMagic;
    std::uint16_t container_version = rime::assets::kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(rime::assets::AssetKind::Texture);
    std::uint64_t schema_hash = rime::assets::texture_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    // A 4×2 texture cooks to a 3-level chain (4×2 → 2×1 → 1×1); small but exercises non-square
    // halving and the 1×1 floor. sRGB by default (the baseColor common case).
    std::uint32_t width = 4;
    std::uint32_t height = 2;
    std::uint32_t format = static_cast<std::uint32_t>(rime::assets::TextureFormat::Rgba8Srgb);
    std::optional<std::uint32_t> mip_count_override; // default: the table's length
    std::optional<std::vector<Mip>> mip_table;       // default: default_table()
    std::optional<std::vector<std::byte>> pixels;    // default: default_pixels(default_table())
    std::vector<std::byte> trailing;                 // extra bytes counted into the payload

    // The correct full-chain mip table for (width, height): each level's halved extent, its
    // width*height*4 byte size, and the running offset that tiles the blob.
    [[nodiscard]] std::vector<Mip> default_table() const {
        std::vector<Mip> table;
        const std::uint32_t levels = rime::assets::full_mip_count(width, height);
        std::uint32_t offset = 0;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::uint32_t w = rime::assets::mip_extent(width, level);
            const std::uint32_t h = rime::assets::mip_extent(height, level);
            const std::uint32_t size = w * h * rime::assets::kTextureBytesPerPixel;
            table.push_back(Mip{w, h, offset, size});
            offset += size;
        }
        return table;
    }

    // Pixels sized to a table, each level filled with a distinct constant byte (0x10, 0x20, …) so a
    // round-trip test can assert which bytes belong to which level.
    [[nodiscard]] static std::vector<std::byte> default_pixels(const std::vector<Mip>& table) {
        std::vector<std::byte> px;
        for (std::size_t level = 0; level < table.size(); ++level) {
            const auto value = static_cast<std::byte>(0x10 * (level + 1));
            px.insert(px.end(), table[level].size, value);
        }
        return px;
    }

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        const std::vector<Mip> table = mip_table.value_or(default_table());
        const std::vector<std::byte> px = pixels.value_or(default_pixels(table));

        std::vector<std::byte> payload;
        rime::core::ByteWriter w(payload);
        w.u32(width);
        w.u32(height);
        w.u32(format);
        w.u32(mip_count_override.value_or(static_cast<std::uint32_t>(table.size())));
        for (const Mip& m : table) {
            w.u32(m.width);
            w.u32(m.height);
            w.u32(m.offset);
            w.u32(m.size);
        }
        w.bytes(px);
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
