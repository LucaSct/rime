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

// A test-side writer for RMA1 cooked-mesh files, shared by the reader and registry tests. It is the
// only writer of the format until the Rust cooker takes over at M6.2 (the golden fixture then
// cross-checks the two). Every field defaults to a valid single-triangle mesh; a negative test
// overrides exactly one field to isolate the failure it provokes.
namespace rime_test {

struct MeshFileBuilder {
    std::array<std::byte, 4> magic = rime::assets::kCookedMagic;
    std::uint16_t container_version = rime::assets::kContainerVersion;
    std::uint16_t kind = static_cast<std::uint16_t>(rime::assets::AssetKind::Mesh);
    std::uint64_t schema_hash = rime::assets::mesh_schema_hash();
    std::optional<std::uint64_t> payload_size_override;

    std::uint32_t attribs = static_cast<std::uint32_t>(rime::assets::kMeshV1Attribs);
    std::uint32_t stride = 32;
    std::uint32_t vertex_count = 3;
    std::uint32_t index_count = 3;
    std::array<float, 3> aabb_min = {-1.0f, -2.0f, -3.0f};
    std::array<float, 3> aabb_max = {1.0f, 2.0f, 3.0f};
    std::vector<rime::assets::Submesh> submeshes = {rime::assets::Submesh{0, 3, 0}};
    std::optional<std::vector<std::byte>> vertex_blob; // default: vertex_count*stride zero bytes
    std::optional<std::vector<std::uint32_t>> indices; // default: 0..index_count-1 (all valid)
    std::vector<std::byte> trailing;                   // extra bytes counted into the payload

    [[nodiscard]] std::vector<std::byte> build_payload() const {
        std::vector<std::byte> payload;
        rime::core::ByteWriter w(payload);
        w.u32(attribs);
        w.u32(stride);
        w.u32(vertex_count);
        w.u32(index_count);
        for (const float m : aabb_min) {
            w.f32(m);
        }
        for (const float m : aabb_max) {
            w.f32(m);
        }
        w.u32(static_cast<std::uint32_t>(submeshes.size()));
        for (const rime::assets::Submesh& s : submeshes) {
            w.u32(s.first_index);
            w.u32(s.index_count);
            w.u32(s.material_slot);
        }
        if (vertex_blob) {
            w.bytes(*vertex_blob);
        } else {
            const std::vector<std::byte> blob(std::size_t{vertex_count} * stride, std::byte{0});
            w.bytes(blob);
        }
        if (indices) {
            for (const std::uint32_t i : *indices) {
                w.u32(i);
            }
        } else {
            for (std::uint32_t i = 0; i < index_count; ++i) {
                w.u32(vertex_count == 0 ? 0 : i % vertex_count);
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
