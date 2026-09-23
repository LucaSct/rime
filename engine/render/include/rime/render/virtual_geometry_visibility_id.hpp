// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

// A render-owned identity for one visible virtual-geometry triangle. The value is intended for an
// R32Uint visibility target; assets and the upload scheduler own the mapping from this identity to
// GPU addresses. Keeping the generation in the pixel makes a recycled cluster slot distinguishable
// from an old pixel that is still being consumed by a later frame.
namespace rime::render {

// The top four bits are ALWAYS the format version, in every layout, so a reader can tell which
// layout the other 28 bits use before it interprets them. Version zero is reserved, and zero as a
// whole is the invalid/empty sentinel. A version's layout is frozen once shipped; a new layout
// takes a new version and must never reinterpret an old one. Versions without a layout below are
// rejected by both pack and unpack.
//
// Version 1 (M18 step 1) — which cluster, but not which triangle:
//
//   [19:0]  cluster slot           (1,048,576 slots)
//   [27:20] allocation generation  (256 generations)
//   [31:28] version = 1
//
// Version 2 (M18 step 2) — adds the triangle within the cluster, which is what a material
// resolve needs to fetch the three vertices a pixel came from. The 32 bits were already full, so
// the triangle's 7 bits are paid for by the slot and generation fields:
//
//   [6:0]   triangle within cluster (128 — the usual cluster triangle cap, Nanite's included)
//   [22:7]  cluster slot           (65,536 resident clusters)
//   [27:23] allocation generation  (32 generations)
//   [31:28] version = 2
//
// 65,536 slots bound the *resident drawable* cluster pool, not the asset's cluster count; 32
// generations is enough because a slot only needs to differ from the pixel of the previous few
// frames still in flight. A wider ID (a 64-bit target, depth in the high word) is the known exit
// if either bound ever binds.
struct VirtualGeometryVisibilityId {
    std::uint32_t cluster = 0;
    std::uint32_t generation = 0;
    std::uint32_t version = 2;
    std::uint32_t triangle = 0; // must be 0 in version 1, which has no triangle field

    friend constexpr bool operator==(const VirtualGeometryVisibilityId&,
                                     const VirtualGeometryVisibilityId&) = default;
};

inline constexpr std::uint32_t kInvalidVirtualGeometryVisibilityId = 0;
inline constexpr std::uint32_t kVirtualGeometryVisibilityVersionBits = 4;
inline constexpr std::uint32_t kVirtualGeometryVisibilityVersionShift = 28;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMinVersion = 1;
inline constexpr std::uint32_t kVirtualGeometryVisibilityCurrentVersion = 2;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxVersion =
    (1u << kVirtualGeometryVisibilityVersionBits) - 1u;

// Per-version field layout. `triangle_bits == 0` means the version carries no triangle.
struct VirtualGeometryVisibilityLayout {
    std::uint32_t triangle_bits;
    std::uint32_t cluster_bits;
    std::uint32_t generation_bits;
};

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityLayout>
virtual_geometry_visibility_layout(std::uint32_t version) noexcept {
    switch (version) {
        case 1:
            return VirtualGeometryVisibilityLayout{0, 20, 8};
        case 2:
            return VirtualGeometryVisibilityLayout{7, 16, 5};
        default:
            return std::nullopt;
    }
}

// Version-1 bounds, kept under their original names (step 1's contract).
inline constexpr std::uint32_t kVirtualGeometryVisibilityClusterBits = 20;
inline constexpr std::uint32_t kVirtualGeometryVisibilityGenerationBits = 8;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxCluster =
    (1u << kVirtualGeometryVisibilityClusterBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxGeneration =
    (1u << kVirtualGeometryVisibilityGenerationBits) - 1u;

// Version-2 bounds. The shaders (vg_visibility.frag, vg_resolve.frag) hard-code the same shifts;
// the pass tests decode GPU-written pixels with unpack_*, which is what keeps the two in step.
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2TriangleBits = 7;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2ClusterBits = 16;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2GenerationBits = 5;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2MaxTriangle =
    (1u << kVirtualGeometryVisibilityV2TriangleBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2MaxCluster =
    (1u << kVirtualGeometryVisibilityV2ClusterBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV2MaxGeneration =
    (1u << kVirtualGeometryVisibilityV2GenerationBits) - 1u;

[[nodiscard]] constexpr std::optional<std::uint32_t>
pack_virtual_geometry_visibility_id(VirtualGeometryVisibilityId id) noexcept {
    const auto layout = virtual_geometry_visibility_layout(id.version);
    if (!layout) {
        return std::nullopt;
    }
    const auto fits = [](std::uint32_t value, std::uint32_t bits) {
        return bits == 0 ? value == 0 : value <= (1u << bits) - 1u;
    };
    if (!fits(id.triangle, layout->triangle_bits) || !fits(id.cluster, layout->cluster_bits) ||
        !fits(id.generation, layout->generation_bits)) {
        return std::nullopt;
    }

    const std::uint32_t cluster_shift = layout->triangle_bits;
    const std::uint32_t generation_shift = cluster_shift + layout->cluster_bits;
    const std::uint32_t packed = id.triangle | (id.cluster << cluster_shift) |
                                 (id.generation << generation_shift) |
                                 (id.version << kVirtualGeometryVisibilityVersionShift);
    return packed == kInvalidVirtualGeometryVisibilityId ? std::nullopt : std::optional{packed};
}

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityId>
unpack_virtual_geometry_visibility_id(std::uint32_t packed) noexcept {
    if (packed == kInvalidVirtualGeometryVisibilityId) {
        return std::nullopt;
    }
    const std::uint32_t version = packed >> kVirtualGeometryVisibilityVersionShift;
    const auto layout = virtual_geometry_visibility_layout(version);
    if (!layout) {
        return std::nullopt; // version 0 or a layout this build does not know
    }
    const auto field = [packed](std::uint32_t shift, std::uint32_t bits) {
        return bits == 0 ? 0u : (packed >> shift) & ((1u << bits) - 1u);
    };
    const std::uint32_t cluster_shift = layout->triangle_bits;
    const std::uint32_t generation_shift = cluster_shift + layout->cluster_bits;
    return VirtualGeometryVisibilityId{field(cluster_shift, layout->cluster_bits),
                                       field(generation_shift, layout->generation_bits),
                                       version,
                                       field(0, layout->triangle_bits)};
}

} // namespace rime::render
