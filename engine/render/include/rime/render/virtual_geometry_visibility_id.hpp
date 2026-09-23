// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

// A render-owned identity for one visible virtual-geometry cluster. The value is intended for an
// R32Uint visibility target; assets and the upload scheduler own the mapping from this identity to
// GPU addresses. Keeping the generation in the pixel makes a recycled cluster slot distinguishable
// from an old pixel that is still being consumed by a later frame.
namespace rime::render {

// Layout, from least significant bit to most significant bit:
//
//   [19:0]  cluster slot       (1,048,576 slots)
//   [27:20] allocation generation (256 generations)
//   [31:28] visibility format version (versions 1..15)
//
// Version zero is reserved, and zero as a whole is the invalid/empty sentinel. A future format
// may use a different layout under a new version, but must not reinterpret version one.
struct VirtualGeometryVisibilityId {
    std::uint32_t cluster = 0;
    std::uint32_t generation = 0;
    std::uint32_t version = 1;

    friend constexpr bool operator==(const VirtualGeometryVisibilityId&,
                                     const VirtualGeometryVisibilityId&) = default;
};

inline constexpr std::uint32_t kInvalidVirtualGeometryVisibilityId = 0;
inline constexpr std::uint32_t kVirtualGeometryVisibilityClusterBits = 20;
inline constexpr std::uint32_t kVirtualGeometryVisibilityGenerationBits = 8;
inline constexpr std::uint32_t kVirtualGeometryVisibilityVersionBits = 4;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxCluster =
    (1u << kVirtualGeometryVisibilityClusterBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxGeneration =
    (1u << kVirtualGeometryVisibilityGenerationBits) - 1u;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMinVersion = 1;
inline constexpr std::uint32_t kVirtualGeometryVisibilityMaxVersion =
    (1u << kVirtualGeometryVisibilityVersionBits) - 1u;

[[nodiscard]] constexpr std::optional<std::uint32_t>
pack_virtual_geometry_visibility_id(VirtualGeometryVisibilityId id) noexcept {
    if (id.cluster > kVirtualGeometryVisibilityMaxCluster ||
        id.generation > kVirtualGeometryVisibilityMaxGeneration ||
        id.version < kVirtualGeometryVisibilityMinVersion ||
        id.version > kVirtualGeometryVisibilityMaxVersion) {
        return std::nullopt;
    }

    const std::uint32_t packed = id.cluster |
                                 (id.generation << kVirtualGeometryVisibilityClusterBits) |
                                 (id.version << (kVirtualGeometryVisibilityClusterBits +
                                                 kVirtualGeometryVisibilityGenerationBits));
    return packed == kInvalidVirtualGeometryVisibilityId ? std::nullopt : std::optional{packed};
}

[[nodiscard]] constexpr std::optional<VirtualGeometryVisibilityId>
unpack_virtual_geometry_visibility_id(std::uint32_t packed) noexcept {
    if (packed == kInvalidVirtualGeometryVisibilityId) {
        return std::nullopt;
    }

    const VirtualGeometryVisibilityId id{
        packed & kVirtualGeometryVisibilityMaxCluster,
        (packed >> kVirtualGeometryVisibilityClusterBits) & kVirtualGeometryVisibilityMaxGeneration,
        packed >>
            (kVirtualGeometryVisibilityClusterBits + kVirtualGeometryVisibilityGenerationBits)};
    return id.version >= kVirtualGeometryVisibilityMinVersion &&
                   id.version <= kVirtualGeometryVisibilityMaxVersion
               ? std::optional{id}
               : std::nullopt;
}

} // namespace rime::render
