// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/reflect/type_info.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/body.hpp"

// `rime::ground` — the one answer to "what is the ground?" (m17.8, ADR-0041 Ruling 5).
//
// WHY THIS EXISTS, and it is not a style complaint. Before this module the ground was five
// independent numbers in five places, and no two of them agreed. In `99-the-block` alone, one
// surface had THREE extents live in one process:
//
//     drawn      half 38 x 38   x[-16, 60] z[-38, 38]   a unit plane scaled by street_length +
//     4*footprint collided   half 44 x 44   x[-22, 66] z[-44, 44]   a hand-authored static box in
//     the sample GI-traced  half 26 x 14   x[ -4, 48] z[-14, 14]   the SDF clipmap's instance 0
//
// So a player could stand six metres past the visible edge of the world, and the probes lit a
// street a third of the size of the one being drawn. The editor host had the same bug from the
// other direction: its floor comment claims "half-extents match make_plane's width/depth", but
// `make_plane(half_extent, uv_tiles)` has no depth argument — `make_plane(10.0f, 4.0f)` is a 20 m
// square and the collider under it is 10 x 4 m, with its top face 10 cm ABOVE the drawn surface.
//
// None of that is a mistake anyone made twice. It is what happens when a surface has no owner: the
// mesh, the collider and the GI proxy are each derived, correctly, from a different formula.
//
// THE SEAM, and it is the whole point of the module. One authored description, three DERIVED
// artefacts — a mesh, a collider and an extent — with exactly two functions allowed to decide
// geometry (`derive_mesh`, `derive_collider`). A later heightfield changes those two bodies and
// touches no consumer. The test of the seam is stated as a rule: if a change inside `engine/ground`
// requires editing a sample, blockkit or the editor host, the seam failed.
//
// WHAT IS DELIBERATELY NOT HERE (ADR-0041 Ruling 5 defers all of it to M18, partly because
// virtualized geometry may subsume terrain LOD): height data, LOD of any kind, splat or blend
// materials, streaming, and any global "the ground" singleton — a world may hold several patches.
namespace rime::ground {

// The AUTHORED description. Reflected, so it rides a `.rscene`; placement is the entity's own
// transform, and scale must be 1 because a physics body cannot be scaled (the same rule
// destructible instances state).
//
// `cells_x`/`cells_z` are authored even though today's surface is flat, and that is the
// load-bearing half of the seam rather than a visual knob: a flat grid IS a heightfield whose
// heights are all zero, so storing the tessellation now means M18 appends heights rather than
// reshaping the component. They are integers so that every peer derives bit-identical geometry
// without a float ceil.
//
// `thickness` makes the collider a slab rather than a plane. A hull at 100 m/s tunnels through a
// zero-thickness floor and CCD is opt-in per body, so the depth is authored rather than assumed —
// which is what the hand-written street box was already doing by eye.
struct GroundSurface {
    float half_x = 32.0f;       // metres along local X
    float half_z = 32.0f;       // metres along local Z
    std::uint32_t cells_x = 32; // tessellation; a heightfield stores one height per cell corner
    std::uint32_t cells_z = 32;
    float tile_metres = 2.0f; // metres per texture repeat — uv = (local_xz + half) / tile_metres
    float thickness = 1.0f;   // collider depth below the surface
};

// The runtime link to the body `bind_ground` created, in the shape `DestructibleInstanceRef` and
// `RigidBodyHandle` already use: derived, transient, NOT reflected and never serialized. That is
// the point rather than a detail — an editor save that baked a collider would bake a SECOND copy
// of the extent, which is the exact defect this module exists to remove.
struct GroundBody {
    physics::BodyId body{};
};

// Register the ground components with a world — idempotent, like every other module's.
inline void register_ground_components(ecs::World& world) {
    (void)world.register_component<GroundSurface>();
    (void)world.register_component<GroundBody>();
}

} // namespace rime::ground

// Reflection (outside the namespace — the macros open rime::core themselves). Only the AUTHORED
// description is reflected; the derived body handle is not.
RIME_REFLECT_BEGIN(rime::ground::GroundSurface)
RIME_REFLECT_FIELD(half_x)
RIME_REFLECT_FIELD(half_z)
RIME_REFLECT_FIELD(cells_x)
RIME_REFLECT_FIELD(cells_z)
RIME_REFLECT_FIELD(tile_metres)
RIME_REFLECT_FIELD(thickness)
RIME_REFLECT_END()
