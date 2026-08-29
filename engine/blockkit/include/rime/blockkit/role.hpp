// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"

// What a piece of the block IS — the one component the `.rscene` carries beyond placement (m13.2c).
//
// WHY A ROLE AND NOT A MaterialRef. `render::MeshRef` and `render::MaterialRef` hold *dense indices
// into runtime registries* (see render/components.hpp: "carrying dense ids into the mesh/material
// registries"). Authoring those into a scene file would be correct only for as long as every loader
// happened to build its registries in the identical order — and the day someone inserts a material
// at the front of the palette, every entity in the file silently shades as something else. Nothing
// would fail; the block would just be wrong. That is precisely the class of bug this repository
// keeps writing counters to catch.
//
// So the scene stores ARRANGEMENT + INTENT and the look is derived at load: a `SlabRole` says
// "building 3's ground-storey front wall, tint 2", and `palette.hpp`'s apply_palette() stamps the
// MeshRef/MaterialRef. This is the same split `.rdest` already makes — cooked destruction geometry
// carries no material either — and it is what makes the block's appearance a few floats in one
// function rather than a regeneration of the whole scene.
//
// ONE HONEST NUANCE, so nobody reads more safety into this than it has: `tint` is itself an index
// into an ordered table (`palette.hpp`'s kBuildingTints). Reordering that array silently re-tints
// every building, which is the same failure shape one level up. The difference that makes it
// acceptable is that the table is a source-reviewed constant a reader can see, not a registry
// order that emerges from load sequence — but it is a difference of degree, and no counter sees it.
//
// Fields are `std::uint32_t` rather than the `std::uint8_t` the value ranges want: reflection
// classifies integers by size and supports 4- and 8-byte ones only (core/reflect/type_info.hpp),
// and a component that cannot reflect cannot be written to a `.rscene` at all — which is the entire
// purpose of this one. 16 bytes across ~200 entities is not a cost worth a bit-packing trick.
namespace rime::blockkit {

// What kind of thing this entity is. Plain constants rather than an enum class because the field
// they live in must reflect as an integer; the names exist so the switch in apply_palette reads.
namespace slab_kind {
inline constexpr std::uint32_t kWall = 0;      // a full-length perimeter wall (front/back)
inline constexpr std::uint32_t kSideWall = 1;  // a side wall, shortened to butt between front/back
inline constexpr std::uint32_t kHalfWall = 2;  // half of the split front ground wall (the doorway)
inline constexpr std::uint32_t kFloor = 3;     // an interior floor slab
inline constexpr std::uint32_t kRoof = 4;      // the top slab
inline constexpr std::uint32_t kCrate = 5;     // a destructible street crate
inline constexpr std::uint32_t kStreet = 6;    // the street plane (not destructible)
inline constexpr std::uint32_t kKerb = 7;      // a kerb strip
inline constexpr std::uint32_t kBarrier = 8;   // a static street barrier debris piles against
inline constexpr std::uint32_t kLampMast = 9;  // the post
inline constexpr std::uint32_t kLampHead = 10; // the emissive fixture
inline constexpr std::uint32_t kLight = 11;    // a light or the camera — carries no geometry
inline constexpr std::uint32_t kCount = 12;

// Is this kind one of the cooked destructibles (as opposed to a static prop)? Used by the proof to
// separate "slabs that must bind" from "props that must not".
[[nodiscard]] inline constexpr bool is_destructible(std::uint32_t kind) noexcept {
    return kind <= kCrate;
}
} // namespace slab_kind

// Which storey index a horizontal slab sits on when it is the roof — chosen so `storey` is always
// "the storey this belongs to" and the roof is simply one above the top one.
inline constexpr std::uint32_t kRoofStorey = 3;

struct SlabRole {
    std::uint32_t building = 0; // 0..building_count-1; kNoBuilding for street furniture
    std::uint32_t storey = 0;   // 0..storeys-1, or kRoofStorey
    std::uint32_t kind = slab_kind::kWall;
    std::uint32_t tint = 0; // index into the palette's building tints
};

// Street furniture belongs to no building.
inline constexpr std::uint32_t kNoBuilding = 0xFFFFFFFFu;

// Register blockkit's components with a world — idempotent, like every other register_* helper.
// Call before loading a `.rscene` that names them, exactly as the format requires.
inline void register_blockkit_components(ecs::World& world) {
    (void)world.register_component<SlabRole>();
}

} // namespace rime::blockkit

// Reflection (outside the namespace — the macros open rime::core themselves).
RIME_REFLECT_BEGIN(rime::blockkit::SlabRole)
RIME_REFLECT_FIELD(building)
RIME_REFLECT_FIELD(storey)
RIME_REFLECT_FIELD(kind)
RIME_REFLECT_FIELD(tint)
RIME_REFLECT_END()
