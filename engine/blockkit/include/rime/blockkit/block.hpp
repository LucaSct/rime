// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rime/blockkit/role.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/world.hpp"

// The block: eight hollow buildings down a street, assembled procedurally (m13.2c, ADR-0035 §1).
//
// WHY THIS IS C++ AND NOT RUST TOOLING. ADR-0035 called for "a procedural assembly script emitting
// `.rscene`", and the Rust-owns-tooling rule would put it in tools/. It cannot go there: `.rscene`
// keys every component record by its C++ reflection `type_hash`, so a Rust emitter would have to
// reproduce those hashes across a language boundary — exactly the kind of coupling that rots
// silently, since a drifted hash is a clean load *error* here but an invisible mis-author there.
// The format's own design note demonstrates C++ `scene::save_scene_file` as the reference writer,
// and this is that writer's first real caller.
//
// THE ARRANGEMENT, AND WHY IT IS THIS ONE (Luca's rulings, 2026-08-28):
//
//   * **Hollow, every storey** — walls plus floor slabs enclosing rooms, so M10's GI thesis (breach
//     a wall, the interior relights) has somewhere to happen at building scale. A solid mass would
//     leave destruction and lighting both running and never interacting on screen.
//   * **A street, four buildings a side** — the frustum cull (m13.2a) gets real work, the two
//     clients at opposite ends get genuinely different relevancy sets, and a collapse falls *into*
//     the street. A plaza ring would put everything in frustum at once and give both clients the
//     same set.
//   * **Two hero buildings**, diagonally opposed mid-street, fractured ~2.3x finer than the other
//     six. ADR-0035 §1 wants >= 400 peak live debris and one background building cannot supply it.
//
// GEOMETRY NOTE — WHY NINE COOKS AND NOT FIVE. The naive prefab makes every wall the full 8 m
// footprint and every floor the full 8x8, which leaves 0.3 x 0.3 corner columns where side walls
// overlap front/back walls, and a 0.15 m lip where each floor slab intersects the walls it rests
// between. Static interpenetration is harmless while everything is standing and is *exactly* wrong
// at the moment the demo is showing off: once the bonds break, overlapping debris hulls resolve
// their penetration by flinging apart. So side walls and floors get their own shortened cooks
// (7.4 = 8 - 2 x 0.3) and butt cleanly between their neighbours.
namespace rime::blockkit {

// ── The cooked destructibles ─────────────────────────────────────────────────────────────────────
// Content ids are CALLER-CHOSEN CONSTANTS, the convention 12-networked-destruction already uses
// (`kAssetId = 'WALL'`). They are not derived from the cook, because nothing in the engine resolves
// an asset id back to a file yet — render::MeshAsset's own comment admits the identical gap.
// Keeping the list here means the generator and every loader share one table and no sidecar
// manifest can drift out of step with the scene; closing the gap properly is an asset-streaming
// brick.
struct CookSpec {
    const char* name;    // the `--name` passed to `rime fracture`, and the .rdest stem
    float size_x = 0.0f; // full dimensions, as the CLI takes them
    float size_y = 0.0f;
    float size_z = 0.0f;
    std::uint32_t parts = 0;
    std::uint64_t seed = 0;
    std::uint64_t asset = 0; // the content id a Destructible component names
};

// ONE SEED PER CLASS, deliberately. A fracture pattern is invisible until something breaks, and in
// the scripted beat only the two hero buildings do. Per-instance seeds would mean 140 cooks to vary
// something nobody can see, and would make the replication byte budget far harder to reason about.
[[nodiscard]] std::span<const CookSpec> cook_specs() noexcept;

// Look a spec up by the name its .rdest carries; null if there is no such cook.
[[nodiscard]] const CookSpec* find_cook(std::string_view name) noexcept;

// Look a spec up by the content id a `Destructible` component names. This is the direction the
// proof reads in: it counts parts from what the LOADED scene actually asks for, rather than from
// what the prefab intended — an asset id nobody has a cook for comes back null and is counted,
// which is the difference between "the block is smaller than it should be" and silence.
[[nodiscard]] const CookSpec* find_cook_by_asset(std::uint64_t asset) noexcept;

// ── The block's dimensions ───────────────────────────────────────────────────────────────────────
// Defaults ARE the shipped block; the fields exist so tests can shrink it (a two-building block
// assembles in microseconds) without a second code path.
struct BlockParams {
    std::uint32_t buildings_per_side = 4;
    std::uint32_t storeys = 3;

    float storey_height = 3.0f;
    float footprint = 8.0f; // square, in metres
    float slab_thickness = 0.3f;
    float street_width = 12.0f; // kerb face to kerb face
    float building_gap = 4.0f;  // along the street, between neighbours on one side
    float doorway_width = 2.0f; // the gap between the two front ground half-walls

    // Which index along each side is the finely-fractured hero. Diagonally opposed so two collapses
    // overlap in time without merging into one pile.
    std::uint32_t hero_south = 1;
    std::uint32_t hero_north = 2;

    std::uint32_t crates = 12;
    std::uint32_t barriers = 8;
    std::uint32_t lamps_per_side = 6;

    [[nodiscard]] std::uint32_t building_count() const noexcept { return buildings_per_side * 2; }

    // The street runs +X from 0 to here: n footprints plus (n-1) gaps.
    [[nodiscard]] float street_length() const noexcept;

    // Slabs per building: 4 walls per storey (the front ground one split in two) plus one
    // horizontal slab per storey (interior floors and the roof).
    [[nodiscard]] std::uint32_t slabs_per_building() const noexcept;
};

// ── What assembly produced ───────────────────────────────────────────────────────────────────────
// Every one of these is asserted by the proof against ADR-0035 §1's floors. `parts` is summed from
// the cook specs the slabs actually name, not from the intended arrangement — the two agree only if
// the prefab wired up what it meant to.
struct BlockStats {
    std::size_t entities = 0;
    std::size_t buildings = 0;
    std::size_t slabs = 0;  // destructible building slabs
    std::size_t crates = 0; // destructible street crates
    std::size_t props = 0;  // street, kerbs, barriers, lamp masts and heads
    std::size_t point_lights = 0;
    std::size_t spot_lights = 0;
    std::size_t dir_lights = 0;
    std::size_t parts = 0; // total destructible parts across every instance

    [[nodiscard]] std::size_t destructibles() const noexcept { return slabs + crates; }

    [[nodiscard]] std::size_t local_lights() const noexcept { return point_lights + spot_lights; }
};

// Spawn the whole block into `world`: LocalTransform + SlabRole on everything, Destructible on the
// cooked slabs and crates, the lights, and the camera. Registers the component set it writes.
//
// It deliberately does NOT touch meshes, materials or WorldTransform — those are derived
// (palette.hpp), which is what lets the same scene file be re-tinted without regenerating it.
BlockStats assemble(ecs::World& world, const BlockParams& params = {});

// Derive the WorldTransform the renderer consumes from each entity's authored LocalTransform, and
// return how many were written.
//
// A loaded `.rscene` carries LocalTransform only: WorldTransform is deliberately NOT reflected
// (ecs/reflect.hpp — it is derived state, and persisting it would bake a stale duplicate), so a
// freshly-loaded world has nothing for the renderer to read. The block is FLAT — no entity has a
// Parent — so the derivation is the identity and this is `propagate_transforms`'s flat fast path
// without needing a JobSystem. A block that ever grows a hierarchy must call the real pass instead;
// this asserts nothing about parents because there are none to assert about.
std::size_t derive_world_transforms(ecs::World& world);

// Compute what `assemble` WOULD produce, without a world. The generator prints this, and the proof
// uses it as the independent expectation to check the assembled world against — a count derived
// twice by different routes is worth more than one count reported by the code that produced it.
[[nodiscard]] BlockStats predict(const BlockParams& params = {}) noexcept;

// ── The camera, and where the two clients stand ──────────────────────────────────────────────────
// Down the street axis from opposite ends (the street runs +X, so the buildings line its south and
// north sides and the two ends are west and east). Opposite ends is what makes the clients'
// relevancy sets differ (M11.5) and what gives the frustum cull something to reject.
[[nodiscard]] core::Vec3 west_viewpoint(const BlockParams& params = {}) noexcept;
[[nodiscard]] core::Vec3 east_viewpoint(const BlockParams& params = {}) noexcept;

// Eye height for both viewpoints and for the assembled camera — a standing human, since m13.3 puts
// a first-person player here.
inline constexpr float kEyeHeight = 1.7f;

} // namespace rime::blockkit
