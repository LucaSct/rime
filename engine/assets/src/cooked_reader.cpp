// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/cooked_reader.hpp"

#include <algorithm>
#include <cmath>

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

// The v1 cooked-texture mip-descriptor record, reflected for the same reason MeshVertexV1 is: so
// the texture schema fingerprint is *derived* from the layout the reader walks, not hand-picked
// (ADR-0024, decision 4). It is the {width, height, offset, size} tuple stored once per mip level.
// Reorder or retype a field and reflect<>().type_hash changes, so a texture cooked against the old
// table layout is rejected with SchemaMismatch rather than misread. Only the *table record* is
// fingerprinted, not the base-extent/format header around it: that header is container-stable, and
// a future pixel format is an appended TextureFormat value (backward-compatible), not a layout
// change.
struct TextureMipV1 {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t offset;
    std::uint32_t size;
};

static_assert(sizeof(TextureMipV1) == 16, "v1 mip record must stay four packed u32s");

// The v1 cooked-material record, reflected for the same reason MeshVertexV1 and TextureMipV1 are:
// so the material schema fingerprint is *derived* from the field set the reader walks, not
// hand-picked (ADR-0024, decision 4). Unlike the mesh/texture records this is the *whole* payload —
// a material has no variable-length tail — so these fields, in this order, are the entire wire
// format. Add, remove, reorder, or retype a field and reflect<>().type_hash changes, so a material
// cooked against the old layout is rejected with SchemaMismatch rather than misread.
// compute_type_hash ignores offsets and sizeof, so the padding this mixed-width struct carries is
// inert; the wire is written field by field (decode_material below, and the Rust cooker), never as
// a struct memcpy, so padding never reaches it.
struct MaterialV1 {
    float base_color_r, base_color_g, base_color_b, base_color_a;
    float emissive_r, emissive_g, emissive_b;
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
    std::uint32_t alpha_mode;
    std::uint64_t base_color_tex;
    std::uint64_t metallic_roughness_tex;
    std::uint64_t normal_tex;
    std::uint64_t occlusion_tex;
    std::uint64_t emissive_tex;
};

// The v1 cooked-skeleton per-joint record, reflected for the same reason the records above are: so
// the skeleton schema fingerprint is *derived* from the layout the reader walks, not hand-picked
// (ADR-0024, decision 4). It is the repeated unit of the joint table — a joint's parent index, its
// name hash, its model→bind-local inverse-bind matrix (16 floats, column-major), and its bind-pose
// local transform (translation, rotation quaternion, scale). Reorder or retype a field and
// reflect<>().type_hash changes, so a skeleton cooked against the old layout is rejected with
// SchemaMismatch rather than misread. Like MaterialV1 this is a mixed-width record, so it carries
// padding — but compute_type_hash ignores offsets and sizeof, and the wire is written field by
// field (decode_skeleton below, and the Rust cooker), never as a struct memcpy, so the padding is
// inert.
struct SkeletonJointV1 {
    std::int32_t parent;
    std::uint64_t name_hash;
    float ib0, ib1, ib2, ib3, ib4, ib5, ib6, ib7;
    float ib8, ib9, ib10, ib11, ib12, ib13, ib14, ib15;
    float tx, ty, tz;     // bind-pose local translation
    float rx, ry, rz, rw; // bind-pose local rotation (quaternion x, y, z, w)
    float sx, sy, sz;     // bind-pose local scale
};

// The v1 cooked-clip channel record, reflected for the same reason TextureMipV1 is: it is the
// repeated table unit the reader walks to slice the keyframe blob. A channel names its target
// joint, which TRS path it drives (0 = translation, 1 = rotation, 2 = scale), how it interpolates
// (0 = step, 1 = linear), and how many keyframes it holds; the times and values themselves are the
// blob after the table (not fingerprinted, exactly as pixels are not — a future value type would be
// an appended path enum, not a record change). Four packed u32s, like the mip record.
struct ClipChannelV1 {
    std::uint32_t target_joint;
    std::uint32_t path;
    std::uint32_t interp;
    std::uint32_t key_count;
};

static_assert(sizeof(ClipChannelV1) == 16, "v1 clip channel record must stay four packed u32s");

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

RIME_REFLECT_BEGIN(rime::assets::detail::TextureMipV1)
RIME_REFLECT_FIELD(width)
RIME_REFLECT_FIELD(height)
RIME_REFLECT_FIELD(offset)
RIME_REFLECT_FIELD(size)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::assets::detail::MaterialV1)
RIME_REFLECT_FIELD(base_color_r)
RIME_REFLECT_FIELD(base_color_g)
RIME_REFLECT_FIELD(base_color_b)
RIME_REFLECT_FIELD(base_color_a)
RIME_REFLECT_FIELD(emissive_r)
RIME_REFLECT_FIELD(emissive_g)
RIME_REFLECT_FIELD(emissive_b)
RIME_REFLECT_FIELD(metallic)
RIME_REFLECT_FIELD(roughness)
RIME_REFLECT_FIELD(normal_scale)
RIME_REFLECT_FIELD(occlusion_strength)
RIME_REFLECT_FIELD(alpha_cutoff)
RIME_REFLECT_FIELD(alpha_mode)
RIME_REFLECT_FIELD(base_color_tex)
RIME_REFLECT_FIELD(metallic_roughness_tex)
RIME_REFLECT_FIELD(normal_tex)
RIME_REFLECT_FIELD(occlusion_tex)
RIME_REFLECT_FIELD(emissive_tex)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::assets::detail::SkeletonJointV1)
RIME_REFLECT_FIELD(parent)
RIME_REFLECT_FIELD(name_hash)
RIME_REFLECT_FIELD(ib0)
RIME_REFLECT_FIELD(ib1)
RIME_REFLECT_FIELD(ib2)
RIME_REFLECT_FIELD(ib3)
RIME_REFLECT_FIELD(ib4)
RIME_REFLECT_FIELD(ib5)
RIME_REFLECT_FIELD(ib6)
RIME_REFLECT_FIELD(ib7)
RIME_REFLECT_FIELD(ib8)
RIME_REFLECT_FIELD(ib9)
RIME_REFLECT_FIELD(ib10)
RIME_REFLECT_FIELD(ib11)
RIME_REFLECT_FIELD(ib12)
RIME_REFLECT_FIELD(ib13)
RIME_REFLECT_FIELD(ib14)
RIME_REFLECT_FIELD(ib15)
RIME_REFLECT_FIELD(tx)
RIME_REFLECT_FIELD(ty)
RIME_REFLECT_FIELD(tz)
RIME_REFLECT_FIELD(rx)
RIME_REFLECT_FIELD(ry)
RIME_REFLECT_FIELD(rz)
RIME_REFLECT_FIELD(rw)
RIME_REFLECT_FIELD(sx)
RIME_REFLECT_FIELD(sy)
RIME_REFLECT_FIELD(sz)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::assets::detail::ClipChannelV1)
RIME_REFLECT_FIELD(target_joint)
RIME_REFLECT_FIELD(path)
RIME_REFLECT_FIELD(interp)
RIME_REFLECT_FIELD(key_count)
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
        case AssetError::InvalidTexture:
            return "invalid texture (unknown format or inconsistent mip table)";
        case AssetError::InvalidMaterial:
            return "invalid material (unknown alpha mode or non-finite factor)";
        case AssetError::InvalidSkeleton:
            return "invalid skeleton (joints out of topological order or a non-finite bind value)";
        case AssetError::InvalidClip:
            return "invalid clip (bad channel path/interp, non-monotonic times, or a non-finite "
                   "value)";
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

std::uint64_t texture_schema_hash() noexcept {
    return core::reflect<detail::TextureMipV1>().type_hash;
}

std::optional<TextureAsset> decode_texture(std::span<const std::byte> payload,
                                           AssetError& out_error) noexcept {
    core::ByteReader reader(payload);

    // Fixed texture header: base extent, colour-space format tag, mip count (16 bytes).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format_raw = 0;
    std::uint32_t mip_count = 0;
    if (!reader.u32(width) || !reader.u32(height) || !reader.u32(format_raw) ||
        !reader.u32(mip_count)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }

    // Validate the header before trusting any of it to size mips. v1 knows exactly two formats
    // (RGBA8 linear / sRGB); a full chain's length is fully determined by the base extent, so any
    // other mip_count is a corrupt or foreign file — caught here rather than by walking a bad
    // table.
    if (format_raw > static_cast<std::uint32_t>(TextureFormat::Rgba8Srgb) || width == 0 ||
        height == 0 || mip_count != full_mip_count(width, height)) {
        out_error = AssetError::InvalidTexture;
        return std::nullopt;
    }

    TextureAsset tex;
    tex.width = width;
    tex.height = height;
    tex.format = static_cast<TextureFormat>(format_raw);
    tex.mips.reserve(mip_count);

    // The mip table: one {width, height, offset, size} record per level. Each field is
    // cross-checked against what a full chain from (width, height) *must* contain — the level's
    // halved extent, its width*height*4 byte size, and an offset that tiles the blob with no gap or
    // overlap. `running` is 64-bit so the offsets cannot wrap; a tampered record fails here, before
    // any pixel is read.
    std::uint64_t running = 0; // expected offset of the next level = bytes accounted for so far
    for (std::uint32_t level = 0; level < mip_count; ++level) {
        std::uint32_t mw = 0;
        std::uint32_t mh = 0;
        std::uint32_t moffset = 0;
        std::uint32_t msize = 0;
        if (!reader.u32(mw) || !reader.u32(mh) || !reader.u32(moffset) || !reader.u32(msize)) {
            out_error = AssetError::Truncated;
            return std::nullopt;
        }
        const std::uint32_t ew = mip_extent(width, level);
        const std::uint32_t eh = mip_extent(height, level);
        const std::uint64_t esize = std::uint64_t{ew} * eh * kTextureBytesPerPixel;
        if (mw != ew || mh != eh || msize != esize || moffset != running) {
            out_error = AssetError::InvalidTexture;
            return std::nullopt;
        }
        tex.mips.push_back(TextureMip{mw, mh, moffset, msize});
        running += msize;
    }

    // The pixel blob must be exactly the mip sizes' sum — no missing bytes (an upload would read
    // past them) and no trailing bytes (a corrupt or foreign file). Then copy it in one shot.
    if (reader.remaining() != running) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }
    std::span<const std::byte> blob;
    if (!reader.bytes(blob, static_cast<std::size_t>(running))) {
        out_error = AssetError::Truncated; // unreachable after the check above; kept for safety
        return std::nullopt;
    }
    tex.pixels.assign(blob.begin(), blob.end());
    return tex;
}

std::optional<TextureAsset>
read_texture(std::span<const std::byte> file, AssetError& out_error, AssetId* out_id) noexcept {
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return std::nullopt;
    }
    if (header->kind != AssetKind::Texture) {
        out_error = AssetError::WrongKind;
        return std::nullopt;
    }
    if (header->type_schema_hash != texture_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return std::nullopt;
    }
    if (out_id != nullptr) {
        *out_id = content_hash(payload);
    }
    return decode_texture(payload, out_error);
}

std::uint64_t material_schema_hash() noexcept {
    return core::reflect<detail::MaterialV1>().type_hash;
}

std::optional<MaterialAsset> decode_material(std::span<const std::byte> payload,
                                             AssetError& out_error) noexcept {
    core::ByteReader reader(payload);

    MaterialAsset mat;
    std::uint32_t alpha_mode_raw = 0;
    std::uint64_t base_color_tex = 0;
    std::uint64_t metallic_roughness_tex = 0;
    std::uint64_t normal_tex = 0;
    std::uint64_t occlusion_tex = 0;
    std::uint64_t emissive_tex = 0;

    // A material is a fixed record with no variable-length tail: read every field in wire order.
    // This order IS the format — it must match detail::MaterialV1 (which fingerprints it) and the
    // Rust cooker (which writes it). Any short read means a truncated payload.
    if (!reader.f32(mat.base_color[0]) || !reader.f32(mat.base_color[1]) ||
        !reader.f32(mat.base_color[2]) || !reader.f32(mat.base_color[3]) ||
        !reader.f32(mat.emissive[0]) || !reader.f32(mat.emissive[1]) ||
        !reader.f32(mat.emissive[2]) || !reader.f32(mat.metallic) || !reader.f32(mat.roughness) ||
        !reader.f32(mat.normal_scale) || !reader.f32(mat.occlusion_strength) ||
        !reader.f32(mat.alpha_cutoff) || !reader.u32(alpha_mode_raw) ||
        !reader.u64(base_color_tex) || !reader.u64(metallic_roughness_tex) ||
        !reader.u64(normal_tex) || !reader.u64(occlusion_tex) || !reader.u64(emissive_tex)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }

    // Fixed record ⇒ the payload must end here. Trailing bytes mean a corrupt or foreign file.
    if (reader.remaining() != 0) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }

    // Validate before handing the material on. The alpha mode must be a known enum value, and every
    // factor must be finite — a NaN or Inf in the bytes would otherwise flow straight into the
    // shader as garbage (and NaN compares make downstream culling/sorting nondeterministic).
    // Texture ids are taken verbatim: 0 = no texture, any other value is a content id the loader
    // will try to resolve.
    if (alpha_mode_raw > static_cast<std::uint32_t>(AlphaMode::Blend)) {
        out_error = AssetError::InvalidMaterial;
        return std::nullopt;
    }
    const float factors[] = {mat.base_color[0],
                             mat.base_color[1],
                             mat.base_color[2],
                             mat.base_color[3],
                             mat.emissive[0],
                             mat.emissive[1],
                             mat.emissive[2],
                             mat.metallic,
                             mat.roughness,
                             mat.normal_scale,
                             mat.occlusion_strength,
                             mat.alpha_cutoff};
    for (const float f : factors) {
        if (!std::isfinite(f)) {
            out_error = AssetError::InvalidMaterial;
            return std::nullopt;
        }
    }

    mat.alpha_mode = static_cast<AlphaMode>(alpha_mode_raw);
    mat.base_color_tex = AssetId{base_color_tex};
    mat.metallic_roughness_tex = AssetId{metallic_roughness_tex};
    mat.normal_tex = AssetId{normal_tex};
    mat.occlusion_tex = AssetId{occlusion_tex};
    mat.emissive_tex = AssetId{emissive_tex};
    return mat;
}

std::optional<MaterialAsset>
read_material(std::span<const std::byte> file, AssetError& out_error, AssetId* out_id) noexcept {
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return std::nullopt;
    }
    if (header->kind != AssetKind::Material) {
        out_error = AssetError::WrongKind;
        return std::nullopt;
    }
    if (header->type_schema_hash != material_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return std::nullopt;
    }
    if (out_id != nullptr) {
        *out_id = content_hash(payload);
    }
    return decode_material(payload, out_error);
}

std::uint64_t skeleton_schema_hash() noexcept {
    return core::reflect<detail::SkeletonJointV1>().type_hash;
}

std::optional<Skeleton> decode_skeleton(std::span<const std::byte> payload,
                                        AssetError& out_error) noexcept {
    core::ByteReader reader(payload);

    std::uint32_t joint_count = 0;
    if (!reader.u32(joint_count)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }
    // A skeleton has at least one joint and at most kMaxJoints (u16-addressable), rejected before
    // any length is sized from the count.
    if (joint_count == 0 || joint_count > kMaxJoints) {
        out_error = AssetError::InvalidSkeleton;
        return std::nullopt;
    }
    // Each joint is a fixed record: parent(4) + name_hash(8) + inverse_bind(16*4) + TRS((3+4+3)*4).
    // The table must be *exactly* what remains, so a corrupt count can neither over- nor under-run
    // the payload, and nothing is allocated before this passes.
    constexpr std::uint64_t kJointRecordBytes = 4 + 8 + 16 * 4 + (3 + 4 + 3) * 4; // 116
    if (reader.remaining() != std::uint64_t{joint_count} * kJointRecordBytes) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }

    Skeleton skeleton;
    skeleton.joints.reserve(joint_count);
    for (std::uint32_t i = 0; i < joint_count; ++i) {
        Joint joint;
        // Reads cannot fail (the size check guaranteed the bytes) but stay defensive, as
        // decode_mesh does. Parent and name hash first, then the 16-float inverse bind, then the
        // bind-pose TRS.
        bool read_ok = reader.i32(joint.parent) && reader.u64(joint.name_hash);
        for (int k = 0; k < 16; ++k) {
            read_ok = read_ok && reader.f32(joint.inverse_bind.m[k]);
        }
        read_ok =
            read_ok && reader.f32(joint.local_bind.translation.x) &&
            reader.f32(joint.local_bind.translation.y) &&
            reader.f32(joint.local_bind.translation.z) && reader.f32(joint.local_bind.rotation.x) &&
            reader.f32(joint.local_bind.rotation.y) && reader.f32(joint.local_bind.rotation.z) &&
            reader.f32(joint.local_bind.rotation.w) && reader.f32(joint.local_bind.scale.x) &&
            reader.f32(joint.local_bind.scale.y) && reader.f32(joint.local_bind.scale.z);
        if (!read_ok) {
            out_error = AssetError::Truncated; // unreachable after the size check; kept for safety
            return std::nullopt;
        }
        // Topological order: a root (kNoParent) or a parent already emitted (index < i). This is
        // the invariant the sampler's single forward pass relies on — enforced here so the loader
        // can trust it without re-scanning.
        if (joint.parent != Joint::kNoParent &&
            (joint.parent < 0 || static_cast<std::uint32_t>(joint.parent) >= i)) {
            out_error = AssetError::InvalidSkeleton;
            return std::nullopt;
        }
        // Every bind value must be finite — a NaN in a bind matrix or pose would poison every
        // palette entry the joint (and its descendants) touch, and NaN comparisons make
        // sorting/culling nondeterministic downstream.
        bool finite = true;
        for (const float m : joint.inverse_bind.m) {
            finite = finite && std::isfinite(m);
        }
        const core::Transform& lb = joint.local_bind;
        finite = finite && std::isfinite(lb.translation.x) && std::isfinite(lb.translation.y) &&
                 std::isfinite(lb.translation.z) && std::isfinite(lb.rotation.x) &&
                 std::isfinite(lb.rotation.y) && std::isfinite(lb.rotation.z) &&
                 std::isfinite(lb.rotation.w) && std::isfinite(lb.scale.x) &&
                 std::isfinite(lb.scale.y) && std::isfinite(lb.scale.z);
        if (!finite) {
            out_error = AssetError::InvalidSkeleton;
            return std::nullopt;
        }
        skeleton.joints.push_back(joint);
    }
    return skeleton;
}

std::optional<Skeleton>
read_skeleton(std::span<const std::byte> file, AssetError& out_error, AssetId* out_id) noexcept {
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return std::nullopt;
    }
    if (header->kind != AssetKind::Skeleton) {
        out_error = AssetError::WrongKind;
        return std::nullopt;
    }
    if (header->type_schema_hash != skeleton_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return std::nullopt;
    }
    if (out_id != nullptr) {
        *out_id = content_hash(payload);
    }
    return decode_skeleton(payload, out_error);
}

std::uint64_t clip_schema_hash() noexcept {
    return core::reflect<detail::ClipChannelV1>().type_hash;
}

namespace {

// The TRS path a channel drives, matching the cooked `path` word. A rotation carries a 4-float
// quaternion; translation and scale carry a 3-float vector — the count that slices the value blob.
enum class ChannelPath : std::uint32_t { Translation = 0, Rotation = 1, Scale = 2 };

[[nodiscard]] std::uint32_t path_components(std::uint32_t path) noexcept {
    return path == static_cast<std::uint32_t>(ChannelPath::Rotation) ? 4u : 3u;
}

} // namespace

std::optional<Clip> decode_clip(std::span<const std::byte> payload,
                                AssetError& out_error) noexcept {
    core::ByteReader reader(payload);

    float duration = 0.0f;
    std::uint32_t joint_count = 0;
    std::uint32_t channel_count = 0;
    if (!reader.f32(duration) || !reader.u32(joint_count) || !reader.u32(channel_count)) {
        out_error = AssetError::Truncated;
        return std::nullopt;
    }
    // Header sanity: a non-negative finite duration, and a joint count in the same u16-addressable
    // range a skeleton uses (it also bounds the dense per-joint table we allocate below, before any
    // skeleton is on hand to cross-check the count against).
    if (!std::isfinite(duration) || duration < 0.0f || joint_count == 0 ||
        joint_count > kMaxJoints) {
        out_error = AssetError::InvalidClip;
        return std::nullopt;
    }

    // The channel table (16 bytes each) then a keyframe blob. Read the table first — validating
    // each record and summing the blob bytes it implies — so the blob's exact length is known
    // before a byte of it is trusted. Everything is 64-bit and checked incrementally, so no count
    // can wrap or run past the payload.
    const std::uint64_t body = reader.remaining();
    const std::uint64_t table_bytes = std::uint64_t{channel_count} * sizeof(detail::ClipChannelV1);
    if (table_bytes > body) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }
    const std::uint64_t blob_capacity = body - table_bytes;

    struct ChannelRecord {
        std::uint32_t target_joint;
        std::uint32_t path;
        std::uint32_t interp;
        std::uint32_t key_count;
    };

    std::vector<ChannelRecord> records;
    records.reserve(channel_count);
    std::uint64_t blob_bytes = 0;
    for (std::uint32_t c = 0; c < channel_count; ++c) {
        ChannelRecord rec;
        if (!reader.u32(rec.target_joint) || !reader.u32(rec.path) || !reader.u32(rec.interp) ||
            !reader.u32(rec.key_count)) {
            out_error = AssetError::Truncated; // unreachable after the table_bytes check; defensive
            return std::nullopt;
        }
        // A channel targets a real joint, drives a known TRS path, interpolates by a known mode,
        // and holds at least one key (a silent track is simply absent from the sparse table, never
        // a zero-key record).
        if (rec.target_joint >= joint_count ||
            rec.path > static_cast<std::uint32_t>(ChannelPath::Scale) || rec.interp > 1u ||
            rec.key_count == 0) {
            out_error = AssetError::InvalidClip;
            return std::nullopt;
        }
        // Bytes this channel adds to the blob: key_count times + key_count*components values, each
        // a f32. Checked against the remaining capacity as we go, so the running total can never
        // wrap.
        blob_bytes +=
            std::uint64_t{rec.key_count} * (1u + path_components(rec.path)) * sizeof(float);
        if (blob_bytes > blob_capacity) {
            out_error = AssetError::SizeMismatch;
            return std::nullopt;
        }
        records.push_back(rec);
    }
    // Exact tail: the blob must be precisely the size the channel table implies (no missing bytes a
    // read would run past, no trailing bytes of a corrupt or foreign file).
    if (blob_bytes != blob_capacity) {
        out_error = AssetError::SizeMismatch;
        return std::nullopt;
    }

    Clip clip;
    clip.duration = duration;
    clip.joints.resize(joint_count); // dense: one JointAnimation per joint, most left silent

    // Second pass: read each channel's keyframes (times then values) straight out of the blob, in
    // the table's order, into the dense per-joint tracks — reconstructing the columnar-per-joint
    // form the sampler consumes.
    for (const ChannelRecord& rec : records) {
        std::vector<float> times(rec.key_count);
        for (std::uint32_t k = 0; k < rec.key_count; ++k) {
            if (!reader.f32(times[k])) {
                out_error = AssetError::Truncated; // unreachable after the size check; defensive
                return std::nullopt;
            }
        }
        // Times must be finite and strictly increasing — the sampler binary-searches them and
        // assumes a positive span between neighbours (glTF requires the same of animation inputs).
        for (std::uint32_t k = 0; k < rec.key_count; ++k) {
            if (!std::isfinite(times[k]) || (k > 0 && !(times[k] > times[k - 1]))) {
                out_error = AssetError::InvalidClip;
                return std::nullopt;
            }
        }

        const std::uint32_t comps = path_components(rec.path);
        std::vector<float> values(std::size_t{rec.key_count} * comps);
        for (float& v : values) {
            if (!reader.f32(v)) {
                out_error = AssetError::Truncated; // unreachable; defensive
                return std::nullopt;
            }
            if (!std::isfinite(v)) {
                out_error = AssetError::InvalidClip;
                return std::nullopt;
            }
        }

        const auto interp = static_cast<Interpolation>(rec.interp);
        JointAnimation& anim = clip.joints[rec.target_joint];
        if (rec.path == static_cast<std::uint32_t>(ChannelPath::Rotation)) {
            anim.rotation.interp = interp;
            anim.rotation.times = std::move(times);
            anim.rotation.values.resize(rec.key_count);
            for (std::uint32_t k = 0; k < rec.key_count; ++k) {
                anim.rotation.values[k] = core::Quat{
                    values[k * 4 + 0], values[k * 4 + 1], values[k * 4 + 2], values[k * 4 + 3]};
            }
        } else {
            Channel<core::Vec3>& ch =
                rec.path == static_cast<std::uint32_t>(ChannelPath::Translation) ? anim.translation
                                                                                 : anim.scale;
            ch.interp = interp;
            ch.times = std::move(times);
            ch.values.resize(rec.key_count);
            for (std::uint32_t k = 0; k < rec.key_count; ++k) {
                ch.values[k] = core::Vec3{values[k * 3 + 0], values[k * 3 + 1], values[k * 3 + 2]};
            }
        }
    }
    return clip;
}

std::optional<Clip>
read_clip(std::span<const std::byte> file, AssetError& out_error, AssetId* out_id) noexcept {
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return std::nullopt;
    }
    if (header->kind != AssetKind::AnimationClip) {
        out_error = AssetError::WrongKind;
        return std::nullopt;
    }
    if (header->type_schema_hash != clip_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return std::nullopt;
    }
    if (out_id != nullptr) {
        *out_id = content_hash(payload);
    }
    return decode_clip(payload, out_error);
}

} // namespace rime::assets
