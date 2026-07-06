// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/core/math/vec.hpp"

// The runtime, in-memory form of a cooked mesh — what the RMA1 reader hands back and what the
// renderer will upload (M6.6). It is deliberately laid out to match what engine/render's mesh
// registry already consumes (position/normal/uv interleaved, 32-bit indices), so uploading is a
// memcpy-and-create, not a per-vertex transform.
//
// The vertex data is kept as an opaque interleaved *blob* plus an attribute-flags description
// rather than a fixed C++ vertex struct. That is decision 6 of ADR-0024: tangents (M6.4) and
// skinning weights (M6.7) are new flag bits and a wider stride, not a new container version — the
// format grows without a break, and the loader validates the layout it is handing on.
namespace rime::assets {

// Which per-vertex attributes an interleaved vertex blob carries, as a bitfield. Position is
// mandatory (a mesh with no positions is meaningless). The high bits are reserved for attributes
// later bricks add; a cooked file that sets an unknown bit is rejected rather than misread.
enum class VertexAttribs : std::uint32_t {
    None = 0,
    Position = 1u << 0, // 3 x f32
    Normal = 1u << 1,   // 3 x f32
    Uv = 1u << 2,       // 2 x f32
    Tangent = 1u << 3,  // 4 x f32 (xyz + handedness sign)  — reserved for M6.4
    Joints = 1u << 4,   // 4 x u16 (skin joint indices)     — reserved for M6.7
    Weights = 1u << 5,  // 4 x f32 (skin weights)           — reserved for M6.7
};

// Every attribute bit defined above; used to reject a cooked file that sets a bit we do not know.
inline constexpr std::uint32_t kKnownVertexAttribs = 0b111111u;

// The three attributes M6.1 actually cooks and renders: a PBR-ready minimum (tangents arrive with
// the M6.4 normal-mapping upgrade). A v1 cooked mesh always declares exactly these.
inline constexpr VertexAttribs kMeshV1Attribs =
    static_cast<VertexAttribs>(static_cast<std::uint32_t>(VertexAttribs::Position) |
                               static_cast<std::uint32_t>(VertexAttribs::Normal) |
                               static_cast<std::uint32_t>(VertexAttribs::Uv));

// --- bitfield operators (scoped enums do not get these for free) ---------------------------------
[[nodiscard]] constexpr VertexAttribs operator|(VertexAttribs a, VertexAttribs b) noexcept {
    return static_cast<VertexAttribs>(static_cast<std::uint32_t>(a) |
                                      static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr VertexAttribs operator&(VertexAttribs a, VertexAttribs b) noexcept {
    return static_cast<VertexAttribs>(static_cast<std::uint32_t>(a) &
                                      static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr bool has_attrib(VertexAttribs set, VertexAttribs bit) noexcept {
    return (set & bit) == bit;
}

// The byte size of one of a single attribute (used to derive a stride from a flag set).
[[nodiscard]] constexpr std::uint32_t attrib_size(VertexAttribs bit) noexcept {
    switch (bit) {
        case VertexAttribs::Position:
        case VertexAttribs::Normal:
            return 3 * sizeof(float);
        case VertexAttribs::Uv:
            return 2 * sizeof(float);
        case VertexAttribs::Tangent:
        case VertexAttribs::Weights:
            return 4 * sizeof(float);
        case VertexAttribs::Joints:
            return 4 * sizeof(std::uint16_t);
        default:
            return 0;
    }
}

// The vertex stride a given attribute set implies: the sum of its attributes' sizes, in fixed
// order. The loader checks the cooked file's declared stride against this, so a corrupt stride
// cannot make vertex addressing walk off the blob.
[[nodiscard]] constexpr std::uint32_t expected_vertex_stride(VertexAttribs attribs) noexcept {
    std::uint32_t stride = 0;
    for (const VertexAttribs bit : {VertexAttribs::Position,
                                    VertexAttribs::Normal,
                                    VertexAttribs::Uv,
                                    VertexAttribs::Tangent,
                                    VertexAttribs::Joints,
                                    VertexAttribs::Weights}) {
        if (has_attrib(attribs, bit)) {
            stride += attrib_size(bit);
        }
    }
    return stride;
}

// An axis-aligned bounding box in the mesh's local space, computed at cook time. Handy immediately
// (culling, camera framing) and load-bearing later (physics broadphase, the M10 SDF bounds).
struct Aabb {
    core::Vec3 min{};
    core::Vec3 max{};
};

// One drawable range of the index buffer, tagged with a material slot. v1 meshes carry zero or one
// submesh; the table exists now so multi-material glTF meshes (M6.4) need no container change.
struct Submesh {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_slot = 0; // index into a future material table (M6.4); 0 for now
};

// A cooked mesh in memory. `vertices` is the interleaved blob (vertex_count * vertex_stride bytes,
// laid out per `attribs`); `indices` is a 32-bit triangle-list index buffer. Both are owned here.
struct MeshAsset {
    VertexAttribs attribs = VertexAttribs::None;
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_count = 0;
    std::vector<std::byte> vertices;
    std::vector<std::uint32_t> indices;
    Aabb bounds{};
    std::vector<Submesh> submeshes;
};

} // namespace rime::assets
