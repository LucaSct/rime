// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rime/core/hash.hpp"

// Asset identity (ADR-0024, decision 2). An asset *is* its cooked bytes: its `AssetId` is the
// 64-bit content hash of its cooked payload. That single mechanism gives us three things at once —
// a stable identity that survives a source-file rename (rename the .gltf, the cooked bytes are
// unchanged, the id is unchanged), the key for the cook cache (same content ⇒ same id ⇒ skip the
// recook), and automatic de-duplication of loads (two references to the same bytes coalesce to one
// loaded asset). A *separate* hash of the source path is what humans and the M9 browser look up by;
// it is not the identity. Neither is cryptographic — asset files are our own build products, not an
// attack surface.
namespace rime::assets {

// What kind of thing a cooked file holds. Stored in the container header as a u16; values are wire
// constants — append, never renumber, so an old cooked file is never silently reinterpreted as a
// different kind. Mesh (M6.1), Texture (M6.3), and Material (M6.4) are cooked today; the rest are
// reserved so the header format is stable as later bricks add them.
enum class AssetKind : std::uint16_t {
    Mesh = 1,
    Texture = 2,
    Material = 3,
    Skeleton = 4,      // joint hierarchy + bind pose (M6.7)
    AnimationClip = 5, // keyframed TRS tracks (M6.7)
    Destructible = 6,  // fracture pattern: convex parts + bond/anchor graph (M8.1, ADR-0029)
    MeshSdf = 7,       // cooked signed-distance volume: whole mesh or one destructible part
                       // (M10.4a, ADR-0032 §2)
};

// A content-hashed asset identity. A struct (not a bare u64) so it cannot be confused with an
// ordinary integer or the *path* hash; `value == 0` is the reserved "no asset" id.
struct AssetId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

    friend constexpr bool operator==(AssetId a, AssetId b) noexcept { return a.value == b.value; }

    friend constexpr bool operator!=(AssetId a, AssetId b) noexcept { return !(a == b); }
};

inline constexpr AssetId kInvalidAssetId{0};

// The identity of a cooked payload: FNV-1a over exactly the payload bytes (not the container
// header, whose kind/schema fields are derived from the same content anyway). The cooker computes
// this over the bytes it writes; the loader recomputes it over the bytes it reads — they agree by
// construction.
[[nodiscard]] inline AssetId content_hash(std::span<const std::byte> payload) noexcept {
    return AssetId{core::fnv1a_64(payload)};
}

// The lookup hash of a source path (e.g. "meshes/barrel.gltf"). Distinct from AssetId: this is how
// a human-facing reference or the manifest finds an asset by *where it came from*, and it changes
// on a rename (by design — a renamed source is a new lookup key even though its content id is
// unchanged).
[[nodiscard]] inline std::uint64_t source_path_hash(std::string_view source_path) noexcept {
    return core::fnv1a_64(source_path);
}

} // namespace rime::assets
