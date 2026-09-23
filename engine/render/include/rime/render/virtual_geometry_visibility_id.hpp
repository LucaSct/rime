// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

// A render-owned identity for one visible virtual-geometry triangle. assets and the upload
// scheduler own the mapping from this identity to GPU addresses. Keeping the generation in the
// pixel makes a recycled cluster slot distinguishable from an old pixel still being consumed.
namespace rime::render {

// Two layouts exist. The version always lives in the TOP NIBBLE of the word that holds it, so a
// reader identifies the layout before it interprets anything else. A shipped layout is frozen;
// a new layout takes a new version. Zero (every word) is the invalid/empty sentinel.
//
// Version 1 (M18 step 1, 32-bit, R32Uint) — which cluster, not which triangle. Frozen:
//
//   [19:0]  cluster slot           (1,048,576)
//   [27:20] allocation generation  (256)
//   [31:28] version = 1
//
// Version 3 (M18 step 2, 64-bit, RG32Uint — the current format):
//
//   .x (low word)   [6:0]   triangle within cluster (128 — the usual cluster cap, Nanite's too)
//                   [31:7]  cluster slot            (33,554,432 resident clusters)
//   .y (high word)  [27:0]  allocation generation   (268,435,456)
//                   [31:28] version = 3
//
// Version 2 (a 32-bit layout that squeezed a triangle field in by shrinking the slot to 16 bits
// and the generation to 5) existed only on an unmerged branch and was withdrawn before shipping:
// 65,536 resident clusters is too few for a town-scale scene. Its number is retired, never reused.
//
// Why widen instead of squeeze: the visibility buffer is the one target every pixel writes, and
// its bits bound the whole virtual-geometry system (resident clusters, how long a slot can be
// recycled before an old pixel aliases). 64 bits removes both bounds for the foreseeable future
// at 4 extra bytes per pixel.
struct VirtualGeometryVisibilityId {
    std::uint32_t cluster = 0;
    std::uint32_t generation = 0;
    std::uint32_t version = 3;
    std::uint32_t triangle = 0; // must be 0 in version 1, which has no triangle field

    friend constexpr bool operator==(const VirtualGeometryVisibilityId&,
                                     const VirtualGeometryVisibilityId&) = default;
};

// The 64-bit form is a {lo, hi} word pair rather than a std::uint64_t because that is exactly
// what the GPU stores and reads (one uvec2 of an RG32Uint texel, .x then .y): no byte-order or
// shift convention sits between the C++ value and the shader's, and a readback of the target is
// an array of these with no reinterpretation.
struct VirtualGeometryVisibilityWords {
    std::uint32_t lo = 0; // .x
    std::uint32_t hi = 0; // .y

    friend constexpr bool operator==(const VirtualGeometryVisibilityWords&,
                                     const VirtualGeometryVisibilityWords&) = default;
};

inline constexpr std::uint32_t kInvalidVirtualGeometryVisibilityId = 0;
inline constexpr VirtualGeometryVisibilityWords kInvalidVirtualGeometryVisibilityWords{};
inline constexpr std::uint32_t kVirtualGeometryVisibilityVersionBits = 4;
inline constexpr std::uint32_t kVirtualGeometryVisibilityVersionShift = 28;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMinVersion = 1;
inline constexpr std::uint32_t kVirtualGeometryVisibilityCurrentVersion = 3;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxVersion =
    (1u << kVirtualGeometryVisibilityVersionBits) - 1u;

// Version-1 bounds (step 1's contract, unchanged).
inline constexpr std::uint32_t kVirtualGeometryVisibilityClusterBits = 20;
inline constexpr std::uint32_t kVirtualGeometryVisibilityGenerationBits = 8;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxCluster =
    (1u << kVirtualGeometryVisibilityClusterBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxGeneration =
    (1u << kVirtualGeometryVisibilityGenerationBits) - 1u;

// Version-3 bounds. vg_visibility.frag and vg_resolve.frag hard-code the same shifts; the pass
// tests decode GPU-written pixels with unpack_*, which is what keeps the two in step.
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3TriangleBits = 7;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3ClusterBits = 25;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3GenerationBits = 28;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3MaxTriangle =
    (1u << kVirtualGeometryVisibilityV3TriangleBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3MaxCluster =
    (1u << kVirtualGeometryVisibilityV3ClusterBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityV3MaxGeneration =
    (1u << kVirtualGeometryVisibilityV3GenerationBits) - 1u;

// ── Version 1: the 32-bit API ─────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr std::optional<std::uint32_t>
pack_virtual_geometry_visibility_id(VirtualGeometryVisibilityId id) noexcept {
    if (id.version != 1 || id.triangle != 0 || id.cluster > kVirtualGeometryVisibilityMaxCluster ||
        id.generation > kVirtualGeometryVisibilityMaxGeneration) {
        return std::nullopt;
    }
    return id.cluster | (id.generation << kVirtualGeometryVisibilityClusterBits) |
           (1u << kVirtualGeometryVisibilityVersionShift);
}

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityId>
unpack_virtual_geometry_visibility_id(std::uint32_t packed) noexcept {
    if ((packed >> kVirtualGeometryVisibilityVersionShift) != 1u) {
        return std::nullopt; // empty, reserved, or not a 32-bit layout
    }
    return VirtualGeometryVisibilityId{packed & kVirtualGeometryVisibilityMaxCluster,
                                       (packed >> kVirtualGeometryVisibilityClusterBits) &
                                           kVirtualGeometryVisibilityMaxGeneration,
                                       1u,
                                       0u};
}

// ── Version 3: the 64-bit API ─────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityWords>
pack_virtual_geometry_visibility_id64(VirtualGeometryVisibilityId id) noexcept {
    if (id.version != 3 || id.triangle > kVirtualGeometryVisibilityV3MaxTriangle ||
        id.cluster > kVirtualGeometryVisibilityV3MaxCluster ||
        id.generation > kVirtualGeometryVisibilityV3MaxGeneration) {
        return std::nullopt;
    }
    // The version makes hi nonzero, so a valid ID can never equal the empty sentinel.
    return VirtualGeometryVisibilityWords{
        id.triangle | (id.cluster << kVirtualGeometryVisibilityV3TriangleBits),
        id.generation | (3u << kVirtualGeometryVisibilityVersionShift)};
}

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityId>
unpack_virtual_geometry_visibility_id64(VirtualGeometryVisibilityWords words) noexcept {
    if ((words.hi >> kVirtualGeometryVisibilityVersionShift) != 3u) {
        return std::nullopt; // empty, reserved, or not a 64-bit layout
    }
    return VirtualGeometryVisibilityId{words.lo >> kVirtualGeometryVisibilityV3TriangleBits,
                                       words.hi & kVirtualGeometryVisibilityV3MaxGeneration,
                                       3u,
                                       words.lo & kVirtualGeometryVisibilityV3MaxTriangle};
}

} // namespace rime::render
