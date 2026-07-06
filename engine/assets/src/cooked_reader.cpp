// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/cooked_reader.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"
#include "rime/core/reflect.hpp"

namespace rime::assets::detail {

// The v1 cooked-mesh vertex layout, expressed as a reflected type purely so its schema fingerprint
// is *derived* from the layout rather than hand-picked (ADR-0024, decision 4). Add, remove, or
// reorder a field here and reflect<>().type_hash changes, so every mesh cooked against the old
// layout is rejected with SchemaMismatch instead of being misread. This is the asset *format's*
// definition of the layout; engine/render's MeshVertex happens to match it today
// (position/normal/uv, 32 bytes).
struct MeshVertexV1 {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

static_assert(sizeof(MeshVertexV1) == 32, "v1 vertex must stay the 32-byte P/N/UV layout");

} // namespace rime::assets::detail

// Registration is at global scope (the macro opens namespace rime::core to specialize its traits).
RIME_REFLECT_BEGIN(rime::assets::detail::MeshVertexV1)
RIME_REFLECT_FIELD(px)
RIME_REFLECT_FIELD(py)
RIME_REFLECT_FIELD(pz)
RIME_REFLECT_FIELD(nx)
RIME_REFLECT_FIELD(ny)
RIME_REFLECT_FIELD(nz)
RIME_REFLECT_FIELD(u)
RIME_REFLECT_FIELD(v)
RIME_REFLECT_END()

namespace rime::assets {

std::string_view to_string(AssetError error) noexcept {
    switch (error) {
        case AssetError::Truncated:
            return "truncated (file ended mid-field)";
        case AssetError::BadMagic:
            return "bad magic (not an RMA1 file)";
        case AssetError::UnsupportedVersion:
            return "unsupported container version";
        case AssetError::WrongKind:
            return "wrong asset kind";
        case AssetError::SchemaMismatch:
            return "schema hash mismatch (re-cook needed)";
        case AssetError::SizeMismatch:
            return "size mismatch (a declared length disagrees with the bytes present)";
        case AssetError::InvalidLayout:
            return "invalid vertex layout";
        case AssetError::IndexOutOfRange:
            return "index out of range";
        case AssetError::BadSubmesh:
            return "submesh range outside the index buffer";
        case AssetError::Io:
            return "I/O error";
    }
    return "unknown error";
}

std::uint64_t mesh_schema_hash() noexcept {
    return core::reflect<detail::MeshVertexV1>().type_hash;
}

std::optional<CookedHeader> read_header(std::span<const std::byte> file,
                                        std::span<const std::byte>& out_payload,
                                        AssetError& out_error) noexcept {
    core::ByteReader reader(file);

    // Magic first: a wrong-format file is rejected before we interpret any length as ours.
    std::span<const std::byte> magic;
    if (!reader.bytes(magic, kCookedMagic.size())) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }
    if (!std::equal(magic.begin(), magic.end(), kCookedMagic.begin())) {
        out_error = AssetError::BadMagic;
        return std::nullopt;
    }

    CookedHeader header;
    std::uint16_t kind_raw = 0;
    if (!reader.u16(header.container_version) || !reader.u16(kind_raw) ||
        !reader.u64(header.type_schema_hash) || !reader.u64(header.payload_size)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }
    if (header.container_version != kContainerVersion) {
        out_error = AssetError::UnsupportedVersion;
        return std::nullopt;
    }
    header.kind = static_cast<AssetKind>(kind_raw);

    // One asset per file (v1): the payload is exactly the rest of the file, and its length must be
    // the length the header promised. This comparison also caps payload_size at the real file size,
    // so a crafted-huge length is rejected here rather than sizing an allocation from it.
    if (reader.remaining() != header.payload_size) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }
    if (!reader.bytes(out_payload, static_cast<std::size_t>(header.payload_size))) {
        out_error = AssetError::Truncated; // unreachable after the check above; kept for safety
        return std::nullopt;
    }
    return header;
}

std::optional<MeshAsset> decode_mesh(std::span<const std::byte> payload,
                                     AssetError& out_error) noexcept {
    core::ByteReader reader(payload);

    // Fixed mesh header: flags, stride, counts, bounds, submesh count (44 bytes). Any short read
    // here is a truncated payload.
    std::uint32_t attribs_raw = 0;
    std::uint32_t stride = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t submesh_count = 0;
    MeshAsset mesh;
    if (!reader.u32(attribs_raw) || !reader.u32(stride) || !reader.u32(vertex_count) ||
        !reader.u32(index_count) || !reader.f32(mesh.bounds.min.x) ||
        !reader.f32(mesh.bounds.min.y) || !reader.f32(mesh.bounds.min.z) ||
        !reader.f32(mesh.bounds.max.x) || !reader.f32(mesh.bounds.max.y) ||
        !reader.f32(mesh.bounds.max.z) || !reader.u32(submesh_count)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }

    // Validate the layout before trusting the stride to address vertices.
    const auto attribs = static_cast<VertexAttribs>(attribs_raw);
    if ((attribs_raw & ~kKnownVertexAttribs) != 0 ||
        !has_attrib(attribs, VertexAttribs::Position) ||
        stride != expected_vertex_stride(attribs)) {
        out_error = AssetError::InvalidLayout;
        return std::nullopt;
    }
    // A drawable mesh has vertices and a whole number of triangles.
    if (vertex_count == 0 || index_count == 0 || (index_count % 3) != 0) {
        out_error = AssetError::InvalidLayout;
        return std::nullopt;
    }

    // How many bytes the variable tail must hold. Everything is 64-bit so the products cannot wrap,
    // and we require the tail to be *exactly* what remains — so no length is ever trusted past the
    // bytes actually present, and nothing is allocated before this check passes.
    const std::uint64_t submesh_bytes = std::uint64_t{submesh_count} * (3u * sizeof(std::uint32_t));
    const std::uint64_t blob_bytes = std::uint64_t{vertex_count} * stride;
    const std::uint64_t index_bytes = std::uint64_t{index_count} * sizeof(std::uint32_t);
    if (reader.remaining() != submesh_bytes + blob_bytes + index_bytes) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }

    mesh.attribs = attribs;
    mesh.vertex_stride = stride;
    mesh.vertex_count = vertex_count;

    mesh.submeshes.reserve(submesh_count);
    for (std::uint32_t i = 0; i < submesh_count; ++i) {
        Submesh submesh;
        // Reads cannot fail (the size check above guaranteed the bytes), but stay defensive.
        if (!reader.u32(submesh.first_index) || !reader.u32(submesh.index_count) ||
            !reader.u32(submesh.material_slot)) {
            out_error = AssetError::Truncated;
            return std::nullopt;
        }
        if (std::uint64_t{submesh.first_index} + submesh.index_count > index_count) {
            out_error = AssetError::BadSubmesh;
            return std::nullopt;
        }
        mesh.submeshes.push_back(submesh);
    }

    std::span<const std::byte> blob;
    if (!reader.bytes(blob, static_cast<std::size_t>(blob_bytes))) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }
    mesh.vertices.assign(blob.begin(), blob.end());

    mesh.indices.reserve(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        std::uint32_t index = 0;
        if (!reader.u32(index)) {
            out_error = AssetError::Truncated;
            return std::nullopt;
        }
        if (index >= vertex_count) {
            out_error = AssetError::IndexOutOfRange;
            return std::nullopt;
        }
        mesh.indices.push_back(index);
    }

    return mesh;
}

std::optional<MeshAsset>
read_mesh(std::span<const std::byte> file, AssetError& out_error, AssetId* out_id) noexcept {
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return std::nullopt;
    }
    if (header->kind != AssetKind::Mesh) {
        out_error = AssetError::WrongKind;
        return std::nullopt;
    }
    if (header->type_schema_hash != mesh_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return std::nullopt;
    }
    // Identity is the hash of the payload the file actually carried — computed once, here, and
    // handed back so the registry can de-duplicate without a second pass.
    if (out_id != nullptr) {
        *out_id = content_hash(payload);
    }
    return decode_mesh(payload, out_error);
}

} // namespace rime::assets
