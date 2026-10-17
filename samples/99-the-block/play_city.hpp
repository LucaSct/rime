// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// The playable street grown into a small city, and its lamps and barriers made genuinely
// destructible (play-only; the authored `--scene` path never touches any of this).
//
// THE SHAPE OF THE BRICK, because three different peers touch it and each wants a different half:
//
//   1. `expand` edits the AUTHORING world -- the ECS the block generator just assembled, before it
//      is serialised and loaded into each peer. Both peers therefore receive byte-identical level
//      geometry from one deterministic function, which is the same reason `blockkit::assemble`
//      runs on a scratch world. It is pure ECS: no physics, no GPU.
//   2. `register_patterns` hands the two CPU-built fracture patterns (lamp, barrier) to a peer's
//      DestructionWorld, and -- on the drawing peer only -- to the PartLeafRenderer, exactly like
//      the cooked `.rdest` patterns. No file is involved: the patterns are generated, hull by
//      hull, from the same `assets::DestructibleAsset` a cook would have produced.
//   3. `Dresser::sync` runs once per client tick after the leaves are posed. It reads ACCEPTED
//      part state (`DestructionWorld::part_alive`) -- never a local guess -- and dresses each leaf
//      with a per-part material and keeps each lamp's spot light in step with its luminaire.
//
// Why a dresser at all: `PartLeafRenderer` gives every part of an instance the ONE material its
// slab entity carries. That is right for a masonry wall and wrong for a lamp, whose pole is
// metal and whose luminaire must glow. Rather than change the engine, the sample re-materials its
// own leaves after the engine has posed them.
//
// Nothing here owns a GPU object except materials added to the caller's registry.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rime/assets/destructible_asset.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/destruction/ids.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_render/part_leaves.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/world.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

namespace rime::block_play::city {

// Content ids, caller-chosen constants exactly like blockkit's `CookSpec::asset` values ('LAMP',
// 'BARR'). They must not collide with the cooked ones; those are small ASCII tags too.
inline constexpr std::uint64_t kLampAsset = 0x4C41'4D50ull;
inline constexpr std::uint64_t kBarrierAsset = 0x4241'5252ull;

// `SlabRole::building` doubles as the marker that ties a replicated destructible (or its light)
// back to this brick. Real buildings are small integers and street furniture is `kNoBuilding`
// (0xFFFFFFFF), so a high-bits tag cannot collide with either, and a client that only ever sees
// the REPLICATED role can still find "lamp number i" without any extra component or channel.
inline constexpr std::uint32_t kLampTag = 0xC17A'0000u;
inline constexpr std::uint32_t kBarrierTag = 0xC17B'0000u;
inline constexpr std::uint32_t kLampDiffuserTag = 0xC17C'0000u;
inline constexpr std::uint32_t kTagMask = 0xFFFF'0000u;

// Part indices of the lamp pattern, in cook order.
inline constexpr std::uint32_t kLampPole = 0;
inline constexpr std::uint32_t kLampArm = 1;
inline constexpr std::uint32_t kLampHead = 2;

inline constexpr std::uint32_t kExtraBuildings = 24;

struct ExpandStats {
    std::uint32_t buildings_added = 0;
    std::uint32_t lamps = 0;
    std::uint32_t barriers = 0;
    std::uint32_t kerbs_added = 0;
    std::uint32_t lots_rejected = 0; // candidate lots refused by the overlap rules
    float ground_half_x = 0.0f;
    float ground_half_z = 0.0f;
};

// The CPU fracture patterns. Pure functions of nothing: the same bytes on every call and every
// peer, which is what lets two machines arrive at their own pattern ids independently.
[[nodiscard]] assets::DestructibleAsset make_lamp_asset();
[[nodiscard]] assets::DestructibleAsset make_barrier_asset();

// Rebuild the authored block as a city. Idempotent only in the sense that it is called once, on a
// freshly assembled world; a second call finds no 8 barriers to replace and adds nothing twice.
// Leaves the original 8 buildings and the hero pair EXACTLY as assembled.
ExpandStats expand(ecs::World& authoring);

// Is the footprint of a candidate building (centre, half-extent 4 m square) clear of the dressed
// environment (trees, verge, path, pond) and of the original block? Exposed so the smoke can
// assert the placed city against the same rule that placed it.
[[nodiscard]] bool lot_clear(float x, float z);

// Register the two patterns with one peer. `leaves`/`meshes` are null on the GPU-free server,
// the same seam `Peer::register_patterns` already has for the cooked patterns.
[[nodiscard]] bool register_patterns(destruction::DestructionWorld& destruction,
                                     physics::PhysicsWorld& physics,
                                     std::unordered_map<std::uint64_t, destruction::PatternId>& out,
                                     destruction_render::PartLeafRenderer* leaves,
                                     render::MeshRegistry* meshes);

struct SyncStats {
    std::uint32_t lamps_seen = 0;
    std::uint32_t lamps_lit = 0;
    std::uint32_t heads_broken = 0;
    std::uint32_t barriers_seen = 0;
    std::uint32_t leaves_dressed = 0;
    std::uint32_t lights_toggled = 0;
};

class Dresser {
public:
    // Adds dark housing and warm diffuser materials and one shared diffuser mesh.
    void init(const blockkit::BlockPalette& palette,
              render::MaterialRegistry& materials,
              render::MeshRegistry& meshes);

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // Once per tick, AFTER `PartLeafRenderer::update`. Cheap: it visits only tagged entities.
    SyncStats sync(ecs::World& world,
                   const destruction::DestructionWorld& destruction,
                   const destruction_render::PartLeafRenderer& leaves);

    [[nodiscard]] const SyncStats& last() const noexcept { return last_; }

private:
    blockkit::BlockPalette palette_{};
    render::MaterialId dead_head_ = render::kInvalidMaterialId;
    render::MaterialId diffuser_ = render::kInvalidMaterialId;
    render::MeshId diffuser_mesh_ = render::kInvalidMeshId;
    core::Vec3 diffuser_offset_{};
    bool ready_ = false;
    SyncStats last_{};
};

} // namespace rime::block_play::city
